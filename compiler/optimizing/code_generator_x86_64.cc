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
    CodeGeneratorX86_64* x64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    InvokeRuntimeCallingConvention calling_convention;
    x64_codegen->Move(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), index_location_);
    x64_codegen->Move(Location::RegisterLocation(calling_convention.GetRegisterAt(1)), length_location_);
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

void CodeGeneratorX86_64::SaveCoreRegister(Location stack_location, uint32_t reg_id) {
  __ movq(Address(CpuRegister(RSP), stack_location.GetStackIndex()), CpuRegister(reg_id));
}

void CodeGeneratorX86_64::RestoreCoreRegister(Location stack_location, uint32_t reg_id) {
  __ movq(CpuRegister(reg_id), Address(CpuRegister(RSP), stack_location.GetStackIndex()));
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

void InstructionCodeGeneratorX86_64::LoadCurrentMethod(CpuRegister reg) {
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
  if (instruction->IsIntConstant()) {
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (location.IsRegister()) {
      __ movl(location.As<CpuRegister>(), imm);
    } else {
      __ movl(Address(CpuRegister(RSP), location.GetStackIndex()), imm);
    }
  } else if (instruction->IsLongConstant()) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ movq(location.As<CpuRegister>(), Immediate(value));
    } else {
      __ movq(CpuRegister(TMP), Immediate(value));
      __ movq(Address(CpuRegister(RSP), location.GetStackIndex()), CpuRegister(TMP));
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
        Move(location, instruction->GetLocations()->Out());
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
    locations->SetInAt(0, Location::Any(), Location::kDiesAtEntry);
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
}

void LocationsBuilderX86_64::VisitCondition(HCondition* comp) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(comp, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetInAt(1, Location::Any(), Location::kDiesAtEntry);
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
  locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetInAt(1, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetOut(Location::RequiresRegister());
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
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitReturnVoid(HReturnVoid* ret) {
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
  uint32_t heap_reference_size = sizeof(mirror::HeapReference<mirror::Object>);
  size_t index_in_cache = mirror::Array::DataOffset(heap_reference_size).SizeValue() +
      invoke->GetIndexInDexCache() * heap_reference_size;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().SizeValue()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, index_in_cache));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
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
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
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
      locations->SetInAt(1, Location::Any());
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
        HConstant* instruction = second.GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ addl(first.As<CpuRegister>(), imm);
      } else {
        __ addl(first.As<CpuRegister>(),
                Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ addq(first.As<CpuRegister>(), second.As<CpuRegister>());
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ addss(first.As<XmmRegister>(), second.As<XmmRegister>());
      } else {
        __ addss(first.As<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ addsd(first.As<XmmRegister>(), second.As<XmmRegister>());
      } else {
        __ addsd(first.As<XmmRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
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

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  DCHECK_EQ(locations->InAt(0).As<CpuRegister>().AsRegister(),
            locations->Out().As<CpuRegister>().AsRegister());
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (locations->InAt(1).IsRegister()) {
        __ subl(locations->InAt(0).As<CpuRegister>(),
                locations->InAt(1).As<CpuRegister>());
      } else if (locations->InAt(1).IsConstant()) {
        HConstant* instruction = locations->InAt(1).GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ subl(locations->InAt(0).As<CpuRegister>(), imm);
      } else {
        __ subl(locations->InAt(0).As<CpuRegister>(),
                Address(CpuRegister(RSP), locations->InAt(1).GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      __ subq(locations->InAt(0).As<CpuRegister>(),
              locations->InAt(1).As<CpuRegister>());
      break;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented sub type " << sub->GetResultType();
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

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented mul type " << mul->GetResultType();
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

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented mul type " << mul->GetResultType();
  }
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
  LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(1)));
  __ movq(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(instruction->GetTypeIndex()));

  __ gs()->call(Address::Absolute(
      QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocObjectWithAccessCheck), true));

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
}

void LocationsBuilderX86_64::VisitNot(HNot* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK_EQ(locations->InAt(0).As<CpuRegister>().AsRegister(),
            locations->Out().As<CpuRegister>().AsRegister());
  __ xorq(locations->Out().As<CpuRegister>(), Immediate(1));
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
  LOG(FATAL) << "Unimplemented";
}

void LocationsBuilderX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Primitive::Type field_type = instruction->GetFieldType();
  bool is_object_type = field_type == Primitive::kPrimNot;
  bool dies_at_entry = !is_object_type;
  locations->SetInAt(0, Location::RequiresRegister(), dies_at_entry);
  locations->SetInAt(1, Location::RequiresRegister(), dies_at_entry);
  if (is_object_type) {
    // Temporary registers for the write barrier.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  CpuRegister value = locations->InAt(1).As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ movb(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      __ movw(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(Address(obj, offset), value);
      if (field_type == Primitive::kPrimNot) {
        CpuRegister temp = locations->GetTemp(0).As<CpuRegister>();
        CpuRegister card = locations->GetTemp(1).As<CpuRegister>();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ movq(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << field_type;
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  CpuRegister out = locations->Out().As<CpuRegister>();
  size_t offset = instruction->GetFieldOffset().SizeValue();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      __ movzxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimByte: {
      __ movsxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimShort: {
      __ movsxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimChar: {
      __ movzxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimLong: {
      __ movq(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
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
  locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetInAt(
      1, Location::RegisterOrConstant(instruction->InputAt(1)), Location::kDiesAtEntry);
  locations->SetOut(Location::RequiresRegister());
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

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  bool is_object = value_type == Primitive::kPrimNot;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, is_object ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (is_object) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
    locations->SetInAt(
        1, Location::RegisterOrConstant(instruction->InputAt(1)), Location::kDiesAtEntry);
    locations->SetInAt(2, Location::RequiresRegister(), Location::kDiesAtEntry);
    if (value_type == Primitive::kPrimLong) {
      locations->SetInAt(2, Location::RequiresRegister(), Location::kDiesAtEntry);
    } else {
      locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)), Location::kDiesAtEntry);
    }
  }
}

void InstructionCodeGeneratorX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).As<CpuRegister>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();

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
          __ movw(Address(obj, offset), Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        if (value.IsRegister()) {
          __ movw(Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset),
                  value.As<CpuRegister>());
        } else {
          __ movw(Address(obj, index.As<CpuRegister>(), TIMES_2, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        if (value.IsRegister()) {
          __ movl(Address(obj, offset), value.As<CpuRegister>());
        } else {
          __ movl(Address(obj, offset), Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        if (value.IsRegister()) {
          __ movl(Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset),
                  value.As<CpuRegister>());
        } else {
          __ movl(Address(obj, index.As<CpuRegister>(), TIMES_4, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      break;
    }

    case Primitive::kPrimNot: {
      __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAputObject), true));
      DCHECK(!codegen_->IsLeafMethod());
      codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
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

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister(), Location::kDiesAtEntry);
  locations->SetOut(Location::RequiresRegister());
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
}

void LocationsBuilderX86_64::VisitParallelMove(HParallelMove* instruction) {
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
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.As<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot());
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
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegister()) {
        __ movq(destination.As<CpuRegister>(), Immediate(value));
      } else {
        __ movq(CpuRegister(TMP), Immediate(value));
        __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
      }
    } else {
      LOG(FATAL) << "Unimplemented constant type";
    }
  } else {
    LOG(FATAL) << "Unimplemented";
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
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}


void ParallelMoveResolverX86_64::SpillScratch(int reg) {
  __ pushq(CpuRegister(reg));
}


void ParallelMoveResolverX86_64::RestoreScratch(int reg) {
  __ popq(CpuRegister(reg));
}

}  // namespace x86_64
}  // namespace art
