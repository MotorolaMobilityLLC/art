/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "stack.h"

#include "arch/context.h"
#include "base/hex_dump.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "quick/quick_method_frame_info.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "throw_location.h"
#include "verify_object-inl.h"
#include "vmap_table.h"

namespace art {

mirror::Object* ShadowFrame::GetThisObject() const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return NULL;
  } else if (m->IsNative()) {
    return GetVRegReference(0);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    CHECK(code_item != NULL) << PrettyMethod(m);
    uint16_t reg = code_item->registers_size_ - code_item->ins_size_;
    return GetVRegReference(reg);
  }
}

mirror::Object* ShadowFrame::GetThisObject(uint16_t num_ins) const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return NULL;
  } else {
    return GetVRegReference(NumberOfVRegs() - num_ins);
  }
}

ThrowLocation ShadowFrame::GetCurrentLocationForThrow() const {
  return ThrowLocation(GetThisObject(), GetMethod(), GetDexPC());
}

size_t ManagedStack::NumJniShadowFrameReferences() const {
  size_t count = 0;
  for (const ManagedStack* current_fragment = this; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    for (ShadowFrame* current_frame = current_fragment->top_shadow_frame_; current_frame != NULL;
         current_frame = current_frame->GetLink()) {
      if (current_frame->GetMethod()->IsNative()) {
        // The JNI ShadowFrame only contains references. (For indirect reference.)
        count += current_frame->NumberOfVRegs();
      }
    }
  }
  return count;
}

bool ManagedStack::ShadowFramesContain(StackReference<mirror::Object>* shadow_frame_entry) const {
  for (const ManagedStack* current_fragment = this; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    for (ShadowFrame* current_frame = current_fragment->top_shadow_frame_; current_frame != NULL;
         current_frame = current_frame->GetLink()) {
      if (current_frame->Contains(shadow_frame_entry)) {
        return true;
      }
    }
  }
  return false;
}

StackVisitor::StackVisitor(Thread* thread, Context* context)
    : thread_(thread), cur_shadow_frame_(NULL),
      cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(0), cur_depth_(0),
      context_(context) {
  DCHECK(thread == Thread::Current() || thread->IsSuspended()) << *thread;
}

StackVisitor::StackVisitor(Thread* thread, Context* context, size_t num_frames)
    : thread_(thread), cur_shadow_frame_(NULL),
      cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(num_frames), cur_depth_(0),
      context_(context) {
  DCHECK(thread == Thread::Current() || thread->IsSuspended()) << *thread;
}

uint32_t StackVisitor::GetDexPc(bool abort_on_failure) const {
  if (cur_shadow_frame_ != NULL) {
    return cur_shadow_frame_->GetDexPC();
  } else if (cur_quick_frame_ != NULL) {
    return GetMethod()->ToDexPc(cur_quick_frame_pc_, abort_on_failure);
  } else {
    return 0;
  }
}

extern "C" mirror::Object* artQuickGetProxyThisObject(StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

mirror::Object* StackVisitor::GetThisObject() const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return nullptr;
  } else if (m->IsNative()) {
    if (cur_quick_frame_ != nullptr) {
      HandleScope* hs = reinterpret_cast<HandleScope*>(
          reinterpret_cast<char*>(cur_quick_frame_) + m->GetHandleScopeOffset().SizeValue());
      return hs->GetReference(0);
    } else {
      return cur_shadow_frame_->GetVRegReference(0);
    }
  } else if (m->IsProxyMethod()) {
    if (cur_quick_frame_ != nullptr) {
      return artQuickGetProxyThisObject(cur_quick_frame_);
    } else {
      return cur_shadow_frame_->GetVRegReference(0);
    }
  } else if (m->IsOptimized(GetInstructionSetPointerSize(
      Runtime::Current()->GetInstructionSet()))) {
    // TODO: Implement, currently only used for exceptions when jdwp is enabled.
    UNIMPLEMENTED(WARNING)
        << "StackVisitor::GetThisObject is unimplemented with the optimizing compiler";
    return nullptr;
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    if (code_item == nullptr) {
      UNIMPLEMENTED(ERROR) << "Failed to determine this object of abstract or proxy method: "
          << PrettyMethod(m);
      return nullptr;
    } else {
      uint16_t reg = code_item->registers_size_ - code_item->ins_size_;
      return reinterpret_cast<mirror::Object*>(GetVReg(m, reg, kReferenceVReg));
    }
  }
}

size_t StackVisitor::GetNativePcOffset() const {
  DCHECK(!IsShadowFrame());
  return GetMethod()->NativeQuickPcOffset(cur_quick_frame_pc_);
}

bool StackVisitor::GetVReg(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind,
                           uint32_t* val) const {
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
    DCHECK(m == GetMethod());
    if (m->IsOptimized(sizeof(void*))) {
      return GetVRegFromOptimizedCode(m, vreg, kind, val);
    } else {
      return GetVRegFromQuickCode(m, vreg, kind, val);
    }
  } else {
    DCHECK(cur_shadow_frame_ != nullptr);
    *val = cur_shadow_frame_->GetVReg(vreg);
    return true;
  }
}

bool StackVisitor::GetVRegFromQuickCode(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind,
                                        uint32_t* val) const {
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  const VmapTable vmap_table(m->GetVmapTable(code_pointer, sizeof(void*)));
  QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
  uint32_t vmap_offset;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, kind, &vmap_offset)) {
    bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
    uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
    uint32_t reg = vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
    return GetRegisterIfAccessible(reg, kind, val);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                      // its instructions?
    *val = *GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                        frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
    return true;
  }
}

bool StackVisitor::GetVRegFromOptimizedCode(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind,
                                            uint32_t* val) const {
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  uint32_t native_pc_offset = m->NativeQuickPcOffset(cur_quick_frame_pc_);
  CodeInfo code_info = m->GetOptimizedCodeInfo();
  StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset);
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                    // its instructions?
  DCHECK_LT(vreg, code_item->registers_size_);
  DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(stack_map,
                                                                  code_item->registers_size_);
  DexRegisterMap::LocationKind location_kind = dex_register_map.GetLocationKind(vreg);
  switch (location_kind) {
    case DexRegisterMap::kInStack: {
      const int32_t offset = dex_register_map.GetStackOffsetInBytes(vreg);
      const uint8_t* addr = reinterpret_cast<const uint8_t*>(cur_quick_frame_) + offset;
      *val = *reinterpret_cast<const uint32_t*>(addr);
      return true;
    }
    case DexRegisterMap::kInRegister:
    case DexRegisterMap::kInFpuRegister: {
      uint32_t reg = dex_register_map.GetMachineRegister(vreg);
      return GetRegisterIfAccessible(reg, kind, val);
    }
    case DexRegisterMap::kConstant:
      *val = dex_register_map.GetConstant(vreg);
      return true;
    case DexRegisterMap::kNone:
      return false;
  }
  UNREACHABLE();
  return false;
}

bool StackVisitor::GetRegisterIfAccessible(uint32_t reg, VRegKind kind, uint32_t* val) const {
  const bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
  if (!IsAccessibleRegister(reg, is_float)) {
    return false;
  }
  uintptr_t ptr_val = GetRegister(reg, is_float);
  const bool target64 = Is64BitInstructionSet(kRuntimeISA);
  if (target64) {
    const bool wide_lo = (kind == kLongLoVReg) || (kind == kDoubleLoVReg);
    const bool wide_hi = (kind == kLongHiVReg) || (kind == kDoubleHiVReg);
    int64_t value_long = static_cast<int64_t>(ptr_val);
    if (wide_lo) {
      ptr_val = static_cast<uintptr_t>(Low32Bits(value_long));
    } else if (wide_hi) {
      ptr_val = static_cast<uintptr_t>(High32Bits(value_long));
    }
  }
  *val = ptr_val;
  return true;
}

bool StackVisitor::GetVRegPair(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind_lo,
                               VRegKind kind_hi, uint64_t* val) const {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
    UNREACHABLE();
  }
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
    DCHECK(m == GetMethod());
    if (m->IsOptimized(sizeof(void*))) {
      return GetVRegPairFromOptimizedCode(m, vreg, kind_lo, kind_hi, val);
    } else {
      return GetVRegPairFromQuickCode(m, vreg, kind_lo, kind_hi, val);
    }
  } else {
    DCHECK(cur_shadow_frame_ != nullptr);
    *val = cur_shadow_frame_->GetVRegLong(vreg);
    return true;
  }
}

bool StackVisitor::GetVRegPairFromQuickCode(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind_lo,
                                            VRegKind kind_hi, uint64_t* val) const {
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  const VmapTable vmap_table(m->GetVmapTable(code_pointer, sizeof(void*)));
  QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
  uint32_t vmap_offset_lo, vmap_offset_hi;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, kind_lo, &vmap_offset_lo) &&
      vmap_table.IsInContext(vreg + 1, kind_hi, &vmap_offset_hi)) {
    bool is_float = (kind_lo == kDoubleLoVReg);
    uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
    uint32_t reg_lo = vmap_table.ComputeRegister(spill_mask, vmap_offset_lo, kind_lo);
    uint32_t reg_hi = vmap_table.ComputeRegister(spill_mask, vmap_offset_hi, kind_hi);
    return GetRegisterPairIfAccessible(reg_lo, reg_hi, kind_lo, val);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                      // its instructions?
    uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                 frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
    *val = *reinterpret_cast<uint64_t*>(addr);
    return true;
  }
}

bool StackVisitor::GetVRegPairFromOptimizedCode(mirror::ArtMethod* m, uint16_t vreg,
                                                VRegKind kind_lo, VRegKind kind_hi,
                                                uint64_t* val) const {
  uint32_t low_32bits;
  uint32_t high_32bits;
  bool success = GetVRegFromOptimizedCode(m, vreg, kind_lo, &low_32bits);
  success &= GetVRegFromOptimizedCode(m, vreg + 1, kind_hi, &high_32bits);
  if (success) {
    *val = (static_cast<uint64_t>(high_32bits) << 32) | static_cast<uint64_t>(low_32bits);
  }
  return success;
}

bool StackVisitor::GetRegisterPairIfAccessible(uint32_t reg_lo, uint32_t reg_hi,
                                               VRegKind kind_lo, uint64_t* val) const {
  const bool is_float = (kind_lo == kDoubleLoVReg);
  if (!IsAccessibleRegister(reg_lo, is_float) || !IsAccessibleRegister(reg_hi, is_float)) {
    return false;
  }
  uintptr_t ptr_val_lo = GetRegister(reg_lo, is_float);
  uintptr_t ptr_val_hi = GetRegister(reg_hi, is_float);
  bool target64 = Is64BitInstructionSet(kRuntimeISA);
  if (target64) {
    int64_t value_long_lo = static_cast<int64_t>(ptr_val_lo);
    int64_t value_long_hi = static_cast<int64_t>(ptr_val_hi);
    ptr_val_lo = static_cast<uintptr_t>(Low32Bits(value_long_lo));
    ptr_val_hi = static_cast<uintptr_t>(High32Bits(value_long_hi));
  }
  *val = (static_cast<uint64_t>(ptr_val_hi) << 32) | static_cast<uint32_t>(ptr_val_lo);
  return true;
}

bool StackVisitor::SetVReg(mirror::ArtMethod* m, uint16_t vreg, uint32_t new_value,
                           VRegKind kind) {
  if (cur_quick_frame_ != nullptr) {
      DCHECK(context_ != nullptr);  // You can't reliably write registers without a context.
      DCHECK(m == GetMethod());
      if (m->IsOptimized(sizeof(void*))) {
        return SetVRegFromOptimizedCode(m, vreg, new_value, kind);
      } else {
        return SetVRegFromQuickCode(m, vreg, new_value, kind);
      }
    } else {
      cur_shadow_frame_->SetVReg(vreg, new_value);
      return true;
    }
}

bool StackVisitor::SetVRegFromQuickCode(mirror::ArtMethod* m, uint16_t vreg, uint32_t new_value,
                                        VRegKind kind) {
  DCHECK(context_ != nullptr);  // You can't reliably write registers without a context.
  DCHECK(m == GetMethod());
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  const VmapTable vmap_table(m->GetVmapTable(code_pointer, sizeof(void*)));
  QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
  uint32_t vmap_offset;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, kind, &vmap_offset)) {
    bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
    uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
    uint32_t reg = vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
    return SetRegisterIfAccessible(reg, new_value, kind);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                      // its instructions?
    uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                 frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
    *addr = new_value;
    return true;
  }
}

bool StackVisitor::SetVRegFromOptimizedCode(mirror::ArtMethod* m, uint16_t vreg, uint32_t new_value,
                                            VRegKind kind) {
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  uint32_t native_pc_offset = m->NativeQuickPcOffset(cur_quick_frame_pc_);
  CodeInfo code_info = m->GetOptimizedCodeInfo();
  StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset);
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                    // its instructions?
  DCHECK_LT(vreg, code_item->registers_size_);
  DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(stack_map,
                                                                  code_item->registers_size_);
  DexRegisterMap::LocationKind location_kind = dex_register_map.GetLocationKind(vreg);
  uint32_t dex_pc = m->ToDexPc(cur_quick_frame_pc_, false);
  switch (location_kind) {
    case DexRegisterMap::kInStack: {
      const int32_t offset = dex_register_map.GetStackOffsetInBytes(vreg);
      uint8_t* addr = reinterpret_cast<uint8_t*>(cur_quick_frame_) + offset;
      *reinterpret_cast<uint32_t*>(addr) = new_value;
      return true;
    }
    case DexRegisterMap::kInRegister:
    case DexRegisterMap::kInFpuRegister: {
      uint32_t reg = dex_register_map.GetMachineRegister(vreg);
      return SetRegisterIfAccessible(reg, new_value, kind);
    }
    case DexRegisterMap::kConstant:
      LOG(ERROR) << StringPrintf("Cannot change value of DEX register v%u used as a constant at "
                                 "DEX pc 0x%x (native pc 0x%x) of method %s",
                                 vreg, dex_pc, native_pc_offset,
                                 PrettyMethod(cur_quick_frame_->AsMirrorPtr()).c_str());
      return false;
    case DexRegisterMap::kNone:
      LOG(ERROR) << StringPrintf("No location for DEX register v%u at DEX pc 0x%x "
                                 "(native pc 0x%x) of method %s",
                                 vreg, dex_pc, native_pc_offset,
                                 PrettyMethod(cur_quick_frame_->AsMirrorPtr()).c_str());
      return false;
    default:
      LOG(FATAL) << StringPrintf("Unknown location for DEX register v%u at DEX pc 0x%x "
                                 "(native pc 0x%x) of method %s",
                                 vreg, dex_pc, native_pc_offset,
                                 PrettyMethod(cur_quick_frame_->AsMirrorPtr()).c_str());
      UNREACHABLE();
  }
}

bool StackVisitor::SetRegisterIfAccessible(uint32_t reg, uint32_t new_value, VRegKind kind) {
  const bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
  if (!IsAccessibleRegister(reg, is_float)) {
    return false;
  }
  const bool target64 = Is64BitInstructionSet(kRuntimeISA);

  // Create a new value that can hold both low 32 and high 32 bits, in
  // case we are running 64 bits.
  uintptr_t full_new_value = new_value;
  // Deal with 32 or 64-bit wide registers in a way that builds on all targets.
  if (target64) {
    bool wide_lo = (kind == kLongLoVReg) || (kind == kDoubleLoVReg);
    bool wide_hi = (kind == kLongHiVReg) || (kind == kDoubleHiVReg);
    if (wide_lo || wide_hi) {
      uintptr_t old_reg_val = GetRegister(reg, is_float);
      uint64_t new_vreg_portion = static_cast<uint64_t>(new_value);
      uint64_t old_reg_val_as_wide = static_cast<uint64_t>(old_reg_val);
      uint64_t mask = 0xffffffff;
      if (wide_lo) {
        mask = mask << 32;
      } else {
        new_vreg_portion = new_vreg_portion << 32;
      }
      full_new_value = static_cast<uintptr_t>((old_reg_val_as_wide & mask) | new_vreg_portion);
    }
  }
  SetRegister(reg, full_new_value, is_float);
  return true;
}

bool StackVisitor::SetVRegPair(mirror::ArtMethod* m, uint16_t vreg, uint64_t new_value,
                               VRegKind kind_lo, VRegKind kind_hi) {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
  }
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably write registers without a context.
    DCHECK(m == GetMethod());
    if (m->IsOptimized(sizeof(void*))) {
      return SetVRegPairFromOptimizedCode(m, vreg, new_value, kind_lo, kind_hi);
    } else {
      return SetVRegPairFromQuickCode(m, vreg, new_value, kind_lo, kind_hi);
    }
  } else {
    DCHECK(cur_shadow_frame_ != nullptr);
    cur_shadow_frame_->SetVRegLong(vreg, new_value);
    return true;
  }
}

bool StackVisitor::SetVRegPairFromQuickCode(mirror::ArtMethod* m, uint16_t vreg, uint64_t new_value,
                                            VRegKind kind_lo, VRegKind kind_hi) {
  const void* code_pointer = m->GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  const VmapTable vmap_table(m->GetVmapTable(code_pointer, sizeof(void*)));
  QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
  uint32_t vmap_offset_lo, vmap_offset_hi;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, kind_lo, &vmap_offset_lo) &&
      vmap_table.IsInContext(vreg + 1, kind_hi, &vmap_offset_hi)) {
    bool is_float = (kind_lo == kDoubleLoVReg);
    uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
    uint32_t reg_lo = vmap_table.ComputeRegister(spill_mask, vmap_offset_lo, kind_lo);
    uint32_t reg_hi = vmap_table.ComputeRegister(spill_mask, vmap_offset_hi, kind_hi);
    return SetRegisterPairIfAccessible(reg_lo, reg_hi, new_value, is_float);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                      // its instructions?
    uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                 frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
    *reinterpret_cast<uint64_t*>(addr) = new_value;
    return true;
  }
}

bool StackVisitor::SetVRegPairFromOptimizedCode(mirror::ArtMethod* m, uint16_t vreg, uint64_t new_value,
                                                VRegKind kind_lo, VRegKind kind_hi) {
  uint32_t low_32bits = Low32Bits(new_value);
  uint32_t high_32bits = High32Bits(new_value);
  bool success = SetVRegFromOptimizedCode(m, vreg, low_32bits, kind_lo);
  success &= SetVRegFromOptimizedCode(m, vreg + 1, high_32bits, kind_hi);
  return success;
}

bool StackVisitor::SetRegisterPairIfAccessible(uint32_t reg_lo, uint32_t reg_hi,
                                               uint64_t new_value, bool is_float) {
  if (!IsAccessibleRegister(reg_lo, is_float) || !IsAccessibleRegister(reg_hi, is_float)) {
    return false;
  }
  uintptr_t new_value_lo = static_cast<uintptr_t>(new_value & 0xFFFFFFFF);
  uintptr_t new_value_hi = static_cast<uintptr_t>(new_value >> 32);
  bool target64 = Is64BitInstructionSet(kRuntimeISA);
  // Deal with 32 or 64-bit wide registers in a way that builds on all targets.
  if (target64) {
    DCHECK_EQ(reg_lo, reg_hi);
    SetRegister(reg_lo, new_value, is_float);
  } else {
    SetRegister(reg_lo, new_value_lo, is_float);
    SetRegister(reg_hi, new_value_hi, is_float);
  }
  return true;
}

bool StackVisitor::IsAccessibleGPR(uint32_t reg) const {
  DCHECK(context_ != nullptr);
  return context_->IsAccessibleGPR(reg);
}

uintptr_t* StackVisitor::GetGPRAddress(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetGPRAddress(reg);
}

uintptr_t StackVisitor::GetGPR(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetGPR(reg);
}

void StackVisitor::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  context_->SetGPR(reg, value);
}

bool StackVisitor::IsAccessibleFPR(uint32_t reg) const {
  DCHECK(context_ != nullptr);
  return context_->IsAccessibleFPR(reg);
}

uintptr_t StackVisitor::GetFPR(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetFPR(reg);
}

void StackVisitor::SetFPR(uint32_t reg, uintptr_t value) {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  context_->SetFPR(reg, value);
}

uintptr_t StackVisitor::GetReturnPc() const {
  uint8_t* sp = reinterpret_cast<uint8_t*>(GetCurrentQuickFrame());
  DCHECK(sp != NULL);
  uint8_t* pc_addr = sp + GetMethod()->GetReturnPcOffset().SizeValue();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

void StackVisitor::SetReturnPc(uintptr_t new_ret_pc) {
  uint8_t* sp = reinterpret_cast<uint8_t*>(GetCurrentQuickFrame());
  CHECK(sp != NULL);
  uint8_t* pc_addr = sp + GetMethod()->GetReturnPcOffset().SizeValue();
  *reinterpret_cast<uintptr_t*>(pc_addr) = new_ret_pc;
}

size_t StackVisitor::ComputeNumFrames(Thread* thread) {
  struct NumFramesVisitor : public StackVisitor {
    explicit NumFramesVisitor(Thread* thread_in)
        : StackVisitor(thread_in, NULL), frames(0) {}

    bool VisitFrame() OVERRIDE {
      frames++;
      return true;
    }

    size_t frames;
  };
  NumFramesVisitor visitor(thread);
  visitor.WalkStack(true);
  return visitor.frames;
}

bool StackVisitor::GetNextMethodAndDexPc(mirror::ArtMethod** next_method, uint32_t* next_dex_pc) {
  struct HasMoreFramesVisitor : public StackVisitor {
    explicit HasMoreFramesVisitor(Thread* thread, size_t num_frames, size_t frame_height)
        : StackVisitor(thread, nullptr, num_frames), frame_height_(frame_height),
          found_frame_(false), has_more_frames_(false), next_method_(nullptr), next_dex_pc_(0) {
    }

    bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      if (found_frame_) {
        mirror::ArtMethod* method = GetMethod();
        if (method != nullptr && !method->IsRuntimeMethod()) {
          has_more_frames_ = true;
          next_method_ = method;
          next_dex_pc_ = GetDexPc();
          return false;  // End stack walk once next method is found.
        }
      } else if (GetFrameHeight() == frame_height_) {
        found_frame_ = true;
      }
      return true;
    }

    size_t frame_height_;
    bool found_frame_;
    bool has_more_frames_;
    mirror::ArtMethod* next_method_;
    uint32_t next_dex_pc_;
  };
  HasMoreFramesVisitor visitor(thread_, GetNumFrames(), GetFrameHeight());
  visitor.WalkStack(true);
  *next_method = visitor.next_method_;
  *next_dex_pc = visitor.next_dex_pc_;
  return visitor.has_more_frames_;
}

void StackVisitor::DescribeStack(Thread* thread) {
  struct DescribeStackVisitor : public StackVisitor {
    explicit DescribeStackVisitor(Thread* thread_in)
        : StackVisitor(thread_in, NULL) {}

    bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      LOG(INFO) << "Frame Id=" << GetFrameId() << " " << DescribeLocation();
      return true;
    }
  };
  DescribeStackVisitor visitor(thread);
  visitor.WalkStack(true);
}

std::string StackVisitor::DescribeLocation() const {
  std::string result("Visiting method '");
  mirror::ArtMethod* m = GetMethod();
  if (m == NULL) {
    return "upcall";
  }
  result += PrettyMethod(m);
  result += StringPrintf("' at dex PC 0x%04x", GetDexPc());
  if (!IsShadowFrame()) {
    result += StringPrintf(" (native PC %p)", reinterpret_cast<void*>(GetCurrentQuickFramePc()));
  }
  return result;
}

static instrumentation::InstrumentationStackFrame& GetInstrumentationStackFrame(Thread* thread,
                                                                                uint32_t depth) {
  CHECK_LT(depth, thread->GetInstrumentationStack()->size());
  return thread->GetInstrumentationStack()->at(depth);
}

void StackVisitor::SanityCheckFrame() const {
  if (kIsDebugBuild) {
    mirror::ArtMethod* method = GetMethod();
    CHECK_EQ(method->GetClass(), mirror::ArtMethod::GetJavaLangReflectArtMethod());
    if (cur_quick_frame_ != nullptr) {
      method->AssertPcIsWithinQuickCode(cur_quick_frame_pc_);
      // Frame sanity.
      size_t frame_size = method->GetFrameSizeInBytes();
      CHECK_NE(frame_size, 0u);
      // A rough guess at an upper size we expect to see for a frame.
      // 256 registers
      // 2 words HandleScope overhead
      // 3+3 register spills
      // TODO: this seems architecture specific for the case of JNI frames.
      // TODO: 083-compiler-regressions ManyFloatArgs shows this estimate is wrong.
      // const size_t kMaxExpectedFrameSize = (256 + 2 + 3 + 3) * sizeof(word);
      const size_t kMaxExpectedFrameSize = 2 * KB;
      CHECK_LE(frame_size, kMaxExpectedFrameSize);
      size_t return_pc_offset = method->GetReturnPcOffset().SizeValue();
      CHECK_LT(return_pc_offset, frame_size);
    }
  }
}

void StackVisitor::WalkStack(bool include_transitions) {
  DCHECK(thread_ == Thread::Current() || thread_->IsSuspended());
  CHECK_EQ(cur_depth_, 0U);
  bool exit_stubs_installed = Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled();
  uint32_t instrumentation_stack_depth = 0;

  for (const ManagedStack* current_fragment = thread_->GetManagedStack(); current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    cur_shadow_frame_ = current_fragment->GetTopShadowFrame();
    cur_quick_frame_ = current_fragment->GetTopQuickFrame();
    cur_quick_frame_pc_ = 0;

    if (cur_quick_frame_ != NULL) {  // Handle quick stack frames.
      // Can't be both a shadow and a quick fragment.
      DCHECK(current_fragment->GetTopShadowFrame() == NULL);
      mirror::ArtMethod* method = cur_quick_frame_->AsMirrorPtr();
      while (method != NULL) {
        SanityCheckFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }

        if (context_ != NULL) {
          context_->FillCalleeSaves(*this);
        }
        size_t frame_size = method->GetFrameSizeInBytes();
        // Compute PC for next stack frame from return PC.
        size_t return_pc_offset = method->GetReturnPcOffset(frame_size).SizeValue();
        uint8_t* return_pc_addr = reinterpret_cast<uint8_t*>(cur_quick_frame_) + return_pc_offset;
        uintptr_t return_pc = *reinterpret_cast<uintptr_t*>(return_pc_addr);
        if (UNLIKELY(exit_stubs_installed)) {
          // While profiling, the return pc is restored from the side stack, except when walking
          // the stack for an exception where the side stack will be unwound in VisitFrame.
          if (reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc()) == return_pc) {
            const instrumentation::InstrumentationStackFrame& instrumentation_frame =
                GetInstrumentationStackFrame(thread_, instrumentation_stack_depth);
            instrumentation_stack_depth++;
            if (GetMethod() == Runtime::Current()->GetCalleeSaveMethod(Runtime::kSaveAll)) {
              // Skip runtime save all callee frames which are used to deliver exceptions.
            } else if (instrumentation_frame.interpreter_entry_) {
              mirror::ArtMethod* callee = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
              CHECK_EQ(GetMethod(), callee) << "Expected: " << PrettyMethod(callee) << " Found: "
                                            << PrettyMethod(GetMethod());
            } else if (instrumentation_frame.method_ != GetMethod()) {
              LOG(FATAL)  << "Expected: " << PrettyMethod(instrumentation_frame.method_)
                          << " Found: " << PrettyMethod(GetMethod());
            }
            if (num_frames_ != 0) {
              // Check agreement of frame Ids only if num_frames_ is computed to avoid infinite
              // recursion.
              CHECK(instrumentation_frame.frame_id_ == GetFrameId())
                    << "Expected: " << instrumentation_frame.frame_id_
                    << " Found: " << GetFrameId();
            }
            return_pc = instrumentation_frame.return_pc_;
          }
        }
        cur_quick_frame_pc_ = return_pc;
        uint8_t* next_frame = reinterpret_cast<uint8_t*>(cur_quick_frame_) + frame_size;
        cur_quick_frame_ = reinterpret_cast<StackReference<mirror::ArtMethod>*>(next_frame);
        cur_depth_++;
        method = cur_quick_frame_->AsMirrorPtr();
      }
    } else if (cur_shadow_frame_ != NULL) {
      do {
        SanityCheckFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }
        cur_depth_++;
        cur_shadow_frame_ = cur_shadow_frame_->GetLink();
      } while (cur_shadow_frame_ != NULL);
    }
    if (include_transitions) {
      bool should_continue = VisitFrame();
      if (!should_continue) {
        return;
      }
    }
    cur_depth_++;
  }
  if (num_frames_ != 0) {
    CHECK_EQ(cur_depth_, num_frames_);
  }
}

void JavaFrameRootInfo::Describe(std::ostream& os) const {
  const StackVisitor* visitor = stack_visitor_;
  CHECK(visitor != nullptr);
  os << "Type=" << GetType() << " thread_id=" << GetThreadId() << " location=" <<
      visitor->DescribeLocation() << " vreg=" << vreg_;
}

}  // namespace art
