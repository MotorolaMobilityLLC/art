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

#include "graph_checker.h"

#include <map>
#include <string>

#include "base/bit_vector-inl.h"
#include "base/stringprintf.h"

namespace art {

void GraphChecker::VisitBasicBlock(HBasicBlock* block) {
  current_block_ = block;

  // Check consistency with respect to predecessors of `block`.
  const GrowableArray<HBasicBlock*>& predecessors = block->GetPredecessors();
  std::map<HBasicBlock*, size_t> predecessors_count;
  for (size_t i = 0, e = predecessors.Size(); i < e; ++i) {
    HBasicBlock* p = predecessors.Get(i);
    ++predecessors_count[p];
  }
  for (auto& pc : predecessors_count) {
    HBasicBlock* p = pc.first;
    size_t p_count_in_block_predecessors = pc.second;
    const GrowableArray<HBasicBlock*>& p_successors = p->GetSuccessors();
    size_t block_count_in_p_successors = 0;
    for (size_t j = 0, f = p_successors.Size(); j < f; ++j) {
      if (p_successors.Get(j) == block) {
        ++block_count_in_p_successors;
      }
    }
    if (p_count_in_block_predecessors != block_count_in_p_successors) {
      AddError(StringPrintf(
          "Block %d lists %zu occurrences of block %d in its predecessors, whereas "
          "block %d lists %zu occurrences of block %d in its successors.",
          block->GetBlockId(), p_count_in_block_predecessors, p->GetBlockId(),
          p->GetBlockId(), block_count_in_p_successors, block->GetBlockId()));
    }
  }

  // Check consistency with respect to successors of `block`.
  const GrowableArray<HBasicBlock*>& successors = block->GetSuccessors();
  std::map<HBasicBlock*, size_t> successors_count;
  for (size_t i = 0, e = successors.Size(); i < e; ++i) {
    HBasicBlock* s = successors.Get(i);
    ++successors_count[s];
  }
  for (auto& sc : successors_count) {
    HBasicBlock* s = sc.first;
    size_t s_count_in_block_successors = sc.second;
    const GrowableArray<HBasicBlock*>& s_predecessors = s->GetPredecessors();
    size_t block_count_in_s_predecessors = 0;
    for (size_t j = 0, f = s_predecessors.Size(); j < f; ++j) {
      if (s_predecessors.Get(j) == block) {
        ++block_count_in_s_predecessors;
      }
    }
    if (s_count_in_block_successors != block_count_in_s_predecessors) {
      AddError(StringPrintf(
          "Block %d lists %zu occurrences of block %d in its successors, whereas "
          "block %d lists %zu occurrences of block %d in its predecessors.",
          block->GetBlockId(), s_count_in_block_successors, s->GetBlockId(),
          s->GetBlockId(), block_count_in_s_predecessors, block->GetBlockId()));
    }
  }

  // Ensure `block` ends with a branch instruction.
  HInstruction* last_inst = block->GetLastInstruction();
  if (last_inst == nullptr || !last_inst->IsControlFlow()) {
    AddError(StringPrintf("Block %d does not end with a branch instruction.",
                          block->GetBlockId()));
  }

  // Visit this block's list of phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    // Ensure this block's list of phis contains only phis.
    if (!it.Current()->IsPhi()) {
      AddError(StringPrintf("Block %d has a non-phi in its phi list.",
                            current_block_->GetBlockId()));
    }
    it.Current()->Accept(this);
  }

  // Visit this block's list of instructions.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done();
       it.Advance()) {
    // Ensure this block's list of instructions does not contains phis.
    if (it.Current()->IsPhi()) {
      AddError(StringPrintf("Block %d has a phi in its non-phi list.",
                            current_block_->GetBlockId()));
    }
    it.Current()->Accept(this);
  }
}

void GraphChecker::VisitInstruction(HInstruction* instruction) {
  if (seen_ids_.IsBitSet(instruction->GetId())) {
    AddError(StringPrintf("Instruction id %d is duplicate in graph.",
                          instruction->GetId()));
  } else {
    seen_ids_.SetBit(instruction->GetId());
  }

  // Ensure `instruction` is associated with `current_block_`.
  if (instruction->GetBlock() == nullptr) {
    AddError(StringPrintf("%s %d in block %d not associated with any block.",
                          instruction->IsPhi() ? "Phi" : "Instruction",
                          instruction->GetId(),
                          current_block_->GetBlockId()));
  } else if (instruction->GetBlock() != current_block_) {
    AddError(StringPrintf("%s %d in block %d associated with block %d.",
                          instruction->IsPhi() ? "Phi" : "Instruction",
                          instruction->GetId(),
                          current_block_->GetBlockId(),
                          instruction->GetBlock()->GetBlockId()));
  }

  // Ensure the inputs of `instruction` are defined in a block of the graph.
  for (HInputIterator input_it(instruction); !input_it.Done();
       input_it.Advance()) {
    HInstruction* input = input_it.Current();
    const HInstructionList& list = input->IsPhi()
        ? input->GetBlock()->GetPhis()
        : input->GetBlock()->GetInstructions();
    if (!list.Contains(input)) {
      AddError(StringPrintf("Input %d of instruction %d is not defined "
                            "in a basic block of the control-flow graph.",
                            input->GetId(),
                            instruction->GetId()));
    }
  }

  // Ensure the uses of `instruction` are defined in a block of the graph.
  for (HUseIterator<HInstruction*> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    const HInstructionList& list = use->IsPhi()
        ? use->GetBlock()->GetPhis()
        : use->GetBlock()->GetInstructions();
    if (!list.Contains(use)) {
      AddError(StringPrintf("User %s:%d of instruction %d is not defined "
                            "in a basic block of the control-flow graph.",
                            use->DebugName(),
                            use->GetId(),
                            instruction->GetId()));
    }
  }

  // Ensure 'instruction' has pointers to its inputs' use entries.
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    HUserRecord<HInstruction*> input_record = instruction->InputRecordAt(i);
    HInstruction* input = input_record.GetInstruction();
    HUseListNode<HInstruction*>* use_node = input_record.GetUseNode();
    if (use_node == nullptr || !input->GetUses().Contains(use_node)) {
      AddError(StringPrintf("Instruction %s:%d has an invalid pointer to use entry "
                            "at input %u (%s:%d).",
                            instruction->DebugName(),
                            instruction->GetId(),
                            static_cast<unsigned>(i),
                            input->DebugName(),
                            input->GetId()));
    }
  }
}

void SSAChecker::VisitBasicBlock(HBasicBlock* block) {
  super_type::VisitBasicBlock(block);

  // Ensure there is no critical edge (i.e., an edge connecting a
  // block with multiple successors to a block with multiple
  // predecessors).
  if (block->GetSuccessors().Size() > 1) {
    for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
      HBasicBlock* successor = block->GetSuccessors().Get(j);
      if (successor->GetPredecessors().Size() > 1) {
        AddError(StringPrintf("Critical edge between blocks %d and %d.",
                              block->GetBlockId(),
                              successor->GetBlockId()));
      }
    }
  }

  if (block->IsLoopHeader()) {
    CheckLoop(block);
  }
}

void SSAChecker::CheckLoop(HBasicBlock* loop_header) {
  int id = loop_header->GetBlockId();

  // Ensure the pre-header block is first in the list of
  // predecessors of a loop header.
  if (!loop_header->IsLoopPreHeaderFirstPredecessor()) {
    AddError(StringPrintf(
        "Loop pre-header is not the first predecessor of the loop header %d.",
        id));
  }

  // Ensure the loop header has only two predecessors and that only the
  // second one is a back edge.
  size_t num_preds = loop_header->GetPredecessors().Size();
  if (num_preds < 2) {
    AddError(StringPrintf(
        "Loop header %d has less than two predecessors: %zu.",
        id,
        num_preds));
  } else if (num_preds > 2) {
    AddError(StringPrintf(
        "Loop header %d has more than two predecessors: %zu.",
        id,
        num_preds));
  } else {
    HLoopInformation* loop_information = loop_header->GetLoopInformation();
    HBasicBlock* first_predecessor = loop_header->GetPredecessors().Get(0);
    if (loop_information->IsBackEdge(first_predecessor)) {
      AddError(StringPrintf(
          "First predecessor of loop header %d is a back edge.",
          id));
    }
    HBasicBlock* second_predecessor = loop_header->GetPredecessors().Get(1);
    if (!loop_information->IsBackEdge(second_predecessor)) {
      AddError(StringPrintf(
          "Second predecessor of loop header %d is not a back edge.",
          id));
    }
  }

  // Ensure there is only one back edge per loop.
  size_t num_back_edges =
    loop_header->GetLoopInformation()->GetBackEdges().Size();
  if (num_back_edges == 0) {
    AddError(StringPrintf(
        "Loop defined by header %d has no back edge.",
        id));
  } else if (num_back_edges > 1) {
    AddError(StringPrintf(
        "Loop defined by header %d has several back edges: %zu.",
        id,
        num_back_edges));
  }

  // Ensure all blocks in the loop are dominated by the loop header.
  const ArenaBitVector& loop_blocks =
    loop_header->GetLoopInformation()->GetBlocks();
  for (uint32_t i : loop_blocks.Indexes()) {
    HBasicBlock* loop_block = GetGraph()->GetBlocks().Get(i);
    if (!loop_header->Dominates(loop_block)) {
      AddError(StringPrintf("Loop block %d not dominated by loop header %d.",
                            loop_block->GetBlockId(),
                            id));
    }
  }
}

void SSAChecker::VisitInstruction(HInstruction* instruction) {
  super_type::VisitInstruction(instruction);

  // Ensure an instruction dominates all its uses.
  for (HUseIterator<HInstruction*> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    if (!use->IsPhi() && !instruction->StrictlyDominates(use)) {
      AddError(StringPrintf("Instruction %d in block %d does not dominate "
                            "use %d in block %d.",
                            instruction->GetId(), current_block_->GetBlockId(),
                            use->GetId(), use->GetBlock()->GetBlockId()));
    }
  }

  // Ensure an instruction having an environment is dominated by the
  // instructions contained in the environment.
  HEnvironment* environment = instruction->GetEnvironment();
  if (environment != nullptr) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* env_instruction = environment->GetInstructionAt(i);
      if (env_instruction != nullptr
          && !env_instruction->StrictlyDominates(instruction)) {
        AddError(StringPrintf("Instruction %d in environment of instruction %d "
                              "from block %d does not dominate instruction %d.",
                              env_instruction->GetId(),
                              instruction->GetId(),
                              current_block_->GetBlockId(),
                              instruction->GetId()));
      }
    }
  }
}

static Primitive::Type PrimitiveKind(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
      return Primitive::kPrimInt;
    default:
      return type;
  }
}

void SSAChecker::VisitPhi(HPhi* phi) {
  VisitInstruction(phi);

  // Ensure the first input of a phi is not itself.
  if (phi->InputAt(0) == phi) {
    AddError(StringPrintf("Loop phi %d in block %d is its own first input.",
                          phi->GetId(),
                          phi->GetBlock()->GetBlockId()));
  }

  // Ensure the number of inputs of a phi is the same as the number of
  // its predecessors.
  const GrowableArray<HBasicBlock*>& predecessors =
    phi->GetBlock()->GetPredecessors();
  if (phi->InputCount() != predecessors.Size()) {
    AddError(StringPrintf(
        "Phi %d in block %d has %zu inputs, "
        "but block %d has %zu predecessors.",
        phi->GetId(), phi->GetBlock()->GetBlockId(), phi->InputCount(),
        phi->GetBlock()->GetBlockId(), predecessors.Size()));
  } else {
    // Ensure phi input at index I either comes from the Ith
    // predecessor or from a block that dominates this predecessor.
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      HBasicBlock* predecessor = predecessors.Get(i);
      if (!(input->GetBlock() == predecessor
            || input->GetBlock()->Dominates(predecessor))) {
        AddError(StringPrintf(
            "Input %d at index %zu of phi %d from block %d is not defined in "
            "predecessor number %zu nor in a block dominating it.",
            input->GetId(), i, phi->GetId(), phi->GetBlock()->GetBlockId(),
            i));
      }
    }
  }
  // Ensure that the inputs have the same primitive kind as the phi.
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    HInstruction* input = phi->InputAt(i);
    if (PrimitiveKind(input->GetType()) != PrimitiveKind(phi->GetType())) {
        AddError(StringPrintf(
            "Input %d at index %zu of phi %d from block %d does not have the "
            "same type as the phi: %s versus %s",
            input->GetId(), i, phi->GetId(), phi->GetBlock()->GetBlockId(),
            Primitive::PrettyDescriptor(input->GetType()),
            Primitive::PrettyDescriptor(phi->GetType())));
    }
  }
}

void SSAChecker::VisitIf(HIf* instruction) {
  VisitInstruction(instruction);
  HInstruction* input = instruction->InputAt(0);
  if (input->IsIntConstant()) {
    int value = input->AsIntConstant()->GetValue();
    if (value != 0 && value != 1) {
      AddError(StringPrintf(
          "If instruction %d has a non-Boolean constant input "
          "whose value is: %d.",
          instruction->GetId(),
          value));
    }
  } else if (instruction->InputAt(0)->GetType() != Primitive::kPrimBoolean) {
    AddError(StringPrintf(
        "If instruction %d has a non-Boolean input type: %s.",
        instruction->GetId(),
        Primitive::PrettyDescriptor(instruction->InputAt(0)->GetType())));
  }
}

void SSAChecker::VisitCondition(HCondition* op) {
  VisitInstruction(op);
  if (op->GetType() != Primitive::kPrimBoolean) {
    AddError(StringPrintf(
        "Condition %s %d has a non-Boolean result type: %s.",
        op->DebugName(), op->GetId(),
        Primitive::PrettyDescriptor(op->GetType())));
  }
  HInstruction* lhs = op->InputAt(0);
  HInstruction* rhs = op->InputAt(1);
  if (lhs->GetType() == Primitive::kPrimNot) {
    if (!op->IsEqual() && !op->IsNotEqual()) {
      AddError(StringPrintf(
          "Condition %s %d uses an object as left-hand side input.",
          op->DebugName(), op->GetId()));
    }
    if (rhs->IsIntConstant() && rhs->AsIntConstant()->GetValue() != 0) {
      AddError(StringPrintf(
          "Condition %s %d compares an object with a non-zero integer: %d.",
          op->DebugName(), op->GetId(),
          rhs->AsIntConstant()->GetValue()));
    }
  } else if (rhs->GetType() == Primitive::kPrimNot) {
    if (!op->IsEqual() && !op->IsNotEqual()) {
      AddError(StringPrintf(
          "Condition %s %d uses an object as right-hand side input.",
          op->DebugName(), op->GetId()));
    }
    if (lhs->IsIntConstant() && lhs->AsIntConstant()->GetValue() != 0) {
      AddError(StringPrintf(
          "Condition %s %d compares a non-zero integer with an object: %d.",
          op->DebugName(), op->GetId(),
          lhs->AsIntConstant()->GetValue()));
    }
  } else if (PrimitiveKind(lhs->GetType()) != PrimitiveKind(rhs->GetType())) {
      AddError(StringPrintf(
          "Condition %s %d has inputs of different types: "
          "%s, and %s.",
          op->DebugName(), op->GetId(),
          Primitive::PrettyDescriptor(lhs->GetType()),
          Primitive::PrettyDescriptor(rhs->GetType())));
  }
}

void SSAChecker::VisitBinaryOperation(HBinaryOperation* op) {
  VisitInstruction(op);
  if (op->IsUShr() || op->IsShr() || op->IsShl()) {
    if (PrimitiveKind(op->InputAt(1)->GetType()) != Primitive::kPrimInt) {
      AddError(StringPrintf(
          "Shift operation %s %d has a non-int kind second input: "
          "%s of type %s.",
          op->DebugName(), op->GetId(),
          op->InputAt(1)->DebugName(),
          Primitive::PrettyDescriptor(op->InputAt(1)->GetType())));
    }
  } else {
    if (PrimitiveKind(op->InputAt(1)->GetType()) != PrimitiveKind(op->InputAt(0)->GetType())) {
      AddError(StringPrintf(
          "Binary operation %s %d has inputs of different types: "
          "%s, and %s.",
          op->DebugName(), op->GetId(),
          Primitive::PrettyDescriptor(op->InputAt(0)->GetType()),
          Primitive::PrettyDescriptor(op->InputAt(1)->GetType())));
    }
  }

  if (op->IsCompare()) {
    if (op->GetType() != Primitive::kPrimInt) {
      AddError(StringPrintf(
          "Compare operation %d has a non-int result type: %s.",
          op->GetId(),
          Primitive::PrettyDescriptor(op->GetType())));
    }
  } else {
    // Use the first input, so that we can also make this check for shift operations.
    if (PrimitiveKind(op->GetType()) != PrimitiveKind(op->InputAt(0)->GetType())) {
      AddError(StringPrintf(
          "Binary operation %s %d has a result type different "
          "from its input type: %s vs %s.",
          op->DebugName(), op->GetId(),
          Primitive::PrettyDescriptor(op->GetType()),
          Primitive::PrettyDescriptor(op->InputAt(1)->GetType())));
    }
  }
}

}  // namespace art
