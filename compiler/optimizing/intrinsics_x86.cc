/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "intrinsics_x86.h"

#include "arch/x86/instruction_set_features_x86.h"
#include "code_generator_x86.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/constants_x86.h"

namespace art {

namespace x86 {

static constexpr int kDoubleNaNHigh = 0x7FF80000;
static constexpr int kDoubleNaNLow = 0x00000000;
static constexpr int kFloatNaN = 0x7FC00000;

IntrinsicLocationsBuilderX86::IntrinsicLocationsBuilderX86(CodeGeneratorX86* codegen)
  : arena_(codegen->GetGraph()->GetArena()), codegen_(codegen) {
}


X86Assembler* IntrinsicCodeGeneratorX86::GetAssembler() {
  return reinterpret_cast<X86Assembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorX86::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

bool IntrinsicLocationsBuilderX86::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  return res != nullptr && res->Intrinsified();
}

#define __ reinterpret_cast<X86Assembler*>(codegen->GetAssembler())->

// TODO: target as memory.
static void MoveFromReturnRegister(Location target,
                                   Primitive::Type type,
                                   CodeGeneratorX86* codegen) {
  if (!target.IsValid()) {
    DCHECK(type == Primitive::kPrimVoid);
    return;
  }

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register target_reg = target.AsRegister<Register>();
      if (target_reg != EAX) {
        __ movl(target_reg, EAX);
      }
      break;
    }
    case Primitive::kPrimLong: {
      Register target_reg_lo = target.AsRegisterPairLow<Register>();
      Register target_reg_hi = target.AsRegisterPairHigh<Register>();
      if (target_reg_lo != EAX) {
        __ movl(target_reg_lo, EAX);
      }
      if (target_reg_hi != EDX) {
        __ movl(target_reg_hi, EDX);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected void type for valid location " << target;
      UNREACHABLE();

    case Primitive::kPrimDouble: {
      XmmRegister target_reg = target.AsFpuRegister<XmmRegister>();
      if (target_reg != XMM0) {
        __ movsd(target_reg, XMM0);
      }
      break;
    }
    case Primitive::kPrimFloat: {
      XmmRegister target_reg = target.AsFpuRegister<XmmRegister>();
      if (target_reg != XMM0) {
        __ movss(target_reg, XMM0);
      }
      break;
    }
  }
}

static void MoveArguments(HInvoke* invoke, ArenaAllocator* arena, CodeGeneratorX86* codegen) {
  if (invoke->InputCount() == 0) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  InvokeDexCallingConventionVisitor calling_convention_visitor;

  // We're moving potentially two or more locations to locations that could overlap, so we need
  // a parallel move resolver.
  HParallelMove parallel_move(arena);

  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    Location cc_loc = calling_convention_visitor.GetNextLocation(input->GetType());
    Location actual_loc = locations->InAt(i);

    parallel_move.AddMove(actual_loc, cc_loc, nullptr);
  }

  codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
}

// Slow-path for fallback (calling the managed code to handle the intrinsic) in an intrinsified
// call. This will copy the arguments into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations given by the invoke's location
//       summary. If an intrinsic modifies those locations before a slowpath call, they must be
//       restored!
class IntrinsicSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit IntrinsicSlowPathX86(HInvoke* invoke, Register temp)
    : invoke_(invoke) {
      // The temporary register has to be EAX for x86 invokes.
      DCHECK_EQ(temp, EAX);
    }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorX86* codegen = down_cast<CodeGeneratorX86*>(codegen_in);
    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen->GetGraph()->GetArena(), codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(), EAX);
      RecordPcInfo(codegen, invoke_, invoke_->GetDexPc());
    } else {
      UNIMPLEMENTED(FATAL) << "Non-direct intrinsic slow-path not yet implemented";
      UNREACHABLE();
    }

    // Copy the result back to the expected output.
    Location out = invoke_->GetLocations()->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister());  // TODO: Replace this when we support output in memory.
      DCHECK(!invoke_->GetLocations()->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      MoveFromReturnRegister(out, invoke_->GetType(), codegen);
    }

    RestoreLiveRegisters(codegen, invoke_->GetLocations());
    __ jmp(GetExitLabel());
  }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathX86);
};

#undef __
#define __ assembler->

static void CreateFPToIntLocations(ArenaAllocator* arena, HInvoke* invoke, bool is64bit) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresRegister());
  if (is64bit) {
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke, bool is64bit) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
  if (is64bit) {
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

static void MoveFPToInt(LocationSummary* locations, bool is64bit, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    // Need to use the temporary.
    XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
    __ movsd(temp, input.AsFpuRegister<XmmRegister>());
    __ movd(output.AsRegisterPairLow<Register>(), temp);
    __ psrlq(temp, Immediate(32));
    __ movd(output.AsRegisterPairHigh<Register>(), temp);
  } else {
    __ movd(output.AsRegister<Register>(), input.AsFpuRegister<XmmRegister>());
  }
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    // Need to use the temporary.
    XmmRegister temp1 = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
    XmmRegister temp2 = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
    __ movd(temp1, input.AsRegisterPairLow<Register>());
    __ movd(temp2, input.AsRegisterPairHigh<Register>());
    __ punpckldq(temp1, temp2);
    __ movsd(output.AsFpuRegister<XmmRegister>(), temp1);
  } else {
    __ movd(output.AsFpuRegister<XmmRegister>(), input.AsRegister<Register>());
  }
}

void IntrinsicLocationsBuilderX86::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke, true);
}
void IntrinsicLocationsBuilderX86::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke, true);
}

void IntrinsicCodeGeneratorX86::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), true, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke, false);
}

void IntrinsicCodeGeneratorX86::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), false, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void CreateLongToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

static void CreateLongToLongLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

static void GenReverseBytes(LocationSummary* locations,
                            Primitive::Type size,
                            X86Assembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();

  switch (size) {
    case Primitive::kPrimShort:
      // TODO: Can be done with an xchg of 8b registers. This is straight from Quick.
      __ bswapl(out);
      __ sarl(out, Immediate(16));
      break;
    case Primitive::kPrimInt:
      __ bswapl(out);
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}


// TODO: Consider Quick's way of doing Double abs through integer operations, as the immediate we
//       need is 64b.

static void CreateFloatToFloat(ArenaAllocator* arena, HInvoke* invoke) {
  // TODO: Enable memory operations when the assembler supports them.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  // TODO: Allow x86 to work with memory. This requires assembler support, see below.
  // locations->SetInAt(0, Location::Any());               // X86 can work on memory directly.
  locations->SetOut(Location::SameAsFirstInput());
}

static void MathAbsFP(LocationSummary* locations, bool is64bit, X86Assembler* assembler) {
  Location output = locations->Out();

  if (output.IsFpuRegister()) {
    // Create the right constant on an aligned stack.
    if (is64bit) {
      __ subl(ESP, Immediate(8));
      __ pushl(Immediate(0x7FFFFFFF));
      __ pushl(Immediate(0xFFFFFFFF));
      __ andpd(output.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
    } else {
      __ subl(ESP, Immediate(12));
      __ pushl(Immediate(0x7FFFFFFF));
      __ andps(output.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
    }
    __ addl(ESP, Immediate(16));
  } else {
    // TODO: update when assember support is available.
    UNIMPLEMENTED(FATAL) << "Needs assembler support.";
//  Once assembler support is available, in-memory operations look like this:
//    if (is64bit) {
//      DCHECK(output.IsDoubleStackSlot());
//      __ andl(Address(Register(RSP), output.GetHighStackIndex(kX86WordSize)),
//              Immediate(0x7FFFFFFF));
//    } else {
//      DCHECK(output.IsStackSlot());
//      // Can use and with a literal directly.
//      __ andl(Address(Register(RSP), output.GetStackIndex()), Immediate(0x7FFFFFFF));
//    }
  }
}

void IntrinsicLocationsBuilderX86::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFloatToFloat(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFloatToFloat(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateAbsIntLocation(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RegisterLocation(EAX));
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RegisterLocation(EDX));
}

static void GenAbsInteger(LocationSummary* locations, X86Assembler* assembler) {
  Location output = locations->Out();
  Register out = output.AsRegister<Register>();
  DCHECK_EQ(out, EAX);
  Register temp = locations->GetTemp(0).AsRegister<Register>();
  DCHECK_EQ(temp, EDX);

  // Sign extend EAX into EDX.
  __ cdq();

  // XOR EAX with sign.
  __ xorl(EAX, EDX);

  // Subtract out sign to correct.
  __ subl(EAX, EDX);

  // The result is in EAX.
}

static void CreateAbsLongLocation(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
  locations->AddTemp(Location::RequiresRegister());
}

static void GenAbsLong(LocationSummary* locations, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Register input_lo = input.AsRegisterPairLow<Register>();
  Register input_hi = input.AsRegisterPairHigh<Register>();
  Location output = locations->Out();
  Register output_lo = output.AsRegisterPairLow<Register>();
  Register output_hi = output.AsRegisterPairHigh<Register>();
  Register temp = locations->GetTemp(0).AsRegister<Register>();

  // Compute the sign into the temporary.
  __ movl(temp, input_hi);
  __ sarl(temp, Immediate(31));

  // Store the sign into the output.
  __ movl(output_lo, temp);
  __ movl(output_hi, temp);

  // XOR the input to the output.
  __ xorl(output_lo, input_lo);
  __ xorl(output_hi, input_hi);

  // Subtract the sign.
  __ subl(output_lo, temp);
  __ sbbl(output_hi, temp);
}

void IntrinsicLocationsBuilderX86::VisitMathAbsInt(HInvoke* invoke) {
  CreateAbsIntLocation(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathAbsLong(HInvoke* invoke) {
  CreateAbsLongLocation(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsLong(invoke->GetLocations(), GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations, bool is_min, bool is_double,
                        X86Assembler* assembler) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);
  Location out_loc = locations->Out();
  XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    DCHECK(out_loc.Equals(op1_loc));
    return;
  }

  //  (out := op1)
  //  out <=? op2
  //  if Nan jmp Nan_label
  //  if out is min jmp done
  //  if op2 is min jmp op2_label
  //  handle -0/+0
  //  jmp done
  // Nan_label:
  //  out := NaN
  // op2_label:
  //  out := op2
  // done:
  //
  // This removes one jmp, but needs to copy one input (op1) to out.
  //
  // TODO: This is straight from Quick (except literal pool). Make NaN an out-of-line slowpath?

  XmmRegister op2 = op2_loc.AsFpuRegister<XmmRegister>();

  Label nan, done, op2_label;
  if (is_double) {
    __ ucomisd(out, op2);
  } else {
    __ ucomiss(out, op2);
  }

  __ j(Condition::kParityEven, &nan);

  __ j(is_min ? Condition::kAbove : Condition::kBelow, &op2_label);
  __ j(is_min ? Condition::kBelow : Condition::kAbove, &done);

  // Handle 0.0/-0.0.
  if (is_min) {
    if (is_double) {
      __ orpd(out, op2);
    } else {
      __ orps(out, op2);
    }
  } else {
    if (is_double) {
      __ andpd(out, op2);
    } else {
      __ andps(out, op2);
    }
  }
  __ jmp(&done);

  // NaN handling.
  __ Bind(&nan);
  if (is_double) {
    __ pushl(Immediate(kDoubleNaNHigh));
    __ pushl(Immediate(kDoubleNaNLow));
    __ movsd(out, Address(ESP, 0));
    __ addl(ESP, Immediate(8));
  } else {
    __ pushl(Immediate(kFloatNaN));
    __ movss(out, Address(ESP, 0));
    __ addl(ESP, Immediate(4));
  }
  __ jmp(&done);

  // out := op2;
  __ Bind(&op2_label);
  if (is_double) {
    __ movsd(out, op2);
  } else {
    __ movss(out, op2);
  }

  // Done.
  __ Bind(&done);
}

static void CreateFPFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  // The following is sub-optimal, but all we can do for now. It would be fine to also accept
  // the second input to be the output (we can simply swap inputs).
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicLocationsBuilderX86::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, false, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, false, GetAssembler());
}

static void GenMinMax(LocationSummary* locations, bool is_min, bool is_long,
                      X86Assembler* assembler) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    // Can return immediately, as op1_loc == out_loc.
    // Note: if we ever support separate registers, e.g., output into memory, we need to check for
    //       a copy here.
    DCHECK(locations->Out().Equals(op1_loc));
    return;
  }

  if (is_long) {
    // Need to perform a subtract to get the sign right.
    // op1 is already in the same location as the output.
    Location output = locations->Out();
    Register output_lo = output.AsRegisterPairLow<Register>();
    Register output_hi = output.AsRegisterPairHigh<Register>();

    Register op2_lo = op2_loc.AsRegisterPairLow<Register>();
    Register op2_hi = op2_loc.AsRegisterPairHigh<Register>();

    // Spare register to compute the subtraction to set condition code.
    Register temp = locations->GetTemp(0).AsRegister<Register>();

    // Subtract off op2_low.
    __ movl(temp, output_lo);
    __ subl(temp, op2_lo);

    // Now use the same tempo and the borrow to finish the subtraction of op2_hi.
    __ movl(temp, output_hi);
    __ sbbl(temp, op2_hi);

    // Now the condition code is correct.
    Condition cond = is_min ? Condition::kGreaterEqual : Condition::kLess;
    __ cmovl(cond, output_lo, op2_lo);
    __ cmovl(cond, output_hi, op2_hi);
  } else {
    Register out = locations->Out().AsRegister<Register>();
    Register op2 = op2_loc.AsRegister<Register>();

    //  (out := op1)
    //  out <=? op2
    //  if out is min jmp done
    //  out := op2
    // done:

    __ cmpl(out, op2);
    Condition cond = is_min ? Condition::kGreater : Condition::kLess;
    __ cmovl(cond, out, op2);
  }
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void CreateLongLongToLongLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  // Register to use to perform a long subtract to set cc.
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicLocationsBuilderX86::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, false, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMinLongLong(HInvoke* invoke) {
  CreateLongLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, false, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateLongLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, true, GetAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

void IntrinsicLocationsBuilderX86::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();

  GetAssembler()->sqrtsd(out, in);
}

static void InvokeOutOfLineIntrinsic(CodeGeneratorX86* codegen, HInvoke* invoke) {
  MoveArguments(invoke, codegen->GetGraph()->GetArena(), codegen);

  DCHECK(invoke->IsInvokeStaticOrDirect());
  codegen->GenerateStaticOrDirectCall(invoke->AsInvokeStaticOrDirect(), EAX);
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());

  // Copy the result back to the expected output.
  Location out = invoke->GetLocations()->Out();
  if (out.IsValid()) {
    DCHECK(out.IsRegister());
    MoveFromReturnRegister(out, invoke->GetType(), codegen);
  }
}

static void CreateSSE41FPToFPLocations(ArenaAllocator* arena,
                                      HInvoke* invoke,
                                      CodeGeneratorX86* codegen) {
  // Do we have instruction support?
  if (codegen->GetInstructionSetFeatures().HasSSE4_1()) {
    CreateFPToFPLocations(arena, invoke);
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

static void GenSSE41FPToFPIntrinsic(CodeGeneratorX86* codegen,
                                   HInvoke* invoke,
                                   X86Assembler* assembler,
                                   int round_mode) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen, invoke);
  } else {
    XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
    XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
    __ roundsd(out, in, Immediate(round_mode));
  }
}

void IntrinsicLocationsBuilderX86::VisitMathCeil(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathCeil(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 2);
}

void IntrinsicLocationsBuilderX86::VisitMathFloor(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathFloor(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 1);
}

void IntrinsicLocationsBuilderX86::VisitMathRint(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathRint(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 0);
}

// Note that 32 bit x86 doesn't have the capability to inline MathRoundDouble,
// as it needs 64 bit instructions.
void IntrinsicLocationsBuilderX86::VisitMathRoundFloat(HInvoke* invoke) {
  // Do we have instruction support?
  if (codegen_->GetInstructionSetFeatures().HasSSE4_1()) {
    LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                              LocationSummary::kNoCall,
                                                              kIntrinsified);
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetOut(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(EAX));
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitMathRoundFloat(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundFloat as t1 = floor(input + 0.5f);  convert to int.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  Register out = locations->Out().AsRegister<Register>();
  XmmRegister maxInt = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
  Label done, nan;
  X86Assembler* assembler = GetAssembler();

  // Generate 0.5 into inPlusPointFive.
  __ movl(out, Immediate(bit_cast<int32_t, float>(0.5f)));
  __ movd(inPlusPointFive, out);

  // Add in the input.
  __ addss(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundss(inPlusPointFive, inPlusPointFive, Immediate(1));

  __ movl(out, Immediate(kPrimIntMax));
  // maxInt = int-to-float(out)
  __ cvtsi2ss(maxInt, out);

  // if inPlusPointFive >= maxInt goto done
  __ comiss(inPlusPointFive, maxInt);
  __ j(kAboveEqual, &done);

  // if input == NaN goto nan
  __ j(kUnordered, &nan);

  // output = float-to-int-truncate(input)
  __ cvttss2si(out, inPlusPointFive);
  __ jmp(&done);
  __ Bind(&nan);

  //  output = 0
  __ xorl(out, out);
  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86::VisitStringCharAt(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array
  const int32_t offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_
  const int32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  Register obj = locations->InAt(0).AsRegister<Register>();
  Register idx = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  Location temp_loc = locations->GetTemp(0);
  Register temp = temp_loc.AsRegister<Register>();

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCodeX86* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke, temp);
  codegen_->AddSlowPath(slow_path);

  X86Assembler* assembler = GetAssembler();

  __ cmpl(idx, Address(obj, count_offset));
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ j(kAboveEqual, slow_path->GetEntryLabel());

  // Get the actual element.
  __ movl(temp, idx);                          // temp := idx.
  __ addl(temp, Address(obj, offset_offset));  // temp := offset + idx.
  __ movl(out, Address(obj, value_offset));    // obj := obj.array.
  // out = out[2*temp].
  __ movzxw(out, Address(out, temp, ScaleFactor::TIMES_2, data_offset));

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitStringCompareTo(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(EAX));
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringCompareTo(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheck());

  Register argument = locations->InAt(1).AsRegister<Register>();
  __ testl(argument, argument);
  SlowPathCodeX86* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(
      invoke, locations->GetTemp(0).AsRegister<Register>());
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pStringCompareTo)));
  __ Bind(slow_path->GetExitLabel());
}

static void GenPeek(LocationSummary* locations, Primitive::Type size, X86Assembler* assembler) {
  Register address = locations->InAt(0).AsRegisterPairLow<Register>();
  Location out_loc = locations->Out();
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      __ movsxb(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimShort:
      __ movsxw(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimInt:
      __ movl(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimLong:
      __ movl(out_loc.AsRegisterPairLow<Register>(), Address(address, 0));
      __ movl(out_loc.AsRegisterPairHigh<Register>(), Address(address, 4));
      break;
    default:
      LOG(FATAL) << "Type not recognized for peek: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekByte(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekIntNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekLongNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekShortNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

static void CreateLongIntToVoidLocations(ArenaAllocator* arena, Primitive::Type size,
                                         HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  HInstruction *value = invoke->InputAt(1);
  if (size == Primitive::kPrimByte) {
    locations->SetInAt(1, Location::ByteRegisterOrConstant(EDX, value));
  } else {
    locations->SetInAt(1, Location::RegisterOrConstant(value));
  }
}

static void GenPoke(LocationSummary* locations, Primitive::Type size, X86Assembler* assembler) {
  Register address = locations->InAt(0).AsRegisterPairLow<Register>();
  Location value_loc = locations->InAt(1);
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      if (value_loc.IsConstant()) {
        __ movb(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movb(Address(address, 0), value_loc.AsRegister<ByteRegister>());
      }
      break;
    case Primitive::kPrimShort:
      if (value_loc.IsConstant()) {
        __ movw(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movw(Address(address, 0), value_loc.AsRegister<Register>());
      }
      break;
    case Primitive::kPrimInt:
      if (value_loc.IsConstant()) {
        __ movl(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movl(Address(address, 0), value_loc.AsRegister<Register>());
      }
      break;
    case Primitive::kPrimLong:
      if (value_loc.IsConstant()) {
        int64_t value = value_loc.GetConstant()->AsLongConstant()->GetValue();
        __ movl(Address(address, 0), Immediate(Low32Bits(value)));
        __ movl(Address(address, 4), Immediate(High32Bits(value)));
      } else {
        __ movl(Address(address, 0), value_loc.AsRegisterPairLow<Register>());
        __ movl(Address(address, 4), value_loc.AsRegisterPairHigh<Register>());
      }
      break;
    default:
      LOG(FATAL) << "Type not recognized for poke: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimByte, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeByte(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimInt, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeIntNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimLong, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeLongNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimShort, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeShortNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86::VisitThreadCurrentThread(HInvoke* invoke) {
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();
  GetAssembler()->fs()->movl(out, Address::Absolute(Thread::PeerOffset<kX86WordSize>()));
}

static void GenUnsafeGet(LocationSummary* locations, Primitive::Type type,
                         bool is_volatile, X86Assembler* assembler) {
  Register base = locations->InAt(1).AsRegister<Register>();
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();
  Location output = locations->Out();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      __ movl(output.AsRegister<Register>(), Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;

    case Primitive::kPrimLong: {
        Register output_lo = output.AsRegisterPairLow<Register>();
        Register output_hi = output.AsRegisterPairHigh<Register>();
        if (is_volatile) {
          // Need to use a XMM to read atomically.
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          __ movsd(temp, Address(base, offset, ScaleFactor::TIMES_1, 0));
          __ movd(output_lo, temp);
          __ psrlq(temp, Immediate(32));
          __ movd(output_hi, temp);
        } else {
          __ movl(output_lo, Address(base, offset, ScaleFactor::TIMES_1, 0));
          __ movl(output_hi, Address(base, offset, ScaleFactor::TIMES_1, 4));
        }
      }
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke,
                                          bool is_long, bool is_volatile) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  if (is_long) {
    if (is_volatile) {
      // Need to use XMM to read volatile.
      locations->AddTemp(Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
    } else {
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
    }
  } else {
    locations->SetOut(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, false, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, false, true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, false, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, true, true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, false, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, false, true);
}


void IntrinsicCodeGeneratorX86::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimInt, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimInt, true, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimLong, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimLong, true, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimNot, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimNot, true, GetAssembler());
}


static void CreateIntIntIntIntToVoidPlusTempsLocations(ArenaAllocator* arena,
                                                       Primitive::Type type,
                                                       HInvoke* invoke,
                                                       bool is_volatile) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());
    // Ensure the value is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  } else if (type == Primitive::kPrimLong && is_volatile) {
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

void IntrinsicLocationsBuilderX86::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke, true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke, true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke, false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke, true);
}

// We don't care for ordered: it requires an AnyStore barrier, which is already given by the x86
// memory model.
static void GenUnsafePut(LocationSummary* locations,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorX86* codegen) {
  X86Assembler* assembler = reinterpret_cast<X86Assembler*>(codegen->GetAssembler());
  Register base = locations->InAt(1).AsRegister<Register>();
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();
  Location value_loc = locations->InAt(3);

  if (type == Primitive::kPrimLong) {
    Register value_lo = value_loc.AsRegisterPairLow<Register>();
    Register value_hi = value_loc.AsRegisterPairHigh<Register>();
    if (is_volatile) {
      XmmRegister temp1 = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      XmmRegister temp2 = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
      __ movd(temp1, value_lo);
      __ movd(temp2, value_hi);
      __ punpckldq(temp1, temp2);
      __ movsd(Address(base, offset, ScaleFactor::TIMES_1, 0), temp1);
    } else {
      __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value_lo);
      __ movl(Address(base, offset, ScaleFactor::TIMES_1, 4), value_hi);
    }
  } else {
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value_loc.AsRegister<Register>());
  }

  if (is_volatile) {
    __ mfence();
  }

  if (type == Primitive::kPrimNot) {
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<Register>(),
                        locations->GetTemp(1).AsRegister<Register>(),
                        base,
                        value_loc.AsRegister<Register>());
  }
}

void IntrinsicCodeGeneratorX86::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, true, codegen_);
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                   \
void IntrinsicLocationsBuilderX86::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                       \
void IntrinsicCodeGeneratorX86::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(IntegerReverse)
UNIMPLEMENTED_INTRINSIC(LongReverse)
UNIMPLEMENTED_INTRINSIC(LongReverseBytes)
UNIMPLEMENTED_INTRINSIC(MathRoundDouble)
UNIMPLEMENTED_INTRINSIC(StringIndexOf)
UNIMPLEMENTED_INTRINSIC(StringIndexOfAfter)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(UnsafeCASInt)
UNIMPLEMENTED_INTRINSIC(UnsafeCASLong)
UNIMPLEMENTED_INTRINSIC(UnsafeCASObject)
UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)

}  // namespace x86
}  // namespace art
