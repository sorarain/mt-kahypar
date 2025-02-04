/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2021 Nikolai Maas <nikolai.maas@student.kit.edu>
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

#include "mt-kahypar/partition/deep_multilevel.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "tbb/parallel_invoke.h"
#include "mt-kahypar/macros.h"
#include "mt-kahypar/partition/multilevel.h"
#include "mt-kahypar/partition/coarsening/multilevel_uncoarsener.h"
#include "mt-kahypar/partition/coarsening/nlevel_uncoarsener.h"
#include "mt-kahypar/partition/initial_partitioning/pool_initial_partitioner.h"
#include "mt-kahypar/partition/preprocessing/sparsification/degree_zero_hn_remover.h"
#include "mt-kahypar/utils/randomize.h"
#include "mt-kahypar/utils/utilities.h"
#include "mt-kahypar/utils/timer.h"
#include "mt-kahypar/io/partitioning_output.h"

namespace mt_kahypar {

namespace deep_multilevel {

namespace {

static constexpr bool enable_heavy_assert = false;
static constexpr bool debug = false;

struct DeepPartitioningResult {
  Hypergraph hypergraph;
  PartitionedHypergraph partitioned_hg;
  bool valid = false;
};

struct OriginalHypergraphInfo {

  // The initial allowed imbalance cannot be used for each bipartition as this could result in an
  // imbalanced k-way partition when performing recursive bipartitioning. We therefore adaptively
  // adjust the allowed imbalance for each bipartition individually based on the adaptive imbalance
  // definition described in our papers.
  double computeAdaptiveEpsilon(const HypernodeWeight current_hypergraph_weight,
                                const PartitionID current_k) const {
    if ( current_hypergraph_weight == 0 ) {
      // In recursive bipartitioning, it can happen that a block becomes too light that
      // all nodes of the block fit into one block in a subsequent bipartitioning step.
      // This will create an empty block, which we fix later in a rebalancing step.
      return 0.0;
    } else {
      double base = ceil(static_cast<double>(original_hypergraph_weight) / original_k)
        / ceil(static_cast<double>(current_hypergraph_weight) / current_k)
        * (1.0 + original_epsilon);
      double adaptive_epsilon = std::min(0.99, std::max(std::pow(base, 1.0 /
        ceil(log2(static_cast<double>(current_k)))) - 1.0,0.0));
      return adaptive_epsilon;
    }
  }

  const HypernodeWeight original_hypergraph_weight;
  const PartitionID original_k;
  const double original_epsilon;
};

// During uncoarsening in the deep multilevel scheme, we recursively bipartition each block of the
// partition until we reach the desired number of blocks. The recursive bipartitioning tree (RBTree)
// contains for each partition information in how many blocks we have to further bipartition each block,
// the range of block IDs in the final partition of each block, and the perfectly balanced and maximum
// allowed block weight for each block.
class RBTree {

 public:
  explicit RBTree(const Context& context) :
    _contraction_limit_multiplier(context.coarsening.contraction_limit_multiplier),
    _desired_blocks(),
    _target_blocks(),
    _perfectly_balanced_weights(),
    _max_part_weights(),
    _partition_to_level() {
    _desired_blocks.emplace_back();
    _desired_blocks[0].push_back(context.partition.k);
    _target_blocks.emplace_back();
    _target_blocks[0].push_back(0);
    _target_blocks[0].push_back(context.partition.k);
    _perfectly_balanced_weights.emplace_back();
    _perfectly_balanced_weights[0].push_back(
      std::accumulate(context.partition.perfect_balance_part_weights.cbegin(),
        context.partition.perfect_balance_part_weights.cend(), 0));
    _max_part_weights.emplace_back();
    _max_part_weights[0].push_back(
      std::accumulate(context.partition.max_part_weights.cbegin(),
        context.partition.max_part_weights.cend(), 0));
    precomputeRBTree(context);
  }

  PartitionID nextK(const PartitionID k) const {
    const PartitionID original_k = _desired_blocks[0][0];
    if ( k < original_k && k != kInvalidPartition ) {
      ASSERT(_partition_to_level.count(k) > 0);
      const size_t level = _partition_to_level.at(k);
      if ( level + 1 < _desired_blocks.size() ) {
        return _desired_blocks[level + 1].size();
      } else {
        return original_k;
      }
    } else {
      return kInvalidPartition;
    }
  }

  PartitionID desiredNumberOfBlocks(const PartitionID current_k,
                                    const PartitionID block) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    ASSERT(block < current_k);
    return _desired_blocks[_partition_to_level.at(current_k)][block];
  }

  std::pair<PartitionID, PartitionID> targetBlocksInFinalPartition(const PartitionID current_k,
                                                                   const PartitionID block) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    ASSERT(block < current_k);
    const vec<PartitionID>& target_blocks =
      _target_blocks[_partition_to_level.at(current_k)];
    return std::make_pair(target_blocks[block], target_blocks[block + 1]);
  }

  HypernodeWeight perfectlyBalancedWeight(const PartitionID current_k,
                                          const PartitionID block) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    ASSERT(block < current_k);
    return _perfectly_balanced_weights[_partition_to_level.at(current_k)][block];
  }

  const std::vector<HypernodeWeight>& perfectlyBalancedWeightVector(const PartitionID current_k) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    return _perfectly_balanced_weights[_partition_to_level.at(current_k)];
  }

  HypernodeWeight maxPartWeight(const PartitionID current_k,
                                const PartitionID block) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    ASSERT(block < current_k);
    return _max_part_weights[_partition_to_level.at(current_k)][block];
  }

  const std::vector<HypernodeWeight>& maxPartWeightVector(const PartitionID current_k) const {
    ASSERT(_partition_to_level.count(current_k) > 0);
    return _max_part_weights[_partition_to_level.at(current_k)];
  }

  PartitionID get_maximum_number_of_blocks(const HypernodeID current_num_nodes) const {
    const int num_levels = _desired_blocks.size();
    for ( int i = num_levels - 1; i >= 0; --i ) {
      const PartitionID k = _desired_blocks[i].size();
      if ( current_num_nodes >= k * _contraction_limit_multiplier ) {
        return k;
      }
    }
    return _desired_blocks.back().size();
  }

  void printRBTree() const {
    for ( size_t level = 0; level < _desired_blocks.size(); ++level ) {
      std::cout << "Level " << (level + 1) << std::endl;
      for ( size_t i = 0; i <  _desired_blocks[level].size(); ++i) {
        std::cout << "(" << _desired_blocks[level][i]
                  << ", [" << _target_blocks[level][i] << "," << _target_blocks[level][i + 1] << "]"
                  << ", " << _perfectly_balanced_weights[level][i]
                  << ", " << _max_part_weights[level][i] << ") ";
      }
      std::cout << std::endl;
    }
  }

 private:
  void precomputeRBTree(const Context& context) {
    auto add_block = [&](const PartitionID k) {
      const PartitionID start = _target_blocks.back().back();
      _desired_blocks.back().push_back(k);
      _target_blocks.back().push_back(start + k);
      const HypernodeWeight perfect_part_weight = std::accumulate(
        context.partition.perfect_balance_part_weights.cbegin() + start,
        context.partition.perfect_balance_part_weights.cbegin() + start + k, 0);
      const HypernodeWeight max_part_weight = std::accumulate(
        context.partition.max_part_weights.cbegin() + start,
        context.partition.max_part_weights.cbegin() + start + k, 0);
      _perfectly_balanced_weights.back().push_back(perfect_part_weight);
      _max_part_weights.back().push_back(max_part_weight);
    };

    int cur_level = 0;
    bool should_continue = true;
    // Simulates recursive bipartitioning
    while ( should_continue ) {
      should_continue = false;
      _desired_blocks.emplace_back();
      _target_blocks.emplace_back();
      _target_blocks.back().push_back(0);
      _perfectly_balanced_weights.emplace_back();
      _max_part_weights.emplace_back();
      for ( size_t i = 0; i < _desired_blocks[cur_level].size(); ++i ) {
        const PartitionID k = _desired_blocks[cur_level][i];
        if ( k > 1 ) {
          const PartitionID k0 = k / 2 + (k % 2);
          const PartitionID k1 = k / 2;
          add_block(k0);
          add_block(k1);
          should_continue |= ( k0 > 1 || k1 > 1 );
        } else {
          add_block(1);
        }
      }
      ++cur_level;
    }

    for ( size_t i = 0; i < _desired_blocks.size(); ++i ) {
      _partition_to_level[_desired_blocks[i].size()] = i;
    }
  }

  const HypernodeID _contraction_limit_multiplier;
  vec<vec<PartitionID>> _desired_blocks;
  vec<vec<PartitionID>> _target_blocks;
  vec<std::vector<HypernodeWeight>> _perfectly_balanced_weights;
  vec<std::vector<HypernodeWeight>> _max_part_weights;
  std::unordered_map<PartitionID, size_t> _partition_to_level;
};

void disableTimerAndStats(const Context& context) {
  if ( context.type == ContextType::main ) {
    utils::Utilities& utils = utils::Utilities::instance();
    parallel::MemoryPool::instance().deactivate_unused_memory_allocations();
    utils.getTimer(context.utility_id).disable();
    utils.getStats(context.utility_id).disable();
  }
}

void enableTimerAndStats(const Context& context) {
  if ( context.type == ContextType::main ) {
    utils::Utilities& utils = utils::Utilities::instance();
    parallel::MemoryPool::instance().activate_unused_memory_allocations();
    utils.getTimer(context.utility_id).enable();
    utils.getStats(context.utility_id).enable();
  }
}

Context setupBipartitioningContext(const Hypergraph& hypergraph,
                                   const Context& context,
                                   const OriginalHypergraphInfo& info,
                                   const PartitionID start_k,
                                   const PartitionID end_k) {
  ASSERT(end_k - start_k >= 2);
  Context b_context(context);

  b_context.partition.k = 2;
  b_context.initial_partitioning.mode = Mode::direct;
  if (context.partition.mode == Mode::direct) {
    b_context.type = ContextType::initial_partitioning;
  }

  // Setup Part Weights
  const HypernodeWeight total_weight = hypergraph.totalWeight();
  const PartitionID k = end_k - start_k;
  const PartitionID k0 = k / 2 + (k % 2 != 0 ? 1 : 0);
  const PartitionID k1 = k / 2;
  ASSERT(k0 + k1 == k);
  if ( context.partition.use_individual_part_weights ) {
    const HypernodeWeight max_part_weights_sum = std::accumulate(
      context.partition.max_part_weights.cbegin() + start_k, context.partition.max_part_weights.cbegin() + end_k, 0);
    const double weight_fraction = total_weight / static_cast<double>(max_part_weights_sum);
    ASSERT(weight_fraction <= 1.0);
    b_context.partition.perfect_balance_part_weights.clear();
    b_context.partition.max_part_weights.clear();
    HypernodeWeight perfect_weight_p0 = 0;
    for ( PartitionID i = start_k; i < start_k + k0; ++i ) {
      perfect_weight_p0 += ceil(weight_fraction * context.partition.max_part_weights[i]);
    }
    HypernodeWeight perfect_weight_p1 = 0;
    for ( PartitionID i = start_k + k0; i < end_k; ++i ) {
      perfect_weight_p1 += ceil(weight_fraction * context.partition.max_part_weights[i]);
    }
    // In the case of individual part weights, the usual adaptive epsilon formula is not applicable because it
    // assumes equal part weights. However, by observing that ceil(current_weight / current_k) is the current
    // perfect part weight and (1 + epsilon)ceil(original_weight / original_k) is the maximum part weight,
    // we can derive an equivalent formula using the sum of the perfect part weights and the sum of the
    // maximum part weights.
    // Note that the sum of the perfect part weights might be unequal to the hypergraph weight due to rounding.
    // Thus, we need to use the former instead of using the hypergraph weight directly, as otherwise it could
    // happen that (1 + epsilon)perfect_part_weight > max_part_weight because of rounding issues.
    const double base = max_part_weights_sum / static_cast<double>(perfect_weight_p0 + perfect_weight_p1);
    b_context.partition.epsilon = total_weight == 0 ? 0 :
      std::min(0.99, std::max(std::pow(base, 1.0 / ceil(log2(static_cast<double>(k)))) - 1.0,0.0));
    b_context.partition.perfect_balance_part_weights.push_back(perfect_weight_p0);
    b_context.partition.perfect_balance_part_weights.push_back(perfect_weight_p1);
    b_context.partition.max_part_weights.push_back(
            round((1 + b_context.partition.epsilon) * perfect_weight_p0));
    b_context.partition.max_part_weights.push_back(
            round((1 + b_context.partition.epsilon) * perfect_weight_p1));
  } else {
    b_context.partition.epsilon = info.computeAdaptiveEpsilon(total_weight, k);

    b_context.partition.perfect_balance_part_weights.clear();
    b_context.partition.max_part_weights.clear();
    b_context.partition.perfect_balance_part_weights.push_back(
            std::ceil(k0 / static_cast<double>(k) * static_cast<double>(total_weight)));
    b_context.partition.perfect_balance_part_weights.push_back(
            std::ceil(k1 / static_cast<double>(k) * static_cast<double>(total_weight)));
    b_context.partition.max_part_weights.push_back(
            (1 + b_context.partition.epsilon) * b_context.partition.perfect_balance_part_weights[0]);
    b_context.partition.max_part_weights.push_back(
            (1 + b_context.partition.epsilon) * b_context.partition.perfect_balance_part_weights[1]);
  }
  b_context.setupContractionLimit(total_weight);
  b_context.setupThreadsPerFlowSearch();

  return b_context;
}

Context setupDeepMultilevelRecursionContext(const Context& context,
                                            const size_t num_threads) {
  Context r_context(context);

  r_context.type = ContextType::initial_partitioning;
  r_context.partition.verbose_output = false;

  const double thread_reduction_factor = static_cast<double>(num_threads) / context.shared_memory.num_threads;
  r_context.shared_memory.num_threads = num_threads;
  r_context.shared_memory.degree_of_parallelism *= thread_reduction_factor;
  r_context.initial_partitioning.runs = std::max(
    std::ceil(static_cast<double>(context.initial_partitioning.runs) *
      thread_reduction_factor), 1.0);

  return r_context;
}

// The current number of blocks are the first k' blocks with non-zero weight
PartitionID get_current_k(const PartitionedHypergraph& partitioned_hg) {
  PartitionID k = 0;
  for ( PartitionID i = 0; i < partitioned_hg.k(); ++i ) {
    if ( partitioned_hg.partWeight(i) > 0 ) ++k;
    else break;
  }
  return k;
}

void printInitialPartitioningResult(const PartitionedHypergraph& partitioned_hg,
                                    const Context& context,
                                    const RBTree& rb_tree) {
  if ( context.partition.verbose_output ) {
    Context m_context(context);
    m_context.partition.k = get_current_k(partitioned_hg);
    m_context.partition.perfect_balance_part_weights = rb_tree.perfectlyBalancedWeightVector(m_context.partition.k);
    m_context.partition.max_part_weights = rb_tree.maxPartWeightVector(m_context.partition.k);
    io::printPartitioningResults(partitioned_hg, m_context, "Initial Partitioning Results:");
  }
}

bool is_balanced(const PartitionedHypergraph& partitioned_hg,
                 const RBTree& rb_tree) {
  const PartitionID k = get_current_k(partitioned_hg);
  bool isBalanced = true;
  for ( PartitionID i = 0; i < k; ++i ) {
    isBalanced = isBalanced && partitioned_hg.partWeight(i) <= rb_tree.maxPartWeight(k, i);
  }
  return isBalanced;
}

const DeepPartitioningResult& select_best_partition(const vec<DeepPartitioningResult>& partitions,
                                                    const Context& context,
                                                    const RBTree& rb_tree) {
  vec<HyperedgeWeight> objectives(partitions.size(), 0);
  vec<bool> isBalanced(partitions.size(), false);

  // Compute objective value and perform balance check for each partition
  tbb::task_group tg;
  for ( size_t i = 0; i < partitions.size(); ++i ) {
    tg.run([&, i] {
      objectives[i] = metrics::objective(
        partitions[i].partitioned_hg, context.partition.objective);
      isBalanced[i] = is_balanced(partitions[i].partitioned_hg, rb_tree);
    });
  }
  tg.wait();

  // We try to choose a balanced partition with the best objective value
  size_t best_idx = 0;
  for ( size_t i = 1; i < partitions.size(); ++i ) {
    if ( ( isBalanced[i] && !isBalanced[best_idx] ) ||
         ( ( ( !isBalanced[i] && !isBalanced[best_idx] ) ||
             ( isBalanced[i] && isBalanced[best_idx] ) ) &&
           objectives[i] < objectives[best_idx] ) ) {
      best_idx = i;
    }
  }

  return partitions[best_idx];
}

DeepPartitioningResult bipartition_block(PartitionedHypergraph& partitioned_hg,
                                         const Context& context,
                                         const OriginalHypergraphInfo& info,
                                         const PartitionID block,
                                         vec<HypernodeID>& mapping,
                                         const PartitionID start_k,
                                         const PartitionID end_k) {
  DeepPartitioningResult bipartition;

  // Extract subhypergraph representing the corresponding block
  const bool cut_net_splitting = context.partition.objective == Objective::km1;
  bipartition.hypergraph = partitioned_hg.extract(block, mapping,
    cut_net_splitting, context.preprocessing.stable_construction_of_incident_edges);
  bipartition.partitioned_hg = PartitionedHypergraph(2, bipartition.hypergraph, parallel_tag_t());
  bipartition.valid = true;

  if ( bipartition.hypergraph.initialNumNodes() > 0 ) {
    // Bipartition block
    Context b_context = setupBipartitioningContext(
      bipartition.hypergraph, context, info, start_k, end_k);
    pool::bipartition(bipartition.partitioned_hg, b_context);
  }

  return bipartition;
}

void bipartition_each_block(PartitionedHypergraph& partitioned_hg,
                            const Context& context,
                            const OriginalHypergraphInfo& info,
                            const RBTree& rb_tree,
                            const PartitionID current_k) {
  vec<DeepPartitioningResult> bipartitions(current_k);
  vec<PartitionID> block_ranges(1, 0);
  vec<HypernodeID> mapping(partitioned_hg.initialNumNodes(), kInvalidHypernode);
  tbb::task_group tg;
  for ( PartitionID block = 0; block < current_k; ++block ) {
    // The recursive bipartitioning tree stores for each block of the current partition
    // the number of blocks in which we have to further bipartition the corresponding block
    // recursively. This is important for computing the adjusted imbalance factor to ensure
    // that the final k-way partition is balanced.
    const PartitionID desired_blocks = rb_tree.desiredNumberOfBlocks(current_k, block);
    if ( desired_blocks > 1 ) {
      // Spawn a task that bipartitions the corresponding block
      tg.run([&, block, desired_blocks] {
        const auto target_blocks = rb_tree.targetBlocksInFinalPartition(current_k, block);
        bipartitions[block] = bipartition_block(partitioned_hg, context,
          info, block, mapping, target_blocks.first, target_blocks.second);
        bipartitions[block].partitioned_hg.setHypergraph(bipartitions[block].hypergraph);
      });
      block_ranges.push_back(block_ranges.back() + 2);
    } else {
      // No further bipartitions required for the corresponding block
      bipartitions[block].valid = false;
      block_ranges.push_back(block_ranges.back() + 1);
    }
  }
  tg.wait();

  // Apply all bipartitions to current hypergraph
  partitioned_hg.doParallelForAllNodes([&](const HypernodeID& hn) {
    const PartitionID from = partitioned_hg.partID(hn);
    ASSERT(static_cast<size_t>(from) < bipartitions.size());
    PartitionID to = kInvalidPartition;
    const DeepPartitioningResult& bipartition = bipartitions[from];
    if ( bipartition.valid ) {
      ASSERT(static_cast<size_t>(hn) < mapping.size());
      const HypernodeID mapped_hn = mapping[hn];
      to = bipartition.partitioned_hg.partID(mapped_hn) == 0 ?
        block_ranges[from] : block_ranges[from] + 1;
    } else {
      to = block_ranges[from];
    }

    ASSERT(to > kInvalidPartition && to < block_ranges.back());
    if ( from != to ) {
      if ( partitioned_hg.isGainCacheInitialized() ) {
        partitioned_hg.changeNodePartWithGainCacheUpdate(hn, from, to);
      } else {
        partitioned_hg.changeNodePart(hn, from, to);
      }
    }
  });

  if ( partitioned_hg.isGainCacheInitialized() ) {
    partitioned_hg.doParallelForAllNodes([&](const HypernodeID& hn) {
      partitioned_hg.recomputeMoveFromPenalty(hn);
    });
  }

  HEAVY_REFINEMENT_ASSERT(partitioned_hg.checkTrackedPartitionInformation());
}

DeepPartitioningResult deep_multilevel_recursion(const Hypergraph& hypergraph,
                                                 const Context& context,
                                                 const OriginalHypergraphInfo& info,
                                                 const RBTree& rb_tree,
                                                 const size_t num_threads);

void deep_multilevel_partitioning(PartitionedHypergraph& partitioned_hg,
                                  const Context& c,
                                  const OriginalHypergraphInfo& info,
                                  const RBTree& rb_tree) {
  Hypergraph& hypergraph = partitioned_hg.hypergraph();
  Context context(c);

  // ################## COARSENING ##################
  mt_kahypar::io::printCoarseningBanner(context);

  // We change the contraction limit to 2C nodes which is the contraction limit where traditional
  // multilevel partitioning bipartitions the smallest hypergraph into two blocks.
  const HypernodeID contraction_limit_for_bipartitioning = 2 * context.coarsening.contraction_limit_multiplier;
  context.coarsening.contraction_limit = contraction_limit_for_bipartitioning;
  PartitionID actual_k = std::max(std::min(static_cast<HypernodeID>(context.partition.k),
    partitioned_hg.initialNumNodes() / context.coarsening.contraction_limit_multiplier), ID(2));
  auto adapt_max_allowed_node_weight = [&](const HypernodeID current_num_nodes, bool& should_continue) {
    // In case our actual k is not two, we check if the current number of nodes is smaller
    // than k * contraction_limit. If so, we increase the maximum allowed node weight.
    while ( ( current_num_nodes <= actual_k * context.coarsening.contraction_limit ||
              !should_continue ) && actual_k > 2 ) {
      actual_k = std::max(actual_k / 2, 2);
      const double hypernode_weight_fraction = context.coarsening.max_allowed_weight_multiplier /
          static_cast<double>(actual_k * context.coarsening.contraction_limit_multiplier);
      context.coarsening.max_allowed_node_weight = std::ceil(hypernode_weight_fraction * hypergraph.totalWeight());
      should_continue = true;
      DBG << "Set max allowed node weight to" << context.coarsening.max_allowed_node_weight
          << "( Current Number of Nodes =" << current_num_nodes << ")";
    }
  };

  const bool nlevel = context.coarsening.algorithm == CoarseningAlgorithm::nlevel_coarsener;
  UncoarseningData uncoarseningData(nlevel, hypergraph, context);
  uncoarseningData.setPartitionedHypergraph(std::move(partitioned_hg));

  utils::Timer& timer = utils::Utilities::instance().getTimer(context.utility_id);
  bool no_further_contractions_possible = true;
  bool should_continue = true;
  adapt_max_allowed_node_weight(hypergraph.initialNumNodes(), should_continue);
  timer.start_timer("coarsening", "Coarsening");
  {
    std::unique_ptr<ICoarsener> coarsener = CoarsenerFactory::getInstance().createObject(
      context.coarsening.algorithm, hypergraph, context, uncoarseningData);

    // Perform coarsening
    coarsener->initialize();
    int pass_nr = 1;
    // Coarsening proceeds until we reach the contraction limit (!shouldNotTerminate()) or
    // no further contractions are possible (should_continue)
    while ( coarsener->shouldNotTerminate() && should_continue ) {
      DBG << "Coarsening Pass" << pass_nr
          << "- Number of Nodes =" << coarsener->currentNumberOfNodes()
          << "- Number of HEs =" << (nlevel ? 0 : coarsener->coarsestHypergraph().initialNumEdges())
          << "- Number of Pins =" << (nlevel ? 0 : coarsener->coarsestHypergraph().initialNumPins());

      // In the coarsening phase, we maintain the invariant that t threads process a hypergraph with
      // at least t * C nodes (C = contraction_limit_for_bipartitioning). If this invariant is violated,
      // we terminate coarsening and call the deep multilevel scheme recursively in parallel with the
      // appropriate number of threads to restore the invariant.
      const HypernodeID current_num_nodes = coarsener->currentNumberOfNodes();
      if (  context.partition.perform_parallel_recursion_in_deep_multilevel &&
            current_num_nodes < context.shared_memory.num_threads * contraction_limit_for_bipartitioning ) {
        no_further_contractions_possible = false;
        break;
      }

      should_continue = coarsener->coarseningPass();
      adapt_max_allowed_node_weight(coarsener->currentNumberOfNodes(), should_continue);
      ++pass_nr;
    }
    coarsener->terminate();


    if (context.partition.verbose_output) {
      Hypergraph& coarsestHypergraph = coarsener->coarsestHypergraph();
      mt_kahypar::io::printHypergraphInfo(coarsestHypergraph,
        "Coarsened Hypergraph", context.partition.show_memory_consumption);
    }
  }
  timer.stop_timer("coarsening");

  // ################## Initial Partitioning ##################
  io::printInitialPartitioningBanner(context);
  timer.start_timer("initial_partitioning", "Initial Partitioning");
  PartitionedHypergraph& coarsest_phg = uncoarseningData.coarsestPartitionedHypergraph();
  if ( no_further_contractions_possible ) {
    DBG << "Smallest Hypergraph"
        << "- Number of Nodes =" << coarsest_phg.initialNumNodes()
        << "- Number of HEs =" << coarsest_phg.initialNumEdges()
        << "- Number of Pins =" << coarsest_phg.initialNumPins();

    // If we reach the contraction limit, we bipartition the smallest hypergraph
    // and continue with uncoarsening.
    const auto target_blocks = rb_tree.targetBlocksInFinalPartition(1, 0);
    Context b_context = setupBipartitioningContext(
      hypergraph, context, info, target_blocks.first, target_blocks.second);
    pool::bipartition(coarsest_phg, b_context);

    DBG << BOLD << "Peform Initial Bipartitioning" << END
        << "- Objective =" << metrics::objective(coarsest_phg, b_context.partition.objective)
        << "- Imbalance =" << metrics::imbalance(coarsest_phg, b_context)
        << "- Epsilon =" << b_context.partition.epsilon;
  } else {
    // If we do not reach the contraction limit, then the invariant that t threads
    // work on a hypergraph with at least t * C nodes is violated. To restore the
    // invariant, we call the deep multilevel scheme recursively in parallel. Each
    // recursive call is initialized with the appropriate number of threads. After
    // returning from the recursion, we continue uncoarsening with the best partition
    // from the recursive calls.
    disableTimerAndStats(context);

    // Determine the number of parallel recursive calls and the number of threads
    // used for each recursive call.
    const Hypergraph& coarsest_hg = coarsest_phg.hypergraph();
    const HypernodeID current_num_nodes = coarsest_hg.initialNumNodes();
    size_t num_threads_per_recursion = std::max(current_num_nodes,
      contraction_limit_for_bipartitioning ) / contraction_limit_for_bipartitioning;
    const size_t num_parallel_calls = context.shared_memory.num_threads / num_threads_per_recursion +
      (context.shared_memory.num_threads % num_threads_per_recursion != 0);
    num_threads_per_recursion = context.shared_memory.num_threads / num_parallel_calls +
      (context.shared_memory.num_threads % num_parallel_calls != 0);


    DBG << BOLD << "Perform Parallel Recursion" << END
        << "- Num. Nodes =" << current_num_nodes
        << "- Parallel Calls =" << num_parallel_calls
        << "- Threads Per Call =" << num_threads_per_recursion
        << "- k =" << rb_tree.get_maximum_number_of_blocks(current_num_nodes);

    // Call deep multilevel scheme recursively
    tbb::task_group tg;
    vec<DeepPartitioningResult> results(num_parallel_calls);
    for ( size_t i = 0; i < num_parallel_calls; ++i ) {
      tg.run([&, i] {
        const size_t num_threads = std::min(num_threads_per_recursion,
          context.shared_memory.num_threads - i * num_threads_per_recursion);
        results[i] = deep_multilevel_recursion(coarsest_hg, context, info, rb_tree, num_threads);
        results[i].partitioned_hg.setHypergraph(results[i].hypergraph);
      });
    }
    tg.wait();

    // Apply best bipartition from the recursive calls to the current hypergraph
    const DeepPartitioningResult& best = select_best_partition(results, context, rb_tree);
    const PartitionedHypergraph& best_phg = best.partitioned_hg;
    coarsest_phg.doParallelForAllNodes([&](const HypernodeID& hn) {
      const PartitionID block = best_phg.partID(hn);
      coarsest_phg.setOnlyNodePart(hn, block);
    });
    coarsest_phg.initializePartition();

    DBG << BOLD << "Best Partition from Recursive Calls" << END
        << "- Objective =" << metrics::objective(coarsest_phg, context.partition.objective)
        << "- isBalanced =" << std::boolalpha << is_balanced(coarsest_phg, rb_tree);

    enableTimerAndStats(context);
  }

  printInitialPartitioningResult(coarsest_phg, context, rb_tree);
  if ( context.partition.verbose_output ) {
    utils::Utilities::instance().getInitialPartitioningStats(
      context.utility_id).printInitialPartitioningStats();
  }
  timer.stop_timer("initial_partitioning");

  // ################## UNCOARSENING ##################
  io::printLocalSearchBanner(context);
  timer.start_timer("refinement", "Refinement");
  std::unique_ptr<IUncoarsener> uncoarsener(nullptr);
  if (uncoarseningData.nlevel) {
    uncoarsener = std::make_unique<NLevelUncoarsener>(hypergraph, context, uncoarseningData);
  } else {
    uncoarsener = std::make_unique<MultilevelUncoarsener>(hypergraph, context, uncoarseningData);
  }


  uncoarsener->initialize();

  // Determine the current number of blocks (k), the number of blocks in which the
  // hypergraph should be partitioned next (k'), and the contraction limit at which we
  // have to partition the hypergraph into k' blocks (k' * C).
  const PartitionID final_k = context.partition.k;
  PartitionID current_k = kInvalidPartition;
  PartitionID next_k = kInvalidPartition;
  HypernodeID contraction_limit_for_rb = std::numeric_limits<HypernodeID>::max();
  auto adapt_contraction_limit_for_recursive_bipartitioning = [&](const PartitionID k) {
    current_k = k;
    next_k = rb_tree.nextK(current_k);
    contraction_limit_for_rb = next_k != kInvalidPartition ?
      next_k * context.coarsening.contraction_limit_multiplier :
      std::numeric_limits<HypernodeID>::max();
    context.partition.k = current_k;
    context.partition.perfect_balance_part_weights = rb_tree.perfectlyBalancedWeightVector(current_k);
    context.partition.max_part_weights = rb_tree.maxPartWeightVector(current_k);
    context.setupThreadsPerFlowSearch();
    uncoarsener->updateMetrics();
  };
  adapt_contraction_limit_for_recursive_bipartitioning(
    get_current_k(uncoarseningData.coarsestPartitionedHypergraph()));

  // Start uncoarsening
  while ( !uncoarsener->isTopLevel() ) {
    // In the uncoarsening phase, we recursively bipartition each block when
    // the number of nodes gets larger than k' * C.
    while ( uncoarsener->currentNumberOfNodes() >= contraction_limit_for_rb ) {
      PartitionedHypergraph& current_phg = uncoarsener->currentPartitionedHypergraph();
      bipartition_each_block(current_phg, context, info, rb_tree, current_k);

      ASSERT(get_current_k(current_phg) == next_k);
      DBG << "Increase number of blocks from" << current_k << "to" << next_k
          << "( Number of Nodes =" << current_phg.initialNumNodes()
          << "- Objective =" << metrics::objective(current_phg, context.partition.objective)
          << "- isBalanced =" << std::boolalpha << is_balanced(current_phg, rb_tree);

      adapt_contraction_limit_for_recursive_bipartitioning(next_k);
      // Improve partition
      uncoarsener->refine();
    }

    // Perform next uncontraction step and improve solution
    uncoarsener->projectToNextLevelAndRefine();
  }

  // Top-Level Bipartitioning
  // Note that in case we reach the input hypergraph (ContextType::main) and
  // we still did not reach the desired number of blocks, we recursively bipartition
  // each block until the number of blocks equals the desired number of blocks.
  while ( uncoarsener->currentNumberOfNodes() >= contraction_limit_for_rb ||
          ( context.type == ContextType::main && current_k != final_k ) ) {
    PartitionedHypergraph& current_phg = uncoarsener->currentPartitionedHypergraph();
    bipartition_each_block(current_phg, context, info, rb_tree, current_k);

    ASSERT(get_current_k(current_phg) == next_k);
    DBG << "Increase number of blocks from" << current_k << "to" << next_k
        << "( Num Nodes =" << current_phg.initialNumNodes()
        << "- Objective =" << metrics::objective(current_phg, context.partition.objective)
        << "- isBalanced =" << std::boolalpha << is_balanced(current_phg, rb_tree);

    adapt_contraction_limit_for_recursive_bipartitioning(next_k);
    // Improve partition
    uncoarsener->refine();
  }

  if ( context.type == ContextType::main ) {
    // The choice of the maximum allowed node weight and adaptive imbalance ratio should
    // ensure that we find on each level a balanced partition for unweighted inputs. Thus,
    // we do not use rebalancing on each level as in the original deep multilevel algorithm.
    uncoarsener->rebalancing();
  }

  partitioned_hg = uncoarsener->movePartitionedHypergraph();

  io::printPartitioningResults(partitioned_hg, context, "Local Search Results:");
  timer.stop_timer("refinement");
}

DeepPartitioningResult deep_multilevel_recursion(const Hypergraph& hypergraph,
                                                 const Context& context,
                                                 const OriginalHypergraphInfo& info,
                                                 const RBTree& rb_tree,
                                                 const size_t num_threads) {
  DeepPartitioningResult result;
  Context r_context = setupDeepMultilevelRecursionContext(context, num_threads);
  r_context.partition.k = rb_tree.get_maximum_number_of_blocks(hypergraph.initialNumNodes());
  r_context.partition.perfect_balance_part_weights = rb_tree.perfectlyBalancedWeightVector(r_context.partition.k);
  r_context.partition.max_part_weights = rb_tree.maxPartWeightVector(r_context.partition.k);
  // Copy hypergraph
  result.hypergraph = hypergraph.copy(parallel_tag_t());
  result.partitioned_hg = PartitionedHypergraph(
    r_context.partition.k, result.hypergraph, parallel_tag_t());
  result.valid = true;

  // Recursively call deep multilevel partitioning
  deep_multilevel_partitioning(result.partitioned_hg, r_context, info, rb_tree);

  return result;
}

}

PartitionedHypergraph partition(Hypergraph& hypergraph, const Context& context) {
  PartitionedHypergraph partitioned_hypergraph(
    context.partition.k, hypergraph, parallel_tag_t());
  partition(partitioned_hypergraph, context);
  return partitioned_hypergraph;
}

void partition(PartitionedHypergraph& hypergraph, const Context& context) {
  RBTree rb_tree(context);
  deep_multilevel_partitioning(hypergraph, context,
    OriginalHypergraphInfo { hypergraph.totalWeight(),
      context.partition.k, context.partition.epsilon }, rb_tree);
}

} // namespace deep_multilevel
} // namepace mt_kahypar
