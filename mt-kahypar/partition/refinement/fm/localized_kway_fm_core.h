/*******************************************************************************
 * This file is part of MT-KaHyPar.
 *
 * Copyright (C) 2020 Lars Gottesbüren <lars.gottesbueren@kit.edu>
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


#pragma once

#include <mt-kahypar/definitions.h>
#include <mt-kahypar/partition/context.h>

#include "fm_commons.h"
#include "clearlist.hpp"


namespace mt_kahypar {
namespace refinement {


class LocalizedKWayFM {
public:
  // unfortunately the compiler thinks we're trying to pass a const-ref for the pq_handles, which we don't. therefore it had to be a pointer :(
  explicit LocalizedKWayFM(const Context& context, HypernodeID numNodes, vec<PosT>* pq_handles) :
          numParts(context.partition.k),
          blockPQ(static_cast<size_t>(numParts)),
          vertexPQs(static_cast<size_t>(numParts), VertexPriorityQueue(*pq_handles)),
          updateDeduplicator(numNodes),
          context(context),
          max_part_weight(context.partition.max_part_weights[0]),
          perfect_balance_part_weight(context.partition.perfect_balance_part_weights[0]),
          min_part_weight(static_cast<HypernodeWeight>(std::floor(perfect_balance_part_weight * (1 - context.partition.epsilon))))
  {

  }


  void findMoves(PartitionedHypergraph& phg, const HypernodeID u, FMSharedData& sharedData, SearchID search_id) {
    /*  NOTE (Lars): only for the version with local rollbacks
    HyperedgeWeight bestGain = 0;
    size_t bestGainIndex = 0;
    HyperedgeWeight overallGain = 0;
    */
    this->thisSearch = search_id;
    reinitialize();
    uint32_t movesWithNonPositiveGain = 0;
    insertOrUpdatePQ(phg, u, sharedData.nodeTracker);

    Move m;
    while (movesWithNonPositiveGain < context.refinement.fm.max_number_of_fruitless_moves && findNextMove(phg, m)) {

      sharedData.nodeTracker.deactivateNode(m.node, thisSearch);
      deactivatedNodes.push_back(m.node);

      if (phg.changeNodePartWithBalanceCheckAndGainUpdatesAndPartWeightUpdates(m.node, m.from, m.to, max_part_weight)) {

        if (phg.partWeight(m.from) <= min_part_weight) {
          blockPQ.remove(m.from);
        }

        performSharedDataUpdates(m, phg, sharedData);
        movesWithNonPositiveGain = m.gain > 0 ? 0 : movesWithNonPositiveGain + 1;

        // activate neighbors of m.node and update their gains
        for (HyperedgeID e : phg.incidentEdges(m.node)) {
          if (phg.edgeSize(e) < context.partition.hyperedge_size_threshold) {
            for (HypernodeID v : phg.pins(e)) {
              if (!updateDeduplicator.contains(u)) {
                updateDeduplicator.insert(u);
                insertOrUpdatePQ(phg, v, sharedData.nodeTracker);
              }
            }
          }
        }
        updateDeduplicator.clear();

        /*  NOTE (Lars): only for the version with local rollbacks
        localMoves.push_back(m);
        overallGain += m.gain;
        if (overallGain < bestGain) {
          bestGain = overallGain;
          bestGainIndex = localMoves.size();
        }
        */
      }
    }

    // revertToBestLocalPrefix(phg, bestGainIndex);   NOTE (Lars): only for the version with local rollbacks

  }


private:

  void performSharedDataUpdates(Move& m, PartitionedHypergraph& phg, FMSharedData& sd) {
    const HypernodeID u = m.node;

    // TODO either sync this with the balance decision (requires double CAS operation)
    // or run another prefix sum on the final move order that checks balance
    // And at least get the move ID increment closer to the balance decision
    const MoveID move_id = sd.moveTracker.insertMove(m);

    for (HyperedgeID he : phg.incidentEdges(u)) {
      // update first move in
      std::atomic<MoveID>& fmi = sd.first_move_in[he * numParts + m.to];
      MoveID expected = fmi.load(std::memory_order_acq_rel);
      while ((sd.moveTracker.isIDStale(expected) || expected > move_id) && !fmi.compare_exchange_weak(expected, move_id, std::memory_order_acq_rel)) {  }

      // update last move out
      std::atomic<MoveID>& lmo = sd.last_move_out[he * numParts + m.from];
      expected = lmo.load(std::memory_order_acq_rel);
      while (expected < move_id && !lmo.compare_exchange_weak(expected, move_id, std::memory_order_acq_rel)) { }
    }
  }

  std::pair<PartitionID, HyperedgeWeight> bestDestinationBlock(PartitionedHypergraph& phg, HypernodeID u) {
    const HypernodeWeight wu = phg.nodeWeight(u);
    PartitionID to = kInvalidPartition;
    HyperedgeWeight to_penalty = std::numeric_limits<HyperedgeWeight>::max();
    for (PartitionID i = 0; i < phg.k(); ++i) {
      const HyperedgeWeight penalty = phg.moveToPenalty(u, i);
      if (penalty < to_penalty && phg.partWeight(to) + wu <= context.partition.max_part_weights[i]) {
        to_penalty = penalty;
        to = i;
      }
    }
    return std::make_pair(to, phg.moveFromBenefit(u, phg.partID(u)) - to_penalty);
  };

  void insertOrUpdatePQ(PartitionedHypergraph& phg, HypernodeID u, NodeTracker& nt) {
    SearchID searchOfU = nt.searchOfNode[u].load(std::memory_order_acq_rel);

    if (nt.isSearchInactive(searchOfU)) {   // both branches already exclude deactivated nodes

      // try to claim node u
      if (nt.searchOfNode[u].compare_exchange_strong(searchOfU, thisSearch, std::memory_order_acq_rel)) {

        // if that was successful, we insert it into the vertices ready to move in its current block
        const PartitionID from = phg.partID(u);
        auto [to, gain] = bestDestinationBlock(phg, u);
        if (!blockPQ.contains(from) && phg.partWeight(from) > min_part_weight) {
          blockPQ.insert(from, gain);
        }
        vertexPQs[from].insert(u, gain);

        // and we update the gain of moving the best vertex from that block
        if (blockPQ.contains(from) && gain > blockPQ.keyOf(from)) {
          blockPQ.increaseKey(from, gain);
        }
      }

    } else if (searchOfU == thisSearch) {

      // update PQ entries
      const PartitionID from = phg.partID(u);
      auto [to, gain] = bestDestinationBlock(phg, u);
      vertexPQs[from].adjustKey(u, gain);

      if (blockPQ.contains(from) && gain > blockPQ.keyOf(from)) {
        blockPQ.increaseKey(from, gain);
      }
    }

  }

  bool findNextMove(PartitionedHypergraph& phg, Move& m) {
    while (!blockPQ.empty()) {
      const PartitionID from = blockPQ.top();
      const HypernodeID u = vertexPQs[from].top();
      const Gain estimated_gain = vertexPQs[from].topKey();
      auto [to, gain] = bestDestinationBlock(phg, u);
      if (gain >= estimated_gain) { // accept any gain that is at least as good
        vertexPQs[from].deleteTop();
        m.node = u; m.to = to; m.from = from;
        m.gain = phg.km1Gain(u, from, to);
        return true;
      } else {
        vertexPQs[from].adjustKey(u, gain);
        if (vertexPQs[from].topKey() != blockPQ.keyOf(from)) {
          blockPQ.adjustKey(from, vertexPQs[from].topKey());
        }
      }
    }
    return false;
  }

  void reinitialize() {
    localMoves.clear();
  }

  void revertToBestLocalPrefix(PartitionedHypergraph &phg, size_t bestGainIndex) {
    while (localMoves.size() > bestGainIndex) {
      Move& m = localMoves.back();
      phg.changeNodePart(m.node, m.to, m.from);
      localMoves.pop_back();
    }
  }

  SearchID thisSearch;
  vec<Move> localMoves;
  PartitionID numParts;

  BlockPriorityQueue blockPQ;
  vec<VertexPriorityQueue> vertexPQs;

  // Note: prefer ClearListSet over SparseSet because
  // ClearListSet takes numNodes + numInsertedNodes*32 bit
  // SparseSet takes 2 * numNodes * 32 bit
  // where numInsertedNodes is presumably much smaller than numNodes
  ldc::ClearListSet<HypernodeID> updateDeduplicator;

  const Context& context;
  HypernodeWeight max_part_weight, perfect_balance_part_weight, min_part_weight;


public:
  vec<HypernodeID> deactivatedNodes;
};

}
}