#include "llvm_propeller_code_layout.h"

#include <memory>
#include <string>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_node_chain_builder.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/string_view.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::CFGEdge;
using ::devtools_crosstool_autofdo::CodeLayout;
using ::devtools_crosstool_autofdo::ControlFlowGraph;
using ::devtools_crosstool_autofdo::MockPropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::NodeChain;
using ::devtools_crosstool_autofdo::NodeChainBuilder;
using ::devtools_crosstool_autofdo::PropellerCodeLayoutParameters;
using ::devtools_crosstool_autofdo::PropellerCodeLayoutScorer;
using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;

static std::unique_ptr<MockPropellerWholeProgramInfo> GetTestWholeProgramInfo(
    const std::string &testdata_path) {
  const PropellerOptions options(PropellerOptionsBuilder().AddPerfNames(
      FLAGS_test_srcdir + testdata_path));
  auto whole_program_info = std::make_unique<
      devtools_crosstool_autofdo::MockPropellerWholeProgramInfo>(options);
  if (!whole_program_info->CreateCfgs()) {
    return nullptr;
  }
  return whole_program_info;
}

// Helper method to capture the ordinals of a chain's nodes in a vector.
static std::vector<uint64_t> GetOrderedNodeIds(
    const std::unique_ptr<NodeChain> &chain) {
  std::vector<uint64_t> node_ids;
  chain->VisitEachNodeRef(
      [&node_ids](auto &n) { node_ids.push_back(n.symbol_ordinal()); });
  return node_ids;
}

// Check that when multiplying the code layout parameters results in integer
// overflow, constructing the scorer crashes.
TEST(CodeLayoutScorerTest, TestOverflow) {
  PropellerCodeLayoutParameters params;
  params.set_fallthrough_weight(1 << 2);
  params.set_forward_jump_weight(1);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_distance(1 << 10);
  params.set_backward_jump_distance(1 << 20);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
  params.set_fallthrough_weight(1);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_weight(1 << 10);
  params.set_forward_jump_distance(0);
  params.set_backward_jump_distance(1 << 22);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
  params.set_fallthrough_weight(1);
  params.set_backward_jump_weight(1 << 10);
  params.set_forward_jump_weight(1);
  params.set_forward_jump_distance(1 << 22);
  params.set_backward_jump_distance(0);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
}

TEST(CodeLayoutScorerTest, GetEdgeScore) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_multi_function.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  const ControlFlowGraph &foo_cfg = *whole_program_info->cfgs().at("foo");
  const ControlFlowGraph &bar_cfg = *whole_program_info->cfgs().at("bar");

  // Build a layout scorer with specific parameters.
  PropellerCodeLayoutParameters params;
  params.set_fallthrough_weight(10);
  params.set_forward_jump_weight(2);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_distance(200);
  params.set_backward_jump_distance(100);
  PropellerCodeLayoutScorer scorer(params);

  ASSERT_EQ(bar_cfg.inter_edges().size(), 1);
  {
    const auto &call_edge = bar_cfg.inter_edges().front();
    ASSERT_TRUE(call_edge->IsCall());
    ASSERT_NE(call_edge->weight(), 0);
    ASSERT_NE(call_edge->src()->size(), 0);
    // Score with negative src-to-sink distance (backward call).
    // Check that for calls, half of src size is always added to the distance.
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, -10),
              call_edge->weight() * 1 * 200 *
                  (100 - 10 + call_edge->src()->size() / 2));
    // Score with zero src-to-sink distance (forward call).
    EXPECT_EQ(
        scorer.GetEdgeScore(*call_edge, 0),
        call_edge->weight() * 2 * 100 * (200 - call_edge->src()->size() / 2));
    // Score with positive src-to-sink distance (forward call).
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, 20),
              call_edge->weight() * 2 * 100 *
                  (200 - 20 - call_edge->src()->size() / 2));
    // Score must be zero when beyond the src-to-sink distance exceeds the
    // distance parameters.
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, 250), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, -150), 0);
  }

  ASSERT_EQ(foo_cfg.inter_edges().size(), 2);
  for (const std::unique_ptr<CFGEdge> &ret_edge : foo_cfg.inter_edges()) {
    ASSERT_TRUE(ret_edge->IsReturn());
    ASSERT_NE(ret_edge->weight(), 0);
    ASSERT_NE(ret_edge->sink()->size(), 0);
    // Score with negative src-to-sink distance (backward return).
    // Check that for returns, half of sink size is always added to the
    // distance.
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, -10),
              ret_edge->weight() * 1 * 200 *
                  (100 - 10 + ret_edge->sink()->size() / 2));
    // Score with zero src-to-sink distance (forward return).
    EXPECT_EQ(
        scorer.GetEdgeScore(*ret_edge, 0),
        ret_edge->weight() * 2 * 100 * (200 - ret_edge->sink()->size() / 2));
    // Score with positive src-to-sink distance (forward return).
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, 20),
              ret_edge->weight() * 2 * 100 *
                  (200 - 20 - ret_edge->sink()->size() / 2));
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, 250), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, -150), 0);
  }

  for (const std::unique_ptr<CFGEdge> &edge : foo_cfg.intra_edges()) {
    ASSERT_EQ(edge->kind(),
              devtools_crosstool_autofdo::CFGEdge::Kind::kBranchOrFallthough);
    ASSERT_NE(edge->weight(), 0);
    // Fallthrough score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 0), edge->weight() * 10 * 100 * 200);
    // Backward edge (within distance threshold) score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, -40),
              edge->weight() * 1 * 200 * (100 - 40));
    // Forward edge (within distance threshold) score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 80),
              edge->weight() * 2 * 100 * (200 - 80));
    // Forward and backward edge beyond the distance thresholds (zero score).
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 201), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*edge, -101), 0);
  }
}

// This tests every step in NodeChainBuilder::BuildChains on a single CFG.
TEST(CodeLayoutTest, BuildChains) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_three_branches.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  const std::unique_ptr<ControlFlowGraph> &foo_cfg =
      whole_program_info->cfgs().at("foo");
  ASSERT_EQ(6, foo_cfg->nodes().size());
  auto chain_builder =
      NodeChainBuilder(PropellerCodeLayoutScorer(
                           whole_program_info->options().code_layout_params()),
                       foo_cfg.get());

  chain_builder.InitNodeChains();

  auto &chains = chain_builder.chains();
  // Verify that there are 5 chains corresponding to the hot nodes.
  EXPECT_EQ(5, chains.size());
  // Verify that every chain has a single node.
  for (auto &chain_elem : chains) {
    EXPECT_THAT(GetOrderedNodeIds(chain_elem.second),
                testing::ElementsAreArray(
                    {chain_elem.second->delegate_node_->symbol_ordinal()}));
  }

  chain_builder.InitChainEdges();

  // Verify the number of in edges and out edges of every chain.
  struct {
    uint64_t chain_id;
    int out_edges_count;
    int in_edges_count;
  } expected_edge_counts[] = {
      {1, 1, 0}, {2, 1, 1}, {3, 1, 0}, {4, 0, 1}, {5, 0, 1}};
  for (const auto &chain_edge_count : expected_edge_counts) {
    EXPECT_EQ(chain_edge_count.out_edges_count,
              chains.at(chain_edge_count.chain_id)->out_edges_.size());
    EXPECT_EQ(chain_edge_count.in_edges_count,
              chains.at(chain_edge_count.chain_id)->in_edges_.size());
  }

  chain_builder.InitChainAssemblies();

  // Verify the number of chain assemblies.
  EXPECT_EQ(5, chain_builder.node_chain_assemblies().size());

  chain_builder.KeepMergingBestChains();

  // Verify that the chain assemblies is empty now.
  EXPECT_TRUE(chain_builder.node_chain_assemblies().empty());
  // Verify the constructed chains.
  EXPECT_EQ(2, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)),
              testing::ElementsAreArray({1, 2, 5}));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)),
              testing::ElementsAreArray({3, 4}));

  chain_builder.CoalesceChains();

  // Verify that the two chains are coalesced together.
  EXPECT_EQ(1, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)),
              testing::ElementsAreArray({1, 2, 5, 3, 4}));
}

TEST(CodeLayoutTest, FindOptimalFallthrough) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_conditional.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info =
      CodeLayout(whole_program_info->options().code_layout_params(),
                 whole_program_info->GetHotCfgs())
          .OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_EQ("foo", func_cluster_info.cfg->GetPrimaryName());
  // TODO(rahmanl) NodeChainBuilder must be improved so it can find {1,2,3}
  // which is optimal.
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 3, 1}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info =
      CodeLayout(whole_program_info->options().code_layout_params(),
                 whole_program_info->GetHotCfgs())
          .OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_EQ("foo", func_cluster_info.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 1, 3, 4}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalNestedLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_nested_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);
  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info =
      CodeLayout(whole_program_info->options().code_layout_params(),
                 whole_program_info->GetHotCfgs())
          .OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 3, 1, 4, 5, 2}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalMultiFunctionLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_multi_function.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 4);
  auto layout_info =
      CodeLayout(whole_program_info->options().code_layout_params(),
                 whole_program_info->GetHotCfgs())
          .OrderAll();
  EXPECT_EQ(3, layout_info.size());

  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info_1 = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info_1.clusters.size());

  EXPECT_NE(layout_info.find(4), layout_info.end());
  auto &func_cluster_info_4 = layout_info.find(4)->second;
  EXPECT_EQ(1, func_cluster_info_4.clusters.size());

  EXPECT_NE(layout_info.find(9), layout_info.end());
  auto &func_cluster_info_9 = layout_info.find(9)->second;
  EXPECT_EQ(1, func_cluster_info_9.clusters.size());

  // Check the BB clusters for every function.
  EXPECT_EQ("foo", func_cluster_info_1.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_1.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 2, 1}));
  EXPECT_EQ("bar", func_cluster_info_4.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_4.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 1, 3}));
  EXPECT_EQ("qux", func_cluster_info_9.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_9.clusters.front().bb_indexes,
              testing::ElementsAreArray({0}));

  // Verify that the new layout improves the score for 'foo' and 'bar' and keeps
  // it equal to zero for 'qux'.
  EXPECT_GT(func_cluster_info_1.optimized_score.intra_score,
            func_cluster_info_1.original_score.intra_score);
  EXPECT_GT(func_cluster_info_4.optimized_score.intra_score,
            func_cluster_info_4.original_score.intra_score);
  EXPECT_EQ(func_cluster_info_9.optimized_score.intra_score, 0);
  EXPECT_EQ(func_cluster_info_9.original_score.intra_score, 0);
  // TODO(rahmanl): Check for improvement in inter_out_score once we have
  // function reordering.

  // Check the layout index of hot clusters.
  EXPECT_EQ(0, func_cluster_info_1.clusters.front().layout_index);
  EXPECT_EQ(1, func_cluster_info_4.clusters.front().layout_index);
  EXPECT_EQ(2, func_cluster_info_9.clusters.front().layout_index);

  // Check that the layout indices of cold clusters are consistent with their
  // hot counterparts.
  EXPECT_EQ(0, func_cluster_info_1.cold_cluster_layout_index);
  EXPECT_EQ(1, func_cluster_info_4.cold_cluster_layout_index);
  EXPECT_EQ(2, func_cluster_info_9.cold_cluster_layout_index);
}

}  // namespace
