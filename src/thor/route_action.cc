#include "baldr/attributes_controller.h"
#include "midgard/logging.h"
#include "proto/common.pb.h"
#include "thor/road_twin_index.h"
#include "thor/roundtrip_expansion.h"
#include "thor/roundtrip_pairs.h"
#include "thor/route_matcher.h"
#include "thor/triplegbuilder.h"
#include "thor/worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

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
void add_shortcut(baldr::GraphReader& reader,
                  GraphId shortcut,
                  valhalla::Costing_Options* options,
                  valhalla::CostFactorEdge* cost_factor) {

  // for ignoring access restrictions, we don't care if it's
  // a partial, it applies to the whole edge
  if (cost_factor->ignore_access_restrictions()) {
    auto* exclude_edge = options->add_exclude_edges();
    exclude_edge->set_id(shortcut.value);
    return;
  }
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
          e->set_ignore_access_restrictions(line.ignore_access_restrictions());
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
            add_shortcut(reader, shortcut, costing_options, e);
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
              e->set_ignore_access_restrictions(line.ignore_access_restrictions());
              e->set_start(is_first ? edge.percent_along() : 0.);
              e->set_end(is_last ? edge.percent_along() : 1.);
              auto shortcut = reader.GetShortcut(path_info.edgeid);
              if (shortcut.is_valid()) {
                add_shortcut(reader, shortcut, costing_options, e);
              }
              break;
            }
          }
        } else { // intermediate edges
          edge_count++;
          auto* e = costing_options->add_cost_factor_edges();
          e->set_id(path_info.edgeid);
          e->set_factor(std::max(line.cost_factor(), min_allowed_factor));
          e->set_ignore_access_restrictions(line.ignore_access_restrictions());
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
              e->set_ignore_access_restrictions(line.ignore_access_restrictions());
              e->set_start(0);
              e->set_end(1);
            }
          } else {
            // if it's not a shortcut, it may be part of one
            // TODO: this is an expensive operation, since we need to expand the graph
            // a little, can't we persist this information somehow?
            auto shortcut = reader.GetShortcut(path_info.edgeid);
            if (shortcut.is_valid()) {
              add_shortcut(reader, shortcut, costing_options, e);
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

// ADR-0037 §3 Start Exemption: path-distance radius around the start inside which the
// return leg may reuse the forward leg's edges. The first stretch of a ride is often
// network-forced (dead-end village starts, single access roads) — barring it turns
// every such start into a Fallback Loop. Spec'd constant: the loopqual harness's
// metrics-v1.2 PARAM (curvagen #55) is pinned to this exact value — change them
// together, never one alone. Granularity is the edge label: an edge whose cumulative
// path distance exceeds the radius is excluded whole.
constexpr double kStartExemptionMeters = 1500.0;

// ADR-0037 progress-graded rejoin: the peak junction-edge penalty near the start, as a
// share of the leash surcharge (reuse_factor - 1). The grade fades linearly to zero at
// the turnaround: rejoining the corridor early is what builds lollipop stems; rejoining
// near the seam is how loops legitimately close.
constexpr float kRejoinGradeShare = 0.5f;

// ADR-0037 Distance Flex: the widened harvest band (fractions of target/2) feeding the
// refill queue's tail — a clean off-target loop beats a defective on-target one. The
// measured ScanBand (wayfinder #50/#54); mostly-shorter by design, so flex fills lean
// under target rather than over.
constexpr float kFlexLoFrac = 0.55f;
constexpr float kFlexHiFrac = 1.18f;

// ADR-0037 attempt budget (#54 candidate 5): total build attempts are capped so a
// pathological cell stops churning A* runs. The lazy widen grants a FRESH +want budget
// when it fires — the cap must never starve a cell before the widen has had its shot
// (the proto-v3f lesson: 32 underfilled cells, dirty served, spikes back).
constexpr uint32_t kAttemptSlack = 8;

// PROTOTYPE proto/v4-p1 (curvagen-valhalla#10): SECOND VIA IS DELETED.  The census
// (defect atlas v2 §7.2) found it structurally unreachable on rider demand — 0 rebuilds
// in 320 Belgrade requests, passes-home 0.1 % of 6 622 loops — while the audit's G4
// pinned it as a figure-8 through the rider's home whose leg C falls back to no
// exclusion at all (F06).  Its constants (kSecondViaStemFrac / kStemCorridorRadiusM /
// kStemGapM / kStemMinM / kSecondViaSectorDeg / kSecondViaRetries), the stem_fraction()
// detector that only it used, its ledger line and its three gurka tests go with it.
// ADR-0037 keeps the record.

// ADR-0037 §2 node-guarded one-shot distance correction (ADR-0033's deferred
// "maintainer's call", now called): a built loop landing outside tolerance gets ONE
// corrective re-aim — a fresh candidate near the compensated turnaround distance, in
// the original's bearing sector, whose node collides with no already-built turnaround
// (the node guard that preserves K-candidate distinctness; the unguarded version was
// rejected in ADR-0033 for converging distinct candidates onto one loop). One shot,
// no convergence loop — the phase-2 latency trap.
constexpr double kDistCorrTolerance = 0.20; // matches the gate-7 per-loop mean threshold
constexpr float kCorrSectorDeg = 90.0f;     // re-aim stays in the original bearing sector
// Latency is a per-REQUEST budget: uncapped, hard cells fire a corrective rebuild on
// nearly every loop (ledger: 11/12 on a 100 km cell) and the median request pays a
// whole extra build — measured over gate 9. The cap keeps the correction inside the
// v3h margin; deterministic (count-based, queue order), a build-time tunable.
constexpr uint32_t kMaxCorrectionsPerRequest = 8;
// A Fallback Loop's length is mostly network-forced (no fresh return), and its rebuild
// pays the exhausted-search double A*, so it only fires the correction on extreme
// misses — the tail that dominates the mean (measured: ~11-18%% of loops carry
// err > 0.40 while the median rebuild there is the only one that pays for itself).
constexpr double kDistCorrFallbackThr = 0.40;

// ADR-0037 Defect Gate: a built loop carrying an exact-mirror stub >= this at the seam
// is rejected and its slot refilled (the harness spike meter's own threshold).
constexpr double kSeamStubRejectM = 30.0;
// Seam-window decode radius: a hard-exclude success cannot retrace the forward leg at
// the seam (those edges were barred from its return search), so decoding ~this much of
// each leg around the seam is a FINAL verdict for it. Fallback Loops never use the
// window — see the gate below.
constexpr double kSeamWindowM = 1500.0;

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

// Visit every auto-accessible, non-shortcut outbound edge of the PHYSICAL junction: the
// node itself and its hierarchy-level twins (a node where road classes meet is split
// across levels, linked by transitions — an edge walker that reads one level sees a
// partial junction; the T2 lesson).
template <typename Fn>
void for_each_junction_edge(const baldr::GraphId& node, baldr::GraphReader& reader, Fn&& fn) {
  graph_tile_ptr tile = reader.GetGraphTile(node);
  if (!tile)
    return;
  const baldr::NodeInfo* ni = tile->node(node);
  std::vector<baldr::GraphId> level_nodes{node};
  for (uint32_t t = 0; t < ni->transition_count(); ++t)
    level_nodes.push_back(tile->transition(ni->transition_index() + t)->endnode());
  for (const baldr::GraphId& n : level_nodes) {
    graph_tile_ptr ntile = n == node ? tile : reader.GetGraphTile(n);
    if (!ntile)
      continue;
    const baldr::NodeInfo* nni = ntile->node(n);
    for (uint32_t i = 0; i < nni->edge_count(); ++i) {
      const baldr::GraphId eid(n.tileid(), n.level(), nni->edge_index() + i);
      const DirectedEdge* de = ntile->directededge(eid);
      if (de->is_shortcut() || !(de->forwardaccess() & kAutoAccess))
        continue;
      fn(eid);
    }
  }
}

// Build a routing Location snapped to a graph node, mirroring a loki node correlation:
// one PathEdge per edge leaving the node (begin-node, percent_along 0) plus its opposing
// inbound edge (end-node, percent_along 1) so the location works as both origin and
// destination of a leg (ADR-0033).
// ADR-0037 turnaround hardening: the opposing edge of the forward leg's arrival edge is
// dropped from the outbound set, so the return leg cannot open with a U-turn back down
// the approach road (the seam_uturn spike door — A* origin edges never pass through the
// costing's Allowed(), so only the correlation itself can close it). The arrival edge is
// added explicitly as an end-node PathEdge: a one-way arrival's opposing edge fails the
// access filter, leaving the forward leg's destination edge missing and TripLegBuilder
// throwing 499 for the WHOLE request — the #53 bank-fill death class.
// proto/v4-p2: `departure_edge` is the edge the ride LEAVES this node on afterwards (a
// bridge's destination); arriving on its opposite would be a U-turn into it, so that
// inbound edge is dropped from the correlation.
valhalla::Location correlate_node(const baldr::GraphId& node,
                                  baldr::GraphReader& reader,
                                  const baldr::GraphId& arrival_edge,
                                  const baldr::GraphId& departure_edge = {}) {
  valhalla::Location loc;
  graph_tile_ptr tile = reader.GetGraphTile(node);
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

  const baldr::GraphId uturn_door =
      arrival_edge.is_valid() ? reader.GetOpposingEdgeId(arrival_edge) : baldr::GraphId{};
  // The return leg needs the outbound set of the PHYSICAL junction (all hierarchy
  // levels) — else a junction turnaround reads as exitless and the outbound guard
  // below discards it (a harvest-yield leak; a 442 in small cells).
  for_each_junction_edge(node, reader, [&](const baldr::GraphId& eid) {
    if (uturn_door.is_valid() && eid == uturn_door)
      return; // no U-turn opening for the return leg
    add_edge(eid, true); // outbound edge leaving the node
    if (departure_edge.is_valid() && eid == departure_edge)
      return; // its opposite is the U-turn door into the departure
    graph_tile_ptr otile;
    const baldr::GraphId opp = reader.GetOpposingEdgeId(eid, otile);
    // proto/v4-p2: the opposite of a one-way outbound edge cannot be ridden INTO the
    // node; seeding it as a destination let a bridge arrive on it (a search ends on a
    // destination edge without an access test).
    if (opp.is_valid() && otile && (otile->directededge(opp)->forwardaccess() & kAutoAccess))
      add_edge(opp, false); // opposing inbound edge arriving at the node
  });
  // The forward leg arrives on this edge; TripLegBuilder needs it present to trim the
  // forward destination. It can never appear in the loop above — its opposing edge is
  // the skipped U-turn door — so adding it here cannot duplicate.
  if (arrival_edge.is_valid())
    add_edge(arrival_edge, false);
  return loc;
}

// ADR-0037 trap-aware tip walk-back. When the harvest lands inside a dead-end stub
// (the out-leg of an excursion — the label U-turn test cannot see it because the
// retrace only completes on the return leg), the turnaround sits above pavement that
// forces the fallback return to bounce out-and-back: the wrapped-seam spike class.
// Walk the built leg's tip back until the tip node offers a fresh exit that survives a
// bounded probe: a "fresh exit" opening into a pure single-thread dead-end corridor is
// a trap, not a way out. Branchy or over-budget threads count as viable — the Defect
// Gate owns those. Both the exit scan and the probe read the whole physical junction
// (hierarchy twins). Pops stop at the Start Exemption: the first stretch is mandatory
// riding. Prefix costs stay valid — tail pops need no rebuild.
constexpr uint32_t kTrapProbeDepth = 60;
constexpr uint32_t kWalkBackPopCap = 400;
std::vector<PathInfo> walk_back_trapped_tip(std::vector<PathInfo> fwd,
                                            baldr::GraphReader& reader) {
  auto dead_end_thread = [&reader](const baldr::GraphId& first_edge) {
    baldr::GraphId cur = first_edge;
    for (uint32_t step = 0; step < kTrapProbeDepth; ++step) {
      graph_tile_ptr tile = reader.GetGraphTile(cur);
      if (!tile)
        return false;
      const baldr::GraphId endnode = tile->directededge(cur)->endnode();
      const baldr::GraphId back = reader.GetOpposingEdgeId(cur);
      baldr::GraphId next;
      uint32_t onward = 0;
      for_each_junction_edge(endnode, reader, [&](const baldr::GraphId& eid) {
        if (back.is_valid() && eid == back)
          return;
        ++onward;
        next = eid;
      });
      if (onward == 0)
        return true; // thread terminated: a pure dead-end corridor
      if (onward > 1)
        return false; // branches: viable
      cur = next;
    }
    return false; // long thread: assume viable
  };

  uint32_t popped = 0;
  while (fwd.size() > 1 && popped < kWalkBackPopCap) {
    const PathInfo& tip = fwd.back();
    // The distance below the tip edge; popping past the exemption would eat the
    // mandatory first stretch.
    if (static_cast<double>(fwd[fwd.size() - 2].path_distance) <= kStartExemptionMeters)
      break;
    graph_tile_ptr tile = reader.GetGraphTile(tip.edgeid);
    if (!tile)
      break;
    const baldr::GraphId tip_node = tile->directededge(tip.edgeid)->endnode();
    const baldr::GraphId back_edge = reader.GetOpposingEdgeId(tip.edgeid);
    uint32_t fresh = 0;
    for_each_junction_edge(tip_node, reader, [&](const baldr::GraphId& eid) {
      if (back_edge.is_valid() && eid == back_edge)
        return; // riding back is not a fresh exit
      if (fresh == 0 && !dead_end_thread(eid))
        ++fresh;
    });
    if (fresh > 0)
      break;
    fwd.pop_back();
    ++popped;
  }
  return fwd;
}

// Decode a leg's edges onto the 1e-5 grid (consecutive duplicates dropped) — the same
// snap the Defect Gate and the loopqual harness use. Returns false on an unresolvable
// tile (caller treats the check as inconclusive-clean, like the gate does).
bool decode_leg_grid(const std::vector<PathInfo>& leg,
                     baldr::GraphReader& reader,
                     std::vector<std::pair<int64_t, int64_t>>& pts) {
  for (const auto& pi : leg) {
    graph_tile_ptr tile = reader.GetGraphTile(pi.edgeid);
    if (!tile)
      return false;
    const DirectedEdge* de = tile->directededge(pi.edgeid);
    auto shape = tile->edgeinfo(de).shape();
    if (!de->forward())
      std::reverse(shape.begin(), shape.end());
    for (const auto& p : shape) {
      std::pair<int64_t, int64_t> k{std::llround(p.lat() * 1e5), std::llround(p.lng() * 1e5)};
      if (pts.empty() || pts.back() != k)
        pts.push_back(k);
    }
  }
  return true;
}

// proto/v4-p1.1: append a leg's shape onto the 1e-5 grid with a running along-path
// distance (consecutive duplicates dropped).  Shared by the seam detector and the
// mid-leg mirror detector below.  false = unresolvable tile (caller treats the check as
// inconclusive-clean, exactly as the seam gate always has).
bool append_leg_pts(const std::vector<PathInfo>& leg,
                    size_t from,
                    size_t to,
                    baldr::GraphReader& reader,
                    std::vector<std::pair<int64_t, int64_t>>& pts,
                    std::vector<double>& cum) {
  for (size_t li = from; li < to; ++li) {
    graph_tile_ptr tile = reader.GetGraphTile(leg[li].edgeid);
    if (!tile)
      return false;
    const DirectedEdge* de = tile->directededge(leg[li].edgeid);
    auto shape = tile->edgeinfo(de).shape();
    if (!de->forward())
      std::reverse(shape.begin(), shape.end());
    for (const auto& p : shape) {
      std::pair<int64_t, int64_t> k{std::llround(p.lat() * 1e5), std::llround(p.lng() * 1e5)};
      if (pts.empty() || pts.back() != k) {
        if (!pts.empty())
          cum.push_back(cum.back() +
                        midgard::PointLL(pts.back().second / 1e5, pts.back().first / 1e5)
                            .Distance(midgard::PointLL(k.second / 1e5, k.first / 1e5)));
        pts.push_back(k);
      }
    }
  }
  return true;
}

// proto/v4-p1.1 GEOMETRY DEFECT GATE, half two (F08 / gurka G6b).  The longest exact-
// mirror stub ANYWHERE in one leg.  seam_stub_m only counts palindromes whose interval
// covers the seam, so a mid-return bounce — the return overshoots into a spur that ends
// on its own path and U-turns back — is invisible to it, and the audit's G6b serves a
// 999.99 m mid-return mirror with the gate reading clean (F08, F13 leak 3).  The price
// is decoding the whole return leg on every candidate, not a 1500 m window; budgeted in
// the stage ledger as `gate=`.
double leg_mirror_stub_m(const std::vector<PathInfo>& leg, baldr::GraphReader& reader) {
  std::vector<std::pair<int64_t, int64_t>> pts;
  std::vector<double> cum{0.0};
  if (!append_leg_pts(leg, 0, leg.size(), reader, pts, cum) || pts.size() < 3)
    return 0.0;
  double worst = 0.0;
  for (size_t i = 1; i + 1 < pts.size(); ++i) {
    if (pts[i - 1] == pts[i + 1]) {
      size_t w = 1;
      while (i >= 1 + w && i + 1 + w < pts.size() && pts[i - 1 - w] == pts[i + 1 + w])
        ++w;
      const double stub = cum[i] - cum[i - w];
      if (stub > worst)
        worst = stub;
      i += w;
    }
  }
  return worst;
}

// ADR-0037 Defect Gate detector: decode both legs onto the 1e-5 grid and measure the
// longest exact-mirror stub whose interval covers the seam — the cross-leg retrace that
// survives every forward-side guard (a fallback return riding back down a dead-end
// stub). window_m > 0 decodes only the last/first ~window_m of each leg around the
// seam; window_m == 0 decodes both legs whole.
double seam_stub_m(const std::vector<PathInfo>& fwd,
                   const std::vector<PathInfo>& ret,
                   baldr::GraphReader& reader,
                   double window_m) {
  auto ddist = [](const std::vector<PathInfo>& leg, size_t i) {
    return static_cast<double>(leg[i].path_distance - (i ? leg[i - 1].path_distance : 0.f));
  };
  size_t fwd_begin = 0;
  size_t ret_end = ret.size();
  if (window_m > 0.0) {
    fwd_begin = fwd.size();
    double acc = 0.0;
    while (fwd_begin > 0 && acc < window_m) {
      --fwd_begin;
      acc += ddist(fwd, fwd_begin);
    }
    ret_end = 0;
    acc = 0.0;
    while (ret_end < ret.size() && acc < window_m) {
      acc += ddist(ret, ret_end);
      ++ret_end;
    }
  }
  std::vector<std::pair<int64_t, int64_t>> pts;
  std::vector<double> cum{0.0};
  size_t seam_idx = 0;
  if (!append_leg_pts(fwd, fwd_begin, fwd.size(), reader, pts, cum))
    return 0.0;
  seam_idx = pts.empty() ? 0 : pts.size() - 1;
  if (!append_leg_pts(ret, 0, ret_end, reader, pts, cum))
    return 0.0;
  if (pts.size() < 3)
    return 0.0;
  double worst = 0.0;
  for (size_t i = 1; i + 1 < pts.size(); ++i) {
    if (pts[i - 1] == pts[i + 1]) {
      size_t w = 1;
      while (i >= 1 + w && i + 1 + w < pts.size() && pts[i - 1 - w] == pts[i + 1 + w])
        ++w;
      // only stubs whose interval covers the seam (the defect site); slack 2 like the meter
      if (i - w <= seam_idx + 2 && i + w + 2 >= seam_idx) {
        const double stub = cum[i] - cum[i - w];
        if (stub > worst)
          worst = stub;
      }
      i += w; // skip past this palindrome
    }
  }
  return worst;
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

  // ADR-0037 §4 stage-timing ledger (config "thor.roundtrip_stage_timing", default
  // off): the only per-stage cost visibility on the box — it found the decisive
  // ScanBand regression (#54). Timestamps are taken unconditionally (nanoseconds
  // against A* runs); only the log line is gated.
  using ledger_clock = std::chrono::steady_clock;
  auto ms_since = [](ledger_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(ledger_clock::now() - t0).count();
  };
  double harvest_ms = 0, scan_ms = 0, walkback_ms = 0, rejoin_ms = 0, astar_ms = 0,
         astar_fb_ms = 0, seam_ms = 0, build_ms = 0;
  // proto/v4-p1.1: the geometry Defect Gate's own budget (score + return-leg decode).
  double gate_ms = 0;
  uint32_t gate_seam = 0, gate_twinride = 0, gate_bouncehits = 0;
  // proto/v4-p1.1: how many GEOMETRY-gate rejects may buy a replacement build in this
  // request, how many were kept instead, and which shape the budget held back.
  uint32_t gate_refills = 0, gate_kept = 0;
  uint32_t gate_denied_twinride = 0, gate_denied_bounce = 0;
  // ADR-0037's seam rejects refill unconditionally and are counted apart, so
  // "refilled n/budget" stays a statement about the budget (§16).
  uint32_t gate_seam_refills = 0;
  // thor.roundtrip_gate_refill_budget: 0 (default) = unlimited, the ADR-0037 seam
  // gate's own semantics.  A positive value caps how many GEOMETRY-gate rejects may
  // buy a replacement build; the rest are kept in the bank's last tier.  The corpus
  // runs both ways — see the latency section of the report.
  const uint32_t gate_refill_budget =
      roundtrip_gate_refill_budget ? roundtrip_gate_refill_budget : 0xffffffffu;

  // 1) One forward expansion + harvest turnarounds (Task A4). An empty primary band is
  //    no longer fatal here — the Distance Flex scan below may still fill the queue.
  // proto/v4-p1: the road-identity sidecar (built at engine start; this is a cached
  // lookup).  `twin_index == nullptr` restores exact v3 behaviour.
  const RoadTwinIndex* twin_index =
      roundtrip_road_identity ? &RoadTwinIndex::get(*reader, roundtrip_twin_radius_m,
                                                    roundtrip_parallel_radius_m,
                                                    roundtrip_parallel_tier,
                                                    roundtrip_switchback_test)
                              : nullptr;
  const bool parallel_tier = roundtrip_parallel_tier;

  RoundTripExpansion expander;
  expander.set_chain_simplicity(roundtrip_simple_chains, twin_index);
  const auto t_harvest = ledger_clock::now();
  auto cands = expander.Harvest(request, *reader, mode_costing, mode, target);
  harvest_ms = ms_since(t_harvest);

  // ---------------------------------------------------------------------------------
  // proto/v4-p2 (curvagen-valhalla#11): THE PAIR PASS.  Suurballe-Tarjan's second phase
  // over the harvest forest gives, for every physical junction the expansion reached,
  // the min-cost pair of directed-edge-disjoint start->junction paths under the harvest
  // costing (thor/roundtrip_pairs.h states the graph, the reduced costs and the
  // approximations).  The loop for a junction IS the pair: one path ridden out, the
  // other ridden home backwards.  Everything the return-leg A* used to discover after a
  // build — is there a fresh way home, how long is it, does it ride the corridor's
  // twins — is known here, before any A* runs, so selection rejects at harvest time.
  // ---------------------------------------------------------------------------------
  std::unique_ptr<RoundTripPairPass> pairs;
  double pair_pass_ms = 0, bridge_ms = 0, pair_eval_ms = 0;
  // prototype debugging: ROUNDTRIP_DEBUG=1 mirrors the pair ledger to stderr (gurka
  // silences the logger inside do_action)
  const bool rt_debug = std::getenv("ROUNDTRIP_DEBUG") != nullptr;
  uint32_t rev_fail_tile = 0, rev_fail_noopp = 0, rev_fail_access = 0, rev_fail_turn = 0,
           rev_fail_gap = 0, pair_fwd_illegal = 0, pair_swapped = 0, rev_bad_ret_edge = 0;
  bool pair_cheap_reject = false; // the last pair build died before any search
  uint32_t pair_cands_dropped = 0;  // no junction, or the junction's tree chain is non-simple
  uint32_t pair_cands_merged = 0;   // a second in-band arrival at a junction already listed
  uint32_t pair_cands_rehung = 0;   // the junction's cheapest arrival is not the harvest label
  std::unordered_set<uint32_t> pair_junctions_seen;
  // A harvest candidate names a JUNCTION; the pair's forward leg is that junction's
  // cheapest arrival (the tree path to its sink vertex), which may be a different label
  // — even an out-of-band one — from the chain the harvest found there.  The candidate
  // is re-hung on the tree parent (its distance band is judged on the pair total,
  // not on this chain) and junctions are listed once.
  auto pair_adopt = [&](const Turnaround& t, Turnaround& out) -> bool {
    const uint32_t j = pairs->junction_of(t.label_index);
    if (j == RoundTripPairPass::kNone) {
      ++pair_cands_dropped;
      return false;
    }
    const uint32_t tp = pairs->tree_parent(j);
    if (expander.chain_nonsimple(tp)) {
      ++pair_cands_dropped; // F01: the cheapest arrival reverses on a ring
      return false;
    }
    if (!pair_junctions_seen.insert(j).second) {
      ++pair_cands_merged;
      return false;
    }
    out = t;
    if (tp != t.label_index) {
      ++pair_cands_rehung;
      const auto& labels = expander.labels();
      out.label_index = tp;
      out.path_distance = labels[tp].path_distance();
      out.node = labels[tp].endnode().value;
    }
    return true;
  };
  if (roundtrip_pair_pass) {
    const auto t_pairs = ledger_clock::now();
    pairs = std::make_unique<RoundTripPairPass>(expander, *reader, twin_index,
                                                cost->access_mode(), kStartExemptionMeters,
                                                roundtrip_pair_twin_join_m);
    pairs->set_return_legal(roundtrip_pair_return_legal);
    pairs->set_prefer_two_way_tree(roundtrip_pair_two_way_tree);
    pairs->Run();
    pair_pass_ms = ms_since(t_pairs);
    // The pair's forward leg is the junction's CHEAPEST arrival (the tree path to its
    // sink vertex), so a harvest candidate is kept only when its label is that arrival:
    // the harvest's per-node "curviest chain" pick gives way to the tree's own.
    std::vector<Turnaround> kept;
    kept.reserve(cands.size());
    for (const auto& t : cands) {
      Turnaround adopted;
      if (pair_adopt(t, adopted))
        kept.push_back(adopted);
    }
    cands.swap(kept);
    const auto& ps = pairs->stats();
    LOG_INFO("roundtrip pair-pass: labels=" + std::to_string(ps.labels) +
             " canonical=" + std::to_string(ps.canonical_labels) +
             " junctions=" + std::to_string(ps.junctions) +
             " unified=" + std::to_string(ps.junctions_unified) +
             " vertices=" + std::to_string(ps.vertices) + " arcs=" + std::to_string(ps.arcs) +
             " (tree=" + std::to_string(ps.arcs_tree) + " fwd=" + std::to_string(ps.arcs_forward) +
             " ret_only=" + std::to_string(ps.arcs_return_only) +
             " two_way=" + std::to_string(ps.arcs_two_way) +
             " exempt=" + std::to_string(ps.arcs_exempt) +
             " clamped=" + std::to_string(ps.arcs_clamped) +
             " fwd_only_dropped=" + std::to_string(ps.arcs_forward_only_dropped) +
             ") sinks_with_pair=" + std::to_string(ps.sinks_with_pair) +
             " labeled=" + std::to_string(ps.labeled) + " steps=" + std::to_string(ps.steps) +
             " source_children=" + std::to_string(ps.source_children) +
             " root_return_arcs=" + std::to_string(ps.root_return_arcs) +
             " tree(two_way/one_way/not_cheapest)=" + std::to_string(ps.tree_two_way) + "/" +
             std::to_string(ps.tree_one_way) + "/" + std::to_string(ps.tree_not_cheapest) +
             " build_ms=" + std::to_string(static_cast<int>(ps.build_ms)) +
             " run_ms=" + std::to_string(static_cast<int>(ps.run_ms)) +
             " bytes=" + std::to_string(ps.bytes) + " band_cands=" + std::to_string(cands.size()) +
             " dropped=" + std::to_string(pair_cands_dropped) +
             " merged=" + std::to_string(pair_cands_merged) +
             " rehung=" + std::to_string(pair_cands_rehung));
  }

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
  std::vector<std::vector<uint32_t>> bucketed(buckets);
  for (uint32_t i : uniq) {
    uint32_t b = std::min(buckets - 1, static_cast<uint32_t>(cands[i].bearing_deg / sector));
    bucketed[b].push_back(i);
  }
  // Min-separation guard (ADR-0036 T9): node-dedup cannot see adjacent nodes
  // on the same road, and the curviness-only backfill floods the single
  // curviest massif when bearing sectors are empty (border-clipped starts:
  // 5/12 byte-identical loops measured). Reject any turnaround within
  // eps straight-line of one already chosen; eps = 0.1 x target/2.
  const double min_separation_m = 0.1 * (target / 2.0);
  std::vector<uint32_t> chosen;
  auto separated = [&](uint32_t i) {
    for (uint32_t c : chosen)
      if (static_cast<double>(cands[i].ll.Distance(cands[c].ll)) < min_separation_m)
        return false;
    return true;
  };

  // proto/v4-p2: lazy, cached evaluation of a candidate's PAIR — constructed from the
  // pass, judged on band, twin ride and near-dup BEFORE it can be chosen.  A rejected
  // sink costs a path walk, never a search.
  struct PairKeys {
    std::unordered_map<uint64_t, double> ridden; // canonical id -> metres, beyond the exemption
    std::unordered_set<uint64_t> twins;          // sidecar twins of the ridden edges
    double total = 0;                            // loop metres
    // proto/v4-p2.1: the FORWARD leg alone — the tree path, which IS the served forward
    // leg (the 2.5 % orientation swaps ride the other path out; ignored) — canonical id
    // -> metres beyond the exemption, and the leg's whole length.  What the per-leg
    // sharing threshold reads.  Empty on a built-loop bank entry (never read there).
    std::unordered_map<uint64_t, double> fwd_ridden;
    double fwd_total = 0;
  };
  struct PairEval {
    RoundTripPairPass::Pair pair;
    bool checked = false, ok = false;
    const char* reject = "";
    double total_len = 0, dist_err = 0, self_overlap_m = 0, score = 0;
    float curviness = 0; // both legs
    PairKeys keys;
  };
  std::unordered_map<uint32_t, PairEval> pair_cache;
  uint32_t pair_sinks_considered = 0, pair_none = 0, pair_band_rejects = 0,
           pair_twin_rejects = 0, pair_share_rejects = 0;
  std::vector<PairKeys> pair_selected_keys; // chosen at selection, for the K x K filter
  std::vector<PairKeys> pair_built_keys;    // committed loops, for the K x K filter
  // Twin-rejected pairs are remembered: when the queue runs dry with the bank short,
  // they are built anyway and land in the geometry gate's dirty tier — a ranked-last
  // dirty loop, never a silent clean tier and never a 442 where a loop exists.
  std::vector<uint32_t> pair_twin_candidates;
  bool pair_allow_twin = false;
  auto shared_fraction = [&](const PairKeys& a, const PairKeys& b) -> double {
    if (a.total <= 0.0)
      return 0.0;
    double shared = 0.0;
    for (const auto& [k, len] : a.ridden)
      if (b.ridden.count(k) || b.twins.count(k))
        shared += len;
    return shared / a.total;
  };
  // proto/v4-p2.1 (curvagen-valhalla#14): distinctness at selection.
  const bool leg_mode = roundtrip_pair_leg_sharing;
  double leg_frac_active = roundtrip_pair_leg_sharing_frac; // raised by the relaxation ladder
  bool leg_test_off = false;                                // the ladder's last rung
  uint8_t leg_relax_rung = 0; // rung the build loop runs under (0 = the main walk)
  uint32_t pair_leg_share_rejects = 0, pair_leg_relaxed = 0;
  uint8_t pair_leg_relax_rung_used = 0;
  // In leg mode the evaluation cap is a PER-RUNG budget of fresh pair evaluations (the
  // v0 lesson: a strict filter that is never dropped walked 45 k candidates on one 200 km
  // ask, cached every one of them, and was OOM-killed); a rung whose budget is spent hands
  // the queue to the next rung instead of walking on.
  uint32_t rung_evals = 0;
  uint32_t rung_eval_count[4] = {0, 0, 0, 0};
  auto rung_budget_spent = [&]() { return leg_mode && rung_evals >= roundtrip_pair_eval_cap; };
  auto pair_shares = [&](const PairKeys& k, const std::vector<PairKeys>& bank) -> bool {
    if (!roundtrip_pair_sharing)
      return false;
    // P2: best effort — past the evaluation cap the near-dup filter is dropped.  P2.1 leg
    // mode: the cap bounds the WALK (per rung), never the test.
    if (!leg_mode && pair_sinks_considered > roundtrip_pair_eval_cap)
      return false;
    for (const auto& prev : bank)
      if (shared_fraction(k, prev) > roundtrip_sharing_frac)
        return true;
    return false;
  };
  // proto/v4-p2.1: the per-leg test — the candidate's forward leg against a bank entry's
  // roads and their twins, as a fraction of the FORWARD leg.  A shared trunk out of the
  // start is ~30 % of a loop (invisible to the whole-pair 0.6 cliff) and 50-80 % of the
  // leg the rider rides out.
  auto fwd_shared_fraction = [&](const PairKeys& cand, const PairKeys& prev) -> double {
    if (cand.fwd_total <= 0.0)
      return 0.0;
    double shared = 0.0;
    for (const auto& [k, len] : cand.fwd_ridden)
      if (prev.ridden.count(k) || prev.twins.count(k))
        shared += len;
    return shared / cand.fwd_total;
  };
  auto leg_shares = [&](const PairKeys& k, const std::vector<PairKeys>& bank) -> bool {
    if (!leg_mode || leg_test_off)
      return false;
    for (const auto& prev : bank)
      if (fwd_shared_fraction(k, prev) > leg_frac_active)
        return true;
    return false;
  };
  // proto/v4-p2.1: a rejected evaluation keeps its verdict and drops its payload — the arc
  // paths and the key containers are what the v0 cache ran out of memory on.  A twin
  // reject is re-evaluated from scratch by the last resort (checked = false), so it is
  // released too.
  auto release_eval = [](PairEval& pe) {
    std::vector<uint32_t>().swap(pe.pair.tree);
    std::vector<uint32_t>().swap(pe.pair.other);
    pe.keys = PairKeys{};
  };
  auto eval_pair = [&](uint32_t ci) -> PairEval& {
    PairEval& pe = pair_cache[ci];
    if (pe.checked)
      return pe;
    pe.checked = true;
    ++pair_sinks_considered;
    ++rung_evals; // proto/v4-p2.1: the rung's budget counts fresh evaluations only
    ++rung_eval_count[leg_relax_rung];
    const auto t_eval = ledger_clock::now();
    struct EvalTimer {
      double& acc;
      ledger_clock::time_point t0;
      ~EvalTimer() {
        acc += std::chrono::duration<double, std::milli>(ledger_clock::now() - t0).count();
      }
    } eval_timer{pair_eval_ms, t_eval};
    const uint32_t j = pairs->junction_of(cands[ci].label_index);
    pe.pair = pairs->construct(j);
    if (!pe.pair.exists) {
      ++pair_none; // Suurballe's existence condition: an edge every route home crosses
      pe.reject = "no_pair";
      release_eval(pe);
      return pe;
    }
    pe.total_len = pe.pair.tree_len + pe.pair.other_len;
    pe.dist_err = target > 0.0 ? std::fabs(pe.total_len - target) / target : 0.0;
    if (pe.dist_err > roundtrip_pair_band) {
      ++pair_band_rejects;
      pe.reject = "band";
      if (rt_debug) {
        std::cerr << "[rt-debug] band reject cand=" << ci << " tree_len=" << pe.pair.tree_len
                  << " other_len=" << pe.pair.other_len << " arcs " << pe.pair.tree.size() << "/"
                  << pe.pair.other.size() << " target=" << target << "\n";
        for (const auto* path : {&pe.pair.tree, &pe.pair.other}) {
          std::cerr << "[rt-debug]   path:";
          for (uint32_t h : *path) {
            const auto& a = pairs->arc(h);
            std::cerr << " (" << a.from << "->" << a.to << " len=" << static_cast<int>(a.len)
                      << (a.tree ? " T" : "") << " fwd="
                      << (a.fwd_label == RoundTripPairPass::kNone
                              ? 0
                              : expander.labels()[a.fwd_label].edgeid().value)
                      << " ret=" << a.ret_edge << ")";
          }
          std::cerr << "\n";
        }
      }
      release_eval(pe);
      return pe;
    }
    // Curviness over BOTH legs, the twins-aware self-overlap (forward vs return AND
    // return vs return — the H3 shape of the same-road study), and the key set for the
    // K x K sharing filter, all from the arc sequences: ~one tile lookup per arc.
    const auto& labels = expander.labels();
    graph_tile_ptr tile, opp_tile;
    std::vector<uint64_t> tw;
    std::unordered_set<uint64_t> fwd_ids, ret_ids;
    double curv = 0.0, len = 0.0, overlap = 0.0;
    // the road an arc stands for: its forward label's edge, else the edge the return rides
    auto arc_edge = [&](const RoundTripPairPass::HArc& h) -> GraphId {
      return h.fwd_label != RoundTripPairPass::kNone ? labels[h.fwd_label].edgeid()
                                                     : GraphId(h.ret_edge);
    };
    auto canon_of = [&](const GraphId& e, const DirectedEdge* de) {
      opp_tile = tile;
      const GraphId opp = reader->GetOpposingEdgeId(e, opp_tile);
      return RoadTwinIndex::canonical_id(de, e, opp);
    };
    double cum = 0.0;
    for (uint32_t h : pe.pair.tree) {
      const auto& a = pairs->arc(h);
      const GraphId e = arc_edge(a);
      const double elen = a.len;
      cum += elen;
      len += elen;
      if (!e.is_valid() || !reader->GetGraphTile(e, tile))
        continue;
      const DirectedEdge* de = tile->directededge(e);
      curv += static_cast<double>(de->curvature()) * elen;
      if (cum <= kStartExemptionMeters)
        continue;
      const uint64_t canon = canon_of(e, de);
      fwd_ids.insert(canon);
      pe.keys.ridden[canon] += elen;
      pe.keys.fwd_ridden[canon] += elen; // proto/v4-p2.1: the forward leg's own record
      if (twin_index) {
        tw.clear();
        twin_index->append_twins(canon, tw);
        fwd_ids.insert(tw.begin(), tw.end());
        pe.keys.twins.insert(tw.begin(), tw.end());
      }
    }
    pe.keys.fwd_total = len; // proto/v4-p2.1: the tree path's whole length, exempt stem included
    // The second path is ridden home backwards, so its distance from the start IS the
    // return's distance from the ride end: the exemption test reads the same number.
    cum = 0.0;
    for (uint32_t h : pe.pair.other) {
      const auto& a = pairs->arc(h);
      const GraphId e = arc_edge(a);
      const double elen = a.len;
      cum += elen;
      len += elen;
      if (!e.is_valid() || !reader->GetGraphTile(e, tile))
        continue;
      const DirectedEdge* de = tile->directededge(e);
      curv += static_cast<double>(de->curvature()) * elen;
      if (cum <= kStartExemptionMeters)
        continue;
      const uint64_t canon = canon_of(e, de);
      bool hit = fwd_ids.count(canon) > 0 || !ret_ids.insert(canon).second;
      if (twin_index) {
        tw.clear();
        twin_index->append_twins(canon, tw);
        for (uint64_t t : tw)
          hit = hit || ret_ids.count(t) > 0;
        pe.keys.twins.insert(tw.begin(), tw.end());
      }
      if (hit)
        overlap += elen;
      pe.keys.ridden[canon] += elen;
    }
    pe.keys.total = len;
    pe.curviness = len > 0.0 ? static_cast<float>(curv / len / 15.0) : 0.f;
    pe.self_overlap_m = overlap;
    {
      // at least one of the two paths must be ridable OUT (every arc a settled forward
      // edge); the other is ridden home backwards — repaired where it cannot be.
      auto fwd_ok = [&](const std::vector<uint32_t>& arcs) {
        for (uint32_t h : arcs)
          if (pairs->arc(h).fwd_label == RoundTripPairPass::kNone)
            return false;
        return true;
      };
      if (!fwd_ok(pe.pair.tree) && !fwd_ok(pe.pair.other)) {
        ++pair_fwd_illegal;
        pe.reject = "fwd_illegal";
        release_eval(pe);
        return pe;
      }
    }
    // The pair's own twin test (not the geometry gate's — that judges built loops):
    // the corridor's twins, forward-vs-return AND return-vs-return, at the gate's
    // threshold.
    if (roundtrip_pair_twin_reject && overlap >= roundtrip_gate_twin_ride_m && !pair_allow_twin) {
      ++pair_twin_rejects; // the geometry gate's twin-ride verdict, at selection
      pe.reject = "twin";
      pair_twin_candidates.push_back(ci);
      release_eval(pe);
      return pe;
    }
    // Selection score: the pair's whole-loop curviness discounted by its distance error
    // (the built-loop score of P1.1 §3 with overlap == 0 by construction).  The pair
    // COST is what the construction minimised; it is surfaced in the ledger.
    pe.score = static_cast<double>(pe.curviness) / (1.0 + roundtrip_rank_disterr_w * pe.dist_err);
    pe.ok = true;
    return pe;
  };

  // Per bearing sector, pick a seed-varied turnaround among the top-M curviest. seed=fixed
  // is reproducible; a fresh seed (Shuffle) rotates the pick to a different curvy loop.
  constexpr uint32_t kShuffleTopM = 8;
  for (uint32_t b = 0; b < buckets; ++b) {
    auto& bk = bucketed[b];
    if (bk.empty())
      continue;
    std::sort(bk.begin(), bk.end(), [&](uint32_t a, uint32_t c) {
      return cands[a].curviness_per_km > cands[c].curviness_per_km;
    });
    if (pairs) {
      // proto/v4-p2: the sector's top-M pairs are constructed and judged before the pick
      // — no pair, off band, twin ride, near-dup of a chosen loop are all selection-time
      // rejects that never buy a build; the survivors are ranked by pair score and the
      // seed rotates among them.
      const uint32_t topm =
          std::min<uint32_t>(std::max<uint32_t>(1, roundtrip_pair_shortlist),
                             static_cast<uint32_t>(bk.size()));
      std::vector<uint32_t> surv;
      for (uint32_t off = 0; off < topm; ++off) {
        const uint32_t pick = bk[off];
        if (!separated(pick))
          continue;
        const PairEval& pe = eval_pair(pick);
        if (!pe.ok)
          continue;
        if (pair_shares(pe.keys, pair_selected_keys)) {
          ++pair_share_rejects;
          continue;
        }
        if (leg_shares(pe.keys, pair_selected_keys)) {
          ++pair_leg_share_rejects; // proto/v4-p2.1: a shared forward trunk, at selection
          continue;
        }
        surv.push_back(pick);
      }
      if (surv.empty())
        continue;
      // proto/v4-p2.1: the diversity term — a survivor's score is discounted by its
      // whole-pair sharing with the chosen set, score / (1 + w * s), BEFORE the seed
      // rotation; the rotation is unchanged and bounds what the term can do (it orders
      // the survivors, the seed still picks among them).  w = 0 is P2's order exactly.
      std::unordered_map<uint32_t, double> div_score;
      for (uint32_t c : surv) {
        double s = 0.0;
        if (roundtrip_pair_diversity_w > 0.0)
          for (const auto& prev : pair_selected_keys)
            s = std::max(s, shared_fraction(pair_cache[c].keys, prev));
        div_score[c] = pair_cache[c].score / (1.0 + roundtrip_pair_diversity_w * s);
      }
      std::stable_sort(surv.begin(), surv.end(),
                       [&](uint32_t a, uint32_t c) { return div_score[a] > div_score[c]; });
      const uint32_t pick = surv[(seed + b) % static_cast<uint32_t>(surv.size())];
      chosen.push_back(pick);
      pair_selected_keys.push_back(pair_cache[pick].keys);
      continue;
    }
    const uint32_t topm = std::min<uint32_t>(kShuffleTopM, static_cast<uint32_t>(bk.size()));
    // Start at the seed-rotated pick; walk the top-M until one clears the
    // separation guard (a fully-rejected sector is left to the backfill).
    for (uint32_t off = 0; off < topm; ++off) {
      const uint32_t pick = bk[(seed + b + off) % topm];
      if (separated(pick)) {
        chosen.push_back(pick);
        break;
      }
    }
  }
  // Backfill from the remaining unique-node turnarounds, best curviness first.
  if (chosen.size() < want) {
    std::vector<uint32_t> rest;
    for (uint32_t i : uniq)
      if (std::find(chosen.begin(), chosen.end(), i) == chosen.end())
        rest.push_back(i);
    std::sort(rest.begin(), rest.end(), [&](uint32_t a, uint32_t c) {
      return cands[a].curviness_per_km > cands[c].curviness_per_km;
    });
    for (uint32_t i = 0; i < rest.size() && chosen.size() < want; ++i) {
      if (!separated(rest[i]))
        continue;
      if (pairs) {
        if (rung_budget_spent())
          break; // proto/v4-p2.1: the rung's evaluation budget bounds the backfill walk too
        const PairEval& pe = eval_pair(rest[i]);
        if (!pe.ok)
          continue;
        if (pair_shares(pe.keys, pair_selected_keys)) {
          ++pair_share_rejects;
          continue;
        }
        if (leg_shares(pe.keys, pair_selected_keys)) {
          ++pair_leg_share_rejects;
          continue;
        }
        pair_selected_keys.push_back(pe.keys);
      }
      chosen.push_back(rest[i]);
    }
  }

  // Refill queue (ADR-0037 build-until-full): hardening, walk-back, and return-leg
  // failures all strike AFTER selection, so a fixed chosen-set systematically
  // under-fills the bank. Candidates are consumed from an ordered queue until `want`
  // loops are actually built: the seed-varied sector picks first, then the
  // curviness-sorted rest of the band.
  std::vector<uint32_t> queue = chosen;
  std::unordered_set<uint64_t> queued;
  {
    for (uint32_t c : chosen)
      queued.insert(cands[c].node);
    std::vector<uint32_t> rest;
    for (uint32_t i : uniq)
      if (queued.insert(cands[i].node).second)
        rest.push_back(i);
    std::sort(rest.begin(), rest.end(), [&](uint32_t a, uint32_t c) {
      return cands[a].curviness_per_km > cands[c].curviness_per_km;
    });
    queue.insert(queue.end(), rest.begin(), rest.end());
  }

  // Return-leg router (ADR-0037 hard-excluded return): the forward leg's edges (both
  // directions) are hard-excluded from the return search, so the loop must close on
  // fresh roads — except within the Start Exemption, where the network-forced first
  // stretch may carry the loop home. Every forward edge stays soft-leashed as before
  // (the leash is what bites on the exempted stretch). If no route home exists under
  // exclusion, one retry under the soft leash alone serves a Fallback Loop — tagged on
  // the Loop and counted, so the build-time Defect Gate (T3) can give fallbacks the
  // full-leg decode and the harness can gate-count them.
  bidir_astar.set_interrupt(interrupt);
  cost->set_allow_destination_only(true);
  cost->set_pass(0);
  // Request-level avoids (loki avoid_locations) survive the per-candidate swaps.
  const auto avoid_baseline = cost->user_avoid_edges();
  uint32_t fallback_count = 0;
  // proto/v4-p1 road-identity ledger counters.
  uint32_t twins_excluded = 0, parallels_leashed = 0, identity_legs = 0;
  // proto/v4-p1.1 fallback-rung ledger (item 5): how many legs finished on each rung.
  uint32_t rung_hits[4] = {0, 0, 0, 0};
  uint32_t parallel_rung_tried = 0, parallel_rung_converted = 0;
  // Cross-candidate corridor memory (wayfinder #46): directed-edge value -> number of
  // already-committed loops that rode it (both directions, outside the Start Exemption
  // disk). Grows as the bank fills; read by route_leg to surcharge shared corridors.
  std::unordered_map<uint64_t, uint32_t> bank_edge_count;
  // Generalized corridor-aware leg router (ADR-0037): routes from -> to with the given
  // corridor hard-excluded beyond the Start Exemption, soft-leashed everywhere, and its
  // junction edges progress-grade-penalized. The round-trip return is corridor=forward
  // leg, to=start; the Second Via sub-legs reuse it with wider corridors.
  //
  // proto/v4-p1.1 FALLBACK RUNGS (item 5).  v3 had one step: "hard exclusion, or drop
  // every hard exclusion".  The ladder is now
  //    rung 0  corridor + twins barred beyond the Start Exemption; corridor, twins and
  //            parallels leashed; parallels carry the progress grade.
  //    rung 1  TWINS RELEASED from the bar (still leashed) and parallels released from
  //            the leash and the grade — the corridor's own pavement is still barred, so
  //            a rung-1 loop still never retraces it.
  //    rung 2  full soft leash: no hard exclusion at all (the v3 Fallback Loop).
  // The ticket's rung 1 was "release parallels, keep twins barred".  That cannot work:
  // mark_edges_used / mark_rejoin_edges are multiplicative cost factors read by
  // EdgeFactor (dynamiccost.h:1292-1308) and never block, so a rung that only relaxes
  // soft costs cannot turn "no route home" into a route — it can only pay for a second
  // exhaustive A*.  It is kept behind thor.roundtrip_fallback_parallel_rung (default
  // off) so the claim can be measured rather than argued.
  auto route_leg = [&](const std::vector<PathInfo>& corridor, valhalla::Location& from,
                       valhalla::Location& to, uint8_t& rung) -> std::vector<PathInfo> {
    const auto t_rejoin = ledger_clock::now();
    cost->clear_used_edges();
    const double total_dist = std::max(1.0f, corridor.back().path_distance);
    std::vector<uint64_t> vals;
    std::vector<sif::AvoidEdge> hard;      // the corridor's own pavement
    std::vector<sif::AvoidEdge> twin_hard; // proto/v4-p1.1: the twin tier, rung-1 releasable
    struct NodeAt {
      GraphId node;
      double dist;
    };
    std::vector<NodeAt> path_nodes;
    // proto/v4-p1 TIERED ROAD IDENTITY.  corridor_vals = the corridor and its TWINS
    // (the opposite carriageway, sidecar radius <= 30 m) — one physical road, so the
    // twin gets exactly the corridor's treatment: leashed everywhere, hard-excluded
    // beyond the Start Exemption.  PARALLELS (30-80 m) are a different road that
    // shadows this one: they join the soft leash and the progress-graded rejoin map,
    // which nudges the return off them without boxing it in.
    std::vector<uint64_t> corridor_vals;
    std::vector<std::pair<uint64_t, double>> parallel_at; // edge value -> corridor distance
    std::vector<uint64_t> tw, par;
    // Walked on the BUILT corridor (not the label chain): edges the walk-back
    // dropped from the leg are neither excluded nor leashed.
    for (const auto& pi : corridor) {
      const GraphId e = pi.edgeid;
      vals.push_back(e.value);
      corridor_vals.push_back(e.value);
      const GraphId opp = reader->GetOpposingEdgeId(e);
      if (opp.is_valid()) {
        vals.push_back(opp.value);
        corridor_vals.push_back(opp.value);
      }
      const bool beyond = static_cast<double>(pi.path_distance) > kStartExemptionMeters;
      if (beyond) {
        hard.push_back({e, 0.0});
        if (opp.is_valid())
          hard.push_back({opp, 0.0});
      }
      graph_tile_ptr tile = reader->GetGraphTile(e);
      if (!tile)
        continue;
      const DirectedEdge* de = tile->directededge(e);
      path_nodes.push_back({de->endnode(), static_cast<double>(pi.path_distance)});
      if (!twin_index)
        continue;
      const uint64_t canon = RoadTwinIndex::canonical_id(de, e, opp);
      tw.clear();
      twin_index->append_twins(canon, tw);
      for (uint64_t cv : tw) {
        const GraphId t(cv);
        const GraphId topp = reader->GetOpposingEdgeId(t);
        vals.push_back(t.value);
        corridor_vals.push_back(t.value);
        if (topp.is_valid()) {
          vals.push_back(topp.value);
          corridor_vals.push_back(topp.value);
        }
        if (beyond) {
          twin_hard.push_back({t, 0.0});
          if (topp.is_valid())
            twin_hard.push_back({topp, 0.0});
          ++twins_excluded;
        }
      }
      if (!parallel_tier)
        continue;
      par.clear();
      twin_index->append_parallels(canon, par);
      const double at = static_cast<double>(pi.path_distance);
      for (uint64_t cv : par) {
        const GraphId t(cv);
        const GraphId topp = reader->GetOpposingEdgeId(t);
        // proto/v4-p1.1: parallels live in their own tier (parallel_at) so rung 1 can
        // release them; P1 pushed them straight into `vals`.
        parallel_at.emplace_back(t.value, at);
        if (topp.is_valid())
          parallel_at.emplace_back(topp.value, at);
        ++parallels_leashed;
      }
    }
    if (twin_index)
      ++identity_legs;
    // proto/v4-p1.1: the leash set is re-assembled per rung, so keep the tiers apart.
    // `vals` = corridor + opposites + twins (+ their opposites); parallel_vals = the
    // parallel tier, dropped from rung 1 up.
    std::vector<uint64_t> parallel_vals;
    parallel_vals.reserve(parallel_at.size());
    for (const auto& [ev, _] : parallel_at)
      parallel_vals.push_back(ev);
    auto apply_leash = [&](bool with_parallels) {
      cost->clear_used_edges();
      cost->mark_edges_used(vals);
      if (with_parallels)
        cost->mark_edges_used(parallel_vals);
    };
    apply_leash(true);

    // Progress-graded rejoin (ADR-0037): junction edges hanging off forward-path nodes
    // get a penalty graded by how far along the forward leg the node sits. Edges on the
    // forward path itself are skipped — the leash and the hard exclusion own those.
    auto build_rejoin = [&](bool with_parallels) {
    std::unordered_map<uint64_t, float> rejoin;
    const float leash_surcharge = cost->reuse_factor() - 1.0f;
    if (leash_surcharge > 0.f) {
      const std::unordered_set<uint64_t> corridor_set(corridor_vals.begin(),
                                                      corridor_vals.end());
      auto grade_at = [&](double dist) {
        return 1.0f + leash_surcharge * kRejoinGradeShare *
                          static_cast<float>(1.0 - std::min(1.0, dist / total_dist));
      };
      auto bump = [&](uint64_t ev, float grade) {
        if (corridor_set.count(ev))
          return;
        auto it = rejoin.emplace(ev, grade);
        if (!it.second && grade > it.first->second)
          it.first->second = grade;
      };
      for (const auto& pn : path_nodes) {
        const float grade = grade_at(pn.dist);
        // proto/v4-p1 F04: the rejoin map is built over the PHYSICAL junction — all
        // hierarchy levels — exactly as correlate_node and the walk-back already are.
        // v3 read only the corridor node's own level, so a primary corridor got no
        // grade on its secondary/tertiary exits and a secondary corridor none on its
        // unclassified ones: the anti-shadow mechanism was absent at precisely the
        // class-mixed rural junctions where stems form.
        for_each_junction_edge(pn.node, *reader, [&](const GraphId& eid) {
          bump(eid.value, grade);
          const GraphId opp = reader->GetOpposingEdgeId(eid);
          if (opp.is_valid())
            bump(opp.value, grade);
        });
      }
      // proto/v4-p1: the parallel tier — a road 30-80 m from the corridor carries the
      // same progress grade as the corridor stretch it shadows.  Released at rung 1.
      if (with_parallels)
        for (const auto& [ev, dist] : parallel_at)
          bump(ev, grade_at(dist));
    }
    // Cross-candidate corridor penalty (wayfinder #46): every edge earlier loops in this
    // bank already rode carries a soft surcharge on THIS return leg, graded by the
    // prior-use count and capped. Merged into the rejoin tier (max wins) so EdgeFactor
    // needs one lookup. The bank memory holds only edges outside the Start Exemption, so
    // the network-forced start stem is never surcharged (it must stay routable home).
    for (const auto& [ev, cnt] : bank_edge_count) {
      const float f = 1.0f + static_cast<float>(roundtrip_xcand_strength) *
                                 static_cast<float>(std::min(cnt, roundtrip_xcand_cap));
      auto it = rejoin.emplace(ev, f);
      if (!it.second && f > it.first->second)
        it.first->second = f;
    }
    if (!rejoin.empty())
      cost->mark_rejoin_edges(std::move(rejoin));
    };
    build_rejoin(true);
    cost->set_user_avoid_edges(avoid_baseline);
    std::vector<sif::AvoidEdge> bars = hard;
    bars.insert(bars.end(), twin_hard.begin(), twin_hard.end());
    if (!bars.empty())
      cost->AddUserAvoidEdges(bars);
    rejoin_ms += ms_since(t_rejoin);
    bidir_astar.Clear();
    std::vector<std::vector<PathInfo>> paths;
    const auto t_astar = ledger_clock::now();
    try {
      paths = bidir_astar.GetBestPath(from, to, *reader, mode_costing, mode, options);
    } catch (const std::exception&) {
      // "No route home under exclusion" can surface as a throw or as an empty result;
      // either way the ladder below decides.
      paths.clear();
    }
    astar_ms += ms_since(t_astar);
    rung = 0;

    // One rung of the ladder: re-arm the leash/grade/bar tiers and search again.
    auto retry = [&](bool with_parallels, bool bar_twins) {
      if (interrupt)
        (*interrupt)(); // rethrow a swallowed client disconnect before paying an A*
      apply_leash(with_parallels);
      build_rejoin(with_parallels);
      cost->set_user_avoid_edges(avoid_baseline);
      std::vector<sif::AvoidEdge> b = hard;
      if (bar_twins)
        b.insert(b.end(), twin_hard.begin(), twin_hard.end());
      if (!b.empty())
        cost->AddUserAvoidEdges(b);
      bidir_astar.Clear();
      const auto t_fb = ledger_clock::now();
      try {
        paths = bidir_astar.GetBestPath(from, to, *reader, mode_costing, mode, options);
      } catch (const std::exception&) {
        paths.clear();
      }
      astar_fb_ms += ms_since(t_fb);
    };

    // Ticket-literal rung: parallels released, twins still barred.  Default off — see
    // the reachability note above; the knob exists to measure the zero.
    if (paths.empty() && !bars.empty() && roundtrip_fallback_parallel_rung && parallel_tier &&
        !parallel_at.empty()) {
      ++parallel_rung_tried;
      retry(false, true);
      if (!paths.empty())
        ++parallel_rung_converted;
    }
    // rung 1: twins released to the leash, parallels released; the corridor stays barred.
    if (paths.empty() && !bars.empty() && roundtrip_fallback_rungs && !twin_hard.empty()) {
      retry(false, false);
      if (!paths.empty())
        rung = 1;
    }
    // rung 2: the v3 Fallback Loop — every hard exclusion dropped.
    if (paths.empty() && !bars.empty()) {
      rung = 2;
      ++fallback_count;
      apply_leash(true);
      build_rejoin(true);
      cost->set_user_avoid_edges(avoid_baseline);
      bidir_astar.Clear();
      const auto t_fb = ledger_clock::now();
      paths = bidir_astar.GetBestPath(from, to, *reader, mode_costing, mode, options);
      astar_fb_ms += ms_since(t_fb);
    }
    if (paths.empty())
      rung = 3; // no route on any rung — the candidate fails
    ++rung_hits[rung];
    cost->set_user_avoid_edges(avoid_baseline);
    cost->clear_used_edges();
    return paths.empty() ? std::vector<PathInfo>{} : paths.front();
  };

  // 3) Build loops from the queue until `want` are built (ADR-0037 build-until-full):
  //    forward leg from the tree + hard-excluded/leashed return. Per-candidate failures
  //    (sink/tip skips, no return) consume the next queue entry instead of shrinking
  //    the fill. Built turnarounds keep the T9 min-separation among themselves.
  struct Loop {
    std::vector<PathInfo> fwd, ret;
    valhalla::Location turn;
    float curviness; // the HARVEST CHAIN's score — v3/P1's whole ranking key
    // Fallback Loop tag (ADR-0037): the return leg came from the soft-leash retry, so
    // it may reuse forward edges anywhere. The Defect Gate reads this to give the
    // loop a full-leg decode (a seam-window verdict provably leaks wrapped bounces).
    bool fallback;
    // proto/v4-p1.1 (item 3, F20's full fix): the BUILT loop's own score.
    uint8_t rung = 0;           // 0 clean hard-exclude / 1 twins released / 2 soft leash
    float loop_curviness = 0.f; // curvature-weighted mean over BOTH legs, 0..1 per km
    double self_overlap_m = 0;  // twins-aware D1-style metres, outside the exemption
    double dist_err = 0;        // |built - target| / target
    double bounce_m = 0;        // longest mid-return exact mirror (the geometry gate)
    bool gated = false;         // failed the geometry gate — the dirty tier, served last
    bool seam_reject = false;   // failed the ADR-0037 SEAM gate: an exact-mirror spike
    double score = 0;           // the documented rank score below
    // proto/v4-p2: provenance of a pair-built loop.
    uint32_t cand = baldr::kInvalidLabel; // the candidate it was built from
    bool pair_built = false;              // forward = the tree path, return = the other path reversed
    uint8_t pair_bridges = 0;             // non-reversible return stretches repaired with a local A*
    bool pair_full_repair = false;        // the whole return was rebuilt with route_leg (P1.1)
    double pair_cost = 0, pair_surplus = 0, pair_fwd_len = 0, pair_ret_len = 0;
    // proto/v4-p2.1: admitted under relaxation rung 1-3 of the per-leg threshold (0 = the
    // main walk); with roundtrip_pair_relaxed_last it ranks after the non-relaxed ones.
    uint8_t relaxed = 0;
    // Rank tier, absolute: clean hard-exclude < twins released < soft leash < gated.
    uint8_t tier() const {
      return gated ? 3 : rung;
    }
  };
  std::vector<Loop> loops;
  std::vector<Loop> dirty_loops;
  // Byte-identical loop dedup (the T2 hardening side effect), enforced AT BUILD TIME:
  // with the U-turn door dropped, adjacent turnarounds can converge onto the same
  // detour loop. Skipping the duplicate here lets the queue refill the slot with a
  // genuinely distinct loop instead of silently shrinking the fill at serialization.
  std::set<std::vector<uint64_t>> served_sigs;
  // Item 4 sharing filter (wayfinder #46): each kept loop's undirected fresh-road edge
  // keys -> length, for the K×K near-dup overlap test. Filled only when the filter is on.
  std::vector<std::unordered_map<uint64_t, double>> built_keylens;
  uint32_t sharing_filter_rejects = 0; // proto/v4-p2 ledger
  // A loop's undirected fresh-road edge-key -> length (Start-Exemption edges dropped so
  // the forced start stem never reads as shared); returns the loop's total length.
  auto loop_keylen = [&](const Loop& L, std::unordered_map<uint64_t, double>& kl,
                         std::unordered_map<uint64_t, double>& kl_twins) -> double {
    std::vector<uint64_t> tw;
    const double fwd_total = static_cast<double>(L.fwd.back().path_distance);
    const double ret_total = static_cast<double>(L.ret.back().path_distance);
    auto add = [&](const std::vector<PathInfo>& leg, bool is_ret, double total) {
      double p = 0.0;
      for (const auto& pi : leg) {
        const double cum = static_cast<double>(pi.path_distance);
        const double seglen = cum - p;
        p = cum;
        const double from_start = is_ret ? (total - cum) : cum;
        if (from_start <= kStartExemptionMeters)
          continue;
        const GraphId e = pi.edgeid;
        const GraphId opp = reader->GetOpposingEdgeId(e);
        const uint64_t key = (opp.is_valid() && opp.value < e.value) ? opp.value : e.value;
        kl[key] += seglen;
        // proto/v4-p2: TWINS-AWARE keys — a later loop riding the other carriageway of
        // a road this loop rode is riding the same road (the ticket's post-pass K x K
        // filter).  The twin entries carry the road's metres so the overlap test reads
        // them like the road itself; they are never counted in this loop's own total.
        if (pairs && twin_index) {
          tw.clear();
          twin_index->append_twins(key, tw);
          for (uint64_t t : tw)
            kl_twins[t] += seglen;
        }
      }
    };
    add(L.fwd, false, fwd_total);
    add(L.ret, true, ret_total);
    return fwd_total + ret_total;
  };

  // proto/v4-p1.1 BUILT-LOOP SCORE (item 3 — F20's full fix).  v3 and P1 ranked on
  // `cands[ci].curviness_per_km`: the harvest label chain's curviness, computed before
  // the walk-back pops the tip, blind to the return leg, to distance error and to
  // self-overlap.  The score below reads the loop that will actually be ridden:
  //
  //     score = curviness(whole loop) / ((1 + Wo * overlap_frac) * (1 + Wd * dist_err))
  //
  //   curviness(whole loop)  sum(curvature * len) / sum(len) / 15 over BOTH legs, so a
  //                          curvy forward leg no longer hides a straight motorway home.
  //   overlap_frac           twins-aware self-overlap metres / loop metres.  A return
  //                          edge counts as overlap when its canonical id, or a sidecar
  //                          TWIN of it, is on the forward leg outside the Start
  //                          Exemption — the engine-side D1 ("ridden both ways").
  //   dist_err               |built - target| / target (F21: ranking was distance-blind).
  //   Wo = 4, Wd = 1         thor.roundtrip_rank_{overlap,disterr}_w.  Wo = 4 makes a
  //                          25 % self-overlapping loop score half a clean one; Wd = 1
  //                          makes a 20 %-off loop lose ~17 %, which is about what a
  //                          rider trades a fifth of the distance for.
  //
  // Tiers come first and are absolute: rung 0 (clean hard-exclude) before rung 1 (twins
  // released) before rung 2 (soft-leash Fallback); the gate's dirty stash is served only
  // if nothing else is.  Within a tier the sort is stable, so equal scores keep queue
  // order and the seed still owns the tie-break.
  auto score_built = [&](Loop& L) {
    graph_tile_ptr tile, opp_tile;
    double curv = 0.0, len = 0.0;
    std::unordered_set<uint64_t> fwd_ids;
    std::vector<uint64_t> tw;
    for (const auto& pi : L.fwd) {
      const GraphId e = pi.edgeid;
      if (!reader->GetGraphTile(e, tile))
        continue;
      const DirectedEdge* de = tile->directededge(e);
      curv += static_cast<double>(de->curvature()) * de->length();
      len += de->length();
      if (static_cast<double>(pi.path_distance) <= kStartExemptionMeters)
        continue;
      opp_tile = tile;
      const GraphId opp = reader->GetOpposingEdgeId(e, opp_tile);
      const uint64_t canon = RoadTwinIndex::canonical_id(de, e, opp);
      fwd_ids.insert(canon);
      if (twin_index) {
        tw.clear();
        twin_index->append_twins(canon, tw);
        fwd_ids.insert(tw.begin(), tw.end());
      }
    }
    const double fwd_total = static_cast<double>(L.fwd.back().path_distance);
    const double ret_total = static_cast<double>(L.ret.back().path_distance);
    double overlap = 0.0, prev = 0.0;
    std::unordered_set<uint64_t> ret_ids; // proto/v4-p2: the return's own pavement/twins
    for (const auto& pi : L.ret) {
      const GraphId e = pi.edgeid;
      const double cum = static_cast<double>(pi.path_distance);
      const double seglen = cum - prev;
      prev = cum;
      if (!reader->GetGraphTile(e, tile))
        continue;
      const DirectedEdge* de = tile->directededge(e);
      curv += static_cast<double>(de->curvature()) * de->length();
      len += de->length();
      if (ret_total - cum <= kStartExemptionMeters)
        continue;
      opp_tile = tile;
      const GraphId opp = reader->GetOpposingEdgeId(e, opp_tile);
      const uint64_t canon = RoadTwinIndex::canonical_id(de, e, opp);
      bool hit = fwd_ids.count(canon) > 0;
      if (pairs) {
        // proto/v4-p2: the RETURN-VS-RETURN term the same-road study asked for (H3):
        // the return riding its own pavement or its own twin — the M11 excursion on
        // the opposite carriageway — counts as self-overlap too.  Pair mode only, so
        // the P1.1 control on this binary keeps its measured meter.
        if (!ret_ids.insert(canon).second)
          hit = true;
        if (!hit && twin_index) {
          tw.clear();
          twin_index->append_twins(canon, tw);
          for (uint64_t t : tw)
            if (ret_ids.count(t)) {
              hit = true;
              break;
            }
        }
      }
      if (hit)
        overlap += seglen;
    }
    const double total = fwd_total + ret_total;
    L.loop_curviness = len > 0.0 ? static_cast<float>(curv / len / 15.0) : 0.0f;
    L.self_overlap_m = overlap;
    L.dist_err = target > 0.0 ? std::fabs(total - target) / target : 0.0;
    const double ov_frac = total > 0.0 ? overlap / total : 0.0;
    L.score = static_cast<double>(L.loop_curviness) /
              ((1.0 + roundtrip_rank_overlap_w * ov_frac) *
               (1.0 + roundtrip_rank_disterr_w * L.dist_err));
  };
  std::vector<PointLL> built_lls;
  std::unordered_set<uint64_t> built_nodes; // the distance-correction node guard
  uint32_t correction_count = 0;
  auto built_separated = [&](const PointLL& ll) {
    for (const auto& b : built_lls)
      if (static_cast<double>(ll.Distance(b)) < min_separation_m)
        return false;
    return true;
  };
  bool widened = false;
  // The widened ScanBand re-scan, shared by the lazy Distance Flex stall path and the
  // Second Via candidate pool (both want the full band; idempotent via `widened`).
  auto widen_pool = [&]() {
    if (widened)
      return;
    widened = true;
    const auto& sll = options.locations(0).ll();
    const auto t_scan = ledger_clock::now();
    auto wide = expander.ScanBand(*reader, PointLL{sll.lng(), sll.lat()}, target,
                                  kFlexLoFrac, kFlexHiFrac);
    scan_ms += ms_since(t_scan);
    const uint32_t base = static_cast<uint32_t>(cands.size());
    for (const auto& t : wide) {
      if (!pairs) {
        if (queued.insert(t.node).second)
          cands.push_back(t);
        continue;
      }
      Turnaround adopted;
      if (pair_adopt(t, adopted) && queued.insert(adopted.node).second)
        cands.push_back(adopted);
    }
    std::vector<uint32_t> widx;
    for (uint32_t i = base; i < static_cast<uint32_t>(cands.size()); ++i)
      widx.push_back(i);
    std::sort(widx.begin(), widx.end(), [&](uint32_t a, uint32_t c) {
      return cands[a].curviness_per_km > cands[c].curviness_per_km;
    });
    queue.insert(queue.end(), widx.begin(), widx.end());
  };

  // Shared tail of every build — the seam gate, the geometry gate and its budget, the
  // built-loop score.  proto/v4-p2 lifted it out of attempt_build so the pair builder
  // runs the identical gate.
  auto finish_build = [&](Loop L) -> std::optional<Loop> {
    // Build-time Defect Gate (ADR-0037): a loop whose seam carries an exact-mirror
    // stub >= 30 m is not bank-worthy — stash it and refill the slot from the queue;
    // it is served only if the cell would otherwise return no route (clean-first,
    // dirty-last-resort). A hard-exclude success cannot retrace the forward leg at the
    // seam (those edges were barred from its return search), so the seam-window decode
    // is a FINAL verdict for it. A Fallback Loop CAN carry a cross-leg retrace whose
    // mirror apex sits far off-seam, where a window sees no palindrome at all — v3f
    // leaked 25 wrapped bounces exactly this way — so fallbacks always get the full
    // decode.
    const bool fell_back = L.rung >= 2;
    const auto t_seam = ledger_clock::now();
    const double stub = fell_back ? seam_stub_m(L.fwd, L.ret, *reader, 0.0)
                                  : seam_stub_m(L.fwd, L.ret, *reader, kSeamWindowM);
    seam_ms += ms_since(t_seam);
    L.fallback = fell_back;

    // proto/v4-p1.1 GEOMETRY DEFECT GATE (item 4).  ADR-0037's gate reads ONE shape:
    // an exact-mirror stub across the seam.  Two rider-visible shapes walk past it —
    //   (a) a return that rides the forward corridor's TWINS beyond the Start Exemption
    //       (the F02 residue: not the same edge, so no seam palindrome, but the same
    //       physical road ridden the other way).  A rung-0 loop cannot do this (the
    //       twins were barred from its search); a rung-1 or rung-2 loop can, and that is
    //       exactly the deep-bank residue P1 pushed into slots 9-11 rather than removed;
    //   (b) a mid-return exact-mirror bounce, whose apex sits far off-seam (F08 / G6b).
    // Both are now treated like a seam mirror: rejected, slot refilled from the queue,
    // served only if the cell would otherwise return nothing.
    const auto t_gate = ledger_clock::now();
    score_built(L);
    bool gate_twin = false, gate_bounce = false;
    if (roundtrip_geometry_gate) {
      gate_twin = L.self_overlap_m >= roundtrip_gate_twin_ride_m;
      if (!gate_twin) {
        L.bounce_m = leg_mirror_stub_m(L.ret, *reader);
        gate_bounce = L.bounce_m >= roundtrip_gate_return_bounce_m;
      }
    }
    gate_ms += ms_since(t_gate);
    if (stub >= kSeamStubRejectM)
      ++gate_seam;
    if (gate_twin)
      ++gate_twinride;
    if (gate_bounce)
      ++gate_bouncehits;
    // WHO PAYS FOR THE REFILL.  Rejecting a loop and refilling its slot costs a whole
    // extra build — a return A*, its fallback rungs and the gate's own decode.  The
    // first P1.1 corpus refilled on EVERY reject (5.7 per request) and paid for it in
    // both currencies: p50 3.95 s against a 1.19 s same-session control (3.33x), and
    // nine requests serving 1-6 loops of 12 because the attempt budget ran out chasing
    // replacements.  The refill therefore has a BUDGET: the first `gate_refill_budget`
    // rejects per request are stashed and refilled as ADR-0037 does; past it a gated
    // loop is kept, marked, and sorted into the last tier — behind every clean loop, so
    // it can only reach a slot a rider reads if the cell has nothing better anyway.
    //
    // THE BUDGET IS SCOPED TO THE GEOMETRY GATE (§16).  The budget shipped in the
    // sweep was shared with ADR-0037's SEAM gate, and the sweep measured what that
    // costs: a seam reject IS the >= 500 m exact-mirror U-turn spike that Gate v1.3's
    // one absolute forbids, so starving its refill put `spike_ge_500m` 0.00 -> 2.08 %
    // and a 23.8 km stub back into the bank at every bounded budget (§15.5).  A seam
    // reject therefore neither consumes the budget nor can be denied by it — ADR-0037
    // keeps its unconditional refill — and the budget bounds only the two shapes this
    // iteration added, the twin ride and the mid-return bounce.  A loop that trips
    // both gates counts as a seam reject: the spike is the more expensive defect.
    const bool seam_reject = stub >= kSeamStubRejectM;
    if (seam_reject || gate_twin || gate_bounce) {
      L.gated = true;
      L.seam_reject = seam_reject;
      if (seam_reject) {
        ++gate_seam_refills;
        if (dirty_loops.size() < want)
          dirty_loops.push_back(std::move(L));
        return std::nullopt;
      }
      if (gate_refills < gate_refill_budget) {
        ++gate_refills;
        if (dirty_loops.size() < want)
          dirty_loops.push_back(std::move(L));
        return std::nullopt;
      }
      ++gate_kept;
      // Per-kind denial, so the ledger can say which shape the budget actually held
      // back.  Both increment when a loop trips both geometry halves.
      if (gate_twin)
        ++gate_denied_twinride;
      if (gate_bounce)
        ++gate_denied_bounce;
    }
    return L;
  };

  // One full candidate build: walk-back, hardened turnaround, corridor return,
  // Defect Gate, Second Via. Returns the loop WITHOUT committing it (the caller owns
  // dedup, separation bookkeeping, and the distance-correction decision).
  auto attempt_build = [&](uint32_t ci) -> std::optional<Loop> {
    std::vector<PathInfo> fwd = ForwardPath(expander, cands[ci].label_index);
    if (fwd.empty())
      return std::nullopt;
    // Trap-aware walk-back (ADR-0037): a tip inside a dead-end stub retreats to the
    // nearest junction with a probed, genuine way out.
    const auto t_walkback = ledger_clock::now();
    fwd = walk_back_trapped_tip(std::move(fwd), *reader);
    walkback_ms += ms_since(t_walkback);
    // ADR-0037: the forward leg's arrival edge closes the return U-turn door in
    // correlate_node and guarantees the destination trim edge (#53). The turnaround is
    // derived from the built leg's tip, not the harvest record.
    const GraphId arrival = fwd.back().edgeid;
    graph_tile_ptr arrival_tile = reader->GetGraphTile(arrival);
    if (!arrival_tile)
      return std::nullopt;
    valhalla::Location turn =
        correlate_node(arrival_tile->directededge(arrival)->endnode(), *reader, arrival);
    // The return leg needs at least one NON-U-turn outbound edge: a dead-end tip whose
    // only exit was the dropped U-turn door is a forced spike, and a one-way sink has
    // no exit at all — both fail this candidate alone (the #44 contract; bidir A*
    // must never see an origin without a traversable outbound edge).
    uint32_t outbound = 0;
    for (const auto& pe : turn.correlation().edges())
      outbound += pe.begin_node() ? 1 : 0;
    if (outbound == 0)
      return std::nullopt;
    std::vector<PathInfo> ret;
    uint8_t rung = 0;
    try {
      ret = route_leg(fwd, turn, start, rung);
    } catch (const std::exception& e) {
      // A return leg that cannot route fails this candidate only — the same
      // contract as the empty-path skip below. Re-poke the interrupt so a
      // swallowed client-disconnect/shutdown still aborts the whole request.
      if (interrupt)
        (*interrupt)();
      LOG_WARN("roundtrip: return leg failed, candidate skipped: " + std::string(e.what()));
      return std::nullopt;
    }
    if (ret.empty())
      return std::nullopt;
    Loop L;
    L.fwd = std::move(fwd);
    L.ret = std::move(ret);
    L.turn = std::move(turn);
    L.curviness = cands[ci].curviness_per_km;
    L.rung = rung;
    L.cand = ci;
    return finish_build(std::move(L));
  };

  // proto/v4-p2: cost a directed-edge sequence with the request's costing — edge cost,
  // transition cost and turn legality (Allowed) between consecutive edges — into
  // PathInfo.  first_frac / last_frac scale the partial origin / destination edges.
  // Returns the index of the first edge that cannot legally follow its predecessor
  // (kInvalidLabel when the whole sequence costs out).
  auto cost_path = [&](const std::vector<GraphId>& seq, double first_frac, double last_frac,
                       std::vector<PathInfo>& out) -> uint32_t {
    out.clear();
    out.reserve(seq.size());
    Cost acc{};
    double pd = 0.0;
    std::optional<sif::BDEdgeLabel> pred;
    graph_tile_ptr tile, opp_tile, ntile;
    auto reader_getter = [&]() { return baldr::LimitedGraphReader(*reader); };
    for (size_t k = 0; k < seq.size(); ++k) {
      const GraphId e = seq[k];
      if (!reader->GetGraphTile(e, tile)) {
        ++rev_fail_tile;
        return static_cast<uint32_t>(k);
      }
      const DirectedEdge* de = tile->directededge(e);
      opp_tile = tile;
      const GraphId opp = reader->GetOpposingEdgeId(e, opp_tile);
      uint8_t flow = 0;
      Cost ec = cost->EdgeCost(de, e, tile, baldr::TimeInfo::invalid(), flow);
      double frac = 1.0;
      if (k == 0)
        frac *= first_frac;
      if (k + 1 == seq.size())
        frac *= last_frac;
      ec = Cost{static_cast<float>(ec.cost * frac), static_cast<float>(ec.secs * frac)};
      Cost tc{};
      uint8_t ridx = kInvalidRestriction;
      sif::InternalTurn iturn = sif::InternalTurn::kNoTurn;
      if (pred) {
        // the junction we leave, on THIS edge's level: the end node of its opposite
        if (!opp.is_valid() || !opp_tile) {
          ++rev_fail_noopp;
          return static_cast<uint32_t>(k);
        }
        const GraphId sn = opp_tile->directededge(opp)->endnode();
        if (!reader->GetGraphTile(sn, ntile)) {
          ++rev_fail_tile;
          return static_cast<uint32_t>(k);
        }
        // connected?  (a folded twin junction is two physical nodes 30 m apart)
        if (!pairs->same_physical_node(pred->endnode(), sn)) {
          ++rev_fail_gap;
          if (rev_fail_gap <= 3) {
            graph_tile_ptr ta, tb;
            const GraphId pe_ = pred->endnode();
            const PointLL la = reader->GetGraphTile(pe_, ta) ? ta->get_node_ll(pe_) : PointLL{};
            const PointLL lb = reader->GetGraphTile(sn, tb) ? tb->get_node_ll(sn) : PointLL{};
            LOG_INFO("roundtrip pair-repair: gap at edge " + std::to_string(k) + "/" +
                     std::to_string(seq.size()) + ": " + std::to_string(pred->edgeid().value) +
                     " ends at " + std::to_string(pe_.value) + " (" + std::to_string(la.lat()) +
                     "," + std::to_string(la.lng()) + "), " + std::to_string(e.value) +
                     " starts at " + std::to_string(sn.value) + " (" + std::to_string(lb.lat()) +
                     "," + std::to_string(lb.lng()) + ") " +
                     std::to_string(static_cast<int>(la.Distance(lb))) + " m apart");
          }
          return static_cast<uint32_t>(k);
        }
        const NodeInfo* ni = ntile->node(sn);
        uint8_t mask = pred->destonly_access_restr_mask();
        if (!cost->Allowed(de, false, *pred, tile, e, 0, 0, ridx, mask)) {
          ++rev_fail_turn;
          if (rev_fail_turn <= 3)
            LOG_INFO("roundtrip pair-repair: turn refused into " + std::to_string(e.value) +
                     " (level " + std::to_string(e.level()) + ") from " +
                     std::to_string(pred->edgeid().value) + " at edge " + std::to_string(k) +
                     "/" + std::to_string(seq.size()) + " (pred opp_local_idx " +
                     std::to_string(pred->opp_local_idx()) + " edge localidx " +
                     std::to_string(de->localedgeidx()) + " restrictions " +
                     std::to_string(pred->restrictions()) + " deadend " +
                     std::to_string(pred->deadend()) + " fwdaccess " +
                     std::to_string(de->forwardaccess()) + " revaccess " +
                     std::to_string(de->reverseaccess()) + " use " +
                     std::to_string(static_cast<int>(de->use())) + " class " +
                     std::to_string(static_cast<int>(de->classification())) + " surface " +
                     std::to_string(static_cast<int>(de->surface())) + " destonly " +
                     std::to_string(de->destonly()) + " shortcut " +
                     std::to_string(de->is_shortcut()) + " accessible " +
                     std::to_string(cost->IsAccessible(de)) + ")");
          return static_cast<uint32_t>(k);
        }
        iturn = cost->TurnType(pred->opp_local_idx(), ni, de);
        tc = cost->TransitionCost(de, ni, *pred, ntile, reader_getter);
      }
      acc += ec + tc;
      pd += de->length() * frac;
      out.emplace_back(mode, acc, e, 0, static_cast<float>(pd), ridx, tc);
      pred.emplace(kInvalidLabel, e, opp, de, acc, mode, tc, static_cast<uint32_t>(pd), false,
                   true, static_cast<bool>(flow & kDefaultFlowMask), iturn, ridx, 0,
                   de->destonly() || (cost->is_hgv() && de->destonly_hgv()),
                   de->forwardaccess() & kTruckAccess, 0);
    }
    return kInvalidLabel;
  };

  // proto/v4-p2 ledger
  uint32_t pair_loops_built = 0, pair_loops_reversible = 0, pair_loops_bridged = 0,
           pair_bridge_runs = 0, pair_loops_full_repair = 0, pair_repair_failed = 0,
           pair_fwd_recost = 0, pair_fwd_recost_fail = 0;

  // proto/v4-p2: build the loop a junction's PAIR describes.  Forward leg = the tree
  // path; return leg = the second path ridden backwards, which is exact where every
  // edge has a legal opposite and every turn is allowed the other way, and REPAIRED
  // where it does not: each non-reversible stretch (a one-way, a turn the costing
  // forbids) is bridged by a local hard-excluded A* between the junctions either side
  // of it, with the rest of the loop and its twins barred from that search.  Only when
  // bridging fails does the whole return fall back to P1.1's route_leg.
  auto attempt_pair_build = [&](uint32_t ci) -> std::optional<Loop> {
    pair_cheap_reject = false;
    PairEval& pe = eval_pair(ci);
    if (!pe.ok) {
      pair_cheap_reject = true;
      return std::nullopt;
    }
    if (pair_shares(pe.keys, pair_built_keys)) {
      ++pair_share_rejects;
      return std::nullopt;
    }
    if (leg_shares(pe.keys, pair_built_keys)) {
      ++pair_leg_share_rejects; // proto/v4-p2.1
      return std::nullopt;
    }
    const auto& labels = expander.labels();
    // the start's partial edge: the fraction of it actually ridden as origin / destination
    auto edge_frac = [&](const GraphId& e, bool as_origin) -> double {
      for (const auto& pedge : start.correlation().edges())
        if (GraphId(pedge.graph_id()) == e)
          return as_origin ? 1.0 - pedge.percent_along() : pedge.percent_along();
      return 1.0;
    };
    // ORIENTATION.  Either path may be the forward leg (every arc needs a settled
    // forward edge) with the other ridden home backwards (every arc needs a rideable
    // opposite).  The tree path forward is the default; the pair is swapped when only
    // the swap can be ridden forward, or when it leaves fewer return stretches to
    // repair.
    auto fwd_ok = [&](const std::vector<uint32_t>& arcs) {
      for (uint32_t h : arcs)
        if (pairs->arc(h).fwd_label == RoundTripPairPass::kNone)
          return false;
      return true;
    };
    auto ret_missing = [&](const std::vector<uint32_t>& arcs) {
      uint32_t n = 0;
      for (uint32_t h : arcs)
        n += pairs->arc(h).ret_edge == 0 ? 1 : 0;
      return n;
    };
    const std::vector<uint32_t>* fwd_arcs = &pe.pair.tree;
    const std::vector<uint32_t>* ret_arcs = &pe.pair.other;
    {
      const bool dflt = fwd_ok(pe.pair.tree), swap = fwd_ok(pe.pair.other);
      if (!dflt && !swap) {
        ++pair_fwd_illegal;
        pair_cheap_reject = true;
        return std::nullopt;
      }
      const uint32_t miss_dflt = dflt ? ret_missing(pe.pair.other) : 0xffffffffu;
      const uint32_t miss_swap = swap ? ret_missing(pe.pair.tree) : 0xffffffffu;
      if (!dflt || miss_swap < miss_dflt) {
        fwd_arcs = &pe.pair.other;
        ret_arcs = &pe.pair.tree;
        ++pair_swapped;
      }
    }
    // FORWARD LEG: the forward edges of its arcs, costed edge by edge.
    std::vector<PathInfo> fwd;
    {
      std::vector<GraphId> seq;
      seq.reserve(fwd_arcs->size());
      for (uint32_t h : *fwd_arcs)
        seq.push_back(labels[pairs->arc(h).fwd_label].edgeid());
      if (cost_path(seq, edge_frac(seq.front(), true), 1.0, fwd) != kInvalidLabel) {
        ++pair_fwd_recost_fail;
        pair_cheap_reject = true;
        return std::nullopt;
      }
      ++pair_fwd_recost;
    }
    if (fwd.empty())
      return std::nullopt;
    const GraphId arrival = fwd.back().edgeid;
    graph_tile_ptr arrival_tile = reader->GetGraphTile(arrival);
    if (!arrival_tile)
      return std::nullopt;
    valhalla::Location turn =
        correlate_node(arrival_tile->directededge(arrival)->endnode(), *reader, arrival);
    uint32_t outbound = 0;
    for (const auto& pedge : turn.correlation().edges())
      outbound += pedge.begin_node() ? 1 : 0;
    if (outbound == 0)
      return std::nullopt;

    // RETURN LEG = the other path, ridden backwards: the arcs' return edges in reverse.
    const size_t m = ret_arcs->size();
    struct RItem {
      GraphId id; // the edge the return rides (invalid when the arc has none)
      float len;
      bool ok;
      int32_t harc; // provenance: the H arc, -1 for a bridge edge
    };
    std::vector<RItem> items;
    items.reserve(m);
    std::vector<uint8_t> rev_ok_count(m, 1);
    for (size_t i = 0; i < m; ++i) {
      const uint32_t h = (*ret_arcs)[m - 1 - i];
      const auto& a = pairs->arc(h);
      bool ok = a.ret_edge != 0;
      if (ok) {
        graph_tile_ptr ct = reader->GetGraphTile(GraphId(a.ret_edge));
        const DirectedEdge* cde = ct ? ct->directededge(GraphId(a.ret_edge)) : nullptr;
        if (!cde || !(cde->forwardaccess() & cost->access_mode())) {
          ++rev_bad_ret_edge;
          if (rev_bad_ret_edge <= 3)
            LOG_INFO("roundtrip pair-repair: RET EDGE NOT RIDEABLE: " +
                     std::to_string(a.ret_edge) + " tree=" + std::to_string(a.tree) +
                     " fwd_label_edge=" +
                     std::to_string(a.fwd_label == RoundTripPairPass::kNone
                                        ? 0
                                        : labels[a.fwd_label].edgeid().value) +
                     " access=" + std::to_string(cde ? cde->forwardaccess() : 0));
          ok = false;
        }
      }
      if (!ok) {
        ++rev_fail_access;
        if (rev_fail_access <= 4)
          LOG_INFO("roundtrip pair-repair: arc without a return edge: tree=" +
                   std::to_string(a.tree) + " fwd_label=" +
                   std::to_string(a.fwd_label == RoundTripPairPass::kNone
                                      ? 0
                                      : labels[a.fwd_label].edgeid().value) +
                   " from=" + std::to_string(a.from) + " to=" + std::to_string(a.to) +
                   " len=" + std::to_string(static_cast<int>(a.len)));
      }
      items.push_back({GraphId(a.ret_edge), a.len, ok, static_cast<int32_t>(h)});
      rev_ok_count[i] = ok ? 1 : 0;
    }
    // the node an edge starts at (the end node of its opposite) / ends at
    auto edge_start_node = [&](const GraphId& e) -> GraphId {
      graph_tile_ptr t;
      const GraphId opp = reader->GetOpposingEdgeId(e, t);
      return (opp.is_valid() && t) ? t->directededge(opp)->endnode() : GraphId{};
    };
    auto edge_end_node = [&](const GraphId& e) -> GraphId {
      graph_tile_ptr t = reader->GetGraphTile(e);
      return t ? t->directededge(e)->endnode() : GraphId{};
    };
    uint8_t rung = 0, bridges = 0;
    bool full = false;
    std::vector<PathInfo> ret;
    for (int iter = 0; iter < 24; ++iter) {
      uint32_t bad = kInvalidLabel;
      for (size_t i = 0; i < items.size(); ++i)
        if (!items[i].ok) {
          bad = static_cast<uint32_t>(i);
          break;
        }
      if (bad == kInvalidLabel) {
        std::vector<GraphId> ids;
        ids.reserve(items.size());
        for (const auto& it : items)
          ids.push_back(it.id);
        bad = cost_path(ids, 1.0, edge_frac(ids.back(), false), ret);
        if (bad == kInvalidLabel)
          break; // the return costs out — done
        items[bad].ok = false; // a turn the costing forbids, or a gap between carriageways
      }
      size_t a = bad, b = bad;
      while (b + 1 < items.size() && !items[b + 1].ok)
        ++b;
      if (!roundtrip_pair_bridge || bridges >= roundtrip_pair_max_bridges) {
        full = true;
        break;
      }
      // BRIDGE a..b: from the node the previous item reaches to the node the next item
      // leaves, the rest of the loop barred.
      const bool from_turn = a == 0;
      const bool to_start = b + 1 == items.size();
      const GraphId from_node = from_turn ? GraphId{} : edge_end_node(items[a - 1].id);
      const GraphId to_node = to_start ? GraphId{} : edge_start_node(items[b + 1].id);
      if ((!from_turn && !from_node.is_valid()) || (!to_start && !to_node.is_valid())) {
        full = true;
        break;
      }
      valhalla::Location from_loc =
          from_turn ? turn : correlate_node(from_node, *reader, items[a - 1].id);
      valhalla::Location to_loc =
          to_start ? start : correlate_node(to_node, *reader, GraphId{}, items[b + 1].id);
      uint32_t from_out = 0;
      for (const auto& pedge : from_loc.correlation().edges())
        from_out += pedge.begin_node() ? 1 : 0;
      if (from_out == 0 || to_loc.correlation().edges().empty()) {
        full = true;
        break;
      }
      // the synthetic corridor route_leg bars and leashes: the reversible return items
      // outside the stretch, carrying "metres to the ride end" as their path distance
      // (so the exemption test reads the return the way it reads the forward leg),
      // then the forward leg (whose last entry is the total route_leg grades against).
      std::vector<PathInfo> corridor;
      {
        double total = 0.0;
        for (const auto& it : items)
          total += it.len;
        double cum = 0.0;
        for (size_t i = 0; i < items.size(); ++i) {
          cum += items[i].len;
          if ((i >= a && i <= b) || !items[i].ok)
            continue;
          corridor.emplace_back(mode, Cost{}, items[i].id, 0,
                                static_cast<float>(std::max(0.0, total - cum)),
                                kInvalidRestriction, Cost{});
        }
      }
      corridor.insert(corridor.end(), fwd.begin(), fwd.end());
      uint8_t brung = 0;
      std::vector<PathInfo> bridge;
      const auto t_b = ledger_clock::now();
      try {
        bridge = route_leg(corridor, from_loc, to_loc, brung);
      } catch (const std::exception&) {
        bridge.clear();
      }
      bridge_ms += ms_since(t_b);
      if (bridge.empty()) {
        full = true;
        break;
      }
      ++bridges;
      rung = std::max(rung, brung);
      std::vector<RItem> next(items.begin(), items.begin() + a);
      float prev_pd = 0.f;
      for (const auto& pi : bridge) {
        next.push_back({pi.edgeid, pi.path_distance - prev_pd, true, -1});
        prev_pd = pi.path_distance;
      }
      next.insert(next.end(), items.begin() + b + 1, items.end());
      items.swap(next);
      ret.clear();
    }
    if (ret.empty() && !full)
      full = true; // the iteration cap: repair the whole return instead
    if (full) {
      ret.clear();
      rung = 0;
      try {
        ret = route_leg(fwd, turn, start, rung);
      } catch (const std::exception& e) {
        if (interrupt)
          (*interrupt)();
        ret.clear();
      }
      if (ret.empty()) {
        ++pair_repair_failed;
        return std::nullopt;
      }
    }
    Loop L;
    L.fwd = std::move(fwd);
    L.ret = std::move(ret);
    L.turn = std::move(turn);
    L.curviness = cands[ci].curviness_per_km;
    L.rung = rung;
    L.cand = ci;
    L.pair_built = true;
    L.pair_bridges = bridges;
    L.pair_full_repair = full;
    L.pair_cost = pe.pair.pair_cost;
    L.pair_surplus = pe.pair.reduced_cost;
    L.pair_fwd_len = pe.pair.tree_len;
    L.pair_ret_len = pe.pair.other_len;
    ++pair_loops_built;
    {
      uint32_t nbad = 0;
      for (size_t i = 0; i < m; ++i)
        nbad += rev_ok_count[i] ? 0 : 1;
      LOG_INFO("roundtrip pair-loop: cand=" + std::to_string(ci) + " ret_arcs=" +
               std::to_string(m) + " no_return_edge=" + std::to_string(nbad) +
               " bridges=" + std::to_string(bridges) + " full=" + std::to_string(full) +
               " fwd_m=" + std::to_string(static_cast<int>(pe.pair.tree_len)) + " ret_m=" +
               std::to_string(static_cast<int>(pe.pair.other_len)) + " surplus=" +
               std::to_string(static_cast<int>(pe.pair.reduced_cost)));
    }
    if (full)
      ++pair_loops_full_repair;
    else if (bridges > 0) {
      ++pair_loops_bridged;
      pair_bridge_runs += bridges;
    } else
      ++pair_loops_reversible;
    return finish_build(std::move(L));
  };
  auto build_candidate = [&](uint32_t ci) -> std::optional<Loop> {
    return pairs ? attempt_pair_build(ci) : attempt_build(ci);
  };

  // Distance Flex, lazy (ADR-0037 / #54 candidate 6): the widened re-scan of the same
  // expansion tree runs only when the primary path stalls — queue dry OR attempt budget
  // dry with loops still missing — and grants a fresh +want budget so the flex pool can
  // actually fill hard cells. For every cell the primary queue fills, the re-scan never
  // runs and the loop set is byte-identical to the eager version's (same append order).
  uint32_t attempts = 0;
  uint32_t attempt_cap = want + kAttemptSlack;
  size_t qi = 0;
  // proto/v4-p1.1 F09 (item 6): the stall branch granted the fresh +want budget only
  // when IT was the one calling widen_pool().  The distance correction also widens
  // (`widened = true`), so a cell whose correction fired first reached its stall with
  // `widened` already set, was granted nothing, and broke at want + kAttemptSlack — the
  // hard cells the budget exists to protect.  The grant now belongs to the stall, once,
  // and never shrinks the cap.
  bool stall_granted = false;
  const char* underfill_cause = "none";
  // proto/v4-p2: the build loop runs twice in pair mode — once on pairs, and, when the
  // bank is still short after the pair pass has said its piece, once more with P1.1's
  // builder over the same queue (the RESCUE pass): a cell with no disjoint pair at all
  // (vlasina: one road in and out) is then served exactly as P1.1 serves it — a soft-
  // leash Fallback Loop, gated and ranked last — never a 442 where P1.1 has a loop.
  uint32_t pair_rescue_attempts = 0, pair_rescue_loops = 0;
  bool rescue_pass = false;
  auto build_loop = [&]() {
  while (true) {
    if (loops.size() >= want)
      break;
    if (pairs && !rescue_pass && rung_budget_spent()) {
      underfill_cause = "evalcap"; // proto/v4-p2.1: this rung's evaluation budget is spent
      break;
    }
    if (qi >= queue.size() || attempts >= attempt_cap) {
      if (roundtrip_f09_budget ? !stall_granted : !widened) {
        stall_granted = true;
        attempt_cap = std::max(attempt_cap, attempts + want);
        widen_pool(); // idempotent
      }
    }
    if (qi >= queue.size() || attempts >= attempt_cap) {
      underfill_cause = attempts >= attempt_cap ? "cap" : "queue";
      if (attempts >= attempt_cap)
        LOG_INFO("roundtrip: attempt cap (" + std::to_string(attempt_cap) + ") hit with " +
                 std::to_string(loops.size()) + " loop(s) built");
      break;
    }
    const uint32_t ci = queue[qi++];
    if (!built_separated(cands[ci].ll))
      continue;
    if (pairs && !rescue_pass) {
      // proto/v4-p2: a selection-time reject (no pair, off band, twin ride, near-dup)
      // costs a path walk, not a search, so it does not spend the attempt budget.
      const PairEval& pe = eval_pair(ci);
      if (!pe.ok)
        continue;
      if (pair_shares(pe.keys, pair_built_keys)) {
        ++pair_share_rejects;
        continue;
      }
      if (leg_shares(pe.keys, pair_built_keys)) {
        ++pair_leg_share_rejects; // proto/v4-p2.1: tested against the loops actually served
        continue;
      }
    }
    ++attempts;
    if (rescue_pass)
      ++pair_rescue_attempts;
    auto built = rescue_pass ? attempt_build(ci) : build_candidate(ci);
    if (!built) {
      if (pairs && !rescue_pass && pair_cheap_reject)
        --attempts; // died before any search: not a spent build
      continue;
    }
    uint32_t serve_ci = ci;

    // ADR-0037 §2 distance correction: ONE corrective re-aim when the build lands
    // outside tolerance. The compensated turnaround distance scales the built leg by
    // target/actual; the re-aim candidate must sit in the original's bearing sector
    // and — the node guard — collide with no already-built turnaround node, so K
    // candidates stay distinct instead of converging on the one ideal node.
    const double actual_m = static_cast<double>(built->fwd.back().path_distance) +
                            static_cast<double>(built->ret.back().path_distance);
    const double dist_err = built->dist_err;
    // Fallback Loops fire only on extreme misses — see kDistCorrFallbackThr.
    // (proto/v4-p1: the two-lobe exemption went with Second Via.)
    const double fire_thr = built->fallback ? kDistCorrFallbackThr : kDistCorrTolerance;
    if (dist_err > fire_thr && correction_count < kMaxCorrectionsPerRequest) {
      widen_pool();
      const double comp_pd = std::clamp(cands[ci].path_distance * target / actual_m,
                                        target * 0.5 * kFlexLoFrac,
                                        target * 0.5 * kFlexHiFrac);
      uint32_t best = kInvalidLabel;
      double best_gap = std::numeric_limits<double>::max();
      for (uint32_t i = 0; i < static_cast<uint32_t>(cands.size()); ++i) {
        if (cands[i].node == cands[ci].node || built_nodes.count(cands[i].node))
          continue;
        if (!built_separated(cands[i].ll))
          continue;
        const float sep = std::fabs(
            std::fmod(cands[i].bearing_deg - cands[ci].bearing_deg + 540.0f, 360.0f) - 180.0f);
        if (sep > kCorrSectorDeg / 2.0f)
          continue;
        const double gap = std::fabs(static_cast<double>(cands[i].path_distance) - comp_pd);
        if (gap < best_gap) {
          best_gap = gap;
          best = i;
        }
      }
      if (best != kInvalidLabel) {
        ++correction_count;
        auto corrected = build_candidate(best);
        if (corrected) {
          const double corr_m = static_cast<double>(corrected->fwd.back().path_distance) +
                                static_cast<double>(corrected->ret.back().path_distance);
          if (std::fabs(corr_m - target) / target < dist_err) {
            built = std::move(corrected);
            serve_ci = best;
          }
        }
      }
      // Correction failed or was worse: the original loop stands (one shot, no loop).
    }

    {
      std::vector<uint64_t> sig;
      sig.reserve(built->fwd.size() + built->ret.size());
      for (const auto& pi : built->fwd)
        sig.push_back(pi.edgeid.value);
      for (const auto& pi : built->ret)
        sig.push_back(pi.edgeid.value);
      if (!served_sigs.insert(std::move(sig)).second)
        continue; // identical to an already-built loop — refill from the queue
    }
    // Item 4 near-dup sharing filter (wayfinder #46): reject a loop that retreads more
    // than kSharingRejectFrac of its fresh-road length onto an already-kept loop, and
    // refill the slot from the queue — the byte-identical dedup above generalized from
    // "identical" to "near-identical". Where the network offers fewer than K distinct
    // corridors the bank honestly under-fills rather than serving twins.
    if (roundtrip_sharing_filter) {
      std::unordered_map<uint64_t, double> kl, kl_twins;
      const double total = loop_keylen(*built, kl, kl_twins);
      bool near_dup = false;
      if (total > 0.0) {
        for (const auto& prev : built_keylens) {
          double shared = 0.0;
          for (const auto& [k, len] : kl)
            if (prev.count(k))
              shared += len;
          if (shared / total > roundtrip_sharing_frac) {
            near_dup = true;
            break;
          }
        }
      }
      if (near_dup) {
        ++sharing_filter_rejects;
        continue; // refill from the queue
      }
      // the kept loop's roads AND their twins are what later loops are tested against
      for (const auto& [t, len] : kl_twins)
        kl.emplace(t, len);
      built_keylens.push_back(std::move(kl));
    }
    built_lls.push_back(cands[serve_ci].ll);
    built_nodes.insert(cands[serve_ci].node);
    // Register this loop's fresh-road edges in the cross-candidate memory (wayfinder
    // #46), each edge counted once per loop so the count grades by distinct prior loops.
    // Forward distance runs from the start; return distance is rebased to the ride end
    // (= start), so both exemption tests mean "outside the shared start disk" and the
    // network-forced start stem is never remembered.
    if (roundtrip_xcand_penalty) {
      const double ret_total = static_cast<double>(built->ret.back().path_distance);
      std::unordered_set<uint64_t> loop_edges;
      std::vector<uint64_t> tws;
      // proto/v4-p1: the cross-candidate surcharge set keys on TWINS too — a later
      // candidate that rides the opposite carriageway of a committed loop is riding
      // the same physical road and must pay the same surcharge.
      auto note = [&](const GraphId& e) {
        loop_edges.insert(e.value);
        const GraphId opp = reader->GetOpposingEdgeId(e);
        if (opp.is_valid())
          loop_edges.insert(opp.value);
        if (!twin_index)
          return;
        graph_tile_ptr tile = reader->GetGraphTile(e);
        if (!tile)
          return;
        tws.clear();
        twin_index->append_twins(RoadTwinIndex::canonical_id(tile->directededge(e), e, opp),
                                 tws);
        for (uint64_t cv : tws) {
          const GraphId t(cv);
          loop_edges.insert(t.value);
          const GraphId topp = reader->GetOpposingEdgeId(t);
          if (topp.is_valid())
            loop_edges.insert(topp.value);
        }
      };
      for (const auto& pi : built->fwd)
        if (static_cast<double>(pi.path_distance) > kStartExemptionMeters)
          note(pi.edgeid);
      for (const auto& pi : built->ret)
        if (ret_total - static_cast<double>(pi.path_distance) > kStartExemptionMeters)
          note(pi.edgeid);
      for (uint64_t ev : loop_edges)
        ++bank_edge_count[ev];
    }
    if (pairs && roundtrip_pair_built_keys) {
      // proto/v4-p2.1: key the bank on the BUILT loop — both legs as served (92 % of P2's
      // returns are repairs the pair never described) — for every loop, rescue-built too,
      // so later candidates are tested against the returns the rider actually gets.
      PairKeys bk;
      std::unordered_map<uint64_t, double> kl_twins;
      bk.total = loop_keylen(*built, bk.ridden, kl_twins);
      for (const auto& [t, len] : kl_twins)
        bk.twins.insert(t);
      pair_built_keys.push_back(std::move(bk));
    } else if (pairs && built->cand != baldr::kInvalidLabel) {
      pair_built_keys.push_back(pair_cache[built->cand].keys);
    }
    built->relaxed = leg_relax_rung; // proto/v4-p2.1
    if (leg_relax_rung) {
      ++pair_leg_relaxed;
      pair_leg_relax_rung_used = std::max(pair_leg_relax_rung_used, leg_relax_rung);
    }
    if (rescue_pass)
      ++pair_rescue_loops;
    loops.push_back(std::move(*built));
  }
  };
  build_loop();
  // proto/v4-p2.1: the RELAXATION LADDER for the fills bar.  A strict per-leg threshold
  // leaves the bank short where the network offers fewer forward-distinct corridors than
  // K; before the twin last resort and the rescue pass, the queue is re-walked at
  // frac + 0.15, then + 0.30, then with the leg test off — each rung with its own
  // evaluation budget (cached verdicts are free, so a re-walk only pays for candidates the
  // previous rung never reached), stopping as soon as the bank is full.  The whole-pair
  // filter stays on at every rung (rung 3 IS P2's selection contract, within a budget).
  // Loops admitted here carry their rung (Loop.relaxed); builds spend attempts like any
  // other, and the ladder is granted the missing count plus slack once, as the rescue
  // pass is granted want plus slack.
  if (pairs && leg_mode && roundtrip_pair_leg_relax && loops.size() < want) {
    attempt_cap = std::max(attempt_cap,
                           attempts + (want - static_cast<uint32_t>(loops.size())) + kAttemptSlack);
    for (uint8_t rung = 1; rung <= 3 && loops.size() < want; ++rung) {
      leg_relax_rung = rung;
      rung_evals = 0;
      leg_test_off = rung == 3;
      leg_frac_active = roundtrip_pair_leg_sharing_frac + 0.15 * rung;
      qi = 0;
      build_loop();
    }
    leg_relax_rung = 0; // the twin last resort and the rescue pass are not relaxed loops
  }
  uint32_t pair_twin_last_resort = 0;
  if (rt_debug)
    std::cerr << "[rt-debug] after main loop: loops=" << loops.size()
              << " dirty=" << dirty_loops.size() << " twin_candidates=" << pair_twin_candidates.size()
              << " considered=" << pair_sinks_considered << " no_pair=" << pair_none
              << " band=" << pair_band_rejects << " twin=" << pair_twin_rejects
              << " share=" << pair_share_rejects << " fwd_illegal=" << pair_fwd_illegal
              << " queue=" << queue.size() << " cands=" << cands.size() << " attempts=" << attempts
              << " underfill=" << underfill_cause << " leg_share=" << pair_leg_share_rejects
              << " leg_relaxed=" << pair_leg_relaxed << " rung=" << int(pair_leg_relax_rung_used)
              << "\n";
  if (pairs && loops.size() < want && !pair_twin_candidates.empty()) {
    // proto/v4-p2 dirty-last: the twin-rejected pairs, built now that nothing clean is
    // left, so the geometry gate can stash them for the per-slot last resort below.
    pair_allow_twin = true;
    for (uint32_t ci : pair_twin_candidates) {
      if (loops.size() + dirty_loops.size() >= want)
        break;
      auto& pe = pair_cache[ci];
      pe.checked = false; // re-evaluate with the twin verdict lifted
      ++pair_twin_last_resort;
      auto built = attempt_pair_build(ci);
      if (rt_debug)
        std::cerr << "[rt-debug] twin last resort cand=" << ci << " built=" << (built ? 1 : 0)
                  << " reject=" << pair_cache[ci].reject << " ok=" << pair_cache[ci].ok
                  << " dirty=" << dirty_loops.size() << " fwd_recost_fail=" << pair_fwd_recost_fail
                  << " repair_failed=" << pair_repair_failed << " rev_fail_turn=" << rev_fail_turn
                  << " noret=" << rev_fail_access << " gap=" << rev_fail_gap << "\n";
      if (built) // the gate has judged the BUILT loop (a repaired return may be clean)
        loops.push_back(std::move(*built));
    }
    pair_allow_twin = false;
  }
  if (pairs && loops.size() < want) {
    rescue_pass = true;
    qi = 0;
    attempt_cap = std::max(attempt_cap, attempts + want + kAttemptSlack);
    stall_granted = false; // the rescue gets P1.1's own stall grant (F09) once more
    build_loop();
    if (rt_debug)
      std::cerr << "[rt-debug] rescue pass: attempts=" << pair_rescue_attempts << " loops="
                << pair_rescue_loops << " bank=" << loops.size() << "\n";
  }
  if (fallback_count > 0)
    LOG_INFO("roundtrip: " + std::to_string(fallback_count) +
             " candidate(s) fell back to the soft leash (Fallback Loop)");
  // proto/v4-p1 road-identity ledger.
  LOG_INFO("roundtrip identity: twins_excluded=" + std::to_string(twins_excluded) +
           " parallels_leashed=" + std::to_string(parallels_leashed) + " over " +
           std::to_string(identity_legs) + " leg(s); f01_chain_rejects=" +
           std::to_string(expander.nonsimple_rejected()) +
           (twin_index ? (parallel_tier ? " [twins+parallels]" : " [twins only]")
                       : " [identity off]"));
  if (correction_count > 0)
    LOG_INFO("roundtrip: distance correction re-aimed " + std::to_string(correction_count) +
             " off-target build(s)");
  if (gate_seam + gate_twinride + gate_bouncehits > 0)
    LOG_INFO("roundtrip gate: rejected seam=" + std::to_string(gate_seam) +
             " twin_ride=" + std::to_string(gate_twinride) +
             " return_bounce=" + std::to_string(gate_bouncehits) + "; refilled " +
             std::to_string(gate_refills) + "/" + std::to_string(gate_refill_budget) +
             ", kept-in-bank " + std::to_string(gate_kept) + ", stashed " +
             std::to_string(dirty_loops.size()) +
             // §16: appended, never inserted — the sweep's ledger reader parses the
             // prefix above and must keep working on both binaries.
             "; seam_refills " + std::to_string(gate_seam_refills) +
             " (unbudgeted), refill_denied_by_budget twin_ride=" +
             std::to_string(gate_denied_twinride) +
             " return_bounce=" + std::to_string(gate_denied_bounce));
  if (pairs)
    LOG_INFO(
        "roundtrip pair-select: considered=" + std::to_string(pair_sinks_considered) +
        " no_pair=" + std::to_string(pair_none) + " band=" + std::to_string(pair_band_rejects) +
        " twin=" + std::to_string(pair_twin_rejects) +
        " share=" + std::to_string(pair_share_rejects) + " chosen=" + std::to_string(chosen.size()) +
        "; built=" + std::to_string(pair_loops_built) + " reversible=" +
        std::to_string(pair_loops_reversible) + " bridged=" + std::to_string(pair_loops_bridged) +
        " (runs=" + std::to_string(pair_bridge_runs) +
        ") full_repair=" + std::to_string(pair_loops_full_repair) + " repair_failed=" +
        std::to_string(pair_repair_failed) + " fwd_recost=" + std::to_string(pair_fwd_recost) + "/" +
        std::to_string(pair_fwd_recost_fail) +
        " twin_last_resort=" + std::to_string(pair_twin_last_resort) +
        " rescue=" + std::to_string(pair_rescue_loops) + "/" + std::to_string(pair_rescue_attempts) +
        " built_share_rejects=" + std::to_string(sharing_filter_rejects) +
        " gate_fires=" + std::to_string(gate_seam + gate_twinride + gate_bouncehits) +
        " pair_ms=" + std::to_string(static_cast<int>(pair_pass_ms)) +
        " eval_ms=" + std::to_string(static_cast<int>(pair_eval_ms)) +
        " bridge_ms=" + std::to_string(static_cast<int>(bridge_ms)) +
        " rev_fail(tile/noopp/noret/turn/gap)=" + std::to_string(rev_fail_tile) + "/" +
        std::to_string(rev_fail_noopp) + "/" + std::to_string(rev_fail_access) + "/" +
        std::to_string(rev_fail_turn) + "/" + std::to_string(rev_fail_gap) + " fwd_illegal=" +
        std::to_string(pair_fwd_illegal) + " swapped=" + std::to_string(pair_swapped) +
        " bad_ret_edge=" + std::to_string(rev_bad_ret_edge) +
        // proto/v4-p2.1: APPENDED, never inserted — the P2 parsers read the prefix.
        " leg_share=" + std::to_string(pair_leg_share_rejects) +
        " leg_relaxed=" + std::to_string(pair_leg_relaxed) +
        " leg_relax_rung=" + std::to_string(pair_leg_relax_rung_used) +
        " div_w=" + std::to_string(roundtrip_pair_diversity_w) +
        " built_keys=" + (roundtrip_pair_built_keys ? "1" : "0") + " rung_evals=" +
        std::to_string(rung_eval_count[0]) + "/" + std::to_string(rung_eval_count[1]) + "/" +
        std::to_string(rung_eval_count[2]) + "/" + std::to_string(rung_eval_count[3]) +
        // the coarse memory guard: evaluations cached, entries still holding keys
        " cache=" + std::to_string(pair_cache.size()) + " keys_held=" +
        std::to_string(std::count_if(pair_cache.begin(), pair_cache.end(),
                                     [](const auto& kv) { return !kv.second.keys.ridden.empty(); })));
  LOG_INFO("roundtrip rungs: r0=" + std::to_string(rung_hits[0]) +
           " r1=" + std::to_string(rung_hits[1]) + " r2=" + std::to_string(rung_hits[2]) +
           " none=" + std::to_string(rung_hits[3]) +
           (parallel_rung_tried
                ? " parallel_rung=" + std::to_string(parallel_rung_converted) + "/" +
                      std::to_string(parallel_rung_tried)
                : std::string()));
  if (loops.size() < want && !dirty_loops.empty()) {
    // Dirty-last-resort, per SLOT rather than per request (proto/v4-p1.1).  ADR-0037
    // promoted the stash only when the bank was completely empty, so a cell that built
    // three clean loops and stashed nine served three; with the geometry gate rejecting
    // more, that is how the first P1.1 corpus returned 1-6 routes on nine requests.
    // The stash now tops the bank up, best-scored first, and the tier sort keeps every
    // topped-up loop behind every clean one.
    for (auto& d : dirty_loops) {
      score_built(d);
      d.gated = true;
    }
    std::stable_sort(dirty_loops.begin(), dirty_loops.end(),
                     [](const Loop& a, const Loop& b) { return a.score > b.score; });
    const size_t before = loops.size();
    // A SEAM-rejected loop is an exact-mirror stub — the harness's own spike meter, and
    // Gate v1.3's one absolute (`spike_ge_500m == 0`).  Topping a bank up with one trades
    // a fill for a spike, which is not a trade this gate is allowed to make: the first
    // per-slot top-up did exactly that and put spikes into 0.69 % of c0.5 loops.  So the
    // top-up draws only on loops the GEOMETRY gate rejected; seam rejects keep ADR-0037's
    // own rule — served only if the bank would otherwise be empty.
    for (auto& d : dirty_loops) {
      if (loops.size() >= want)
        break;
      if (d.seam_reject)
        continue;
      loops.push_back(std::move(d));
    }
    if (loops.empty())
      for (auto& d : dirty_loops)
        if (d.seam_reject && loops.size() < want)
          loops.push_back(std::move(d));
    LOG_INFO("roundtrip: topped the bank up with " + std::to_string(loops.size() - before) +
             " gated loop(s) as a last resort (had " + std::to_string(before) + ")");
  }
  if (loops.empty())
    throw valhalla_exception_t{442};

  // 4) proto/v4-p1 RANKING (F20, partial): hard-exclude successes rank ahead of
  //    Fallback Loops, then curviness-per-km, stable.  v3 sorted on curviness alone —
  //    the HARVEST CHAIN's score — which put a soft-leash return with a curvy forward
  //    leg ahead of a clean loop, and the census measured 25.6 % of served candidates
  //    as Fallbacks with the direct-serve slots 0-2 the worst on every axis the score
  //    can see.  Clean-first is the one axis P1 adds; ranking the BUILT loop (return
  //    leg, distance error, self-overlap) is P2's job.
  const bool built_rank = roundtrip_built_ranking;
  const bool relaxed_last = roundtrip_pair_relaxed_last;
  std::stable_sort(loops.begin(), loops.end(),
                   [built_rank, relaxed_last](const Loop& a, const Loop& b) {
                     if (!built_rank) {
                       if (a.fallback != b.fallback)
                         return !a.fallback;
                       return a.curviness > b.curviness;
                     }
                     // proto/v4-p1.1: rung/gate tier first (absolute), then the BUILT
                     // loop's score.
                     if (a.tier() != b.tier())
                       return a.tier() < b.tier();
                     // proto/v4-p2.1: within a tier, loops the relaxation ladder admitted
                     // rank after the ones that met the per-leg threshold.
                     if (relaxed_last && (a.relaxed != 0) != (b.relaxed != 0))
                       return a.relaxed == 0;
                     return a.score > b.score;
                   });

  {
    // proto/v4-p1: surface the served order so the ranking change is auditable in the
    // ledger the way the fallback/gate counts already are.  proto/v4-p1.1 adds the
    // score's three inputs, so a slot can be explained from the log alone:
    //   slot:rung/score*1000/loop-curviness*1000/self-overlap m/distance-error*100
    std::string order;
    for (size_t li = 0; li < loops.size(); ++li)
      order += (li ? " " : "") + std::to_string(li) + ":r" +
               std::to_string(loops[li].tier()) + "/" +
               std::to_string(static_cast<int>(loops[li].score * 1000.0)) + "/" +
               std::to_string(static_cast<int>(loops[li].loop_curviness * 1000.0f)) + "/" +
               std::to_string(static_cast<int>(loops[li].self_overlap_m)) + "/" +
               std::to_string(static_cast<int>(loops[li].dist_err * 100.0));
    LOG_INFO(std::string("roundtrip ranking: ") +
             (built_rank ? "rung tier then built-loop score" : "clean-first then curviness") +
             " — " + order);
    if (pairs) {
      // proto/v4-p2: slot:paircost/surplus/fwd-m/ret-m/bridges/full  (a P1.1-built
      // loop — the full repair — prints its pair cost too; `-` marks a non-pair loop)
      std::string pr;
      for (size_t li = 0; li < loops.size(); ++li) {
        const Loop& L = loops[li];
        pr += (li ? " " : "") + std::to_string(li) + ":";
        if (!L.pair_built) {
          pr += "-";
          continue;
        }
        pr += std::to_string(static_cast<int>(L.pair_cost)) + "/" +
              std::to_string(static_cast<int>(L.pair_surplus)) + "/" +
              std::to_string(static_cast<int>(L.pair_fwd_len)) + "/" +
              std::to_string(static_cast<int>(L.pair_ret_len)) + "/" +
              std::to_string(L.pair_bridges) + "/" + (L.pair_full_repair ? "1" : "0");
      }
      LOG_INFO("roundtrip pair-ranks: " + pr);
      // proto/v4-p2.1: slot:relax-rung/pair-built/tier, keyed by request (start, target,
      // seed, prefer_curvature, use_highways) so the per-slot read can be joined to the
      // response — three workers interleave the ledger.
      std::string pl;
      for (size_t li = 0; li < loops.size(); ++li)
        pl += (li ? " " : "") + std::to_string(li) + ":" + std::to_string(loops[li].relaxed) + "/" +
              (loops[li].pair_built ? "1" : "0") + "/" + std::to_string(loops[li].tier());
      const auto& co = options.costings().find(options.costing_type())->second.options();
      char key[96];
      std::snprintf(key, sizeof(key), "%.5f_%.5f_%d_%u_%.2f_%.2f", options.locations(0).ll().lat(),
                    options.locations(0).ll().lng(), static_cast<int>(target), seed,
                    co.prefer_curvature(), co.use_highways());
      LOG_INFO("roundtrip pair-leg: req=" + std::string(key) + " slots=" + pl);
      if (rt_debug) // gurka cannot read the ledger (the logger is a one-shot static)
        std::cerr << "[rt-debug] pair-leg: slots=" << pl << "\n";
    }
  }

  // 5) Serialize each loop as a 2-leg TripRoute (start -> turnaround -> start). Pass fresh
  //    Location copies per leg since TripLegBuilder mutates origin/destination.
  valhalla::Trip& trip = *request.mutable_trip();
  trip.mutable_routes()->Reserve(static_cast<int>(loops.size()));
  std::vector<std::string> algorithms;
  const auto t_build = ledger_clock::now();
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
  build_ms = ms_since(t_build);
  if (roundtrip_stage_timing) {
    auto ms = [](double v) { return std::to_string(static_cast<int>(v)); };
    LOG_INFO("roundtrip timing: harvest=" + ms(harvest_ms) + " scan=" + ms(scan_ms) +
             " walkback=" + ms(walkback_ms) + " rejoin=" + ms(rejoin_ms) +
             " astar=" + ms(astar_ms) + " astar_fb=" + ms(astar_fb_ms) +
             " seam=" + ms(seam_ms) + " gate=" + ms(gate_ms) + " build=" + ms(build_ms) +
             " f01_key=" + ms(expander.f01_key_ms()) + " f01_dfs=" + ms(expander.f01_dfs_ms()) +
             " f01_labels=" + std::to_string(expander.f01_labels_scanned()) +
             " (ms) attempts=" + std::to_string(attempts) +
             " corrections=" + std::to_string(correction_count) +
             " fallbacks=" + std::to_string(fallback_count) +
             " loops=" + std::to_string(loops.size()) +
             " dirty=" + std::to_string(dirty_loops.size()) + (widened ? " widened" : "") +
             " underfill=" + underfill_cause +
             (pairs ? " pairs=" + ms(pair_pass_ms) + " bridge=" + ms(bridge_ms) : std::string()));
  }
}
} // namespace thor
} // namespace valhalla
