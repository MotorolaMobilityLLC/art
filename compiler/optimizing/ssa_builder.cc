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

#include "ssa_builder.h"

#include "nodes.h"
#include "primitive_type_propagation.h"
#include "ssa_phi_elimination.h"

namespace art {

void SsaBuilder::BuildSsa() {
  // 1) Visit in reverse post order. We need to have all predecessors of a block visited
  // (with the exception of loops) in order to create the right environment for that
  // block. For loops, we create phis whose inputs will be set in 2).
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }

  // 2) Set inputs of loop phis.
  for (size_t i = 0; i < loop_headers_.Size(); i++) {
    HBasicBlock* block = loop_headers_.Get(i);
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      for (size_t pred = 0; pred < block->GetPredecessors().Size(); pred++) {
        HInstruction* input = ValueOfLocal(block->GetPredecessors().Get(pred), phi->GetRegNumber());
        phi->AddInput(input);
      }
    }
  }

  // 3) Mark dead phis. This will mark phis that are only used by environments:
  // at the DEX level, the type of these phis does not need to be consistent, but
  // our code generator will complain if the inputs of a phi do not have the same
  // type. The marking allows the type propagation to know which phis it needs
  // to handle. We mark but do not eliminate: the elimination will be done in
  // step 5).
  {
    SsaDeadPhiElimination dead_phis(GetGraph());
    dead_phis.MarkDeadPhis();
  }

  // 4) Propagate types of phis. At this point, phis are typed void in the general
  // case, or float/double/reference when we created an equivalent phi. So we
  // need to propagate the types across phis to give them a correct type.
  PrimitiveTypePropagation type_propagation(GetGraph());
  type_propagation.Run();

  // 5) Step 4) changes inputs of phis which may lead to dead phis again. We re-run
  // the algorithm and this time elimimates them.
  // TODO: Make this work with debug info and reference liveness. We currently
  // eagerly remove phis used in environments.
  {
    SsaDeadPhiElimination dead_phis(GetGraph());
    dead_phis.Run();
  }

  // 6) Clear locals.
  // TODO: Move this to a dead code eliminator phase.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()->GetInstructions());
       !it.Done();
       it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsLocal()) {
      current->GetBlock()->RemoveInstruction(current);
    }
  }
}

HInstruction* SsaBuilder::ValueOfLocal(HBasicBlock* block, size_t local) {
  return GetLocalsFor(block)->GetInstructionAt(local);
}

void SsaBuilder::VisitBasicBlock(HBasicBlock* block) {
  current_locals_ = GetLocalsFor(block);

  if (block->IsLoopHeader()) {
    // If the block is a loop header, we know we only have visited the pre header
    // because we are visiting in reverse post order. We create phis for all initialized
    // locals from the pre header. Their inputs will be populated at the end of
    // the analysis.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      HInstruction* incoming = ValueOfLocal(block->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, 0, Primitive::kPrimVoid);
        block->AddPhi(phi);
        current_locals_->SetRawEnvAt(local, phi);
      }
    }
    // Save the loop header so that the last phase of the analysis knows which
    // blocks need to be updated.
    loop_headers_.Add(block);
  } else if (block->GetPredecessors().Size() > 0) {
    // All predecessors have already been visited because we are visiting in reverse post order.
    // We merge the values of all locals, creating phis if those values differ.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocal(block->GetPredecessors().Get(0), local);

      for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
        HInstruction* current = ValueOfLocal(block->GetPredecessors().Get(i), local);
        if (current == nullptr) {
          one_predecessor_has_no_value = true;
          break;
        } else if (current != value) {
          is_different = true;
        }
      }

      if (one_predecessor_has_no_value) {
        // If one predecessor has no value for this local, we trust the verifier has
        // successfully checked that there is a store dominating any read after this block.
        continue;
      }

      if (is_different) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, block->GetPredecessors().Size(), Primitive::kPrimVoid);
        for (size_t i = 0; i < block->GetPredecessors().Size(); i++) {
          HInstruction* pred_value = ValueOfLocal(block->GetPredecessors().Get(i), local);
          phi->SetRawInputAt(i, pred_value);
        }
        block->AddPhi(phi);
        value = phi;
      }
      current_locals_->SetRawEnvAt(local, value);
    }
  }

  // Visit all instructions. The instructions of interest are:
  // - HLoadLocal: replace them with the current value of the local.
  // - HStoreLocal: update current value of the local and remove the instruction.
  // - Instructions that require an environment: populate their environment
  //   with the current values of the locals.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

/**
 * Constants in the Dex format are not typed. So the builder types them as
 * integers, but when doing the SSA form, we might realize the constant
 * is used for floating point operations. We create a floating-point equivalent
 * constant to make the operations correctly typed.
 */
static HFloatConstant* GetFloatEquivalent(HIntConstant* constant) {
  // We place the floating point constant next to this constant.
  HFloatConstant* result = constant->GetNext()->AsFloatConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HFloatConstant(bit_cast<int32_t, float>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<float, int32_t>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Wide constants in the Dex format are not typed. So the builder types them as
 * longs, but when doing the SSA form, we might realize the constant
 * is used for floating point operations. We create a floating-point equivalent
 * constant to make the operations correctly typed.
 */
static HDoubleConstant* GetDoubleEquivalent(HLongConstant* constant) {
  // We place the floating point constant next to this constant.
  HDoubleConstant* result = constant->GetNext()->AsDoubleConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HDoubleConstant(bit_cast<int64_t, double>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<double, int64_t>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Because of Dex format, we might end up having the same phi being
 * used for non floating point operations and floating point / reference operations.
 * Because we want the graph to be correctly typed (and thereafter avoid moves between
 * floating point registers and core registers), we need to create a copy of the
 * phi with a floating point / reference type.
 */
static HPhi* GetFloatDoubleOrReferenceEquivalentOfPhi(HPhi* phi, Primitive::Type type) {
  // We place the floating point /reference phi next to this phi.
  HInstruction* next = phi->GetNext();
  if (next != nullptr
      && next->AsPhi()->GetRegNumber() == phi->GetRegNumber()
      && next->GetType() != type) {
    // Move to the next phi to see if it is the one we are looking for.
    next = next->GetNext();
  }

  if (next == nullptr
      || (next->AsPhi()->GetRegNumber() != phi->GetRegNumber())
      || (next->GetType() != type)) {
    ArenaAllocator* allocator = phi->GetBlock()->GetGraph()->GetArena();
    HPhi* new_phi = new (allocator) HPhi(allocator, phi->GetRegNumber(), phi->InputCount(), type);
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      // Copy the inputs. Note that the graph may not be correctly typed by doing this copy,
      // but the type propagation phase will fix it.
      new_phi->SetRawInputAt(i, phi->InputAt(i));
    }
    phi->GetBlock()->InsertPhiAfter(new_phi, phi);
    return new_phi;
  } else {
    DCHECK_EQ(next->GetType(), type);
    return next->AsPhi();
  }
}

HInstruction* SsaBuilder::GetFloatOrDoubleEquivalent(HInstruction* user,
                                                     HInstruction* value,
                                                     Primitive::Type type) {
  if (value->IsArrayGet()) {
    // The verifier has checked that values in arrays cannot be used for both
    // floating point and non-floating point operations. It is therefore safe to just
    // change the type of the operation.
    value->AsArrayGet()->SetType(type);
    return value;
  } else if (value->IsLongConstant()) {
    return GetDoubleEquivalent(value->AsLongConstant());
  } else if (value->IsIntConstant()) {
    return GetFloatEquivalent(value->AsIntConstant());
  } else if (value->IsPhi()) {
    return GetFloatDoubleOrReferenceEquivalentOfPhi(value->AsPhi(), type);
  } else {
    // For other instructions, we assume the verifier has checked that the dex format is correctly
    // typed and the value in a dex register will not be used for both floating point and
    // non-floating point operations. So the only reason an instruction would want a floating
    // point equivalent is for an unused phi that will be removed by the dead phi elimination phase.
    DCHECK(user->IsPhi());
    return value;
  }
}

HInstruction* SsaBuilder::GetReferenceTypeEquivalent(HInstruction* value) {
  if (value->IsIntConstant()) {
    DCHECK_EQ(value->AsIntConstant()->GetValue(), 0);
    return value->GetBlock()->GetGraph()->GetNullConstant();
  } else {
    DCHECK(value->IsPhi());
    return GetFloatDoubleOrReferenceEquivalentOfPhi(value->AsPhi(), Primitive::kPrimNot);
  }
}

void SsaBuilder::VisitLoadLocal(HLoadLocal* load) {
  HInstruction* value = current_locals_->GetInstructionAt(load->GetLocal()->GetRegNumber());
  // If the operation requests a specific type, we make sure its input is of that type.
  if (load->GetType() != value->GetType()) {
    if (load->GetType() == Primitive::kPrimFloat || load->GetType() == Primitive::kPrimDouble) {
      value = GetFloatOrDoubleEquivalent(load, value, load->GetType());
    } else if (load->GetType() == Primitive::kPrimNot) {
      value = GetReferenceTypeEquivalent(value);
    }
  }
  load->ReplaceWith(value);
  load->GetBlock()->RemoveInstruction(load);
}

void SsaBuilder::VisitStoreLocal(HStoreLocal* store) {
  current_locals_->SetRawEnvAt(store->GetLocal()->GetRegNumber(), store->InputAt(1));
  store->GetBlock()->RemoveInstruction(store);
}

void SsaBuilder::VisitInstruction(HInstruction* instruction) {
  if (!instruction->NeedsEnvironment()) {
    return;
  }
  HEnvironment* environment = new (GetGraph()->GetArena()) HEnvironment(
      GetGraph()->GetArena(), current_locals_->Size());
  environment->CopyFrom(current_locals_);
  instruction->SetEnvironment(environment);
}

void SsaBuilder::VisitTemporary(HTemporary* temp) {
  // Temporaries are only used by the baseline register allocator.
  temp->GetBlock()->RemoveInstruction(temp);
}

}  // namespace art
