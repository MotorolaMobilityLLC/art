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

#include "code_generator_x86.h"
#include "utils/assembler.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/managed_register_x86.h"

#include "mirror/array.h"
#include "mirror/art_method.h"

#define __ reinterpret_cast<X86Assembler*>(GetAssembler())->

namespace art {

x86::X86ManagedRegister Location::AsX86() const {
  return reg().AsX86();
}

namespace x86 {

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static Location X86CpuLocation(Register reg) {
  return Location::RegisterLocation(X86ManagedRegister::FromCpuRegister(reg));
}

InstructionCodeGeneratorX86::InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorX86::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 8;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  // Add the current ART method to the frame size, the return PC, and the filler.
  SetFrameSize(RoundUp((
      GetGraph()->GetMaximumNumberOfOutVRegs() + GetGraph()->GetNumberOfVRegs() + 3) * kX86WordSize,
      kStackAlignment));
  // The return PC has already been pushed on the stack.
  __ subl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
  __ movl(Address(ESP, kCurrentMethodStackOffset), EAX);
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ addl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
}

void CodeGeneratorX86::Bind(Label* label) {
  __ Bind(label);
}

void InstructionCodeGeneratorX86::LoadCurrentMethod(Register reg) {
  __ movl(reg, Address(ESP, kCurrentMethodStackOffset));
}

int32_t CodeGeneratorX86::GetStackSlot(HLocal* local) const {
  uint16_t reg_number = local->GetRegNumber();
  uint16_t number_of_vregs = GetGraph()->GetNumberOfVRegs();
  uint16_t number_of_in_vregs = GetGraph()->GetNumberOfInVRegs();
  if (reg_number >= number_of_vregs - number_of_in_vregs) {
    // Local is a parameter of the method. It is stored in the caller's frame.
    return GetFrameSize() + kX86WordSize  // ART method
                          + (reg_number - number_of_vregs + number_of_in_vregs) * kX86WordSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - (kNumberOfPushedRegistersAtEntry * kX86WordSize)
                          - kX86WordSize  // filler.
                          - (number_of_vregs * kX86WordSize)
                          + (reg_number * kX86WordSize);
  }
}

static constexpr Register kParameterCoreRegisters[] = { ECX, EDX, EBX };
static constexpr RegisterPair kParameterCorePairRegisters[] = { ECX_EDX, EDX_EBX };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

class InvokeDexCallingConvention : public CallingConvention<Register> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters, kParameterCoreRegistersLength) {}

  RegisterPair GetRegisterPairAt(size_t argument_index) {
    DCHECK_LT(argument_index + 1, GetNumberOfRegisters());
    return kParameterCorePairRegisters[argument_index];
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

void CodeGeneratorX86::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movl(destination.AsX86().AsCpuRegister(), source.AsX86().AsCpuRegister());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(destination.AsX86().AsCpuRegister(), Address(ESP, source.GetStackIndex()));
    }
  } else {
    if (source.IsRegister()) {
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsX86().AsCpuRegister());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(EAX, Address(ESP, source.GetStackIndex()));
      __ movl(Address(ESP, destination.GetStackIndex()), EAX);
    }
  }
}

void CodeGeneratorX86::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movl(destination.AsX86().AsRegisterPairLow(), source.AsX86().AsRegisterPairLow());
      __ movl(destination.AsX86().AsRegisterPairHigh(), source.AsX86().AsRegisterPairHigh());
    } else if (source.IsQuickParameter()) {
      uint32_t argument_index = source.GetQuickParameterIndex();
      InvokeDexCallingConvention calling_convention;
      __ movl(destination.AsX86().AsRegisterPairLow(),
              calling_convention.GetRegisterAt(argument_index));
      __ movl(destination.AsX86().AsRegisterPairHigh(),
              Address(ESP,
                      calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movl(destination.AsX86().AsRegisterPairLow(), Address(ESP, source.GetStackIndex()));
      __ movl(destination.AsX86().AsRegisterPairHigh(),
              Address(ESP, source.GetHighStackIndex(kX86WordSize)));
    }
  } else if (destination.IsQuickParameter()) {
    InvokeDexCallingConvention calling_convention;
    uint32_t argument_index = destination.GetQuickParameterIndex();
    if (source.IsRegister()) {
      __ movl(calling_convention.GetRegisterAt(argument_index), source.AsX86().AsRegisterPairLow());
      __ movl(Address(ESP, calling_convention.GetStackOffsetOf(argument_index + 1)),
              source.AsX86().AsRegisterPairHigh());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movl(calling_convention.GetRegisterAt(argument_index),
              Address(ESP, source.GetStackIndex()));
      __ movl(EAX, Address(ESP, source.GetHighStackIndex(kX86WordSize)));
      __ movl(Address(ESP, calling_convention.GetStackOffsetOf(argument_index + 1)), EAX);
    }
  } else {
    if (source.IsRegister()) {
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsX86().AsRegisterPairLow());
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)),
              source.AsX86().AsRegisterPairHigh());
    } else if (source.IsQuickParameter()) {
      InvokeDexCallingConvention calling_convention;
      uint32_t argument_index = source.GetQuickParameterIndex();
      __ movl(Address(ESP, destination.GetStackIndex()),
              calling_convention.GetRegisterAt(argument_index));
      __ movl(EAX,
              Address(ESP,
                      calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)), EAX);
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movl(EAX, Address(ESP, source.GetStackIndex()));
      __ movl(Address(ESP, destination.GetStackIndex()), EAX);
      __ movl(EAX, Address(ESP, source.GetHighStackIndex(kX86WordSize)));
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)), EAX);
    }
  }
}

void CodeGeneratorX86::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  if (instruction->AsIntConstant() != nullptr) {
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (location.IsRegister()) {
      __ movl(location.AsX86().AsCpuRegister(), imm);
    } else {
      __ movl(Address(ESP, location.GetStackIndex()), imm);
    }
  } else if (instruction->AsLongConstant() != nullptr) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ movl(location.AsX86().AsRegisterPairLow(), Immediate(Low32Bits(value)));
      __ movl(location.AsX86().AsRegisterPairHigh(), Immediate(High32Bits(value)));
    } else {
      __ movl(Address(ESP, location.GetStackIndex()), Immediate(Low32Bits(value)));
      __ movl(Address(ESP, location.GetHighStackIndex(kX86WordSize)), Immediate(High32Bits(value)));
    }
  } else if (instruction->AsLoadLocal() != nullptr) {
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        Move32(location, Location::StackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      case Primitive::kPrimLong:
        Move64(location, Location::DoubleStackSlot(
            GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      default:
        LOG(FATAL) << "Unimplemented local type " << instruction->GetType();
    }
  } else {
    // This can currently only happen when the instruction that requests the move
    // is the next to be compiled.
    DCHECK_EQ(instruction->GetNext(), move_for);
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        Move32(location, instruction->GetLocations()->Out());
        break;

      case Primitive::kPrimLong:
        Move64(location, instruction->GetLocations()->Out());
        break;

      default:
        LOG(FATAL) << "Unimplemented type " << instruction->GetType();
    }
  }
}

void LocationsBuilderX86::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (GetGraph()->GetExitBlock() == successor) {
    codegen_->GenerateFrameExit();
  } else if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void LocationsBuilderX86::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  locations->SetInAt(0, X86CpuLocation(EAX));
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitIf(HIf* if_instr) {
  // TODO: Generate the input as a condition, instead of materializing in a register.
  __ cmpl(if_instr->GetLocations()->InAt(0).AsX86().AsCpuRegister(), Immediate(0));
  __ j(kEqual, codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfTrueSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  }
}

void LocationsBuilderX86::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderX86::VisitLoadLocal(HLoadLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderX86::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  switch (store->InputAt(1)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unimplemented local type " << store->InputAt(1)->GetType();
  }
  store->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitStoreLocal(HStoreLocal* store) {
}

void LocationsBuilderX86::VisitEqual(HEqual* equal) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(equal);
  locations->SetInAt(0, X86CpuLocation(EAX));
  locations->SetInAt(1, X86CpuLocation(ECX));
  locations->SetOut(X86CpuLocation(EAX));
  equal->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitEqual(HEqual* equal) {
  __ cmpl(equal->GetLocations()->InAt(0).AsX86().AsCpuRegister(),
          equal->GetLocations()->InAt(1).AsX86().AsCpuRegister());
  __ setb(kEqual, equal->GetLocations()->Out().AsX86().AsCpuRegister());
}

void LocationsBuilderX86::VisitIntConstant(HIntConstant* constant) {
  constant->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitLongConstant(HLongConstant* constant) {
  constant->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  codegen_->GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(0, X86CpuLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(
          0, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      break;

    default:
      LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
  }
  ret->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsX86().AsCpuRegister(), EAX);
        break;

      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsX86().AsRegisterPair(), EAX_EDX);
        break;

      default:
        LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
  __ ret();
}

static constexpr Register kRuntimeParameterCoreRegisters[] = { EAX, ECX, EDX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<Register> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

void LocationsBuilderX86::VisitPushArgument(HPushArgument* argument) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(argument);
  InvokeDexCallingConvention calling_convention;
  uint32_t argument_index = argument->GetArgumentIndex();
  switch (argument->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (argument_index < calling_convention.GetNumberOfRegisters()) {
        locations->SetInAt(
            0, X86CpuLocation(calling_convention.GetRegisterAt(argument->GetArgumentIndex())));
      } else {
        locations->SetInAt(
            0, Location::StackSlot(calling_convention.GetStackOffsetOf(argument_index)));
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (argument_index + 1 < calling_convention.GetNumberOfRegisters()) {
        Location location = Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(argument_index)));
        locations->SetInAt(0, location);
      } else if (argument_index + 1 == calling_convention.GetNumberOfRegisters()) {
        locations->SetInAt(0, Location::QuickParameter(argument_index));
      } else {
        locations->SetInAt(
            0, Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(argument_index)));
      }
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented argument type " << argument->InputAt(0)->GetType();
  }

  argument->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitPushArgument(HPushArgument* argument) {
  // Nothing to do.
}

void LocationsBuilderX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(X86CpuLocation(EAX));
  switch (invoke->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetOut(X86CpuLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetOut(Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      break;

    case Primitive::kPrimVoid:
      break;

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      LOG(FATAL) << "Unimplemented return type " << invoke->GetType();
      break;
  }

  invoke->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).AsX86().AsCpuRegister();
  size_t index_in_cache = mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value() +
      invoke->GetIndexInDexCache() * kX86WordSize;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, index_in_cache));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value()));

  codegen_->RecordPcInfo(invoke->GetDexPc());
}

void LocationsBuilderX86::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, X86CpuLocation(EAX));
      locations->SetInAt(1, X86CpuLocation(ECX));
      locations->SetOut(X86CpuLocation(EAX));
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(
          0, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      locations->SetInAt(
          1, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(ECX_EBX)));
      locations->SetOut(Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      break;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented add type " << add->GetResultType();
  }
  add->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->Out().AsX86().AsCpuRegister());
      __ addl(locations->InAt(0).AsX86().AsCpuRegister(),
              locations->InAt(1).AsX86().AsCpuRegister());
      break;
    }

    case Primitive::kPrimLong: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsRegisterPair(),
                locations->Out().AsX86().AsRegisterPair());
      __ addl(locations->InAt(0).AsX86().AsRegisterPairLow(),
              locations->InAt(1).AsX86().AsRegisterPairLow());
      __ adcl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
              locations->InAt(1).AsX86().AsRegisterPairHigh());
      break;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented add type " << add->GetResultType();
  }
}

void LocationsBuilderX86::VisitSub(HSub* sub) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(sub);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, X86CpuLocation(EAX));
      locations->SetInAt(1, X86CpuLocation(ECX));
      locations->SetOut(X86CpuLocation(EAX));
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(
          0, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      locations->SetInAt(
          1, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(ECX_EBX)));
      locations->SetOut(Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
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
  sub->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->Out().AsX86().AsCpuRegister());
      __ subl(locations->InAt(0).AsX86().AsCpuRegister(),
              locations->InAt(1).AsX86().AsCpuRegister());
      break;
    }

    case Primitive::kPrimLong: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsRegisterPair(),
                locations->Out().AsX86().AsRegisterPair());
      __ subl(locations->InAt(0).AsX86().AsRegisterPairLow(),
              locations->InAt(1).AsX86().AsRegisterPairLow());
      __ sbbl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
              locations->InAt(1).AsX86().AsRegisterPairHigh());
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

void LocationsBuilderX86::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetOut(X86CpuLocation(EAX));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ movl(calling_convention.GetRegisterAt(0),
          Immediate(instruction->GetTypeIndex()));

  __ fs()->call(
      Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocObjectWithAccessCheck)));

  codegen_->RecordPcInfo(instruction->GetDexPc());
}

void LocationsBuilderX86::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  InvokeDexCallingConvention calling_convention;
  uint32_t argument_index = instruction->GetIndex();
  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      if (argument_index < calling_convention.GetNumberOfRegisters()) {
        locations->SetOut(X86CpuLocation(calling_convention.GetRegisterAt(argument_index)));
      } else {
        locations->SetOut(Location::StackSlot(
            calling_convention.GetStackOffsetOf(argument_index) + codegen_->GetFrameSize()));
      }
      break;

    case Primitive::kPrimLong:
      if (argument_index + 1 < calling_convention.GetNumberOfRegisters()) {
        locations->SetOut(Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(
            (calling_convention.GetRegisterPairAt(argument_index)))));
      } else if (argument_index + 1 == calling_convention.GetNumberOfRegisters()) {
        locations->SetOut(Location::QuickParameter(argument_index));
      } else {
        locations->SetOut(Location::DoubleStackSlot(
            calling_convention.GetStackOffsetOf(argument_index) + codegen_->GetFrameSize()));
      }
      break;

    default:
      LOG(FATAL) << "Unimplemented parameter type " << instruction->GetType();
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderX86::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, X86CpuLocation(EAX));
  locations->SetOut(X86CpuLocation(EAX));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(), locations->Out().AsX86().AsCpuRegister());
  __ xorl(locations->Out().AsX86().AsCpuRegister(), Immediate(1));
}

}  // namespace x86
}  // namespace art
