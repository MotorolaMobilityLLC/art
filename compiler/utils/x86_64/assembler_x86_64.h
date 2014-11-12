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

#ifndef ART_COMPILER_UTILS_X86_64_ASSEMBLER_X86_64_H_
#define ART_COMPILER_UTILS_X86_64_ASSEMBLER_X86_64_H_

#include <vector>
#include "base/macros.h"
#include "constants_x86_64.h"
#include "globals.h"
#include "managed_register_x86_64.h"
#include "offsets.h"
#include "utils/assembler.h"
#include "utils.h"

namespace art {
namespace x86_64 {

// Encodes an immediate value for operands.
//
// Note: Immediates can be 64b on x86-64 for certain instructions, but are often restricted
// to 32b.
//
// Note: As we support cross-compilation, the value type must be int64_t. Please be aware of
// conversion rules in expressions regarding negation, especially size_t on 32b.
class Immediate : public ValueObject {
 public:
  explicit Immediate(int64_t value_in) : value_(value_in) {}

  int64_t value() const { return value_; }

  bool is_int8() const { return IsInt(8, value_); }
  bool is_uint8() const { return IsUint(8, value_); }
  bool is_int16() const { return IsInt(16, value_); }
  bool is_uint16() const { return IsUint(16, value_); }
  bool is_int32() const {
    // This does not work on 32b machines: return IsInt(32, value_);
    int64_t limit = static_cast<int64_t>(1) << 31;
    return (-limit <= value_) && (value_ < limit);
  }

 private:
  const int64_t value_;
};


class Operand : public ValueObject {
 public:
  uint8_t mod() const {
    return (encoding_at(0) >> 6) & 3;
  }

  Register rm() const {
    return static_cast<Register>(encoding_at(0) & 7);
  }

  ScaleFactor scale() const {
    return static_cast<ScaleFactor>((encoding_at(1) >> 6) & 3);
  }

  Register index() const {
    return static_cast<Register>((encoding_at(1) >> 3) & 7);
  }

  Register base() const {
    return static_cast<Register>(encoding_at(1) & 7);
  }

  uint8_t rex() const {
    return rex_;
  }

  int8_t disp8() const {
    CHECK_GE(length_, 2);
    return static_cast<int8_t>(encoding_[length_ - 1]);
  }

  int32_t disp32() const {
    CHECK_GE(length_, 5);
    int32_t value;
    memcpy(&value, &encoding_[length_ - 4], sizeof(value));
    return value;
  }

  bool IsRegister(CpuRegister reg) const {
    return ((encoding_[0] & 0xF8) == 0xC0)  // Addressing mode is register only.
        && ((encoding_[0] & 0x07) == reg.LowBits())  // Register codes match.
        && (reg.NeedsRex() == ((rex_ & 1) != 0));  // REX.000B bits match.
  }

 protected:
  // Operand can be sub classed (e.g: Address).
  Operand() : rex_(0), length_(0) { }

  void SetModRM(uint8_t mod_in, CpuRegister rm_in) {
    CHECK_EQ(mod_in & ~3, 0);
    if (rm_in.NeedsRex()) {
      rex_ |= 0x41;  // REX.000B
    }
    encoding_[0] = (mod_in << 6) | rm_in.LowBits();
    length_ = 1;
  }

  void SetSIB(ScaleFactor scale_in, CpuRegister index_in, CpuRegister base_in) {
    CHECK_EQ(length_, 1);
    CHECK_EQ(scale_in & ~3, 0);
    if (base_in.NeedsRex()) {
      rex_ |= 0x41;  // REX.000B
    }
    if (index_in.NeedsRex()) {
      rex_ |= 0x42;  // REX.00X0
    }
    encoding_[1] = (scale_in << 6) | (static_cast<uint8_t>(index_in.LowBits()) << 3) |
        static_cast<uint8_t>(base_in.LowBits());
    length_ = 2;
  }

  void SetDisp8(int8_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    encoding_[length_++] = static_cast<uint8_t>(disp);
  }

  void SetDisp32(int32_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    int disp_size = sizeof(disp);
    memmove(&encoding_[length_], &disp, disp_size);
    length_ += disp_size;
  }

 private:
  uint8_t rex_;
  uint8_t length_;
  uint8_t encoding_[6];

  explicit Operand(CpuRegister reg) : rex_(0), length_(0) { SetModRM(3, reg); }

  // Get the operand encoding byte at the given index.
  uint8_t encoding_at(int index_in) const {
    CHECK_GE(index_in, 0);
    CHECK_LT(index_in, length_);
    return encoding_[index_in];
  }

  friend class X86_64Assembler;
};


class Address : public Operand {
 public:
  Address(CpuRegister base_in, int32_t disp) {
    Init(base_in, disp);
  }

  Address(CpuRegister base_in, Offset disp) {
    Init(base_in, disp.Int32Value());
  }

  Address(CpuRegister base_in, FrameOffset disp) {
    CHECK_EQ(base_in.AsRegister(), RSP);
    Init(CpuRegister(RSP), disp.Int32Value());
  }

  Address(CpuRegister base_in, MemberOffset disp) {
    Init(base_in, disp.Int32Value());
  }

  void Init(CpuRegister base_in, int32_t disp) {
    if (disp == 0 && base_in.AsRegister() != RBP) {
      SetModRM(0, base_in);
      if (base_in.AsRegister() == RSP) {
        SetSIB(TIMES_1, CpuRegister(RSP), base_in);
      }
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, base_in);
      if (base_in.AsRegister() == RSP) {
        SetSIB(TIMES_1, CpuRegister(RSP), base_in);
      }
      SetDisp8(disp);
    } else {
      SetModRM(2, base_in);
      if (base_in.AsRegister() == RSP) {
        SetSIB(TIMES_1, CpuRegister(RSP), base_in);
      }
      SetDisp32(disp);
    }
  }


  Address(CpuRegister index_in, ScaleFactor scale_in, int32_t disp) {
    CHECK_NE(index_in.AsRegister(), RSP);  // Illegal addressing mode.
    SetModRM(0, CpuRegister(RSP));
    SetSIB(scale_in, index_in, CpuRegister(RBP));
    SetDisp32(disp);
  }

  Address(CpuRegister base_in, CpuRegister index_in, ScaleFactor scale_in, int32_t disp) {
    CHECK_NE(index_in.AsRegister(), RSP);  // Illegal addressing mode.
    if (disp == 0 && base_in.AsRegister() != RBP) {
      SetModRM(0, CpuRegister(RSP));
      SetSIB(scale_in, index_in, base_in);
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, CpuRegister(RSP));
      SetSIB(scale_in, index_in, base_in);
      SetDisp8(disp);
    } else {
      SetModRM(2, CpuRegister(RSP));
      SetSIB(scale_in, index_in, base_in);
      SetDisp32(disp);
    }
  }

  // If no_rip is true then the Absolute address isn't RIP relative.
  static Address Absolute(uintptr_t addr, bool no_rip = false) {
    Address result;
    if (no_rip) {
      result.SetModRM(0, CpuRegister(RSP));
      result.SetSIB(TIMES_1, CpuRegister(RSP), CpuRegister(RBP));
      result.SetDisp32(addr);
    } else {
      result.SetModRM(0, CpuRegister(RBP));
      result.SetDisp32(addr);
    }
    return result;
  }

  // If no_rip is true then the Absolute address isn't RIP relative.
  static Address Absolute(ThreadOffset<8> addr, bool no_rip = false) {
    return Absolute(addr.Int32Value(), no_rip);
  }

 private:
  Address() {}
};


class X86_64Assembler FINAL : public Assembler {
 public:
  X86_64Assembler() : cfi_cfa_offset_(0), cfi_pc_(0) {}
  virtual ~X86_64Assembler() {}

  /*
   * Emit Machine Instructions.
   */
  void call(CpuRegister reg);
  void call(const Address& address);
  void call(Label* label);

  void pushq(CpuRegister reg);
  void pushq(const Address& address);
  void pushq(const Immediate& imm);

  void popq(CpuRegister reg);
  void popq(const Address& address);

  void movq(CpuRegister dst, const Immediate& src);
  void movl(CpuRegister dst, const Immediate& src);
  void movq(CpuRegister dst, CpuRegister src);
  void movl(CpuRegister dst, CpuRegister src);

  void movq(CpuRegister dst, const Address& src);
  void movl(CpuRegister dst, const Address& src);
  void movq(const Address& dst, CpuRegister src);
  void movl(const Address& dst, CpuRegister src);
  void movl(const Address& dst, const Immediate& imm);

  void movzxb(CpuRegister dst, CpuRegister src);
  void movzxb(CpuRegister dst, const Address& src);
  void movsxb(CpuRegister dst, CpuRegister src);
  void movsxb(CpuRegister dst, const Address& src);
  void movb(CpuRegister dst, const Address& src);
  void movb(const Address& dst, CpuRegister src);
  void movb(const Address& dst, const Immediate& imm);

  void movzxw(CpuRegister dst, CpuRegister src);
  void movzxw(CpuRegister dst, const Address& src);
  void movsxw(CpuRegister dst, CpuRegister src);
  void movsxw(CpuRegister dst, const Address& src);
  void movw(CpuRegister dst, const Address& src);
  void movw(const Address& dst, CpuRegister src);
  void movw(const Address& dst, const Immediate& imm);

  void leaq(CpuRegister dst, const Address& src);

  void movaps(XmmRegister dst, XmmRegister src);

  void movss(XmmRegister dst, const Address& src);
  void movss(const Address& dst, XmmRegister src);
  void movss(XmmRegister dst, XmmRegister src);

  void movsxd(CpuRegister dst, CpuRegister src);
  void movsxd(CpuRegister dst, const Address& src);

  void movd(XmmRegister dst, CpuRegister src);
  void movd(CpuRegister dst, XmmRegister src);

  void addss(XmmRegister dst, XmmRegister src);
  void addss(XmmRegister dst, const Address& src);
  void subss(XmmRegister dst, XmmRegister src);
  void subss(XmmRegister dst, const Address& src);
  void mulss(XmmRegister dst, XmmRegister src);
  void mulss(XmmRegister dst, const Address& src);
  void divss(XmmRegister dst, XmmRegister src);
  void divss(XmmRegister dst, const Address& src);

  void movsd(XmmRegister dst, const Address& src);
  void movsd(const Address& dst, XmmRegister src);
  void movsd(XmmRegister dst, XmmRegister src);

  void addsd(XmmRegister dst, XmmRegister src);
  void addsd(XmmRegister dst, const Address& src);
  void subsd(XmmRegister dst, XmmRegister src);
  void subsd(XmmRegister dst, const Address& src);
  void mulsd(XmmRegister dst, XmmRegister src);
  void mulsd(XmmRegister dst, const Address& src);
  void divsd(XmmRegister dst, XmmRegister src);
  void divsd(XmmRegister dst, const Address& src);

  void cvtsi2ss(XmmRegister dst, CpuRegister src);  // Note: this is the r/m32 version.
  void cvtsi2sd(XmmRegister dst, CpuRegister src);  // Note: this is the r/m32 version.

  void cvtss2si(CpuRegister dst, XmmRegister src);  // Note: this is the r32 version.
  void cvtss2sd(XmmRegister dst, XmmRegister src);

  void cvtsd2si(CpuRegister dst, XmmRegister src);  // Note: this is the r32 version.
  void cvtsd2ss(XmmRegister dst, XmmRegister src);

  void cvttss2si(CpuRegister dst, XmmRegister src);  // Note: this is the r32 version.
  void cvttsd2si(CpuRegister dst, XmmRegister src);  // Note: this is the r32 version.

  void cvtdq2pd(XmmRegister dst, XmmRegister src);

  void comiss(XmmRegister a, XmmRegister b);
  void comisd(XmmRegister a, XmmRegister b);

  void sqrtsd(XmmRegister dst, XmmRegister src);
  void sqrtss(XmmRegister dst, XmmRegister src);

  void xorpd(XmmRegister dst, const Address& src);
  void xorpd(XmmRegister dst, XmmRegister src);
  void xorps(XmmRegister dst, const Address& src);
  void xorps(XmmRegister dst, XmmRegister src);

  void andpd(XmmRegister dst, const Address& src);

  void flds(const Address& src);
  void fstps(const Address& dst);

  void fldl(const Address& src);
  void fstpl(const Address& dst);

  void fnstcw(const Address& dst);
  void fldcw(const Address& src);

  void fistpl(const Address& dst);
  void fistps(const Address& dst);
  void fildl(const Address& src);

  void fincstp();
  void ffree(const Immediate& index);

  void fsin();
  void fcos();
  void fptan();

  void xchgl(CpuRegister dst, CpuRegister src);
  void xchgq(CpuRegister dst, CpuRegister src);
  void xchgl(CpuRegister reg, const Address& address);

  void cmpw(const Address& address, const Immediate& imm);

  void cmpl(CpuRegister reg, const Immediate& imm);
  void cmpl(CpuRegister reg0, CpuRegister reg1);
  void cmpl(CpuRegister reg, const Address& address);
  void cmpl(const Address& address, CpuRegister reg);
  void cmpl(const Address& address, const Immediate& imm);

  void cmpq(CpuRegister reg0, CpuRegister reg1);
  void cmpq(CpuRegister reg0, const Immediate& imm);
  void cmpq(CpuRegister reg0, const Address& address);
  void cmpq(const Address& address, const Immediate& imm);

  void testl(CpuRegister reg1, CpuRegister reg2);
  void testl(CpuRegister reg, const Immediate& imm);

  void testq(CpuRegister reg1, CpuRegister reg2);
  void testq(CpuRegister reg, const Address& address);

  void andl(CpuRegister dst, const Immediate& imm);
  void andl(CpuRegister dst, CpuRegister src);
  void andl(CpuRegister reg, const Address& address);
  void andq(CpuRegister dst, const Immediate& imm);
  void andq(CpuRegister dst, CpuRegister src);

  void orl(CpuRegister dst, const Immediate& imm);
  void orl(CpuRegister dst, CpuRegister src);
  void orl(CpuRegister reg, const Address& address);
  void orq(CpuRegister dst, CpuRegister src);

  void xorl(CpuRegister dst, CpuRegister src);
  void xorl(CpuRegister dst, const Immediate& imm);
  void xorl(CpuRegister reg, const Address& address);
  void xorq(CpuRegister dst, const Immediate& imm);
  void xorq(CpuRegister dst, CpuRegister src);

  void addl(CpuRegister dst, CpuRegister src);
  void addl(CpuRegister reg, const Immediate& imm);
  void addl(CpuRegister reg, const Address& address);
  void addl(const Address& address, CpuRegister reg);
  void addl(const Address& address, const Immediate& imm);

  void addq(CpuRegister reg, const Immediate& imm);
  void addq(CpuRegister dst, CpuRegister src);
  void addq(CpuRegister dst, const Address& address);

  void subl(CpuRegister dst, CpuRegister src);
  void subl(CpuRegister reg, const Immediate& imm);
  void subl(CpuRegister reg, const Address& address);

  void subq(CpuRegister reg, const Immediate& imm);
  void subq(CpuRegister dst, CpuRegister src);
  void subq(CpuRegister dst, const Address& address);

  void cdq();
  void cqo();

  void idivl(CpuRegister reg);
  void idivq(CpuRegister reg);

  void imull(CpuRegister dst, CpuRegister src);
  void imull(CpuRegister reg, const Immediate& imm);
  void imull(CpuRegister reg, const Address& address);

  void imulq(CpuRegister dst, CpuRegister src);
  void imulq(CpuRegister reg, const Immediate& imm);
  void imulq(CpuRegister reg, const Address& address);

  void imull(CpuRegister reg);
  void imull(const Address& address);

  void mull(CpuRegister reg);
  void mull(const Address& address);

  void shll(CpuRegister reg, const Immediate& imm);
  void shll(CpuRegister operand, CpuRegister shifter);
  void shrl(CpuRegister reg, const Immediate& imm);
  void shrl(CpuRegister operand, CpuRegister shifter);
  void sarl(CpuRegister reg, const Immediate& imm);
  void sarl(CpuRegister operand, CpuRegister shifter);

  void shrq(CpuRegister reg, const Immediate& imm);

  void negl(CpuRegister reg);
  void negq(CpuRegister reg);

  void notl(CpuRegister reg);
  void notq(CpuRegister reg);

  void enter(const Immediate& imm);
  void leave();

  void ret();
  void ret(const Immediate& imm);

  void nop();
  void int3();
  void hlt();

  void j(Condition condition, Label* label);

  void jmp(CpuRegister reg);
  void jmp(const Address& address);
  void jmp(Label* label);

  X86_64Assembler* lock();
  void cmpxchgl(const Address& address, CpuRegister reg);

  void mfence();

  X86_64Assembler* gs();

  void setcc(Condition condition, CpuRegister dst);

  //
  // Macros for High-level operations.
  //

  void AddImmediate(CpuRegister reg, const Immediate& imm);

  void LoadDoubleConstant(XmmRegister dst, double value);

  void DoubleNegate(XmmRegister d);
  void FloatNegate(XmmRegister f);

  void DoubleAbs(XmmRegister reg);

  void LockCmpxchgl(const Address& address, CpuRegister reg) {
    lock()->cmpxchgl(address, reg);
  }

  //
  // Misc. functionality
  //
  int PreferredLoopAlignment() { return 16; }
  void Align(int alignment, int offset);
  void Bind(Label* label);

  //
  // Overridden common assembler high-level functionality
  //

  // Emit code that will create an activation on the stack
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack
  void RemoveFrame(size_t frame_size, const std::vector<ManagedRegister>& callee_save_regs)
      OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines
  void Store(FrameOffset offs, ManagedRegister src, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister src) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister src) OVERRIDE;

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister scratch) OVERRIDE;

  void StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm, ManagedRegister scratch)
      OVERRIDE;

  void StoreStackOffsetToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
                                  ManagedRegister scratch) OVERRIDE;

  void StoreStackPointerToThread64(ThreadOffset<8> thr_offs) OVERRIDE;

  void StoreSpanning(FrameOffset dest, ManagedRegister src, FrameOffset in_off,
                     ManagedRegister scratch) OVERRIDE;

  // Load routines
  void Load(ManagedRegister dest, FrameOffset src, size_t size) OVERRIDE;

  void LoadFromThread64(ManagedRegister dest, ThreadOffset<8> src, size_t size) OVERRIDE;

  void LoadRef(ManagedRegister dest, FrameOffset  src) OVERRIDE;

  void LoadRef(ManagedRegister dest, ManagedRegister base, MemberOffset offs) OVERRIDE;

  void LoadRawPtr(ManagedRegister dest, ManagedRegister base, Offset offs) OVERRIDE;

  void LoadRawPtrFromThread64(ManagedRegister dest, ThreadOffset<8> offs) OVERRIDE;

  // Copying routines
  void Move(ManagedRegister dest, ManagedRegister src, size_t size);

  void CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<8> thr_offs,
                              ManagedRegister scratch) OVERRIDE;

  void CopyRawPtrToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs, ManagedRegister scratch)
      OVERRIDE;

  void CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister scratch) OVERRIDE;

  void Copy(FrameOffset dest, FrameOffset src, ManagedRegister scratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset, ManagedRegister scratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src, ManagedRegister scratch,
            size_t size) OVERRIDE;

  void Copy(FrameOffset dest, FrameOffset src_base, Offset src_offset, ManagedRegister scratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest, Offset dest_offset, ManagedRegister src, Offset src_offset,
            ManagedRegister scratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest, Offset dest_offset, FrameOffset src, Offset src_offset,
            ManagedRegister scratch, size_t size) OVERRIDE;

  void MemoryBarrier(ManagedRegister) OVERRIDE;

  // Sign extension
  void SignExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Zero extension
  void ZeroExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Exploit fast access in managed code to Thread::Current()
  void GetCurrentThread(ManagedRegister tr) OVERRIDE;
  void GetCurrentThread(FrameOffset dest_offset, ManagedRegister scratch) OVERRIDE;

  // Set up out_reg to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // NULL.
  void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset, ManagedRegister in_reg,
                       bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed.
  void CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handlescope_offset, ManagedRegister scratch,
                       bool null_allowed) OVERRIDE;

  // src holds a handle scope entry (Object**) load this into dst
  virtual void LoadReferenceFromHandleScope(ManagedRegister dst,
                                     ManagedRegister src);

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset]
  void Call(ManagedRegister base, Offset offset, ManagedRegister scratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister scratch) OVERRIDE;
  void CallFromThread64(ThreadOffset<8> offset, ManagedRegister scratch) OVERRIDE;

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister scratch, size_t stack_adjust) OVERRIDE;

  void InitializeFrameDescriptionEntry() OVERRIDE;
  void FinalizeFrameDescriptionEntry() OVERRIDE;
  std::vector<uint8_t>* GetFrameDescriptionEntry() OVERRIDE {
    return &cfi_info_;
  }

 private:
  void EmitUint8(uint8_t value);
  void EmitInt32(int32_t value);
  void EmitInt64(int64_t value);
  void EmitRegisterOperand(uint8_t rm, uint8_t reg);
  void EmitXmmRegisterOperand(uint8_t rm, XmmRegister reg);
  void EmitFixup(AssemblerFixup* fixup);
  void EmitOperandSizeOverride();

  void EmitOperand(uint8_t rm, const Operand& operand);
  void EmitImmediate(const Immediate& imm);
  void EmitComplex(uint8_t rm, const Operand& operand, const Immediate& immediate);
  void EmitLabel(Label* label, int instruction_size);
  void EmitLabelLink(Label* label);
  void EmitNearLabelLink(Label* label);

  void EmitGenericShift(bool wide, int rm, CpuRegister reg, const Immediate& imm);
  void EmitGenericShift(int rm, CpuRegister operand, CpuRegister shifter);

  // If any input is not false, output the necessary rex prefix.
  void EmitOptionalRex(bool force, bool w, bool r, bool x, bool b);

  // Emit a rex prefix byte if necessary for reg. ie if reg is a register in the range R8 to R15.
  void EmitOptionalRex32(CpuRegister reg);
  void EmitOptionalRex32(CpuRegister dst, CpuRegister src);
  void EmitOptionalRex32(XmmRegister dst, XmmRegister src);
  void EmitOptionalRex32(CpuRegister dst, XmmRegister src);
  void EmitOptionalRex32(XmmRegister dst, CpuRegister src);
  void EmitOptionalRex32(const Operand& operand);
  void EmitOptionalRex32(CpuRegister dst, const Operand& operand);
  void EmitOptionalRex32(XmmRegister dst, const Operand& operand);

  // Emit a REX.W prefix plus necessary register bit encodings.
  void EmitRex64();
  void EmitRex64(CpuRegister reg);
  void EmitRex64(const Operand& operand);
  void EmitRex64(CpuRegister dst, CpuRegister src);
  void EmitRex64(CpuRegister dst, const Operand& operand);
  void EmitRex64(XmmRegister dst, CpuRegister src);

  // Emit a REX prefix to normalize byte registers plus necessary register bit encodings.
  void EmitOptionalByteRegNormalizingRex32(CpuRegister dst, CpuRegister src);
  void EmitOptionalByteRegNormalizingRex32(CpuRegister dst, const Operand& operand);

  std::vector<uint8_t> cfi_info_;
  uint32_t cfi_cfa_offset_, cfi_pc_;

  DISALLOW_COPY_AND_ASSIGN(X86_64Assembler);
};

inline void X86_64Assembler::EmitUint8(uint8_t value) {
  buffer_.Emit<uint8_t>(value);
}

inline void X86_64Assembler::EmitInt32(int32_t value) {
  buffer_.Emit<int32_t>(value);
}

inline void X86_64Assembler::EmitInt64(int64_t value) {
  // Write this 64-bit value as two 32-bit words for alignment reasons
  // (this is essentially when running on ARM, which does not allow
  // 64-bit unaligned accesses).  We assume little-endianness here.
  EmitInt32(Low32Bits(value));
  EmitInt32(High32Bits(value));
}

inline void X86_64Assembler::EmitRegisterOperand(uint8_t rm, uint8_t reg) {
  CHECK_GE(rm, 0);
  CHECK_LT(rm, 8);
  buffer_.Emit<uint8_t>((0xC0 | (reg & 7)) + (rm << 3));
}

inline void X86_64Assembler::EmitXmmRegisterOperand(uint8_t rm, XmmRegister reg) {
  EmitRegisterOperand(rm, static_cast<uint8_t>(reg.AsFloatRegister()));
}

inline void X86_64Assembler::EmitFixup(AssemblerFixup* fixup) {
  buffer_.EmitFixup(fixup);
}

inline void X86_64Assembler::EmitOperandSizeOverride() {
  EmitUint8(0x66);
}

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_UTILS_X86_64_ASSEMBLER_X86_64_H_
