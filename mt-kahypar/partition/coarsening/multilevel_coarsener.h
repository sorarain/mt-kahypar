/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2019 Lars Gottesbüren <lars.gottesbueren@kit.edu>
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#pragma once

#include <string>

#include "tbb/concurrent_queue.h"
#include "tbb/task_group.h"
#include "tbb/parallel_for.h"
#include "tbb/parallel_reduce.h"

#include "kahypar/meta/mandatory.h"

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/partition/coarsening/multilevel_coarsener_base.h"
#include "mt-kahypar/partition/coarsening/multilevel_vertex_pair_rater.h"
#include "mt-kahypar/partition/coarsening/i_coarsener.h"
#include "mt-kahypar/partition/coarsening/policies/rating_acceptance_policy.h"
#include "mt-kahypar/partition/coarsening/policies/rating_heavy_node_penalty_policy.h"
#include "mt-kahypar/partition/coarsening/policies/rating_score_policy.h"
#include "mt-kahypar/parallel/atomic_wrapper.h"
#include "mt-kahypar/utils/progress_bar.h"
#include "mt-kahypar/utils/randomize.h"
#include "mt-kahypar/utils/stats.h"
#include "mt-kahypar/utils/timer.h"

namespace mt_kahypar {
template <class ScorePolicy = HeavyEdgeScore,
          class HeavyNodePenaltyPolicy = MultiplicativePenalty,
          class AcceptancePolicy = BestRatingPreferringUnmatched>
class MultilevelCoarsener : public ICoarsener,
                            private MultilevelCoarsenerBase {
 private:

  using Base = MultilevelCoarsenerBase;
  using Rater = MultilevelVertexPairRater<ScorePolicy,
                                          HeavyNodePenaltyPolicy,
                                          AcceptancePolicy>;
  using Rating = typename Rater::Rating;

  enum class MatchingState : uint8_t {
    UNMATCHED = 0,
    MATCHING_IN_PROGRESS = 1,
    MATCHED = 2
  };

  #define STATE(X) static_cast<uint8_t>(X)
  using AtomicMatchingState = parallel::IntegralAtomicWrapper<uint8_t>;
  using AtomicWeight = parallel::IntegralAtomicWrapper<HypernodeWeight>;

  static constexpr bool debug = false;
  static constexpr bool enable_heavy_assert = false;
  static constexpr HypernodeID kInvalidHypernode = std::numeric_limits<HypernodeID>::max();

 public:
  MultilevelCoarsener(Hypergraph& hypergraph,
                      const Context& context,
                      UncoarseningData& uncoarseningData) :
    Base(hypergraph, context, uncoarseningData),
    _rater(hypergraph, context),
    _initial_num_nodes(hypergraph.initialNumNodes()),
    _current_vertices(),
    _matching_state(),
    _cluster_weight(),
    _matching_partner(),
    _pass_nr(0),
    _progress_bar(hypergraph.initialNumNodes(), 0, false),
    _enable_randomization(true) {
    _progress_bar += hypergraph.numRemovedHypernodes();

    // Initialize internal data structures parallel
    tbb::parallel_invoke([&] {
      _current_vertices.resize(hypergraph.initialNumNodes());
    }, [&] {
      _matching_state.resize(hypergraph.initialNumNodes());
    }, [&] {
      _cluster_weight.resize(hypergraph.initialNumNodes());
    }, [&] {
      _matching_partner.resize(hypergraph.initialNumNodes());
    });
  }

  MultilevelCoarsener(const MultilevelCoarsener&) = delete;
  MultilevelCoarsener(MultilevelCoarsener&&) = delete;
  MultilevelCoarsener & operator= (const MultilevelCoarsener &) = delete;
  MultilevelCoarsener & operator= (MultilevelCoarsener &&) = delete;

  ~MultilevelCoarsener() {
    parallel::parallel_free(
      _current_vertices, _matching_state,
      _cluster_weight, _matching_partner);
  }

  void disableRandomization() {
    _enable_randomization = false;
  }

 private:
  void initializeImpl() override {
    if ( _context.partition.verbose_output && _context.partition.enable_progress_bar ) {
      _progress_bar.enable();
    }
  }

  bool shouldNotTerminateImpl() const override {
    return Base::currentNumNodes() > _context.coarsening.contraction_limit;
  }

  bool coarseningPassImpl() override {
    HighResClockTimepoint round_start = std::chrono::high_resolution_clock::now();
    Hypergraph& current_hg = Base::currentHypergraph();
    DBG << V(_pass_nr)
        << V(current_hg.initialNumNodes())
        << V(current_hg.initialNumEdges())
        << V(current_hg.initialNumPins());

    // Random shuffle vertices of current hypergraph
    _current_vertices.resize(current_hg.initialNumNodes());
    parallel::scalable_vector<HypernodeID> cluster_ids(current_hg.initialNumNodes());
    tbb::parallel_for(ID(0), current_hg.initialNumNodes(), [&](const HypernodeID hn) {
      ASSERT(hn < _current_vertices.size());
      // Reset clustering
      _current_vertices[hn] = hn;
      _matching_state[hn] = STATE(MatchingState::UNMATCHED);
      _matching_partner[hn] = hn;
      cluster_ids[hn] = hn;
      if ( current_hg.nodeIsEnabled(hn) ) {
        _cluster_weight[hn] = current_hg.nodeWeight(hn);
      }
    });

    if ( _enable_randomization ) {
      utils::Randomize::instance().parallelShuffleVector( _current_vertices, UL(0), _current_vertices.size());
    }

    // We iterate in parallel over all vertices of the hypergraph and compute its contraction partner.
    // Matched vertices are linked in a concurrent union find data structure, that also aggregates
    // weights of the resulting clusters and keep track of the number of nodes left, if we would
    // contract all matched vertices.
    _timer.start_timer("clustering", "Clustering");
    if ( _context.partition.show_detailed_clustering_timings ) {
      _timer.start_timer("clustering_level_" + std::to_string(_pass_nr), "Level " + std::to_string(_pass_nr));
    }
    _rater.resetMatches();
    _rater.setCurrentNumberOfNodes(current_hg.initialNumNodes());
    const HypernodeID num_hns_before_pass = current_hg.initialNumNodes() - current_hg.numRemovedHypernodes();
    const HypernodeID hierarchy_contraction_limit = hierarchyContractionLimit(current_hg);
    DBG << V(current_hg.initialNumNodes()) << V(hierarchy_contraction_limit);
    HypernodeID current_num_nodes = num_hns_before_pass;
    tbb::enumerable_thread_specific<HypernodeID> contracted_nodes(0);
    tbb::enumerable_thread_specific<HypernodeID> num_nodes_update_threshold(0);
    tbb::parallel_for(0U, current_hg.initialNumNodes(), [&](const HypernodeID id) {
      ASSERT(id < _current_vertices.size());
      const HypernodeID hn = _current_vertices[id];
      if (current_hg.nodeIsEnabled(hn)) {
        // We perform rating if ...
        //  1.) The contraction limit of the current level is not reached
        //  2.) Vertex hn is not matched before
        const HypernodeID u = hn;
        if (_matching_state[u] == STATE(MatchingState::UNMATCHED)) {
          if (current_num_nodes > hierarchy_contraction_limit) {
            ASSERT(current_hg.nodeIsEnabled(hn));
            const Rating rating = _rater.rate(current_hg, hn,
              cluster_ids, _cluster_weight, _context.coarsening.max_allowed_node_weight);
            if (rating.target != kInvalidHypernode) {
              const HypernodeID v = rating.target;
              HypernodeID& local_contracted_nodes = contracted_nodes.local();
              matchVertices(current_hg, u, v, cluster_ids, local_contracted_nodes);

              // To maintain the current number of nodes of the hypergraph each PE sums up
              // its number of contracted nodes locally. To compute the current number of
              // nodes, we have to sum up the number of contracted nodes of each PE. This
              // operation becomes more expensive the more PEs are participating in coarsening.
              // In order to prevent expensive updates of the current number of nodes, we
              // define a threshold which the local number of contracted nodes have to exceed
              // before the current PE updates the current number of nodes. This threshold is defined
              // by the distance to the current contraction limit divided by the number of PEs.
              // Once one PE exceeds this bound the first time it is not possible that the
              // contraction limit is reached, because otherwise an other PE would update
              // the global current number of nodes before. After update the threshold is
              // increased by the new difference (in number of nodes) to the contraction limit
              // divided by the number of PEs.
              if (local_contracted_nodes >= num_nodes_update_threshold.local()) {
                current_num_nodes = num_hns_before_pass -
                                    contracted_nodes.combine(std::plus<HypernodeID>());
                const HypernodeID dist_to_contraction_limit =
                  current_num_nodes > hierarchy_contraction_limit ?
                  current_num_nodes - hierarchy_contraction_limit : 0;
                num_nodes_update_threshold.local() +=
                  dist_to_contraction_limit / _context.shared_memory.original_num_threads;
              }
            }
          }
        }
      }
    });
    if ( _context.partition.show_detailed_clustering_timings ) {
      _timer.stop_timer("clustering_level_" + std::to_string(_pass_nr));
    }
    _timer.stop_timer("clustering");
    current_num_nodes = num_hns_before_pass - contracted_nodes.combine(std::plus<>());
    DBG << V(current_num_nodes);

    HEAVY_COARSENING_ASSERT([&] {
      parallel::scalable_vector<HypernodeWeight> expected_weights(current_hg.initialNumNodes());
      // Verify that clustering is correct
      for ( const HypernodeID& hn : current_hg.nodes() ) {
        const HypernodeID u = hn;
        const HypernodeID root_u = cluster_ids[u];
        if ( root_u != cluster_ids[root_u] ) {
          LOG << "Hypernode" << u << "is part of cluster" << root_u << ", but cluster"
              << root_u << "is also part of cluster" << cluster_ids[root_u];
          return false;
        }
        expected_weights[root_u] += current_hg.nodeWeight(hn);
      }

      // Verify that cluster weights are aggregated correct
      for ( const HypernodeID& hn : current_hg.nodes() ) {
        const HypernodeID u = hn;
        const HypernodeID root_u = cluster_ids[u];
        if ( root_u == u && expected_weights[u] != _cluster_weight[u] ) {
          LOG << "The expected weight of cluster" << u << "is" << expected_weights[u]
              << ", but currently it is" << _cluster_weight[u];
          return false;
        }
      }
      return true;
    }(), "Parallel clustering computed invalid cluster ids and weights");

    const double reduction_vertices_percentage =
      static_cast<double>(num_hns_before_pass) /
      static_cast<double>(current_num_nodes);
    if ( reduction_vertices_percentage <= _context.coarsening.minimum_shrink_factor ) {
      return false;
    }
    _progress_bar += (num_hns_before_pass - current_num_nodes);

    _timer.start_timer("contraction", "Contraction");
    // Perform parallel contraction
    _uncoarseningData.performMultilevelContraction(std::move(cluster_ids), round_start);
    _timer.stop_timer("contraction");

    ++_pass_nr;
    return true;
  }

  void terminateImpl() override {
    _progress_bar += (_initial_num_nodes - _progress_bar.count());
    _progress_bar.disable();
    _uncoarseningData.finalizeCoarsening();
  }


  /*!
   * We maintain the invariant during clustering that each cluster has a unique
   * representative and all vertices also part of that cluster point to that
   * representative. Let v be the representative of a cluster C_v, then for
   * all nodes u \in C_v follows that cluster_ids[u] = v.
   * If we perform sequential clustering, we can simply set
   * cluster_ids[u] = cluster_ids[v] to maintain our invariant. However,
   * things become more complicated if we perform parallel clustering.
   * Especially, if two neighbors u and v are concurrently matched, we have
   * to guarantee that our clustering fullfils our invariant. There are mainly
   * two different cases, which needs special attention:
   *   1.) u is matched with v and v is matched with u concurrently
   *   2.) u is matched with v and v is matched an other vertex w concurrently
   * The following functions guarantees that our invariant is fullfilled, if
   * vertices are matched concurrently.
   */
  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE bool matchVertices(const Hypergraph& hypergraph,
                                                        const HypernodeID u,
                                                        const HypernodeID v,
                                                        parallel::scalable_vector<HypernodeID>& cluster_ids,
                                                        HypernodeID& contracted_nodes) {
    ASSERT(u < hypergraph.initialNumNodes());
    ASSERT(v < hypergraph.initialNumNodes());
    uint8_t unmatched = STATE(MatchingState::UNMATCHED);
    uint8_t match_in_progress = STATE(MatchingState::MATCHING_IN_PROGRESS);

    // Indicates that u wants to join the cluster of v.
    // Will be important later for conflict resolution.
    bool success = false;
    const HypernodeWeight weight_u = hypergraph.nodeWeight(u);
    HypernodeWeight weight_v = _cluster_weight[v];
    if ( weight_u + weight_v <= _context.coarsening.max_allowed_node_weight ) {

      if ( _matching_state[u].compare_exchange_strong(unmatched, match_in_progress) ) {
        _matching_partner[u] = v;
        // Current thread gets "ownership" for vertex u. Only threads with "ownership"
        // can change the cluster id of a vertex.

        uint8_t matching_state_v = _matching_state[v].load();
        if ( matching_state_v == STATE(MatchingState::MATCHED) ) {
          // Vertex v is already matched and will not change it cluster id any more.
          // In that case, it is safe to set the cluster id of u to the cluster id of v.
          if ( v == cluster_ids[v] ) {
            // In case v is also the representative of the cluster,
            // we change the cluster id of u to v, ...
            cluster_ids[u] = v;
            _cluster_weight[v] += weight_u;
            ++contracted_nodes;
            success = true;
          } else {
            // ... otherwise, we try again to match u with the
            // representative of the cluster.
            const HypernodeID cluster_v = cluster_ids[v];
            weight_v = _cluster_weight[cluster_v];
            if ( weight_u + weight_v <= _context.coarsening.max_allowed_node_weight ) {
              ASSERT(_matching_state[cluster_v] == STATE(MatchingState::MATCHED));
              cluster_ids[u] = cluster_v;
              _cluster_weight[cluster_v] += weight_u;
              ++contracted_nodes;
              success = true;
            }
          }
        } else if ( _matching_state[v].compare_exchange_strong(unmatched, match_in_progress) ) {
          // Current thread has the "ownership" for u and v and can change the cluster id
          // of both vertices thread-safe.
          cluster_ids[u] = v;
          _cluster_weight[v] += weight_u;
          ++contracted_nodes;
          _matching_state[v] = STATE(MatchingState::MATCHED);
          success = true;
        } else {
          // State of v must be either MATCHING_IN_PROGRESS or an other thread changed the state
          // in the meantime to MATCHED. We have to wait until the state of v changed to
          // MATCHED or resolve the conflict if u is matched within a cyclic matching dependency

          // Conflict Resolution
          while ( _matching_state[v] == STATE(MatchingState::MATCHING_IN_PROGRESS) ) {

            // Check if current vertex is in a cyclic matching dependency
            HypernodeID cur_u = u;
            HypernodeID smallest_node_id_in_cycle = cur_u;
            while ( _matching_partner[cur_u] != u && _matching_partner[cur_u] != cur_u ) {
              cur_u = _matching_partner[cur_u];
              smallest_node_id_in_cycle = std::min(smallest_node_id_in_cycle, cur_u);
            }

            // Resolve cyclic matching dependency
            // Vertex with smallest id starts to resolve conflict
            const bool is_in_cyclic_dependency = _matching_partner[cur_u] == u;
            if ( is_in_cyclic_dependency && u == smallest_node_id_in_cycle) {
              cluster_ids[u] = v;
              _cluster_weight[v] += weight_u;
              ++contracted_nodes;
              _matching_state[v] = STATE(MatchingState::MATCHED);
              _matching_state[u] = STATE(MatchingState::MATCHED);
              success = true;
            }
          }

          // If u is still in state MATCHING_IN_PROGRESS its matching partner v
          // must be matched in the meantime with an other vertex. Therefore,
          // we try to match u with the representative v's cluster.
          if ( _matching_state[u] == STATE(MatchingState::MATCHING_IN_PROGRESS) ) {
            ASSERT( _matching_state[v] == STATE(MatchingState::MATCHED) );
            const HypernodeID cluster_v = cluster_ids[v];
            const HypernodeWeight weight_v = _cluster_weight[cluster_v];
            if ( weight_u + weight_v <= _context.coarsening.max_allowed_node_weight ){
              cluster_ids[u] = cluster_v;
              _cluster_weight[cluster_v] += weight_u;
              ++contracted_nodes;
              success = true;
            }
          }
        }
        _rater.markAsMatched(u);
        _rater.markAsMatched(v);
        _matching_partner[u] = u;
        _matching_state[u] = STATE(MatchingState::MATCHED);
      }
    }
    return success;
  }

  HypernodeID currentNumberOfNodesImpl() const override {
    return Base::currentNumNodes();
  }

  Hypergraph& coarsestHypergraphImpl() override {
    return Base::currentHypergraph();
  }

  PartitionedHypergraph& coarsestPartitionedHypergraphImpl() override {
    return Base::currentPartitionedHypergraph();
  }

  HypernodeID hierarchyContractionLimit(const Hypergraph& hypergraph) const {
    return std::max( static_cast<HypernodeID>( static_cast<double>(hypergraph.initialNumNodes() -
      hypergraph.numRemovedHypernodes()) / _context.coarsening.maximum_shrink_factor ),
      _context.coarsening.contraction_limit );
  }

  using Base::_context;
  Rater _rater;
  HypernodeID _initial_num_nodes;
  parallel::scalable_vector<HypernodeID> _current_vertices;
  parallel::scalable_vector<AtomicMatchingState> _matching_state;
  parallel::scalable_vector<AtomicWeight> _cluster_weight;
  parallel::scalable_vector<HypernodeID> _matching_partner;
  int _pass_nr;
  utils::ProgressBar _progress_bar;
  bool _enable_randomization;
};

}  // namespace mt_kahypar
