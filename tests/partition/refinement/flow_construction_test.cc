/*******************************************************************************
 * This file is part of KaHyPar.
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

#include "gmock/gmock.h"

#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/io/hypergraph_io.h"
#include "mt-kahypar/partition/refinement/flows/sequential_construction.h"
#include "mt-kahypar/partition/refinement/flows/parallel_construction.h"
#include "mt-kahypar/partition/refinement/flows/flow_common.h"

using ::testing::Test;

#define NODE(X) whfc::Node(X)
#define CAPACITY(X) whfc::Flow(X)

namespace mt_kahypar {

template<typename C, bool is_default>
struct Config {
  using Constructor = C;
  static bool is_default_construction() {
    return is_default;
  }
};

template<typename Configuration>
class AFlowHypergraphConstructor : public Test {

  using Constructor = typename Configuration::Constructor;

 public:
  AFlowHypergraphConstructor() :
    hg(HypergraphFactory::construct(10 , 8,
      { {0, 1, 3}, {1, 2, 3}, {4, 5, 6}, {4, 6, 7},
        {1, 3, 4, 6}, {0, 1, 4, 5}, {3, 8}, {6, 9} }, nullptr, nullptr, true)),
    phg(3, hg, parallel_tag_t()),
    context(),
    flow_hg(),
    hfc(flow_hg, 42),
    constructor(nullptr),
    whfc_to_node() {
    context.partition.k = 3;
    context.partition.perfect_balance_part_weights.assign(3, 5);
    context.partition.max_part_weights.assign(2, 4);
    context.partition.objective = kahypar::Objective::km1;

    context.shared_memory.num_threads = 2;
    context.refinement.flows.algorithm = AdvancedRefinementAlgorithm::mock;
    context.refinement.flows.num_threads_per_search = 1;
    context.refinement.flows.determine_distance_from_cut = false;

    phg.setOnlyNodePart(0, 0);
    phg.setOnlyNodePart(1, 0);
    phg.setOnlyNodePart(2, 0);
    phg.setOnlyNodePart(3, 0);
    phg.setOnlyNodePart(4, 1);
    phg.setOnlyNodePart(5, 1);
    phg.setOnlyNodePart(6, 1);
    phg.setOnlyNodePart(7, 1);
    phg.setOnlyNodePart(8, 2);
    phg.setOnlyNodePart(9, 2);
    phg.initializePartition();

    constructor = std::make_unique<Constructor>(hg, flow_hg, hfc, context);
  }

  bool is_default_construction() const {
    return Configuration::is_default_construction();
  }

  Hypergraph hg;
  PartitionedHypergraph phg;
  Context context;

  FlowHypergraphBuilder flow_hg;
  whfc::HyperFlowCutter<whfc::Dinic> hfc;
  std::unique_ptr<Constructor> constructor;
  vec<HypernodeID> whfc_to_node;
};

typedef ::testing::Types<Config<SequentialConstruction, true>, Config<SequentialConstruction, false>,
                         Config<ParallelConstruction, true>, Config<ParallelConstruction, false> > TestConfigs;

TYPED_TEST_CASE(AFlowHypergraphConstructor, TestConfigs);

void constructSubhypergraph(const PartitionedHypergraph& phg,
                            Subhypergraph& sub_hg) {
  vec<bool> visited(phg.initialNumEdges(), false);
  for ( const HypernodeID& hn : sub_hg.nodes_of_block_0 ) {
    sub_hg.num_pins += phg.nodeDegree(hn);
    sub_hg.weight_of_block_0 += phg.nodeWeight(hn);
    for ( const HyperedgeID& he : phg.incidentEdges(hn) ) {
      if ( !visited[he] ) {
        sub_hg.hes.push_back(he);
        visited[he] = true;
      }
    }
  }
  for ( const HypernodeID& hn : sub_hg.nodes_of_block_1 ) {
    sub_hg.num_pins += phg.nodeDegree(hn);
    sub_hg.weight_of_block_1 += phg.nodeWeight(hn);
    for ( const HyperedgeID& he : phg.incidentEdges(hn) ) {
      if ( !visited[he] ) {
        sub_hg.hes.push_back(he);
        visited[he] = true;
      }
    }
  }
}

void verifyFlowProblemStats(const FlowProblem expected_prob, const FlowProblem actual_prob) {
  ASSERT_EQ(expected_prob.source, actual_prob.source);
  ASSERT_EQ(expected_prob.sink, actual_prob.sink);
  ASSERT_EQ(expected_prob.total_cut, actual_prob.total_cut);
  ASSERT_EQ(expected_prob.non_removable_cut, actual_prob.non_removable_cut);
  ASSERT_EQ(expected_prob.weight_of_block_0, actual_prob.weight_of_block_0);
  ASSERT_EQ(expected_prob.weight_of_block_1, actual_prob.weight_of_block_1);
}

struct Hyperedge {
  vec<whfc::Node> pins;
  whfc::Flow capacity;
};

void verifyFlowHypergraph(FlowHypergraphBuilder& flow_hg, const vec<Hyperedge>& tmp_hyperedges) {
  vec<Hyperedge> hyperedges = tmp_hyperedges;
  vec<bool> already_matched_hes(flow_hg.numHyperedges(), false);
  for ( size_t i = 0; i < hyperedges.size(); ++i ) {
    Hyperedge& he = hyperedges[i];
    whfc::Hyperedge found_he = whfc::Hyperedge::Invalid();

    for ( const whfc::Hyperedge& whfc_he : flow_hg.hyperedgeIDs() ) {
      if ( !already_matched_hes[whfc_he] &&
           flow_hg.pinCount(whfc_he) == he.pins.size() ) {
        size_t idx = 0;
        bool equal = true;
        for ( const auto& p : flow_hg.pinsOf(whfc_he) ) {
          if ( p.pin != he.pins[idx++] ) {
            equal = false;
            break;
          }
        }
        if ( equal ) {
          found_he = whfc_he;
          break;
        }
      }
    }

    if ( found_he != whfc::Hyperedge::Invalid() ) {
      ASSERT_EQ(flow_hg.capacity(found_he), he.capacity);
      he.capacity -= flow_hg.capacity(found_he);
      already_matched_hes[found_he] = true;
      if ( he.capacity > 0 ) {
        --i;
      }
    } else {
      LOG << "Hyperedge not found:";
      for ( const whfc::Node& pin : he.pins ) {
        std::cout << pin << " ";
      }
      std::cout << std::endl;
      ASSERT_TRUE(false);
    }
  }
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithTwoHypernodes1) {
  Subhypergraph sub_hg { 0, 1, {1}, {4}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(2), 2, 2, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 4);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 2);
  ASSERT_EQ(this->flow_hg.numPins(), 4);

  ASSERT_EQ(this->whfc_to_node[1], 1);
  ASSERT_EQ(this->whfc_to_node[3], 4);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(0), NODE(1) }, CAPACITY(2) }, { { NODE(2), NODE(3) }, CAPACITY(2) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithTwoHypernodes2) {
  Subhypergraph sub_hg { 0, 1, {3}, {6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(2), 1, 1, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 4);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 2);
  ASSERT_EQ(this->flow_hg.numPins(), 4);

  ASSERT_EQ(this->whfc_to_node[1], 3);
  ASSERT_EQ(this->whfc_to_node[3], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(0), NODE(1) }, CAPACITY(2) }, { { NODE(2), NODE(3) }, CAPACITY(2) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithThreeHypernodes1) {
  Subhypergraph sub_hg { 0, 1, {1, 3}, {4}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(3), 2, 1, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 5);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 3);
  ASSERT_EQ(this->flow_hg.numPins(), 9);

  ASSERT_EQ(this->whfc_to_node[1], 1);
  ASSERT_EQ(this->whfc_to_node[2], 3);
  ASSERT_EQ(this->whfc_to_node[4], 4);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(0), NODE(1), NODE(2) }, CAPACITY(2) },
      { { NODE(3), NODE(4) }, CAPACITY(2) },
      { { NODE(3), NODE(1), NODE(2), NODE(4) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithThreeHypernodes2) {
  Subhypergraph sub_hg { 0, 1, {1}, {4, 6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(2), 2, 1, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 5);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 3);
  ASSERT_EQ(this->flow_hg.numPins(), 9);

  ASSERT_EQ(this->whfc_to_node[1], 1);
  ASSERT_EQ(this->whfc_to_node[3], 4);
  ASSERT_EQ(this->whfc_to_node[4], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(0), NODE(1) }, CAPACITY(2) },
      { { NODE(2), NODE(3), NODE(4) }, CAPACITY(2) },
      { { NODE(0), NODE(1), NODE(3), NODE(4) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithFourHypernodes) {
  Subhypergraph sub_hg { 0, 1, {1, 3}, {4, 6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(3), 2, 1, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 6);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 3);
  ASSERT_EQ(this->flow_hg.numPins(), 10);

  ASSERT_EQ(this->whfc_to_node[1], 1);
  ASSERT_EQ(this->whfc_to_node[2], 3);
  ASSERT_EQ(this->whfc_to_node[4], 4);
  ASSERT_EQ(this->whfc_to_node[5], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(0), NODE(1), NODE(2) }, CAPACITY(2) },
      { { NODE(3), NODE(4), NODE(5) }, CAPACITY(2) },
      { { NODE(1), NODE(2), NODE(4), NODE(5) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithFiveHypernodes1) {
  Subhypergraph sub_hg { 0, 1, {0, 1, 3}, {4, 6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(4), 2, 0, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 7);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 5);
  ASSERT_EQ(this->flow_hg.numPins(), 17);

  ASSERT_EQ(this->whfc_to_node[1], 0);
  ASSERT_EQ(this->whfc_to_node[2], 1);
  ASSERT_EQ(this->whfc_to_node[3], 3);
  ASSERT_EQ(this->whfc_to_node[5], 4);
  ASSERT_EQ(this->whfc_to_node[6], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(1), NODE(2), NODE(3) }, CAPACITY(1) },
      { { NODE(0), NODE(2), NODE(3) }, CAPACITY(1) },
      { { NODE(2), NODE(3), NODE(5), NODE(6) }, CAPACITY(1) },
      { { NODE(4), NODE(5), NODE(6) }, CAPACITY(2) },
      { { NODE(4), NODE(1), NODE(2), NODE(5) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithFiveHypernodes2) {
  Subhypergraph sub_hg { 0, 1, {1, 3}, {4, 5, 6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(3), 2, 0, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 7);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 5);
  ASSERT_EQ(this->flow_hg.numPins(), 17);

  ASSERT_EQ(this->whfc_to_node[1], 1);
  ASSERT_EQ(this->whfc_to_node[2], 3);
  ASSERT_EQ(this->whfc_to_node[4], 4);
  ASSERT_EQ(this->whfc_to_node[5], 5);
  ASSERT_EQ(this->whfc_to_node[6], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(4), NODE(5), NODE(6) }, CAPACITY(1) },
      { { NODE(3), NODE(4), NODE(6) }, CAPACITY(1) },
      { { NODE(1), NODE(2), NODE(4), NODE(6) }, CAPACITY(1) },
      { { NODE(0), NODE(1), NODE(2) }, CAPACITY(2) },
      { { NODE(0), NODE(1), NODE(4), NODE(5) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithSixHypernodes) {
  Subhypergraph sub_hg { 0, 1, {0, 1, 3}, {4, 5, 6}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(4), 2, 0, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);

  ASSERT_EQ(this->flow_hg.numNodes(), 8);
  ASSERT_EQ(this->flow_hg.numHyperedges(), 6);
  ASSERT_EQ(this->flow_hg.numPins(), 20);

  ASSERT_EQ(this->whfc_to_node[1], 0);
  ASSERT_EQ(this->whfc_to_node[2], 1);
  ASSERT_EQ(this->whfc_to_node[3], 3);
  ASSERT_EQ(this->whfc_to_node[5], 4);
  ASSERT_EQ(this->whfc_to_node[6], 5);
  ASSERT_EQ(this->whfc_to_node[7], 6);

  verifyFlowHypergraph(this->flow_hg,
    { { { NODE(1), NODE(2), NODE(3) }, CAPACITY(1) },
      { { NODE(0), NODE(2), NODE(3) }, CAPACITY(1) },
      { { NODE(2), NODE(3), NODE(5), NODE(7) }, CAPACITY(1) },
      { { NODE(5), NODE(6), NODE(7) }, CAPACITY(1) },
      { { NODE(4), NODE(5), NODE(7) }, CAPACITY(1) },
      { { NODE(1), NODE(2), NODE(5), NODE(6) }, CAPACITY(1) } });
}

TYPED_TEST(AFlowHypergraphConstructor, ConstructsAFlowHypergraphWithAllHypernodes) {
  Subhypergraph sub_hg { 0, 1, {0, 1, 2, 3}, {4, 5, 6, 7}, 0, 0, {}, 0 };
  constructSubhypergraph(this->phg, sub_hg);

  FlowProblem actual_prob = this->constructor->constructFlowHypergraph(
    this->phg, sub_hg, 0, 1, this->whfc_to_node, this->is_default_construction());
  FlowProblem expected_prob { NODE(0), NODE(5), 0, 0, 4, 4 };
  verifyFlowProblemStats(expected_prob, actual_prob);
}

}