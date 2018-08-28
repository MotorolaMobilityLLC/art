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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_

#include "base/bit_field.h"
#include "code_generator.h"
#include "common_arm64.h"
#include "dex/dex_file_types.h"
#include "dex/string_reference.h"
#include "dex/type_reference.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm64/assembler_arm64.h"

// TODO(VIXL): Make VIXL compile with -Wshadow.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "aarch64/disasm-aarch64.h"
#include "aarch64/macro-assembler-aarch64.h"
#pragma GCC diagnostic pop

namespace art {

namespace linker {
class Arm64RelativePatcherTest;
}  // namespace linker

namespace arm64 {

class CodeGeneratorARM64;

// Use a local definition to prevent copying mistakes.
static constexpr size_t kArm64WordSize = static_cast<size_t>(kArm64PointerSize);

// These constants are used as an approximate margin when emission of veneer and literal pools
// must be blocked.
static constexpr int kMaxMacroInstructionSizeInBytes = 15 * vixl::aarch64::kInstructionSize;
static constexpr int kInvokeCodeMarginSizeInBytes = 6 * kMaxMacroInstructionSizeInBytes;

static const vixl::aarch64::Register kParameterCoreRegisters[] = {
  vixl::aarch64::x1,
  vixl::aarch64::x2,
  vixl::aarch64::x3,
  vixl::aarch64::x4,
  vixl::aarch64::x5,
  vixl::aarch64::x6,
  vixl::aarch64::x7
};
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static const vixl::aarch64::FPRegister kParameterFPRegisters[] = {
  vixl::aarch64::d0,
  vixl::aarch64::d1,
  vixl::aarch64::d2,
  vixl::aarch64::d3,
  vixl::aarch64::d4,
  vixl::aarch64::d5,
  vixl::aarch64::d6,
  vixl::aarch64::d7
};
static constexpr size_t kParameterFPRegistersLength = arraysize(kParameterFPRegisters);

// Thread Register.
const vixl::aarch64::Register tr = vixl::aarch64::x19;
// Marking Register.
const vixl::aarch64::Register mr = vixl::aarch64::x20;
// Method register on invoke.
static const vixl::aarch64::Register kArtMethodRegister = vixl::aarch64::x0;
const vixl::aarch64::CPURegList vixl_reserved_core_registers(vixl::aarch64::ip0,
                                                             vixl::aarch64::ip1);
const vixl::aarch64::CPURegList vixl_reserved_fp_registers(vixl::aarch64::d31);

const vixl::aarch64::CPURegList runtime_reserved_core_registers =
    vixl::aarch64::CPURegList(
        tr,
        // Reserve X20 as Marking Register when emitting Baker read barriers.
        ((kEmitCompilerReadBarrier && kUseBakerReadBarrier) ? mr : vixl::aarch64::NoCPUReg),
        vixl::aarch64::lr);

// Some instructions have special requirements for a temporary, for example
// LoadClass/kBssEntry and LoadString/kBssEntry for Baker read barrier require
// temp that's not an R0 (to avoid an extra move) and Baker read barrier field
// loads with large offsets need a fixed register to limit the number of link-time
// thunks we generate. For these and similar cases, we want to reserve a specific
// register that's neither callee-save nor an argument register. We choose x15.
inline Location FixedTempLocation() {
  return Location::RegisterLocation(vixl::aarch64::x15.GetCode());
}

// Callee-save registers AAPCS64, without x19 (Thread Register) (nor
// x20 (Marking Register) when emitting Baker read barriers).
const vixl::aarch64::CPURegList callee_saved_core_registers(
    vixl::aarch64::CPURegister::kRegister,
    vixl::aarch64::kXRegSize,
    ((kEmitCompilerReadBarrier && kUseBakerReadBarrier)
         ? vixl::aarch64::x21.GetCode()
         : vixl::aarch64::x20.GetCode()),
     vixl::aarch64::x30.GetCode());
const vixl::aarch64::CPURegList callee_saved_fp_registers(vixl::aarch64::CPURegister::kFPRegister,
                                                          vixl::aarch64::kDRegSize,
                                                          vixl::aarch64::d8.GetCode(),
                                                          vixl::aarch64::d15.GetCode());
Location ARM64ReturnLocation(DataType::Type return_type);

class SlowPathCodeARM64 : public SlowPathCode {
 public:
  explicit SlowPathCodeARM64(HInstruction* instruction)
      : SlowPathCode(instruction), entry_label_(), exit_label_() {}

  vixl::aarch64::Label* GetEntryLabel() { return &entry_label_; }
  vixl::aarch64::Label* GetExitLabel() { return &exit_label_; }

  void SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) override;
  void RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) override;

 private:
  vixl::aarch64::Label entry_label_;
  vixl::aarch64::Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM64);
};

class JumpTableARM64 : public DeletableArenaObject<kArenaAllocSwitchTable> {
 public:
  explicit JumpTableARM64(HPackedSwitch* switch_instr)
    : switch_instr_(switch_instr), table_start_() {}

  vixl::aarch64::Label* GetTableStartLabel() { return &table_start_; }

  void EmitTable(CodeGeneratorARM64* codegen);

 private:
  HPackedSwitch* const switch_instr_;
  vixl::aarch64::Label table_start_;

  DISALLOW_COPY_AND_ASSIGN(JumpTableARM64);
};

static const vixl::aarch64::Register kRuntimeParameterCoreRegisters[] =
    { vixl::aarch64::x0,
      vixl::aarch64::x1,
      vixl::aarch64::x2,
      vixl::aarch64::x3,
      vixl::aarch64::x4,
      vixl::aarch64::x5,
      vixl::aarch64::x6,
      vixl::aarch64::x7 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static const vixl::aarch64::FPRegister kRuntimeParameterFpuRegisters[] =
    { vixl::aarch64::d0,
      vixl::aarch64::d1,
      vixl::aarch64::d2,
      vixl::aarch64::d3,
      vixl::aarch64::d4,
      vixl::aarch64::d5,
      vixl::aarch64::d6,
      vixl::aarch64::d7 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<vixl::aarch64::Register,
                                                                vixl::aarch64::FPRegister> {
 public:
  static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kArm64PointerSize) {}

  Location GetReturnLocation(DataType::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<vixl::aarch64::Register,
                                                            vixl::aarch64::FPRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFPRegisters,
                          kParameterFPRegistersLength,
                          kArm64PointerSize) {}

  Location GetReturnLocation(DataType::Type return_type) const {
    return ARM64ReturnLocation(return_type);
  }


 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorARM64 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorARM64() {}
  virtual ~InvokeDexCallingConventionVisitorARM64() {}

  Location GetNextLocation(DataType::Type type) override;
  Location GetReturnLocation(DataType::Type return_type) const override {
    return calling_convention.GetReturnLocation(return_type);
  }
  Location GetMethodLocation() const override;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorARM64);
};

class FieldAccessCallingConventionARM64 : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionARM64() {}

  Location GetObjectLocation() const override {
    return helpers::LocationFrom(vixl::aarch64::x1);
  }
  Location GetFieldIndexLocation() const override {
    return helpers::LocationFrom(vixl::aarch64::x0);
  }
  Location GetReturnLocation(DataType::Type type ATTRIBUTE_UNUSED) const override {
    return helpers::LocationFrom(vixl::aarch64::x0);
  }
  Location GetSetValueLocation(DataType::Type type ATTRIBUTE_UNUSED,
                               bool is_instance) const override {
    return is_instance
        ? helpers::LocationFrom(vixl::aarch64::x2)
        : helpers::LocationFrom(vixl::aarch64::x1);
  }
  Location GetFpuLocation(DataType::Type type ATTRIBUTE_UNUSED) const override {
    return helpers::LocationFrom(vixl::aarch64::d0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionARM64);
};

class InstructionCodeGeneratorARM64 : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorARM64(HGraph* graph, CodeGeneratorARM64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_ARM64(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  Arm64Assembler* GetAssembler() const { return assembler_; }
  vixl::aarch64::MacroAssembler* GetVIXLAssembler() { return GetAssembler()->GetVIXLAssembler(); }

 private:
  void GenerateClassInitializationCheck(SlowPathCodeARM64* slow_path,
                                        vixl::aarch64::Register class_reg);
  void GenerateBitstringTypeCheckCompare(HTypeCheckInstruction* check,
                                         vixl::aarch64::Register temp);
  void GenerateSuspendCheck(HSuspendCheck* instruction, HBasicBlock* successor);
  void HandleBinaryOp(HBinaryOperation* instr);

  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleCondition(HCondition* instruction);

  // Generate a heap reference load using one register `out`:
  //
  //   out <- *(out + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a read barrier and
  // shall be a register in that case; it may be an invalid location
  // otherwise.
  void GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                        Location out,
                                        uint32_t offset,
                                        Location maybe_temp,
                                        ReadBarrierOption read_barrier_option);
  // Generate a heap reference load using two different registers
  // `out` and `obj`:
  //
  //   out <- *(obj + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a Baker's (fast
  // path) read barrier and shall be a register in that case; it may
  // be an invalid location otherwise.
  void GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                         Location out,
                                         Location obj,
                                         uint32_t offset,
                                         Location maybe_temp,
                                         ReadBarrierOption read_barrier_option);

  // Generate a floating-point comparison.
  void GenerateFcmp(HInstruction* instruction);

  void HandleShift(HBinaryOperation* instr);
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             vixl::aarch64::Label* true_target,
                             vixl::aarch64::Label* false_target);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivRemByPowerOfTwo(HBinaryOperation* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateIntDiv(HDiv* instruction);
  void GenerateIntDivForConstDenom(HDiv *instruction);
  void GenerateIntDivForPower2Denom(HDiv *instruction);
  void GenerateIntRem(HRem* instruction);
  void GenerateIntRemForConstDenom(HRem *instruction);
  void GenerateIntRemForPower2Denom(HRem *instruction);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  vixl::aarch64::MemOperand VecAddress(
      HVecMemoryOperation* instruction,
      // This function may acquire a scratch register.
      vixl::aarch64::UseScratchRegisterScope* temps_scope,
      size_t size,
      bool is_string_char_at,
      /*out*/ vixl::aarch64::Register* scratch);

  Arm64Assembler* const assembler_;
  CodeGeneratorARM64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM64);
};

class LocationsBuilderARM64 : public HGraphVisitor {
 public:
  LocationsBuilderARM64(HGraph* graph, CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_ARM64(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleBinaryOp(HBinaryOperation* instr);
  void HandleFieldSet(HInstruction* instruction);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleInvoke(HInvoke* instr);
  void HandleCondition(HCondition* instruction);
  void HandleShift(HBinaryOperation* instr);

  CodeGeneratorARM64* const codegen_;
  InvokeDexCallingConventionVisitorARM64 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM64);
};

class ParallelMoveResolverARM64 : public ParallelMoveResolverNoSwap {
 public:
  ParallelMoveResolverARM64(ArenaAllocator* allocator, CodeGeneratorARM64* codegen)
      : ParallelMoveResolverNoSwap(allocator), codegen_(codegen), vixl_temps_() {}

 protected:
  void PrepareForEmitNativeCode() override;
  void FinishEmitNativeCode() override;
  Location AllocateScratchLocationFor(Location::Kind kind) override;
  void FreeScratchLocation(Location loc) override;
  void EmitMove(size_t index) override;

 private:
  Arm64Assembler* GetAssembler() const;
  vixl::aarch64::MacroAssembler* GetVIXLAssembler() const {
    return GetAssembler()->GetVIXLAssembler();
  }

  CodeGeneratorARM64* const codegen_;
  vixl::aarch64::UseScratchRegisterScope vixl_temps_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverARM64);
};

class CodeGeneratorARM64 : public CodeGenerator {
 public:
  CodeGeneratorARM64(HGraph* graph,
                     const CompilerOptions& compiler_options,
                     OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorARM64() {}

  void GenerateFrameEntry() override;
  void GenerateFrameExit() override;

  vixl::aarch64::CPURegList GetFramePreservedCoreRegisters() const;
  vixl::aarch64::CPURegList GetFramePreservedFPRegisters() const;

  void Bind(HBasicBlock* block) override;

  vixl::aarch64::Label* GetLabelOf(HBasicBlock* block) {
    block = FirstNonEmptyBlock(block);
    return &(block_labels_[block->GetBlockId()]);
  }

  size_t GetWordSize() const override {
    return kArm64WordSize;
  }

  size_t GetFloatingPointSpillSlotSize() const override {
    return GetGraph()->HasSIMD()
        ? 2 * kArm64WordSize   // 16 bytes == 2 arm64 words for each spill
        : 1 * kArm64WordSize;  //  8 bytes == 1 arm64 words for each spill
  }

  uintptr_t GetAddressOf(HBasicBlock* block) override {
    vixl::aarch64::Label* block_entry_label = GetLabelOf(block);
    DCHECK(block_entry_label->IsBound());
    return block_entry_label->GetLocation();
  }

  HGraphVisitor* GetLocationBuilder() override { return &location_builder_; }
  HGraphVisitor* GetInstructionVisitor() override { return &instruction_visitor_; }
  Arm64Assembler* GetAssembler() override { return &assembler_; }
  const Arm64Assembler& GetAssembler() const override { return assembler_; }
  vixl::aarch64::MacroAssembler* GetVIXLAssembler() { return GetAssembler()->GetVIXLAssembler(); }

  // Emit a write barrier.
  void MarkGCCard(vixl::aarch64::Register object,
                  vixl::aarch64::Register value,
                  bool value_can_be_null);

  void GenerateMemoryBarrier(MemBarrierKind kind);

  // Register allocation.

  void SetupBlockedRegisters() const override;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;

  // The number of registers that can be allocated. The register allocator may
  // decide to reserve and not use a few of them.
  // We do not consider registers sp, xzr, wzr. They are either not allocatable
  // (xzr, wzr), or make for poor allocatable registers (sp alignment
  // requirements, etc.). This also facilitates our task as all other registers
  // can easily be mapped via to or from their type and index or code.
  static const int kNumberOfAllocatableRegisters = vixl::aarch64::kNumberOfRegisters - 1;
  static const int kNumberOfAllocatableFPRegisters = vixl::aarch64::kNumberOfFPRegisters;
  static constexpr int kNumberOfAllocatableRegisterPairs = 0;

  void DumpCoreRegister(std::ostream& stream, int reg) const override;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const override;

  InstructionSet GetInstructionSet() const override {
    return InstructionSet::kArm64;
  }

  const Arm64InstructionSetFeatures& GetInstructionSetFeatures() const;

  void Initialize() override {
    block_labels_.resize(GetGraph()->GetBlocks().size());
  }

  // We want to use the STP and LDP instructions to spill and restore registers for slow paths.
  // These instructions can only encode offsets that are multiples of the register size accessed.
  uint32_t GetPreferredSlotsAlignment() const override { return vixl::aarch64::kXRegSizeInBytes; }

  JumpTableARM64* CreateJumpTable(HPackedSwitch* switch_instr) {
    jump_tables_.emplace_back(new (GetGraph()->GetAllocator()) JumpTableARM64(switch_instr));
    return jump_tables_.back().get();
  }

  void Finalize(CodeAllocator* allocator) override;

  // Code generation helpers.
  void MoveConstant(vixl::aarch64::CPURegister destination, HConstant* constant);
  void MoveConstant(Location destination, int32_t value) override;
  void MoveLocation(Location dst, Location src, DataType::Type dst_type) override;
  void AddLocationAsTemp(Location location, LocationSummary* locations) override;

  void Load(DataType::Type type,
            vixl::aarch64::CPURegister dst,
            const vixl::aarch64::MemOperand& src);
  void Store(DataType::Type type,
             vixl::aarch64::CPURegister src,
             const vixl::aarch64::MemOperand& dst);
  void LoadAcquire(HInstruction* instruction,
                   vixl::aarch64::CPURegister dst,
                   const vixl::aarch64::MemOperand& src,
                   bool needs_null_check);
  void StoreRelease(HInstruction* instruction,
                    DataType::Type type,
                    vixl::aarch64::CPURegister src,
                    const vixl::aarch64::MemOperand& dst,
                    bool needs_null_check);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path = nullptr) override;

  // Generate code to invoke a runtime entry point, but do not record
  // PC-related information in a stack map.
  void InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                           HInstruction* instruction,
                                           SlowPathCode* slow_path);

  ParallelMoveResolverARM64* GetMoveResolver() override { return &move_resolver_; }

  bool NeedsTwoRegisters(DataType::Type type ATTRIBUTE_UNUSED) const override {
    return false;
  }

  // Check if the desired_string_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadString::LoadKind GetSupportedLoadStringKind(
      HLoadString::LoadKind desired_string_load_kind) override;

  // Check if the desired_class_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadClass::LoadKind GetSupportedLoadClassKind(
      HLoadClass::LoadKind desired_class_load_kind) override;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      HInvokeStaticOrDirect* invoke) override;

  void GenerateStaticOrDirectCall(
      HInvokeStaticOrDirect* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;
  void GenerateVirtualCall(
      HInvokeVirtual* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;

  void MoveFromReturnRegister(Location trg ATTRIBUTE_UNUSED,
                              DataType::Type type ATTRIBUTE_UNUSED) override {
    UNIMPLEMENTED(FATAL);
  }

  // Add a new boot image intrinsic patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBootImageIntrinsicPatch(uint32_t intrinsic_data,
                                                   vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new boot image relocation patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the LDR (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBootImageRelRoPatch(uint32_t boot_image_offset,
                                               vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new boot image method patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBootImageMethodPatch(MethodReference target_method,
                                                vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new .bss entry method patch for an instruction and return
  // the label to be bound before the instruction. The instruction will be
  // either the ADRP (pass `adrp_label = null`) or the LDR (pass `adrp_label`
  // pointing to the associated ADRP patch label).
  vixl::aarch64::Label* NewMethodBssEntryPatch(MethodReference target_method,
                                               vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new boot image type patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBootImageTypePatch(const DexFile& dex_file,
                                              dex::TypeIndex type_index,
                                              vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new .bss entry type patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBssEntryTypePatch(const DexFile& dex_file,
                                             dex::TypeIndex type_index,
                                             vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new boot image string patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewBootImageStringPatch(const DexFile& dex_file,
                                                dex::StringIndex string_index,
                                                vixl::aarch64::Label* adrp_label = nullptr);

  // Add a new .bss entry string patch for an instruction and return the label
  // to be bound before the instruction. The instruction will be either the
  // ADRP (pass `adrp_label = null`) or the ADD (pass `adrp_label` pointing
  // to the associated ADRP patch label).
  vixl::aarch64::Label* NewStringBssEntryPatch(const DexFile& dex_file,
                                               dex::StringIndex string_index,
                                               vixl::aarch64::Label* adrp_label = nullptr);

  // Emit the CBNZ instruction for baker read barrier and record
  // the associated patch for AOT or slow path for JIT.
  void EmitBakerReadBarrierCbnz(uint32_t custom_data);

  vixl::aarch64::Literal<uint32_t>* DeduplicateBootImageAddressLiteral(uint64_t address);
  vixl::aarch64::Literal<uint32_t>* DeduplicateJitStringLiteral(const DexFile& dex_file,
                                                                dex::StringIndex string_index,
                                                                Handle<mirror::String> handle);
  vixl::aarch64::Literal<uint32_t>* DeduplicateJitClassLiteral(const DexFile& dex_file,
                                                               dex::TypeIndex string_index,
                                                               Handle<mirror::Class> handle);

  void EmitAdrpPlaceholder(vixl::aarch64::Label* fixup_label, vixl::aarch64::Register reg);
  void EmitAddPlaceholder(vixl::aarch64::Label* fixup_label,
                          vixl::aarch64::Register out,
                          vixl::aarch64::Register base);
  void EmitLdrOffsetPlaceholder(vixl::aarch64::Label* fixup_label,
                                vixl::aarch64::Register out,
                                vixl::aarch64::Register base);

  void LoadBootImageAddress(vixl::aarch64::Register reg, uint32_t boot_image_reference);
  void AllocateInstanceForIntrinsic(HInvokeStaticOrDirect* invoke, uint32_t boot_image_offset);

  void EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) override;
  bool NeedsThunkCode(const linker::LinkerPatch& patch) const override;
  void EmitThunkCode(const linker::LinkerPatch& patch,
                     /*out*/ ArenaVector<uint8_t>* code,
                     /*out*/ std::string* debug_name) override;

  void EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) override;

  // Generate a GC root reference load:
  //
  //   root <- *(obj + offset)
  //
  // while honoring read barriers based on read_barrier_option.
  void GenerateGcRootFieldLoad(HInstruction* instruction,
                               Location root,
                               vixl::aarch64::Register obj,
                               uint32_t offset,
                               vixl::aarch64::Label* fixup_label,
                               ReadBarrierOption read_barrier_option);
  // Generate MOV for the `old_value` in UnsafeCASObject and mark it with Baker read barrier.
  void GenerateUnsafeCasOldValueMovWithBakerReadBarrier(vixl::aarch64::Register marked,
                                                        vixl::aarch64::Register old_value);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  // Overload suitable for Unsafe.getObject/-Volatile() intrinsic.
  void GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             vixl::aarch64::Register obj,
                                             const vixl::aarch64::MemOperand& src,
                                             bool needs_null_check,
                                             bool use_load_acquire);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  void GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             vixl::aarch64::Register obj,
                                             uint32_t offset,
                                             Location maybe_temp,
                                             bool needs_null_check,
                                             bool use_load_acquire);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference array load when Baker's read barriers are used.
  void GenerateArrayLoadWithBakerReadBarrier(Location ref,
                                             vixl::aarch64::Register obj,
                                             uint32_t data_offset,
                                             Location index,
                                             vixl::aarch64::Register temp,
                                             bool needs_null_check);

  // Emit code checking the status of the Marking Register, and
  // aborting the program if MR does not match the value stored in the
  // art::Thread object. Code is only emitted in debug mode and if
  // CompilerOptions::EmitRunTimeChecksInDebugMode returns true.
  //
  // Argument `code` is used to identify the different occurrences of
  // MaybeGenerateMarkingRegisterCheck in the code generator, and is
  // passed to the BRK instruction.
  //
  // If `temp_loc` is a valid location, it is expected to be a
  // register and will be used as a temporary to generate code;
  // otherwise, a temporary will be fetched from the core register
  // scratch pool.
  virtual void MaybeGenerateMarkingRegisterCheck(int code,
                                                 Location temp_loc = Location::NoLocation());

  // Generate a read barrier for a heap reference within `instruction`
  // using a slow path.
  //
  // A read barrier for an object reference read from the heap is
  // implemented as a call to the artReadBarrierSlow runtime entry
  // point, which is passed the values in locations `ref`, `obj`, and
  // `offset`:
  //
  //   mirror::Object* artReadBarrierSlow(mirror::Object* ref,
  //                                      mirror::Object* obj,
  //                                      uint32_t offset);
  //
  // The `out` location contains the value returned by
  // artReadBarrierSlow.
  //
  // When `index` is provided (i.e. for array accesses), the offset
  // value passed to artReadBarrierSlow is adjusted to take `index`
  // into account.
  void GenerateReadBarrierSlow(HInstruction* instruction,
                               Location out,
                               Location ref,
                               Location obj,
                               uint32_t offset,
                               Location index = Location::NoLocation());

  // If read barriers are enabled, generate a read barrier for a heap
  // reference using a slow path. If heap poisoning is enabled, also
  // unpoison the reference in `out`.
  void MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                    Location out,
                                    Location ref,
                                    Location obj,
                                    uint32_t offset,
                                    Location index = Location::NoLocation());

  // Generate a read barrier for a GC root within `instruction` using
  // a slow path.
  //
  // A read barrier for an object reference GC root is implemented as
  // a call to the artReadBarrierForRootSlow runtime entry point,
  // which is passed the value in location `root`:
  //
  //   mirror::Object* artReadBarrierForRootSlow(GcRoot<mirror::Object>* root);
  //
  // The `out` location contains the value returned by
  // artReadBarrierForRootSlow.
  void GenerateReadBarrierForRootSlow(HInstruction* instruction, Location out, Location root);

  void GenerateNop() override;

  void GenerateImplicitNullCheck(HNullCheck* instruction) override;
  void GenerateExplicitNullCheck(HNullCheck* instruction) override;

 private:
  // Encoding of thunk type and data for link-time generated thunks for Baker read barriers.

  enum class BakerReadBarrierKind : uint8_t {
    kField,     // Field get or array get with constant offset (i.e. constant index).
    kAcquire,   // Volatile field get.
    kArray,     // Array get with index in register.
    kGcRoot,    // GC root load.
    kLast = kGcRoot
  };

  static constexpr uint32_t kBakerReadBarrierInvalidEncodedReg = /* sp/zr is invalid */ 31u;

  static constexpr size_t kBitsForBakerReadBarrierKind =
      MinimumBitsToStore(static_cast<size_t>(BakerReadBarrierKind::kLast));
  static constexpr size_t kBakerReadBarrierBitsForRegister =
      MinimumBitsToStore(kBakerReadBarrierInvalidEncodedReg);
  using BakerReadBarrierKindField =
      BitField<BakerReadBarrierKind, 0, kBitsForBakerReadBarrierKind>;
  using BakerReadBarrierFirstRegField =
      BitField<uint32_t, kBitsForBakerReadBarrierKind, kBakerReadBarrierBitsForRegister>;
  using BakerReadBarrierSecondRegField =
      BitField<uint32_t,
               kBitsForBakerReadBarrierKind + kBakerReadBarrierBitsForRegister,
               kBakerReadBarrierBitsForRegister>;

  static void CheckValidReg(uint32_t reg) {
    DCHECK(reg < vixl::aarch64::lr.GetCode() &&
           reg != vixl::aarch64::ip0.GetCode() &&
           reg != vixl::aarch64::ip1.GetCode()) << reg;
  }

  static inline uint32_t EncodeBakerReadBarrierFieldData(uint32_t base_reg, uint32_t holder_reg) {
    CheckValidReg(base_reg);
    CheckValidReg(holder_reg);
    return BakerReadBarrierKindField::Encode(BakerReadBarrierKind::kField) |
           BakerReadBarrierFirstRegField::Encode(base_reg) |
           BakerReadBarrierSecondRegField::Encode(holder_reg);
  }

  static inline uint32_t EncodeBakerReadBarrierAcquireData(uint32_t base_reg, uint32_t holder_reg) {
    CheckValidReg(base_reg);
    CheckValidReg(holder_reg);
    DCHECK_NE(base_reg, holder_reg);
    return BakerReadBarrierKindField::Encode(BakerReadBarrierKind::kAcquire) |
           BakerReadBarrierFirstRegField::Encode(base_reg) |
           BakerReadBarrierSecondRegField::Encode(holder_reg);
  }

  static inline uint32_t EncodeBakerReadBarrierArrayData(uint32_t base_reg) {
    CheckValidReg(base_reg);
    return BakerReadBarrierKindField::Encode(BakerReadBarrierKind::kArray) |
           BakerReadBarrierFirstRegField::Encode(base_reg) |
           BakerReadBarrierSecondRegField::Encode(kBakerReadBarrierInvalidEncodedReg);
  }

  static inline uint32_t EncodeBakerReadBarrierGcRootData(uint32_t root_reg) {
    CheckValidReg(root_reg);
    return BakerReadBarrierKindField::Encode(BakerReadBarrierKind::kGcRoot) |
           BakerReadBarrierFirstRegField::Encode(root_reg) |
           BakerReadBarrierSecondRegField::Encode(kBakerReadBarrierInvalidEncodedReg);
  }

  void CompileBakerReadBarrierThunk(Arm64Assembler& assembler,
                                    uint32_t encoded_data,
                                    /*out*/ std::string* debug_name);

  using Uint64ToLiteralMap = ArenaSafeMap<uint64_t, vixl::aarch64::Literal<uint64_t>*>;
  using Uint32ToLiteralMap = ArenaSafeMap<uint32_t, vixl::aarch64::Literal<uint32_t>*>;
  using StringToLiteralMap = ArenaSafeMap<StringReference,
                                          vixl::aarch64::Literal<uint32_t>*,
                                          StringReferenceValueComparator>;
  using TypeToLiteralMap = ArenaSafeMap<TypeReference,
                                        vixl::aarch64::Literal<uint32_t>*,
                                        TypeReferenceValueComparator>;

  vixl::aarch64::Literal<uint32_t>* DeduplicateUint32Literal(uint32_t value);
  vixl::aarch64::Literal<uint64_t>* DeduplicateUint64Literal(uint64_t value);

  // The PcRelativePatchInfo is used for PC-relative addressing of methods/strings/types,
  // whether through .data.bimg.rel.ro, .bss, or directly in the boot image.
  struct PcRelativePatchInfo : PatchInfo<vixl::aarch64::Label> {
    PcRelativePatchInfo(const DexFile* dex_file, uint32_t off_or_idx)
        : PatchInfo<vixl::aarch64::Label>(dex_file, off_or_idx), pc_insn_label() { }

    vixl::aarch64::Label* pc_insn_label;
  };

  struct BakerReadBarrierPatchInfo {
    explicit BakerReadBarrierPatchInfo(uint32_t data) : label(), custom_data(data) { }

    vixl::aarch64::Label label;
    uint32_t custom_data;
  };

  vixl::aarch64::Label* NewPcRelativePatch(const DexFile* dex_file,
                                           uint32_t offset_or_index,
                                           vixl::aarch64::Label* adrp_label,
                                           ArenaDeque<PcRelativePatchInfo>* patches);

  void EmitJumpTables();

  template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
  static void EmitPcRelativeLinkerPatches(const ArenaDeque<PcRelativePatchInfo>& infos,
                                          ArenaVector<linker::LinkerPatch>* linker_patches);

  // Labels for each block that will be compiled.
  // We use a deque so that the `vixl::aarch64::Label` objects do not move in memory.
  ArenaDeque<vixl::aarch64::Label> block_labels_;  // Indexed by block id.
  vixl::aarch64::Label frame_entry_label_;
  ArenaVector<std::unique_ptr<JumpTableARM64>> jump_tables_;

  LocationsBuilderARM64 location_builder_;
  InstructionCodeGeneratorARM64 instruction_visitor_;
  ParallelMoveResolverARM64 move_resolver_;
  Arm64Assembler assembler_;

  // Deduplication map for 32-bit literals, used for non-patchable boot image addresses.
  Uint32ToLiteralMap uint32_literals_;
  // Deduplication map for 64-bit literals, used for non-patchable method address or method code.
  Uint64ToLiteralMap uint64_literals_;
  // PC-relative method patch info for kBootImageLinkTimePcRelative/BootImageRelRo.
  // Also used for type/string patches for kBootImageRelRo (same linker patch as for methods).
  ArenaDeque<PcRelativePatchInfo> boot_image_method_patches_;
  // PC-relative method patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> method_bss_entry_patches_;
  // PC-relative type patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PcRelativePatchInfo> boot_image_type_patches_;
  // PC-relative type patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> type_bss_entry_patches_;
  // PC-relative String patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PcRelativePatchInfo> boot_image_string_patches_;
  // PC-relative String patch info for kBssEntry.
  ArenaDeque<PcRelativePatchInfo> string_bss_entry_patches_;
  // PC-relative patch info for IntrinsicObjects.
  ArenaDeque<PcRelativePatchInfo> boot_image_intrinsic_patches_;
  // Baker read barrier patch info.
  ArenaDeque<BakerReadBarrierPatchInfo> baker_read_barrier_patches_;

  // Patches for string literals in JIT compiled code.
  StringToLiteralMap jit_string_patches_;
  // Patches for class literals in JIT compiled code.
  TypeToLiteralMap jit_class_patches_;

  // Baker read barrier slow paths, mapping custom data (uint32_t) to label.
  // Wrap the label to work around vixl::aarch64::Label being non-copyable
  // and non-moveable and as such unusable in ArenaSafeMap<>.
  struct LabelWrapper {
    LabelWrapper(const LabelWrapper& src)
        : label() {
      DCHECK(!src.label.IsLinked() && !src.label.IsBound());
    }
    LabelWrapper() = default;
    vixl::aarch64::Label label;
  };
  ArenaSafeMap<uint32_t, LabelWrapper> jit_baker_read_barrier_slow_paths_;

  friend class linker::Arm64RelativePatcherTest;
  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM64);
};

inline Arm64Assembler* ParallelMoveResolverARM64::GetAssembler() const {
  return codegen_->GetAssembler();
}

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_
