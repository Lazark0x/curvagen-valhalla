#include "baldr/attributes_controller.h"
#include "midgard/logging.h"
#include "proto/common.pb.h"
#include "thor/route_matcher.h"
#include "thor/roundtrip_expansion.h"
#include "thor/triplegbuilder.h"
#include "thor/worker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;

namespace {
// Threshold for running a second pass pedestrian route with adjusted A*. The first
// pass for pedestrian routes is run with an aggressive A* threshold based on walking
// speed. If ferries are included in the path the A* heuristic rules can be violated
// which can lead to irregular paths. Running a second pass with less aggressive
// A* can take excessive time for longer paths - so exclude them to protect the service.
constexpr float kPedestrianMultipassThreshold = 50000.0f; // 50km

/**
 * Check if the paths meet at opposing edges (but not at a node). If so, add an intermediate location
 * so that the shape / distance along the path is adjusted at the location.
 */
bool intermediate_loc_edge_trimming(
    valhalla::Location& loc,
    const GraphId& in,
    const GraphId& out,
    std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>>& edge_trimming,
    const size_t path_index,
    const bool arrive_by) {
  // Find the path edges within the locations.
  auto in_pe = std::find_if(loc.correlation().edges().begin(), loc.correlation().edges().end(),
                            [&in](const valhalla::PathEdge& e) { return e.graph_id() == in; });
  auto out_pe = std::find_if(loc.correlation().edges().begin(), loc.correlation().edges().end(),
                             [&out](const valhalla::PathEdge& e) { return e.graph_id() == out; });

  // Could not find the edges. This seems like it should not happen. Log a warning
  // and do not insert an intermediate location.
  if (in_pe == loc.correlation().edges().end() || out_pe == loc.correlation().edges().end()) {
    LOG_WARN("Could not find connecting edges within the intermediate loc edge trimming");
    return false;
  }

  // Just set the edge index on the location for now then in triplegbuilder set to the shape index
  loc.mutable_correlation()->set_leg_shape_index(path_index + (arrive_by ? 1 : 0));
  // If the intermediate point is at a node we dont need to trim the edge we just set the edge index
  if (in_pe->begin_node() || in_pe->end_node() || out_pe->begin_node() || out_pe->end_node()) {
    // In this case we won't add a duplicate edge so we cant increment for arrive by
    loc.mutable_correlation()->set_leg_shape_index(path_index);
    return true;
  }

  // So what we are doing below is we are telling tripleg builder how to trim the two edges that come
  // together at an intermediate location. There are two cases, one where the route keeps going on the
  // same edge and one where the route does a uturn onto to opposing edge. In both cases we get two
  // edges in the final route just for simplicities sake. We could technically only do two when its
  // the uturn case. The way we do this is we specify a way to trim the shape of each edge. We use the
  // lat lon of the intermediate location to fix the geometry and we use the distance along to tell
  // trip leg builder how to cut the shape. We have to have at least one cut on each edge on either
  // side of the intermediate location. If we have multiple intermediate locations on a single edge we
  // will need to cut the edge based on the previous intermediate location we processed.

  PointLL snap_ll(in_pe->ll().lng(), in_pe->ll().lat());
  double dist_along = in_pe->percent_along();

  // Cut the first edges end off back to where the location lands along it
  auto inserted = edge_trimming.insert(
      {path_index + (arrive_by ? 1 : 0), {{false, PointLL(), 0.0}, {true, snap_ll, dist_along}}});
  // If it was already there we need to update it, should only happen for depart at (left to right)
  if (!inserted.second) {
    inserted.first->second.second = EdgeTrimmingInfo{true, snap_ll, dist_along};
  }

  // We need to use the distance along from the edge exiting the intermediate location to handle the
  // case when its a uturn at a via using the opposing edge that came into the via. Basically we need
  // to invert the distance along because we inverted the direction we are traveling along the edge.
  // This is handled automatically by using the correct exiting edge
  dist_along = out_pe->percent_along();

  // Cut the second edges beginning off up to where the location lands along it
  inserted = edge_trimming.insert(
      {path_index + (arrive_by ? 0 : 1), {{true, snap_ll, dist_along}, {false, PointLL(), 1.0}}});
  // If it was already there we need to update it, should only happen for arrive by (right to left)
  if (!inserted.second) {
    inserted.first->second.first = EdgeTrimmingInfo{true, snap_ll, dist_along};
  }

  return false;
}

inline bool is_through_point(const valhalla::Location& l) {
  return l.type() == valhalla::Location::kThrough || l.type() == valhalla::Location::kBreakThrough;
}

inline bool is_break_point(const valhalla::Location& l) {
  return l.type() == valhalla::Location::kBreak || l.type() == valhalla::Location::kBreakThrough;
}

inline bool is_highly_reachable(const valhalla::Location& loc, const valhalla::PathEdge& edge) {
  return static_cast<google::protobuf::uint32>(edge.inbound_reach()) >=
             loc.minimum_inbound_reachability() &&
         static_cast<google::protobuf::uint32>(edge.outbound_reach()) >=
             loc.minimum_outbound_reachability();
}

template <typename Predicate> inline void remove_path_edges(valhalla::Location& loc, Predicate pred) {
  auto new_end = std::remove_if(loc.mutable_correlation()->mutable_edges()->begin(),
                                loc.mutable_correlation()->mutable_edges()->end(), pred);
  int start_idx = std::distance(loc.mutable_correlation()->mutable_edges()->begin(), new_end);
  loc.mutable_correlation()->mutable_edges()->DeleteSubrange(start_idx,
                                                             loc.correlation().edges_size() -
                                                                 start_idx);

  new_end = std::remove_if(loc.mutable_correlation()->mutable_filtered_edges()->begin(),
                           loc.mutable_correlation()->mutable_filtered_edges()->end(), pred);
  start_idx = std::distance(loc.mutable_correlation()->mutable_filtered_edges()->begin(), new_end);
  loc.mutable_correlation()
      ->mutable_filtered_edges()
      ->DeleteSubrange(start_idx, loc.correlation().filtered_edges_size() - start_idx);
}

/**
// removes any edges from the location that aren't connected to it (because of radius)
void remove_edges(const GraphId& edge_id, valhalla::Location& loc, GraphReader& reader) {
  // find the path edge at this point
  auto pe =
      std::find_if(loc.correlation().edges().begin(), loc.correlation().edges().end(),
                   [&edge_id](const valhalla::PathEdge& e) { return e.graph_id() == edge_id;
});
  // if its in the middle of the edge it can only be this edge or the opposing depending on type
  if (!pe->begin_node() && !pe->end_node()) {
    GraphId opposing;
    if (loc.type() == valhalla::Location::kBreak || loc.type() == valhalla::Location::kVia)
      opposing = reader.GetOpposingEdgeId(edge_id);
    // remove anything that isnt one of these two edges
    for (int i = 0; i < loc.path_edges_size(); ++i) {
      if (loc.correlation().edges(i).graph_id() != edge_id && loc.correlation().edges(i).graph_id() !=
opposing) { loc.mutable_correlation()->mutable_edges()->SwapElements(i, loc.path_edges_size() - 1);
        loc.mutable_correlation()->mutable_edges()->RemoveLast();
      }
    }
    return;
  }

  // if its at the begin node lets center our sights on that
 graph_tile_ptr tile = reader.GetGraphTile(edge_id);
  const auto* edge = tile->directededge(edge_id);
  const auto* node = reader.GetEndNode(edge, tile);
  if (pe->begin_node()) {
    const auto* opp_edge = tile->directededge(node->edge_index() + edge->opp_index());
    node = reader.GetEndNode(opp_edge, tile);
  }

  // TODO: nuke any edges that aren't connected to this node
  GraphId start_edge = tile->header()->graphid();
  start_edge.set_id(node->edge_index);
  GraphId end_edge = start_edge + node->edge_count;
  for (int i = 0; i < loc.path_edges_size(); ++i) {
    if (loc.correlation().edges(i).graph_id() < start_edge || loc.correlation().edges(i) >= end_edge)
{ loc.mutable_correlation()->mutable_edges()->SwapElements(i, loc.path_edges_size() - 1);
      loc.mutable_correlation()->mutable_edges()->RemoveLast();
    }
  }
}*/

/**
 * Adds a shortcut to the cost factor edges given one
 * of its constituents
 */
void add_partial_shortcut(baldr::GraphReader& reader,
                          GraphId shortcut,
                          valhalla::Costing_Options* options,
                          valhalla::CostFactorEdge* cost_factor) {
  GraphId edge = static_cast<GraphId>(cost_factor->id());
  graph_tile_ptr tile = reader.GetGraphTile(shortcut);
  // it's part of a shortcut
  auto constituents = reader.RecoverShortcut(shortcut);
  auto* shortcut_edge = tile->directededge(shortcut);

  tile = reader.GetGraphTile(edge);
  auto* current_edge = tile->directededge(edge);

  // walk the base edges until we find ours
  uint64_t accumulated_length = 0;
  for (const auto& constituent : constituents) {
    if (edge == constituent)
      break;

    tile = reader.GetGraphTile(constituent, tile);
    if (!tile)
      break;

    auto* de = tile->directededge(constituent);
    accumulated_length += de->length();
  }
  auto* e = options->add_cost_factor_edges();
  e->set_id(shortcut);
  e->set_factor(cost_factor->factor());
  e->set_start(static_cast<double>(accumulated_length + (static_cast<double>(current_edge->length()) *
                                                         cost_factor->start())) /
               static_cast<double>(shortcut_edge->length()));
  e->set_end(static_cast<double>(accumulated_length +
                                 (static_cast<double>(current_edge->length()) * cost_factor->end())) /
             static_cast<double>(shortcut_edge->length()));
}

/**
 * Given one or more cost factor shapes, resolve them into single edges with an ID, a cost factor and
 * a range by edge walking the graph to match each shape.
 */
void add_cost_factor_edges(const sif::mode_costing_t& costing,
                           const sif::TravelMode& mode,
                           baldr::GraphReader& reader,
                           valhalla::Options& options,
                           double min_allowed_factor,
                           uint64_t max_allowed_edges) {
  Costing_Options* costing_options =
      options.mutable_costings()->find(options.costing_type())->second.mutable_options();

  // keep track of how many edges we're adding
  uint64_t edge_count = 0;

  for (auto& line : *options.mutable_cost_factor_lines()) {
    std::vector<std::vector<PathInfo>> legs;
    if (!RouteMatcher::FormPath(costing, mode, reader, line, false, /* use_shortcuts=*/true, legs)) {
      throw valhalla_exception_t{233};
    }
    for (const auto& leg : legs) {
      for (size_t i = 0; i < leg.size(); ++i) {
        if (edge_count > max_allowed_edges)
          throw valhalla_exception_t{234};
        auto& path_info = leg[i];
        bool is_first = i == 0;
        bool is_last = i == leg.size() - 1;
        if (is_first && is_last) { // trivial path
          edge_count++;
          auto* e = costing_options->add_cost_factor_edges();
          e->set_id(path_info.edgeid);
          e->set_factor(line.cost_factor());
          for (const auto& edge : line.locations(0).correlation().edges()) {
            if (path_info.edgeid == edge.graph_id()) {
              e->set_start(edge.percent_along());
              break;
            }
          }
          for (const auto& edge : line.locations(1).correlation().edges()) {
            if (path_info.edgeid == edge.graph_id()) {
              e->set_end(edge.percent_along());
              break;
            }
          }
          auto shortcut = reader.GetShortcut(path_info.edgeid);
          if (shortcut.is_valid()) {
            add_partial_shortcut(reader, shortcut, costing_options, e);
          }
        } else if (is_first || is_last) { // beginning or end edge
          for (const auto& edge :
               line.locations(static_cast<size_t>(is_last)).correlation().edges()) {
            if (path_info.edgeid == edge.graph_id()) {
              edge_count++;
              auto* e = costing_options->add_cost_factor_edges();
              e->set_id(path_info.edgeid);
              // apply the minimum allowed value specified in the config
              e->set_factor(std::max(line.cost_factor(), min_allowed_factor));
              e->set_start(is_first ? edge.percent_along() : 0.);
              e->set_end(is_last ? edge.percent_along() : 1.);
              auto shortcut = reader.GetShortcut(path_info.edgeid);
              if (shortcut.is_valid()) {
                add_partial_shortcut(reader, shortcut, costing_options, e);
              }
              break;
            }
          }
        } else { // intermediate edges
          edge_count++;
          auto* e = costing_options->add_cost_factor_edges();
          e->set_id(path_info.edgeid);
          e->set_factor(std::max(line.cost_factor(), min_allowed_factor));
          e->set_start(0.);
          e->set_end(1.);

          // if it's a shortcut, also add all of its constituent edges
          if (path_info.is_shortcut) {
            auto constituents = reader.RecoverShortcut(path_info.edgeid);
            for (const auto& constituent : constituents) {
              edge_count++;
              auto* e = costing_options->add_cost_factor_edges();
              e->set_id(constituent);
              e->set_factor(std::max(line.cost_factor(), min_allowed_factor));
              e->set_start(0);
              e->set_end(1);
            }
          } else {
            // if it's not a shortcut, it may be part of one
            // TODO: this is an expensive operation, since we need to expand the graph
            // a little, can't we persist this information somehow?
            auto shortcut = reader.GetShortcut(path_info.edgeid);
            if (shortcut.is_valid()) {
              add_partial_shortcut(reader, shortcut, costing_options, e);
            }
          }
        }
      }
    }
  }
}

} // namespace

namespace valhalla {
namespace thor {

void thor_worker_t::centroid(Api& request) {
  // time this whole method and save that statistic
  auto _ = measure_scope_time(request);

  auto& options = *request.mutable_options();
  adjust_locations(request);
  controller = AttributesController(request.options());
  auto costing = parse_costing(request);
  auto& locations = *options.mutable_locations();
  valhalla::Location destination;

  // get all the routes
  auto paths =
      centroid_gen.Expand(ExpansionType::forward, request, *reader, mode_costing, mode, destination);

  // serialize path information of each route into protobuf route objects
  auto origin = locations.begin();
  for (const auto& path : paths) {
    // the centroid could be either direction of the edge so here we set which it was by id
    auto dest = destination;
    dest.mutable_correlation()->mutable_edges(0)->set_graph_id(path.back().edgeid);

    // actually build the route object
    auto* route = request.mutable_trip()->mutable_routes()->Add();
    auto& leg = *route->mutable_legs()->Add();
    thor::TripLegBuilder::Build(options, controller, *reader, mode_costing, path.begin(), path.end(),
                                *origin, dest, leg, {"centroid"}, interrupt);

    // TODO: set the time at the destination if time dependent

    // next route
    ++origin;
  }
}

void thor_worker_t::route(Api& request) {
  // time this whole method and save that statistic
  auto _ = measure_scope_time(request);

  auto& options = *request.mutable_options();
  adjust_locations(request);
  controller = AttributesController(options);

  if (!request.options().cost_factor_lines().empty()) {
    // we parse costing twice in this case, once for edge walking,
    // and then again once with the edge factors added
    parse_costing(request);
    add_cost_factor_edges(mode_costing, mode, *reader, *request.mutable_options(),
                          min_linear_cost_factor, max_linear_cost_edges);
  }
  auto costing = parse_costing(request);

  // ADR-0033: native round-trip loop action. locations are [start, start].
  if (options.has_roundtrip()) {
    roundtrip_impl(request, costing);
    return;
  }

  // get all the legs
  if (options.date_time_type() == Options::arrive_by) {
    path_arrive_by(request, costing);
  } else {
    path_depart_at(request, costing);
  }
}

thor::PathAlgorithm* thor_worker_t::get_path_algorithm(const std::string& routetype,
                                                       const valhalla::Location& origin,
                                                       const valhalla::Location& destination,
                                                       Api& request) {
  // make sure they are all cancelable
  for (auto* alg : std::vector<PathAlgorithm*>{
           &multi_modal_transit,
           &timedep_forward,
           &timedep_reverse,
           &bidir_astar,
           &multimodal_astar,
       }) {
    alg->set_interrupt(interrupt);
  }

  // Have to use multimodal for transit based routing
  if (routetype == "multimodal" || routetype == "transit") {
    return &multi_modal_transit;
  }

  if (routetype == "auto_pedestrian") {
    return &multimodal_astar;
  }

  // Have to use bike share station algorithm
  if (routetype == "bikeshare") {
    return &multimodal_astar;
  }

  const auto& options = request.options();
  // If the origin has date_time set use timedep_forward method if the distance
  // between location is below some maximum distance (TBD).
  if (!origin.date_time().empty() && options.date_time_type() != Options::invariant &&
      !options.prioritize_bidirectional()) {
    PointLL ll1(origin.ll().lng(), origin.ll().lat());
    PointLL ll2(destination.ll().lng(), destination.ll().lat());
    if (ll1.Distance(ll2) < max_timedep_distance) {
      return &timedep_forward;
    } else {
      add_warning(request, 402);
    }
  }

  // If the destination has date_time set use timedep_reverse method if the distance
  // between location is below some maximum distance (TBD).
  if (!destination.date_time().empty() && options.date_time_type() != Options::invariant) {
    PointLL ll1(origin.ll().lng(), origin.ll().lat());
    PointLL ll2(destination.ll().lng(), destination.ll().lat());
    if (ll1.Distance(ll2) < max_timedep_distance) {
      return &timedep_reverse;
    } else {
      add_warning(request, 214);
    }
  }

  // Use A* if any origin and destination edges are the same or are connected - otherwise
  // use bidirectional A*. Bidirectional A* does not handle trivial cases with oneways and
  // has issues when cost of origin or destination edge is high (needs a high threshold to
  // find the proper connection).
  for (auto& edge1 : origin.correlation().edges()) {
    for (auto& edge2 : destination.correlation().edges()) {
      bool same_graph_id = edge1.graph_id() == edge2.graph_id();
      bool are_connected =
          reader->AreEdgesConnected(GraphId(edge1.graph_id()), GraphId(edge2.graph_id()));
      if (same_graph_id || are_connected) {
        return &timedep_forward;
      }
    }
  }

  // No other special cases we land on bidirectional a*
  return &bidir_astar;
}

std::vector<std::vector<thor::PathInfo>> thor_worker_t::get_path(PathAlgorithm* path_algorithm,
                                                                 valhalla::Location& origin,
                                                                 valhalla::Location& destination,
                                                                 const std::string& costing,
                                                                 Api& request) {
  const Options& options = request.options();
  // Find the path.
  valhalla::sif::cost_ptr_t cost = mode_costing[static_cast<uint32_t>(mode)];

  // If bidirectional A* disable use of destination-only edges on the
  // first pass. If there is a failure, we allow them on the second pass.
  // Other path algorithms can use destination-only edges on the first pass.
  // TODO(nils): why not others with destonly pruning? it gets a 2nd pass as well
  cost->set_allow_destination_only(path_algorithm == &bidir_astar ? false : true);

  cost->set_pass(0);
  auto paths = path_algorithm->GetBestPath(origin, destination, *reader, mode_costing, mode, options);

  // Check if we should run a second pass pedestrian route with different A*
  // (to look for better routes where a ferry is taken)
  // TODO(nils): how would a second pass find a better route, if it changes nothing ferry-related?
  bool ped_second_pass = false;
  if (!paths.empty() && (costing == "pedestrian" && path_algorithm->has_ferry())) {
    // DO NOT run a second pass on long routes due to performance issues
    float d = PointLL(origin.ll().lng(), origin.ll().lat())
                  .Distance(PointLL(destination.ll().lng(), destination.ll().lat()));
    if (d < kPedestrianMultipassThreshold) {
      ped_second_pass = true;
    }
  }

  // If path is not found try again with relaxed limits (if allowed). Use less aggressive
  // hierarchy transition limits, and retry with more candidate edges (add those filtered
  // by heading on first pass).
  if ((paths.empty() || ped_second_pass) && cost->AllowMultiPass()) {
    add_warning(request, 401);
    // add filtered edges to candidate edges for origin and destination
    origin.mutable_correlation()->mutable_edges()->MergeFrom(origin.correlation().filtered_edges());
    destination.mutable_correlation()->mutable_edges()->MergeFrom(
        destination.correlation().filtered_edges());

    path_algorithm->Clear();
    cost->set_pass(1);
    const bool using_bd = path_algorithm == &bidir_astar;
    cost->RelaxHierarchyLimits(using_bd);
    cost->set_allow_destination_only(true);
    cost->set_allow_conditional_destination(true);
    path_algorithm->set_not_thru_pruning(false);
    // Get the best path. Return if not empty (else return the original path)
    auto relaxed_paths =
        path_algorithm->GetBestPath(origin, destination, *reader, mode_costing, mode, options);
    if (!relaxed_paths.empty()) {
      return relaxed_paths;
    }
  }

  return paths;
}

void thor_worker_t::path_arrive_by(Api& api, const std::string& costing) {
  // Things we'll need
  TripRoute* route = nullptr;
  GraphId first_edge;
  std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>> edge_trimming;
  std::vector<thor::PathInfo> path;
  std::vector<std::string> algorithms;
  const Options& options = api.options();
  const Costing_Options& costing_options =
      options.costings().find(options.costing_type())->second.options();
  valhalla::Trip& trip = *api.mutable_trip();
  // ADR-0031: start each request with an empty used-edge set.
  mode_costing[static_cast<uint32_t>(mode)]->clear_used_edges();
  trip.mutable_routes()->Reserve(options.alternates() + 1);

  graph_tile_ptr tile = nullptr;

  // get the user provided hierarchy limits and store one for each path algorithm
  // because we may use them interchangeably
  std::vector<HierarchyLimits> hierarchy_limits_bidir =
      mode_costing[static_cast<uint32_t>(mode)]->GetHierarchyLimits();
  std::vector<HierarchyLimits> hierarchy_limits_unidir =
      mode_costing[static_cast<uint32_t>(mode)]->GetHierarchyLimits();

  // check whether hierarchy limits were already checked for this algorithm
  // on a multi-leg route
  bool used_unidir = false;
  bool used_bidir = false;
  bool add_hierarchy_limits_warning = false;

  auto route_two_locations = [&](auto& origin, auto& destination) -> bool {
    // Get the algorithm type for this location pair
    thor::PathAlgorithm* path_algorithm =
        this->get_path_algorithm(costing, *origin, *destination, api);
    path_algorithm->Clear();

    // once we know which algorithm will be used, set the hierarchy limits accordingly
    bool is_bidir = path_algorithm == &bidir_astar;
    auto& hierarchy_limits = is_bidir ? hierarchy_limits_bidir : hierarchy_limits_unidir;

    // only check hierarchy limits if not already done for the current algorithm
    add_hierarchy_limits_warning =
        (!(is_bidir ? used_bidir : used_unidir) &&
         check_hierarchy_limits(hierarchy_limits, mode_costing[static_cast<uint32_t>(mode)],
                                costing_options,
                                path_algorithm == &bidir_astar
                                    ? hierarchy_limits_config_bidirectional_astar
                                    : hierarchy_limits_config_astar,
                                allow_hierarchy_limits_modifications,
                                mode_costing[int(mode)]->UseHierarchyLimits())) ||
        add_hierarchy_limits_warning;

    // ..and mark hierarchy limits for this algorithm as checked
    is_bidir ? (used_bidir = true) : (used_unidir = true);
    mode_costing[static_cast<uint32_t>(mode)]->SetHierarchyLimits(hierarchy_limits);

    algorithms.push_back(path_algorithm->name());
    LOG_INFO(std::string("algorithm::") + path_algorithm->name());

    // If we are continuing through a location we need to make sure we
    // only allow the edge that was used previously (avoid u-turns)
    if (is_through_point(*destination) && first_edge.is_valid()) {
      remove_path_edges(*destination,
                        [&first_edge](const auto& edge) { return edge.graph_id() != first_edge; });
    }

    // Get best path and keep it
    auto temp_paths = this->get_path(path_algorithm, *origin, *destination, costing, api);
    if (temp_paths.empty())
      return false;
    // ADR-0031 edge-reuse leash: remember this leg's edges (both directions) so
    // subsequent legs of this request pay reuse_factor_ to re-ride them.
    {
      std::vector<uint64_t> leg_edge_values;
      leg_edge_values.reserve(temp_paths.front().size() * 2);
      for (const auto& info : temp_paths.front()) {
        leg_edge_values.push_back(info.edgeid.value);
        const baldr::GraphId opp = reader->GetOpposingEdgeId(info.edgeid);
        if (opp.is_valid())
          leg_edge_values.push_back(opp.value);
      }
      mode_costing[static_cast<uint32_t>(mode)]->mark_edges_used(leg_edge_values);
    }
    for (auto& temp_path : temp_paths) {
      auto out_tz = reader->GetTimezoneFromEdge(temp_path.back().edgeid, tile);
      auto in_tz = reader->GetTimezoneFromEdge(temp_path.front().edgeid, tile);

      // we add the timezone info if destination is the last location
      // and add waiting_secs again from the final destination's datetime, so we output the departing
      // time at intermediate locations, not the arrival time
      if ((destination->correlation().original_index() ==
               static_cast<google::protobuf::uint32>((options.locations().size() - 1)) &&
           (in_tz || out_tz))) {
        auto destination_dt = DateTime::offset_date(destination->date_time(), out_tz, out_tz,
                                                    destination->waiting_secs());
        destination->set_date_time(destination_dt.date_time);
        destination->set_time_zone_offset(destination_dt.time_zone_offset);
        destination->set_time_zone_name(destination_dt.time_zone_name);
      }

      // back propagate time information
      if (!destination->date_time().empty()) {
        auto origin_dt = DateTime::offset_date(destination->date_time(), out_tz, in_tz,
                                               -temp_path.back().elapsed_cost.secs);
        origin->set_date_time(origin_dt.date_time);
        origin->set_time_zone_offset(origin_dt.time_zone_offset);
        origin->set_time_zone_name(origin_dt.time_zone_name);
      }

      first_edge = temp_path.front().edgeid;
      temp_path.swap(path); // so we can append to path instead of prepend

      // Merge through legs by updating the time and splicing the lists
      if (!temp_path.empty()) {
        auto offset = path.back().elapsed_cost;
        auto distance_offset = path.back().path_distance;
        // NOTE: This will not work for all algorithms.  At this point, path_distance
        // is not correct across the board so do not rely on it downstream.
        std::for_each(temp_path.begin(), temp_path.end(), [offset, distance_offset](PathInfo& i) {
          i.elapsed_cost += offset;
          i.path_distance += distance_offset;
        });

        // When stitching routes at an intermediate location we need to store information about where
        // along the edge it happened so triplegbuilder can properly cut the shape where the location
        // was and store that info to be serialized int he output
        auto at_node =
            intermediate_loc_edge_trimming(*destination, path.back().edgeid, temp_path.front().edgeid,
                                           edge_trimming, temp_path.size(), true);

        // Connects via the same edge so we only need it once
        if (path.back().edgeid == temp_path.front().edgeid && at_node) {
          path.pop_back();
        }

        path.insert(path.end(), temp_path.begin(), temp_path.end());
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (is_break_point(*origin)) {
        // Move destination back to the last break
        std::vector<valhalla::Location> intermediates;
        while (!is_break_point(*destination)) {
          destination->mutable_correlation()->set_leg_shape_index(
              path.size() - destination->correlation().leg_shape_index());
          intermediates.push_back(*destination);
          --destination;
        }

        // We have to flip the intermediate loc indices because we built them in backwards order
        std::remove_reference<decltype(edge_trimming)>::type flipped;
        flipped.reserve(edge_trimming.size());
        for (const auto& kv : edge_trimming) {
          flipped.emplace(path.size() - kv.first, kv.second);
        }
        edge_trimming.swap(flipped);

        // Form output information based on path edges
        if (trip.routes_size() == 0 || options.alternates() > 0) {
          route = trip.mutable_routes()->Add();
          route->mutable_legs()->Reserve(options.locations_size());
        }
        auto& leg = *route->mutable_legs()->Add();
        TripLegBuilder::Build(options, controller, *reader, mode_costing, path.begin(), path.end(),
                              *origin, *destination, leg, algorithms, interrupt, edge_trimming,
                              intermediates);

        // advance the time for the next destination (i.e. algo origin) by the waiting_secs
        // of this origin (i.e. algo destination)
        // TODO(nils): why do we do this twice? above we also do it for a destination..
        if (origin->waiting_secs() && !origin->date_time().empty()) {
          auto origin_dt =
              DateTime::offset_date(origin->date_time(), in_tz, in_tz, -origin->waiting_secs());
          origin->set_date_time(origin_dt.date_time);
          origin->set_time_zone_offset(origin_dt.time_zone_offset);
          origin->set_time_zone_name(origin_dt.time_zone_name);
        }
        path.clear();
        edge_trimming.clear();
      }
    }

    // if we just made a leg that means we are done recording which algorithms were used
    if (path.empty())
      algorithms.clear();

    return true;
  };

  auto correlated = options.locations();
  bool allow_retry = true;

  // For each pair of locations
  auto origin = ++correlated.rbegin();
  while (origin != correlated.rend()) {
    auto destination = std::prev(origin);
    if (!route_two_locations(origin, destination)) {
      // if routing failed because an intermediate waypoint was snapped to the low reachability road
      // (such road lies in a small connectivity component that is not connected to other locations)
      // we should leave only high reachability candidates and try to route again
      if (allow_retry && destination != correlated.rbegin() && is_through_point(*destination) &&
          destination->correlation().edges_size() > 0 &&
          !is_highly_reachable(*destination, destination->correlation().edges(0))) {
        allow_retry = false;
        // for each intermediate waypoint remove candidates with low reachability
        correlated = options.locations();
        for (auto loc = std::next(correlated.begin()); loc != std::prev(correlated.end()); ++loc) {
          remove_path_edges(*loc,
                            [&loc](const auto& edge) { return !is_highly_reachable(*loc, edge); });
          // it doesn't make sense to continue if there are no more path edges
          if (loc->correlation().edges_size() == 0)
            // no route found
            throw valhalla_exception_t{442};
        }
        // resets the entire state of all the legs of the route and starts completely
        // over from the beginning doing all the legs over
        route = nullptr;
        first_edge = {};
        edge_trimming.clear();
        path.clear();
        algorithms.clear();
        trip.mutable_routes()->Clear();
        origin = ++correlated.rbegin();
        continue;
      }
      // no route found
      throw valhalla_exception_t{442};
    }
    ++origin;
  }

  // maybe warn if we needed to change user provided hierarchy limits
  if (add_hierarchy_limits_warning)
    add_warning(api, allow_hierarchy_limits_modifications ? 210 : 209);
  // Reverse the legs because protobuf only has adding to the end
  std::reverse(route->mutable_legs()->begin(), route->mutable_legs()->end());
  // assign changed locations
  *api.mutable_options()->mutable_locations() = std::move(correlated);
}

void thor_worker_t::path_depart_at(Api& api, const std::string& costing) {
  // Things we'll need
  TripRoute* route = nullptr;
  GraphId last_edge;
  std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>> edge_trimming;
  std::vector<thor::PathInfo> path;
  std::vector<std::string> algorithms;
  const Options& options = api.options();
  const Costing_Options& costing_options =
      options.costings().find(options.costing_type())->second.options();
  valhalla::Trip& trip = *api.mutable_trip();
  // ADR-0031: start each request with an empty used-edge set.
  mode_costing[static_cast<uint32_t>(mode)]->clear_used_edges();
  trip.mutable_routes()->Reserve(options.alternates() + 1);

  // get the user provided hierarchy limits and store one for each path algorithm
  // because we may use them interchangeably
  auto hierarchy_limits_bidir = mode_costing[static_cast<uint32_t>(mode)]->GetHierarchyLimits();
  // TODO: what about multimodal costing? we need to check the  hierarchy limits for all
  // costings that use hierarchy limits
  auto hierarchy_limits_unidir = mode_costing[static_cast<uint32_t>(mode)]->GetHierarchyLimits();

  // check whether hierarchy limits were already checked for this algorithm
  // on a multi-leg route
  bool used_unidir = false;
  bool used_bidir = false;
  bool add_hierarchy_limits_warning = false;

  graph_tile_ptr tile = nullptr;
  auto route_two_locations = [&, this](auto& origin, auto& destination) -> bool {
    // Get the algorithm type for this location pair
    thor::PathAlgorithm* path_algorithm =
        this->get_path_algorithm(costing, *origin, *destination, api);
    path_algorithm->Clear();
    algorithms.push_back(path_algorithm->name());
    LOG_INFO(std::string("algorithm::") + path_algorithm->name());

    // once we know which algorithm will be used, set the hierarchy limits accordingly
    bool is_bidir = path_algorithm == &bidir_astar;
    auto& hierarchy_limits = is_bidir ? hierarchy_limits_bidir : hierarchy_limits_unidir;

    // only check hierarchy limits if not already done for the current algorithm
    add_hierarchy_limits_warning =
        (!(is_bidir ? used_bidir : used_unidir) &&
         check_hierarchy_limits(hierarchy_limits, mode_costing[static_cast<uint32_t>(mode)],
                                costing_options,
                                path_algorithm == &bidir_astar
                                    ? hierarchy_limits_config_bidirectional_astar
                                    : hierarchy_limits_config_astar,
                                allow_hierarchy_limits_modifications,
                                mode_costing[static_cast<uint32_t>(mode)]->UseHierarchyLimits())) ||
        add_hierarchy_limits_warning;
    // ..and mark hierarchy limits for this algorithm as checked
    is_bidir ? (used_bidir = true) : (used_unidir = true);
    mode_costing[static_cast<uint32_t>(mode)]->SetHierarchyLimits(hierarchy_limits);

    // If we are continuing through a location we need to make sure we
    // only allow the edge that was used previously (avoid u-turns)
    if (is_through_point(*origin) && last_edge.is_valid()) {
      remove_path_edges(*origin,
                        [&last_edge](const auto& edge) { return edge.graph_id() != last_edge; });
    }
    // Get best path and keep it
    auto temp_paths = this->get_path(path_algorithm, *origin, *destination, costing, api);
    if (temp_paths.empty())
      return false;
    // ADR-0031 edge-reuse leash: remember this leg's edges (both directions) so
    // subsequent legs of this request pay reuse_factor_ to re-ride them.
    {
      std::vector<uint64_t> leg_edge_values;
      leg_edge_values.reserve(temp_paths.front().size() * 2);
      for (const auto& info : temp_paths.front()) {
        leg_edge_values.push_back(info.edgeid.value);
        const baldr::GraphId opp = reader->GetOpposingEdgeId(info.edgeid);
        if (opp.is_valid())
          leg_edge_values.push_back(opp.value);
      }
      mode_costing[static_cast<uint32_t>(mode)]->mark_edges_used(leg_edge_values);
    }

    for (auto& temp_path : temp_paths) {

      auto in_tz = reader->GetTimezoneFromEdge(temp_path.front().edgeid, tile);
      auto out_tz = reader->GetTimezoneFromEdge(temp_path.back().edgeid, tile);
      if ((origin->correlation().original_index() == 0) && (in_tz || out_tz)) {
        auto origin_dt = DateTime::offset_date(origin->date_time(), in_tz, in_tz, 0);

        origin->set_date_time(origin_dt.date_time);
        origin->set_time_zone_offset(origin_dt.time_zone_offset);
        origin->set_time_zone_name(origin_dt.time_zone_name);
      }
      // forward propagate time information
      if (!origin->date_time().empty() && (in_tz || out_tz)) {
        float offset = (options.date_time_type() != valhalla::Options::invariant)
                           ? (temp_path.back().elapsed_cost.secs + destination->waiting_secs())
                           : 0.0f;
        auto destination_dt = DateTime::offset_date(origin->date_time(), in_tz, out_tz, offset);
        destination->set_date_time(destination_dt.date_time);
        destination->set_time_zone_offset(destination_dt.time_zone_offset);
        destination->set_time_zone_name(destination_dt.time_zone_name);
      }

      last_edge = temp_path.back().edgeid;

      // Merge through legs by updating the time and splicing the lists
      if (!path.empty()) {
        auto offset = path.back().elapsed_cost;
        auto distance_offset = path.back().path_distance;
        std::for_each(temp_path.begin(), temp_path.end(), [offset, distance_offset](PathInfo& i) {
          i.elapsed_cost += offset;
          i.path_distance += distance_offset;
        });

        // When stitching routes at an intermediate location we need to store information about where
        // along the edge it happened so triplegbuilder can properly cut the shape where the location
        // was and store that info to be serialized int he output
        auto at_node =
            intermediate_loc_edge_trimming(*origin, path.back().edgeid, temp_path.front().edgeid,
                                           edge_trimming, path.size() - 1, false);

        // Connects via the same edge so we only need it once
        if (path.back().edgeid == temp_path.front().edgeid && at_node) {
          path.pop_back();
        }

        path.insert(path.end(), temp_path.begin(), temp_path.end());
      } // Didnt need to merge
      else {
        path.swap(temp_path);
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (is_break_point(*destination)) {
        // Move origin back to the last break
        while (!is_break_point(*origin)) {
          --origin;
        }

        // Form output information based on path edges.
        if (trip.routes_size() == 0 || options.alternates() > 0) {
          route = trip.mutable_routes()->Add();
          route->mutable_legs()->Reserve(options.locations_size());
        }
        auto& leg = *route->mutable_legs()->Add();
        thor::TripLegBuilder::Build(options, controller, *reader, mode_costing, path.begin(),
                                    path.end(), *origin, *destination, leg, algorithms, interrupt,
                                    edge_trimming, {std::next(origin), destination});

        path.clear();
        edge_trimming.clear();
      }
    }

    // if we just made a leg that means we are done recording which algorithms were used
    if (path.empty())
      algorithms.clear();

    return true;
  };

  auto correlated = options.locations();
  bool allow_retry = true;

  // For each pair of locations
  auto destination = ++correlated.begin();
  while (destination != correlated.end()) {
    auto origin = std::prev(destination);
    if (!route_two_locations(origin, destination)) {
      // if routing failed because an intermediate waypoint was snapped to the low reachability road
      // (such road lies in a small connectivity component that is not connected to other locations)
      // we should leave only high reachability candidates and try to route again
      if (allow_retry && origin != correlated.begin() && is_through_point(*origin) &&
          origin->correlation().edges_size() > 0 &&
          !is_highly_reachable(*origin, origin->correlation().edges(0))) {
        allow_retry = false;
        // for each intermediate waypoint remove candidates with low reachability
        correlated = options.locations();
        for (auto loc = std::next(correlated.begin()); loc != std::prev(correlated.end()); ++loc) {
          remove_path_edges(*loc,
                            [&loc](const auto& edge) { return !is_highly_reachable(*loc, edge); });
          // it doesn't make sense to continue if there are no more path edges
          if (loc->correlation().edges_size() == 0)
            // no route found
            throw valhalla_exception_t{442};
        }
        // resets the entire state of all the legs of the route and starts completely
        // over from the beginning doing all the legs over
        route = nullptr;
        last_edge = {};
        edge_trimming.clear();
        path.clear();
        algorithms.clear();
        trip.mutable_routes()->Clear();
        destination = ++correlated.begin();
        continue;
      }
      // no route found
      throw valhalla_exception_t{442};
    }
    ++destination;
  }
  // maybe warn if we needed to change user provided hierarchy limits
  if (add_hierarchy_limits_warning)
    add_warning(api, allow_hierarchy_limits_modifications ? 210 : 209);

  // assign changed locations
  *api.mutable_options()->mutable_locations() = std::move(correlated);
}

namespace {

// Reconstruct the forward PathInfo (start -> turnaround) from the expansion label tree.
// Walk the predecessor chain (mirror centroid path reconstruction), then reverse to
// start->turnaround order. Each leg's costs are cumulative from the start, which is the
// leg start for the forward leg.
std::vector<PathInfo> ForwardPath(const RoundTripExpansion& exp, uint32_t label_index) {
  std::vector<PathInfo> rev;
  const auto& labels = exp.labels();
  for (uint32_t l = label_index; l != baldr::kInvalidLabel; l = labels[l].predecessor()) {
    rev.emplace_back(labels[l].mode(), labels[l].cost(), labels[l].edgeid(), 0,
                     labels[l].path_distance(), labels[l].restriction_idx(),
                     labels[l].transition_cost());
  }
  std::reverse(rev.begin(), rev.end()); // start -> turnaround
  return rev;
}

// Build a routing Location snapped to a graph node, mirroring a loki node correlation:
// one PathEdge per edge leaving the node (begin-node, percent_along 0) plus its opposing
// inbound edge (end-node, percent_along 1) so the location works as both origin and
// destination of a leg (ADR-0033).
valhalla::Location correlate_node(const baldr::GraphId& node, baldr::GraphReader& reader) {
  valhalla::Location loc;
  graph_tile_ptr tile = reader.GetGraphTile(node);
  const baldr::NodeInfo* ni = tile->node(node);
  const PointLL node_ll = tile->get_node_ll(node);
  loc.mutable_ll()->set_lng(node_ll.lng());
  loc.mutable_ll()->set_lat(node_ll.lat());

  auto add_edge = [&](const baldr::GraphId& eid, bool begin_node) {
    auto* pe = loc.mutable_correlation()->mutable_edges()->Add();
    pe->set_graph_id(eid);
    pe->set_percent_along(begin_node ? 0.0 : 1.0);
    pe->set_begin_node(begin_node);
    pe->set_end_node(!begin_node);
    pe->set_distance(0);
    pe->set_inbound_reach(0);
    pe->set_outbound_reach(0);
    pe->set_side_of_street(valhalla::Location::kNone);
    pe->mutable_ll()->set_lng(node_ll.lng());
    pe->mutable_ll()->set_lat(node_ll.lat());
  };

  for (uint32_t i = 0; i < ni->edge_count(); ++i) {
    const baldr::GraphId eid(node.tileid(), node.level(), ni->edge_index() + i);
    const DirectedEdge* de = tile->directededge(eid);
    if (de->is_shortcut() || !(de->forwardaccess() & kAutoAccess))
      continue;
    add_edge(eid, true); // outbound edge leaving the node
    const baldr::GraphId opp = reader.GetOpposingEdgeId(eid);
    if (opp.is_valid())
      add_edge(opp, false); // opposing inbound edge arriving at the node
  }
  return loc;
}

} // namespace

// ADR-0033 native round-trip: one forward expansion grows the curvy frontier; turnarounds
// are harvested near target/2, bearing-bucketed for diversity, and each is closed by a
// leash-penalized return with a one-shot distance correction. The N loops serialize as
// Trip.routes (alternates), best-first by curviness-per-km.
void thor_worker_t::roundtrip_impl(Api& request, const std::string& /*costing*/) {
  auto& options = *request.mutable_options();
  const double target = options.roundtrip().target_distance();
  const uint32_t want = std::max<uint32_t>(1, options.roundtrip().num_candidates());

  // Start = locations(0); the duplicate locations(1) is ignored.
  valhalla::Location start = options.locations(0);
  auto* cost = mode_costing[static_cast<uint32_t>(mode)].get();
  cost->clear_used_edges();

  // 1) One forward expansion + harvest turnarounds (Task A4).
  RoundTripExpansion expander;
  auto cands = expander.Harvest(request, *reader, mode_costing, mode, target);
  if (cands.empty())
    throw valhalla_exception_t{442}; // no path / no candidates -> 422 to the client

  // 2) Dedup turnarounds by node (best curviness per node) so candidates are genuinely
  //    distinct loops, then bearing-bucket for diversity. The shuffle seed rotates the bucket
  //    origin, so the same (start,target,curviness) yields a different loop set per seed.
  const uint32_t seed = options.roundtrip().seed();
  std::unordered_map<uint64_t, uint32_t> best_by_node;
  for (uint32_t i = 0; i < cands.size(); ++i) {
    auto it = best_by_node.find(cands[i].node);
    if (it == best_by_node.end() ||
        cands[i].curviness_per_km > cands[it->second].curviness_per_km)
      best_by_node[cands[i].node] = i;
  }
  std::vector<uint32_t> uniq;
  uniq.reserve(best_by_node.size());
  for (const auto& kv : best_by_node)
    uniq.push_back(kv.second);
  std::sort(uniq.begin(), uniq.end()); // deterministic order (unordered_map is not)

  const uint32_t buckets = want;
  const float sector = 360.0f / static_cast<float>(buckets);
  const float offset = static_cast<float>(seed % 360u);
  std::vector<int> best(buckets, -1);
  for (uint32_t i : uniq) {
    float rb = cands[i].bearing_deg + offset;
    if (rb >= 360.0f)
      rb -= 360.0f;
    uint32_t b = std::min(buckets - 1, static_cast<uint32_t>(rb / sector));
    if (best[b] < 0 || cands[i].curviness_per_km > cands[best[b]].curviness_per_km)
      best[b] = static_cast<int>(i);
  }
  std::vector<uint32_t> chosen;
  for (int b : best)
    if (b >= 0)
      chosen.push_back(static_cast<uint32_t>(b));
  // Backfill from the remaining unique-node turnarounds, best curviness first.
  if (chosen.size() < want) {
    std::vector<uint32_t> rest;
    for (uint32_t i : uniq)
      if (std::find(chosen.begin(), chosen.end(), i) == chosen.end())
        rest.push_back(i);
    std::sort(rest.begin(), rest.end(), [&](uint32_t a, uint32_t c) {
      return cands[a].curviness_per_km > cands[c].curviness_per_km;
    });
    for (uint32_t i = 0; i < rest.size() && chosen.size() < want; ++i)
      chosen.push_back(rest[i]);
  }

  // Return-leg router: seed the reuse leash with the forward leg's edges (both directions),
  // route turnaround -> start on fresh roads, then clear the leash for the next candidate.
  bidir_astar.set_interrupt(interrupt);
  cost->set_allow_destination_only(true);
  cost->set_pass(0);
  auto route_return = [&](uint32_t fwd_label, valhalla::Location& turn) -> std::vector<PathInfo> {
    cost->clear_used_edges();
    std::vector<uint64_t> vals;
    for (uint32_t l = fwd_label; l != baldr::kInvalidLabel; l = expander.labels()[l].predecessor()) {
      const GraphId e = expander.labels()[l].edgeid();
      vals.push_back(e.value);
      const GraphId opp = reader->GetOpposingEdgeId(e);
      if (opp.is_valid())
        vals.push_back(opp.value);
    }
    cost->mark_edges_used(vals);
    bidir_astar.Clear();
    auto paths = bidir_astar.GetBestPath(turn, start, *reader, mode_costing, mode, options);
    cost->clear_used_edges();
    return paths.empty() ? std::vector<PathInfo>{} : paths.front();
  };

  auto node_for = [&](uint32_t cand_index) -> baldr::GraphId {
    return baldr::GraphId(cands[cand_index].node);
  };

  // 3) Build each chosen loop: forward leg from the tree + leashed return, with a one-shot
  //    distance correction (re-pick a turnaround at ~target-return from the SAME tree).
  struct Loop {
    std::vector<PathInfo> fwd, ret;
    valhalla::Location turn;
    float curviness;
  };
  std::vector<Loop> loops;
  for (uint32_t ci : chosen) {
    valhalla::Location turn = correlate_node(node_for(ci), *reader);
    std::vector<PathInfo> fwd = ForwardPath(expander, cands[ci].label_index);
    std::vector<PathInfo> ret = route_return(cands[ci].label_index, turn);
    if (ret.empty() || fwd.empty())
      continue;

    // One-shot distance correction: re-pick a turnaround whose forward distance ~=
    // target - return_length from the same tree (free), route its return once, keep closer.
    const double ret_m = ret.back().path_distance;
    const double want_fwd = target - ret_m;
    int alt = -1;
    uint32_t bestdiff = std::numeric_limits<uint32_t>::max();
    for (uint32_t k = 0; k < cands.size(); ++k) {
      const uint32_t d = static_cast<uint32_t>(
          std::abs(static_cast<int>(cands[k].path_distance) - static_cast<int>(want_fwd)));
      if (d < bestdiff) {
        bestdiff = d;
        alt = static_cast<int>(k);
      }
    }
    if (alt >= 0 && static_cast<uint32_t>(alt) != ci) {
      valhalla::Location turn2 = correlate_node(node_for(static_cast<uint32_t>(alt)), *reader);
      auto fwd2 = ForwardPath(expander, cands[alt].label_index);
      auto ret2 = route_return(cands[alt].label_index, turn2);
      if (!ret2.empty() && !fwd2.empty()) {
        const double tot1 = fwd.back().path_distance + ret_m;
        const double tot2 = fwd2.back().path_distance + ret2.back().path_distance;
        if (std::fabs(tot2 - target) < std::fabs(tot1 - target)) {
          fwd = std::move(fwd2);
          ret = std::move(ret2);
          turn = std::move(turn2);
        }
      }
    }

    loops.push_back(
        {std::move(fwd), std::move(ret), std::move(turn), cands[ci].curviness_per_km});
  }
  if (loops.empty())
    throw valhalla_exception_t{442};

  // 4) Engine ranks best-first by curviness-per-km (distance gated, reuse leashed).
  std::stable_sort(loops.begin(), loops.end(),
                   [](const Loop& a, const Loop& b) { return a.curviness > b.curviness; });

  // 5) Serialize each loop as a 2-leg TripRoute (start -> turnaround -> start). Pass fresh
  //    Location copies per leg since TripLegBuilder mutates origin/destination.
  valhalla::Trip& trip = *request.mutable_trip();
  trip.mutable_routes()->Reserve(static_cast<int>(loops.size()));
  std::vector<std::string> algorithms;
  for (auto& lp : loops) {
    auto* route = trip.mutable_routes()->Add();
    route->mutable_legs()->Reserve(2);
    {
      valhalla::Location o = start, d = lp.turn;
      auto& leg = *route->mutable_legs()->Add();
      TripLegBuilder::Build(options, controller, *reader, mode_costing, lp.fwd.begin(),
                            lp.fwd.end(), o, d, leg, algorithms, interrupt, {}, {});
    }
    {
      valhalla::Location o = lp.turn, d = start;
      auto& leg = *route->mutable_legs()->Add();
      TripLegBuilder::Build(options, controller, *reader, mode_costing, lp.ret.begin(),
                            lp.ret.end(), o, d, leg, algorithms, interrupt, {}, {});
    }
  }
}
} // namespace thor
} // namespace valhalla
