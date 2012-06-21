/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_VERIFIER_REGISTER_LINE_H_
#define ART_SRC_VERIFIER_REGISTER_LINE_H_

#include <deque>
#include <vector>

#include "dex_instruction.h"
#include "reg_type.h"
#include "safe_map.h"

namespace art {
namespace verifier {

class MethodVerifier;

/*
 * Register type categories, for type checking.
 *
 * The spec says category 1 includes boolean, byte, char, short, int, float, reference, and
 * returnAddress. Category 2 includes long and double.
 *
 * We treat object references separately, so we have "category1nr". We don't support jsr/ret, so
 * there is no "returnAddress" type.
 */
enum TypeCategory {
  kTypeCategoryUnknown = 0,
  kTypeCategory1nr = 1,         // boolean, byte, char, short, int, float
  kTypeCategory2 = 2,           // long, double
  kTypeCategoryRef = 3,         // object reference
};

// During verification, we associate one of these with every "interesting" instruction. We track
// the status of all registers, and (if the method has any monitor-enter instructions) maintain a
// stack of entered monitors (identified by code unit offset).
// If live-precise register maps are enabled, the "liveRegs" vector will be populated. Unlike the
// other lists of registers here, we do not track the liveness of the method result register
// (which is not visible to the GC).
class RegisterLine {
 public:
  RegisterLine(size_t num_regs, MethodVerifier* verifier) :
    line_(new uint16_t[num_regs]), verifier_(verifier), num_regs_(num_regs) {
    memset(line_.get(), 0, num_regs_ * sizeof(uint16_t));
    result_[0] = RegType::kRegTypeUndefined;
    result_[1] = RegType::kRegTypeUndefined;
  }

  // Implement category-1 "move" instructions. Copy a 32-bit value from "vsrc" to "vdst".
  void CopyRegister1(uint32_t vdst, uint32_t vsrc, TypeCategory cat);

  // Implement category-2 "move" instructions. Copy a 64-bit value from "vsrc" to "vdst". This
  // copies both halves of the register.
  void CopyRegister2(uint32_t vdst, uint32_t vsrc);

  // Implement "move-result". Copy the category-1 value from the result register to another
  // register, and reset the result register.
  void CopyResultRegister1(uint32_t vdst, bool is_reference);

  // Implement "move-result-wide". Copy the category-2 value from the result register to another
  // register, and reset the result register.
  void CopyResultRegister2(uint32_t vdst);

  // Set the invisible result register to unknown
  void SetResultTypeToUnknown();

  // Set the type of register N, verifying that the register is valid.  If "newType" is the "Lo"
  // part of a 64-bit value, register N+1 will be set to "newType+1".
  // The register index was validated during the static pass, so we don't need to check it here.
  bool SetRegisterType(uint32_t vdst, const RegType& new_type);

  /* Set the type of the "result" register. */
  void SetResultRegisterType(const RegType& new_type);

  // Get the type of register vsrc.
  const RegType& GetRegisterType(uint32_t vsrc) const;

  bool VerifyRegisterType(uint32_t vsrc, const RegType& check_type);

  void CopyFromLine(const RegisterLine* src) {
    DCHECK_EQ(num_regs_, src->num_regs_);
    memcpy(line_.get(), src->line_.get(), num_regs_ * sizeof(uint16_t));
    monitors_ = src->monitors_;
    reg_to_lock_depths_ = src->reg_to_lock_depths_;
  }

  std::string Dump() const {
    std::string result;
    for (size_t i = 0; i < num_regs_; i++) {
      result += StringPrintf("%zd:[", i);
      result += GetRegisterType(i).Dump();
      result += "],";
    }
    typedef std::deque<uint32_t>::const_iterator It; // TODO: C++0x auto
    for (It it = monitors_.begin(), end = monitors_.end(); it != end ; ++it) {
      result += StringPrintf("{%d},", *it);
    }
    return result;
  }

  void FillWithGarbage() {
    memset(line_.get(), 0xf1, num_regs_ * sizeof(uint16_t));
    while (!monitors_.empty()) {
      monitors_.pop_back();
    }
    reg_to_lock_depths_.clear();
  }

  /*
   * We're creating a new instance of class C at address A. Any registers holding instances
   * previously created at address A must be initialized by now. If not, we mark them as "conflict"
   * to prevent them from being used (otherwise, MarkRefsAsInitialized would mark the old ones and
   * the new ones at the same time).
   */
  void MarkUninitRefsAsInvalid(const RegType& uninit_type);

  /*
   * Update all registers holding "uninit_type" to instead hold the corresponding initialized
   * reference type. This is called when an appropriate constructor is invoked -- all copies of
   * the reference must be marked as initialized.
   */
  void MarkRefsAsInitialized(const RegType& uninit_type);

  /*
   * Check constraints on constructor return. Specifically, make sure that the "this" argument got
   * initialized.
   * The "this" argument to <init> uses code offset kUninitThisArgAddr, which puts it at the start
   * of the list in slot 0. If we see a register with an uninitialized slot 0 reference, we know it
   * somehow didn't get initialized.
   */
  bool CheckConstructorReturn() const;

  // Compare two register lines. Returns 0 if they match.
  // Using this for a sort is unwise, since the value can change based on machine endianness.
  int CompareLine(const RegisterLine* line2) const {
    DCHECK(monitors_ == line2->monitors_);
    // TODO: DCHECK(reg_to_lock_depths_ == line2->reg_to_lock_depths_);
    return memcmp(line_.get(), line2->line_.get(), num_regs_ * sizeof(uint16_t));
  }

  size_t NumRegs() const {
    return num_regs_;
  }

  /*
   * Get the "this" pointer from a non-static method invocation. This returns the RegType so the
   * caller can decide whether it needs the reference to be initialized or not. (Can also return
   * kRegTypeZero if the reference can only be zero at this point.)
   *
   * The argument count is in vA, and the first argument is in vC, for both "simple" and "range"
   * versions. We just need to make sure vA is >= 1 and then return vC.
   */
  const RegType& GetInvocationThis(const DecodedInstruction& dec_insn);

  /*
   * Verify types for a simple two-register instruction (e.g. "neg-int").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   */
  void CheckUnaryOp(const DecodedInstruction& dec_insn,
                    const RegType& dst_type, const RegType& src_type);

  /*
   * Verify types for a simple three-register instruction (e.g. "add-int").
   * "dst_type" is stored into vA, and "src_type1"/"src_type2" are verified
   * against vB/vC.
   */
  void CheckBinaryOp(const DecodedInstruction& dec_insn,
                     const RegType& dst_type, const RegType& src_type1, const RegType& src_type2,
                     bool check_boolean_op);

  /*
   * Verify types for a binary "2addr" operation. "src_type1"/"src_type2"
   * are verified against vA/vB, then "dst_type" is stored into vA.
   */
  void CheckBinaryOp2addr(const DecodedInstruction& dec_insn,
                          const RegType& dst_type,
                          const RegType& src_type1, const RegType& src_type2,
                          bool check_boolean_op);

  /*
   * Verify types for A two-register instruction with a literal constant (e.g. "add-int/lit8").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   *
   * If "check_boolean_op" is set, we use the constant value in vC.
   */
  void CheckLiteralOp(const DecodedInstruction& dec_insn,
                      const RegType& dst_type, const RegType& src_type, bool check_boolean_op);

  // Verify/push monitor onto the monitor stack, locking the value in reg_idx at location insn_idx.
  void PushMonitor(uint32_t reg_idx, int32_t insn_idx);

  // Verify/pop monitor from monitor stack ensuring that we believe the monitor is locked
  void PopMonitor(uint32_t reg_idx);

  // Stack of currently held monitors and where they were locked
  size_t MonitorStackDepth() const {
    return monitors_.size();
  }

  // We expect no monitors to be held at certain points, such a method returns. Verify the stack
  // is empty, failing and returning false if not.
  bool VerifyMonitorStackEmpty();

  bool MergeRegisters(const RegisterLine* incoming_line);

  size_t GetMaxNonZeroReferenceReg(size_t max_ref_reg) {
    size_t i = static_cast<int>(max_ref_reg) < 0 ? 0 : max_ref_reg;
    for (; i < num_regs_; i++) {
      if (GetRegisterType(i).IsNonZeroReferenceTypes()) {
        max_ref_reg = i;
      }
    }
    return max_ref_reg;
  }

  // Write a bit at each register location that holds a reference
  void WriteReferenceBitMap(std::vector<uint8_t>& data, size_t max_bytes);

 private:
  void CopyRegToLockDepth(size_t dst, size_t src) {
    SafeMap<uint32_t, uint32_t>::iterator it = reg_to_lock_depths_.find(src);
    if (it != reg_to_lock_depths_.end()) {
      reg_to_lock_depths_.Put(dst, it->second);
    }
  }

  bool IsSetLockDepth(size_t reg, size_t depth) {
    SafeMap<uint32_t, uint32_t>::iterator it = reg_to_lock_depths_.find(reg);
    if (it != reg_to_lock_depths_.end()) {
      return (it->second & (1 << depth)) != 0;
    } else {
      return false;
    }
  }

  void SetRegToLockDepth(size_t reg, size_t depth) {
    CHECK_LT(depth, 32u);
    DCHECK(!IsSetLockDepth(reg, depth));
    SafeMap<uint32_t, uint32_t>::iterator it = reg_to_lock_depths_.find(reg);
    if (it == reg_to_lock_depths_.end()) {
      reg_to_lock_depths_.Put(reg, 1 << depth);
    } else {
      it->second |= (1 << depth);
    }
  }

  void ClearRegToLockDepth(size_t reg, size_t depth) {
    CHECK_LT(depth, 32u);
    DCHECK(IsSetLockDepth(reg, depth));
    SafeMap<uint32_t, uint32_t>::iterator it = reg_to_lock_depths_.find(reg);
    DCHECK(it != reg_to_lock_depths_.end());
    uint32_t depths = it->second ^ (1 << depth);
    if (depths != 0) {
      it->second = depths;
    } else {
      reg_to_lock_depths_.erase(it);
    }
  }

  void ClearAllRegToLockDepths(size_t reg) {
    reg_to_lock_depths_.erase(reg);
  }

  // Storage for the result register's type, valid after an invocation
  uint16_t result_[2];

  // An array of RegType Ids associated with each dex register
  UniquePtr<uint16_t[]> line_;

  // Back link to the verifier
  MethodVerifier* verifier_;

  // Length of reg_types_
  const uint32_t num_regs_;
  // A stack of monitor enter locations
  std::deque<uint32_t> monitors_;
  // A map from register to a bit vector of indices into the monitors_ stack. As we pop the monitor
  // stack we verify that monitor-enter/exit are correctly nested. That is, if there was a
  // monitor-enter on v5 and then on v6, we expect the monitor-exit to be on v6 then on v5
  SafeMap<uint32_t, uint32_t> reg_to_lock_depths_;
};
std::ostream& operator<<(std::ostream& os, const RegisterLine& rhs);

}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_VERIFIER_REGISTER_LINE_H_
