/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "instruction_simplifier_arm64.h"

#include "common_arm64.h"
#include "instruction_simplifier_shared.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"

namespace art {

using helpers::CanFitInShifterOperand;
using helpers::HasShifterOperand;

namespace arm64 {

using helpers::ShifterOperandSupportsExtension;

bool InstructionSimplifierArm64Visitor::TryMergeIntoShifterOperand(HInstruction* use,
                                                                   HInstruction* bitfield_op,
                                                                   bool do_merge) {
  DCHECK(HasShifterOperand(use, kArm64));
  DCHECK(use->IsBinaryOperation() || use->IsNeg());
  DCHECK(CanFitInShifterOperand(bitfield_op));
  DCHECK(!bitfield_op->HasEnvironmentUses());

  Primitive::Type type = use->GetType();
  if (type != Primitive::kPrimInt && type != Primitive::kPrimLong) {
    return false;
  }

  HInstruction* left;
  HInstruction* right;
  if (use->IsBinaryOperation()) {
    left = use->InputAt(0);
    right = use->InputAt(1);
  } else {
    DCHECK(use->IsNeg());
    right = use->AsNeg()->InputAt(0);
    left = GetGraph()->GetConstant(right->GetType(), 0);
  }
  DCHECK(left == bitfield_op || right == bitfield_op);

  if (left == right) {
    // TODO: Handle special transformations in this situation?
    // For example should we transform `(x << 1) + (x << 1)` into `(x << 2)`?
    // Or should this be part of a separate transformation logic?
    return false;
  }

  bool is_commutative = use->IsBinaryOperation() && use->AsBinaryOperation()->IsCommutative();
  HInstruction* other_input;
  if (bitfield_op == right) {
    other_input = left;
  } else {
    if (is_commutative) {
      other_input = right;
    } else {
      return false;
    }
  }

  HDataProcWithShifterOp::OpKind op_kind;
  int shift_amount = 0;
  HDataProcWithShifterOp::GetOpInfoFromInstruction(bitfield_op, &op_kind, &shift_amount);

  if (HDataProcWithShifterOp::IsExtensionOp(op_kind) && !ShifterOperandSupportsExtension(use)) {
    return false;
  }

  if (do_merge) {
    HDataProcWithShifterOp* alu_with_op =
        new (GetGraph()->GetArena()) HDataProcWithShifterOp(use,
                                                            other_input,
                                                            bitfield_op->InputAt(0),
                                                            op_kind,
                                                            shift_amount,
                                                            use->GetDexPc());
    use->GetBlock()->ReplaceAndRemoveInstructionWith(use, alu_with_op);
    if (bitfield_op->GetUses().empty()) {
      bitfield_op->GetBlock()->RemoveInstruction(bitfield_op);
    }
    RecordSimplification();
  }

  return true;
}

// Merge a bitfield move instruction into its uses if it can be merged in all of them.
bool InstructionSimplifierArm64Visitor::TryMergeIntoUsersShifterOperand(HInstruction* bitfield_op) {
  DCHECK(CanFitInShifterOperand(bitfield_op));

  if (bitfield_op->HasEnvironmentUses()) {
    return false;
  }

  const HUseList<HInstruction*>& uses = bitfield_op->GetUses();

  // Check whether we can merge the instruction in all its users' shifter operand.
  for (const HUseListNode<HInstruction*>& use : uses) {
    HInstruction* user = use.GetUser();
    if (!HasShifterOperand(user, kArm64)) {
      return false;
    }
    if (!CanMergeIntoShifterOperand(user, bitfield_op)) {
      return false;
    }
  }

  // Merge the instruction into its uses.
  for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
    HInstruction* user = it->GetUser();
    // Increment `it` now because `*it` will disappear thanks to MergeIntoShifterOperand().
    ++it;
    bool merged = MergeIntoShifterOperand(user, bitfield_op);
    DCHECK(merged);
  }

  return true;
}

void InstructionSimplifierArm64Visitor::VisitAnd(HAnd* instruction) {
  if (TryMergeNegatedInput(instruction)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitArrayGet(HArrayGet* instruction) {
  size_t data_offset = CodeGenerator::GetArrayDataOffset(instruction);
  if (TryExtractArrayAccessAddress(instruction,
                                   instruction->GetArray(),
                                   instruction->GetIndex(),
                                   data_offset)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitArraySet(HArraySet* instruction) {
  size_t access_size = Primitive::ComponentSize(instruction->GetComponentType());
  size_t data_offset = mirror::Array::DataOffset(access_size).Uint32Value();
  if (TryExtractArrayAccessAddress(instruction,
                                   instruction->GetArray(),
                                   instruction->GetIndex(),
                                   data_offset)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitMul(HMul* instruction) {
  if (TryCombineMultiplyAccumulate(instruction, kArm64)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitOr(HOr* instruction) {
  if (TryMergeNegatedInput(instruction)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitShl(HShl* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitShr(HShr* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitTypeConversion(HTypeConversion* instruction) {
  Primitive::Type result_type = instruction->GetResultType();
  Primitive::Type input_type = instruction->GetInputType();

  if (input_type == result_type) {
    // We let the arch-independent code handle this.
    return;
  }

  if (Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type)) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitUShr(HUShr* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitXor(HXor* instruction) {
  if (TryMergeNegatedInput(instruction)) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitVecLoad(HVecLoad* instruction) {
  if (!instruction->IsStringCharAt()
      && TryExtractVecArrayAccessAddress(instruction, instruction->GetIndex())) {
    RecordSimplification();
  }
}

void InstructionSimplifierArm64Visitor::VisitVecStore(HVecStore* instruction) {
  if (TryExtractVecArrayAccessAddress(instruction, instruction->GetIndex())) {
    RecordSimplification();
  }
}

}  // namespace arm64
}  // namespace art
