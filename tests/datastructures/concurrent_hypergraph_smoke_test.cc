/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
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

#include "gmock/gmock.h"

#include "tbb/task_arena.h"
#include "tbb/task_group.h"
#include "tbb/blocked_range.h"

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/io/hypergraph_io.h"
#include "mt-kahypar/utils/randomize.h"

using ::testing::Test;

namespace mt_kahypar {
namespace ds {

template< PartitionID k, size_t num_threads >
struct TestConfig {
  static constexpr PartitionID K = k;
  static constexpr size_t NUM_THREADS = num_threads;
};

template< typename Config >
class AConcurrentHypergraph : public Test {

 public:
  AConcurrentHypergraph() :
    k(Config::K),
    num_threads(Config::NUM_THREADS),
    hypergraph() {
    int cpu_id = sched_getcpu();
    hypergraph =
      io::readHypergraphFile("../partition/test_instances/ibm01.hgr", k);
    for ( const HypernodeID& hn : hypergraph.nodes() ) {
      PartitionID id = utils::Randomize::instance().getRandomInt(0, k - 1, cpu_id);
      hypergraph.setNodePart(hn, id);
    }
    hypergraph.updateGlobalPartInfos();
  }

  static void SetUpTestSuite() {
    TBBNumaArena::instance(std::thread::hardware_concurrency());
    utils::Randomize::instance().setSeed(0);
  }

  PartitionID k;
  size_t num_threads;
  mt_kahypar::Hypergraph hypergraph;
};

typedef ::testing::Types<TestConfig<2,   1>,
                         TestConfig<2,   2>,
                         TestConfig<2,   4>,
                         TestConfig<4,   1>,
                         TestConfig<4,   2>,
                         TestConfig<4,   4>,
                         TestConfig<8,   1>,
                         TestConfig<8,   2>,
                         TestConfig<8,   4>,
                         TestConfig<16,  1>,
                         TestConfig<16,  2>,
                         TestConfig<16,  4>,
                         TestConfig<32,  1>,
                         TestConfig<32,  2>,
                         TestConfig<32,  4>,
                         TestConfig<64,  1>,
                         TestConfig<64,  2>,
                         TestConfig<64,  4>,
                         TestConfig<128, 1>,
                         TestConfig<128, 2>,
                         TestConfig<128, 4> > TestConfigs;

TYPED_TEST_CASE(AConcurrentHypergraph,
                TestConfigs);

void moveAllNodesOfHypergraphRandom(mt_kahypar::Hypergraph& hypergraph,
                                    const PartitionID k,
                                    const size_t num_threads,
                                    const bool show_timings) {
  tbb::task_arena arena(num_threads, 0);
  tbb::task_group group;

  HighResClockTimepoint start = std::chrono::high_resolution_clock::now();
  arena.execute([&] {
    group.run([&] {
      tbb::parallel_for(tbb::blocked_range<HypernodeID>(0UL, hypergraph.initialNumNodes()),
        [&](const tbb::blocked_range<HypernodeID>& range) {
          int cpu_id = sched_getcpu();
          for ( HypernodeID node = range.begin(); node < range.end(); ++node  ) {
            const HypernodeID hn = hypergraph.globalNodeID(node);
            const PartitionID from = hypergraph.partID(hn);
            PartitionID to = -1;
            while ( to == -1 || to == from ) {
              to = utils::Randomize::instance().getRandomInt(0, k - 1, cpu_id);
            }
            ASSERT( ( to >= 0 && to < k ) && to != from );
            hypergraph.changeNodePart(hn, from, to);
          }
      });
    });
  });

  arena.execute([&] {
    group.wait();
  });
  HighResClockTimepoint end = std::chrono::high_resolution_clock::now();
  double timing = std::chrono::duration<double>(end - start).count();
  if ( show_timings ) {
    LOG << V(k) << V(num_threads) << V(timing);
  }
  hypergraph.updateGlobalPartInfos();
}

void verifyBlockWeightsAndSizes(mt_kahypar::Hypergraph& hypergraph,
                                const PartitionID k) {
  std::vector<HypernodeWeight> block_weight(k, 0);
  std::vector<size_t> block_size(k, 0);
  for ( const HypernodeID& hn : hypergraph.nodes() ) {
    block_weight[hypergraph.partID(hn)] += hypergraph.nodeWeight(hn);
    ++block_size[hypergraph.partID(hn)];
  }

  for ( PartitionID i = 0; i < k; ++i ) {
    ASSERT_EQ(block_weight[i], hypergraph.partWeight(i));
    ASSERT_EQ(block_size[i], hypergraph.partSize(i));
  }
}

void verifyPinCountsInParts(mt_kahypar::Hypergraph& hypergraph,
                            const PartitionID k) {
  for ( const HyperedgeID& he : hypergraph.edges() ) {
    std::vector<HypernodeID> pin_count_in_part(k, 0);
    for ( const HypernodeID& pin : hypergraph.pins(he) ) {
      ++pin_count_in_part[hypergraph.partID(pin)];
    }

    for ( PartitionID i = 0; i < k; ++i )  {
      ASSERT_EQ(pin_count_in_part[i], hypergraph.pinCountInPart(he, i));
    }
  }
}

void verifyConnectivitySet(mt_kahypar::Hypergraph& hypergraph,
                           const PartitionID k) {
  for ( const HyperedgeID& he : hypergraph.edges() ) {
    std::vector<HypernodeID> pin_count_in_part(k, 0);
    std::set<PartitionID> connectivity_set;
    for ( const HypernodeID& pin : hypergraph.pins(he) ) {
      PartitionID id = hypergraph.partID(pin);
      HypernodeID pin_count_before = pin_count_in_part[id]++;
      if ( pin_count_before == 0 ) {
        connectivity_set.insert(id);
      }
    }

    ASSERT_EQ(connectivity_set.size(), hypergraph.connectivity(he));
    size_t connectivity = 0;
    for ( const PartitionID id : hypergraph.connectivitySet(he) ) {
      ASSERT_TRUE( connectivity_set.find(id) != connectivity_set.end() );
      ++connectivity;
    }
    ASSERT_EQ(connectivity_set.size(), connectivity);
  }
}

TYPED_TEST(AConcurrentHypergraph, SmokeTest) {
  moveAllNodesOfHypergraphRandom(this->hypergraph, this->k, this->num_threads, false);
  verifyBlockWeightsAndSizes(this->hypergraph, this->k);
  verifyPinCountsInParts(this->hypergraph, this->k);
  verifyConnectivitySet(this->hypergraph, this->k);
}


} // namespace ds
} // namespace mt_kahypar