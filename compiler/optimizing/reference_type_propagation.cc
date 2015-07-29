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

#include "class_linker-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "scoped_thread_state_change.h"

namespace art {

class RTPVisitor : public HGraphDelegateVisitor {
 public:
  RTPVisitor(HGraph* graph, StackHandleScopeCollection* handles)
    : HGraphDelegateVisitor(graph),
      handles_(handles) {}

  void VisitNewInstance(HNewInstance* new_instance) OVERRIDE;
  void VisitLoadClass(HLoadClass* load_class) OVERRIDE;
  void VisitNewArray(HNewArray* instr) OVERRIDE;
  void UpdateFieldAccessTypeInfo(HInstruction* instr, const FieldInfo& info);
  void SetClassAsTypeInfo(HInstruction* instr, mirror::Class* klass, bool is_exact);
  void VisitInstanceFieldGet(HInstanceFieldGet* instr) OVERRIDE;
  void VisitStaticFieldGet(HStaticFieldGet* instr) OVERRIDE;
  void VisitInvoke(HInvoke* instr) OVERRIDE;
  void VisitArrayGet(HArrayGet* instr) OVERRIDE;
  void VisitCheckCast(HCheckCast* instr) OVERRIDE;
  void UpdateReferenceTypeInfo(HInstruction* instr,
                               uint16_t type_idx,
                               const DexFile& dex_file,
                               bool is_exact);

 private:
  StackHandleScopeCollection* handles_;
};

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
  // (array accesses)

  RTPVisitor visitor(graph_, handles_);
  // Initialize exact types first for faster convergence.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instr = it.Current();
    instr->Accept(&visitor);
  }

  // Handle Phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    VisitPhi(it.Current()->AsPhi());
  }

  // Add extra nodes to bound types.
  BoundTypeForIfNotNull(block);
  BoundTypeForIfInstanceOf(block);
}

// Create a bound type for the given object narrowing the type as much as possible.
// The BoundType upper values for the super type and can_be_null will be taken from
// load_class.GetLoadedClassRTI() and upper_can_be_null.
static HBoundType* CreateBoundType(ArenaAllocator* arena,
                                   HInstruction* obj,
                                   HLoadClass* load_class,
                                   bool upper_can_be_null)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  ReferenceTypeInfo obj_rti = obj->GetReferenceTypeInfo();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  HBoundType* bound_type = new (arena) HBoundType(obj, class_rti, upper_can_be_null);
  // Narrow the type as much as possible.
  if (load_class->IsResolved() && class_rti.GetTypeHandle()->IsFinal()) {
    bound_type->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ true));
  } else if (!load_class->IsResolved() || class_rti.IsSupertypeOf(obj_rti)) {
    bound_type->SetReferenceTypeInfo(obj_rti);
  } else {
    bound_type->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ false));
  }
  return bound_type;
}

// Check if we should create a bound type for the given object at the specified
// position. Because of inlining and the fact we run RTP more than once and we
// might have a HBoundType already. If we do, we should not create a new one.
// In this case we also assert that there are no other uses of the object (except
// the bound type) dominated by the specified dominator_instr or dominator_block.
static bool ShouldCreateBoundType(HInstruction* position,
                                  HInstruction* obj,
                                  ReferenceTypeInfo upper_bound,
                                  HInstruction* dominator_instr,
                                  HBasicBlock* dominator_block)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // If the position where we should insert the bound type is not already a
  // a bound type then we need to create one.
  if (position == nullptr || !position->IsBoundType()) {
    return true;
  }

  HBoundType* existing_bound_type = position->AsBoundType();
  if (existing_bound_type->GetUpperBound().IsSupertypeOf(upper_bound)) {
    if (kIsDebugBuild) {
      // Check that the existing HBoundType dominates all the uses.
      for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
        HInstruction* user = it.Current()->GetUser();
        if (dominator_instr != nullptr) {
          DCHECK(!dominator_instr->StrictlyDominates(user)
              || user == existing_bound_type
              || existing_bound_type->StrictlyDominates(user));
        } else if (dominator_block != nullptr) {
          DCHECK(!dominator_block->Dominates(user->GetBlock())
              || user == existing_bound_type
              || existing_bound_type->StrictlyDominates(user));
        }
      }
    }
  } else {
    // TODO: if the current bound type is a refinement we could update the
    // existing_bound_type with the a new upper limit. However, we also need to
    // update its users and have access to the work list.
  }
  return false;
}

void ReferenceTypePropagation::BoundTypeForIfNotNull(HBasicBlock* block) {
  HIf* ifInstruction = block->GetLastInstruction()->AsIf();
  if (ifInstruction == nullptr) {
    return;
  }
  HInstruction* ifInput = ifInstruction->InputAt(0);
  if (!ifInput->IsNotEqual() && !ifInput->IsEqual()) {
    return;
  }
  HInstruction* input0 = ifInput->InputAt(0);
  HInstruction* input1 = ifInput->InputAt(1);
  HInstruction* obj = nullptr;

  if (input1->IsNullConstant()) {
    obj = input0;
  } else if (input0->IsNullConstant()) {
    obj = input1;
  } else {
    return;
  }

  if (!obj->CanBeNull() || obj->IsNullConstant()) {
    // Null check is dead code and will be removed by DCE.
    return;
  }
  DCHECK(!obj->IsLoadClass()) << "We should not replace HLoadClass instructions";

  // We only need to bound the type if we have uses in the relevant block.
  // So start with null and create the HBoundType lazily, only if it's needed.
  HBoundType* bound_type = nullptr;
  HBasicBlock* notNullBlock = ifInput->IsNotEqual()
      ? ifInstruction->IfTrueSuccessor()
      : ifInstruction->IfFalseSuccessor();

  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (notNullBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HInstruction* insert_point = notNullBlock->GetFirstInstruction();
        ReferenceTypeInfo object_rti = ReferenceTypeInfo::CreateTop(false);
        if (ShouldCreateBoundType(insert_point, obj, object_rti, nullptr, notNullBlock)) {
          bound_type = new (graph_->GetArena()) HBoundType(
              obj, object_rti, /* bound_can_be_null */ false);
          notNullBlock->InsertInstructionBefore(bound_type, insert_point);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
}

// Detects if `block` is the True block for the pattern
// `if (x instanceof ClassX) { }`
// If that's the case insert an HBoundType instruction to bound the type of `x`
// to `ClassX` in the scope of the dominated blocks.
void ReferenceTypePropagation::BoundTypeForIfInstanceOf(HBasicBlock* block) {
  HIf* ifInstruction = block->GetLastInstruction()->AsIf();
  if (ifInstruction == nullptr) {
    return;
  }
  HInstruction* ifInput = ifInstruction->InputAt(0);
  HInstruction* instanceOf = nullptr;
  HBasicBlock* instanceOfTrueBlock = nullptr;

  // The instruction simplifier has transformed:
  //   - `if (a instanceof A)` into an HIf with an HInstanceOf input
  //   - `if (!(a instanceof A)` into an HIf with an HBooleanNot input (which in turn
  //     has an HInstanceOf input)
  // So we should not see the usual HEqual here.
  if (ifInput->IsInstanceOf()) {
    instanceOf = ifInput;
    instanceOfTrueBlock = ifInstruction->IfTrueSuccessor();
  } else if (ifInput->IsBooleanNot() && ifInput->InputAt(0)->IsInstanceOf()) {
    instanceOf = ifInput->InputAt(0);
    instanceOfTrueBlock = ifInstruction->IfFalseSuccessor();
  } else {
    return;
  }

  // We only need to bound the type if we have uses in the relevant block.
  // So start with null and create the HBoundType lazily, only if it's needed.
  HBoundType* bound_type = nullptr;

  HInstruction* obj = instanceOf->InputAt(0);
  if (obj->GetReferenceTypeInfo().IsExact() && !obj->IsPhi()) {
    // This method is being called while doing a fixed-point calculation
    // over phis. Non-phis instruction whose type is already known do
    // not need to be bound to another type.
    // Not that this also prevents replacing `HLoadClass` with a `HBoundType`.
    // `HCheckCast` and `HInstanceOf` expect a `HLoadClass` as a second
    // input.
    return;
  }
  DCHECK(!obj->IsLoadClass()) << "We should not replace HLoadClass instructions";
  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (instanceOfTrueBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HLoadClass* load_class = instanceOf->InputAt(1)->AsLoadClass();
        ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
        HInstruction* insert_point = instanceOfTrueBlock->GetFirstInstruction();
        if (ShouldCreateBoundType(insert_point, obj, class_rti, nullptr, instanceOfTrueBlock)) {
          bound_type = CreateBoundType(
              graph_->GetArena(),
              obj,
              load_class,
              false /* InstanceOf ensures the object is not null. */);
          instanceOfTrueBlock->InsertInstructionBefore(bound_type, insert_point);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
}

void RTPVisitor::SetClassAsTypeInfo(HInstruction* instr,
                                    mirror::Class* klass,
                                    bool is_exact) {
  if (klass != nullptr) {
    ScopedObjectAccess soa(Thread::Current());
    MutableHandle<mirror::Class> handle = handles_->NewHandle(klass);
    is_exact = is_exact || klass->IsFinal();
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(handle, is_exact));
  }
}

void RTPVisitor::UpdateReferenceTypeInfo(HInstruction* instr,
                                         uint16_t type_idx,
                                         const DexFile& dex_file,
                                         bool is_exact) {
  DCHECK_EQ(instr->GetType(), Primitive::kPrimNot);

  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier.
  SetClassAsTypeInfo(instr, dex_cache->GetResolvedType(type_idx), is_exact);
}

void RTPVisitor::VisitNewInstance(HNewInstance* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void RTPVisitor::VisitNewArray(HNewArray* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void RTPVisitor::UpdateFieldAccessTypeInfo(HInstruction* instr,
                                           const FieldInfo& info) {
  // The field index is unknown only during tests.
  if (instr->GetType() != Primitive::kPrimNot || info.GetFieldIndex() == kUnknownFieldIndex) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  mirror::DexCache* dex_cache = cl->FindDexCache(info.GetDexFile());
  ArtField* field = cl->GetResolvedField(info.GetFieldIndex(), dex_cache);
  if (field != nullptr) {
    mirror::Class* klass = field->GetType<false>();
    SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
  }
}

void RTPVisitor::VisitInstanceFieldGet(HInstanceFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void RTPVisitor::VisitStaticFieldGet(HStaticFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void RTPVisitor::VisitLoadClass(HLoadClass* instr) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache =
      Runtime::Current()->GetClassLinker()->FindDexCache(instr->GetDexFile());
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(instr->GetTypeIndex());
  if (resolved_class != nullptr) {
    Handle<mirror::Class> handle = handles_->NewHandle(resolved_class);
    instr->SetLoadedClassRTI(ReferenceTypeInfo::Create(handle, /* is_exact */ true));
  }
  Handle<mirror::Class> class_handle = handles_->NewHandle(mirror::Class::GetJavaLangClass());
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(class_handle, /* is_exact */ true));
}

void RTPVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HInstruction* obj = check_cast->InputAt(0);
  HBoundType* bound_type = nullptr;
  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (check_cast->StrictlyDominates(user)) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HLoadClass* load_class = check_cast->InputAt(1)->AsLoadClass();
        ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
        if (ShouldCreateBoundType(check_cast->GetNext(), obj, class_rti, check_cast, nullptr)) {
          bound_type = CreateBoundType(
              GetGraph()->GetArena(),
              obj,
              load_class,
              true /* CheckCast succeeds for nulls. */);
          check_cast->GetBlock()->InsertInstructionAfter(bound_type, check_cast);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
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

void RTPVisitor::VisitInvoke(HInvoke* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  mirror::DexCache* dex_cache = cl->FindDexCache(instr->GetDexFile());
  ArtMethod* method = dex_cache->GetResolvedMethod(
      instr->GetDexMethodIndex(), cl->GetImagePointerSize());
  if (method != nullptr) {
    mirror::Class* klass = method->GetReturnType(false);
    SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
  }
}

void RTPVisitor::VisitArrayGet(HArrayGet* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  HInstruction* parent = instr->InputAt(0);
  ScopedObjectAccess soa(Thread::Current());
  Handle<mirror::Class> handle = parent->GetReferenceTypeInfo().GetTypeHandle();
  if (handle.GetReference() != nullptr && handle->IsObjectArrayClass()) {
    SetClassAsTypeInfo(instr, handle->GetComponentType(), /* is_exact */ false);
  }
}

void ReferenceTypePropagation::UpdateBoundType(HBoundType* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  // Be sure that we don't go over the bounded type.
  ReferenceTypeInfo upper_bound_rti = instr->GetUpperBound();
  if (!upper_bound_rti.IsSupertypeOf(new_rti)) {
    new_rti = upper_bound_rti;
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

  bool existing_can_be_null = instr->CanBeNull();
  if (instr->IsPhi()) {
    HPhi* phi = instr->AsPhi();
    bool new_can_be_null = false;
    for (size_t i = 0; i < phi->InputCount(); i++) {
      if (phi->InputAt(i)->CanBeNull()) {
        new_can_be_null = true;
        break;
      }
    }
    phi->SetCanBeNull(new_can_be_null);
  } else if (instr->IsBoundType()) {
    HBoundType* bound_type = instr->AsBoundType();
    bound_type->SetCanBeNull(instr->InputAt(0)->CanBeNull() && bound_type->GetUpperCanBeNull());
  }
  return existing_can_be_null != instr->CanBeNull();
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
