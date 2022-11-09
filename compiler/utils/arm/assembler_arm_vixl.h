/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_VIXL_H_
#define ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_VIXL_H_

#include <android-base/logging.h>

#include "base/arena_containers.h"
#include "base/macros.h"
#include "constants_arm.h"
#include "dwarf/register.h"
#include "offsets.h"
#include "utils/arm/assembler_arm_shared.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/assembler.h"
#include "utils/jni_macro_assembler.h"

// TODO(VIXL): Make VIXL compile with -Wshadow and remove pragmas.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "aarch32/macro-assembler-aarch32.h"
#pragma GCC diagnostic pop

namespace vixl32 = vixl::aarch32;

namespace art HIDDEN {
namespace arm {

inline dwarf::Reg DWARFReg(vixl32::Register reg) {
  return dwarf::Reg::ArmCore(static_cast<int>(reg.GetCode()));
}

inline dwarf::Reg DWARFReg(vixl32::SRegister reg) {
  return dwarf::Reg::ArmFp(static_cast<int>(reg.GetCode()));
}

class ArmVIXLMacroAssembler final : public vixl32::MacroAssembler {
 public:
  // Most methods fit in a 1KB code buffer, which results in more optimal alloc/realloc and
  // fewer system calls than a larger default capacity.
  static constexpr size_t kDefaultCodeBufferCapacity = 1 * KB;

  ArmVIXLMacroAssembler()
      : vixl32::MacroAssembler(ArmVIXLMacroAssembler::kDefaultCodeBufferCapacity) {}

  // The following interfaces can generate CMP+Bcc or Cbz/Cbnz.
  // CMP+Bcc are generated by default.
  // If a hint is given (is_far_target = false) and rn and label can all fit into Cbz/Cbnz,
  // then Cbz/Cbnz is generated.
  // Prefer following interfaces to using vixl32::MacroAssembler::Cbz/Cbnz.
  // In T32, Cbz/Cbnz instructions have following limitations:
  // - Far targets, which are over 126 bytes away, are not supported.
  // - Only low registers can be encoded.
  // - Backward branches are not supported.
  void CompareAndBranchIfZero(vixl32::Register rn,
                              vixl32::Label* label,
                              bool is_far_target = true);
  void CompareAndBranchIfNonZero(vixl32::Register rn,
                                 vixl32::Label* label,
                                 bool is_far_target = true);

  // In T32 some of the instructions (add, mov, etc) outside an IT block
  // have only 32-bit encodings. But there are 16-bit flag setting
  // versions of these instructions (adds, movs, etc). In most of the
  // cases in ART we don't care if the instructions keep flags or not;
  // thus we can benefit from smaller code size.
  // VIXL will never generate flag setting versions (for example, adds
  // for Add macro instruction) unless vixl32::DontCare option is
  // explicitly specified. That's why we introduce wrappers to use
  // DontCare option by default.
#define WITH_FLAGS_DONT_CARE_RD_RN_OP(func_name) \
  void (func_name)(vixl32::Register rd, vixl32::Register rn, const vixl32::Operand& operand) { \
    MacroAssembler::func_name(vixl32::DontCare, rd, rn, operand); \
  } \
  using MacroAssembler::func_name

  WITH_FLAGS_DONT_CARE_RD_RN_OP(Adc);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Sub);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Sbc);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Rsb);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Rsc);

  WITH_FLAGS_DONT_CARE_RD_RN_OP(Eor);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Orr);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Orn);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(And);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Bic);

  WITH_FLAGS_DONT_CARE_RD_RN_OP(Asr);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Lsr);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Lsl);
  WITH_FLAGS_DONT_CARE_RD_RN_OP(Ror);

#undef WITH_FLAGS_DONT_CARE_RD_RN_OP

#define WITH_FLAGS_DONT_CARE_RD_OP(func_name) \
  void (func_name)(vixl32::Register rd, const vixl32::Operand& operand) { \
    MacroAssembler::func_name(vixl32::DontCare, rd, operand); \
  } \
  using MacroAssembler::func_name

  WITH_FLAGS_DONT_CARE_RD_OP(Mvn);
  WITH_FLAGS_DONT_CARE_RD_OP(Mov);

#undef WITH_FLAGS_DONT_CARE_RD_OP

  // The following two functions don't fall into above categories. Overload them separately.
  void Rrx(vixl32::Register rd, vixl32::Register rn) {
    MacroAssembler::Rrx(vixl32::DontCare, rd, rn);
  }
  using MacroAssembler::Rrx;

  void Mul(vixl32::Register rd, vixl32::Register rn, vixl32::Register rm) {
    MacroAssembler::Mul(vixl32::DontCare, rd, rn, rm);
  }
  using MacroAssembler::Mul;

  // TODO: Remove when MacroAssembler::Add(FlagsUpdate, Condition, Register, Register, Operand)
  // makes the right decision about 16-bit encodings.
  void Add(vixl32::Register rd, vixl32::Register rn, const vixl32::Operand& operand) {
    if (rd.Is(rn) && operand.IsPlainRegister()) {
      MacroAssembler::Add(rd, rn, operand);
    } else {
      MacroAssembler::Add(vixl32::DontCare, rd, rn, operand);
    }
  }
  using MacroAssembler::Add;

  // These interfaces try to use 16-bit T2 encoding of B instruction.
  void B(vixl32::Label* label);
  // For B(label), we always try to use Narrow encoding, because 16-bit T2 encoding supports
  // jumping within 2KB range. For B(cond, label), because the supported branch range is 256
  // bytes; we use the far_target hint to try to use 16-bit T1 encoding for short range jumps.
  void B(vixl32::Condition cond, vixl32::Label* label, bool is_far_target = true);

  // Use literal for generating double constant if it doesn't fit VMOV encoding.
  void Vmov(vixl32::DRegister rd, double imm) {
    if (vixl::VFP::IsImmFP64(imm)) {
      MacroAssembler::Vmov(rd, imm);
    } else {
      MacroAssembler::Vldr(rd, imm);
    }
  }
  using MacroAssembler::Vmov;
};

class ArmVIXLAssembler final : public Assembler {
 private:
  class ArmException;
 public:
  explicit ArmVIXLAssembler(ArenaAllocator* allocator)
      : Assembler(allocator) {
    // Use Thumb2 instruction set.
    vixl_masm_.UseT32();
  }

  virtual ~ArmVIXLAssembler() {}
  ArmVIXLMacroAssembler* GetVIXLAssembler() { return &vixl_masm_; }
  void FinalizeCode() override;

  // Size of generated code.
  size_t CodeSize() const override;
  const uint8_t* CodeBufferBaseAddress() const override;

  // Copy instructions out of assembly buffer into the given region of memory.
  void FinalizeInstructions(const MemoryRegion& region) override;

  void Bind(Label* label ATTRIBUTE_UNUSED) override {
    UNIMPLEMENTED(FATAL) << "Do not use Bind(Label*) for ARM";
  }
  void Jump(Label* label ATTRIBUTE_UNUSED) override {
    UNIMPLEMENTED(FATAL) << "Do not use Jump(Label*) for ARM";
  }

  void Bind(vixl::aarch32::Label* label) {
    vixl_masm_.Bind(label);
  }
  void Jump(vixl::aarch32::Label* label) {
    vixl_masm_.B(label);
  }

  //
  // Heap poisoning.
  //

  // Poison a heap reference contained in `reg`.
  void PoisonHeapReference(vixl32::Register reg);
  // Unpoison a heap reference contained in `reg`.
  void UnpoisonHeapReference(vixl32::Register reg);
  // Poison a heap reference contained in `reg` if heap poisoning is enabled.
  void MaybePoisonHeapReference(vixl32::Register reg);
  // Unpoison a heap reference contained in `reg` if heap poisoning is enabled.
  void MaybeUnpoisonHeapReference(vixl32::Register reg);

  // Emit code checking the status of the Marking Register, and aborting
  // the program if MR does not match the value stored in the art::Thread
  // object.
  //
  // Argument `temp` is used as a temporary register to generate code.
  // Argument `code` is used to identify the different occurrences of
  // MaybeGenerateMarkingRegisterCheck and is passed to the BKPT instruction.
  void GenerateMarkingRegisterCheck(vixl32::Register temp, int code = 0);

  void StoreToOffset(StoreOperandType type,
                     vixl32::Register reg,
                     vixl32::Register base,
                     int32_t offset);
  void StoreSToOffset(vixl32::SRegister source, vixl32::Register base, int32_t offset);
  void StoreDToOffset(vixl32::DRegister source, vixl32::Register base, int32_t offset);

  void LoadImmediate(vixl32::Register dest, int32_t value);
  void LoadFromOffset(LoadOperandType type,
                      vixl32::Register reg,
                      vixl32::Register base,
                      int32_t offset);
  void LoadSFromOffset(vixl32::SRegister reg, vixl32::Register base, int32_t offset);
  void LoadDFromOffset(vixl32::DRegister reg, vixl32::Register base, int32_t offset);

  void LoadRegisterList(RegList regs, size_t stack_offset);
  void StoreRegisterList(RegList regs, size_t stack_offset);

  bool ShifterOperandCanAlwaysHold(uint32_t immediate);
  bool ShifterOperandCanHold(Opcode opcode,
                             uint32_t immediate,
                             vixl::aarch32::FlagsUpdate update_flags = vixl::aarch32::DontCare);
  bool CanSplitLoadStoreOffset(int32_t allowed_offset_bits,
                               int32_t offset,
                               /*out*/ int32_t* add_to_base,
                               /*out*/ int32_t* offset_for_load_store);
  int32_t AdjustLoadStoreOffset(int32_t allowed_offset_bits,
                                vixl32::Register temp,
                                vixl32::Register base,
                                int32_t offset);
  int32_t GetAllowedLoadOffsetBits(LoadOperandType type);
  int32_t GetAllowedStoreOffsetBits(StoreOperandType type);

  void AddConstant(vixl32::Register rd, int32_t value);
  void AddConstant(vixl32::Register rd, vixl32::Register rn, int32_t value);
  void AddConstantInIt(vixl32::Register rd,
                       vixl32::Register rn,
                       int32_t value,
                       vixl32::Condition cond = vixl32::al);

  template <typename T>
  vixl::aarch32::Literal<T>* CreateLiteralDestroyedWithPool(T value) {
    vixl::aarch32::Literal<T>* literal =
        new vixl::aarch32::Literal<T>(value,
                                      vixl32::RawLiteral::kPlacedWhenUsed,
                                      vixl32::RawLiteral::kDeletedOnPoolDestruction);
    return literal;
  }

 private:
  // VIXL assembler.
  ArmVIXLMacroAssembler vixl_masm_;
};

// Thread register declaration.
extern const vixl32::Register tr;
// Marking register declaration.
extern const vixl32::Register mr;

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_VIXL_H_
