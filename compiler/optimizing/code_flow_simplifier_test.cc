/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "code_flow_simplifier.h"

#include "base/arena_allocator.h"
#include "base/macros.h"
#include "builder.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "side_effects_analysis.h"

namespace art HIDDEN {

class CodeFlowSimplifierTest : public OptimizingUnitTest {
 protected:
  HPhi* ConstructBasicGraphForSelect(HBasicBlock* return_block, HInstruction* instr) {
    HParameterValue* bool_param = MakeParam(DataType::Type::kBool);
    HIntConstant* const1 =  graph_->GetIntConstant(1);

    auto [if_block, then_block, else_block] = CreateDiamondPattern(return_block, bool_param);

    AddOrInsertInstruction(then_block, instr);
    HPhi* phi = MakePhi(return_block, {instr, const1});
    return phi;
  }

  bool CheckGraphAndTryCodeFlowSimplifier() {
    graph_->BuildDominatorTree();
    EXPECT_TRUE(CheckGraph());

    SideEffectsAnalysis side_effects(graph_);
    side_effects.Run();
    return HCodeFlowSimplifier(graph_, /*handles*/ nullptr, /*stats*/ nullptr).Run();
  }
};

// HDivZeroCheck might throw and should not be hoisted from the conditional to an unconditional.
TEST_F(CodeFlowSimplifierTest, testZeroCheckPreventsSelect) {
  HBasicBlock* return_block = InitEntryMainExitGraphWithReturnVoid();
  HParameterValue* param = MakeParam(DataType::Type::kInt32);
  HDivZeroCheck* instr = new (GetAllocator()) HDivZeroCheck(param, 0);
  HPhi* phi = ConstructBasicGraphForSelect(return_block, instr);

  ManuallyBuildEnvFor(instr, {param, graph_->GetIntConstant(1)});

  EXPECT_FALSE(CheckGraphAndTryCodeFlowSimplifier());
  EXPECT_FALSE(phi->GetBlock() == nullptr);
}

// Test that CodeFlowSimplifier succeeds with HAdd.
TEST_F(CodeFlowSimplifierTest, testSelectWithAdd) {
  HBasicBlock* return_block = InitEntryMainExitGraphWithReturnVoid();
  HParameterValue* param = MakeParam(DataType::Type::kInt32);
  HAdd* instr = new (GetAllocator()) HAdd(DataType::Type::kInt32, param, param, /*dex_pc=*/ 0);
  HPhi* phi = ConstructBasicGraphForSelect(return_block, instr);
  EXPECT_TRUE(CheckGraphAndTryCodeFlowSimplifier());
  EXPECT_TRUE(phi->GetBlock() == nullptr);
}

// Test `HSelect` optimization in an irreducible loop.
TEST_F(CodeFlowSimplifierTest, testSelectInIrreducibleLoop) {
  HBasicBlock* return_block = InitEntryMainExitGraphWithReturnVoid();
  auto [split, left_header, right_header, body] = CreateIrreducibleLoop(return_block);

  HParameterValue* split_param = MakeParam(DataType::Type::kBool);
  HParameterValue* bool_param = MakeParam(DataType::Type::kBool);
  HParameterValue* n_param = MakeParam(DataType::Type::kInt32);

  MakeIf(split, split_param);

  HInstruction* const0 = graph_->GetIntConstant(0);
  HInstruction* const1 = graph_->GetIntConstant(1);
  HPhi* right_phi = MakePhi(right_header, {const0, /* placeholder */ const0});
  HPhi* left_phi = MakePhi(left_header, {const1, right_phi});
  HAdd* add = MakeBinOp<HAdd>(body, DataType::Type::kInt32, left_phi, const1);
  right_phi->ReplaceInput(add, 1u);  // Update back-edge input.
  HCondition* condition = MakeCondition(left_header, kCondGE, left_phi, n_param);
  MakeIf(left_header, condition);

  auto [if_block, then_block, else_block] = CreateDiamondPattern(body, bool_param);
  HPhi* phi = MakePhi(body, {const1, const0});

  EXPECT_TRUE(CheckGraphAndTryCodeFlowSimplifier());
  HLoopInformation* loop_info = left_header->GetLoopInformation();
  ASSERT_TRUE(loop_info != nullptr);
  ASSERT_TRUE(loop_info->IsIrreducible());

  EXPECT_TRUE(phi->GetBlock() == nullptr);
  ASSERT_TRUE(if_block->GetFirstInstruction()->IsSelect());

  ASSERT_EQ(if_block, add->GetBlock());  // Moved when merging blocks.

  for (HBasicBlock* removed_block : {then_block, else_block, body}) {
    uint32_t removed_block_id = removed_block->GetBlockId();
    ASSERT_TRUE(removed_block->GetGraph() == nullptr) << removed_block_id;
    ASSERT_FALSE(loop_info->GetBlocks().IsBitSet(removed_block_id)) << removed_block_id;
  }
}

}  // namespace art
