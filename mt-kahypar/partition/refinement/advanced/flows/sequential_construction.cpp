/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2021 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#include "mt-kahypar/partition/refinement/advanced/flows/sequential_construction.h"

#include "mt-kahypar/parallel/stl/scalable_queue.h"

#include "mt-kahypar/partition/refinement/advanced/flows/flow_common.h"

namespace mt_kahypar {

whfc::Hyperedge SequentialConstruction::DynamicIdenticalNetDetection::add_if_not_contained(const whfc::Hyperedge he,
                                                                                           const size_t he_hash,
                                                                                           const vec<whfc::Node>& pins) {
  const size_t* bucket_idx = _he_hashes.get_if_contained(he_hash);
  if ( bucket_idx ) {
    // There exists already some hyperedges with the same hash
    for ( const whfc::Hyperedge& e : _hash_buckets[*bucket_idx] ) {
      // Check if there is some hyperedge equal to he
      if ( _flow_hg.pinCount(e) == pins.size() ) {
        bool is_identical = true;
        size_t idx = 0;
        for ( const whfc::FlowHypergraph::Pin& u : _flow_hg.pinsOf(e) ) {
          if ( u.pin != pins[idx++] ) {
            is_identical = false;
            break;
          }
        }
        if ( is_identical ) {
          return e;
        }
      }
    }
  }

  // There is no hyperedge currently identical to he
  if ( bucket_idx ) {
    // If there already exist hyperedges with the same hash,
    // we insert he into the corresponding bucket.
    _hash_buckets[*bucket_idx].push_back(he);
  } else {
    // Otherwise, we create a new bucket (or reuse an existing)
    // and insert he into the hash table
    size_t idx = std::numeric_limits<size_t>::max();
    if ( _used_entries < _hash_buckets.size() ) {
      _hash_buckets[_used_entries].clear();
    } else {
      _hash_buckets.emplace_back();
    }
    idx = _used_entries++;
    _hash_buckets[idx].push_back(he);
    _he_hashes[he_hash] = idx;
  }

  return whfc::invalidHyperedge;
}

FlowProblem SequentialConstruction::constructFlowHypergraph(const PartitionedHypergraph& phg,
                                                            const Subhypergraph& sub_hg,
                                                            const PartitionID block_0,
                                                            const PartitionID block_1,
                                                            vec<HypernodeID>& whfc_to_node) {
  ASSERT(block_0 != kInvalidPartition && block_1 != kInvalidPartition);
  FlowProblem flow_problem;
  flow_problem.total_cut = 0;
  flow_problem.non_removable_cut = 0;
  _identical_nets.reset();
  _node_to_whfc.clear();
  whfc_to_node.resize(sub_hg.nodes.size() + 2);

  if ( _context.refinement.advanced.flows.determine_distance_from_cut ) {
    _cut_hes.clear();
  }

  // Add refinement nodes to flow network
  whfc::Node flow_hn(0);
  HypernodeWeight weight_block_0 = 0;
  HypernodeWeight weight_block_1 = 0;
  auto add_nodes = [&](const PartitionID block, HypernodeWeight& weight_of_block) {
    for ( const HypernodeID& hn : sub_hg.nodes) {
      if ( phg.partID(hn) == block ) {
        const HypernodeWeight hn_weight = phg.nodeWeight(hn);
        whfc_to_node[flow_hn] = hn;
        _node_to_whfc[hn] = flow_hn++;
        _flow_hg.addNode(whfc::NodeWeight(hn_weight));
        weight_of_block += hn_weight;
      }
    }
  };
  // Add source nodes
  whfc_to_node[flow_hn] = kInvalidHypernode;
  flow_problem.source = flow_hn++;
  _flow_hg.addNode(whfc::NodeWeight(0));
  add_nodes(block_0, weight_block_0);
  _flow_hg.nodeWeight(flow_problem.source) = whfc::NodeWeight(std::max(0, phg.partWeight(block_0) - weight_block_0));
  // Add sink nodes
  whfc_to_node[flow_hn] = kInvalidHypernode;
  flow_problem.sink = flow_hn++;
  _flow_hg.addNode(whfc::NodeWeight(0));
  add_nodes(block_1, weight_block_1);
  _flow_hg.nodeWeight(flow_problem.sink) = whfc::NodeWeight(std::max(0, phg.partWeight(block_1) - weight_block_1));
  flow_problem.weight_of_block_0 = _flow_hg.nodeWeight(flow_problem.source) + weight_block_0;
  flow_problem.weight_of_block_1 = _flow_hg.nodeWeight(flow_problem.sink) + weight_block_1;
  whfc_to_node.resize(flow_hn);

  auto push_into_tmp_pins = [&](const whfc::Node pin, size_t& current_hash, const bool is_source_or_sink) {
    _tmp_pins.push_back(pin);
    current_hash += kahypar::math::hash(pin);
    if ( is_source_or_sink ) {
      // According to Lars: Adding to source or sink to the start of
      // each pin list improves running time
      std::swap(_tmp_pins[0], _tmp_pins.back());
    }
  };

  // Add hyperedge to flow network and configure source and sink
  whfc::Hyperedge current_he(0);
  for ( const HyperedgeID& he : sub_hg.hes ) {
    if ( !canHyperedgeBeDropped(phg, he, block_0, block_1) ) {
      size_t he_hash = 0;
      _tmp_pins.clear();
      const HyperedgeWeight he_weight = phg.edgeWeight(he);
      _flow_hg.startHyperedge(whfc::Flow(he_weight));
      bool connectToSource = false;
      bool connectToSink = false;
      if ( phg.pinCountInPart(he, block_0) > 0 && phg.pinCountInPart(he, block_1) > 0 ) {
        flow_problem.total_cut += he_weight;
      }
      for ( const HypernodeID& pin : phg.pins(he) ) {
        if ( _node_to_whfc.contains(pin) ) {
          push_into_tmp_pins(_node_to_whfc[pin], he_hash, false);
        } else {
          const PartitionID pin_block = phg.partID(pin);
          connectToSource |= pin_block == block_0;
          connectToSink |= pin_block == block_1;
        }
      }

      const bool empty_hyperedge = _tmp_pins.size() == 0;
      const bool connected_to_source_and_sink = connectToSource && connectToSink;
      if ( connected_to_source_and_sink || empty_hyperedge ) {
        // Hyperedge is connected to source and sink which means we can not remove it
        // from the cut with the current flow problem => remove he from flow problem
        _flow_hg.removeCurrentHyperedge();
        flow_problem.non_removable_cut += connected_to_source_and_sink ? he_weight : 0;
      } else {

        if ( connectToSource ) {
          push_into_tmp_pins(flow_problem.source, he_hash, true);
        } else if ( connectToSink ) {
          push_into_tmp_pins(flow_problem.sink, he_hash, true);
        }

        // Sort pins for identical net detection
        std::sort( _tmp_pins.begin() +
                 ( _tmp_pins[0] == flow_problem.source ||
                   _tmp_pins[0] == flow_problem.sink), _tmp_pins.end());

        if ( _tmp_pins.size() > 1 ) {
          whfc::Hyperedge identical_net =
            _identical_nets.add_if_not_contained(current_he, he_hash, _tmp_pins);
          if ( identical_net == whfc::invalidHyperedge ) {
            for ( const whfc::Node& pin : _tmp_pins ) {
              _flow_hg.addPin(pin);
            }
            if ( _context.refinement.advanced.flows.determine_distance_from_cut &&
                 phg.pinCountInPart(he, block_0) > 0 && phg.pinCountInPart(he, block_1) > 0 ) {
              _cut_hes.push_back(current_he);
            }
            ++current_he;
          } else {
            // Current hyperedge is identical to an already added
            _flow_hg.capacity(identical_net) += he_weight;
          }
        }
      }
    }
  }

  if ( _flow_hg.nodeWeight(flow_problem.source) == 0 ||
       _flow_hg.nodeWeight(flow_problem.sink) == 0 ) {
    // Source or sink not connected to vertices in the flow problem
    flow_problem.non_removable_cut = 0;
    flow_problem.total_cut = 0;
  } else {
    _flow_hg.finalize();

    if ( _context.refinement.advanced.flows.determine_distance_from_cut ) {
      // Determine the distance of each node contained in the flow network from the cut.
      // This technique improves piercing decision within the WHFC framework.
      determineDistanceFromCut(phg, flow_problem.source,
        flow_problem.sink, block_0, block_1, whfc_to_node);
    }
  }

  DBG << "Flow Hypergraph [ Nodes =" << _flow_hg.numNodes()
      << ", Edges =" << _flow_hg.numHyperedges()
      << ", Pins =" << _flow_hg.numPins()
      << ", Blocks = (" << block_0 << "," << block_1 << ") ]";

  return flow_problem;
}

void SequentialConstruction::determineDistanceFromCut(const PartitionedHypergraph& phg,
                                                      const whfc::Node source,
                                                      const whfc::Node sink,
                                                      const PartitionID block_0,
                                                      const PartitionID block_1,
                                                      const vec<HypernodeID>& whfc_to_node) {
  _hfc.cs.borderNodes.distance.distance.assign(_flow_hg.numNodes(), whfc::HopDistance(0));
  _visited_hns.resize(_flow_hg.numNodes() + _flow_hg.numHyperedges());
  _visited_hns.reset();

  // Initialize bfs queue with vertices contained in cut hyperedges
  parallel::scalable_queue<whfc::Node> q;
  parallel::scalable_queue<whfc::Node> next_q;
  for ( const whfc::Hyperedge& he : _cut_hes ) {
    for ( const whfc::FlowHypergraph::Pin& pin : _flow_hg.pinsOf(he) ) {
      if ( pin.pin != source && pin.pin != sink && !_visited_hns[pin.pin] ) {
        q.push(pin.pin);
        _visited_hns.setUnsafe(pin.pin, true);
      }
    }
    _visited_hns.setUnsafe(_flow_hg.numNodes() + he, true);
  }

  // Perform BFS to determine distance of each vertex from cut
  whfc::HopDistance dist(1);
  whfc::HopDistance max_dist_source(0);
  whfc::HopDistance max_dist_sink(0);
  while ( !q.empty() ) {
    const whfc::Node u = q.front();
    q.pop();

    const PartitionID block_of_u = phg.partID(whfc_to_node[u]);
    if ( block_of_u == block_0 ) {
      _hfc.cs.borderNodes.distance[u] = -dist;
      max_dist_source = std::max(max_dist_source, dist);
    } else if ( block_of_u == block_1 ) {
      _hfc.cs.borderNodes.distance[u] = dist;
      max_dist_sink = std::max(max_dist_sink, dist);
    }

    for ( const whfc::FlowHypergraph::InHe& in_he : _flow_hg.hyperedgesOf(u) ) {
      const whfc::Hyperedge he = in_he.e;
      if ( !_visited_hns[_flow_hg.numNodes() + he] ) {
        for ( const whfc::FlowHypergraph::Pin& pin : _flow_hg.pinsOf(he) ) {
          if ( pin.pin != source && pin.pin != sink && !_visited_hns[pin.pin] ) {
            next_q.push(pin.pin);
            _visited_hns.setUnsafe(pin.pin, true);
          }
        }
        _visited_hns.setUnsafe(_flow_hg.numNodes() + he, true);
      }
    }

    if ( q.empty() ) {
      std::swap(q, next_q);
      ++dist;
    }
  }
  _hfc.cs.borderNodes.distance[source] = -(max_dist_source + 1);
  _hfc.cs.borderNodes.distance[sink] = max_dist_sink + 1;
}

} // namespace mt_kahypar