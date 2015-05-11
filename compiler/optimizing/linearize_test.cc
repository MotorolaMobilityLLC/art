/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include <fstream>

#include "arch/x86/instruction_set_features_x86.h"
#include "base/arena_allocator.h"
#include "base/stringprintf.h"
#include "builder.h"
#include "code_generator.h"
#include "code_generator_x86.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "driver/compiler_options.h"
#include "graph_visualizer.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "pretty_printer.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"

#include "gtest/gtest.h"

namespace art {

static void TestCode(const uint16_t* data, const int* expected_order, size_t number_of_blocks) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateGraph(&allocator);
  HGraphBuilder builder(graph);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  bool graph_built = builder.BuildGraph(*item);
  ASSERT_TRUE(graph_built);

  graph->TryBuildingSsa();

  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();

  ASSERT_EQ(graph->GetLinearOrder().Size(), number_of_blocks);
  for (size_t i = 0; i < number_of_blocks; ++i) {
    ASSERT_EQ(graph->GetLinearOrder().Get(i)->GetBlockId(), expected_order[i]);
  }
}

TEST(LinearizeTest, CFG1) {
  // Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2 ++++++
  //            /   \       +
  //       Block5   Block7  +
  //         |        |     +
  //       Block6   Block3  +
  //               + /   \  +
  //           Block4   Block8

  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 5,
    Instruction::IF_EQ, 0xFFFE,
    Instruction::GOTO | 0xFE00,
    Instruction::RETURN_VOID);

  const int blocks[] = {0, 1, 2, 7, 3, 4, 8, 5, 6};
  TestCode(data, blocks, 9);
}

TEST(LinearizeTest, CFG2) {
  // Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2 ++++++
  //            /   \       +
  //       Block3   Block7  +
  //         |        |     +
  //       Block6   Block4  +
  //               + /   \  +
  //           Block5   Block8

  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::RETURN_VOID,
    Instruction::IF_EQ, 0xFFFD,
    Instruction::GOTO | 0xFE00);

  const int blocks[] = {0, 1, 2, 7, 4, 5, 8, 3, 6};
  TestCode(data, blocks, 9);
}

TEST(LinearizeTest, CFG3) {
  // Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2 ++++++
  //            /   \       +
  //       Block3   Block8  +
  //         |        |     +
  //       Block7   Block5  +
  //                 / +  \ +
  //           Block6  + Block9
  //             |     +
  //           Block4 ++
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 4,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x0100,
    Instruction::IF_EQ, 0xFFFC,
    Instruction::GOTO | 0xFD00);

  const int blocks[] = {0, 1, 2, 8, 5, 6, 4, 9, 3, 7};
  TestCode(data, blocks, 10);
}

TEST(LinearizeTest, CFG4) {
  /* Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2
  //            / +  \
  //       Block6 + Block8
  //         |    +   |
  //       Block7 + Block3 +++++++
  //              +  /  \        +
  //           Block9   Block10  +
  //                      |      +
  //                    Block4   +
  //                  + /    \   +
  //                Block5  Block11
  */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 7,
    Instruction::IF_EQ, 0xFFFE,
    Instruction::IF_EQ, 0xFFFE,
    Instruction::GOTO | 0xFE00,
    Instruction::RETURN_VOID);

  const int blocks[] = {0, 1, 2, 8, 3, 10, 4, 5, 11, 9, 6, 7};
  TestCode(data, blocks, 12);
}

TEST(LinearizeTest, CFG5) {
  /* Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2
  //            / +  \
  //       Block3 + Block8
  //         |    +   |
  //       Block7 + Block4 +++++++
  //              +  /  \        +
  //           Block9   Block10  +
  //                      |      +
  //                    Block5   +
  //                   +/    \   +
  //                Block6  Block11
  */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::RETURN_VOID,
    Instruction::IF_EQ, 0xFFFD,
    Instruction::IF_EQ, 0xFFFE,
    Instruction::GOTO | 0xFE00);

  const int blocks[] = {0, 1, 2, 8, 4, 10, 5, 6, 11, 9, 3, 7};
  TestCode(data, blocks, 12);
}

TEST(LinearizeTest, CFG6) {
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2 ++++++++++++++
  //              |                 +
  //            Block3              +
  //            /     \             +
  //       Block8     Block4        +
  //         |         /   \        +
  //       Block5 <- Block9 Block6  +
  //         |
  //       Block7
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::GOTO | 0x0100,
    Instruction::IF_EQ, 0x0004,
    Instruction::IF_EQ, 0x0003,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFA00);

  const int blocks[] = {0, 1, 2, 3, 4, 6, 9, 8, 5, 7};
  TestCode(data, blocks, arraysize(blocks));
}

TEST(LinearizeTest, CFG7) {
  // Structure of this graph (+ are back edges)
  //            Block0
  //              |
  //            Block1
  //              |
  //            Block2 ++++++++
  //              |           +
  //            Block3        +
  //            /    \        +
  //        Block4  Block8    +
  //        /  \        |     +
  //   Block5 Block9 - Block6 +
  //     |
  //   Block7
  //
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::GOTO | 0x0100,
    Instruction::IF_EQ, 0x0005,
    Instruction::IF_EQ, 0x0003,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFA00);

  const int blocks[] = {0, 1, 2, 3, 4, 9, 8, 6, 5, 7};
  TestCode(data, blocks, arraysize(blocks));
}

}  // namespace art
