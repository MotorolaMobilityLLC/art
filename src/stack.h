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

#ifndef ART_SRC_STACK_H_
#define ART_SRC_STACK_H_

#include "dex_file.h"
#include "heap.h"
#include "instrumentation.h"
#include "jni.h"
#include "macros.h"
#include "oat/runtime/context.h"

#include <stdint.h>
#include <string>

namespace art {

class AbstractMethod;
class Context;
class Object;
class ShadowFrame;
class StackIndirectReferenceTable;
class ScopedObjectAccess;
class Thread;

// The kind of vreg being accessed in calls to Set/GetVReg.
enum VRegKind {
  kReferenceVReg,
  kIntVReg,
  kFloatVReg,
  kLongLoVReg,
  kLongHiVReg,
  kDoubleLoVReg,
  kDoubleHiVReg,
  kConstant,
  kImpreciseConstant,
  kUndefined,
};

class ShadowFrame {
 public:
  static ShadowFrame* Create(uint16_t num_refs, uint16_t num_vregs, ShadowFrame* link,
                             AbstractMethod* method, uint32_t dex_pc) {
    size_t sz = sizeof(ShadowFrame) + (sizeof(Object*) * num_refs) + (sizeof(uint32_t) * num_vregs);
    uint8_t* memory = new uint8_t[sz];
    return new (memory) ShadowFrame(num_refs, num_vregs, link, method, dex_pc);
  }
  ~ShadowFrame() {}

  uint32_t NumberOfReferences() const {
    return number_of_references_;
  }

  void SetNumberOfReferences(uint16_t number_of_references) {
    number_of_references_ = number_of_references;
  }

  void SetNumberOfVRegs(uint16_t number_of_vregs) {
    number_of_vregs_ = number_of_vregs;
  }

  uint32_t GetDexPC() const {
    return dex_pc_;
  }

  void SetDexPC(uint32_t dex_pc) {
    dex_pc_ = dex_pc;
  }

  ShadowFrame* GetLink() const {
    return link_;
  }

  void SetLink(ShadowFrame* frame) {
    DCHECK_NE(this, frame);
    link_ = frame;
  }

  Object* GetReference(size_t i) const {
    DCHECK_LT(i, number_of_references_);
    return references_[i];
  }

  void SetReference(size_t i, Object* object) {
    DCHECK_LT(i, number_of_references_);
    references_[i] = object;
  }

  int32_t GetVReg(size_t i) const {
    DCHECK_LT(i, number_of_vregs_);
    const int8_t* vregs = reinterpret_cast<const int8_t*>(this) + VRegsOffset();
    return reinterpret_cast<const int32_t*>(vregs)[i];
  }

  float GetVRegFloat(size_t i) const {
    DCHECK_LT(i, number_of_vregs_);
    const int8_t* vregs = reinterpret_cast<const int8_t*>(this) + VRegsOffset();
    return reinterpret_cast<const float*>(vregs)[i];
  }

  int64_t GetVRegLong(size_t i) const {
    const int8_t* vregs = reinterpret_cast<const int8_t*>(this) + VRegsOffset();
    const int32_t* low_half = &reinterpret_cast<const int32_t*>(vregs)[i];
    return *reinterpret_cast<const int64_t*>(low_half);
  }

  double GetVRegDouble(size_t i) const {
    const int8_t* vregs = reinterpret_cast<const int8_t*>(this) + VRegsOffset();
    const int32_t* low_half = &reinterpret_cast<const int32_t*>(vregs)[i];
    return *reinterpret_cast<const double*>(low_half);
  }

  void SetVReg(size_t i, int32_t val) {
    DCHECK_LT(i, number_of_vregs_);
    int8_t* vregs = reinterpret_cast<int8_t*>(this) + VRegsOffset();
    reinterpret_cast<int32_t*>(vregs)[i] = val;
  }

  void SetVRegFloat(size_t i, float val) {
    DCHECK_LT(i, number_of_vregs_);
    int8_t* vregs = reinterpret_cast<int8_t*>(this) + VRegsOffset();
    reinterpret_cast<float*>(vregs)[i] = val;
  }

  void SetVRegLong(size_t i, int64_t val) {
    int8_t* vregs = reinterpret_cast<int8_t*>(this) + VRegsOffset();
    int32_t* low_half = &reinterpret_cast<int32_t*>(vregs)[i];
    *reinterpret_cast<int64_t*>(low_half) = val;
  }

  void SetVRegDouble(size_t i, double val) {
    int8_t* vregs = reinterpret_cast<int8_t*>(this) + VRegsOffset();
    int32_t* low_half = &reinterpret_cast<int32_t*>(vregs)[i];
    *reinterpret_cast<double*>(low_half) = val;
  }

  void SetReferenceAndVReg(size_t i, Object* val) {
    SetReference(i, val);
    SetVReg(i, reinterpret_cast<int32_t>(val));
  }

  AbstractMethod* GetMethod() const {
    DCHECK_NE(method_, static_cast<void*>(NULL));
    return method_;
  }

  void SetMethod(AbstractMethod* method) {
    DCHECK_NE(method, static_cast<void*>(NULL));
    method_ = method;
  }

  bool Contains(Object** shadow_frame_entry) const {
    return ((&references_[0] <= shadow_frame_entry) &&
            (shadow_frame_entry <= (&references_[number_of_references_ - 1])));
  }

  template <typename Visitor>
  void VisitRoots(const Visitor& visitor) {
    size_t num_refs = NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      Object* object = GetReference(j);
      if (object != NULL) {
        visitor(object, j);
      }
    }
  }

  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, link_);
  }

  static size_t MethodOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, method_);
  }

  static size_t DexPCOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, dex_pc_);
  }

  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, number_of_references_);
  }

  static size_t NumberOfVRegsOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, number_of_vregs_);
  }

  static size_t ReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, references_);
  }

  size_t VRegsOffset() const {
    return ReferencesOffset() + (sizeof(Object*) * NumberOfReferences());
  }

 private:
  ShadowFrame(uint16_t num_refs, uint16_t num_vregs, ShadowFrame* link, AbstractMethod* method,
              uint32_t dex_pc)
      : number_of_references_ (num_refs), number_of_vregs_(num_vregs), link_(link),
        method_(method), dex_pc_(dex_pc) {
    for (size_t i = 0; i < num_refs; ++i) {
      SetReference(i, NULL);
    }
    for (size_t i = 0; i < num_vregs; ++i) {
      SetVReg(i, 0);
    }
  }

  // TODO: make the majority of these fields const.
  uint16_t number_of_references_;
  uint16_t number_of_vregs_;
  // Link to previous shadow frame or NULL.
  ShadowFrame* link_;
  AbstractMethod* method_;
  uint32_t dex_pc_;
  Object* references_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(ShadowFrame);
};

// The managed stack is used to record fragments of managed code stacks. Managed code stacks
// may either be shadow frames or lists of frames using fixed frame sizes. Transition records are
// necessary for transitions between code using different frame layouts and transitions into native
// code.
class PACKED ManagedStack {
 public:
  ManagedStack()
      : link_(NULL), top_shadow_frame_(NULL), top_quick_frame_(NULL), top_quick_frame_pc_(0) {}

  void PushManagedStackFragment(ManagedStack* fragment) {
    // Copy this top fragment into given fragment.
    memcpy(fragment, this, sizeof(ManagedStack));
    // Clear this fragment, which has become the top.
    memset(this, 0, sizeof(ManagedStack));
    // Link our top fragment onto the given fragment.
    link_ = fragment;
  }

  void PopManagedStackFragment(const ManagedStack& fragment) {
    DCHECK(&fragment == link_);
    // Copy this given fragment back to the top.
    memcpy(this, &fragment, sizeof(ManagedStack));
  }

  ManagedStack* GetLink() const {
    return link_;
  }

  AbstractMethod** GetTopQuickFrame() const {
    return top_quick_frame_;
  }

  void SetTopQuickFrame(AbstractMethod** top) {
    top_quick_frame_ = top;
  }

  uintptr_t GetTopQuickFramePc() const {
    return top_quick_frame_pc_;
  }

  void SetTopQuickFramePc(uintptr_t pc) {
    top_quick_frame_pc_ = pc;
  }

  static size_t TopQuickFrameOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_quick_frame_);
  }

  static size_t TopQuickFramePcOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_quick_frame_pc_);
  }

  ShadowFrame* PushShadowFrame(ShadowFrame* new_top_frame) {
    ShadowFrame* old_frame = top_shadow_frame_;
    top_shadow_frame_ = new_top_frame;
    new_top_frame->SetLink(old_frame);
    return old_frame;
  }

  ShadowFrame* PopShadowFrame() {
    CHECK(top_shadow_frame_ != NULL);
    ShadowFrame* frame = top_shadow_frame_;
    top_shadow_frame_ = frame->GetLink();
    return frame;
  }

  ShadowFrame* GetTopShadowFrame() const {
    return top_shadow_frame_;
  }

  static size_t TopShadowFrameOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_shadow_frame_);
  }

  size_t NumShadowFrameReferences() const;

  bool ShadowFramesContain(Object** shadow_frame_entry) const;

 private:
  ManagedStack* link_;
  ShadowFrame* top_shadow_frame_;
  AbstractMethod** top_quick_frame_;
  uintptr_t top_quick_frame_pc_;
};

class StackVisitor {
 protected:
  StackVisitor(const ManagedStack* stack, const std::vector<InstrumentationStackFrame>* instrumentation_stack,
               Context* context)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : stack_start_(stack), instrumentation_stack_(instrumentation_stack), cur_shadow_frame_(NULL),
        cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(0), cur_depth_(0),
        context_(context) {}

 public:
  virtual ~StackVisitor() {}

  // Return 'true' if we should continue to visit more frames, 'false' to stop.
  virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) = 0;

  void WalkStack(bool include_transitions = false)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* GetMethod() const {
    if (cur_shadow_frame_ != NULL) {
      return cur_shadow_frame_->GetMethod();
    } else if (cur_quick_frame_ != NULL) {
      return *cur_quick_frame_;
    } else {
      return NULL;
    }
  }

  bool IsShadowFrame() const {
    return cur_shadow_frame_ != NULL;
  }

  uint32_t GetDexPc() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t GetNativePcOffset() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uintptr_t LoadCalleeSave(int num, size_t frame_size) const {
    // Callee saves are held at the top of the frame
    AbstractMethod* method = GetMethod();
    DCHECK(method != NULL);
    byte* save_addr =
        reinterpret_cast<byte*>(cur_quick_frame_) + frame_size - ((num + 1) * kPointerSize);
#if defined(__i386__)
    save_addr -= kPointerSize;  // account for return address
#endif
    return *reinterpret_cast<uintptr_t*>(save_addr);
  }

  // Returns the height of the stack in the managed stack frames, including transitions.
  size_t GetFrameHeight() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetNumFrames() - cur_depth_;
  }

  // Returns a frame ID for JDWP use, starting from 1.
  size_t GetFrameId() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFrameHeight() + 1;
  }

  size_t GetNumFrames() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (num_frames_ == 0) {
      num_frames_ = ComputeNumFrames();
    }
    return num_frames_;
  }

  uint32_t GetVReg(AbstractMethod* m, uint16_t vreg, VRegKind kind) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetVReg(AbstractMethod* m, uint16_t vreg, uint32_t new_value, VRegKind kind)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uintptr_t GetGPR(uint32_t reg) const;

  uint32_t GetVReg(AbstractMethod** cur_quick_frame, const DexFile::CodeItem* code_item,
                   uint32_t core_spills, uint32_t fp_spills, size_t frame_size,
                   uint16_t vreg) const {
    int offset = GetVRegOffset(code_item, core_spills, fp_spills, frame_size, vreg);
    DCHECK_EQ(cur_quick_frame, GetCurrentQuickFrame());
    byte* vreg_addr = reinterpret_cast<byte*>(cur_quick_frame) + offset;
    return *reinterpret_cast<uint32_t*>(vreg_addr);
  }

  uintptr_t GetReturnPc() const;

  void SetReturnPc(uintptr_t new_ret_pc);

  /*
   * Return sp-relative offset for a Dalvik virtual register, compiler
   * spill or Method* in bytes using Method*.
   * Note that (reg >= 0) refers to a Dalvik register, (reg == -2)
   * denotes Method* and (reg <= -3) denotes a compiler temp.
   *
   *     +------------------------+
   *     | IN[ins-1]              |  {Note: resides in caller's frame}
   *     |       .                |
   *     | IN[0]                  |
   *     | caller's Method*       |
   *     +========================+  {Note: start of callee's frame}
   *     | core callee-save spill |  {variable sized}
   *     +------------------------+
   *     | fp callee-save spill   |
   *     +------------------------+
   *     | filler word            |  {For compatibility, if V[locals-1] used as wide
   *     +------------------------+
   *     | V[locals-1]            |
   *     | V[locals-2]            |
   *     |      .                 |
   *     |      .                 |  ... (reg == 2)
   *     | V[1]                   |  ... (reg == 1)
   *     | V[0]                   |  ... (reg == 0) <---- "locals_start"
   *     +------------------------+
   *     | Compiler temps         |  ... (reg == -2)
   *     |                        |  ... (reg == -3)
   *     |                        |  ... (reg == -4)
   *     +------------------------+
   *     | stack alignment padding|  {0 to (kStackAlignWords-1) of padding}
   *     +------------------------+
   *     | OUT[outs-1]            |
   *     | OUT[outs-2]            |
   *     |       .                |
   *     | OUT[0]                 |
   *     | curMethod*             |  ... (reg == -1) <<== sp, 16-byte aligned
   *     +========================+
   */
  static int GetVRegOffset(const DexFile::CodeItem* code_item,
                           uint32_t core_spills, uint32_t fp_spills,
                           size_t frame_size, int reg) {
    DCHECK_EQ(frame_size & (kStackAlignment - 1), 0U);
    int num_spills = __builtin_popcount(core_spills) + __builtin_popcount(fp_spills) + 1; // Filler.
    int num_ins = code_item->ins_size_;
    int num_regs = code_item->registers_size_ - num_ins;
    int locals_start = frame_size - ((num_spills + num_regs) * sizeof(uint32_t));
    if (reg == -2) {
      return 0;  // Method*
    } else if (reg <= -3) {
      return locals_start - ((reg + 1) * sizeof(uint32_t));  // Compiler temp.
    } else if (reg < num_regs) {
      return locals_start + (reg * sizeof(uint32_t));        // Dalvik local reg.
    } else {
      return frame_size + ((reg - num_regs) * sizeof(uint32_t)) + sizeof(uint32_t); // Dalvik in.
    }
  }

  uintptr_t GetCurrentQuickFramePc() const {
    return cur_quick_frame_pc_;
  }

  AbstractMethod** GetCurrentQuickFrame() const {
    return cur_quick_frame_;
  }

  ShadowFrame* GetCurrentShadowFrame() const {
    return cur_shadow_frame_;
  }

  StackIndirectReferenceTable* GetCurrentSirt() const {
    AbstractMethod** sp = GetCurrentQuickFrame();
    ++sp; // Skip Method*; SIRT comes next;
    return reinterpret_cast<StackIndirectReferenceTable*>(sp);
  }

  std::string DescribeLocation() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  size_t ComputeNumFrames() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  InstrumentationStackFrame GetInstrumentationStackFrame(uint32_t depth) const {
    return instrumentation_stack_->at(instrumentation_stack_->size() - depth - 1);
  }

  void SanityCheckFrame() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const ManagedStack* const stack_start_;
  const std::vector<InstrumentationStackFrame>* const instrumentation_stack_;
  ShadowFrame* cur_shadow_frame_;
  AbstractMethod** cur_quick_frame_;
  uintptr_t cur_quick_frame_pc_;
  // Lazily computed, number of frames in the stack.
  size_t num_frames_;
  // Depth of the frame we're currently at.
  size_t cur_depth_;
 protected:
  Context* const context_;
};

class VmapTable {
 public:
  explicit VmapTable(const uint16_t* table) : table_(table) {
  }

  uint16_t operator[](size_t i) const {
    return table_[i + 1];
  }

  size_t size() const {
    return table_[0];
  }

  // Is the dex register 'vreg' in the context or on the stack? Should not be called when the
  // 'kind' is unknown or constant.
  bool IsInContext(size_t vreg, uint32_t& vmap_offset, VRegKind kind) const {
    DCHECK(kind == kReferenceVReg || kind == kIntVReg || kind == kFloatVReg ||
           kind == kLongLoVReg || kind == kLongHiVReg || kind == kDoubleLoVReg ||
           kind == kDoubleHiVReg || kind == kImpreciseConstant);
    vmap_offset = 0xEBAD0FF5;
    // TODO: take advantage of the registers being ordered
    // TODO: we treat kImpreciseConstant as an integer below, need to ensure that such values
    //       are never promoted to floating point registers.
    bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
    bool in_floats = false;
    for (size_t i = 0; i < size(); ++i) {
      // Stop if we find what we are are looking for.
      if ((table_[i + 1] == vreg) && (in_floats == is_float)) {
        vmap_offset = i;
        return true;
      }
      // 0xffff is the marker for LR (return PC on x86), following it are spilled float registers.
      if (table_[i + 1] == 0xffff) {
        in_floats = true;
      }
    }
    return false;
  }

  // Compute the register number that corresponds to the entry in the vmap (vmap_offset, computed
  // by IsInContext above). If the kind is floating point then the result will be a floating point
  // register number, otherwise it will be an integer register number.
  uint32_t ComputeRegister(uint32_t spill_mask, uint32_t vmap_offset, VRegKind kind) const {
    // Compute the register we need to load from the context.
    DCHECK(kind == kReferenceVReg || kind == kIntVReg || kind == kFloatVReg ||
           kind == kLongLoVReg || kind == kLongHiVReg || kind == kDoubleLoVReg ||
           kind == kDoubleHiVReg || kind == kImpreciseConstant);
    // TODO: we treat kImpreciseConstant as an integer below, need to ensure that such values
    //       are never promoted to floating point registers.
    bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
    uint32_t matches = 0;
    if (is_float) {
      while (table_[matches] != 0xffff) {
        matches++;
      }
    }
    CHECK_LT(vmap_offset - matches, static_cast<uint32_t>(__builtin_popcount(spill_mask)));
    uint32_t spill_shifts = 0;
    while (matches != (vmap_offset + 1)) {
      DCHECK_NE(spill_mask, 0u);
      matches += spill_mask & 1;  // Add 1 if the low bit is set
      spill_mask >>= 1;
      spill_shifts++;
    }
    spill_shifts--;  // wind back one as we want the last match
    return spill_shifts;
  }
 private:
  const uint16_t* table_;
};

}  // namespace art

#endif  // ART_SRC_STACK_H_
