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

#include "reference_type_propagation.h"

#include "class_linker.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "scoped_thread_state_change.h"

namespace art {

// TODO: handle: a !=/== null.

void ReferenceTypePropagation::Run() {
  // To properly propagate type info we need to visit in the dominator-based order.
  // Reverse post order guarantees a node's dominators are visited first.
  // We take advantage of this order in `VisitBasicBlock`.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
  ProcessWorklist();
}

void ReferenceTypePropagation::VisitBasicBlock(HBasicBlock* block) {
  // TODO: handle other instructions that give type info
  // (NewArray/Call/Field accesses/array accesses)

  // Initialize exact types first for faster convergence.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instr = it.Current();
    if (instr->IsNewInstance()) {
      VisitNewInstance(instr->AsNewInstance());
    } else if (instr->IsLoadClass()) {
      VisitLoadClass(instr->AsLoadClass());
    }
  }

  // Handle Phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    VisitPhi(it.Current()->AsPhi());
  }

  // Add extra nodes to bound types.
  BoundTypeForIfInstanceOf(block);
}

// Detects if `block` is the True block for the pattern
// `if (x instanceof ClassX) { }`
// If that's the case insert an HBoundType instruction to bound the type of `x`
// to `ClassX` in the scope of the dominated blocks.
void ReferenceTypePropagation::BoundTypeForIfInstanceOf(HBasicBlock* block) {
  HInstruction* lastInstruction = block->GetLastInstruction();
  if (!lastInstruction->IsIf()) {
    return;
  }
  HInstruction* ifInput = lastInstruction->InputAt(0);
  // TODO: Handle more patterns here: HIf(bool) HIf(HNotEqual).
  if (!ifInput->IsEqual()) {
    return;
  }
  HInstruction* instanceOf = ifInput->InputAt(0);
  HInstruction* comp_value = ifInput->InputAt(1);
  if (!instanceOf->IsInstanceOf() || !comp_value->IsIntConstant()) {
    return;
  }

  HInstruction* obj = instanceOf->InputAt(0);
  HLoadClass* load_class = instanceOf->InputAt(1)->AsLoadClass();

  ReferenceTypeInfo obj_rti = obj->GetReferenceTypeInfo();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  HBoundType* bound_type = new (graph_->GetArena()) HBoundType(obj, class_rti);

  // Narrow the type as much as possible.
  {
    ScopedObjectAccess soa(Thread::Current());
    if (!load_class->IsResolved() || class_rti.IsSupertypeOf(obj_rti)) {
      bound_type->SetReferenceTypeInfo(obj_rti);
    } else {
      bound_type->SetReferenceTypeInfo(
          ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ false));
    }
  }

  block->InsertInstructionBefore(bound_type, lastInstruction);
  // Pick the right successor based on the value we compare against.
  HIntConstant* comp_value_int = comp_value->AsIntConstant();
  HBasicBlock* instanceOfTrueBlock = comp_value_int->GetValue() == 0
      ? lastInstruction->AsIf()->IfFalseSuccessor()
      : lastInstruction->AsIf()->IfTrueSuccessor();

  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (instanceOfTrueBlock->Dominates(user->GetBlock())) {
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
}

void ReferenceTypePropagation::VisitNewInstance(HNewInstance* instr) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = dex_compilation_unit_.GetClassLinker()->FindDexCache(dex_file_);
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(instr->GetTypeIndex());
  if (resolved_class != nullptr) {
    MutableHandle<mirror::Class> handle = handles_->NewHandle(resolved_class);
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(handle, true));
  }
}

void ReferenceTypePropagation::VisitLoadClass(HLoadClass* instr) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = dex_compilation_unit_.GetClassLinker()->FindDexCache(dex_file_);
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(instr->GetTypeIndex());
  if (resolved_class != nullptr) {
    Handle<mirror::Class> handle = handles_->NewHandle(resolved_class);
    instr->SetLoadedClassRTI(ReferenceTypeInfo::Create(handle, /* is_exact */ true));
  }
  Handle<mirror::Class> class_handle = handles_->NewHandle(mirror::Class::GetJavaLangClass());
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(class_handle, /* is_exact */ true));
}

void ReferenceTypePropagation::VisitPhi(HPhi* phi) {
  if (phi->GetType() != Primitive::kPrimNot) {
    return;
  }

  if (phi->GetBlock()->IsLoopHeader()) {
    // Set the initial type for the phi. Use the non back edge input for reaching
    // a fixed point faster.
    AddToWorklist(phi);
    phi->SetCanBeNull(phi->InputAt(0)->CanBeNull());
    phi->SetReferenceTypeInfo(phi->InputAt(0)->GetReferenceTypeInfo());
  } else {
    // Eagerly compute the type of the phi, for quicker convergence. Note
    // that we don't need to add users to the worklist because we are
    // doing a reverse post-order visit, therefore either the phi users are
    // non-loop phi and will be visited later in the visit, or are loop-phis,
    // and they are already in the work list.
    UpdateNullability(phi);
    UpdateReferenceTypeInfo(phi);
  }
}

ReferenceTypeInfo ReferenceTypePropagation::MergeTypes(const ReferenceTypeInfo& a,
                                                       const ReferenceTypeInfo& b) {
  bool is_exact = a.IsExact() && b.IsExact();
  bool is_top = a.IsTop() || b.IsTop();
  Handle<mirror::Class> type_handle;

  if (!is_top) {
    if (a.GetTypeHandle().Get() == b.GetTypeHandle().Get()) {
      type_handle = a.GetTypeHandle();
    } else if (a.IsSupertypeOf(b)) {
      type_handle = a.GetTypeHandle();
      is_exact = false;
    } else if (b.IsSupertypeOf(a)) {
      type_handle = b.GetTypeHandle();
      is_exact = false;
    } else {
      // TODO: Find a common super class.
      is_top = true;
      is_exact = false;
    }
  }

  return is_top
      ? ReferenceTypeInfo::CreateTop(is_exact)
      : ReferenceTypeInfo::Create(type_handle, is_exact);
}

bool ReferenceTypePropagation::UpdateReferenceTypeInfo(HInstruction* instr) {
  ScopedObjectAccess soa(Thread::Current());

  ReferenceTypeInfo previous_rti = instr->GetReferenceTypeInfo();
  if (instr->IsBoundType()) {
    UpdateBoundType(instr->AsBoundType());
  } else if (instr->IsPhi()) {
    UpdatePhi(instr->AsPhi());
  } else {
    LOG(FATAL) << "Invalid instruction (should not get here)";
  }

  return !previous_rti.IsEqual(instr->GetReferenceTypeInfo());
}

void ReferenceTypePropagation::UpdateBoundType(HBoundType* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  // Be sure that we don't go over the bounded type.
  ReferenceTypeInfo bound_rti = instr->GetBoundType();
  if (!bound_rti.IsSupertypeOf(new_rti)) {
    new_rti = bound_rti;
  }
  instr->SetReferenceTypeInfo(new_rti);
}

void ReferenceTypePropagation::UpdatePhi(HPhi* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  if (new_rti.IsTop() && !new_rti.IsExact()) {
    // Early return if we are Top and inexact.
    instr->SetReferenceTypeInfo(new_rti);
    return;
  }
  for (size_t i = 1; i < instr->InputCount(); i++) {
    new_rti = MergeTypes(new_rti, instr->InputAt(i)->GetReferenceTypeInfo());
    if (new_rti.IsTop()) {
      if (!new_rti.IsExact()) {
        break;
      } else {
        continue;
      }
    }
  }
  instr->SetReferenceTypeInfo(new_rti);
}

// Re-computes and updates the nullability of the instruction. Returns whether or
// not the nullability was changed.
bool ReferenceTypePropagation::UpdateNullability(HInstruction* instr) {
  DCHECK(instr->IsPhi() || instr->IsBoundType());

  if (!instr->IsPhi()) {
    return false;
  }

  HPhi* phi = instr->AsPhi();
  bool existing_can_be_null = phi->CanBeNull();
  bool new_can_be_null = false;
  for (size_t i = 0; i < phi->InputCount(); i++) {
    new_can_be_null |= phi->InputAt(i)->CanBeNull();
  }
  phi->SetCanBeNull(new_can_be_null);

  return existing_can_be_null != new_can_be_null;
}

void ReferenceTypePropagation::ProcessWorklist() {
  while (!worklist_.IsEmpty()) {
    HInstruction* instruction = worklist_.Pop();
    if (UpdateNullability(instruction) || UpdateReferenceTypeInfo(instruction)) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void ReferenceTypePropagation::AddToWorklist(HInstruction* instruction) {
  DCHECK_EQ(instruction->GetType(), Primitive::kPrimNot) << instruction->GetType();
  worklist_.Add(instruction);
}

void ReferenceTypePropagation::AddDependentInstructionsToWorklist(HInstruction* instruction) {
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (user->IsPhi() || user->IsBoundType()) {
      AddToWorklist(user);
    }
  }
}
}  // namespace art
