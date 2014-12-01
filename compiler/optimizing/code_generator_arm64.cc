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

#include "code_generator_arm64.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "thread.h"
#include "utils/arm64/assembler_arm64.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"


using namespace vixl;   // NOLINT(build/namespaces)

#ifdef __
#error "ARM64 Codegen VIXL macro-assembler macro already defined."
#endif


namespace art {

namespace arm64 {

static constexpr bool kExplicitStackOverflowCheck = false;
static constexpr size_t kHeapRefSize = sizeof(mirror::HeapReference<mirror::Object>);
static constexpr int kCurrentMethodStackOffset = 0;

namespace {

bool IsFPType(Primitive::Type type) {
  return type == Primitive::kPrimFloat || type == Primitive::kPrimDouble;
}

bool IsIntegralType(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      return true;
    default:
      return false;
  }
}

bool Is64BitType(Primitive::Type type) {
  return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
}

// Convenience helpers to ease conversion to and from VIXL operands.
static_assert((SP == 31) && (WSP == 31) && (XZR == 32) && (WZR == 32),
              "Unexpected values for register codes.");

int VIXLRegCodeFromART(int code) {
  if (code == SP) {
    return vixl::kSPRegInternalCode;
  }
  if (code == XZR) {
    return vixl::kZeroRegCode;
  }
  return code;
}

int ARTRegCodeFromVIXL(int code) {
  if (code == vixl::kSPRegInternalCode) {
    return SP;
  }
  if (code == vixl::kZeroRegCode) {
    return XZR;
  }
  return code;
}

Register XRegisterFrom(Location location) {
  return Register::XRegFromCode(VIXLRegCodeFromART(location.reg()));
}

Register WRegisterFrom(Location location) {
  return Register::WRegFromCode(VIXLRegCodeFromART(location.reg()));
}

Register RegisterFrom(Location location, Primitive::Type type) {
  DCHECK(type != Primitive::kPrimVoid && !IsFPType(type));
  return type == Primitive::kPrimLong ? XRegisterFrom(location) : WRegisterFrom(location);
}

Register OutputRegister(HInstruction* instr) {
  return RegisterFrom(instr->GetLocations()->Out(), instr->GetType());
}

Register InputRegisterAt(HInstruction* instr, int input_index) {
  return RegisterFrom(instr->GetLocations()->InAt(input_index),
                      instr->InputAt(input_index)->GetType());
}

FPRegister DRegisterFrom(Location location) {
  return FPRegister::DRegFromCode(location.reg());
}

FPRegister SRegisterFrom(Location location) {
  return FPRegister::SRegFromCode(location.reg());
}

FPRegister FPRegisterFrom(Location location, Primitive::Type type) {
  DCHECK(IsFPType(type));
  return type == Primitive::kPrimDouble ? DRegisterFrom(location) : SRegisterFrom(location);
}

FPRegister OutputFPRegister(HInstruction* instr) {
  return FPRegisterFrom(instr->GetLocations()->Out(), instr->GetType());
}

FPRegister InputFPRegisterAt(HInstruction* instr, int input_index) {
  return FPRegisterFrom(instr->GetLocations()->InAt(input_index),
                        instr->InputAt(input_index)->GetType());
}

CPURegister OutputCPURegister(HInstruction* instr) {
  return IsFPType(instr->GetType()) ? static_cast<CPURegister>(OutputFPRegister(instr))
                                    : static_cast<CPURegister>(OutputRegister(instr));
}

CPURegister InputCPURegisterAt(HInstruction* instr, int index) {
  return IsFPType(instr->InputAt(index)->GetType())
      ? static_cast<CPURegister>(InputFPRegisterAt(instr, index))
      : static_cast<CPURegister>(InputRegisterAt(instr, index));
}

int64_t Int64ConstantFrom(Location location) {
  HConstant* instr = location.GetConstant();
  return instr->IsIntConstant() ? instr->AsIntConstant()->GetValue()
                                : instr->AsLongConstant()->GetValue();
}

Operand OperandFrom(Location location, Primitive::Type type) {
  if (location.IsRegister()) {
    return Operand(RegisterFrom(location, type));
  } else {
    return Operand(Int64ConstantFrom(location));
  }
}

Operand InputOperandAt(HInstruction* instr, int input_index) {
  return OperandFrom(instr->GetLocations()->InAt(input_index),
                     instr->InputAt(input_index)->GetType());
}

MemOperand StackOperandFrom(Location location) {
  return MemOperand(sp, location.GetStackIndex());
}

MemOperand HeapOperand(const Register& base, size_t offset = 0) {
  // A heap reference must be 32bit, so fit in a W register.
  DCHECK(base.IsW());
  return MemOperand(base.X(), offset);
}

MemOperand HeapOperand(const Register& base, Offset offset) {
  return HeapOperand(base, offset.SizeValue());
}

MemOperand HeapOperandFrom(Location location, Offset offset) {
  return HeapOperand(RegisterFrom(location, Primitive::kPrimNot), offset);
}

Location LocationFrom(const Register& reg) {
  return Location::RegisterLocation(ARTRegCodeFromVIXL(reg.code()));
}

Location LocationFrom(const FPRegister& fpreg) {
  return Location::FpuRegisterLocation(fpreg.code());
}

}  // namespace

inline Condition ARM64Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne;
    case kCondLT: return lt;
    case kCondLE: return le;
    case kCondGT: return gt;
    case kCondGE: return ge;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return nv;  // Unreachable.
}

Location ARM64ReturnLocation(Primitive::Type return_type) {
  DCHECK_NE(return_type, Primitive::kPrimVoid);
  // Note that in practice, `LocationFrom(x0)` and `LocationFrom(w0)` create the
  // same Location object, and so do `LocationFrom(d0)` and `LocationFrom(s0)`,
  // but we use the exact registers for clarity.
  if (return_type == Primitive::kPrimFloat) {
    return LocationFrom(s0);
  } else if (return_type == Primitive::kPrimDouble) {
    return LocationFrom(d0);
  } else if (return_type == Primitive::kPrimLong) {
    return LocationFrom(x0);
  } else {
    return LocationFrom(w0);
  }
}

static const Register kRuntimeParameterCoreRegisters[] = { x0, x1, x2, x3, x4, x5, x6, x7 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static const FPRegister kRuntimeParameterFpuRegisters[] = { };
static constexpr size_t kRuntimeParameterFpuRegistersLength = 0;

class InvokeRuntimeCallingConvention : public CallingConvention<Register, FPRegister> {
 public:
  static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength) {}

  Location GetReturnLocation(Primitive::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type return_type) {
  return ARM64ReturnLocation(return_type);
}

#define __ down_cast<CodeGeneratorARM64*>(codegen)->GetVIXLAssembler()->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, x).Int32Value()

class SlowPathCodeARM64 : public SlowPathCode {
 public:
  SlowPathCodeARM64() : entry_label_(), exit_label_() {}

  vixl::Label* GetEntryLabel() { return &entry_label_; }
  vixl::Label* GetExitLabel() { return &exit_label_; }

 private:
  vixl::Label entry_label_;
  vixl::Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM64);
};

class BoundsCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  BoundsCheckSlowPathARM64() {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ Brk(__LINE__);  // TODO: Unimplemented BoundsCheckSlowPathARM64.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class DivZeroCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DivZeroCheckSlowPathARM64(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowDivZero), instruction_, instruction_->GetDexPc());
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM64);
};

class LoadClassSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  LoadClassSlowPathARM64(HLoadClass* cls,
                         HInstruction* at,
                         uint32_t dex_pc,
                         bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ Mov(calling_convention.GetRegisterAt(0).W(), cls_->GetTypeIndex());
    arm64_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(1).W());
    int32_t entry_point_offset = do_clinit_ ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
                                            : QUICK_ENTRY_POINT(pInitializeType);
    arm64_codegen->InvokeRuntime(entry_point_offset, at_, dex_pc_);

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      Primitive::Type type = at_->GetType();
      arm64_codegen->MoveHelper(out, calling_convention.GetReturnLocation(type), type);
    }

    codegen->RestoreLiveRegisters(locations);
    __ B(GetExitLabel());
  }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;

  // The instruction where this slow path is happening.
  // (Might be the load class or an initialization check).
  HInstruction* const at_;

  // The dex PC of `at_`.
  const uint32_t dex_pc_;

  // Whether to initialize the class.
  const bool do_clinit_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM64);
};

class LoadStringSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit LoadStringSlowPathARM64(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    arm64_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(0).W());
    __ Mov(calling_convention.GetRegisterAt(1).W(), instruction_->GetStringIndex());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pResolveString), instruction_, instruction_->GetDexPc());
    Primitive::Type type = instruction_->GetType();
    arm64_codegen->MoveHelper(locations->Out(), calling_convention.GetReturnLocation(type), type);

    codegen->RestoreLiveRegisters(locations);
    __ B(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM64);
};

class NullCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit NullCheckSlowPathARM64(HNullCheck* instr) : instruction_(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowNullPointer), instruction_, instruction_->GetDexPc());
  }

 private:
  HNullCheck* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64);
};

class StackOverflowCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  StackOverflowCheckSlowPathARM64() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowStackOverflow), nullptr, 0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathARM64);
};

class SuspendCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit SuspendCheckSlowPathARM64(HSuspendCheck* instruction,
                                     HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(instruction_->GetLocations());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pTestSuspend), instruction_, instruction_->GetDexPc());
    codegen->RestoreLiveRegisters(instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ B(GetReturnLabel());
    } else {
      __ B(arm64_codegen->GetLabelOf(successor_));
    }
  }

  vixl::Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

 private:
  HSuspendCheck* const instruction_;
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  vixl::Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64);
};

class TypeCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  TypeCheckSlowPathARM64() {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ Brk(__LINE__);  // TODO: Unimplemented TypeCheckSlowPathARM64.
    __ B(GetExitLabel());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM64);
};

#undef __

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  Location next_location;
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (IsFPType(type) && (fp_index_ < calling_convention.GetNumberOfFpuRegisters())) {
    next_location = LocationFrom(calling_convention.GetFpuRegisterAt(fp_index_++));
  } else if (!IsFPType(type) && (gp_index_ < calling_convention.GetNumberOfRegisters())) {
    next_location = LocationFrom(calling_convention.GetRegisterAt(gp_index_++));
  } else {
    size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
    next_location = Is64BitType(type) ? Location::DoubleStackSlot(stack_offset)
                                      : Location::StackSlot(stack_offset);
  }

  // Space on the stack is reserved for all arguments.
  stack_index_ += Is64BitType(type) ? 2 : 1;
  return next_location;
}

CodeGeneratorARM64::CodeGeneratorARM64(HGraph* graph)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFPRegisters,
                    kNumberOfAllocatableRegisterPairs),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this) {}

#undef __
#define __ GetVIXLAssembler()->

void CodeGeneratorARM64::Finalize(CodeAllocator* allocator) {
  // Ensure we emit the literal pool.
  __ FinalizeCode();
  CodeGenerator::Finalize(allocator);
}

void CodeGeneratorARM64::GenerateFrameEntry() {
  bool do_overflow_check = FrameNeedsStackCheck(GetFrameSize(), kArm64) || !IsLeafMethod();
  if (do_overflow_check) {
    UseScratchRegisterScope temps(GetVIXLAssembler());
    Register temp = temps.AcquireX();
    if (kExplicitStackOverflowCheck) {
      SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathARM64();
      AddSlowPath(slow_path);

      __ Ldr(temp, MemOperand(tr, Thread::StackEndOffset<kArm64WordSize>().Int32Value()));
      __ Cmp(sp, temp);
      __ B(lo, slow_path->GetEntryLabel());
    } else {
      __ Add(temp, sp, -static_cast<int32_t>(GetStackOverflowReservedBytes(kArm64)));
      __ Ldr(wzr, MemOperand(temp, 0));
      RecordPcInfo(nullptr, 0);
    }
  }

  CPURegList preserved_regs = GetFramePreservedRegisters();
  int frame_size = GetFrameSize();
  core_spill_mask_ |= preserved_regs.list();

  __ Str(w0, MemOperand(sp, -frame_size, PreIndex));
  __ PokeCPURegList(preserved_regs, frame_size - preserved_regs.TotalSizeInBytes());

  // Stack layout:
  // sp[frame_size - 8]        : lr.
  // ...                       : other preserved registers.
  // sp[frame_size - regs_size]: first preserved register.
  // ...                       : reserved frame space.
  // sp[0]                     : current method.
}

void CodeGeneratorARM64::GenerateFrameExit() {
  int frame_size = GetFrameSize();
  CPURegList preserved_regs = GetFramePreservedRegisters();
  __ PeekCPURegList(preserved_regs, frame_size - preserved_regs.TotalSizeInBytes());
  __ Drop(frame_size);
}

void CodeGeneratorARM64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorARM64::Move(HInstruction* instruction,
                              Location location,
                              HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  Primitive::Type type = instruction->GetType();
  DCHECK_NE(type, Primitive::kPrimVoid);

  if (instruction->IsIntConstant() || instruction->IsLongConstant()) {
    int64_t value = instruction->IsIntConstant() ? instruction->AsIntConstant()->GetValue()
                                                 : instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      Register dst = RegisterFrom(location, type);
      DCHECK((instruction->IsIntConstant() && dst.Is32Bits()) ||
             (instruction->IsLongConstant() && dst.Is64Bits()));
      __ Mov(dst, value);
    } else {
      DCHECK(location.IsStackSlot() || location.IsDoubleStackSlot());
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = instruction->IsIntConstant() ? temps.AcquireW() : temps.AcquireX();
      __ Mov(temp, value);
      __ Str(temp, StackOperandFrom(location));
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    MoveHelper(location, temp_location, type);
  } else if (instruction->IsLoadLocal()) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    if (Is64BitType(type)) {
      MoveHelper(location, Location::DoubleStackSlot(stack_slot), type);
    } else {
      MoveHelper(location, Location::StackSlot(stack_slot), type);
    }

  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    MoveHelper(location, locations->Out(), type);
  }
}

size_t CodeGeneratorARM64::FrameEntrySpillSize() const {
  return GetFramePreservedRegistersSize();
}

Location CodeGeneratorARM64::GetStackLocation(HLoadLocal* load) const {
  Primitive::Type type = load->GetType();

  switch (type) {
    case Primitive::kPrimNot:
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << type;
  }

  LOG(FATAL) << "Unreachable";
  return Location::NoLocation();
}

void CodeGeneratorARM64::MarkGCCard(Register object, Register value) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register card = temps.AcquireX();
  Register temp = temps.AcquireW();   // Index within the CardTable - 32bit.
  vixl::Label done;
  __ Cbz(value, &done);
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64WordSize>().Int32Value()));
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ Strb(card, MemOperand(card, temp.X()));
  __ Bind(&done);
}

void CodeGeneratorARM64::SetupBlockedRegisters() const {
  // Block reserved registers:
  //   ip0 (VIXL temporary)
  //   ip1 (VIXL temporary)
  //   tr
  //   lr
  // sp is not part of the allocatable registers, so we don't need to block it.
  // TODO: Avoid blocking callee-saved registers, and instead preserve them
  // where necessary.
  CPURegList reserved_core_registers = vixl_reserved_core_registers;
  reserved_core_registers.Combine(runtime_reserved_core_registers);
  reserved_core_registers.Combine(quick_callee_saved_registers);
  while (!reserved_core_registers.IsEmpty()) {
    blocked_core_registers_[reserved_core_registers.PopLowestIndex().code()] = true;
  }
  CPURegList reserved_fp_registers = vixl_reserved_fp_registers;
  reserved_fp_registers.Combine(CPURegList::GetCalleeSavedFP());
  while (!reserved_core_registers.IsEmpty()) {
    blocked_fpu_registers_[reserved_fp_registers.PopLowestIndex().code()] = true;
  }
}

Location CodeGeneratorARM64::AllocateFreeRegister(Primitive::Type type) const {
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (IsFPType(type)) {
    ssize_t reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfAllocatableFPRegisters);
    DCHECK_NE(reg, -1);
    return Location::FpuRegisterLocation(reg);
  } else {
    ssize_t reg = FindFreeEntry(blocked_core_registers_, kNumberOfAllocatableRegisters);
    DCHECK_NE(reg, -1);
    return Location::RegisterLocation(reg);
  }
}

void CodeGeneratorARM64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromXRegister(XRegister(reg));
}

void CodeGeneratorARM64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromDRegister(DRegister(reg));
}

void CodeGeneratorARM64::MoveConstant(CPURegister destination, HConstant* constant) {
  if (constant->IsIntConstant() || constant->IsLongConstant()) {
    __ Mov(Register(destination),
           constant->IsIntConstant() ? constant->AsIntConstant()->GetValue()
                                     : constant->AsLongConstant()->GetValue());
  } else if (constant->IsFloatConstant()) {
    __ Fmov(FPRegister(destination), constant->AsFloatConstant()->GetValue());
  } else {
    DCHECK(constant->IsDoubleConstant());
    __ Fmov(FPRegister(destination), constant->AsDoubleConstant()->GetValue());
  }
}

void CodeGeneratorARM64::MoveHelper(Location destination,
                                    Location source,
                                    Primitive::Type type) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    Register dst = RegisterFrom(destination, type);
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      DCHECK(dst.Is64Bits() == source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    } else {
      __ Mov(dst, OperandFrom(source, type));
    }
  } else if (destination.IsFpuRegister()) {
    FPRegister dst = FPRegisterFrom(destination, type);
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      DCHECK(dst.Is64Bits() == source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    } else if (source.IsFpuRegister()) {
      __ Fmov(dst, FPRegisterFrom(source, type));
    } else {
      MoveConstant(dst, source.GetConstant());
    }
  } else {
    DCHECK(destination.IsStackSlot() || destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ Str(RegisterFrom(source, type), StackOperandFrom(destination));
    } else if (source.IsFpuRegister()) {
      __ Str(FPRegisterFrom(source, type), StackOperandFrom(destination));
    } else if (source.IsConstant()) {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      HConstant* cst = source.GetConstant();
      CPURegister temp;
      if (cst->IsIntConstant() || cst->IsLongConstant()) {
        temp = cst->IsIntConstant() ? temps.AcquireW() : temps.AcquireX();
      } else {
        DCHECK(cst->IsFloatConstant() || cst->IsDoubleConstant());
        temp = cst->IsFloatConstant() ? temps.AcquireS() : temps.AcquireD();
      }
      MoveConstant(temp, cst);
      __ Str(temp, StackOperandFrom(destination));
    } else {
      DCHECK(source.IsStackSlot() || source.IsDoubleStackSlot());
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = destination.IsDoubleStackSlot() ? temps.AcquireX() : temps.AcquireW();
      __ Ldr(temp, StackOperandFrom(source));
      __ Str(temp, StackOperandFrom(destination));
    }
  }
}

void CodeGeneratorARM64::Load(Primitive::Type type,
                              vixl::CPURegister dst,
                              const vixl::MemOperand& src) {
  switch (type) {
    case Primitive::kPrimBoolean:
      __ Ldrb(Register(dst), src);
      break;
    case Primitive::kPrimByte:
      __ Ldrsb(Register(dst), src);
      break;
    case Primitive::kPrimShort:
      __ Ldrsh(Register(dst), src);
      break;
    case Primitive::kPrimChar:
      __ Ldrh(Register(dst), src);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      DCHECK(dst.Is64Bits() == Is64BitType(type));
      __ Ldr(dst, src);
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::Store(Primitive::Type type,
                               vixl::CPURegister rt,
                               const vixl::MemOperand& dst) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      __ Strb(Register(rt), dst);
      break;
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      __ Strh(Register(rt), dst);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      DCHECK(rt.Is64Bits() == Is64BitType(type));
      __ Str(rt, dst);
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::LoadCurrentMethod(vixl::Register current_method) {
  DCHECK(current_method.IsW());
  __ Ldr(current_method, MemOperand(sp, kCurrentMethodStackOffset));
}

void CodeGeneratorARM64::InvokeRuntime(int32_t entry_point_offset,
                                       HInstruction* instruction,
                                       uint32_t dex_pc) {
  __ Ldr(lr, MemOperand(tr, entry_point_offset));
  __ Blr(lr);
  if (instruction != nullptr) {
    RecordPcInfo(instruction, dex_pc);
    DCHECK(instruction->IsSuspendCheck()
        || instruction->IsBoundsCheck()
        || instruction->IsNullCheck()
        || instruction->IsDivZeroCheck()
        || !IsLeafMethod());
    }
}

void InstructionCodeGeneratorARM64::GenerateClassInitializationCheck(SlowPathCodeARM64* slow_path,
                                                                     vixl::Register class_reg) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireW();
  __ Ldr(temp, HeapOperand(class_reg, mirror::Class::StatusOffset()));
  __ Cmp(temp, mirror::Class::kStatusInitialized);
  __ B(lt, slow_path->GetEntryLabel());
  // Even if the initialized flag is set, we need to ensure consistent memory ordering.
  __ Dmb(InnerShareable, BarrierReads);
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorARM64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                         HBasicBlock* successor) {
  SuspendCheckSlowPathARM64* slow_path =
    new (GetGraph()->GetArena()) SuspendCheckSlowPathARM64(instruction, successor);
  codegen_->AddSlowPath(slow_path);
  UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());
  Register temp = temps.AcquireW();

  __ Ldrh(temp, MemOperand(tr, Thread::ThreadFlagsOffset<kArm64WordSize>().SizeValue()));
  if (successor == nullptr) {
    __ Cbnz(temp, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ Cbz(temp, codegen_->GetLabelOf(successor));
    __ B(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorARM64::InstructionCodeGeneratorARM64(HGraph* graph,
                                                             CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

#define FOR_EACH_UNIMPLEMENTED_INSTRUCTION(M)              \
  M(ParallelMove)                                          \

#define UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name) name##UnimplementedInstructionBreakCode

enum UnimplementedInstructionBreakCode {
  // Using a base helps identify when we hit such breakpoints.
  UnimplementedInstructionBreakCodeBaseCode = 0x900,
#define ENUM_UNIMPLEMENTED_INSTRUCTION(name) UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name),
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(ENUM_UNIMPLEMENTED_INSTRUCTION)
#undef ENUM_UNIMPLEMENTED_INSTRUCTION
};

#define DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS(name)                               \
  void InstructionCodeGeneratorARM64::Visit##name(H##name* instr) {                   \
    UNUSED(instr);                                                                    \
    __ Brk(UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name));                               \
  }                                                                                   \
  void LocationsBuilderARM64::Visit##name(H##name* instr) {                           \
    LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr); \
    locations->SetOut(Location::Any());                                               \
  }
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS)
#undef DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS

#undef UNIMPLEMENTED_INSTRUCTION_BREAK_CODE
#undef FOR_EACH_UNIMPLEMENTED_INSTRUCTION

void LocationsBuilderARM64::HandleBinaryOp(HBinaryOperation* instr) {
  DCHECK_EQ(instr->InputCount(), 2U);
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected " << instr->DebugName() << " type " << type;
  }
}

void InstructionCodeGeneratorARM64::HandleBinaryOp(HBinaryOperation* instr) {
  Primitive::Type type = instr->GetType();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (instr->IsAdd()) {
        __ Add(dst, lhs, rhs);
      } else if (instr->IsAnd()) {
        __ And(dst, lhs, rhs);
      } else if (instr->IsOr()) {
        __ Orr(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Sub(dst, lhs, rhs);
      } else {
        DCHECK(instr->IsXor());
        __ Eor(dst, lhs, rhs);
      }
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FPRegister dst = OutputFPRegister(instr);
      FPRegister lhs = InputFPRegisterAt(instr, 0);
      FPRegister rhs = InputFPRegisterAt(instr, 1);
      if (instr->IsAdd()) {
        __ Fadd(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Fsub(dst, lhs, rhs);
      } else {
        LOG(FATAL) << "Unexpected floating-point binary operation";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
}

void LocationsBuilderARM64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift type " << type;
  }
}

void InstructionCodeGeneratorARM64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  Primitive::Type type = instr->GetType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (rhs.IsImmediate()) {
        uint32_t shift_value = (type == Primitive::kPrimInt)
          ? static_cast<uint32_t>(rhs.immediate() & kMaxIntShiftValue)
          : static_cast<uint32_t>(rhs.immediate() & kMaxLongShiftValue);
        if (instr->IsShl()) {
          __ Lsl(dst, lhs, shift_value);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, shift_value);
        } else {
          __ Lsr(dst, lhs, shift_value);
        }
      } else {
        Register rhs_reg = dst.IsX() ? rhs.reg().X() : rhs.reg().W();

        if (instr->IsShl()) {
          __ Lsl(dst, lhs, rhs_reg);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, rhs_reg);
        } else {
          __ Lsr(dst, lhs, rhs_reg);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
}

void LocationsBuilderARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Primitive::Type type = instruction->GetType();
  Register obj = InputRegisterAt(instruction, 0);
  Location index = locations->InAt(1);
  size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(type)).Uint32Value();
  MemOperand source = HeapOperand(obj);
  UseScratchRegisterScope temps(GetVIXLAssembler());

  if (index.IsConstant()) {
    offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(type);
    source = HeapOperand(obj, offset);
  } else {
    Register temp = temps.AcquireSameSizeAs(obj);
    Register index_reg = RegisterFrom(index, Primitive::kPrimInt);
    __ Add(temp, obj, Operand(index_reg, LSL, Primitive::ComponentSizeShift(type)));
    source = HeapOperand(temp, offset);
  }

  codegen_->Load(type, OutputCPURegister(instruction), source);
}

void LocationsBuilderARM64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitArrayLength(HArrayLength* instruction) {
  __ Ldr(OutputRegister(instruction),
         HeapOperand(InputRegisterAt(instruction, 0), mirror::Array::LengthOffset()));
}

void LocationsBuilderARM64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  bool is_object = value_type == Primitive::kPrimNot;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, is_object ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (is_object) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, LocationFrom(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  if (value_type == Primitive::kPrimNot) {
    codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject), instruction, instruction->GetDexPc());

  } else {
    LocationSummary* locations = instruction->GetLocations();
    Register obj = InputRegisterAt(instruction, 0);
    CPURegister value = InputCPURegisterAt(instruction, 2);
    Location index = locations->InAt(1);
    size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(value_type)).Uint32Value();
    MemOperand destination = HeapOperand(obj);
    UseScratchRegisterScope temps(GetVIXLAssembler());

    if (index.IsConstant()) {
      offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(value_type);
      destination = HeapOperand(obj, offset);
    } else {
      Register temp = temps.AcquireSameSizeAs(obj);
      Register index_reg = InputRegisterAt(instruction, 1);
      __ Add(temp, obj, Operand(index_reg, LSL, Primitive::ComponentSizeShift(value_type)));
      destination = HeapOperand(temp, offset);
    }

    codegen_->Store(value_type, value, destination);
  }
}

void LocationsBuilderARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  BoundsCheckSlowPathARM64* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathARM64();
  codegen_->AddSlowPath(slow_path);

  __ Cmp(InputRegisterAt(instruction, 0), InputOperandAt(instruction, 1));
  __ B(slow_path->GetEntryLabel(), hs);
}

void LocationsBuilderARM64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitCheckCast(HCheckCast* instruction) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register obj = InputRegisterAt(instruction, 0);;
  Register cls = InputRegisterAt(instruction, 1);;
  Register temp = temps.AcquireW();

  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64();
  codegen_->AddSlowPath(slow_path);

  // TODO: avoid this check if we know obj is not null.
  __ Cbz(obj, slow_path->GetExitLabel());
  // Compare the class of `obj` with `cls`.
  __ Ldr(temp, HeapOperand(obj, mirror::Object::ClassOffset()));
  __ Cmp(temp, cls);
  __ B(ne, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM64(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, InputRegisterAt(check, 0));
}

void LocationsBuilderARM64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  Primitive::Type in_type = compare->InputAt(0)->GetType();
  switch (in_type) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(compare->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << in_type;
  }
}

void InstructionCodeGeneratorARM64::VisitCompare(HCompare* compare) {
  Primitive::Type in_type = compare->InputAt(0)->GetType();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (in_type) {
    case Primitive::kPrimLong: {
      Register result = OutputRegister(compare);
      Register left = InputRegisterAt(compare, 0);
      Operand right = InputOperandAt(compare, 1);

      __ Cmp(left, right);
      __ Cset(result, ne);
      __ Cneg(result, result, lt);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      Register result = OutputRegister(compare);
      FPRegister left = InputFPRegisterAt(compare, 0);
      FPRegister right = InputFPRegisterAt(compare, 1);

      __ Fcmp(left, right);
      if (compare->IsGtBias()) {
        __ Cset(result, ne);
      } else {
        __ Csetm(result, ne);
      }
      __ Cneg(result, result, compare->IsGtBias() ? mi : gt);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderARM64::VisitCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (instruction->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitCondition(HCondition* instruction) {
  if (!instruction->NeedsMaterialization()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  Register lhs = InputRegisterAt(instruction, 0);
  Operand rhs = InputOperandAt(instruction, 1);
  Register res = RegisterFrom(locations->Out(), instruction->GetType());
  Condition cond = ARM64Condition(instruction->GetCondition());

  __ Cmp(lhs, rhs);
  __ Cset(res, cond);
}

#define FOR_EACH_CONDITION_INSTRUCTION(M)                                                \
  M(Equal)                                                                               \
  M(NotEqual)                                                                            \
  M(LessThan)                                                                            \
  M(LessThanOrEqual)                                                                     \
  M(GreaterThan)                                                                         \
  M(GreaterThanOrEqual)
#define DEFINE_CONDITION_VISITORS(Name)                                                  \
void LocationsBuilderARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }         \
void InstructionCodeGeneratorARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }
FOR_EACH_CONDITION_INSTRUCTION(DEFINE_CONDITION_VISITORS)
#undef DEFINE_CONDITION_VISITORS
#undef FOR_EACH_CONDITION_INSTRUCTION

void LocationsBuilderARM64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitDiv(HDiv* div) {
  Primitive::Type type = div->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Sdiv(OutputRegister(div), InputRegisterAt(div, 0), InputRegisterAt(div, 1));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fdiv(OutputFPRegister(div), InputFPRegisterAt(div, 0), InputFPRegisterAt(div, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void LocationsBuilderARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeARM64* slow_path =
      new (GetGraph()->GetArena()) DivZeroCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);
  Location value = instruction->GetLocations()->InAt(0);

  if (value.IsConstant()) {
    int64_t divisor = Int64ConstantFrom(value);
    if (divisor == 0) {
      __ B(slow_path->GetEntryLabel());
    } else {
      LOG(FATAL) << "Divisions by non-null constants should have been optimized away.";
    }
  } else {
    __ Cbz(InputRegisterAt(instruction, 0), slow_path->GetEntryLabel());
  }
}

void LocationsBuilderARM64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitDoubleConstant(HDoubleConstant* constant) {
  UNUSED(constant);
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitExit(HExit* exit) {
  UNUSED(exit);
  if (kIsDebugBuild) {
    down_cast<Arm64Assembler*>(GetAssembler())->Comment("Unreachable");
    __ Brk(__LINE__);    // TODO: Introduce special markers for such code locations.
  }
}

void LocationsBuilderARM64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitFloatConstant(HFloatConstant* constant) {
  UNUSED(constant);
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  DCHECK(!successor->IsExitBlock());
  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();
  HLoopInformation* info = block->GetLoopInformation();

  if (info != nullptr && info->IsBackEdge(block) && info->HasSuspendCheck()) {
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(info->GetSuspendCheck());
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;
  }
  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
  }
  if (!codegen_->GoesToNextBlock(block, successor)) {
    __ B(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  HCondition* condition = cond->AsCondition();
  vixl::Label* true_target = codegen_->GetLabelOf(if_instr->IfTrueSuccessor());
  vixl::Label* false_target = codegen_->GetLabelOf(if_instr->IfFalseSuccessor());

  if (cond->IsIntConstant()) {
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfTrueSuccessor())) {
        __ B(true_target);
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else if (!cond->IsCondition() || condition->NeedsMaterialization()) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = if_instr->GetLocations()->InAt(0);
    DCHECK(cond_val.IsRegister());
    __ Cbnz(InputRegisterAt(if_instr, 0), true_target);
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    Register lhs = InputRegisterAt(condition, 0);
    Operand rhs = InputOperandAt(condition, 1);
    Condition arm64_cond = ARM64Condition(condition->GetCondition());
    if ((arm64_cond == eq || arm64_cond == ne) && rhs.IsImmediate() && (rhs.immediate() == 0)) {
      if (arm64_cond == eq) {
        __ Cbz(lhs, true_target);
      } else {
        __ Cbnz(lhs, true_target);
      }
    } else {
      __ Cmp(lhs, rhs);
      __ B(arm64_cond, true_target);
    }
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfFalseSuccessor())) {
    __ B(false_target);
  }
}

void LocationsBuilderARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  MemOperand field = HeapOperand(InputRegisterAt(instruction, 0), instruction->GetFieldOffset());
  codegen_->Load(instruction->GetType(), OutputCPURegister(instruction), field);
}

void LocationsBuilderARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  Primitive::Type field_type = instruction->GetFieldType();
  CPURegister value = InputCPURegisterAt(instruction, 1);
  Register obj = InputRegisterAt(instruction, 0);
  codegen_->Store(field_type, value, HeapOperand(obj, instruction->GetFieldOffset()));
  if (field_type == Primitive::kPrimNot) {
    codegen_->MarkGCCard(obj, Register(value));
  }
}

void LocationsBuilderARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind =
      instruction->IsClassFinal() ? LocationSummary::kNoCall : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), true);  // The output does overlap inputs.
}

void InstructionCodeGeneratorARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = InputRegisterAt(instruction, 0);;
  Register cls = InputRegisterAt(instruction, 1);;
  Register out = OutputRegister(instruction);

  vixl::Label done;

  // Return 0 if `obj` is null.
  // TODO: Avoid this check if we know `obj` is not null.
  __ Mov(out, 0);
  __ Cbz(obj, &done);

  // Compare the class of `obj` with `cls`.
  __ Ldr(out, HeapOperand(obj, mirror::Object::ClassOffset()));
  __ Cmp(out, cls);
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ Cset(out, eq);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    SlowPathCodeARM64* slow_path =
        new (GetGraph()->GetArena()) TypeCheckSlowPathARM64();
    codegen_->AddSlowPath(slow_path);
    __ B(ne, slow_path->GetEntryLabel());
    __ Mov(out, 1);
    __ Bind(slow_path->GetExitLabel());
  }

  __ Bind(&done);
}

void LocationsBuilderARM64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM64::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(LocationFrom(x0));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  Primitive::Type return_type = invoke->GetType();
  if (return_type != Primitive::kPrimVoid) {
    locations->SetOut(calling_convention_visitor.GetReturnLocation(return_type));
  }
}

void LocationsBuilderARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = WRegisterFrom(invoke->GetLocations()->GetTemp(0));
  uint32_t method_offset = mirror::Class::EmbeddedImTableOffset().Uint32Value() +
          (invoke->GetImtIndex() % mirror::Class::kImtSize) * sizeof(mirror::Class::ImTableEntry);
  Location receiver = invoke->GetLocations()->InAt(0);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  // The register ip1 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so prevent VIXL from using it.
  UseScratchRegisterScope scratch_scope(GetVIXLAssembler());
  scratch_scope.Exclude(ip1);
  __ Mov(ip1, invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ Ldr(temp, StackOperandFrom(receiver));
    __ Ldr(temp, HeapOperand(temp, class_offset));
  } else {
    __ Ldr(temp, HeapOperandFrom(receiver, class_offset));
  }
  // temp = temp->GetImtEntryAt(method_offset);
  __ Ldr(temp, HeapOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, HeapOperand(temp, entry_point));
  // lr();
  __ Blr(lr);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void LocationsBuilderARM64::VisitInvokeStatic(HInvokeStatic* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = WRegisterFrom(invoke->GetLocations()->GetTemp(0));
  // Make sure that ArtMethod* is passed in W0 as per the calling convention
  DCHECK(temp.Is(w0));
  size_t index_in_cache = mirror::Array::DataOffset(kHeapRefSize).SizeValue() +
    invoke->GetIndexInDexCache() * kHeapRefSize;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  codegen_->LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ Ldr(temp, HeapOperand(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset()));
  // temp = temp[index_in_cache];
  __ Ldr(temp, HeapOperand(temp, index_in_cache));
  // lr = temp->entry_point_from_quick_compiled_code_;
  __ Ldr(lr, HeapOperand(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kArm64WordSize)));
  // lr();
  __ Blr(lr);

  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void InstructionCodeGeneratorARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  Register temp = WRegisterFrom(invoke->GetLocations()->GetTemp(0));
  size_t method_offset = mirror::Class::EmbeddedVTableOffset().SizeValue() +
    invoke->GetVTableIndex() * sizeof(mirror::Class::VTableEntry);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ Ldr(temp, MemOperand(sp, receiver.GetStackIndex()));
    __ Ldr(temp, HeapOperand(temp, class_offset));
  } else {
    DCHECK(receiver.IsRegister());
    __ Ldr(temp, HeapOperandFrom(receiver, class_offset));
  }
  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp, HeapOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, HeapOperand(temp, entry_point.SizeValue()));
  // lr();
  __ Blr(lr);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime() ? LocationSummary::kCallOnSlowPath
                                                              : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadClass(HLoadClass* cls) {
  Register out = OutputRegister(cls);
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ Ldr(out, HeapOperand(out, mirror::ArtMethod::DeclaringClassOffset()));
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ Ldr(out, HeapOperand(out, mirror::ArtMethod::DexCacheResolvedTypesOffset()));
    __ Ldr(out, HeapOperand(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));

    SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM64(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ Cbz(out, slow_path->GetEntryLabel());
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadException(HLoadException* instruction) {
  MemOperand exception = MemOperand(tr, Thread::ExceptionOffset<kArm64WordSize>().Int32Value());
  __ Ldr(OutputRegister(instruction), exception);
  __ Str(wzr, exception);
}

void LocationsBuilderARM64::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderARM64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadString(HLoadString* load) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM64(load);
  codegen_->AddSlowPath(slow_path);

  Register out = OutputRegister(load);
  codegen_->LoadCurrentMethod(out);
  __ Ldr(out, HeapOperand(out, mirror::ArtMethod::DeclaringClassOffset()));
  __ Ldr(out, HeapOperand(out, mirror::Class::DexCacheStringsOffset()));
  __ Ldr(out, HeapOperand(out, CodeGenerator::GetCacheOffset(load->GetStringIndex())));
  __ Cbz(out, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderARM64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter()
        ? QUICK_ENTRY_POINT(pLockObject) : QUICK_ENTRY_POINT(pUnlockObject),
      instruction,
      instruction->GetDexPc());
}

void LocationsBuilderARM64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitMul(HMul* mul) {
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Mul(OutputRegister(mul), InputRegisterAt(mul, 0), InputRegisterAt(mul, 1));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fmul(OutputFPRegister(mul), InputFPRegisterAt(mul, 0), InputFPRegisterAt(mul, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RegisterOrConstant(neg->InputAt(0)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitNeg(HNeg* neg) {
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Neg(OutputRegister(neg), InputOperandAt(neg, 0));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fneg(OutputFPRegister(neg), InputFPRegisterAt(neg, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(LocationFrom(x0));
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  InvokeRuntimeCallingConvention calling_convention;
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  Register current_method = RegisterFrom(locations->GetTemp(1), Primitive::kPrimNot);
  DCHECK(current_method.Is(w1));
  codegen_->LoadCurrentMethod(current_method);
  __ Mov(type_index, instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pAllocArrayWithAccessCheck), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  Register current_method = RegisterFrom(locations->GetTemp(1), Primitive::kPrimNot);
  DCHECK(current_method.Is(w1));
  codegen_->LoadCurrentMethod(current_method);
  __ Mov(type_index, instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pAllocObjectWithAccessCheck), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitNot(HNot* instruction) {
  switch (instruction->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
      __ Eor(OutputRegister(instruction), InputRegisterAt(instruction, 0), Operand(1));
      break;

    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Mvn(OutputRegister(instruction), InputOperandAt(instruction, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);
  if (obj.IsRegister()) {
    __ Cbz(RegisterFrom(obj, instruction->InputAt(0)->GetType()), slow_path->GetEntryLabel());
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK_EQ(obj.GetConstant()->AsIntConstant()->GetValue(), 0);
    __ B(slow_path->GetEntryLabel());
  }
}

void LocationsBuilderARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorARM64::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
  UNUSED(instruction);
}

void LocationsBuilderARM64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM64::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM64::VisitRem(HRem* rem) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(rem, LocationSummary::kNoCall);
  switch (rem->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register dividend = InputRegisterAt(rem, 0);
      Register divisor = InputRegisterAt(rem, 1);
      Register output = OutputRegister(rem);
      Register temp = temps.AcquireSameSizeAs(output);

      __ Sdiv(temp, dividend, divisor);
      __ Msub(output, temp, divisor, dividend);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderARM64::VisitReturn(HReturn* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type return_type = instruction->InputAt(0)->GetType();
  locations->SetInAt(0, ARM64ReturnLocation(return_type));
}

void InstructionCodeGeneratorARM64::VisitReturn(HReturn* instruction) {
  UNUSED(instruction);
  codegen_->GenerateFrameExit();
  __ Br(lr);
}

void LocationsBuilderARM64::VisitReturnVoid(HReturnVoid* instruction) {
  instruction->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitReturnVoid(HReturnVoid* instruction) {
  UNUSED(instruction);
  codegen_->GenerateFrameExit();
  __ Br(lr);
}

void LocationsBuilderARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderARM64::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  Primitive::Type field_type = store->InputAt(1)->GetType();
  switch (field_type) {
    case Primitive::kPrimNot:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unimplemented local type " << field_type;
  }
}

void InstructionCodeGeneratorARM64::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  MemOperand field = HeapOperand(InputRegisterAt(instruction, 0), instruction->GetFieldOffset());
  codegen_->Load(instruction->GetType(), OutputCPURegister(instruction), field);
}

void LocationsBuilderARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  CPURegister value = InputCPURegisterAt(instruction, 1);
  Register cls = InputRegisterAt(instruction, 0);
  Offset offset = instruction->GetFieldOffset();
  Primitive::Type field_type = instruction->GetFieldType();

  codegen_->Store(field_type, value, HeapOperand(cls, offset));
  if (field_type == Primitive::kPrimNot) {
    codegen_->MarkGCCard(cls, Register(value));
  }
}

void LocationsBuilderARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (block->GetLoopInformation() != nullptr) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == instruction);
    // The back edge will generate the suspend check.
    return;
  }
  if (block->IsEntryBlock() && instruction->GetNext()->IsGoto()) {
    // The goto will generate the suspend check.
    return;
  }
  GenerateSuspendCheck(instruction, nullptr);
}

void LocationsBuilderARM64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderARM64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pDeliverException), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, LocationSummary::kNoCall);
  Primitive::Type input_type = conversion->GetInputType();
  Primitive::Type result_type = conversion->GetResultType();
  DCHECK_NE(input_type, result_type);
  if ((input_type == Primitive::kPrimNot) || (input_type == Primitive::kPrimVoid) ||
      (result_type == Primitive::kPrimNot) || (result_type == Primitive::kPrimVoid)) {
    LOG(FATAL) << "Unexpected type conversion from " << input_type << " to " << result_type;
  }

  if (IsFPType(input_type)) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
  }

  if (IsFPType(result_type)) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitTypeConversion(HTypeConversion* conversion) {
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();

  DCHECK_NE(input_type, result_type);

  if (IsIntegralType(result_type) && IsIntegralType(input_type)) {
    int result_size = Primitive::ComponentSize(result_type);
    int input_size = Primitive::ComponentSize(input_type);
    int min_size = kBitsPerByte * std::min(result_size, input_size);
    Register output = OutputRegister(conversion);
    Register source = InputRegisterAt(conversion, 0);
    if ((result_type == Primitive::kPrimChar) ||
        ((input_type == Primitive::kPrimChar) && (result_size > input_size))) {
      __ Ubfx(output, output.IsX() ? source.X() : source.W(), 0, min_size);
    } else {
      __ Sbfx(output, output.IsX() ? source.X() : source.W(), 0, min_size);
    }
  } else if (IsFPType(result_type) && IsIntegralType(input_type)) {
    CHECK(input_type == Primitive::kPrimInt || input_type == Primitive::kPrimLong);
    __ Scvtf(OutputFPRegister(conversion), InputRegisterAt(conversion, 0));
  } else if (IsIntegralType(result_type) && IsFPType(input_type)) {
    CHECK(result_type == Primitive::kPrimInt || result_type == Primitive::kPrimLong);
    __ Fcvtzs(OutputRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else if (IsFPType(result_type) && IsFPType(input_type)) {
    __ Fcvt(OutputFPRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type
                << " to " << result_type;
  }
}

void LocationsBuilderARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

#undef __
#undef QUICK_ENTRY_POINT

}  // namespace arm64
}  // namespace art
