#ifndef __VALHALLA_THOR_SERVICE_H__
#define __VALHALLA_THOR_SERVICE_H__

#include <valhalla/baldr/attributes_controller.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/exceptions.h>
#include <valhalla/meili/map_matcher_factory.h>
#include <valhalla/meili/match_result.h>
#include <valhalla/proto/options.pb.h>
#include <valhalla/proto/trip.pb.h>
#include <valhalla/sif/costfactory.h>
#include <valhalla/thor/bidirectional_astar.h>
#include <valhalla/thor/centroid.h>
#include <valhalla/thor/costmatrix.h>
#include <valhalla/thor/isochrone.h>
#include <valhalla/thor/multimodal_astar.h>
#include <valhalla/thor/multimodal_transit.h>
#include <valhalla/thor/timedistancebssmatrix.h>
#include <valhalla/thor/timedistancematrix.h>
#include <valhalla/thor/unidirectional_astar.h>
#include <valhalla/worker.h>

#include <boost/property_tree/ptree_fwd.hpp>

#include <tuple>
#include <vector>

namespace valhalla {
namespace thor {

#ifdef ENABLE_SERVICES
void run_service(const boost::property_tree::ptree& config);
#endif

class thor_worker_t : public service_worker_t {
public:
  enum SOURCE_TO_TARGET_ALGORITHM : uint8_t {
    SELECT_OPTIMAL = 0,
    COST_MATRIX = 1,
    TIME_DISTANCE_MATRIX = 2
  };
  thor_worker_t(const boost::property_tree::ptree& config,
                const std::shared_ptr<baldr::GraphReader>& graph_reader = {});
  virtual ~thor_worker_t();
#ifdef ENABLE_SERVICES
  virtual prime_server::worker_t::result_t work(const std::list<zmq::message_t>& job,
                                                void* request_info,
                                                const std::function<void()>& interrupt) override;
#endif
  virtual void cleanup() override;

  static void adjust_locations(valhalla::Api& options);

  void route(Api& request);
  // ADR-0033: native round-trip loop action (branched from route() on options.roundtrip).
  void roundtrip_impl(Api& request, const std::string& costing);
  std::string matrix(Api& request);
  void optimized_route(Api& request);
  std::string isochrones(Api& request);
  void trace_route(Api& request);
  std::string trace_attributes(Api& request);
  std::string expansion(Api& request);
  void centroid(Api& request);
  void status(Api& request) const;

  void set_interrupt(const std::function<void()>* interrupt) override;

protected:
  std::vector<std::vector<thor::PathInfo>> get_path(PathAlgorithm* path_algorithm,
                                                    Location& origin,
                                                    Location& destination,
                                                    const std::string& costing,
                                                    Api& request);
  void log_admin(const TripLeg&);
  thor::PathAlgorithm* get_path_algorithm(const std::string& routetype,
                                          const Location& origin,
                                          const Location& destination,
                                          Api& request);
  thor::MatrixAlgorithm*
  get_matrix_algorithm(Api& request, const bool has_time, const std::string& costing);
  void route_match(Api& request);
  /**
   * Returns the results of the map match where the first float is the normalized
   * match score (based on alternatives), the second is the raw score (the cost)
   * and the final is the list of match results (how the trace points were matched)
   * @param request   The request to map match (options.shape)
   * @return the match results and scores
   */
  std::vector<std::tuple<float, float, std::vector<meili::MatchResult>>> map_match(Api& request);

  void path_arrive_by(Api& api, const std::string& costing);
  void path_depart_at(Api& api, const std::string& costing);
  void parse_measurements(const Api& request);
  std::string parse_costing(const Api& request);

  void build_route(
      const std::deque<std::pair<std::vector<PathInfo>, std::vector<const meili::EdgeSegment*>>>&
          paths,
      const std::vector<meili::MatchResult>& match_results,
      Options& options,
      Api& request);

  void build_trace(
      const std::deque<std::pair<std::vector<PathInfo>, std::vector<const meili::EdgeSegment*>>>&
          paths,
      std::vector<meili::MatchResult>& match_results,
      Options& options,
      Api& request);

  sif::TravelMode mode;
  std::vector<meili::Measurement> trace;
  sif::CostFactory factory;
  sif::mode_costing_t mode_costing;

  // Path algorithms (TODO - perhaps use a map?))
  BidirectionalAStar bidir_astar;
  MultimodalAStar multimodal_astar;
  MultiModalPathAlgorithm multi_modal_transit;
  TimeDepForward timedep_forward;
  TimeDepReverse timedep_reverse;

  // Time distance matrix
  CostMatrix costmatrix_;
  TimeDistanceMatrix time_distance_matrix_;
  TimeDistanceBSSMatrix time_distance_bss_matrix_;

  Isochrone isochrone_gen;
  std::shared_ptr<meili::MapMatcher> matcher;
  float max_timedep_distance;
  std::unordered_map<std::string, float> max_matrix_distance;
  SOURCE_TO_TARGET_ALGORITHM source_to_target_algorithm;
  bool costmatrix_allow_second_pass;
  std::shared_ptr<baldr::GraphReader> reader;
  meili::MapMatcherFactory matcher_factory;
  baldr::AttributesController controller;
  Centroid centroid_gen;

  // Hierarchy limits
  bool allow_hierarchy_limits_modifications;
  // ignored if allow_hierarchy_limits_modifications is false
  hierarchy_limits_config_t hierarchy_limits_config_astar;
  hierarchy_limits_config_t hierarchy_limits_config_bidirectional_astar;
  hierarchy_limits_config_t hierarchy_limits_config_costmatrix;

  double min_linear_cost_factor;
  uint64_t max_linear_cost_edges;

  // ADR-0037 §4: per-request round-trip stage-timing ledger (harvest/scan/walk-back/
  // rejoin/A*/seam/build). Config "thor.roundtrip_stage_timing"; default off. It found
  // the decisive ScanBand regression (#54) and is the only stage-cost visibility on
  // the box, so it stays wired rather than compiled out.
  bool roundtrip_stage_timing;

  // wayfinder #46 cross-candidate distinctness. roundtrip_xcand_penalty: each built
  // loop's fresh-road edges soft-surcharge later candidates' return legs, pushing the
  // bank off shared corridors. roundtrip_sharing_filter: reject a built loop that shares
  // more than roundtrip_sharing_frac of its length with an already-kept one and refill
  // the slot. Both config-gated, default off, so the bank-distinctness fix can be A/B'd.
  bool roundtrip_xcand_penalty;
  bool roundtrip_sharing_filter;
  // Tuning for the two knobs above (config "thor.roundtrip_*"; defaults are the swept
  // ship values). xcand_strength = soft surcharge added per prior loop that rode an edge;
  // xcand_cap = max prior-use count that surcharge grows with (the ATMOS per-edge-increase
  // cap, so a genuinely-single corridor stays routable); sharing_frac = the fresh-road
  // overlap above which the filter calls a loop a near-duplicate.
  double roundtrip_xcand_strength;
  uint32_t roundtrip_xcand_cap;
  double roundtrip_sharing_frac;

  // PROTOTYPE proto/v4-p1 (curvagen-valhalla#10) — tiered road identity.  The
  // twin/parallel sidecar (thor::RoadTwinIndex) is built once per process at engine
  // start and read by roundtrip_impl: twins of a hard-excluded corridor edge are
  // hard-excluded too, parallels join the soft leash + progress-graded rejoin tier.
  // roundtrip_road_identity turns the whole mechanism off (v3 behaviour);
  // roundtrip_parallel_tier turns the parallel tier off (the "twins only" variant).
  bool roundtrip_road_identity;
  bool roundtrip_parallel_tier;
  double roundtrip_twin_radius_m;
  double roundtrip_parallel_radius_m;
  // PROTOTYPE proto/v4-p1 — F01: reject harvest chains that revisit an undirected edge
  // (or its twin).  Config-gated so the before/after is one binary.
  bool roundtrip_simple_chains;

  // PROTOTYPE proto/v4-p1.1 (curvagen-valhalla#12).
  // 1. switchback test: a geometric twin pair is only the same physical road if it is a
  //    different OSM way OR holds a constant lateral offset (a hairpin's arms do not).
  // 3. built-loop ranking: score the BUILT loop (both legs) instead of the harvest chain.
  // 4. geometry Defect Gate: reject a loop whose return rides forward-corridor twins for
  //    >= roundtrip_gate_twin_ride_m beyond the Start Exemption, or whose return carries
  //    an exact-mirror bounce >= roundtrip_gate_return_bounce_m anywhere (F08/G6b).
  // 5. fallback rungs: corridor+twins barred -> corridor only -> full soft leash.
  bool roundtrip_switchback_test;
  bool roundtrip_built_ranking;
  bool roundtrip_geometry_gate;
  double roundtrip_gate_twin_ride_m;
  double roundtrip_gate_return_bounce_m;
  bool roundtrip_fallback_rungs;
  uint32_t roundtrip_gate_refill_budget;
  // Ticket-literal intermediate rung (release the parallel leash only, twins still
  // barred).  A soft multiplier can never restore reachability, so this rung is a
  // provable no-op; kept behind a default-off knob for the measurement that says so.
  bool roundtrip_fallback_parallel_rung;
  // F09: the stall branch grants a fresh +want attempt budget even if something else
  // widened the pool first.
  bool roundtrip_f09_budget;
  double roundtrip_rank_overlap_w;
  double roundtrip_rank_disterr_w;
  // proto/v4-p2 (curvagen-valhalla#11): the Suurballe-Tarjan whole-loop pair pass.
  // roundtrip_pair_pass swaps harvest->select->return-A* for one disjoint-pair pass
  // over the harvest forest (the return is the pair's second path, reversed);
  // roundtrip_pair_bridge repairs a non-reversible stretch of that return with a local
  // hard-excluded A* instead of rebuilding the whole return; roundtrip_pair_sharing
  // applies the K x K near-dup filter at SELECTION (the pair is known before any build);
  // roundtrip_pair_band is the |built - target| / target the pair must satisfy to be
  // chosen; roundtrip_pair_shortlist is the per-sector top-M whose pairs are constructed
  // and scored before the seed-rotated pick.
  bool roundtrip_pair_pass;
  bool roundtrip_pair_bridge;
  bool roundtrip_pair_sharing;
  double roundtrip_pair_band;
  uint32_t roundtrip_pair_shortlist;
  double roundtrip_pair_twin_join_m; // fold twin carriageways whose end junctions sit within this (0 = off, the measured setting: the fold teleports across carriageways)
  uint32_t roundtrip_pair_max_bridges; // local repairs per loop before the whole return is rebuilt
  bool roundtrip_pair_return_legal;    // offer only return-rideable arcs to the second phase
  uint32_t roundtrip_pair_eval_cap;    // pair evaluations per request before the near-dup filter is dropped
  bool roundtrip_pair_two_way_tree;    // prefer two-way arrivals as tree arcs (the forward legs)
  bool roundtrip_pair_twin_reject;     // reject a pair riding a twin (fwd-vs-ret or ret-vs-ret) at selection
  // proto/v4-p2.1 (curvagen-valhalla#14): distinctness AT SELECTION.  All default off,
  // so the P2 behaviour is byte-identical when unset.
  //   roundtrip_pair_leg_sharing       reject a candidate whose FORWARD leg (the tree path
  //                                    = the served forward leg) shares more than
  //                                    roundtrip_pair_leg_sharing_frac of its length with
  //                                    a chosen / built loop's roads or their twins.  In
  //                                    this mode roundtrip_pair_eval_cap is a PER-RUNG
  //                                    budget of fresh pair evaluations, never a filter
  //                                    drop.
  //   roundtrip_pair_built_keys        key the built bank on the BUILT loop (both legs as
  //                                    served, every loop) instead of on the pair.
  //   roundtrip_pair_diversity_w       order a sector's survivors by score / (1 + w * s),
  //                                    s = max whole-pair sharing with the chosen set.
  //   roundtrip_pair_leg_relax         bank short after the main walk: re-walk the queue
  //                                    at frac + 0.15, + 0.30, then leg test off.
  //   roundtrip_pair_relaxed_last      rank loops admitted under a relaxed rung after the
  //                                    non-relaxed ones within their tier.
  bool roundtrip_pair_leg_sharing;
  double roundtrip_pair_leg_sharing_frac;
  bool roundtrip_pair_built_keys;
  double roundtrip_pair_diversity_w;
  bool roundtrip_pair_leg_relax;
  bool roundtrip_pair_relaxed_last;
  uint32_t roundtrip_pair_eval_total; // leg mode: fresh evaluations per REQUEST across all rungs (0 = per-rung budget only)

private:
  std::string service_name() const override {
    return "thor";
  }
};

} // namespace thor
} // namespace valhalla

#endif //__VALHALLA_THOR_SERVICE_H__
