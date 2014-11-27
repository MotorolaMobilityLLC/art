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

#include "code_generator_x86_64.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "mirror/object_reference.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/managed_register_x86_64.h"

namespace art {

namespace x86_64 {

static constexpr bool kExplicitStackOverflowCheck = false;

// Some x86_64 instructions require a register to be available as temp.
static constexpr Register TMP = R11;

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static constexpr Register kRuntimeParameterCoreRegisters[] = { RDI, RSI, RDX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr FloatRegister kRuntimeParameterFpuRegisters[] = { };
static constexpr size_t kRuntimeParameterFpuRegistersLength = 0;

class InvokeRuntimeCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

#define __ reinterpret_cast<X86_64Assembler*>(codegen->GetAssembler())->

class SlowPathCodeX86_64 : public SlowPathCode {
 public:
  SlowPathCodeX86_64() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeX86_64);
};

class NullCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit NullCheckSlowPathX86_64(HNullCheck* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ gs()->call(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowNullPointer), true));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HNullCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86_64);
};

class DivZeroCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit DivZeroCheckSlowPathX86_64(HDivZeroCheck* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ gs()->call(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowDivZero), true));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86_64);
};

class DivRemMinusOneSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit DivRemMinusOneSlowPathX86_64(Register reg, Primitive::Type type, bool is_div)
      : cpu_reg_(CpuRegister(reg)), type_(type), is_div_(is_div) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    if (type_ == Primitive::kPrimInt) {
      if (is_div_) {
        __ negl(cpu_reg_);
      } else {
        __ movl(cpu_reg_, Immediate(0));
      }

    } else {
      DCHECK_EQ(Primitive::kPrimLong, type_);
      if (is_div_) {
        __ negq(cpu_reg_);
      } else {
        __ movq(cpu_reg_, Immediate(0));
      }
    }
    __ jmp(GetExitLabel());
  }

 private:
  const CpuRegister cpu_reg_;
  const Primitive::Type type_;
  const bool is_div_;
  DISALLOW_COPY_AND_ASSIGN(DivRemMinusOneSlowPathX86_64);
};

class StackOverflowCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  StackOverflowCheckSlowPathX86_64() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ addq(CpuRegister(RSP),
            Immediate(codegen->GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));
    __ gs()->jmp(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowStackOverflow), true));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathX86_64);
};

class SuspendCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit SuspendCheckSlowPathX86_64(HSuspendCheck* instruction, HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(instruction_->GetLocations());
    __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pTestSuspend), true));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
    codegen->RestoreLiveRegisters(instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ jmp(GetReturnLabel());
    } else {
      __ jmp(x64_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

 private:
  HSuspendCheck* const instruction_;
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86_64);
};

class BoundsCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  BoundsCheckSlowPathX86_64(HBoundsCheck* instruction,
                            Location index_location,
                            Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        index_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        length_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    __ gs()->call(Address::Absolute(
        QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowArrayBounds), true));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86_64);
};

class LoadClassSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  LoadClassSlowPathX86_64(HLoadClass* cls,
                          HInstruction* at,
                          uint32_t dex_pc,
                          bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorX86_64* x64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());

    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(cls_->GetTypeIndex()));
    x64_codegen->LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(1)));
    __ gs()->call(Address::Absolute((do_clinit_
          ? QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pInitializeStaticStorage)
          : QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pInitializeType)) , true));
    codegen->RecordPcInfo(at_, dex_pc_);

    Location out = locations->Out();
    // Move the class to the desired location.
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x64_codegen->Move(out, Location::RegisterLocation(RAX));
    }

    codegen->RestoreLiveRegisters(locations);
    __ jmp(GetExitLabel());
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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathX86_64);
};

class LoadStringSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit LoadStringSlowPathX86_64(HLoadString* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    x64_codegen->LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(0)));
    __ movl(CpuRegister(calling_convention.GetRegisterAt(1)),
            Immediate(instruction_->GetStringIndex()));
    __ gs()->call(Address::Absolute(
        QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pResolveString), true));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
    x64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
    codegen->RestoreLiveRegisters(locations);
    __ jmp(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathX86_64);
};

class TypeCheckSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  TypeCheckSlowPathX86_64(HInstruction* instruction,
                          Location class_to_check,
                          Location object_class,
                          uint32_t dex_pc)
      : instruction_(instruction),
        class_to_check_(class_to_check),
        object_class_(object_class),
        dex_pc_(dex_pc) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        class_to_check_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        object_class_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));

    if (instruction_->IsInstanceOf()) {
      __ gs()->call(
          Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pInstanceofNonTrivial), true));
    } else {
      DCHECK(instruction_->IsCheckCast());
      __ gs()->call(
          Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pCheckCast), true));
    }
    codegen->RecordPcInfo(instruction_, dex_pc_);

    if (instruction_->IsInstanceOf()) {
      x64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
    }

    codegen->RestoreLiveRegisters(locations);
    __ jmp(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86_64);
};

#undef __
#define __ reinterpret_cast<X86_64Assembler*>(GetAssembler())->

inline Condition X86_64Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kLess;
    case kCondLE: return kLessEqual;
    case kCondGT: return kGreater;
    case kCondGE: return kGreaterEqual;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return kEqual;
}

void CodeGeneratorX86_64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromCpuRegister(Register(reg));
}

void CodeGeneratorX86_64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromXmmRegister(FloatRegister(reg));
}

size_t CodeGeneratorX86_64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(Address(CpuRegister(RSP), stack_index), CpuRegister(reg_id));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(CpuRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(Address(CpuRegister(RSP), stack_index), XmmRegister(reg_id));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(XmmRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  return kX86_64WordSize;
}

CodeGeneratorX86_64::CodeGeneratorX86_64(HGraph* graph)
      : CodeGenerator(graph, kNumberOfCpuRegisters, kNumberOfFloatRegisters, 0),
        block_labels_(graph->GetArena(), 0),
        location_builder_(graph, this),
        instruction_visitor_(graph, this),
        move_resolver_(graph->GetArena(), this) {}

size_t CodeGeneratorX86_64::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kX86_64WordSize;
}

InstructionCodeGeneratorX86_64::InstructionCodeGeneratorX86_64(HGraph* graph,
                                                               CodeGeneratorX86_64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

Location CodeGeneratorX86_64::AllocateFreeRegister(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimLong:
    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      size_t reg = FindFreeEntry(blocked_core_registers_, kNumberOfCpuRegisters);
      return Location::RegisterLocation(reg);
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      size_t reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfFloatRegisters);
      return Location::FpuRegisterLocation(reg);
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return Location();
}

void CodeGeneratorX86_64::SetupBlockedRegisters() const {
  // Stack register is always reserved.
  blocked_core_registers_[RSP] = true;

  // Block the register used as TMP.
  blocked_core_registers_[TMP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  blocked_core_registers_[RBX] = true;
  blocked_core_registers_[RBP] = true;
  blocked_core_registers_[R12] = true;
  blocked_core_registers_[R13] = true;
  blocked_core_registers_[R14] = true;
  blocked_core_registers_[R15] = true;

  blocked_fpu_registers_[XMM12] = true;
  blocked_fpu_registers_[XMM13] = true;
  blocked_fpu_registers_[XMM14] = true;
  blocked_fpu_registers_[XMM15] = true;
}

void CodeGeneratorX86_64::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 16;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  bool skip_overflow_check = IsLeafMethod()
      && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86_64);

  if (!skip_overflow_check && !kExplicitStackOverflowCheck) {
    __ testq(CpuRegister(RAX), Address(
        CpuRegister(RSP), -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86_64))));
    RecordPcInfo(nullptr, 0);
  }

  // The return PC has already been pushed on the stack.
  __ subq(CpuRegister(RSP),
          Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));

  if (!skip_overflow_check && kExplicitStackOverflowCheck) {
    SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathX86_64();
    AddSlowPath(slow_path);

    __ gs()->cmpq(CpuRegister(RSP),
                  Address::Absolute(Thread::StackEndOffset<kX86_64WordSize>(), true));
    __ j(kLess, slow_path->GetEntryLabel());
  }

  __ movl(Address(CpuRegister(RSP), kCurrentMethodStackOffset), CpuRegister(RDI));
}

void CodeGeneratorX86_64::GenerateFrameExit() {
  __ addq(CpuRegister(RSP),
          Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));
}

void CodeGeneratorX86_64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorX86_64::LoadCurrentMethod(CpuRegister reg) {
  __ movl(reg, Address(CpuRegister(RSP), kCurrentMethodStackOffset));
}

Location CodeGeneratorX86_64::GetStackLocation(HLoadLocal* load) const {
  switch (load->GetType()) {
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));
      break;

    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << load->GetType();
  }

  LOG(FATAL) << "Unreachable";
  return Location();
}

void CodeGeneratorX86_64::Move(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movq(destination.As<CpuRegister>(), source.As<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movd(destination.As<CpuRegister>(), source.As<XmmRegister>());
    } else if (source.IsStackSlot()) {
      __ movl(destination.As<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(destination.As<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegister()) {
      __ movd(destination.As<XmmRegister>(), source.As<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movaps(destination.As<XmmRegister>(), source.As<XmmRegister>());
    } else if (source.IsStackSlot()) {
      __ movss(destination.As<XmmRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movsd(destination.As<XmmRegister>(),
               Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsStackSlot()) {
    if (source.IsRegister()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.As<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.As<XmmRegister>());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.As<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.As<XmmRegister>());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  }
}

void CodeGeneratorX86_64::Move(HInstruction* instruction,
                               Location location,
                               HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  if (locations != nullptr && locations->Out().IsConstant()) {
    HConstant* const_to_move = locations->Out().GetConstant();
    if (const_to_move->IsIntConstant()) {
      Immediate imm(const_to_move->AsIntConstant()->GetValue());
      if (location.IsRegister()) {
        __ movl(location.As<CpuRegister>(), imm);
      } else if (location.IsStackSlot()) {
        __ movl(Address(CpuRegister(RSP), location.GetStackIndex()), imm);
      } else {
        DCHECK(location.IsConstant());
        DCHECK_EQ(location.GetConstant(), const_to_move);
      }
    } else if (const_to_move->IsLongConstant()) {
      int64_t value = const_to_move->AsLongConstant()->GetValue();
      if (location.IsRegister()) {
        __ movq(location.As<CpuRegister>(), Immediate(value));
      } else if (location.IsDoubleStackSlot()) {
        __ movq(CpuRegister(TMP), Immediate(value));
        __ movq(Address(CpuRegister(RSP), location.GetStackIndex()), CpuRegister(TMP));
      } else {
        DCHECK(location.IsConstant());
        DCHECK_EQ(location.GetConstant(), const_to_move);
      }
    }
  } else if (instruction->IsLoadLocal()) {
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimFloat:
        Move(location, Location::StackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move(location, Location::DoubleStackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      default:
        LOG(FATAL) << "Unexpected local type " << instruction->GetType();
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    Move(location, temp_location);
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimLong:
      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        Move(location, locations->Out());
        break;

      default:
        LOG(FATAL) << "Unexpected type " << instruction->GetType();
    }
  }
}

void LocationsBuilderX86_64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitGoto(HGoto* got) {
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
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86_64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitExit(HExit* exit) {
  UNUSED(exit);
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void LocationsBuilderX86_64::VisitIf(HIf* if_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(if_instr, LocationSummary::kNoCall);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86_64::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                     if_instr->IfTrueSuccessor())) {
        __ jmp(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else {
    bool materialized =
        !cond->IsCondition() || cond->AsCondition()->NeedsMaterialization();
    // Moves do not affect the eflags register, so if the condition is
    // evaluated just before the if, we don't need to evaluate it
    // again.
    bool eflags_set = cond->IsCondition()
        && cond->AsCondition()->IsBeforeWhenDisregardMoves(if_instr);
    if (materialized) {
      if (!eflags_set) {
        // Materialized condition, compare against 0.
        Location lhs = if_instr->GetLocations()->InAt(0);
        if (lhs.IsRegister()) {
          __ cmpl(lhs.As<CpuRegister>(), Immediate(0));
        } else {
          __ cmpl(Address(CpuRegister(RSP), lhs.GetStackIndex()),
                  Immediate(0));
        }
        __ j(kNotEqual, codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      } else {
        __ j(X86_64Condition(cond->AsCondition()->GetCondition()),
             codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      }
    } else {
      Location lhs = cond->GetLocations()->InAt(0);
      Location rhs = cond->GetLocations()->InAt(1);
      if (rhs.IsRegister()) {
        __ cmpl(lhs.As<CpuRegister>(), rhs.As<CpuRegister>());
      } else if (rhs.IsConstant()) {
        __ cmpl(lhs.As<CpuRegister>(),
                Immediate(rhs.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ cmpl(lhs.As<CpuRegister>(),
                Address(CpuRegister(RSP), rhs.GetStackIndex()));
      }
      __ j(X86_64Condition(cond->AsCondition()->GetCondition()),
           codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
    }
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                 if_instr->IfFalseSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
}

void LocationsBuilderX86_64::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderX86_64::VisitLoadLocal(HLoadLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderX86_64::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(store, LocationSummary::kNoCall);
  switch (store->InputAt(1)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unexpected local type " << store->InputAt(1)->GetType();
  }
}

void InstructionCodeGeneratorX86_64::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderX86_64::VisitCondition(HCondition* comp) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(comp, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitCondition(HCondition* comp) {
  if (comp->NeedsMaterialization()) {
    LocationSummary* locations = comp->GetLocations();
    CpuRegister reg = locations->Out().As<CpuRegister>();
    // Clear register: setcc only sets the low byte.
    __ xorq(reg, reg);
    if (locations->InAt(1).IsRegister()) {
      __ cmpl(locations->InAt(0).As<CpuRegister>(),
              locations->InAt(1).As<CpuRegister>());
    } else if (locations->InAt(1).IsConstant()) {
      __ cmpl(locations->InAt(0).As<CpuRegister>(),
              Immediate(locations->InAt(1).GetConstant()->AsIntConstant()->GetValue()));
    } else {
      __ cmpl(locations->InAt(0).As<CpuRegister>(),
              Address(CpuRegister(RSP), locations->InAt(1).GetStackIndex()));
    }
    __ setcc(X86_64Condition(comp->GetCondition()), reg);
  }
}

void LocationsBuilderX86_64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitCompare(HCompare* compare) {
  Label greater, done;
  LocationSummary* locations = compare->GetLocations();
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong:
      __ cmpq(locations->InAt(0).As<CpuRegister>(),
              locations->InAt(1).As<CpuRegister>());
      break;
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare->InputAt(0)->GetType();
  }

  CpuRegister output = locations->Out().As<CpuRegister>();
  __ movl(output, Immediate(0));
  __ j(kEqual, &done);
  __ j(kGreater, &greater);

  __ movl(output, Immediate(-1));
  __ jmp(&done);

  __ Bind(&greater);
  __ movl(output, Immediate(1));

  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86_64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitFloatConstant(HFloatConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86_64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitDoubleConstant(HDoubleConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86_64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitReturnVoid(HReturnVoid* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86_64::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ret, LocationSummary::kNoCall);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0,
          Location::FpuRegisterLocation(XMM0));
      break;

    default:
      LOG(FATAL) << "Unexpected return type " << ret->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorX86_64::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).As<CpuRegister>().AsRegister(), RAX);
        break;

      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        DCHECK_EQ(ret->GetLocations()->InAt(0).As<XmmRegister>().AsFloatRegister(),
                  XMM0);
        break;

      default:
        LOG(FATAL) << "Unexpected return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
  __ ret();
}

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfRegisters()) {
        gp_index_ += 1;
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        gp_index_ += 2;
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case Primitive::kPrimFloat: {
      uint32_t index = fp_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case Primitive::kPrimDouble: {
      uint32_t index = fp_index_++;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }
  return Location();
}

void LocationsBuilderX86_64::VisitInvokeStatic(HInvokeStatic* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeStatic(HInvokeStatic* invoke) {
  CpuRegister temp = invoke->GetLocations()->GetTemp(0).As<CpuRegister>();
  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  codegen_->LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().SizeValue()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, CodeGenerator::GetCacheOffset(invoke->GetIndexInDexCache())));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86_64WordSize).SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(RDI));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  switch (invoke->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      locations->SetOut(Location::RegisterLocation(RAX));
      break;

    case Primitive::kPrimVoid:
      break;

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      locations->SetOut(Location::FpuRegisterLocation(XMM0));
      break;
  }
}

void LocationsBuilderX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  CpuRegister temp = invoke->GetLocations()->GetTemp(0).As<CpuRegister>();
  size_t method_offset = mirror::Class::EmbeddedVTableOffset().SizeValue() +
          invoke->GetVTableIndex() * sizeof(mirror::Class::VTableEntry);
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();
  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(CpuRegister(RSP), receiver.GetStackIndex()));
    __ movl(temp, Address(temp, class_offset));
  } else {
    __ movl(temp, Address(receiver.As<CpuRegister>(), class_offset));
  }
  // temp = temp->GetMethodAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86_64WordSize).SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  CpuRegister temp = invoke->GetLocations()->GetTemp(0).As<CpuRegister>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableOffset().Uint32Value() +
          (invoke->GetImtIndex() % mirror::Class::kImtSize) * sizeof(mirror::Class::ImTableEntry);
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();

  // Set the hidden argument.
  __ movq(invoke->GetLocations()->GetTemp(1).As<CpuRegister>(),
          Immediate(invoke->GetDexMethodIndex()));

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(CpuRegister(RSP), receiver.GetStackIndex()));
    __ movl(temp, Address(temp, class_offset));
  } else {
    __ movl(temp, Address(receiver.As<CpuRegister>(), class_offset));
  }
  // temp = temp->GetImtEntryAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86_64WordSize).SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      // Output overlaps as we need a fresh (zero-initialized)
      // register to perform subtraction from zero.
      locations->SetOut(Location::RequiresFpuRegister());
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negl(out.As<CpuRegister>());
      break;

    case Primitive::kPrimLong:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negq(out.As<CpuRegister>());
      break;

    case Primitive::kPrimFloat:
      DCHECK(in.IsFpuRegister());
      DCHECK(out.IsFpuRegister());
      DCHECK(!in.Equals(out));
      // TODO: Instead of computing negation as a subtraction from
      // zero, implement it with an exclusive or with value 0x80000000
      // (mask for bit 31, representing the sign of a single-precision
      // floating-point number), fetched from a constant pool:
      //
      //   xorps out, [RIP:...] // value at RIP is 0x80 00 00 00

      // out = 0
      __ xorps(out.As<XmmRegister>(), out.As<XmmRegister>());
      // out = out - in
      __ subss(out.As<XmmRegister>(), in.As<XmmRegister>());
      break;

    case Primitive::kPrimDouble:
      DCHECK(in.IsFpuRegister());
      DCHECK(out.IsFpuRegister());
      DCHECK(!in.Equals(out));
      // TODO: Instead of computing negation as a subtraction from
      // zero, implement it with an exclusive or with value
      // 0x8000000000000000 (mask for bit 63, representing the sign of
      // a double-precision floating-point number), fetched from a
      // constant pool:
      //
      //   xorpd out, [RIP:...] // value at RIP is 0x80 00 00 00 00 00 00 00

      // out = 0
      __ xorpd(out.As<XmmRegister>(), out.As<XmmRegister>());
      // out = out - in
      __ subsd(out.As<XmmRegister>(), in.As<XmmRegister>());
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, LocationSummary::kNoCall);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          // TODO: We would benefit from a (to-be-implemented)
          // Location::RegisterOrStackSlot requirement for this input.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-char' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-float' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-double' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-double' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void InstructionCodeGeneratorX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          if (in.IsRegister()) {
            __ movsxb(out.As<CpuRegister>(), in.As<CpuRegister>());
          } else if (in.IsStackSlot()) {
            __ movsxb(out.As<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            __ movl(out.As<CpuRegister>(),
                    Immediate(static_cast<int8_t>(in.GetConstant()->AsIntConstant()->GetValue())));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          if (in.IsRegister()) {
            __ movsxw(out.As<CpuRegister>(), in.As<CpuRegister>());
          } else if (in.IsStackSlot()) {
            __ movsxw(out.As<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            __ movl(out.As<CpuRegister>(),
                    Immediate(static_cast<int16_t>(in.GetConstant()->AsIntConstant()->GetValue())));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          if (in.IsRegister()) {
            __ movl(out.As<CpuRegister>(), in.As<CpuRegister>());
          } else if (in.IsDoubleStackSlot()) {
            __ movl(out.As<CpuRegister>(),
                    Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.As<CpuRegister>(), Immediate(static_cast<int32_t>(value)));
          }
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        DCHECK(out.IsRegister());
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          DCHECK(in.IsRegister());
          __ movsxd(out.As<CpuRegister>(), in.As<CpuRegister>());
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-char' instruction.
          if (in.IsRegister()) {
            __ movzxw(out.As<CpuRegister>(), in.As<CpuRegister>());
          } else if (in.IsStackSlot()) {
            __ movzxw(out.As<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            __ movl(out.As<CpuRegister>(),
                    Immediate(static_cast<uint16_t>(in.GetConstant()->AsIntConstant()->GetValue())));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
          // Processing a Dex `int-to-float' instruction.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          __ cvtsi2ss(out.As<XmmRegister>(), in.As<CpuRegister>());
          break;

        case Primitive::kPrimLong:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
          // Processing a Dex `int-to-double' instruction.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          __ cvtsi2sd(out.As<XmmRegister>(), in.As<CpuRegister>(), false);
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-double' instruction.
          __ cvtsi2sd(out.As<XmmRegister>(), in.As<CpuRegister>(), true);
          break;

        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void LocationsBuilderX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(add, LocationSummary::kNoCall);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ addl(first.As<CpuRegister>(), second.As<CpuRegister>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ addl(first.As<CpuRegister>(), imm);
      } else {
        __ addl(first.As<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ addq(first.As<CpuRegister>(), second.As<CpuRegister>());
      break;
    }

    case Primitive::kPrimFloat: {
      __ addss(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ addsd(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ subl(first.As<CpuRegister>(), second.As<CpuRegister>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ subl(first.As<CpuRegister>(), imm);
      } else {
        __ subl(first.As<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      __ subq(first.As<CpuRegister>(), second.As<CpuRegister>());
      break;
    }

    case Primitive::kPrimFloat: {
      __ subss(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ subsd(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ imull(first.As<CpuRegister>(), second.As<CpuRegister>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ imull(first.As<CpuRegister>(), imm);
      } else {
        DCHECK(second.IsStackSlot());
        __ imull(first.As<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      __ imulq(first.As<CpuRegister>(), second.As<CpuRegister>());
      break;
    }

    case Primitive::kPrimFloat: {
      __ mulss(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ mulsd(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || Primitive::kPrimLong);

  bool is_div = instruction->IsDiv();
  LocationSummary* locations = instruction->GetLocations();

  CpuRegister out_reg = locations->Out().As<CpuRegister>();
  CpuRegister second_reg = locations->InAt(1).As<CpuRegister>();

  DCHECK_EQ(RAX, locations->InAt(0).As<CpuRegister>().AsRegister());
  DCHECK_EQ(is_div ? RAX : RDX, out_reg.AsRegister());

  SlowPathCodeX86_64* slow_path =
      new (GetGraph()->GetArena()) DivRemMinusOneSlowPathX86_64(
          out_reg.AsRegister(), type, is_div);
  codegen_->AddSlowPath(slow_path);

  // 0x80000000(00000000)/-1 triggers an arithmetic exception!
  // Dividing by -1 is actually negation and -0x800000000(00000000) = 0x80000000(00000000)
  // so it's safe to just use negl instead of more complex comparisons.

  __ cmpl(second_reg, Immediate(-1));
  __ j(kEqual, slow_path->GetEntryLabel());

  if (type == Primitive::kPrimInt) {
    // edx:eax <- sign-extended of eax
    __ cdq();
    // eax = quotient, edx = remainder
    __ idivl(second_reg);
  } else {
    // rdx:rax <- sign-extended of rax
    __ cqo();
    // rax = quotient, rdx = remainder
    __ idivq(second_reg);
  }

  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      // Intel uses edx:eax as the dividend.
      locations->AddTemp(Location::RegisterLocation(RDX));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  Primitive::Type type = div->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(div);
      break;
    }

    case Primitive::kPrimFloat: {
      __ divss(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ divsd(first.As<XmmRegister>(), second.As<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitRem(HRem* rem) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(rem, LocationSummary::kNoCall);
  switch (rem->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RequiresRegister());
      // Intel uses rdx:rax as the dividend and puts the remainder in rdx
      locations->SetOut(Location::RegisterLocation(RDX));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      LOG(FATAL) << "Unimplemented rem type " << rem->GetResultType();
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(rem);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      LOG(FATAL) << "Unimplemented rem type " << rem->GetResultType();
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::Any());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeX86_64* slow_path =
      new (GetGraph()->GetArena()) DivZeroCheckSlowPathX86_64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case Primitive::kPrimInt: {
      if (value.IsRegister()) {
        __ testl(value.As<CpuRegister>(), value.As<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsStackSlot()) {
        __ cmpl(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
        __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (value.IsRegister()) {
        __ testq(value.As<CpuRegister>(), value.As<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsDoubleStackSlot()) {
        __ cmpq(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
        __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
  }
}

void LocationsBuilderX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(op, LocationSummary::kNoCall);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL.
      locations->SetInAt(1, Location::ByteRegisterOrConstant(RCX, op->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  CpuRegister first_reg = locations->InAt(0).As<CpuRegister>();
  Location second = locations->InAt(1);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        CpuRegister second_reg = second.As<CpuRegister>();
        if (op->IsShl()) {
          __ shll(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarl(first_reg, second_reg);
        } else {
          __ shrl(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        if (op->IsShl()) {
          __ shll(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarl(first_reg, imm);
        } else {
          __ shrl(first_reg, imm);
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (second.IsRegister()) {
        CpuRegister second_reg = second.As<CpuRegister>();
        if (op->IsShl()) {
          __ shlq(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarq(first_reg, second_reg);
        } else {
          __ shrq(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        if (op->IsShl()) {
          __ shlq(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarq(first_reg, imm);
        } else {
          __ shrq(first_reg, imm);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderX86_64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(1)));
  __ movq(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(instruction->GetTypeIndex()));

  __ gs()->call(Address::Absolute(
      QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocObjectWithAccessCheck), true));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86_64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(RAX));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorX86_64::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(1)));
  __ movq(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(instruction->GetTypeIndex()));

  __ gs()->call(Address::Absolute(
      QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocArrayWithAccessCheck), true));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86_64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorX86_64::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
  UNUSED(instruction);
}

void LocationsBuilderX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  DCHECK_EQ(locations->InAt(0).As<CpuRegister>().AsRegister(),
            locations->Out().As<CpuRegister>().AsRegister());
  Location out = locations->Out();
  switch (not_->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
      __ xorq(out.As<CpuRegister>(), Immediate(1));
      break;

    case Primitive::kPrimInt:
      __ notl(out.As<CpuRegister>());
      break;

    case Primitive::kPrimLong:
      __ notq(out.As<CpuRegister>());
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorX86_64::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unimplemented";
}

void LocationsBuilderX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Primitive::Type field_type = instruction->GetFieldType();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue());
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (needs_write_barrier) {
    // Temporary registers for the write barrier.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movb(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movw(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movl(Address(obj, offset), value);
      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue())) {
        CpuRegister temp = locations->GetTemp(0).As<CpuRegister>();
        CpuRegister card = locations->GetTemp(1).As<CpuRegister>();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movq(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister value = locations->InAt(1).As<XmmRegister>();
      __ movss(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister value = locations->InAt(1).As<XmmRegister>();
      __ movsd(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movzxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimByte: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movsxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimShort: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movsxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimChar: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movzxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movl(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimLong: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movq(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister out = locations->Out().As<XmmRegister>();
      __ movss(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister out = locations->Out().As<XmmRegister>();
      __ movsd(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::Any());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathX86_64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ cmpl(obj.As<CpuRegister>(), Immediate(0));
  } else if (obj.IsStackSlot()) {
    __ cmpl(Address(CpuRegister(RSP), obj.GetStackIndex()), Immediate(0));
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK_EQ(obj.GetConstant()->AsIntConstant()->GetValue(), 0);
    __ jmp(slow_path->GetEntryLabel());
    return;
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void LocationsBuilderX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(
      1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movzxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movzxb(out, Address(obj, index.As<CpuRegister>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movsxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movsxb(out, Address(obj, index.As<CpuRegister>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movsxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movsxw(out, Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      CpuRegister out = locations->Out().As<CpuRegister>();
      if (index.IsConstant()) {
        __ movq(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movq(out, Address(obj, index.As<CpuRegister>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      XmmRegister out = locations->Out().As<XmmRegister>();
      if (index.IsConstant()) {
        __ movss(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movss(out, Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      XmmRegister out = locations->Out().As<XmmRegister>();
      if (index.IsConstant()) {
        __ movsd(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movsd(out, Address(obj, index.As<CpuRegister>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();

  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  bool needs_runtime_call = instruction->NeedsTypeCheck();

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, needs_runtime_call ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (needs_runtime_call) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(
        1, Location::RegisterOrConstant(instruction->InputAt(1)));
    locations->SetInAt(2, Location::RequiresRegister());
    if (value_type == Primitive::kPrimLong) {
      locations->SetInAt(2, Location::RequiresRegister());
    } else if (value_type == Primitive::kPrimFloat || value_type == Primitive::kPrimDouble) {
      locations->SetInAt(2, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)));
    }

    if (needs_write_barrier) {
      // Temporary registers for the write barrier.
      locations->AddTemp(Location::RequiresRegister());
      locations->AddTemp(Location::RequiresRegister());
    }
  }
}

void InstructionCodeGeneratorX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        if (value.IsRegister()) {
          __ movb(Address(obj, offset), value.As<CpuRegister>());
        } else {
          __ movb(Address(obj, offset), Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        if (value.IsRegister()) {
          __ movb(Address(obj, index.As<CpuRegister>(), TIMES_1, data_offset),
                  value.As<CpuRegister>());
        } else {
          __ movb(Address(obj, index.As<CpuRegister>(), TIMES_1, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        if (value.IsRegister()) {
          __ movw(Address(obj, offset), value.As<CpuRegister>());
        } else {
          DCHECK(value.IsConstant()) << value;
          __ movw(Address(obj, offset), Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        DCHECK(index.IsRegister()) << index;
        if (value.IsRegister()) {
          __ movw(Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset),
                  value.As<CpuRegister>());
        } else {
          DCHECK(value.IsConstant()) << value;
          __ movw(Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        if (index.IsConstant()) {
          size_t offset =
              (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          if (value.IsRegister()) {
            __ movl(Address(obj, offset), value.As<CpuRegister>());
          } else {
            DCHECK(value.IsConstant()) << value;
            __ movl(Address(obj, offset),
                    Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
          }
        } else {
          DCHECK(index.IsRegister()) << index;
          if (value.IsRegister()) {
            __ movl(Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset),
                    value.As<CpuRegister>());
          } else {
            DCHECK(value.IsConstant()) << value;
            __ movl(Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset),
                    Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
          }
        }

        if (needs_write_barrier) {
          DCHECK_EQ(value_type, Primitive::kPrimNot);
          CpuRegister temp = locations->GetTemp(0).As<CpuRegister>();
          CpuRegister card = locations->GetTemp(1).As<CpuRegister>();
          codegen_->MarkGCCard(temp, card, obj, value.As<CpuRegister>());
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAputObject), true));
        DCHECK(!codegen_->IsLeafMethod());
        codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        DCHECK(value.IsRegister());
        __ movq(Address(obj, offset), value.As<CpuRegister>());
      } else {
        DCHECK(value.IsRegister());
        __ movq(Address(obj, index.As<CpuRegister>(), TIMES_8, data_offset),
                value.As<CpuRegister>());
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        DCHECK(value.IsFpuRegister());
        __ movss(Address(obj, offset), value.As<XmmRegister>());
      } else {
        DCHECK(value.IsFpuRegister());
        __ movss(Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset),
                value.As<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        DCHECK(value.IsFpuRegister());
        __ movsd(Address(obj, offset), value.As<XmmRegister>());
      } else {
        DCHECK(value.IsFpuRegister());
        __ movsd(Address(obj, index.As<CpuRegister>(), TIMES_8, data_offset),
                value.As<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  CpuRegister out = locations->Out().As<CpuRegister>();
  __ movl(out, Address(obj, offset));
}

void LocationsBuilderX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathX86_64(
      instruction, locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  CpuRegister index = locations->InAt(0).As<CpuRegister>();
  CpuRegister length = locations->InAt(1).As<CpuRegister>();

  __ cmpl(index, length);
  __ j(kAboveEqual, slow_path->GetEntryLabel());
}

void CodeGeneratorX86_64::MarkGCCard(CpuRegister temp,
                                     CpuRegister card,
                                     CpuRegister object,
                                     CpuRegister value) {
  Label is_null;
  __ testl(value, value);
  __ j(kEqual, &is_null);
  __ gs()->movq(card, Address::Absolute(
      Thread::CardTableOffset<kX86_64WordSize>().Int32Value(), true));
  __ movq(temp, object);
  __ shrq(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0),  card);
  __ Bind(&is_null);
}

void LocationsBuilderX86_64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderX86_64::VisitParallelMove(HParallelMove* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorX86_64::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void InstructionCodeGeneratorX86_64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                          HBasicBlock* successor) {
  SuspendCheckSlowPathX86_64* slow_path =
      new (GetGraph()->GetArena()) SuspendCheckSlowPathX86_64(instruction, successor);
  codegen_->AddSlowPath(slow_path);
  __ gs()->cmpw(Address::Absolute(
      Thread::ThreadFlagsOffset<kX86_64WordSize>().Int32Value(), true), Immediate(0));
  if (successor == nullptr) {
    __ j(kNotEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ j(kEqual, codegen_->GetLabelOf(successor));
    __ jmp(slow_path->GetEntryLabel());
  }
}

X86_64Assembler* ParallelMoveResolverX86_64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86_64::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movq(destination.As<CpuRegister>(), source.As<CpuRegister>());
    } else if (destination.IsStackSlot()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.As<CpuRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.As<CpuRegister>());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.As<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movss(destination.As<XmmRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.As<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movsd(destination.As<XmmRegister>(), Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsConstant()) {
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant()) {
      Immediate imm(constant->AsIntConstant()->GetValue());
      if (destination.IsRegister()) {
        __ movl(destination.As<CpuRegister>(), imm);
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegister()) {
        __ movq(destination.As<CpuRegister>(), Immediate(value));
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        __ movq(CpuRegister(TMP), Immediate(value));
        __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
      }
    } else if (constant->IsFloatConstant()) {
      Immediate imm(bit_cast<float, int32_t>(constant->AsFloatConstant()->GetValue()));
      if (destination.IsFpuRegister()) {
        __ movl(CpuRegister(TMP), imm);
        __ movd(destination.As<XmmRegister>(), CpuRegister(TMP));
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else {
      DCHECK(constant->IsDoubleConstant()) << constant->DebugName();
      Immediate imm(bit_cast<double, int64_t>(constant->AsDoubleConstant()->GetValue()));
      if (destination.IsFpuRegister()) {
        __ movq(CpuRegister(TMP), imm);
        __ movd(destination.As<XmmRegister>(), CpuRegister(TMP));
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        __ movq(CpuRegister(TMP), imm);
        __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
      }
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      __ movaps(destination.As<XmmRegister>(), source.As<XmmRegister>());
    } else if (destination.IsStackSlot()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.As<XmmRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.As<XmmRegister>());
    }
  }
}

void ParallelMoveResolverX86_64::Exchange32(CpuRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movl(Address(CpuRegister(RSP), mem), reg);
  __ movl(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange32(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movl(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movl(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movl(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movq(Address(CpuRegister(RSP), mem), reg);
  __ movq(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movq(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movq(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movq(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::Exchange32(XmmRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movss(Address(CpuRegister(RSP), mem), reg);
  __ movd(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(XmmRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movsd(Address(CpuRegister(RSP), mem), reg);
  __ movd(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgq(destination.As<CpuRegister>(), source.As<CpuRegister>());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange32(source.As<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange32(destination.As<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange32(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.As<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsRegister()) {
    Exchange64(destination.As<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    Exchange64(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    __ movd(CpuRegister(TMP), source.As<XmmRegister>());
    __ movaps(source.As<XmmRegister>(), destination.As<XmmRegister>());
    __ movd(destination.As<XmmRegister>(), CpuRegister(TMP));
  } else if (source.IsFpuRegister() && destination.IsStackSlot()) {
    Exchange32(source.As<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsFpuRegister()) {
    Exchange32(destination.As<XmmRegister>(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.As<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsFpuRegister()) {
    Exchange64(destination.As<XmmRegister>(), source.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented swap between " << source << " and " << destination;
  }
}


void ParallelMoveResolverX86_64::SpillScratch(int reg) {
  __ pushq(CpuRegister(reg));
}


void ParallelMoveResolverX86_64::RestoreScratch(int reg) {
  __ popq(CpuRegister(reg));
}

void InstructionCodeGeneratorX86_64::GenerateClassInitializationCheck(
    SlowPathCodeX86_64* slow_path, CpuRegister class_reg) {
  __ cmpl(Address(class_reg,  mirror::Class::StatusOffset().Int32Value()),
          Immediate(mirror::Class::kStatusInitialized));
  __ j(kLess, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
  // No need for memory fence, thanks to the X86_64 memory model.
}

void LocationsBuilderX86_64::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadClass(HLoadClass* cls) {
  CpuRegister out = cls->GetLocations()->Out().As<CpuRegister>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ movl(out, Address(out, mirror::ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ movl(out, Address(out, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value()));
    __ movl(out, Address(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));
    SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86_64(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ testl(out, out);
    __ j(kEqual, slow_path->GetEntryLabel());
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderX86_64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class to not be null.
  SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86_64(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, check->GetLocations()->InAt(0).As<CpuRegister>());
}

void LocationsBuilderX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister cls = locations->InAt(0).As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movzxb(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimByte: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movsxb(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimShort: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movsxw(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimChar: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movzxw(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movl(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimLong: {
      CpuRegister out = locations->Out().As<CpuRegister>();
      __ movq(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister out = locations->Out().As<XmmRegister>();
      __ movss(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister out = locations->Out().As<XmmRegister>();
      __ movsd(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Primitive::Type field_type = instruction->GetFieldType();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue());
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (needs_write_barrier) {
    // Temporary registers for the write barrier.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister cls = locations->InAt(0).As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movb(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movw(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movl(Address(cls, offset), value);
      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue())) {
        CpuRegister temp = locations->GetTemp(0).As<CpuRegister>();
        CpuRegister card = locations->GetTemp(1).As<CpuRegister>();
        codegen_->MarkGCCard(temp, card, cls, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      CpuRegister value = locations->InAt(1).As<CpuRegister>();
      __ movq(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister value = locations->InAt(1).As<XmmRegister>();
      __ movss(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister value = locations->InAt(1).As<XmmRegister>();
      __ movsd(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadString(HLoadString* load) {
  SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathX86_64(load);
  codegen_->AddSlowPath(slow_path);

  CpuRegister out = load->GetLocations()->Out().As<CpuRegister>();
  codegen_->LoadCurrentMethod(CpuRegister(out));
  __ movl(out, Address(out, mirror::ArtMethod::DeclaringClassOffset().Int32Value()));
  __ movl(out, Address(out, mirror::Class::DexCacheStringsOffset().Int32Value()));
  __ movl(out, Address(out, CodeGenerator::GetCacheOffset(load->GetStringIndex())));
  __ testl(out, out);
  __ j(kEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadException(HLoadException* load) {
  Address address = Address::Absolute(
      Thread::ExceptionOffset<kX86_64WordSize>().Int32Value(), true);
  __ gs()->movl(load->GetLocations()->Out().As<CpuRegister>(), address);
  __ gs()->movl(address, Immediate(0));
}

void LocationsBuilderX86_64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitThrow(HThrow* instruction) {
  __ gs()->call(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pDeliverException), true));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86_64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = instruction->IsClassFinal()
      ? LocationSummary::kNoCall
      : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  Location cls = locations->InAt(1);
  CpuRegister out = locations->Out().As<CpuRegister>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Label done, zero;
  SlowPathCodeX86_64* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // TODO: avoid this check if we know obj is not null.
  __ testl(obj, obj);
  __ j(kEqual, &zero);
  // Compare the class of `obj` with `cls`.
  __ movl(out, Address(obj, class_offset));
  if (cls.IsRegister()) {
    __ cmpl(out, cls.As<CpuRegister>());
  } else {
    DCHECK(cls.IsStackSlot()) << cls;
    __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
  }
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ j(kNotEqual, &zero);
    __ movl(out, Immediate(1));
    __ jmp(&done);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86_64(
        instruction, locations->InAt(1), locations->Out(), instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ j(kNotEqual, slow_path->GetEntryLabel());
    __ movl(out, Immediate(1));
    __ jmp(&done);
  }
  __ Bind(&zero);
  __ movl(out, Immediate(0));
  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  Location cls = locations->InAt(1);
  CpuRegister temp = locations->GetTemp(0).As<CpuRegister>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  SlowPathCodeX86_64* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86_64(
      instruction, locations->InAt(1), locations->GetTemp(0), instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // TODO: avoid this check if we know obj is not null.
  __ testl(obj, obj);
  __ j(kEqual, slow_path->GetExitLabel());
  // Compare the class of `obj` with `cls`.
  __ movl(temp, Address(obj, class_offset));
  if (cls.IsRegister()) {
    __ cmpl(temp, cls.As<CpuRegister>());
  } else {
    DCHECK(cls.IsStackSlot()) << cls;
    __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
  }
  // Classes must be equal for the checkcast to succeed.
  __ j(kNotEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  __ gs()->call(Address::Absolute(instruction->IsEnter()
        ? QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pLockObject)
        : QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pUnlockObject),
      true));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86_64::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == Primitive::kPrimInt
         || instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->GetType() == Primitive::kPrimInt) {
    locations->SetInAt(1, Location::Any());
  } else {
    // Request a register to avoid loading a 64bits constant.
    locations->SetInAt(1, Location::RequiresRegister());
  }
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    if (second.IsRegister()) {
      if (instruction->IsAnd()) {
        __ andl(first.As<CpuRegister>(), second.As<CpuRegister>());
      } else if (instruction->IsOr()) {
        __ orl(first.As<CpuRegister>(), second.As<CpuRegister>());
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.As<CpuRegister>(), second.As<CpuRegister>());
      }
    } else if (second.IsConstant()) {
      Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
      if (instruction->IsAnd()) {
        __ andl(first.As<CpuRegister>(), imm);
      } else if (instruction->IsOr()) {
        __ orl(first.As<CpuRegister>(), imm);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.As<CpuRegister>(), imm);
      }
    } else {
      Address address(CpuRegister(RSP), second.GetStackIndex());
      if (instruction->IsAnd()) {
        __ andl(first.As<CpuRegister>(), address);
      } else if (instruction->IsOr()) {
        __ orl(first.As<CpuRegister>(), address);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.As<CpuRegister>(), address);
      }
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    if (instruction->IsAnd()) {
      __ andq(first.As<CpuRegister>(), second.As<CpuRegister>());
    } else if (instruction->IsOr()) {
      __ orq(first.As<CpuRegister>(), second.As<CpuRegister>());
    } else {
      DCHECK(instruction->IsXor());
      __ xorq(first.As<CpuRegister>(), second.As<CpuRegister>());
    }
  }
}

}  // namespace x86_64
}  // namespace art
