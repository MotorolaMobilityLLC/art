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

#include "code_generator_mips.h"

#include "arch/mips/entrypoints_direct_mips.h"
#include "arch/mips/instruction_set_features_mips.h"
#include "art_method.h"
#include "code_generator_utils.h"
#include "compiled_method.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_mips.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "offsets.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/mips/assembler_mips.h"
#include "utils/stack_checks.h"

namespace art {
namespace mips {

static constexpr int kCurrentMethodStackOffset = 0;
static constexpr Register kMethodRegisterArgument = A0;

// We'll maximize the range of a single load instruction for dex cache array accesses
// by aligning offset -32768 with the offset of the first used element.
static constexpr uint32_t kDexCacheArrayLwOffset = 0x8000;

Location MipsReturnLocation(Primitive::Type return_type) {
  switch (return_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      return Location::RegisterLocation(V0);

    case Primitive::kPrimLong:
      return Location::RegisterPairLocation(V0, V1);

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      return Location::FpuRegisterLocation(F0);

    case Primitive::kPrimVoid:
      return Location();
  }
  UNREACHABLE();
}

Location InvokeDexCallingConventionVisitorMIPS::GetReturnLocation(Primitive::Type type) const {
  return MipsReturnLocation(type);
}

Location InvokeDexCallingConventionVisitorMIPS::GetMethodLocation() const {
  return Location::RegisterLocation(kMethodRegisterArgument);
}

Location InvokeDexCallingConventionVisitorMIPS::GetNextLocation(Primitive::Type type) {
  Location next_location;

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t gp_index = gp_index_++;
      if (gp_index < calling_convention.GetNumberOfRegisters()) {
        next_location = Location::RegisterLocation(calling_convention.GetRegisterAt(gp_index));
      } else {
        size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
        next_location = Location::StackSlot(stack_offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t gp_index = gp_index_;
      gp_index_ += 2;
      if (gp_index + 1 < calling_convention.GetNumberOfRegisters()) {
        if (calling_convention.GetRegisterAt(gp_index) == A1) {
          gp_index_++;  // Skip A1, and use A2_A3 instead.
          gp_index++;
        }
        Register low_even = calling_convention.GetRegisterAt(gp_index);
        Register high_odd = calling_convention.GetRegisterAt(gp_index + 1);
        DCHECK_EQ(low_even + 1, high_odd);
        next_location = Location::RegisterPairLocation(low_even, high_odd);
      } else {
        size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
        next_location = Location::DoubleStackSlot(stack_offset);
      }
      break;
    }

    // Note: both float and double types are stored in even FPU registers. On 32 bit FPU, double
    // will take up the even/odd pair, while floats are stored in even regs only.
    // On 64 bit FPU, both double and float are stored in even registers only.
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      uint32_t float_index = float_index_++;
      if (float_index < calling_convention.GetNumberOfFpuRegisters()) {
        next_location = Location::FpuRegisterLocation(
            calling_convention.GetFpuRegisterAt(float_index));
      } else {
        size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
        next_location = Primitive::Is64BitType(type) ? Location::DoubleStackSlot(stack_offset)
                                                     : Location::StackSlot(stack_offset);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }

  // Space on the stack is reserved for all arguments.
  stack_index_ += Primitive::Is64BitType(type) ? 2 : 1;

  return next_location;
}

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type type) {
  return MipsReturnLocation(type);
}

// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<CodeGeneratorMIPS*>(codegen)->GetAssembler()->  // NOLINT
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMipsPointerSize, x).Int32Value()

class BoundsCheckSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit BoundsCheckSlowPathMIPS(HBoundsCheck* instruction) : SlowPathCodeMIPS(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(0),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimInt,
                               locations->InAt(1),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimInt);
    QuickEntrypointEnum entrypoint = instruction_->AsBoundsCheck()->IsStringCharAt()
        ? kQuickThrowStringBounds
        : kQuickThrowArrayBounds;
    mips_codegen->InvokeRuntime(entrypoint, instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowStringBounds, void, int32_t, int32_t>();
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "BoundsCheckSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathMIPS);
};

class DivZeroCheckSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit DivZeroCheckSlowPathMIPS(HDivZeroCheck* instruction) : SlowPathCodeMIPS(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);
    __ Bind(GetEntryLabel());
    mips_codegen->InvokeRuntime(kQuickThrowDivZero, instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "DivZeroCheckSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathMIPS);
};

class LoadClassSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  LoadClassSlowPathMIPS(HLoadClass* cls,
                        HInstruction* at,
                        uint32_t dex_pc,
                        bool do_clinit)
      : SlowPathCodeMIPS(at), cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadConst32(calling_convention.GetRegisterAt(0), cls_->GetTypeIndex());

    QuickEntrypointEnum entrypoint = do_clinit_ ? kQuickInitializeStaticStorage
                                                : kQuickInitializeType;
    mips_codegen->InvokeRuntime(entrypoint, at_, dex_pc_, this);
    if (do_clinit_) {
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, uint32_t>();
    } else {
      CheckEntrypointTypes<kQuickInitializeType, void*, uint32_t>();
    }

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      Primitive::Type type = at_->GetType();
      mips_codegen->MoveLocation(out, calling_convention.GetReturnLocation(type), type);
    }

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadClassSlowPathMIPS"; }

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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathMIPS);
};

class LoadStringSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit LoadStringSlowPathMIPS(HLoadString* instruction) : SlowPathCodeMIPS(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const uint32_t string_index = instruction_->AsLoadString()->GetStringIndex();
    __ LoadConst32(calling_convention.GetRegisterAt(0), string_index);
    mips_codegen->InvokeRuntime(kQuickResolveString, instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    Primitive::Type type = instruction_->GetType();
    mips_codegen->MoveLocation(locations->Out(),
                               calling_convention.GetReturnLocation(type),
                               type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadStringSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathMIPS);
};

class NullCheckSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit NullCheckSlowPathMIPS(HNullCheck* instr) : SlowPathCodeMIPS(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    mips_codegen->InvokeRuntime(kQuickThrowNullPointer,
                                instruction_,
                                instruction_->GetDexPc(),
                                this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "NullCheckSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathMIPS);
};

class SuspendCheckSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  SuspendCheckSlowPathMIPS(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCodeMIPS(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);
    __ Bind(GetEntryLabel());
    mips_codegen->InvokeRuntime(kQuickTestSuspend, instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    if (successor_ == nullptr) {
      __ B(GetReturnLabel());
    } else {
      __ B(mips_codegen->GetLabelOf(successor_));
    }
  }

  MipsLabel* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  const char* GetDescription() const OVERRIDE { return "SuspendCheckSlowPathMIPS"; }

 private:
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  MipsLabel return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathMIPS);
};

class TypeCheckSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit TypeCheckSlowPathMIPS(HInstruction* instruction) : SlowPathCodeMIPS(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Location object_class = instruction_->IsCheckCast() ? locations->GetTemp(0) : locations->Out();
    uint32_t dex_pc = instruction_->GetDexPc();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(1),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimNot,
                               object_class,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      mips_codegen->InvokeRuntime(kQuickInstanceofNonTrivial, instruction_, dex_pc, this);
      CheckEntrypointTypes<
          kQuickInstanceofNonTrivial, size_t, const mirror::Class*, const mirror::Class*>();
      Primitive::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      mips_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
    } else {
      DCHECK(instruction_->IsCheckCast());
      mips_codegen->InvokeRuntime(kQuickCheckCast, instruction_, dex_pc, this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "TypeCheckSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathMIPS);
};

class DeoptimizationSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit DeoptimizationSlowPathMIPS(HDeoptimize* instruction)
    : SlowPathCodeMIPS(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS* mips_codegen = down_cast<CodeGeneratorMIPS*>(codegen);
    __ Bind(GetEntryLabel());
    mips_codegen->InvokeRuntime(kQuickDeoptimize, instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathMIPS"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathMIPS);
};

CodeGeneratorMIPS::CodeGeneratorMIPS(HGraph* graph,
                                     const MipsInstructionSetFeatures& isa_features,
                                     const CompilerOptions& compiler_options,
                                     OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfCoreRegisters,
                    kNumberOfFRegisters,
                    kNumberOfRegisterPairs,
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves)),
                    ComputeRegisterMask(reinterpret_cast<const int*>(kFpuCalleeSaves),
                                        arraysize(kFpuCalleeSaves)),
                    compiler_options,
                    stats),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(graph->GetArena(), &isa_features),
      isa_features_(isa_features),
      uint32_literals_(std::less<uint32_t>(),
                       graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      method_patches_(MethodReferenceComparator(),
                      graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      call_patches_(MethodReferenceComparator(),
                    graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_dex_cache_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_string_patches_(StringReferenceValueComparator(),
                                 graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_string_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_type_patches_(TypeReferenceValueComparator(),
                               graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_type_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_address_patches_(std::less<uint32_t>(),
                                  graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      clobbered_ra_(false) {
  // Save RA (containing the return address) to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(RA));
}

#undef __
// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<MipsAssembler*>(GetAssembler())->  // NOLINT
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMipsPointerSize, x).Int32Value()

void CodeGeneratorMIPS::Finalize(CodeAllocator* allocator) {
  // Ensure that we fix up branches.
  __ FinalizeCode();

  // Adjust native pc offsets in stack maps.
  for (size_t i = 0, num = stack_map_stream_.GetNumberOfStackMaps(); i != num; ++i) {
    uint32_t old_position = stack_map_stream_.GetStackMap(i).native_pc_offset;
    uint32_t new_position = __ GetAdjustedPosition(old_position);
    DCHECK_GE(new_position, old_position);
    stack_map_stream_.SetStackMapNativePcOffset(i, new_position);
  }

  // Adjust pc offsets for the disassembly information.
  if (disasm_info_ != nullptr) {
    GeneratedCodeInterval* frame_entry_interval = disasm_info_->GetFrameEntryInterval();
    frame_entry_interval->start = __ GetAdjustedPosition(frame_entry_interval->start);
    frame_entry_interval->end = __ GetAdjustedPosition(frame_entry_interval->end);
    for (auto& it : *disasm_info_->GetInstructionIntervals()) {
      it.second.start = __ GetAdjustedPosition(it.second.start);
      it.second.end = __ GetAdjustedPosition(it.second.end);
    }
    for (auto& it : *disasm_info_->GetSlowPathIntervals()) {
      it.code_interval.start = __ GetAdjustedPosition(it.code_interval.start);
      it.code_interval.end = __ GetAdjustedPosition(it.code_interval.end);
    }
  }

  CodeGenerator::Finalize(allocator);
}

MipsAssembler* ParallelMoveResolverMIPS::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverMIPS::EmitMove(size_t index) {
  DCHECK_LT(index, moves_.size());
  MoveOperands* move = moves_[index];
  codegen_->MoveLocation(move->GetDestination(), move->GetSource(), move->GetType());
}

void ParallelMoveResolverMIPS::EmitSwap(size_t index) {
  DCHECK_LT(index, moves_.size());
  MoveOperands* move = moves_[index];
  Primitive::Type type = move->GetType();
  Location loc1 = move->GetDestination();
  Location loc2 = move->GetSource();

  DCHECK(!loc1.IsConstant());
  DCHECK(!loc2.IsConstant());

  if (loc1.Equals(loc2)) {
    return;
  }

  if (loc1.IsRegister() && loc2.IsRegister()) {
    // Swap 2 GPRs.
    Register r1 = loc1.AsRegister<Register>();
    Register r2 = loc2.AsRegister<Register>();
    __ Move(TMP, r2);
    __ Move(r2, r1);
    __ Move(r1, TMP);
  } else if (loc1.IsFpuRegister() && loc2.IsFpuRegister()) {
    FRegister f1 = loc1.AsFpuRegister<FRegister>();
    FRegister f2 = loc2.AsFpuRegister<FRegister>();
    if (type == Primitive::kPrimFloat) {
      __ MovS(FTMP, f2);
      __ MovS(f2, f1);
      __ MovS(f1, FTMP);
    } else {
      DCHECK_EQ(type, Primitive::kPrimDouble);
      __ MovD(FTMP, f2);
      __ MovD(f2, f1);
      __ MovD(f1, FTMP);
    }
  } else if ((loc1.IsRegister() && loc2.IsFpuRegister()) ||
             (loc1.IsFpuRegister() && loc2.IsRegister())) {
    // Swap FPR and GPR.
    DCHECK_EQ(type, Primitive::kPrimFloat);  // Can only swap a float.
    FRegister f1 = loc1.IsFpuRegister() ? loc1.AsFpuRegister<FRegister>()
                                        : loc2.AsFpuRegister<FRegister>();
    Register r2 = loc1.IsRegister() ? loc1.AsRegister<Register>()
                                    : loc2.AsRegister<Register>();
    __ Move(TMP, r2);
    __ Mfc1(r2, f1);
    __ Mtc1(TMP, f1);
  } else if (loc1.IsRegisterPair() && loc2.IsRegisterPair()) {
    // Swap 2 GPR register pairs.
    Register r1 = loc1.AsRegisterPairLow<Register>();
    Register r2 = loc2.AsRegisterPairLow<Register>();
    __ Move(TMP, r2);
    __ Move(r2, r1);
    __ Move(r1, TMP);
    r1 = loc1.AsRegisterPairHigh<Register>();
    r2 = loc2.AsRegisterPairHigh<Register>();
    __ Move(TMP, r2);
    __ Move(r2, r1);
    __ Move(r1, TMP);
  } else if ((loc1.IsRegisterPair() && loc2.IsFpuRegister()) ||
             (loc1.IsFpuRegister() && loc2.IsRegisterPair())) {
    // Swap FPR and GPR register pair.
    DCHECK_EQ(type, Primitive::kPrimDouble);
    FRegister f1 = loc1.IsFpuRegister() ? loc1.AsFpuRegister<FRegister>()
                                        : loc2.AsFpuRegister<FRegister>();
    Register r2_l = loc1.IsRegisterPair() ? loc1.AsRegisterPairLow<Register>()
                                          : loc2.AsRegisterPairLow<Register>();
    Register r2_h = loc1.IsRegisterPair() ? loc1.AsRegisterPairHigh<Register>()
                                          : loc2.AsRegisterPairHigh<Register>();
    // Use 2 temporary registers because we can't first swap the low 32 bits of an FPR and
    // then swap the high 32 bits of the same FPR. mtc1 makes the high 32 bits of an FPR
    // unpredictable and the following mfch1 will fail.
    __ Mfc1(TMP, f1);
    __ MoveFromFpuHigh(AT, f1);
    __ Mtc1(r2_l, f1);
    __ MoveToFpuHigh(r2_h, f1);
    __ Move(r2_l, TMP);
    __ Move(r2_h, AT);
  } else if (loc1.IsStackSlot() && loc2.IsStackSlot()) {
    Exchange(loc1.GetStackIndex(), loc2.GetStackIndex(), /* double_slot */ false);
  } else if (loc1.IsDoubleStackSlot() && loc2.IsDoubleStackSlot()) {
    Exchange(loc1.GetStackIndex(), loc2.GetStackIndex(), /* double_slot */ true);
  } else if ((loc1.IsRegister() && loc2.IsStackSlot()) ||
             (loc1.IsStackSlot() && loc2.IsRegister())) {
    Register reg = loc1.IsRegister() ? loc1.AsRegister<Register>()
                                     : loc2.AsRegister<Register>();
    intptr_t offset = loc1.IsStackSlot() ? loc1.GetStackIndex()
                                         : loc2.GetStackIndex();
    __ Move(TMP, reg);
    __ LoadFromOffset(kLoadWord, reg, SP, offset);
    __ StoreToOffset(kStoreWord, TMP, SP, offset);
  } else if ((loc1.IsRegisterPair() && loc2.IsDoubleStackSlot()) ||
             (loc1.IsDoubleStackSlot() && loc2.IsRegisterPair())) {
    Register reg_l = loc1.IsRegisterPair() ? loc1.AsRegisterPairLow<Register>()
                                           : loc2.AsRegisterPairLow<Register>();
    Register reg_h = loc1.IsRegisterPair() ? loc1.AsRegisterPairHigh<Register>()
                                           : loc2.AsRegisterPairHigh<Register>();
    intptr_t offset_l = loc1.IsDoubleStackSlot() ? loc1.GetStackIndex()
                                                 : loc2.GetStackIndex();
    intptr_t offset_h = loc1.IsDoubleStackSlot() ? loc1.GetHighStackIndex(kMipsWordSize)
                                                 : loc2.GetHighStackIndex(kMipsWordSize);
    __ Move(TMP, reg_l);
    __ LoadFromOffset(kLoadWord, reg_l, SP, offset_l);
    __ StoreToOffset(kStoreWord, TMP, SP, offset_l);
    __ Move(TMP, reg_h);
    __ LoadFromOffset(kLoadWord, reg_h, SP, offset_h);
    __ StoreToOffset(kStoreWord, TMP, SP, offset_h);
  } else {
    LOG(FATAL) << "Swap between " << loc1 << " and " << loc2 << " is unsupported";
  }
}

void ParallelMoveResolverMIPS::RestoreScratch(int reg) {
  __ Pop(static_cast<Register>(reg));
}

void ParallelMoveResolverMIPS::SpillScratch(int reg) {
  __ Push(static_cast<Register>(reg));
}

void ParallelMoveResolverMIPS::Exchange(int index1, int index2, bool double_slot) {
  // Allocate a scratch register other than TMP, if available.
  // Else, spill V0 (arbitrary choice) and use it as a scratch register (it will be
  // automatically unspilled when the scratch scope object is destroyed).
  ScratchRegisterScope ensure_scratch(this, TMP, V0, codegen_->GetNumberOfCoreRegisters());
  // If V0 spills onto the stack, SP-relative offsets need to be adjusted.
  int stack_offset = ensure_scratch.IsSpilled() ? kMipsWordSize : 0;
  for (int i = 0; i <= (double_slot ? 1 : 0); i++, stack_offset += kMipsWordSize) {
    __ LoadFromOffset(kLoadWord,
                      Register(ensure_scratch.GetRegister()),
                      SP,
                      index1 + stack_offset);
    __ LoadFromOffset(kLoadWord,
                      TMP,
                      SP,
                      index2 + stack_offset);
    __ StoreToOffset(kStoreWord,
                     Register(ensure_scratch.GetRegister()),
                     SP,
                     index2 + stack_offset);
    __ StoreToOffset(kStoreWord, TMP, SP, index1 + stack_offset);
  }
}

void CodeGeneratorMIPS::ComputeSpillMask() {
  core_spill_mask_ = allocated_registers_.GetCoreRegisters() & core_callee_save_mask_;
  fpu_spill_mask_ = allocated_registers_.GetFloatingPointRegisters() & fpu_callee_save_mask_;
  DCHECK_NE(core_spill_mask_, 0u) << "At least the return address register must be saved";
  // If there're FPU callee-saved registers and there's an odd number of GPR callee-saved
  // registers, include the ZERO register to force alignment of FPU callee-saved registers
  // within the stack frame.
  if ((fpu_spill_mask_ != 0) && (POPCOUNT(core_spill_mask_) % 2 != 0)) {
    core_spill_mask_ |= (1 << ZERO);
  }
}

bool CodeGeneratorMIPS::HasAllocatedCalleeSaveRegisters() const {
  // If RA is clobbered by PC-relative operations on R2 and it's the only spilled register
  // (this can happen in leaf methods), force CodeGenerator::InitializeCodeGeneration()
  // into the path that creates a stack frame so that RA can be explicitly saved and restored.
  // RA can't otherwise be saved/restored when it's the only spilled register.
  // TODO: Can this be improved? It causes creation of a stack frame (while RA might be
  // saved in an unused temporary register) and saving of RA and the current method pointer
  // in the frame.
  return CodeGenerator::HasAllocatedCalleeSaveRegisters() || clobbered_ra_;
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::MipsCore(static_cast<int>(reg));
}

// TODO: mapping of floating-point registers to DWARF.

void CodeGeneratorMIPS::GenerateFrameEntry() {
  __ Bind(&frame_entry_label_);

  bool do_overflow_check = FrameNeedsStackCheck(GetFrameSize(), kMips) || !IsLeafMethod();

  if (do_overflow_check) {
    __ LoadFromOffset(kLoadWord,
                      ZERO,
                      SP,
                      -static_cast<int32_t>(GetStackOverflowReservedBytes(kMips)));
    RecordPcInfo(nullptr, 0);
  }

  if (HasEmptyFrame()) {
    CHECK_EQ(fpu_spill_mask_, 0u);
    CHECK_EQ(core_spill_mask_, 1u << RA);
    CHECK(!clobbered_ra_);
    return;
  }

  // Make sure the frame size isn't unreasonably large.
  if (GetFrameSize() > GetStackOverflowReservedBytes(kMips)) {
    LOG(FATAL) << "Stack frame larger than " << GetStackOverflowReservedBytes(kMips) << " bytes";
  }

  // Spill callee-saved registers.

  uint32_t ofs = GetFrameSize();
  __ IncreaseFrameSize(ofs);

  for (uint32_t mask = core_spill_mask_; mask != 0; ) {
    Register reg = static_cast<Register>(MostSignificantBit(mask));
    mask ^= 1u << reg;
    ofs -= kMipsWordSize;
    // The ZERO register is only included for alignment.
    if (reg != ZERO) {
      __ StoreToOffset(kStoreWord, reg, SP, ofs);
      __ cfi().RelOffset(DWARFReg(reg), ofs);
    }
  }

  for (uint32_t mask = fpu_spill_mask_; mask != 0; ) {
    FRegister reg = static_cast<FRegister>(MostSignificantBit(mask));
    mask ^= 1u << reg;
    ofs -= kMipsDoublewordSize;
    __ StoreDToOffset(reg, SP, ofs);
    // TODO: __ cfi().RelOffset(DWARFReg(reg), ofs);
  }

  // Store the current method pointer.
  // TODO: can we not do this if RequiresCurrentMethod() returns false?
  __ StoreToOffset(kStoreWord, kMethodRegisterArgument, SP, kCurrentMethodStackOffset);
}

void CodeGeneratorMIPS::GenerateFrameExit() {
  __ cfi().RememberState();

  if (!HasEmptyFrame()) {
    // Restore callee-saved registers.

    // For better instruction scheduling restore RA before other registers.
    uint32_t ofs = GetFrameSize();
    for (uint32_t mask = core_spill_mask_; mask != 0; ) {
      Register reg = static_cast<Register>(MostSignificantBit(mask));
      mask ^= 1u << reg;
      ofs -= kMipsWordSize;
      // The ZERO register is only included for alignment.
      if (reg != ZERO) {
        __ LoadFromOffset(kLoadWord, reg, SP, ofs);
        __ cfi().Restore(DWARFReg(reg));
      }
    }

    for (uint32_t mask = fpu_spill_mask_; mask != 0; ) {
      FRegister reg = static_cast<FRegister>(MostSignificantBit(mask));
      mask ^= 1u << reg;
      ofs -= kMipsDoublewordSize;
      __ LoadDFromOffset(reg, SP, ofs);
      // TODO: __ cfi().Restore(DWARFReg(reg));
    }

    size_t frame_size = GetFrameSize();
    // Adjust the stack pointer in the delay slot if doing so doesn't break CFI.
    bool exchange = IsInt<16>(static_cast<int32_t>(frame_size));
    bool reordering = __ SetReorder(false);
    if (exchange) {
      __ Jr(RA);
      __ DecreaseFrameSize(frame_size);  // Single instruction in delay slot.
    } else {
      __ DecreaseFrameSize(frame_size);
      __ Jr(RA);
      __ Nop();  // In delay slot.
    }
    __ SetReorder(reordering);
  } else {
    __ Jr(RA);
    __ NopIfNoReordering();
  }

  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorMIPS::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorMIPS::MoveLocation(Location dst, Location src, Primitive::Type dst_type) {
  if (src.Equals(dst)) {
    return;
  }

  if (src.IsConstant()) {
    MoveConstant(dst, src.GetConstant());
  } else {
    if (Primitive::Is64BitType(dst_type)) {
      Move64(dst, src);
    } else {
      Move32(dst, src);
    }
  }
}

void CodeGeneratorMIPS::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }

  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ Move(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ Mfc1(destination.AsRegister<Register>(), source.AsFpuRegister<FRegister>());
    } else {
      DCHECK(source.IsStackSlot()) << "Cannot move from " << source << " to " << destination;
      __ LoadFromOffset(kLoadWord, destination.AsRegister<Register>(), SP, source.GetStackIndex());
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegister()) {
      __ Mtc1(source.AsRegister<Register>(), destination.AsFpuRegister<FRegister>());
    } else if (source.IsFpuRegister()) {
      __ MovS(destination.AsFpuRegister<FRegister>(), source.AsFpuRegister<FRegister>());
    } else {
      DCHECK(source.IsStackSlot()) << "Cannot move from " << source << " to " << destination;
      __ LoadSFromOffset(destination.AsFpuRegister<FRegister>(), SP, source.GetStackIndex());
    }
  } else {
    DCHECK(destination.IsStackSlot()) << destination;
    if (source.IsRegister()) {
      __ StoreToOffset(kStoreWord, source.AsRegister<Register>(), SP, destination.GetStackIndex());
    } else if (source.IsFpuRegister()) {
      __ StoreSToOffset(source.AsFpuRegister<FRegister>(), SP, destination.GetStackIndex());
    } else {
      DCHECK(source.IsStackSlot()) << "Cannot move from " << source << " to " << destination;
      __ LoadFromOffset(kLoadWord, TMP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, TMP, SP, destination.GetStackIndex());
    }
  }
}

void CodeGeneratorMIPS::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }

  if (destination.IsRegisterPair()) {
    if (source.IsRegisterPair()) {
      __ Move(destination.AsRegisterPairHigh<Register>(), source.AsRegisterPairHigh<Register>());
      __ Move(destination.AsRegisterPairLow<Register>(), source.AsRegisterPairLow<Register>());
    } else if (source.IsFpuRegister()) {
      Register dst_high = destination.AsRegisterPairHigh<Register>();
      Register dst_low =  destination.AsRegisterPairLow<Register>();
      FRegister src = source.AsFpuRegister<FRegister>();
      __ Mfc1(dst_low, src);
      __ MoveFromFpuHigh(dst_high, src);
    } else {
      DCHECK(source.IsDoubleStackSlot()) << "Cannot move from " << source << " to " << destination;
      int32_t off = source.GetStackIndex();
      Register r = destination.AsRegisterPairLow<Register>();
      __ LoadFromOffset(kLoadDoubleword, r, SP, off);
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegisterPair()) {
      FRegister dst = destination.AsFpuRegister<FRegister>();
      Register src_high = source.AsRegisterPairHigh<Register>();
      Register src_low = source.AsRegisterPairLow<Register>();
      __ Mtc1(src_low, dst);
      __ MoveToFpuHigh(src_high, dst);
    } else if (source.IsFpuRegister()) {
      __ MovD(destination.AsFpuRegister<FRegister>(), source.AsFpuRegister<FRegister>());
    } else {
      DCHECK(source.IsDoubleStackSlot()) << "Cannot move from " << source << " to " << destination;
      __ LoadDFromOffset(destination.AsFpuRegister<FRegister>(), SP, source.GetStackIndex());
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot()) << destination;
    int32_t off = destination.GetStackIndex();
    if (source.IsRegisterPair()) {
      __ StoreToOffset(kStoreDoubleword, source.AsRegisterPairLow<Register>(), SP, off);
    } else if (source.IsFpuRegister()) {
      __ StoreDToOffset(source.AsFpuRegister<FRegister>(), SP, off);
    } else {
      DCHECK(source.IsDoubleStackSlot()) << "Cannot move from " << source << " to " << destination;
      __ LoadFromOffset(kLoadWord, TMP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, TMP, SP, off);
      __ LoadFromOffset(kLoadWord, TMP, SP, source.GetStackIndex() + 4);
      __ StoreToOffset(kStoreWord, TMP, SP, off + 4);
    }
  }
}

void CodeGeneratorMIPS::MoveConstant(Location destination, HConstant* c) {
  if (c->IsIntConstant() || c->IsNullConstant()) {
    // Move 32 bit constant.
    int32_t value = GetInt32ValueOf(c);
    if (destination.IsRegister()) {
      Register dst = destination.AsRegister<Register>();
      __ LoadConst32(dst, value);
    } else {
      DCHECK(destination.IsStackSlot())
          << "Cannot move " << c->DebugName() << " to " << destination;
      __ StoreConstToOffset(kStoreWord, value, SP, destination.GetStackIndex(), TMP);
    }
  } else if (c->IsLongConstant()) {
    // Move 64 bit constant.
    int64_t value = GetInt64ValueOf(c);
    if (destination.IsRegisterPair()) {
      Register r_h = destination.AsRegisterPairHigh<Register>();
      Register r_l = destination.AsRegisterPairLow<Register>();
      __ LoadConst64(r_h, r_l, value);
    } else {
      DCHECK(destination.IsDoubleStackSlot())
          << "Cannot move " << c->DebugName() << " to " << destination;
      __ StoreConstToOffset(kStoreDoubleword, value, SP, destination.GetStackIndex(), TMP);
    }
  } else if (c->IsFloatConstant()) {
    // Move 32 bit float constant.
    int32_t value = GetInt32ValueOf(c);
    if (destination.IsFpuRegister()) {
      __ LoadSConst32(destination.AsFpuRegister<FRegister>(), value, TMP);
    } else {
      DCHECK(destination.IsStackSlot())
          << "Cannot move " << c->DebugName() << " to " << destination;
      __ StoreConstToOffset(kStoreWord, value, SP, destination.GetStackIndex(), TMP);
    }
  } else {
    // Move 64 bit double constant.
    DCHECK(c->IsDoubleConstant()) << c->DebugName();
    int64_t value = GetInt64ValueOf(c);
    if (destination.IsFpuRegister()) {
      FRegister fd = destination.AsFpuRegister<FRegister>();
      __ LoadDConst64(fd, value, TMP);
    } else {
      DCHECK(destination.IsDoubleStackSlot())
          << "Cannot move " << c->DebugName() << " to " << destination;
      __ StoreConstToOffset(kStoreDoubleword, value, SP, destination.GetStackIndex(), TMP);
    }
  }
}

void CodeGeneratorMIPS::MoveConstant(Location destination, int32_t value) {
  DCHECK(destination.IsRegister());
  Register dst = destination.AsRegister<Register>();
  __ LoadConst32(dst, value);
}

void CodeGeneratorMIPS::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else if (location.IsRegisterPair()) {
    locations->AddTemp(Location::RegisterLocation(location.AsRegisterPairLow<Register>()));
    locations->AddTemp(Location::RegisterLocation(location.AsRegisterPairHigh<Register>()));
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void CodeGeneratorMIPS::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      method_patches_.size() +
      call_patches_.size() +
      pc_relative_dex_cache_patches_.size() +
      pc_relative_string_patches_.size() +
      pc_relative_type_patches_.size() +
      boot_image_string_patches_.size() +
      boot_image_type_patches_.size() +
      boot_image_address_patches_.size();
  linker_patches->reserve(size);
  for (const auto& entry : method_patches_) {
    const MethodReference& target_method = entry.first;
    Literal* literal = entry.second;
    DCHECK(literal->GetLabel()->IsBound());
    uint32_t literal_offset = __ GetLabelLocation(literal->GetLabel());
    linker_patches->push_back(LinkerPatch::MethodPatch(literal_offset,
                                                       target_method.dex_file,
                                                       target_method.dex_method_index));
  }
  for (const auto& entry : call_patches_) {
    const MethodReference& target_method = entry.first;
    Literal* literal = entry.second;
    DCHECK(literal->GetLabel()->IsBound());
    uint32_t literal_offset = __ GetLabelLocation(literal->GetLabel());
    linker_patches->push_back(LinkerPatch::CodePatch(literal_offset,
                                                     target_method.dex_file,
                                                     target_method.dex_method_index));
  }
  for (const PcRelativePatchInfo& info : pc_relative_dex_cache_patches_) {
    const DexFile& dex_file = info.target_dex_file;
    size_t base_element_offset = info.offset_or_index;
    DCHECK(info.high_label.IsBound());
    uint32_t high_offset = __ GetLabelLocation(&info.high_label);
    DCHECK(info.pc_rel_label.IsBound());
    uint32_t pc_rel_offset = __ GetLabelLocation(&info.pc_rel_label);
    linker_patches->push_back(LinkerPatch::DexCacheArrayPatch(high_offset,
                                                              &dex_file,
                                                              pc_rel_offset,
                                                              base_element_offset));
  }
  for (const PcRelativePatchInfo& info : pc_relative_string_patches_) {
    const DexFile& dex_file = info.target_dex_file;
    size_t string_index = info.offset_or_index;
    DCHECK(info.high_label.IsBound());
    uint32_t high_offset = __ GetLabelLocation(&info.high_label);
    // On R2 we use HMipsComputeBaseMethodAddress and patch relative to
    // the assembler's base label used for PC-relative literals.
    uint32_t pc_rel_offset = info.pc_rel_label.IsBound()
        ? __ GetLabelLocation(&info.pc_rel_label)
        : __ GetPcRelBaseLabelLocation();
    linker_patches->push_back(LinkerPatch::RelativeStringPatch(high_offset,
                                                               &dex_file,
                                                               pc_rel_offset,
                                                               string_index));
  }
  for (const PcRelativePatchInfo& info : pc_relative_type_patches_) {
    const DexFile& dex_file = info.target_dex_file;
    size_t type_index = info.offset_or_index;
    DCHECK(info.high_label.IsBound());
    uint32_t high_offset = __ GetLabelLocation(&info.high_label);
    // On R2 we use HMipsComputeBaseMethodAddress and patch relative to
    // the assembler's base label used for PC-relative literals.
    uint32_t pc_rel_offset = info.pc_rel_label.IsBound()
        ? __ GetLabelLocation(&info.pc_rel_label)
        : __ GetPcRelBaseLabelLocation();
    linker_patches->push_back(LinkerPatch::RelativeTypePatch(high_offset,
                                                             &dex_file,
                                                             pc_rel_offset,
                                                             type_index));
  }
  for (const auto& entry : boot_image_string_patches_) {
    const StringReference& target_string = entry.first;
    Literal* literal = entry.second;
    DCHECK(literal->GetLabel()->IsBound());
    uint32_t literal_offset = __ GetLabelLocation(literal->GetLabel());
    linker_patches->push_back(LinkerPatch::StringPatch(literal_offset,
                                                       target_string.dex_file,
                                                       target_string.string_index));
  }
  for (const auto& entry : boot_image_type_patches_) {
    const TypeReference& target_type = entry.first;
    Literal* literal = entry.second;
    DCHECK(literal->GetLabel()->IsBound());
    uint32_t literal_offset = __ GetLabelLocation(literal->GetLabel());
    linker_patches->push_back(LinkerPatch::TypePatch(literal_offset,
                                                     target_type.dex_file,
                                                     target_type.type_index));
  }
  for (const auto& entry : boot_image_address_patches_) {
    DCHECK(GetCompilerOptions().GetIncludePatchInformation());
    Literal* literal = entry.second;
    DCHECK(literal->GetLabel()->IsBound());
    uint32_t literal_offset = __ GetLabelLocation(literal->GetLabel());
    linker_patches->push_back(LinkerPatch::RecordPosition(literal_offset));
  }
}

CodeGeneratorMIPS::PcRelativePatchInfo* CodeGeneratorMIPS::NewPcRelativeStringPatch(
    const DexFile& dex_file, uint32_t string_index) {
  return NewPcRelativePatch(dex_file, string_index, &pc_relative_string_patches_);
}

CodeGeneratorMIPS::PcRelativePatchInfo* CodeGeneratorMIPS::NewPcRelativeTypePatch(
    const DexFile& dex_file, uint32_t type_index) {
  return NewPcRelativePatch(dex_file, type_index, &pc_relative_type_patches_);
}

CodeGeneratorMIPS::PcRelativePatchInfo* CodeGeneratorMIPS::NewPcRelativeDexCacheArrayPatch(
    const DexFile& dex_file, uint32_t element_offset) {
  return NewPcRelativePatch(dex_file, element_offset, &pc_relative_dex_cache_patches_);
}

CodeGeneratorMIPS::PcRelativePatchInfo* CodeGeneratorMIPS::NewPcRelativePatch(
    const DexFile& dex_file, uint32_t offset_or_index, ArenaDeque<PcRelativePatchInfo>* patches) {
  patches->emplace_back(dex_file, offset_or_index);
  return &patches->back();
}

Literal* CodeGeneratorMIPS::DeduplicateUint32Literal(uint32_t value, Uint32ToLiteralMap* map) {
  return map->GetOrCreate(
      value,
      [this, value]() { return __ NewLiteral<uint32_t>(value); });
}

Literal* CodeGeneratorMIPS::DeduplicateMethodLiteral(MethodReference target_method,
                                                     MethodToLiteralMap* map) {
  return map->GetOrCreate(
      target_method,
      [this]() { return __ NewLiteral<uint32_t>(/* placeholder */ 0u); });
}

Literal* CodeGeneratorMIPS::DeduplicateMethodAddressLiteral(MethodReference target_method) {
  return DeduplicateMethodLiteral(target_method, &method_patches_);
}

Literal* CodeGeneratorMIPS::DeduplicateMethodCodeLiteral(MethodReference target_method) {
  return DeduplicateMethodLiteral(target_method, &call_patches_);
}

Literal* CodeGeneratorMIPS::DeduplicateBootImageStringLiteral(const DexFile& dex_file,
                                                              uint32_t string_index) {
  return boot_image_string_patches_.GetOrCreate(
      StringReference(&dex_file, string_index),
      [this]() { return __ NewLiteral<uint32_t>(/* placeholder */ 0u); });
}

Literal* CodeGeneratorMIPS::DeduplicateBootImageTypeLiteral(const DexFile& dex_file,
                                                            uint32_t type_index) {
  return boot_image_type_patches_.GetOrCreate(
      TypeReference(&dex_file, type_index),
      [this]() { return __ NewLiteral<uint32_t>(/* placeholder */ 0u); });
}

Literal* CodeGeneratorMIPS::DeduplicateBootImageAddressLiteral(uint32_t address) {
  bool needs_patch = GetCompilerOptions().GetIncludePatchInformation();
  Uint32ToLiteralMap* map = needs_patch ? &boot_image_address_patches_ : &uint32_literals_;
  return DeduplicateUint32Literal(dchecked_integral_cast<uint32_t>(address), map);
}

void CodeGeneratorMIPS::MarkGCCard(Register object, Register value) {
  MipsLabel done;
  Register card = AT;
  Register temp = TMP;
  __ Beqz(value, &done);
  __ LoadFromOffset(kLoadWord,
                    card,
                    TR,
                    Thread::CardTableOffset<kMipsPointerSize>().Int32Value());
  __ Srl(temp, object, gc::accounting::CardTable::kCardShift);
  __ Addu(temp, card, temp);
  __ Sb(card, temp, 0);
  __ Bind(&done);
}

void CodeGeneratorMIPS::SetupBlockedRegisters() const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[A1_A2] = true;

  // ZERO, K0, K1, GP, SP, RA are always reserved and can't be allocated.
  blocked_core_registers_[ZERO] = true;
  blocked_core_registers_[K0] = true;
  blocked_core_registers_[K1] = true;
  blocked_core_registers_[GP] = true;
  blocked_core_registers_[SP] = true;
  blocked_core_registers_[RA] = true;

  // AT and TMP(T8) are used as temporary/scratch registers
  // (similar to how AT is used by MIPS assemblers).
  blocked_core_registers_[AT] = true;
  blocked_core_registers_[TMP] = true;
  blocked_fpu_registers_[FTMP] = true;

  // Reserve suspend and thread registers.
  blocked_core_registers_[S0] = true;
  blocked_core_registers_[TR] = true;

  // Reserve T9 for function calls
  blocked_core_registers_[T9] = true;

  // Reserve odd-numbered FPU registers.
  for (size_t i = 1; i < kNumberOfFRegisters; i += 2) {
    blocked_fpu_registers_[i] = true;
  }

  if (GetGraph()->IsDebuggable()) {
    // Stubs do not save callee-save floating point registers. If the graph
    // is debuggable, we need to deal with these registers differently. For
    // now, just block them.
    for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
      blocked_fpu_registers_[kFpuCalleeSaves[i]] = true;
    }
  }

  UpdateBlockedPairRegisters();
}

void CodeGeneratorMIPS::UpdateBlockedPairRegisters() const {
  for (int i = 0; i < kNumberOfRegisterPairs; i++) {
    MipsManagedRegister current =
        MipsManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
    if (blocked_core_registers_[current.AsRegisterPairLow()]
        || blocked_core_registers_[current.AsRegisterPairHigh()]) {
      blocked_register_pairs_[i] = true;
    }
  }
}

size_t CodeGeneratorMIPS::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreToOffset(kStoreWord, Register(reg_id), SP, stack_index);
  return kMipsWordSize;
}

size_t CodeGeneratorMIPS::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFromOffset(kLoadWord, Register(reg_id), SP, stack_index);
  return kMipsWordSize;
}

size_t CodeGeneratorMIPS::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreDToOffset(FRegister(reg_id), SP, stack_index);
  return kMipsDoublewordSize;
}

size_t CodeGeneratorMIPS::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadDFromOffset(FRegister(reg_id), SP, stack_index);
  return kMipsDoublewordSize;
}

void CodeGeneratorMIPS::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Register(reg);
}

void CodeGeneratorMIPS::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << FRegister(reg);
}

constexpr size_t kMipsDirectEntrypointRuntimeOffset = 16;

void CodeGeneratorMIPS::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                      HInstruction* instruction,
                                      uint32_t dex_pc,
                                      SlowPathCode* slow_path) {
  ValidateInvokeRuntime(entrypoint, instruction, slow_path);
  bool reordering = __ SetReorder(false);
  __ LoadFromOffset(kLoadWord, T9, TR, GetThreadOffset<kMipsPointerSize>(entrypoint).Int32Value());
  __ Jalr(T9);
  if (IsDirectEntrypoint(entrypoint)) {
    // Reserve argument space on stack (for $a0-$a3) for
    // entrypoints that directly reference native implementations.
    // Called function may use this space to store $a0-$a3 regs.
    __ IncreaseFrameSize(kMipsDirectEntrypointRuntimeOffset);  // Single instruction in delay slot.
    __ DecreaseFrameSize(kMipsDirectEntrypointRuntimeOffset);
  } else {
    __ Nop();  // In delay slot.
  }
  __ SetReorder(reordering);
  if (EntrypointRequiresStackMap(entrypoint)) {
    RecordPcInfo(instruction, dex_pc, slow_path);
  }
}

void InstructionCodeGeneratorMIPS::GenerateClassInitializationCheck(SlowPathCodeMIPS* slow_path,
                                                                    Register class_reg) {
  __ LoadFromOffset(kLoadWord, TMP, class_reg, mirror::Class::StatusOffset().Int32Value());
  __ LoadConst32(AT, mirror::Class::kStatusInitialized);
  __ Blt(TMP, AT, slow_path->GetEntryLabel());
  // Even if the initialized flag is set, we need to ensure consistent memory ordering.
  __ Sync(0);
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorMIPS::GenerateMemoryBarrier(MemBarrierKind kind ATTRIBUTE_UNUSED) {
  __ Sync(0);  // Only stype 0 is supported.
}

void InstructionCodeGeneratorMIPS::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                        HBasicBlock* successor) {
  SuspendCheckSlowPathMIPS* slow_path =
    new (GetGraph()->GetArena()) SuspendCheckSlowPathMIPS(instruction, successor);
  codegen_->AddSlowPath(slow_path);

  __ LoadFromOffset(kLoadUnsignedHalfword,
                    TMP,
                    TR,
                    Thread::ThreadFlagsOffset<kMipsPointerSize>().Int32Value());
  if (successor == nullptr) {
    __ Bnez(TMP, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ Beqz(TMP, codegen_->GetLabelOf(successor));
    __ B(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorMIPS::InstructionCodeGeneratorMIPS(HGraph* graph,
                                                           CodeGeneratorMIPS* codegen)
      : InstructionCodeGenerator(graph, codegen),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void LocationsBuilderMIPS::HandleBinaryOp(HBinaryOperation* instruction) {
  DCHECK_EQ(instruction->InputCount(), 2U);
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type type = instruction->GetResultType();
  switch (type) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      HInstruction* right = instruction->InputAt(1);
      bool can_use_imm = false;
      if (right->IsConstant()) {
        int32_t imm = CodeGenerator::GetInt32ValueOf(right->AsConstant());
        if (instruction->IsAnd() || instruction->IsOr() || instruction->IsXor()) {
          can_use_imm = IsUint<16>(imm);
        } else if (instruction->IsAdd()) {
          can_use_imm = IsInt<16>(imm);
        } else {
          DCHECK(instruction->IsSub());
          can_use_imm = IsInt<16>(-imm);
        }
      }
      if (can_use_imm)
        locations->SetInAt(1, Location::ConstantLocation(right->AsConstant()));
      else
        locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      DCHECK(instruction->IsAdd() || instruction->IsSub());
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected " << instruction->DebugName() << " type " << type;
  }
}

void InstructionCodeGeneratorMIPS::HandleBinaryOp(HBinaryOperation* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt: {
      Register dst = locations->Out().AsRegister<Register>();
      Register lhs = locations->InAt(0).AsRegister<Register>();
      Location rhs_location = locations->InAt(1);

      Register rhs_reg = ZERO;
      int32_t rhs_imm = 0;
      bool use_imm = rhs_location.IsConstant();
      if (use_imm) {
        rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
      } else {
        rhs_reg = rhs_location.AsRegister<Register>();
      }

      if (instruction->IsAnd()) {
        if (use_imm)
          __ Andi(dst, lhs, rhs_imm);
        else
          __ And(dst, lhs, rhs_reg);
      } else if (instruction->IsOr()) {
        if (use_imm)
          __ Ori(dst, lhs, rhs_imm);
        else
          __ Or(dst, lhs, rhs_reg);
      } else if (instruction->IsXor()) {
        if (use_imm)
          __ Xori(dst, lhs, rhs_imm);
        else
          __ Xor(dst, lhs, rhs_reg);
      } else if (instruction->IsAdd()) {
        if (use_imm)
          __ Addiu(dst, lhs, rhs_imm);
        else
          __ Addu(dst, lhs, rhs_reg);
      } else {
        DCHECK(instruction->IsSub());
        if (use_imm)
          __ Addiu(dst, lhs, -rhs_imm);
        else
          __ Subu(dst, lhs, rhs_reg);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
      Register dst_low = locations->Out().AsRegisterPairLow<Register>();
      Register lhs_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register lhs_low = locations->InAt(0).AsRegisterPairLow<Register>();
      Location rhs_location = locations->InAt(1);
      bool use_imm = rhs_location.IsConstant();
      if (!use_imm) {
        Register rhs_high = rhs_location.AsRegisterPairHigh<Register>();
        Register rhs_low = rhs_location.AsRegisterPairLow<Register>();
        if (instruction->IsAnd()) {
          __ And(dst_low, lhs_low, rhs_low);
          __ And(dst_high, lhs_high, rhs_high);
        } else if (instruction->IsOr()) {
          __ Or(dst_low, lhs_low, rhs_low);
          __ Or(dst_high, lhs_high, rhs_high);
        } else if (instruction->IsXor()) {
          __ Xor(dst_low, lhs_low, rhs_low);
          __ Xor(dst_high, lhs_high, rhs_high);
        } else if (instruction->IsAdd()) {
          if (lhs_low == rhs_low) {
            // Special case for lhs = rhs and the sum potentially overwriting both lhs and rhs.
            __ Slt(TMP, lhs_low, ZERO);
            __ Addu(dst_low, lhs_low, rhs_low);
          } else {
            __ Addu(dst_low, lhs_low, rhs_low);
            // If the sum overwrites rhs, lhs remains unchanged, otherwise rhs remains unchanged.
            __ Sltu(TMP, dst_low, (dst_low == rhs_low) ? lhs_low : rhs_low);
          }
          __ Addu(dst_high, lhs_high, rhs_high);
          __ Addu(dst_high, dst_high, TMP);
        } else {
          DCHECK(instruction->IsSub());
          __ Sltu(TMP, lhs_low, rhs_low);
          __ Subu(dst_low, lhs_low, rhs_low);
          __ Subu(dst_high, lhs_high, rhs_high);
          __ Subu(dst_high, dst_high, TMP);
        }
      } else {
        int64_t value = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant()->AsConstant());
        if (instruction->IsOr()) {
          uint32_t low = Low32Bits(value);
          uint32_t high = High32Bits(value);
          if (IsUint<16>(low)) {
            if (dst_low != lhs_low || low != 0) {
              __ Ori(dst_low, lhs_low, low);
            }
          } else {
            __ LoadConst32(TMP, low);
            __ Or(dst_low, lhs_low, TMP);
          }
          if (IsUint<16>(high)) {
            if (dst_high != lhs_high || high != 0) {
              __ Ori(dst_high, lhs_high, high);
            }
          } else {
            if (high != low) {
              __ LoadConst32(TMP, high);
            }
            __ Or(dst_high, lhs_high, TMP);
          }
        } else if (instruction->IsXor()) {
          uint32_t low = Low32Bits(value);
          uint32_t high = High32Bits(value);
          if (IsUint<16>(low)) {
            if (dst_low != lhs_low || low != 0) {
              __ Xori(dst_low, lhs_low, low);
            }
          } else {
            __ LoadConst32(TMP, low);
            __ Xor(dst_low, lhs_low, TMP);
          }
          if (IsUint<16>(high)) {
            if (dst_high != lhs_high || high != 0) {
              __ Xori(dst_high, lhs_high, high);
            }
          } else {
            if (high != low) {
              __ LoadConst32(TMP, high);
            }
            __ Xor(dst_high, lhs_high, TMP);
          }
        } else if (instruction->IsAnd()) {
          uint32_t low = Low32Bits(value);
          uint32_t high = High32Bits(value);
          if (IsUint<16>(low)) {
            __ Andi(dst_low, lhs_low, low);
          } else if (low != 0xFFFFFFFF) {
            __ LoadConst32(TMP, low);
            __ And(dst_low, lhs_low, TMP);
          } else if (dst_low != lhs_low) {
            __ Move(dst_low, lhs_low);
          }
          if (IsUint<16>(high)) {
            __ Andi(dst_high, lhs_high, high);
          } else if (high != 0xFFFFFFFF) {
            if (high != low) {
              __ LoadConst32(TMP, high);
            }
            __ And(dst_high, lhs_high, TMP);
          } else if (dst_high != lhs_high) {
            __ Move(dst_high, lhs_high);
          }
        } else {
          if (instruction->IsSub()) {
            value = -value;
          } else {
            DCHECK(instruction->IsAdd());
          }
          int32_t low = Low32Bits(value);
          int32_t high = High32Bits(value);
          if (IsInt<16>(low)) {
            if (dst_low != lhs_low || low != 0) {
              __ Addiu(dst_low, lhs_low, low);
            }
            if (low != 0) {
              __ Sltiu(AT, dst_low, low);
            }
          } else {
            __ LoadConst32(TMP, low);
            __ Addu(dst_low, lhs_low, TMP);
            __ Sltu(AT, dst_low, TMP);
          }
          if (IsInt<16>(high)) {
            if (dst_high != lhs_high || high != 0) {
              __ Addiu(dst_high, lhs_high, high);
            }
          } else {
            if (high != low) {
              __ LoadConst32(TMP, high);
            }
            __ Addu(dst_high, lhs_high, TMP);
          }
          if (low != 0) {
            __ Addu(dst_high, dst_high, AT);
          }
        }
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
      FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
      if (instruction->IsAdd()) {
        if (type == Primitive::kPrimFloat) {
          __ AddS(dst, lhs, rhs);
        } else {
          __ AddD(dst, lhs, rhs);
        }
      } else {
        DCHECK(instruction->IsSub());
        if (type == Primitive::kPrimFloat) {
          __ SubS(dst, lhs, rhs);
        } else {
          __ SubD(dst, lhs, rhs);
        }
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
}

void LocationsBuilderMIPS::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr() || instr->IsRor());

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister());
      break;
    default:
      LOG(FATAL) << "Unexpected shift type " << type;
  }
}

static constexpr size_t kMipsBitsPerWord = kMipsWordSize * kBitsPerByte;

void InstructionCodeGeneratorMIPS::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr() || instr->IsRor());
  LocationSummary* locations = instr->GetLocations();
  Primitive::Type type = instr->GetType();

  Location rhs_location = locations->InAt(1);
  bool use_imm = rhs_location.IsConstant();
  Register rhs_reg = use_imm ? ZERO : rhs_location.AsRegister<Register>();
  int64_t rhs_imm = use_imm ? CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant()) : 0;
  const uint32_t shift_mask =
      (type == Primitive::kPrimInt) ? kMaxIntShiftDistance : kMaxLongShiftDistance;
  const uint32_t shift_value = rhs_imm & shift_mask;
  // Are the INS (Insert Bit Field) and ROTR instructions supported?
  bool has_ins_rotr = codegen_->GetInstructionSetFeatures().IsMipsIsaRevGreaterThanEqual2();

  switch (type) {
    case Primitive::kPrimInt: {
      Register dst = locations->Out().AsRegister<Register>();
      Register lhs = locations->InAt(0).AsRegister<Register>();
      if (use_imm) {
        if (shift_value == 0) {
          if (dst != lhs) {
            __ Move(dst, lhs);
          }
        } else if (instr->IsShl()) {
          __ Sll(dst, lhs, shift_value);
        } else if (instr->IsShr()) {
          __ Sra(dst, lhs, shift_value);
        } else if (instr->IsUShr()) {
          __ Srl(dst, lhs, shift_value);
        } else {
          if (has_ins_rotr) {
            __ Rotr(dst, lhs, shift_value);
          } else {
            __ Sll(TMP, lhs, (kMipsBitsPerWord - shift_value) & shift_mask);
            __ Srl(dst, lhs, shift_value);
            __ Or(dst, dst, TMP);
          }
        }
      } else {
        if (instr->IsShl()) {
          __ Sllv(dst, lhs, rhs_reg);
        } else if (instr->IsShr()) {
          __ Srav(dst, lhs, rhs_reg);
        } else if (instr->IsUShr()) {
          __ Srlv(dst, lhs, rhs_reg);
        } else {
          if (has_ins_rotr) {
            __ Rotrv(dst, lhs, rhs_reg);
          } else {
            __ Subu(TMP, ZERO, rhs_reg);
            // 32-bit shift instructions use the 5 least significant bits of the shift count, so
            // shifting by `-rhs_reg` is equivalent to shifting by `(32 - rhs_reg) & 31`. The case
            // when `rhs_reg & 31 == 0` is OK even though we don't shift `lhs` left all the way out
            // by 32, because the result in this case is computed as `(lhs >> 0) | (lhs << 0)`,
            // IOW, the OR'd values are equal.
            __ Sllv(TMP, lhs, TMP);
            __ Srlv(dst, lhs, rhs_reg);
            __ Or(dst, dst, TMP);
          }
        }
      }
      break;
    }

    case Primitive::kPrimLong: {
      Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
      Register dst_low = locations->Out().AsRegisterPairLow<Register>();
      Register lhs_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register lhs_low = locations->InAt(0).AsRegisterPairLow<Register>();
      if (use_imm) {
          if (shift_value == 0) {
            codegen_->Move64(locations->Out(), locations->InAt(0));
          } else if (shift_value < kMipsBitsPerWord) {
            if (has_ins_rotr) {
              if (instr->IsShl()) {
                __ Srl(dst_high, lhs_low, kMipsBitsPerWord - shift_value);
                __ Ins(dst_high, lhs_high, shift_value, kMipsBitsPerWord - shift_value);
                __ Sll(dst_low, lhs_low, shift_value);
              } else if (instr->IsShr()) {
                __ Srl(dst_low, lhs_low, shift_value);
                __ Ins(dst_low, lhs_high, kMipsBitsPerWord - shift_value, shift_value);
                __ Sra(dst_high, lhs_high, shift_value);
              } else if (instr->IsUShr()) {
                __ Srl(dst_low, lhs_low, shift_value);
                __ Ins(dst_low, lhs_high, kMipsBitsPerWord - shift_value, shift_value);
                __ Srl(dst_high, lhs_high, shift_value);
              } else {
                __ Srl(dst_low, lhs_low, shift_value);
                __ Ins(dst_low, lhs_high, kMipsBitsPerWord - shift_value, shift_value);
                __ Srl(dst_high, lhs_high, shift_value);
                __ Ins(dst_high, lhs_low, kMipsBitsPerWord - shift_value, shift_value);
              }
            } else {
              if (instr->IsShl()) {
                __ Sll(dst_low, lhs_low, shift_value);
                __ Srl(TMP, lhs_low, kMipsBitsPerWord - shift_value);
                __ Sll(dst_high, lhs_high, shift_value);
                __ Or(dst_high, dst_high, TMP);
              } else if (instr->IsShr()) {
                __ Sra(dst_high, lhs_high, shift_value);
                __ Sll(TMP, lhs_high, kMipsBitsPerWord - shift_value);
                __ Srl(dst_low, lhs_low, shift_value);
                __ Or(dst_low, dst_low, TMP);
              } else if (instr->IsUShr()) {
                __ Srl(dst_high, lhs_high, shift_value);
                __ Sll(TMP, lhs_high, kMipsBitsPerWord - shift_value);
                __ Srl(dst_low, lhs_low, shift_value);
                __ Or(dst_low, dst_low, TMP);
              } else {
                __ Srl(TMP, lhs_low, shift_value);
                __ Sll(dst_low, lhs_high, kMipsBitsPerWord - shift_value);
                __ Or(dst_low, dst_low, TMP);
                __ Srl(TMP, lhs_high, shift_value);
                __ Sll(dst_high, lhs_low, kMipsBitsPerWord - shift_value);
                __ Or(dst_high, dst_high, TMP);
              }
            }
          } else {
            const uint32_t shift_value_high = shift_value - kMipsBitsPerWord;
            if (instr->IsShl()) {
              __ Sll(dst_high, lhs_low, shift_value_high);
              __ Move(dst_low, ZERO);
            } else if (instr->IsShr()) {
              __ Sra(dst_low, lhs_high, shift_value_high);
              __ Sra(dst_high, dst_low, kMipsBitsPerWord - 1);
            } else if (instr->IsUShr()) {
              __ Srl(dst_low, lhs_high, shift_value_high);
              __ Move(dst_high, ZERO);
            } else {
              if (shift_value == kMipsBitsPerWord) {
                // 64-bit rotation by 32 is just a swap.
                __ Move(dst_low, lhs_high);
                __ Move(dst_high, lhs_low);
              } else {
                if (has_ins_rotr) {
                  __ Srl(dst_low, lhs_high, shift_value_high);
                  __ Ins(dst_low, lhs_low, kMipsBitsPerWord - shift_value_high, shift_value_high);
                  __ Srl(dst_high, lhs_low, shift_value_high);
                  __ Ins(dst_high, lhs_high, kMipsBitsPerWord - shift_value_high, shift_value_high);
                } else {
                  __ Sll(TMP, lhs_low, kMipsBitsPerWord - shift_value_high);
                  __ Srl(dst_low, lhs_high, shift_value_high);
                  __ Or(dst_low, dst_low, TMP);
                  __ Sll(TMP, lhs_high, kMipsBitsPerWord - shift_value_high);
                  __ Srl(dst_high, lhs_low, shift_value_high);
                  __ Or(dst_high, dst_high, TMP);
                }
              }
            }
          }
      } else {
        MipsLabel done;
        if (instr->IsShl()) {
          __ Sllv(dst_low, lhs_low, rhs_reg);
          __ Nor(AT, ZERO, rhs_reg);
          __ Srl(TMP, lhs_low, 1);
          __ Srlv(TMP, TMP, AT);
          __ Sllv(dst_high, lhs_high, rhs_reg);
          __ Or(dst_high, dst_high, TMP);
          __ Andi(TMP, rhs_reg, kMipsBitsPerWord);
          __ Beqz(TMP, &done);
          __ Move(dst_high, dst_low);
          __ Move(dst_low, ZERO);
        } else if (instr->IsShr()) {
          __ Srav(dst_high, lhs_high, rhs_reg);
          __ Nor(AT, ZERO, rhs_reg);
          __ Sll(TMP, lhs_high, 1);
          __ Sllv(TMP, TMP, AT);
          __ Srlv(dst_low, lhs_low, rhs_reg);
          __ Or(dst_low, dst_low, TMP);
          __ Andi(TMP, rhs_reg, kMipsBitsPerWord);
          __ Beqz(TMP, &done);
          __ Move(dst_low, dst_high);
          __ Sra(dst_high, dst_high, 31);
        } else if (instr->IsUShr()) {
          __ Srlv(dst_high, lhs_high, rhs_reg);
          __ Nor(AT, ZERO, rhs_reg);
          __ Sll(TMP, lhs_high, 1);
          __ Sllv(TMP, TMP, AT);
          __ Srlv(dst_low, lhs_low, rhs_reg);
          __ Or(dst_low, dst_low, TMP);
          __ Andi(TMP, rhs_reg, kMipsBitsPerWord);
          __ Beqz(TMP, &done);
          __ Move(dst_low, dst_high);
          __ Move(dst_high, ZERO);
        } else {
          __ Nor(AT, ZERO, rhs_reg);
          __ Srlv(TMP, lhs_low, rhs_reg);
          __ Sll(dst_low, lhs_high, 1);
          __ Sllv(dst_low, dst_low, AT);
          __ Or(dst_low, dst_low, TMP);
          __ Srlv(TMP, lhs_high, rhs_reg);
          __ Sll(dst_high, lhs_low, 1);
          __ Sllv(dst_high, dst_high, AT);
          __ Or(dst_high, dst_high, TMP);
          __ Andi(TMP, rhs_reg, kMipsBitsPerWord);
          __ Beqz(TMP, &done);
          __ Move(TMP, dst_high);
          __ Move(dst_high, dst_low);
          __ Move(dst_low, TMP);
        }
        __ Bind(&done);
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
}

void LocationsBuilderMIPS::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

auto InstructionCodeGeneratorMIPS::GetImplicitNullChecker(HInstruction* instruction) {
  auto null_checker = [this, instruction]() {
    this->codegen_->MaybeRecordImplicitNullCheck(instruction);
  };
  return null_checker;
}

void InstructionCodeGeneratorMIPS::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);
  uint32_t data_offset = CodeGenerator::GetArrayDataOffset(instruction);
  auto null_checker = GetImplicitNullChecker(instruction);

  Primitive::Type type = instruction->GetType();
  switch (type) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset, null_checker);
      } else {
        __ Addu(TMP, obj, index.AsRegister<Register>());
        __ LoadFromOffset(kLoadUnsignedByte, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadSignedByte, out, obj, offset, null_checker);
      } else {
        __ Addu(TMP, obj, index.AsRegister<Register>());
        __ LoadFromOffset(kLoadSignedByte, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_2);
        __ Addu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadSignedHalfword, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_2);
        __ Addu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadUnsignedHalfword, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFromOffset(kLoadWord, out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_4);
        __ Addu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadWord, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Register out = locations->Out().AsRegisterPairLow<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFromOffset(kLoadDoubleword, out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_8);
        __ Addu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadDoubleword, out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      FRegister out = locations->Out().AsFpuRegister<FRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadSFromOffset(out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_4);
        __ Addu(TMP, obj, TMP);
        __ LoadSFromOffset(out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      FRegister out = locations->Out().AsFpuRegister<FRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadDFromOffset(out, obj, offset, null_checker);
      } else {
        __ Sll(TMP, index.AsRegister<Register>(), TIMES_8);
        __ Addu(TMP, obj, TMP);
        __ LoadDFromOffset(out, TMP, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderMIPS::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = CodeGenerator::GetArrayLengthOffset(instruction);
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  __ LoadFromOffset(kLoadWord, out, obj, offset);
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

Location LocationsBuilderMIPS::RegisterOrZeroConstant(HInstruction* instruction) {
  return (instruction->IsConstant() && instruction->AsConstant()->IsZeroBitPattern())
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RequiresRegister();
}

Location LocationsBuilderMIPS::FpuRegisterOrConstantForStore(HInstruction* instruction) {
  // We can store 0.0 directly (from the ZERO register) without loading it into an FPU register.
  // We can store a non-zero float or double constant without first loading it into the FPU,
  // but we should only prefer this if the constant has a single use.
  if (instruction->IsConstant() &&
      (instruction->AsConstant()->IsZeroBitPattern() ||
       instruction->GetUses().HasExactlyOneElement())) {
    return Location::ConstantLocation(instruction->AsConstant());
    // Otherwise fall through and require an FPU register for the constant.
  }
  return Location::RequiresFpuRegister();
}

void LocationsBuilderMIPS::VisitArraySet(HArraySet* instruction) {
  bool needs_runtime_call = instruction->NeedsTypeCheck();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      needs_runtime_call ? LocationSummary::kCallOnMainOnly : LocationSummary::kNoCall);
  if (needs_runtime_call) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (Primitive::IsFloatingPointType(instruction->InputAt(2)->GetType())) {
      locations->SetInAt(2, FpuRegisterOrConstantForStore(instruction->InputAt(2)));
    } else {
      locations->SetInAt(2, RegisterOrZeroConstant(instruction->InputAt(2)));
    }
  }
}

void InstructionCodeGeneratorMIPS::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);
  Location value_location = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  auto null_checker = GetImplicitNullChecker(instruction);
  Register base_reg = index.IsConstant() ? obj : TMP;

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      if (index.IsConstant()) {
        data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1;
      } else {
        __ Addu(base_reg, obj, index.AsRegister<Register>());
      }
      if (value_location.IsConstant()) {
        int32_t value = CodeGenerator::GetInt32ValueOf(value_location.GetConstant());
        __ StoreConstToOffset(kStoreByte, value, base_reg, data_offset, TMP, null_checker);
      } else {
        Register value = value_location.AsRegister<Register>();
        __ StoreToOffset(kStoreByte, value, base_reg, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      if (index.IsConstant()) {
        data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2;
      } else {
        __ Sll(base_reg, index.AsRegister<Register>(), TIMES_2);
        __ Addu(base_reg, obj, base_reg);
      }
      if (value_location.IsConstant()) {
        int32_t value = CodeGenerator::GetInt32ValueOf(value_location.GetConstant());
        __ StoreConstToOffset(kStoreHalfword, value, base_reg, data_offset, TMP, null_checker);
      } else {
        Register value = value_location.AsRegister<Register>();
        __ StoreToOffset(kStoreHalfword, value, base_reg, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        if (index.IsConstant()) {
          data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4;
        } else {
          __ Sll(base_reg, index.AsRegister<Register>(), TIMES_4);
          __ Addu(base_reg, obj, base_reg);
        }
        if (value_location.IsConstant()) {
          int32_t value = CodeGenerator::GetInt32ValueOf(value_location.GetConstant());
          __ StoreConstToOffset(kStoreWord, value, base_reg, data_offset, TMP, null_checker);
          DCHECK(!needs_write_barrier);
        } else {
          Register value = value_location.AsRegister<Register>();
          __ StoreToOffset(kStoreWord, value, base_reg, data_offset, null_checker);
          if (needs_write_barrier) {
            DCHECK_EQ(value_type, Primitive::kPrimNot);
            codegen_->MarkGCCard(obj, value);
          }
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        codegen_->InvokeRuntime(kQuickAputObject, instruction, instruction->GetDexPc());
        CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      if (index.IsConstant()) {
        data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8;
      } else {
        __ Sll(base_reg, index.AsRegister<Register>(), TIMES_8);
        __ Addu(base_reg, obj, base_reg);
      }
      if (value_location.IsConstant()) {
        int64_t value = CodeGenerator::GetInt64ValueOf(value_location.GetConstant());
        __ StoreConstToOffset(kStoreDoubleword, value, base_reg, data_offset, TMP, null_checker);
      } else {
        Register value = value_location.AsRegisterPairLow<Register>();
        __ StoreToOffset(kStoreDoubleword, value, base_reg, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      if (index.IsConstant()) {
        data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4;
      } else {
        __ Sll(base_reg, index.AsRegister<Register>(), TIMES_4);
        __ Addu(base_reg, obj, base_reg);
      }
      if (value_location.IsConstant()) {
        int32_t value = CodeGenerator::GetInt32ValueOf(value_location.GetConstant());
        __ StoreConstToOffset(kStoreWord, value, base_reg, data_offset, TMP, null_checker);
      } else {
        FRegister value = value_location.AsFpuRegister<FRegister>();
        __ StoreSToOffset(value, base_reg, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      if (index.IsConstant()) {
        data_offset += index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8;
      } else {
        __ Sll(base_reg, index.AsRegister<Register>(), TIMES_8);
        __ Addu(base_reg, obj, base_reg);
      }
      if (value_location.IsConstant()) {
        int64_t value = CodeGenerator::GetInt64ValueOf(value_location.GetConstant());
        __ StoreConstToOffset(kStoreDoubleword, value, base_reg, data_offset, TMP, null_checker);
      } else {
        FRegister value = value_location.AsFpuRegister<FRegister>();
        __ StoreDToOffset(value, base_reg, data_offset, null_checker);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderMIPS::VisitBoundsCheck(HBoundsCheck* instruction) {
  RegisterSet caller_saves = RegisterSet::Empty();
  InvokeRuntimeCallingConvention calling_convention;
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction, caller_saves);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  BoundsCheckSlowPathMIPS* slow_path =
      new (GetGraph()->GetArena()) BoundsCheckSlowPathMIPS(instruction);
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).AsRegister<Register>();
  Register length = locations->InAt(1).AsRegister<Register>();

  // length is limited by the maximum positive signed 32-bit integer.
  // Unsigned comparison of length and index checks for index < 0
  // and for length <= index simultaneously.
  __ Bgeu(index, length, slow_path->GetEntryLabel());
}

void LocationsBuilderMIPS::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Note that TypeCheckSlowPathMIPS uses this register too.
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register cls = locations->InAt(1).AsRegister<Register>();
  Register obj_cls = locations->GetTemp(0).AsRegister<Register>();

  SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS(instruction);
  codegen_->AddSlowPath(slow_path);

  // TODO: avoid this check if we know obj is not null.
  __ Beqz(obj, slow_path->GetExitLabel());
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, obj_cls, obj, mirror::Object::ClassOffset().Int32Value());
  __ Bne(obj_cls, cls, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderMIPS::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathMIPS(
      check->GetLoadClass(),
      check,
      check->GetDexPc(),
      true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<Register>());
}

void LocationsBuilderMIPS::VisitCompare(HCompare* compare) {
  Primitive::Type in_type = compare->InputAt(0)->GetType();

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);

  switch (in_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      // Output overlaps because it is written before doing the low comparison.
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected type for compare operation " << in_type;
  }
}

void InstructionCodeGeneratorMIPS::VisitCompare(HCompare* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register res = locations->Out().AsRegister<Register>();
  Primitive::Type in_type = instruction->InputAt(0)->GetType();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (in_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt: {
      Register lhs = locations->InAt(0).AsRegister<Register>();
      Register rhs = locations->InAt(1).AsRegister<Register>();
      __ Slt(TMP, lhs, rhs);
      __ Slt(res, rhs, lhs);
      __ Subu(res, res, TMP);
      break;
    }
    case Primitive::kPrimLong: {
      MipsLabel done;
      Register lhs_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register lhs_low  = locations->InAt(0).AsRegisterPairLow<Register>();
      Register rhs_high = locations->InAt(1).AsRegisterPairHigh<Register>();
      Register rhs_low  = locations->InAt(1).AsRegisterPairLow<Register>();
      // TODO: more efficient (direct) comparison with a constant.
      __ Slt(TMP, lhs_high, rhs_high);
      __ Slt(AT, rhs_high, lhs_high);  // Inverted: is actually gt.
      __ Subu(res, AT, TMP);           // Result -1:1:0 for [ <, >, == ].
      __ Bnez(res, &done);             // If we compared ==, check if lower bits are also equal.
      __ Sltu(TMP, lhs_low, rhs_low);
      __ Sltu(AT, rhs_low, lhs_low);   // Inverted: is actually gt.
      __ Subu(res, AT, TMP);           // Result -1:1:0 for [ <, >, == ].
      __ Bind(&done);
      break;
    }

    case Primitive::kPrimFloat: {
      bool gt_bias = instruction->IsGtBias();
      FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
      FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
      MipsLabel done;
      if (isR6) {
        __ CmpEqS(FTMP, lhs, rhs);
        __ LoadConst32(res, 0);
        __ Bc1nez(FTMP, &done);
        if (gt_bias) {
          __ CmpLtS(FTMP, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Bc1nez(FTMP, &done);
          __ LoadConst32(res, 1);
        } else {
          __ CmpLtS(FTMP, rhs, lhs);
          __ LoadConst32(res, 1);
          __ Bc1nez(FTMP, &done);
          __ LoadConst32(res, -1);
        }
      } else {
        if (gt_bias) {
          __ ColtS(0, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Bc1t(0, &done);
          __ CeqS(0, lhs, rhs);
          __ LoadConst32(res, 1);
          __ Movt(res, ZERO, 0);
        } else {
          __ ColtS(0, rhs, lhs);
          __ LoadConst32(res, 1);
          __ Bc1t(0, &done);
          __ CeqS(0, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Movt(res, ZERO, 0);
        }
      }
      __ Bind(&done);
      break;
    }
    case Primitive::kPrimDouble: {
      bool gt_bias = instruction->IsGtBias();
      FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
      FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
      MipsLabel done;
      if (isR6) {
        __ CmpEqD(FTMP, lhs, rhs);
        __ LoadConst32(res, 0);
        __ Bc1nez(FTMP, &done);
        if (gt_bias) {
          __ CmpLtD(FTMP, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Bc1nez(FTMP, &done);
          __ LoadConst32(res, 1);
        } else {
          __ CmpLtD(FTMP, rhs, lhs);
          __ LoadConst32(res, 1);
          __ Bc1nez(FTMP, &done);
          __ LoadConst32(res, -1);
        }
      } else {
        if (gt_bias) {
          __ ColtD(0, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Bc1t(0, &done);
          __ CeqD(0, lhs, rhs);
          __ LoadConst32(res, 1);
          __ Movt(res, ZERO, 0);
        } else {
          __ ColtD(0, rhs, lhs);
          __ LoadConst32(res, 1);
          __ Bc1t(0, &done);
          __ CeqD(0, lhs, rhs);
          __ LoadConst32(res, -1);
          __ Movt(res, ZERO, 0);
        }
      }
      __ Bind(&done);
      break;
    }

    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderMIPS::HandleCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  switch (instruction->InputAt(0)->GetType()) {
    default:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      break;
  }
  if (!instruction->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorMIPS::HandleCondition(HCondition* instruction) {
  if (instruction->IsEmittedAtUseSite()) {
    return;
  }

  Primitive::Type type = instruction->InputAt(0)->GetType();
  LocationSummary* locations = instruction->GetLocations();
  Register dst = locations->Out().AsRegister<Register>();
  MipsLabel true_label;

  switch (type) {
    default:
      // Integer case.
      GenerateIntCompare(instruction->GetCondition(), locations);
      return;

    case Primitive::kPrimLong:
      // TODO: don't use branches.
      GenerateLongCompareAndBranch(instruction->GetCondition(), locations, &true_label);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      GenerateFpCompare(instruction->GetCondition(), instruction->IsGtBias(), type, locations);
      return;
  }

  // Convert the branches into the result.
  MipsLabel done;

  // False case: result = 0.
  __ LoadConst32(dst, 0);
  __ B(&done);

  // True case: result = 1.
  __ Bind(&true_label);
  __ LoadConst32(dst, 1);
  __ Bind(&done);
}

void InstructionCodeGeneratorMIPS::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimInt);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = locations->Out().AsRegister<Register>();
  Register dividend = locations->InAt(0).AsRegister<Register>();
  int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();
  DCHECK(imm == 1 || imm == -1);

  if (instruction->IsRem()) {
    __ Move(out, ZERO);
  } else {
    if (imm == -1) {
      __ Subu(out, ZERO, dividend);
    } else if (out != dividend) {
      __ Move(out, dividend);
    }
  }
}

void InstructionCodeGeneratorMIPS::DivRemByPowerOfTwo(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimInt);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = locations->Out().AsRegister<Register>();
  Register dividend = locations->InAt(0).AsRegister<Register>();
  int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();
  uint32_t abs_imm = static_cast<uint32_t>(AbsOrMin(imm));
  int ctz_imm = CTZ(abs_imm);

  if (instruction->IsDiv()) {
    if (ctz_imm == 1) {
      // Fast path for division by +/-2, which is very common.
      __ Srl(TMP, dividend, 31);
    } else {
      __ Sra(TMP, dividend, 31);
      __ Srl(TMP, TMP, 32 - ctz_imm);
    }
    __ Addu(out, dividend, TMP);
    __ Sra(out, out, ctz_imm);
    if (imm < 0) {
      __ Subu(out, ZERO, out);
    }
  } else {
    if (ctz_imm == 1) {
      // Fast path for modulo +/-2, which is very common.
      __ Sra(TMP, dividend, 31);
      __ Subu(out, dividend, TMP);
      __ Andi(out, out, 1);
      __ Addu(out, out, TMP);
    } else {
      __ Sra(TMP, dividend, 31);
      __ Srl(TMP, TMP, 32 - ctz_imm);
      __ Addu(out, dividend, TMP);
      if (IsUint<16>(abs_imm - 1)) {
        __ Andi(out, out, abs_imm - 1);
      } else {
        __ Sll(out, out, 32 - ctz_imm);
        __ Srl(out, out, 32 - ctz_imm);
      }
      __ Subu(out, out, TMP);
    }
  }
}

void InstructionCodeGeneratorMIPS::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimInt);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = locations->Out().AsRegister<Register>();
  Register dividend = locations->InAt(0).AsRegister<Register>();
  int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, false /* is_long */, &magic, &shift);

  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  __ LoadConst32(TMP, magic);
  if (isR6) {
    __ MuhR6(TMP, dividend, TMP);
  } else {
    __ MultR2(dividend, TMP);
    __ Mfhi(TMP);
  }
  if (imm > 0 && magic < 0) {
    __ Addu(TMP, TMP, dividend);
  } else if (imm < 0 && magic > 0) {
    __ Subu(TMP, TMP, dividend);
  }

  if (shift != 0) {
    __ Sra(TMP, TMP, shift);
  }

  if (instruction->IsDiv()) {
    __ Sra(out, TMP, 31);
    __ Subu(out, TMP, out);
  } else {
    __ Sra(AT, TMP, 31);
    __ Subu(AT, TMP, AT);
    __ LoadConst32(TMP, imm);
    if (isR6) {
      __ MulR6(TMP, AT, TMP);
    } else {
      __ MulR2(TMP, AT, TMP);
    }
    __ Subu(out, dividend, TMP);
  }
}

void InstructionCodeGeneratorMIPS::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimInt);

  LocationSummary* locations = instruction->GetLocations();
  Register out = locations->Out().AsRegister<Register>();
  Location second = locations->InAt(1);

  if (second.IsConstant()) {
    int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();
    if (imm == 0) {
      // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    } else if (imm == 1 || imm == -1) {
      DivRemOneOrMinusOne(instruction);
    } else if (IsPowerOfTwo(AbsOrMin(imm))) {
      DivRemByPowerOfTwo(instruction);
    } else {
      DCHECK(imm <= -2 || imm >= 2);
      GenerateDivRemWithAnyConstant(instruction);
    }
  } else {
    Register dividend = locations->InAt(0).AsRegister<Register>();
    Register divisor = second.AsRegister<Register>();
    bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();
    if (instruction->IsDiv()) {
      if (isR6) {
        __ DivR6(out, dividend, divisor);
      } else {
        __ DivR2(out, dividend, divisor);
      }
    } else {
      if (isR6) {
        __ ModR6(out, dividend, divisor);
      } else {
        __ ModR2(out, dividend, divisor);
      }
    }
  }
}

void LocationsBuilderMIPS::VisitDiv(HDiv* div) {
  Primitive::Type type = div->GetResultType();
  LocationSummary::CallKind call_kind = (type == Primitive::kPrimLong)
      ? LocationSummary::kCallOnMainOnly
      : LocationSummary::kNoCall;

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(div, call_kind);

  switch (type) {
    case Primitive::kPrimInt:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      locations->SetOut(calling_convention.GetReturnLocation(type));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void InstructionCodeGeneratorMIPS::VisitDiv(HDiv* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
      GenerateDivRemIntegral(instruction);
      break;
    case Primitive::kPrimLong: {
      codegen_->InvokeRuntime(kQuickLdiv, instruction, instruction->GetDexPc());
      CheckEntrypointTypes<kQuickLdiv, int64_t, int64_t, int64_t>();
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
      FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
      if (type == Primitive::kPrimFloat) {
        __ DivS(dst, lhs, rhs);
      } else {
        __ DivD(dst, lhs, rhs);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void LocationsBuilderMIPS::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
}

void InstructionCodeGeneratorMIPS::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) DivZeroCheckSlowPathMIPS(instruction);
  codegen_->AddSlowPath(slow_path);
  Location value = instruction->GetLocations()->InAt(0);
  Primitive::Type type = instruction->GetType();

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt: {
      if (value.IsConstant()) {
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
          __ B(slow_path->GetEntryLabel());
        } else {
          // A division by a non-null constant is valid. We don't need to perform
          // any check, so simply fall through.
        }
      } else {
        DCHECK(value.IsRegister()) << value;
        __ Beqz(value.AsRegister<Register>(), slow_path->GetEntryLabel());
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (value.IsConstant()) {
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
          __ B(slow_path->GetEntryLabel());
        } else {
          // A division by a non-null constant is valid. We don't need to perform
          // any check, so simply fall through.
        }
      } else {
        DCHECK(value.IsRegisterPair()) << value;
        __ Or(TMP, value.AsRegisterPairHigh<Register>(), value.AsRegisterPairLow<Register>());
        __ Beqz(TMP, slow_path->GetEntryLabel());
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type " << type << " for DivZeroCheck.";
  }
}

void LocationsBuilderMIPS::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS::VisitDoubleConstant(HDoubleConstant* cst ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS::VisitExit(HExit* exit ATTRIBUTE_UNUSED) {
}

void LocationsBuilderMIPS::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS::VisitFloatConstant(HFloatConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS::HandleGoto(HInstruction* got, HBasicBlock* successor) {
  DCHECK(!successor->IsExitBlock());
  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();
  HLoopInformation* info = block->GetLoopInformation();

  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
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

void InstructionCodeGeneratorMIPS::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderMIPS::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void InstructionCodeGeneratorMIPS::GenerateIntCompare(IfCondition cond,
                                                      LocationSummary* locations) {
  Register dst = locations->Out().AsRegister<Register>();
  Register lhs = locations->InAt(0).AsRegister<Register>();
  Location rhs_location = locations->InAt(1);
  Register rhs_reg = ZERO;
  int64_t rhs_imm = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
  } else {
    rhs_reg = rhs_location.AsRegister<Register>();
  }

  switch (cond) {
    case kCondEQ:
    case kCondNE:
      if (use_imm && IsUint<16>(rhs_imm)) {
        __ Xori(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Xor(dst, lhs, rhs_reg);
      }
      if (cond == kCondEQ) {
        __ Sltiu(dst, dst, 1);
      } else {
        __ Sltu(dst, ZERO, dst);
      }
      break;

    case kCondLT:
    case kCondGE:
      if (use_imm && IsInt<16>(rhs_imm)) {
        __ Slti(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Slt(dst, lhs, rhs_reg);
      }
      if (cond == kCondGE) {
        // Simulate lhs >= rhs via !(lhs < rhs) since there's
        // only the slt instruction but no sge.
        __ Xori(dst, dst, 1);
      }
      break;

    case kCondLE:
    case kCondGT:
      if (use_imm && IsInt<16>(rhs_imm + 1)) {
        // Simulate lhs <= rhs via lhs < rhs + 1.
        __ Slti(dst, lhs, rhs_imm + 1);
        if (cond == kCondGT) {
          // Simulate lhs > rhs via !(lhs <= rhs) since there's
          // only the slti instruction but no sgti.
          __ Xori(dst, dst, 1);
        }
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Slt(dst, rhs_reg, lhs);
        if (cond == kCondLE) {
          // Simulate lhs <= rhs via !(rhs < lhs) since there's
          // only the slt instruction but no sle.
          __ Xori(dst, dst, 1);
        }
      }
      break;

    case kCondB:
    case kCondAE:
      if (use_imm && IsInt<16>(rhs_imm)) {
        // Sltiu sign-extends its 16-bit immediate operand before
        // the comparison and thus lets us compare directly with
        // unsigned values in the ranges [0, 0x7fff] and
        // [0xffff8000, 0xffffffff].
        __ Sltiu(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Sltu(dst, lhs, rhs_reg);
      }
      if (cond == kCondAE) {
        // Simulate lhs >= rhs via !(lhs < rhs) since there's
        // only the sltu instruction but no sgeu.
        __ Xori(dst, dst, 1);
      }
      break;

    case kCondBE:
    case kCondA:
      if (use_imm && (rhs_imm != -1) && IsInt<16>(rhs_imm + 1)) {
        // Simulate lhs <= rhs via lhs < rhs + 1.
        // Note that this only works if rhs + 1 does not overflow
        // to 0, hence the check above.
        // Sltiu sign-extends its 16-bit immediate operand before
        // the comparison and thus lets us compare directly with
        // unsigned values in the ranges [0, 0x7fff] and
        // [0xffff8000, 0xffffffff].
        __ Sltiu(dst, lhs, rhs_imm + 1);
        if (cond == kCondA) {
          // Simulate lhs > rhs via !(lhs <= rhs) since there's
          // only the sltiu instruction but no sgtiu.
          __ Xori(dst, dst, 1);
        }
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Sltu(dst, rhs_reg, lhs);
        if (cond == kCondBE) {
          // Simulate lhs <= rhs via !(rhs < lhs) since there's
          // only the sltu instruction but no sleu.
          __ Xori(dst, dst, 1);
        }
      }
      break;
  }
}

void InstructionCodeGeneratorMIPS::GenerateIntCompareAndBranch(IfCondition cond,
                                                               LocationSummary* locations,
                                                               MipsLabel* label) {
  Register lhs = locations->InAt(0).AsRegister<Register>();
  Location rhs_location = locations->InAt(1);
  Register rhs_reg = ZERO;
  int32_t rhs_imm = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
  } else {
    rhs_reg = rhs_location.AsRegister<Register>();
  }

  if (use_imm && rhs_imm == 0) {
    switch (cond) {
      case kCondEQ:
      case kCondBE:  // <= 0 if zero
        __ Beqz(lhs, label);
        break;
      case kCondNE:
      case kCondA:  // > 0 if non-zero
        __ Bnez(lhs, label);
        break;
      case kCondLT:
        __ Bltz(lhs, label);
        break;
      case kCondGE:
        __ Bgez(lhs, label);
        break;
      case kCondLE:
        __ Blez(lhs, label);
        break;
      case kCondGT:
        __ Bgtz(lhs, label);
        break;
      case kCondB:  // always false
        break;
      case kCondAE:  // always true
        __ B(label);
        break;
    }
  } else {
    if (use_imm) {
      // TODO: more efficient comparison with 16-bit constants without loading them into TMP.
      rhs_reg = TMP;
      __ LoadConst32(rhs_reg, rhs_imm);
    }
    switch (cond) {
      case kCondEQ:
        __ Beq(lhs, rhs_reg, label);
        break;
      case kCondNE:
        __ Bne(lhs, rhs_reg, label);
        break;
      case kCondLT:
        __ Blt(lhs, rhs_reg, label);
        break;
      case kCondGE:
        __ Bge(lhs, rhs_reg, label);
        break;
      case kCondLE:
        __ Bge(rhs_reg, lhs, label);
        break;
      case kCondGT:
        __ Blt(rhs_reg, lhs, label);
        break;
      case kCondB:
        __ Bltu(lhs, rhs_reg, label);
        break;
      case kCondAE:
        __ Bgeu(lhs, rhs_reg, label);
        break;
      case kCondBE:
        __ Bgeu(rhs_reg, lhs, label);
        break;
      case kCondA:
        __ Bltu(rhs_reg, lhs, label);
        break;
    }
  }
}

void InstructionCodeGeneratorMIPS::GenerateLongCompareAndBranch(IfCondition cond,
                                                                LocationSummary* locations,
                                                                MipsLabel* label) {
  Register lhs_high = locations->InAt(0).AsRegisterPairHigh<Register>();
  Register lhs_low = locations->InAt(0).AsRegisterPairLow<Register>();
  Location rhs_location = locations->InAt(1);
  Register rhs_high = ZERO;
  Register rhs_low = ZERO;
  int64_t imm = 0;
  uint32_t imm_high = 0;
  uint32_t imm_low = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    imm = rhs_location.GetConstant()->AsLongConstant()->GetValue();
    imm_high = High32Bits(imm);
    imm_low = Low32Bits(imm);
  } else {
    rhs_high = rhs_location.AsRegisterPairHigh<Register>();
    rhs_low = rhs_location.AsRegisterPairLow<Register>();
  }

  if (use_imm && imm == 0) {
    switch (cond) {
      case kCondEQ:
      case kCondBE:  // <= 0 if zero
        __ Or(TMP, lhs_high, lhs_low);
        __ Beqz(TMP, label);
        break;
      case kCondNE:
      case kCondA:  // > 0 if non-zero
        __ Or(TMP, lhs_high, lhs_low);
        __ Bnez(TMP, label);
        break;
      case kCondLT:
        __ Bltz(lhs_high, label);
        break;
      case kCondGE:
        __ Bgez(lhs_high, label);
        break;
      case kCondLE:
        __ Or(TMP, lhs_high, lhs_low);
        __ Sra(AT, lhs_high, 31);
        __ Bgeu(AT, TMP, label);
        break;
      case kCondGT:
        __ Or(TMP, lhs_high, lhs_low);
        __ Sra(AT, lhs_high, 31);
        __ Bltu(AT, TMP, label);
        break;
      case kCondB:  // always false
        break;
      case kCondAE:  // always true
        __ B(label);
        break;
    }
  } else if (use_imm) {
    // TODO: more efficient comparison with constants without loading them into TMP/AT.
    switch (cond) {
      case kCondEQ:
        __ LoadConst32(TMP, imm_high);
        __ Xor(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Xor(AT, AT, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondNE:
        __ LoadConst32(TMP, imm_high);
        __ Xor(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Xor(AT, AT, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Bnez(TMP, label);
        break;
      case kCondLT:
        __ LoadConst32(TMP, imm_high);
        __ Blt(lhs_high, TMP, label);
        __ Slt(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, lhs_low, AT);
        __ Blt(TMP, AT, label);
        break;
      case kCondGE:
        __ LoadConst32(TMP, imm_high);
        __ Blt(TMP, lhs_high, label);
        __ Slt(TMP, lhs_high, TMP);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, lhs_low, AT);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondLE:
        __ LoadConst32(TMP, imm_high);
        __ Blt(lhs_high, TMP, label);
        __ Slt(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, AT, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondGT:
        __ LoadConst32(TMP, imm_high);
        __ Blt(TMP, lhs_high, label);
        __ Slt(TMP, lhs_high, TMP);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, AT, lhs_low);
        __ Blt(TMP, AT, label);
        break;
      case kCondB:
        __ LoadConst32(TMP, imm_high);
        __ Bltu(lhs_high, TMP, label);
        __ Sltu(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, lhs_low, AT);
        __ Blt(TMP, AT, label);
        break;
      case kCondAE:
        __ LoadConst32(TMP, imm_high);
        __ Bltu(TMP, lhs_high, label);
        __ Sltu(TMP, lhs_high, TMP);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, lhs_low, AT);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondBE:
        __ LoadConst32(TMP, imm_high);
        __ Bltu(lhs_high, TMP, label);
        __ Sltu(TMP, TMP, lhs_high);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, AT, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondA:
        __ LoadConst32(TMP, imm_high);
        __ Bltu(TMP, lhs_high, label);
        __ Sltu(TMP, lhs_high, TMP);
        __ LoadConst32(AT, imm_low);
        __ Sltu(AT, AT, lhs_low);
        __ Blt(TMP, AT, label);
        break;
    }
  } else {
    switch (cond) {
      case kCondEQ:
        __ Xor(TMP, lhs_high, rhs_high);
        __ Xor(AT, lhs_low, rhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondNE:
        __ Xor(TMP, lhs_high, rhs_high);
        __ Xor(AT, lhs_low, rhs_low);
        __ Or(TMP, TMP, AT);
        __ Bnez(TMP, label);
        break;
      case kCondLT:
        __ Blt(lhs_high, rhs_high, label);
        __ Slt(TMP, rhs_high, lhs_high);
        __ Sltu(AT, lhs_low, rhs_low);
        __ Blt(TMP, AT, label);
        break;
      case kCondGE:
        __ Blt(rhs_high, lhs_high, label);
        __ Slt(TMP, lhs_high, rhs_high);
        __ Sltu(AT, lhs_low, rhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondLE:
        __ Blt(lhs_high, rhs_high, label);
        __ Slt(TMP, rhs_high, lhs_high);
        __ Sltu(AT, rhs_low, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondGT:
        __ Blt(rhs_high, lhs_high, label);
        __ Slt(TMP, lhs_high, rhs_high);
        __ Sltu(AT, rhs_low, lhs_low);
        __ Blt(TMP, AT, label);
        break;
      case kCondB:
        __ Bltu(lhs_high, rhs_high, label);
        __ Sltu(TMP, rhs_high, lhs_high);
        __ Sltu(AT, lhs_low, rhs_low);
        __ Blt(TMP, AT, label);
        break;
      case kCondAE:
        __ Bltu(rhs_high, lhs_high, label);
        __ Sltu(TMP, lhs_high, rhs_high);
        __ Sltu(AT, lhs_low, rhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondBE:
        __ Bltu(lhs_high, rhs_high, label);
        __ Sltu(TMP, rhs_high, lhs_high);
        __ Sltu(AT, rhs_low, lhs_low);
        __ Or(TMP, TMP, AT);
        __ Beqz(TMP, label);
        break;
      case kCondA:
        __ Bltu(rhs_high, lhs_high, label);
        __ Sltu(TMP, lhs_high, rhs_high);
        __ Sltu(AT, rhs_low, lhs_low);
        __ Blt(TMP, AT, label);
        break;
    }
  }
}

void InstructionCodeGeneratorMIPS::GenerateFpCompare(IfCondition cond,
                                                     bool gt_bias,
                                                     Primitive::Type type,
                                                     LocationSummary* locations) {
  Register dst = locations->Out().AsRegister<Register>();
  FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
  FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();
  if (type == Primitive::kPrimFloat) {
    if (isR6) {
      switch (cond) {
        case kCondEQ:
          __ CmpEqS(FTMP, lhs, rhs);
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondNE:
          __ CmpEqS(FTMP, lhs, rhs);
          __ Mfc1(dst, FTMP);
          __ Addiu(dst, dst, 1);
          break;
        case kCondLT:
          if (gt_bias) {
            __ CmpLtS(FTMP, lhs, rhs);
          } else {
            __ CmpUltS(FTMP, lhs, rhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondLE:
          if (gt_bias) {
            __ CmpLeS(FTMP, lhs, rhs);
          } else {
            __ CmpUleS(FTMP, lhs, rhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CmpUltS(FTMP, rhs, lhs);
          } else {
            __ CmpLtS(FTMP, rhs, lhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CmpUleS(FTMP, rhs, lhs);
          } else {
            __ CmpLeS(FTMP, rhs, lhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition " << cond;
          UNREACHABLE();
      }
    } else {
      switch (cond) {
        case kCondEQ:
          __ CeqS(0, lhs, rhs);
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondNE:
          __ CeqS(0, lhs, rhs);
          __ LoadConst32(dst, 1);
          __ Movt(dst, ZERO, 0);
          break;
        case kCondLT:
          if (gt_bias) {
            __ ColtS(0, lhs, rhs);
          } else {
            __ CultS(0, lhs, rhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondLE:
          if (gt_bias) {
            __ ColeS(0, lhs, rhs);
          } else {
            __ CuleS(0, lhs, rhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CultS(0, rhs, lhs);
          } else {
            __ ColtS(0, rhs, lhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CuleS(0, rhs, lhs);
          } else {
            __ ColeS(0, rhs, lhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition " << cond;
          UNREACHABLE();
      }
    }
  } else {
    DCHECK_EQ(type, Primitive::kPrimDouble);
    if (isR6) {
      switch (cond) {
        case kCondEQ:
          __ CmpEqD(FTMP, lhs, rhs);
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondNE:
          __ CmpEqD(FTMP, lhs, rhs);
          __ Mfc1(dst, FTMP);
          __ Addiu(dst, dst, 1);
          break;
        case kCondLT:
          if (gt_bias) {
            __ CmpLtD(FTMP, lhs, rhs);
          } else {
            __ CmpUltD(FTMP, lhs, rhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondLE:
          if (gt_bias) {
            __ CmpLeD(FTMP, lhs, rhs);
          } else {
            __ CmpUleD(FTMP, lhs, rhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CmpUltD(FTMP, rhs, lhs);
          } else {
            __ CmpLtD(FTMP, rhs, lhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CmpUleD(FTMP, rhs, lhs);
          } else {
            __ CmpLeD(FTMP, rhs, lhs);
          }
          __ Mfc1(dst, FTMP);
          __ Andi(dst, dst, 1);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition " << cond;
          UNREACHABLE();
      }
    } else {
      switch (cond) {
        case kCondEQ:
          __ CeqD(0, lhs, rhs);
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondNE:
          __ CeqD(0, lhs, rhs);
          __ LoadConst32(dst, 1);
          __ Movt(dst, ZERO, 0);
          break;
        case kCondLT:
          if (gt_bias) {
            __ ColtD(0, lhs, rhs);
          } else {
            __ CultD(0, lhs, rhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondLE:
          if (gt_bias) {
            __ ColeD(0, lhs, rhs);
          } else {
            __ CuleD(0, lhs, rhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CultD(0, rhs, lhs);
          } else {
            __ ColtD(0, rhs, lhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CuleD(0, rhs, lhs);
          } else {
            __ ColeD(0, rhs, lhs);
          }
          __ LoadConst32(dst, 1);
          __ Movf(dst, ZERO, 0);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition " << cond;
          UNREACHABLE();
      }
    }
  }
}

void InstructionCodeGeneratorMIPS::GenerateFpCompareAndBranch(IfCondition cond,
                                                              bool gt_bias,
                                                              Primitive::Type type,
                                                              LocationSummary* locations,
                                                              MipsLabel* label) {
  FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
  FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();
  if (type == Primitive::kPrimFloat) {
    if (isR6) {
      switch (cond) {
        case kCondEQ:
          __ CmpEqS(FTMP, lhs, rhs);
          __ Bc1nez(FTMP, label);
          break;
        case kCondNE:
          __ CmpEqS(FTMP, lhs, rhs);
          __ Bc1eqz(FTMP, label);
          break;
        case kCondLT:
          if (gt_bias) {
            __ CmpLtS(FTMP, lhs, rhs);
          } else {
            __ CmpUltS(FTMP, lhs, rhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondLE:
          if (gt_bias) {
            __ CmpLeS(FTMP, lhs, rhs);
          } else {
            __ CmpUleS(FTMP, lhs, rhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CmpUltS(FTMP, rhs, lhs);
          } else {
            __ CmpLtS(FTMP, rhs, lhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CmpUleS(FTMP, rhs, lhs);
          } else {
            __ CmpLeS(FTMP, rhs, lhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition";
      }
    } else {
      switch (cond) {
        case kCondEQ:
          __ CeqS(0, lhs, rhs);
          __ Bc1t(0, label);
          break;
        case kCondNE:
          __ CeqS(0, lhs, rhs);
          __ Bc1f(0, label);
          break;
        case kCondLT:
          if (gt_bias) {
            __ ColtS(0, lhs, rhs);
          } else {
            __ CultS(0, lhs, rhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondLE:
          if (gt_bias) {
            __ ColeS(0, lhs, rhs);
          } else {
            __ CuleS(0, lhs, rhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CultS(0, rhs, lhs);
          } else {
            __ ColtS(0, rhs, lhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CuleS(0, rhs, lhs);
          } else {
            __ ColeS(0, rhs, lhs);
          }
          __ Bc1t(0, label);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition";
      }
    }
  } else {
    DCHECK_EQ(type, Primitive::kPrimDouble);
    if (isR6) {
      switch (cond) {
        case kCondEQ:
          __ CmpEqD(FTMP, lhs, rhs);
          __ Bc1nez(FTMP, label);
          break;
        case kCondNE:
          __ CmpEqD(FTMP, lhs, rhs);
          __ Bc1eqz(FTMP, label);
          break;
        case kCondLT:
          if (gt_bias) {
            __ CmpLtD(FTMP, lhs, rhs);
          } else {
            __ CmpUltD(FTMP, lhs, rhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondLE:
          if (gt_bias) {
            __ CmpLeD(FTMP, lhs, rhs);
          } else {
            __ CmpUleD(FTMP, lhs, rhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CmpUltD(FTMP, rhs, lhs);
          } else {
            __ CmpLtD(FTMP, rhs, lhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CmpUleD(FTMP, rhs, lhs);
          } else {
            __ CmpLeD(FTMP, rhs, lhs);
          }
          __ Bc1nez(FTMP, label);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition";
      }
    } else {
      switch (cond) {
        case kCondEQ:
          __ CeqD(0, lhs, rhs);
          __ Bc1t(0, label);
          break;
        case kCondNE:
          __ CeqD(0, lhs, rhs);
          __ Bc1f(0, label);
          break;
        case kCondLT:
          if (gt_bias) {
            __ ColtD(0, lhs, rhs);
          } else {
            __ CultD(0, lhs, rhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondLE:
          if (gt_bias) {
            __ ColeD(0, lhs, rhs);
          } else {
            __ CuleD(0, lhs, rhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondGT:
          if (gt_bias) {
            __ CultD(0, rhs, lhs);
          } else {
            __ ColtD(0, rhs, lhs);
          }
          __ Bc1t(0, label);
          break;
        case kCondGE:
          if (gt_bias) {
            __ CuleD(0, rhs, lhs);
          } else {
            __ ColeD(0, rhs, lhs);
          }
          __ Bc1t(0, label);
          break;
        default:
          LOG(FATAL) << "Unexpected non-floating-point condition";
      }
    }
  }
}

void InstructionCodeGeneratorMIPS::GenerateTestAndBranch(HInstruction* instruction,
                                                         size_t condition_input_index,
                                                         MipsLabel* true_target,
                                                         MipsLabel* false_target) {
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ B(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
      if (false_target != nullptr) {
        __ B(false_target);
      }
    }
    return;
  }

  // The following code generates these patterns:
  //  (1) true_target == nullptr && false_target != nullptr
  //        - opposite condition true => branch to false_target
  //  (2) true_target != nullptr && false_target == nullptr
  //        - condition true => branch to true_target
  //  (3) true_target != nullptr && false_target != nullptr
  //        - condition true => branch to true_target
  //        - branch to false_target
  if (IsBooleanValueOrMaterializedCondition(cond)) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = instruction->GetLocations()->InAt(condition_input_index);
    DCHECK(cond_val.IsRegister());
    if (true_target == nullptr) {
      __ Beqz(cond_val.AsRegister<Register>(), false_target);
    } else {
      __ Bnez(cond_val.AsRegister<Register>(), true_target);
    }
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();
    Primitive::Type type = condition->InputAt(0)->GetType();
    LocationSummary* locations = cond->GetLocations();
    IfCondition if_cond = condition->GetCondition();
    MipsLabel* branch_target = true_target;

    if (true_target == nullptr) {
      if_cond = condition->GetOppositeCondition();
      branch_target = false_target;
    }

    switch (type) {
      default:
        GenerateIntCompareAndBranch(if_cond, locations, branch_target);
        break;
      case Primitive::kPrimLong:
        GenerateLongCompareAndBranch(if_cond, locations, branch_target);
        break;
      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        GenerateFpCompareAndBranch(if_cond, condition->IsGtBias(), type, locations, branch_target);
        break;
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ B(false_target);
  }
}

void LocationsBuilderMIPS::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  MipsLabel* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  MipsLabel* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index */ 0, true_target, false_target);
}

void LocationsBuilderMIPS::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeMIPS* slow_path =
      deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathMIPS>(deoptimize);
  GenerateTestAndBranch(deoptimize,
                        /* condition_input_index */ 0,
                        slow_path->GetEntryLabel(),
                        /* false_target */ nullptr);
}

void LocationsBuilderMIPS::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(select);
  if (Primitive::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RequiresRegister());
  }
  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorMIPS::VisitSelect(HSelect* select) {
  LocationSummary* locations = select->GetLocations();
  MipsLabel false_target;
  GenerateTestAndBranch(select,
                        /* condition_input_index */ 2,
                        /* true_target */ nullptr,
                        &false_target);
  codegen_->MoveLocation(locations->Out(), locations->InAt(1), select->GetType());
  __ Bind(&false_target);
}

void LocationsBuilderMIPS::VisitNativeDebugInfo(HNativeDebugInfo* info) {
  new (GetGraph()->GetArena()) LocationSummary(info);
}

void InstructionCodeGeneratorMIPS::VisitNativeDebugInfo(HNativeDebugInfo*) {
  // MaybeRecordNativeDebugInfo is already called implicitly in CodeGenerator::Compile.
}

void CodeGeneratorMIPS::GenerateNop() {
  __ Nop();
}

void LocationsBuilderMIPS::HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info) {
  Primitive::Type field_type = field_info.GetFieldType();
  bool is_wide = (field_type == Primitive::kPrimLong) || (field_type == Primitive::kPrimDouble);
  bool generate_volatile = field_info.IsVolatile() && is_wide;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, generate_volatile ? LocationSummary::kCallOnMainOnly : LocationSummary::kNoCall);

  locations->SetInAt(0, Location::RequiresRegister());
  if (generate_volatile) {
    InvokeRuntimeCallingConvention calling_convention;
    // need A0 to hold base + offset
    locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    if (field_type == Primitive::kPrimLong) {
      locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimLong));
    } else {
      // Use Location::Any() to prevent situations when running out of available fp registers.
      locations->SetOut(Location::Any());
      // Need some temp core regs since FP results are returned in core registers
      Location reg = calling_convention.GetReturnLocation(Primitive::kPrimLong);
      locations->AddTemp(Location::RegisterLocation(reg.AsRegisterPairLow<Register>()));
      locations->AddTemp(Location::RegisterLocation(reg.AsRegisterPairHigh<Register>()));
    }
  } else {
    if (Primitive::IsFloatingPointType(instruction->GetType())) {
      locations->SetOut(Location::RequiresFpuRegister());
    } else {
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
    }
  }
}

void InstructionCodeGeneratorMIPS::HandleFieldGet(HInstruction* instruction,
                                                  const FieldInfo& field_info,
                                                  uint32_t dex_pc) {
  Primitive::Type type = field_info.GetFieldType();
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  LoadOperandType load_type = kLoadUnsignedByte;
  bool is_volatile = field_info.IsVolatile();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  auto null_checker = GetImplicitNullChecker(instruction);

  switch (type) {
    case Primitive::kPrimBoolean:
      load_type = kLoadUnsignedByte;
      break;
    case Primitive::kPrimByte:
      load_type = kLoadSignedByte;
      break;
    case Primitive::kPrimShort:
      load_type = kLoadSignedHalfword;
      break;
    case Primitive::kPrimChar:
      load_type = kLoadUnsignedHalfword;
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
    case Primitive::kPrimNot:
      load_type = kLoadWord;
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      load_type = kLoadDoubleword;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }

  if (is_volatile && load_type == kLoadDoubleword) {
    InvokeRuntimeCallingConvention calling_convention;
    __ Addiu32(locations->GetTemp(0).AsRegister<Register>(), obj, offset);
    // Do implicit Null check
    __ Lw(ZERO, locations->GetTemp(0).AsRegister<Register>(), 0);
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
    codegen_->InvokeRuntime(kQuickA64Load, instruction, dex_pc);
    CheckEntrypointTypes<kQuickA64Load, int64_t, volatile const int64_t*>();
    if (type == Primitive::kPrimDouble) {
      // FP results are returned in core registers. Need to move them.
      Location out = locations->Out();
      if (out.IsFpuRegister()) {
        __ Mtc1(locations->GetTemp(1).AsRegister<Register>(), out.AsFpuRegister<FRegister>());
        __ MoveToFpuHigh(locations->GetTemp(2).AsRegister<Register>(),
                         out.AsFpuRegister<FRegister>());
      } else {
        DCHECK(out.IsDoubleStackSlot());
        __ StoreToOffset(kStoreWord,
                         locations->GetTemp(1).AsRegister<Register>(),
                         SP,
                         out.GetStackIndex());
        __ StoreToOffset(kStoreWord,
                         locations->GetTemp(2).AsRegister<Register>(),
                         SP,
                         out.GetStackIndex() + 4);
      }
    }
  } else {
    if (!Primitive::IsFloatingPointType(type)) {
      Register dst;
      if (type == Primitive::kPrimLong) {
        DCHECK(locations->Out().IsRegisterPair());
        dst = locations->Out().AsRegisterPairLow<Register>();
      } else {
        DCHECK(locations->Out().IsRegister());
        dst = locations->Out().AsRegister<Register>();
      }
      __ LoadFromOffset(load_type, dst, obj, offset, null_checker);
    } else {
      DCHECK(locations->Out().IsFpuRegister());
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      if (type == Primitive::kPrimFloat) {
        __ LoadSFromOffset(dst, obj, offset, null_checker);
      } else {
        __ LoadDFromOffset(dst, obj, offset, null_checker);
      }
    }
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
  }
}

void LocationsBuilderMIPS::HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info) {
  Primitive::Type field_type = field_info.GetFieldType();
  bool is_wide = (field_type == Primitive::kPrimLong) || (field_type == Primitive::kPrimDouble);
  bool generate_volatile = field_info.IsVolatile() && is_wide;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, generate_volatile ? LocationSummary::kCallOnMainOnly : LocationSummary::kNoCall);

  locations->SetInAt(0, Location::RequiresRegister());
  if (generate_volatile) {
    InvokeRuntimeCallingConvention calling_convention;
    // need A0 to hold base + offset
    locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    if (field_type == Primitive::kPrimLong) {
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
    } else {
      // Use Location::Any() to prevent situations when running out of available fp registers.
      locations->SetInAt(1, Location::Any());
      // Pass FP parameters in core registers.
      locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
      locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
    }
  } else {
    if (Primitive::IsFloatingPointType(field_type)) {
      locations->SetInAt(1, FpuRegisterOrConstantForStore(instruction->InputAt(1)));
    } else {
      locations->SetInAt(1, RegisterOrZeroConstant(instruction->InputAt(1)));
    }
  }
}

void InstructionCodeGeneratorMIPS::HandleFieldSet(HInstruction* instruction,
                                                  const FieldInfo& field_info,
                                                  uint32_t dex_pc) {
  Primitive::Type type = field_info.GetFieldType();
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location value_location = locations->InAt(1);
  StoreOperandType store_type = kStoreByte;
  bool is_volatile = field_info.IsVolatile();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  auto null_checker = GetImplicitNullChecker(instruction);

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      store_type = kStoreByte;
      break;
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
      store_type = kStoreHalfword;
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
    case Primitive::kPrimNot:
      store_type = kStoreWord;
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      store_type = kStoreDoubleword;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  if (is_volatile && store_type == kStoreDoubleword) {
    InvokeRuntimeCallingConvention calling_convention;
    __ Addiu32(locations->GetTemp(0).AsRegister<Register>(), obj, offset);
    // Do implicit Null check.
    __ Lw(ZERO, locations->GetTemp(0).AsRegister<Register>(), 0);
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
    if (type == Primitive::kPrimDouble) {
      // Pass FP parameters in core registers.
      if (value_location.IsFpuRegister()) {
        __ Mfc1(locations->GetTemp(1).AsRegister<Register>(),
                value_location.AsFpuRegister<FRegister>());
        __ MoveFromFpuHigh(locations->GetTemp(2).AsRegister<Register>(),
                           value_location.AsFpuRegister<FRegister>());
      } else if (value_location.IsDoubleStackSlot()) {
        __ LoadFromOffset(kLoadWord,
                          locations->GetTemp(1).AsRegister<Register>(),
                          SP,
                          value_location.GetStackIndex());
        __ LoadFromOffset(kLoadWord,
                          locations->GetTemp(2).AsRegister<Register>(),
                          SP,
                          value_location.GetStackIndex() + 4);
      } else {
        DCHECK(value_location.IsConstant());
        DCHECK(value_location.GetConstant()->IsDoubleConstant());
        int64_t value = CodeGenerator::GetInt64ValueOf(value_location.GetConstant());
        __ LoadConst64(locations->GetTemp(2).AsRegister<Register>(),
                       locations->GetTemp(1).AsRegister<Register>(),
                       value);
      }
    }
    codegen_->InvokeRuntime(kQuickA64Store, instruction, dex_pc);
    CheckEntrypointTypes<kQuickA64Store, void, volatile int64_t *, int64_t>();
  } else {
    if (value_location.IsConstant()) {
      int64_t value = CodeGenerator::GetInt64ValueOf(value_location.GetConstant());
      __ StoreConstToOffset(store_type, value, obj, offset, TMP, null_checker);
    } else if (!Primitive::IsFloatingPointType(type)) {
      Register src;
      if (type == Primitive::kPrimLong) {
        src = value_location.AsRegisterPairLow<Register>();
      } else {
        src = value_location.AsRegister<Register>();
      }
      __ StoreToOffset(store_type, src, obj, offset, null_checker);
    } else {
      FRegister src = value_location.AsFpuRegister<FRegister>();
      if (type == Primitive::kPrimFloat) {
        __ StoreSToOffset(src, obj, offset, null_checker);
      } else {
        __ StoreDToOffset(src, obj, offset, null_checker);
      }
    }
  }

  // TODO: memory barriers?
  if (CodeGenerator::StoreNeedsWriteBarrier(type, instruction->InputAt(1))) {
    Register src = value_location.AsRegister<Register>();
    codegen_->MarkGCCard(obj, src);
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
  }
}

void LocationsBuilderMIPS::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo(), instruction->GetDexPc());
}

void LocationsBuilderMIPS::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetDexPc());
}

void InstructionCodeGeneratorMIPS::GenerateGcRootFieldLoad(
    HInstruction* instruction ATTRIBUTE_UNUSED,
    Location root,
    Register obj,
    uint32_t offset) {
  Register root_reg = root.AsRegister<Register>();
  if (kEmitCompilerReadBarrier) {
    UNIMPLEMENTED(FATAL) << "for read barrier";
  } else {
    // Plain GC root load with no read barrier.
    // /* GcRoot<mirror::Object> */ root = *(obj + offset)
    __ LoadFromOffset(kLoadWord, root_reg, obj, offset);
    // Note that GC roots are not affected by heap poisoning, thus we
    // do not have to unpoison `root_reg` here.
  }
}

void LocationsBuilderMIPS::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind =
      instruction->IsExactCheck() ? LocationSummary::kNoCall : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The output does overlap inputs.
  // Note that TypeCheckSlowPathMIPS uses this register too.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void InstructionCodeGeneratorMIPS::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register cls = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  MipsLabel done;

  // Return 0 if `obj` is null.
  // TODO: Avoid this check if we know `obj` is not null.
  __ Move(out, ZERO);
  __ Beqz(obj, &done);

  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, out, obj, mirror::Object::ClassOffset().Int32Value());
  if (instruction->IsExactCheck()) {
    // Classes must be equal for the instanceof to succeed.
    __ Xor(out, out, cls);
    __ Sltiu(out, out, 1);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS(instruction);
    codegen_->AddSlowPath(slow_path);
    __ Bne(out, cls, slow_path->GetEntryLabel());
    __ LoadConst32(out, 1);
    __ Bind(slow_path->GetExitLabel());
  }

  __ Bind(&done);
}

void LocationsBuilderMIPS::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS::VisitIntConstant(HIntConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS::VisitNullConstant(HNullConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorMIPS calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
}

void LocationsBuilderMIPS::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // The register T0 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(T0));
}

void InstructionCodeGeneratorMIPS::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  Location receiver = invoke->GetLocations()->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMipsPointerSize);

  // Set the hidden argument.
  __ LoadConst32(invoke->GetLocations()->GetTemp(1).AsRegister<Register>(),
                 invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadWord, temp, receiver.AsRegister<Register>(), class_offset);
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ LoadFromOffset(kLoadWord, temp, temp,
      mirror::Class::ImtPtrOffset(kMipsPointerSize).Uint32Value());
  uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
      invoke->GetImtIndex(), kMipsPointerSize));
  // temp = temp->GetImtEntryAt(method_offset);
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  __ NopIfNoReordering();
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderMIPS intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void LocationsBuilderMIPS::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  HInvokeStaticOrDirect::MethodLoadKind method_load_kind = invoke->GetMethodLoadKind();
  HInvokeStaticOrDirect::CodePtrLocation code_ptr_location = invoke->GetCodePtrLocation();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  // kDirectAddressWithFixup and kCallDirectWithFixup need no extra input on R6 because
  // R6 has PC-relative addressing.
  bool has_extra_input = !isR6 &&
      ((method_load_kind == HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup) ||
       (code_ptr_location == HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup));

  if (invoke->HasPcRelativeDexCache()) {
    // kDexCachePcRelative is mutually exclusive with
    // kDirectAddressWithFixup/kCallDirectWithFixup.
    CHECK(!has_extra_input);
    has_extra_input = true;
  }

  IntrinsicLocationsBuilderMIPS intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    if (invoke->GetLocations()->CanCall() && has_extra_input) {
      invoke->GetLocations()->SetInAt(invoke->GetSpecialInputIndex(), Location::Any());
    }
    return;
  }

  HandleInvoke(invoke);

  // Add the extra input register if either the dex cache array base register
  // or the PC-relative base register for accessing literals is needed.
  if (has_extra_input) {
    invoke->GetLocations()->SetInAt(invoke->GetSpecialInputIndex(), Location::RequiresRegister());
  }
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorMIPS* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorMIPS intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

HLoadString::LoadKind CodeGeneratorMIPS::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind) {
  if (kEmitCompilerReadBarrier) {
    UNIMPLEMENTED(FATAL) << "for read barrier";
  }
  // We disable PC-relative load when there is an irreducible loop, as the optimization
  // is incompatible with it.
  bool has_irreducible_loops = GetGraph()->HasIrreducibleLoops();
  bool fallback_load = has_irreducible_loops;
  switch (desired_string_load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!GetCompilerOptions().GetCompilePic());
      break;
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().GetCompilePic());
      break;
    case HLoadString::LoadKind::kBootImageAddress:
      break;
    case HLoadString::LoadKind::kDexCacheAddress:
      DCHECK(Runtime::Current()->UseJitCompilation());
      fallback_load = false;
      break;
    case HLoadString::LoadKind::kDexCachePcRelative:
      DCHECK(!Runtime::Current()->UseJitCompilation());
      // TODO: Create as many MipsDexCacheArraysBase instructions as needed for methods
      // with irreducible loops.
      break;
    case HLoadString::LoadKind::kDexCacheViaMethod:
      fallback_load = false;
      break;
  }
  if (fallback_load) {
    desired_string_load_kind = HLoadString::LoadKind::kDexCacheViaMethod;
  }
  return desired_string_load_kind;
}

HLoadClass::LoadKind CodeGeneratorMIPS::GetSupportedLoadClassKind(
    HLoadClass::LoadKind desired_class_load_kind) {
  if (kEmitCompilerReadBarrier) {
    UNIMPLEMENTED(FATAL) << "for read barrier";
  }
  // We disable pc-relative load when there is an irreducible loop, as the optimization
  // is incompatible with it.
  bool has_irreducible_loops = GetGraph()->HasIrreducibleLoops();
  bool fallback_load = has_irreducible_loops;
  switch (desired_class_load_kind) {
    case HLoadClass::LoadKind::kReferrersClass:
      fallback_load = false;
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!GetCompilerOptions().GetCompilePic());
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().GetCompilePic());
      break;
    case HLoadClass::LoadKind::kBootImageAddress:
      break;
    case HLoadClass::LoadKind::kDexCacheAddress:
      DCHECK(Runtime::Current()->UseJitCompilation());
      fallback_load = false;
      break;
    case HLoadClass::LoadKind::kDexCachePcRelative:
      DCHECK(!Runtime::Current()->UseJitCompilation());
      // TODO: Create as many MipsDexCacheArraysBase instructions as needed for methods
      // with irreducible loops.
      break;
    case HLoadClass::LoadKind::kDexCacheViaMethod:
      fallback_load = false;
      break;
  }
  if (fallback_load) {
    desired_class_load_kind = HLoadClass::LoadKind::kDexCacheViaMethod;
  }
  return desired_class_load_kind;
}

Register CodeGeneratorMIPS::GetInvokeStaticOrDirectExtraParameter(HInvokeStaticOrDirect* invoke,
                                                                  Register temp) {
  CHECK_EQ(invoke->InputCount(), invoke->GetNumberOfArguments() + 1u);
  Location location = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
  if (!invoke->GetLocations()->Intrinsified()) {
    return location.AsRegister<Register>();
  }
  // For intrinsics we allow any location, so it may be on the stack.
  if (!location.IsRegister()) {
    __ LoadFromOffset(kLoadWord, temp, SP, location.GetStackIndex());
    return temp;
  }
  // For register locations, check if the register was saved. If so, get it from the stack.
  // Note: There is a chance that the register was saved but not overwritten, so we could
  // save one load. However, since this is just an intrinsic slow path we prefer this
  // simple and more robust approach rather that trying to determine if that's the case.
  SlowPathCode* slow_path = GetCurrentSlowPath();
  DCHECK(slow_path != nullptr);  // For intrinsified invokes the call is emitted on the slow path.
  if (slow_path->IsCoreRegisterSaved(location.AsRegister<Register>())) {
    int stack_offset = slow_path->GetStackOffsetOfCoreRegister(location.AsRegister<Register>());
    __ LoadFromOffset(kLoadWord, temp, SP, stack_offset);
    return temp;
  }
  return location.AsRegister<Register>();
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorMIPS::GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      HInvokeStaticOrDirect* invoke ATTRIBUTE_UNUSED) {
  HInvokeStaticOrDirect::DispatchInfo dispatch_info = desired_dispatch_info;
  // We disable PC-relative load when there is an irreducible loop, as the optimization
  // is incompatible with it.
  bool has_irreducible_loops = GetGraph()->HasIrreducibleLoops();
  bool fallback_load = true;
  bool fallback_call = true;
  switch (dispatch_info.method_load_kind) {
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative:
      fallback_load = has_irreducible_loops;
      break;
    default:
      fallback_load = false;
      break;
  }
  switch (dispatch_info.code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
      fallback_call = has_irreducible_loops;
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative:
      // TODO: Implement this type.
      break;
    default:
      fallback_call = false;
      break;
  }
  if (fallback_load) {
    dispatch_info.method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod;
    dispatch_info.method_load_data = 0;
  }
  if (fallback_call) {
    dispatch_info.code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod;
    dispatch_info.direct_code_ptr = 0;
  }
  return dispatch_info;
}

void CodeGeneratorMIPS::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) {
  // All registers are assumed to be correctly set up per the calling convention.
  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  HInvokeStaticOrDirect::MethodLoadKind method_load_kind = invoke->GetMethodLoadKind();
  HInvokeStaticOrDirect::CodePtrLocation code_ptr_location = invoke->GetCodePtrLocation();
  bool isR6 = isa_features_.IsR6();
  // kDirectAddressWithFixup and kCallDirectWithFixup have no extra input on R6 because
  // R6 has PC-relative addressing.
  bool has_extra_input = invoke->HasPcRelativeDexCache() ||
      (!isR6 &&
       ((method_load_kind == HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup) ||
        (code_ptr_location == HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup)));
  Register base_reg = has_extra_input
      ? GetInvokeStaticOrDirectExtraParameter(invoke, temp.AsRegister<Register>())
      : ZERO;

  // For better instruction scheduling we load the direct code pointer before the method pointer.
  switch (code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // T9 = invoke->GetDirectCodePtr();
      __ LoadConst32(T9, invoke->GetDirectCodePtr());
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
      // T9 = code address from literal pool with link-time patch.
      __ LoadLiteral(T9, base_reg, DeduplicateMethodCodeLiteral(invoke->GetTargetMethod()));
      break;
    default:
      break;
  }

  switch (method_load_kind) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit: {
      // temp = thread->string_init_entrypoint
      uint32_t offset =
          GetThreadOffset<kMipsPointerSize>(invoke->GetStringInitEntryPoint()).Int32Value();
      __ LoadFromOffset(kLoadWord,
                        temp.AsRegister<Register>(),
                        TR,
                        offset);
      break;
    }
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      __ LoadConst32(temp.AsRegister<Register>(), invoke->GetMethodAddress());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
      __ LoadLiteral(temp.AsRegister<Register>(),
                     base_reg,
                     DeduplicateMethodAddressLiteral(invoke->GetTargetMethod()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative: {
      HMipsDexCacheArraysBase* base =
          invoke->InputAt(invoke->GetSpecialInputIndex())->AsMipsDexCacheArraysBase();
      int32_t offset =
          invoke->GetDexCacheArrayOffset() - base->GetElementOffset() - kDexCacheArrayLwOffset;
      __ LoadFromOffset(kLoadWord, temp.AsRegister<Register>(), base_reg, offset);
      break;
    }
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod: {
      Location current_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      Register reg = temp.AsRegister<Register>();
      Register method_reg;
      if (current_method.IsRegister()) {
        method_reg = current_method.AsRegister<Register>();
      } else {
        // TODO: use the appropriate DCHECK() here if possible.
        // DCHECK(invoke->GetLocations()->Intrinsified());
        DCHECK(!current_method.IsValid());
        method_reg = reg;
        __ Lw(reg, SP, kCurrentMethodStackOffset);
      }

      // temp = temp->dex_cache_resolved_methods_;
      __ LoadFromOffset(kLoadWord,
                        reg,
                        method_reg,
                        ArtMethod::DexCacheResolvedMethodsOffset(kMipsPointerSize).Int32Value());
      // temp = temp[index_in_cache];
      // Note: Don't use invoke->GetTargetMethod() as it may point to a different dex file.
      uint32_t index_in_cache = invoke->GetDexMethodIndex();
      __ LoadFromOffset(kLoadWord,
                        reg,
                        reg,
                        CodeGenerator::GetCachePointerOffset(index_in_cache));
      break;
    }
  }

  switch (code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallSelf:
      __ Bal(&frame_entry_label_);
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
      // T9 prepared above for better instruction scheduling.
      // T9()
      __ Jalr(T9);
      __ NopIfNoReordering();
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative:
      // TODO: Implement this type.
      // Currently filtered out by GetSupportedInvokeStaticOrDirectDispatch().
      LOG(FATAL) << "Unsupported";
      UNREACHABLE();
    case HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod:
      // T9 = callee_method->entry_point_from_quick_compiled_code_;
      __ LoadFromOffset(kLoadWord,
                        T9,
                        callee_method.AsRegister<Register>(),
                        ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                            kMipsPointerSize).Int32Value());
      // T9()
      __ Jalr(T9);
      __ NopIfNoReordering();
      break;
  }
  DCHECK(!IsLeafMethod());
}

void InstructionCodeGeneratorMIPS::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(invoke,
                                       locations->HasTemps()
                                           ? locations->GetTemp(0)
                                           : Location::NoLocation());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void CodeGeneratorMIPS::GenerateVirtualCall(HInvokeVirtual* invoke, Location temp_location) {
  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);

  Register temp = temp_location.AsRegister<Register>();
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kMipsPointerSize).SizeValue();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMipsPointerSize);

  // temp = object->GetClass();
  __ LoadFromOffset(kLoadWord, temp, receiver, class_offset);
  MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  __ NopIfNoReordering();
}

void InstructionCodeGeneratorMIPS::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS::VisitLoadClass(HLoadClass* cls) {
  if (cls->NeedsAccessCheck()) {
    InvokeRuntimeCallingConvention calling_convention;
    CodeGenerator::CreateLoadClassLocationSummary(
        cls,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Location::RegisterLocation(V0),
        /* code_generator_supports_read_barrier */ false);  // TODO: revisit this bool.
    return;
  }

  LocationSummary::CallKind call_kind = (cls->NeedsEnvironment() || kEmitCompilerReadBarrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  switch (load_kind) {
    // We need an extra register for PC-relative literals on R2.
    case HLoadClass::LoadKind::kBootImageLinkTimeAddress:
    case HLoadClass::LoadKind::kBootImageAddress:
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
      if (codegen_->GetInstructionSetFeatures().IsR6()) {
        break;
      }
      FALLTHROUGH_INTENDED;
    // We need an extra register for PC-relative dex cache accesses.
    case HLoadClass::LoadKind::kDexCachePcRelative:
    case HLoadClass::LoadKind::kReferrersClass:
    case HLoadClass::LoadKind::kDexCacheViaMethod:
      locations->SetInAt(0, Location::RequiresRegister());
      break;
    default:
      break;
  }
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitLoadClass(HLoadClass* cls) {
  LocationSummary* locations = cls->GetLocations();
  if (cls->NeedsAccessCheck()) {
    codegen_->MoveConstant(locations->GetTemp(0), cls->GetTypeIndex());
    codegen_->InvokeRuntime(kQuickInitializeTypeAndVerifyAccess, cls, cls->GetDexPc());
    CheckEntrypointTypes<kQuickInitializeTypeAndVerifyAccess, void*, uint32_t>();
    return;
  }

  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  Location out_loc = locations->Out();
  Register out = out_loc.AsRegister<Register>();
  Register base_or_current_method_reg;
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();
  switch (load_kind) {
    // We need an extra register for PC-relative literals on R2.
    case HLoadClass::LoadKind::kBootImageLinkTimeAddress:
    case HLoadClass::LoadKind::kBootImageAddress:
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
      base_or_current_method_reg = isR6 ? ZERO : locations->InAt(0).AsRegister<Register>();
      break;
    // We need an extra register for PC-relative dex cache accesses.
    case HLoadClass::LoadKind::kDexCachePcRelative:
    case HLoadClass::LoadKind::kReferrersClass:
    case HLoadClass::LoadKind::kDexCacheViaMethod:
      base_or_current_method_reg = locations->InAt(0).AsRegister<Register>();
      break;
    default:
      base_or_current_method_reg = ZERO;
      break;
  }

  bool generate_null_check = false;
  switch (load_kind) {
    case HLoadClass::LoadKind::kReferrersClass: {
      DCHECK(!cls->CanCallRuntime());
      DCHECK(!cls->MustGenerateClinitCheck());
      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      GenerateGcRootFieldLoad(cls,
                              out_loc,
                              base_or_current_method_reg,
                              ArtMethod::DeclaringClassOffset().Int32Value());
      break;
    }
    case HLoadClass::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!kEmitCompilerReadBarrier);
      __ LoadLiteral(out,
                     base_or_current_method_reg,
                     codegen_->DeduplicateBootImageTypeLiteral(cls->GetDexFile(),
                                                               cls->GetTypeIndex()));
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(!kEmitCompilerReadBarrier);
      CodeGeneratorMIPS::PcRelativePatchInfo* info =
          codegen_->NewPcRelativeTypePatch(cls->GetDexFile(), cls->GetTypeIndex());
      bool reordering = __ SetReorder(false);
      if (isR6) {
        __ Bind(&info->high_label);
        __ Bind(&info->pc_rel_label);
        // Add a 32-bit offset to PC.
        __ Auipc(out, /* placeholder */ 0x1234);
        __ Addiu(out, out, /* placeholder */ 0x5678);
      } else {
        __ Bind(&info->high_label);
        __ Lui(out, /* placeholder */ 0x1234);
        // We do not bind info->pc_rel_label here, we'll use the assembler's label
        // for PC-relative literals and the base from HMipsComputeBaseMethodAddress.
        __ Ori(out, out, /* placeholder */ 0x5678);
        // Add a 32-bit offset to PC.
        __ Addu(out, out, base_or_current_method_reg);
      }
      __ SetReorder(reordering);
      break;
    }
    case HLoadClass::LoadKind::kBootImageAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      DCHECK_NE(cls->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(cls->GetAddress());
      __ LoadLiteral(out,
                     base_or_current_method_reg,
                     codegen_->DeduplicateBootImageAddressLiteral(address));
      break;
    }
    case HLoadClass::LoadKind::kDexCacheAddress: {
      DCHECK_NE(cls->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(cls->GetAddress());
      static_assert(sizeof(GcRoot<mirror::Class>) == 4u, "Expected GC root to be 4 bytes.");
      DCHECK_ALIGNED(cls->GetAddress(), 4u);
      int16_t offset = Low16Bits(address);
      uint32_t base_address = address - offset;  // This accounts for offset sign extension.
      __ Lui(out, High16Bits(base_address));
      // /* GcRoot<mirror::Class> */ out = *(base_address + offset)
      GenerateGcRootFieldLoad(cls, out_loc, out, offset);
      generate_null_check = !cls->IsInDexCache();
      break;
    }
    case HLoadClass::LoadKind::kDexCachePcRelative: {
      HMipsDexCacheArraysBase* base = cls->InputAt(0)->AsMipsDexCacheArraysBase();
      int32_t offset =
          cls->GetDexCacheElementOffset() - base->GetElementOffset() - kDexCacheArrayLwOffset;
      // /* GcRoot<mirror::Class> */ out = *(dex_cache_arrays_base + offset)
      GenerateGcRootFieldLoad(cls, out_loc, base_or_current_method_reg, offset);
      generate_null_check = !cls->IsInDexCache();
      break;
    }
    case HLoadClass::LoadKind::kDexCacheViaMethod: {
      // /* GcRoot<mirror::Class>[] */ out =
      //        current_method.ptr_sized_fields_->dex_cache_resolved_types_
      __ LoadFromOffset(kLoadWord,
                        out,
                        base_or_current_method_reg,
                        ArtMethod::DexCacheResolvedTypesOffset(kArmPointerSize).Int32Value());
      // /* GcRoot<mirror::Class> */ out = out[type_index]
      size_t offset = CodeGenerator::GetCacheOffset(cls->GetTypeIndex());
      GenerateGcRootFieldLoad(cls, out_loc, out, offset);
      generate_null_check = !cls->IsInDexCache();
    }
  }

  if (generate_null_check || cls->MustGenerateClinitCheck()) {
    DCHECK(cls->CanCallRuntime());
    SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathMIPS(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    if (generate_null_check) {
      __ Beqz(out, slow_path->GetEntryLabel());
    }
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

static int32_t GetExceptionTlsOffset() {
  return Thread::ExceptionOffset<kMipsPointerSize>().Int32Value();
}

void LocationsBuilderMIPS::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitLoadException(HLoadException* load) {
  Register out = load->GetLocations()->Out().AsRegister<Register>();
  __ LoadFromOffset(kLoadWord, out, TR, GetExceptionTlsOffset());
}

void LocationsBuilderMIPS::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetArena()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorMIPS::VisitClearException(HClearException* clear ATTRIBUTE_UNUSED) {
  __ StoreToOffset(kStoreWord, ZERO, TR, GetExceptionTlsOffset());
}

void LocationsBuilderMIPS::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = (load->NeedsEnvironment() || kEmitCompilerReadBarrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load, call_kind);
  HLoadString::LoadKind load_kind = load->GetLoadKind();
  switch (load_kind) {
    // We need an extra register for PC-relative literals on R2.
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
    case HLoadString::LoadKind::kBootImageAddress:
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
      if (codegen_->GetInstructionSetFeatures().IsR6()) {
        break;
      }
      FALLTHROUGH_INTENDED;
    // We need an extra register for PC-relative dex cache accesses.
    case HLoadString::LoadKind::kDexCachePcRelative:
    case HLoadString::LoadKind::kDexCacheViaMethod:
      locations->SetInAt(0, Location::RequiresRegister());
      break;
    default:
      break;
  }
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitLoadString(HLoadString* load) {
  HLoadString::LoadKind load_kind = load->GetLoadKind();
  LocationSummary* locations = load->GetLocations();
  Location out_loc = locations->Out();
  Register out = out_loc.AsRegister<Register>();
  Register base_or_current_method_reg;
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();
  switch (load_kind) {
    // We need an extra register for PC-relative literals on R2.
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
    case HLoadString::LoadKind::kBootImageAddress:
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
      base_or_current_method_reg = isR6 ? ZERO : locations->InAt(0).AsRegister<Register>();
      break;
    default:
      base_or_current_method_reg = ZERO;
      break;
  }

  switch (load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!kEmitCompilerReadBarrier);
      __ LoadLiteral(out,
                     base_or_current_method_reg,
                     codegen_->DeduplicateBootImageStringLiteral(load->GetDexFile(),
                                                                 load->GetStringIndex()));
      return;  // No dex cache slow path.
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(!kEmitCompilerReadBarrier);
      CodeGeneratorMIPS::PcRelativePatchInfo* info =
          codegen_->NewPcRelativeStringPatch(load->GetDexFile(), load->GetStringIndex());
      bool reordering = __ SetReorder(false);
      if (isR6) {
        __ Bind(&info->high_label);
        __ Bind(&info->pc_rel_label);
        // Add a 32-bit offset to PC.
        __ Auipc(out, /* placeholder */ 0x1234);
        __ Addiu(out, out, /* placeholder */ 0x5678);
      } else {
        __ Bind(&info->high_label);
        __ Lui(out, /* placeholder */ 0x1234);
        // We do not bind info->pc_rel_label here, we'll use the assembler's label
        // for PC-relative literals and the base from HMipsComputeBaseMethodAddress.
        __ Ori(out, out, /* placeholder */ 0x5678);
        // Add a 32-bit offset to PC.
        __ Addu(out, out, base_or_current_method_reg);
      }
      __ SetReorder(reordering);
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kBootImageAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      DCHECK_NE(load->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(load->GetAddress());
      __ LoadLiteral(out,
                     base_or_current_method_reg,
                     codegen_->DeduplicateBootImageAddressLiteral(address));
      return;  // No dex cache slow path.
    }
    default:
      break;
  }

  // TODO: Re-add the compiler code to do string dex cache lookup again.
  SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathMIPS(load);
  codegen_->AddSlowPath(slow_path);
  __ B(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderMIPS::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS::VisitLongConstant(HLongConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorMIPS::VisitMonitorOperation(HMonitorOperation* instruction) {
  if (instruction->IsEnter()) {
    codegen_->InvokeRuntime(kQuickLockObject, instruction, instruction->GetDexPc());
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    codegen_->InvokeRuntime(kQuickUnlockObject, instruction, instruction->GetDexPc());
  }
  CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
}

void LocationsBuilderMIPS::VisitMul(HMul* mul) {
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

void InstructionCodeGeneratorMIPS::VisitMul(HMul* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  switch (type) {
    case Primitive::kPrimInt: {
      Register dst = locations->Out().AsRegister<Register>();
      Register lhs = locations->InAt(0).AsRegister<Register>();
      Register rhs = locations->InAt(1).AsRegister<Register>();

      if (isR6) {
        __ MulR6(dst, lhs, rhs);
      } else {
        __ MulR2(dst, lhs, rhs);
      }
      break;
    }
    case Primitive::kPrimLong: {
      Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
      Register dst_low = locations->Out().AsRegisterPairLow<Register>();
      Register lhs_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register lhs_low = locations->InAt(0).AsRegisterPairLow<Register>();
      Register rhs_high = locations->InAt(1).AsRegisterPairHigh<Register>();
      Register rhs_low = locations->InAt(1).AsRegisterPairLow<Register>();

      // Extra checks to protect caused by the existance of A1_A2.
      // The algorithm is wrong if dst_high is either lhs_lo or rhs_lo:
      // (e.g. lhs=a0_a1, rhs=a2_a3 and dst=a1_a2).
      DCHECK_NE(dst_high, lhs_low);
      DCHECK_NE(dst_high, rhs_low);

      // A_B * C_D
      // dst_hi:  [ low(A*D) + low(B*C) + hi(B*D) ]
      // dst_lo:  [ low(B*D) ]
      // Note: R2 and R6 MUL produce the low 32 bit of the multiplication result.

      if (isR6) {
        __ MulR6(TMP, lhs_high, rhs_low);
        __ MulR6(dst_high, lhs_low, rhs_high);
        __ Addu(dst_high, dst_high, TMP);
        __ MuhuR6(TMP, lhs_low, rhs_low);
        __ Addu(dst_high, dst_high, TMP);
        __ MulR6(dst_low, lhs_low, rhs_low);
      } else {
        __ MulR2(TMP, lhs_high, rhs_low);
        __ MulR2(dst_high, lhs_low, rhs_high);
        __ Addu(dst_high, dst_high, TMP);
        __ MultuR2(lhs_low, rhs_low);
        __ Mfhi(TMP);
        __ Addu(dst_high, dst_high, TMP);
        __ Mflo(dst_low);
      }
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      FRegister lhs = locations->InAt(0).AsFpuRegister<FRegister>();
      FRegister rhs = locations->InAt(1).AsFpuRegister<FRegister>();
      if (type == Primitive::kPrimFloat) {
        __ MulS(dst, lhs, rhs);
      } else {
        __ MulD(dst, lhs, rhs);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected mul type " << type;
  }
}

void LocationsBuilderMIPS::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
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

void InstructionCodeGeneratorMIPS::VisitNeg(HNeg* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt: {
      Register dst = locations->Out().AsRegister<Register>();
      Register src = locations->InAt(0).AsRegister<Register>();
      __ Subu(dst, ZERO, src);
      break;
    }
    case Primitive::kPrimLong: {
      Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
      Register dst_low = locations->Out().AsRegisterPairLow<Register>();
      Register src_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register src_low = locations->InAt(0).AsRegisterPairLow<Register>();
      __ Subu(dst_low, ZERO, src_low);
      __ Sltu(TMP, ZERO, dst_low);
      __ Subu(dst_high, ZERO, src_high);
      __ Subu(dst_high, dst_high, TMP);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      FRegister src = locations->InAt(0).AsFpuRegister<FRegister>();
      if (type == Primitive::kPrimFloat) {
        __ NegS(dst, src);
      } else {
        __ NegD(dst, src);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected neg type " << type;
  }
}

void LocationsBuilderMIPS::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorMIPS::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  Register current_method_register = calling_convention.GetRegisterAt(2);
  __ Lw(current_method_register, SP, kCurrentMethodStackOffset);
  // Move an uint16_t value to a register.
  __ LoadConst32(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(instruction->GetEntrypoint(), instruction, instruction->GetDexPc());
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck,
                       void*, uint32_t, int32_t, ArtMethod*>();
}

void LocationsBuilderMIPS::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  if (instruction->IsStringAlloc()) {
    locations->AddTemp(Location::RegisterLocation(kMethodRegisterArgument));
  } else {
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  }
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorMIPS::VisitNewInstance(HNewInstance* instruction) {
  if (instruction->IsStringAlloc()) {
    // String is allocated through StringFactory. Call NewEmptyString entry point.
    Register temp = instruction->GetLocations()->GetTemp(0).AsRegister<Register>();
    MemberOffset code_offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMipsPointerSize);
    __ LoadFromOffset(kLoadWord, temp, TR, QUICK_ENTRY_POINT(pNewEmptyString));
    __ LoadFromOffset(kLoadWord, T9, temp, code_offset.Int32Value());
    __ Jalr(T9);
    __ NopIfNoReordering();
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  } else {
    codegen_->InvokeRuntime(instruction->GetEntrypoint(), instruction, instruction->GetDexPc());
    CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
  }
}

void LocationsBuilderMIPS::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS::VisitNot(HNot* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt: {
      Register dst = locations->Out().AsRegister<Register>();
      Register src = locations->InAt(0).AsRegister<Register>();
      __ Nor(dst, src, ZERO);
      break;
    }

    case Primitive::kPrimLong: {
      Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
      Register dst_low = locations->Out().AsRegisterPairLow<Register>();
      Register src_high = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register src_low = locations->InAt(0).AsRegisterPairLow<Register>();
      __ Nor(dst_high, src_high, ZERO);
      __ Nor(dst_low, src_low, ZERO);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderMIPS::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  __ Xori(locations->Out().AsRegister<Register>(),
          locations->InAt(0).AsRegister<Register>(),
          1);
}

void LocationsBuilderMIPS::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
}

void CodeGeneratorMIPS::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  Location obj = instruction->GetLocations()->InAt(0);

  __ Lw(ZERO, obj.AsRegister<Register>(), 0);
  RecordPcInfo(instruction, instruction->GetDexPc());
}

void CodeGeneratorMIPS::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeMIPS* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathMIPS(instruction);
  AddSlowPath(slow_path);

  Location obj = instruction->GetLocations()->InAt(0);

  __ Beqz(obj.AsRegister<Register>(), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorMIPS::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
}

void LocationsBuilderMIPS::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS::VisitParallelMove(HParallelMove* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorMIPS::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderMIPS::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorMIPS::VisitParameterValue(HParameterValue* instruction
                                                         ATTRIBUTE_UNUSED) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderMIPS::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::RegisterLocation(kMethodRegisterArgument));
}

void InstructionCodeGeneratorMIPS::VisitCurrentMethod(HCurrentMethod* instruction
                                                        ATTRIBUTE_UNUSED) {
  // Nothing to do, the method is already at its location.
}

void LocationsBuilderMIPS::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorMIPS::VisitPhi(HPhi* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderMIPS::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  LocationSummary::CallKind call_kind =
      (type == Primitive::kPrimInt) ? LocationSummary::kNoCall : LocationSummary::kCallOnMainOnly;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (type) {
    case Primitive::kPrimInt:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      locations->SetOut(calling_convention.GetReturnLocation(type));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(calling_convention.GetReturnLocation(type));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorMIPS::VisitRem(HRem* instruction) {
  Primitive::Type type = instruction->GetType();

  switch (type) {
    case Primitive::kPrimInt:
      GenerateDivRemIntegral(instruction);
      break;
    case Primitive::kPrimLong: {
      codegen_->InvokeRuntime(kQuickLmod, instruction, instruction->GetDexPc());
      CheckEntrypointTypes<kQuickLmod, int64_t, int64_t, int64_t>();
      break;
    }
    case Primitive::kPrimFloat: {
      codegen_->InvokeRuntime(kQuickFmodf, instruction, instruction->GetDexPc());
      CheckEntrypointTypes<kQuickFmodf, float, float, float>();
      break;
    }
    case Primitive::kPrimDouble: {
      codegen_->InvokeRuntime(kQuickFmod, instruction, instruction->GetDexPc());
      CheckEntrypointTypes<kQuickFmod, double, double, double>();
      break;
    }
    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderMIPS::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderMIPS::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  Primitive::Type return_type = ret->InputAt(0)->GetType();
  locations->SetInAt(0, MipsReturnLocation(return_type));
}

void InstructionCodeGeneratorMIPS::VisitReturn(HReturn* ret ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderMIPS::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS::VisitReturnVoid(HReturnVoid* ret ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderMIPS::VisitRor(HRor* ror) {
  HandleShift(ror);
}

void InstructionCodeGeneratorMIPS::VisitRor(HRor* ror) {
  HandleShift(ror);
}

void LocationsBuilderMIPS::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorMIPS::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderMIPS::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorMIPS::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderMIPS::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo(), instruction->GetDexPc());
}

void LocationsBuilderMIPS::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetDexPc());
}

void LocationsBuilderMIPS::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(instruction,
                                                 instruction->GetFieldType(),
                                                 calling_convention);
}

void InstructionCodeGeneratorMIPS::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(instruction,
                                                 instruction->GetFieldType(),
                                                 calling_convention);
}

void InstructionCodeGeneratorMIPS::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(instruction,
                                                 instruction->GetFieldType(),
                                                 calling_convention);
}

void InstructionCodeGeneratorMIPS::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(instruction,
                                                 instruction->GetFieldType(),
                                                 calling_convention);
}

void InstructionCodeGeneratorMIPS::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionMIPS calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS::VisitSuspendCheck(HSuspendCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
  locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
}

void InstructionCodeGeneratorMIPS::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void LocationsBuilderMIPS::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorMIPS::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(kQuickDeliverException, instruction, instruction->GetDexPc());
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

void LocationsBuilderMIPS::VisitTypeConversion(HTypeConversion* conversion) {
  Primitive::Type input_type = conversion->GetInputType();
  Primitive::Type result_type = conversion->GetResultType();
  DCHECK_NE(input_type, result_type);
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  if ((input_type == Primitive::kPrimNot) || (input_type == Primitive::kPrimVoid) ||
      (result_type == Primitive::kPrimNot) || (result_type == Primitive::kPrimVoid)) {
    LOG(FATAL) << "Unexpected type conversion from " << input_type << " to " << result_type;
  }

  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  if (!isR6 &&
      ((Primitive::IsFloatingPointType(result_type) && input_type == Primitive::kPrimLong) ||
       (result_type == Primitive::kPrimLong && Primitive::IsFloatingPointType(input_type)))) {
    call_kind = LocationSummary::kCallOnMainOnly;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(conversion, call_kind);

  if (call_kind == LocationSummary::kNoCall) {
    if (Primitive::IsFloatingPointType(input_type)) {
      locations->SetInAt(0, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(0, Location::RequiresRegister());
    }

    if (Primitive::IsFloatingPointType(result_type)) {
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
    } else {
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
    }
  } else {
    InvokeRuntimeCallingConvention calling_convention;

    if (Primitive::IsFloatingPointType(input_type)) {
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
    } else {
      DCHECK_EQ(input_type, Primitive::kPrimLong);
      locations->SetInAt(0, Location::RegisterPairLocation(
                 calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
    }

    locations->SetOut(calling_convention.GetReturnLocation(result_type));
  }
}

void InstructionCodeGeneratorMIPS::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  bool has_sign_extension = codegen_->GetInstructionSetFeatures().IsMipsIsaRevGreaterThanEqual2();
  bool isR6 = codegen_->GetInstructionSetFeatures().IsR6();

  DCHECK_NE(input_type, result_type);

  if (result_type == Primitive::kPrimLong && Primitive::IsIntegralType(input_type)) {
    Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
    Register dst_low = locations->Out().AsRegisterPairLow<Register>();
    Register src = locations->InAt(0).AsRegister<Register>();

    if (dst_low != src) {
      __ Move(dst_low, src);
    }
    __ Sra(dst_high, src, 31);
  } else if (Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type)) {
    Register dst = locations->Out().AsRegister<Register>();
    Register src = (input_type == Primitive::kPrimLong)
        ? locations->InAt(0).AsRegisterPairLow<Register>()
        : locations->InAt(0).AsRegister<Register>();

    switch (result_type) {
      case Primitive::kPrimChar:
        __ Andi(dst, src, 0xFFFF);
        break;
      case Primitive::kPrimByte:
        if (has_sign_extension) {
          __ Seb(dst, src);
        } else {
          __ Sll(dst, src, 24);
          __ Sra(dst, dst, 24);
        }
        break;
      case Primitive::kPrimShort:
        if (has_sign_extension) {
          __ Seh(dst, src);
        } else {
          __ Sll(dst, src, 16);
          __ Sra(dst, dst, 16);
        }
        break;
      case Primitive::kPrimInt:
        if (dst != src) {
          __ Move(dst, src);
        }
        break;

      default:
        LOG(FATAL) << "Unexpected type conversion from " << input_type
                   << " to " << result_type;
    }
  } else if (Primitive::IsFloatingPointType(result_type) && Primitive::IsIntegralType(input_type)) {
    if (input_type == Primitive::kPrimLong) {
      if (isR6) {
        // cvt.s.l/cvt.d.l requires MIPSR2+ with FR=1. MIPS32R6 is implemented as a secondary
        // architecture on top of MIPS64R6, which has FR=1, and therefore can use the instruction.
        Register src_high = locations->InAt(0).AsRegisterPairHigh<Register>();
        Register src_low = locations->InAt(0).AsRegisterPairLow<Register>();
        FRegister dst = locations->Out().AsFpuRegister<FRegister>();
        __ Mtc1(src_low, FTMP);
        __ Mthc1(src_high, FTMP);
        if (result_type == Primitive::kPrimFloat) {
          __ Cvtsl(dst, FTMP);
        } else {
          __ Cvtdl(dst, FTMP);
        }
      } else {
        QuickEntrypointEnum entrypoint = (result_type == Primitive::kPrimFloat) ? kQuickL2f
                                                                                : kQuickL2d;
        codegen_->InvokeRuntime(entrypoint, conversion, conversion->GetDexPc());
        if (result_type == Primitive::kPrimFloat) {
          CheckEntrypointTypes<kQuickL2f, float, int64_t>();
        } else {
          CheckEntrypointTypes<kQuickL2d, double, int64_t>();
        }
      }
    } else {
      Register src = locations->InAt(0).AsRegister<Register>();
      FRegister dst = locations->Out().AsFpuRegister<FRegister>();
      __ Mtc1(src, FTMP);
      if (result_type == Primitive::kPrimFloat) {
        __ Cvtsw(dst, FTMP);
      } else {
        __ Cvtdw(dst, FTMP);
      }
    }
  } else if (Primitive::IsIntegralType(result_type) && Primitive::IsFloatingPointType(input_type)) {
    CHECK(result_type == Primitive::kPrimInt || result_type == Primitive::kPrimLong);
    if (result_type == Primitive::kPrimLong) {
      if (isR6) {
        // trunc.l.s/trunc.l.d requires MIPSR2+ with FR=1. MIPS32R6 is implemented as a secondary
        // architecture on top of MIPS64R6, which has FR=1, and therefore can use the instruction.
        FRegister src = locations->InAt(0).AsFpuRegister<FRegister>();
        Register dst_high = locations->Out().AsRegisterPairHigh<Register>();
        Register dst_low = locations->Out().AsRegisterPairLow<Register>();
        MipsLabel truncate;
        MipsLabel done;

        // When NAN2008=0 (R2 and before), the truncate instruction produces the maximum positive
        // value when the input is either a NaN or is outside of the range of the output type
        // after the truncation. IOW, the three special cases (NaN, too small, too big) produce
        // the same result.
        //
        // When NAN2008=1 (R6), the truncate instruction caps the output at the minimum/maximum
        // value of the output type if the input is outside of the range after the truncation or
        // produces 0 when the input is a NaN. IOW, the three special cases produce three distinct
        // results. This matches the desired float/double-to-int/long conversion exactly.
        //
        // So, NAN2008 affects handling of negative values and NaNs by the truncate instruction.
        //
        // The following code supports both NAN2008=0 and NAN2008=1 behaviors of the truncate
        // instruction, the reason being that the emulator implements NAN2008=0 on MIPS64R6,
        // even though it must be NAN2008=1 on R6.
        //
        // The code takes care of the different behaviors by first comparing the input to the
        // minimum output value (-2**-63 for truncating to long, -2**-31 for truncating to int).
        // If the input is greater than or equal to the minimum, it procedes to the truncate
        // instruction, which will handle such an input the same way irrespective of NAN2008.
        // Otherwise the input is compared to itself to determine whether it is a NaN or not
        // in order to return either zero or the minimum value.
        //
        // TODO: simplify this when the emulator correctly implements NAN2008=1 behavior of the
        // truncate instruction for MIPS64R6.
        if (input_type == Primitive::kPrimFloat) {
          uint32_t min_val = bit_cast<uint32_t, float>(std::numeric_limits<int64_t>::min());
          __ LoadConst32(TMP, min_val);
          __ Mtc1(TMP, FTMP);
          __ CmpLeS(FTMP, FTMP, src);
        } else {
          uint64_t min_val = bit_cast<uint64_t, double>(std::numeric_limits<int64_t>::min());
          __ LoadConst32(TMP, High32Bits(min_val));
          __ Mtc1(ZERO, FTMP);
          __ Mthc1(TMP, FTMP);
          __ CmpLeD(FTMP, FTMP, src);
        }

        __ Bc1nez(FTMP, &truncate);

        if (input_type == Primitive::kPrimFloat) {
          __ CmpEqS(FTMP, src, src);
        } else {
          __ CmpEqD(FTMP, src, src);
        }
        __ Move(dst_low, ZERO);
        __ LoadConst32(dst_high, std::numeric_limits<int32_t>::min());
        __ Mfc1(TMP, FTMP);
        __ And(dst_high, dst_high, TMP);

        __ B(&done);

        __ Bind(&truncate);

        if (input_type == Primitive::kPrimFloat) {
          __ TruncLS(FTMP, src);
        } else {
          __ TruncLD(FTMP, src);
        }
        __ Mfc1(dst_low, FTMP);
        __ Mfhc1(dst_high, FTMP);

        __ Bind(&done);
      } else {
        QuickEntrypointEnum entrypoint = (input_type == Primitive::kPrimFloat) ? kQuickF2l
                                                                               : kQuickD2l;
        codegen_->InvokeRuntime(entrypoint, conversion, conversion->GetDexPc());
        if (input_type == Primitive::kPrimFloat) {
          CheckEntrypointTypes<kQuickF2l, int64_t, float>();
        } else {
          CheckEntrypointTypes<kQuickD2l, int64_t, double>();
        }
      }
    } else {
      FRegister src = locations->InAt(0).AsFpuRegister<FRegister>();
      Register dst = locations->Out().AsRegister<Register>();
      MipsLabel truncate;
      MipsLabel done;

      // The following code supports both NAN2008=0 and NAN2008=1 behaviors of the truncate
      // instruction, the reason being that the emulator implements NAN2008=0 on MIPS64R6,
      // even though it must be NAN2008=1 on R6.
      //
      // For details see the large comment above for the truncation of float/double to long on R6.
      //
      // TODO: simplify this when the emulator correctly implements NAN2008=1 behavior of the
      // truncate instruction for MIPS64R6.
      if (input_type == Primitive::kPrimFloat) {
        uint32_t min_val = bit_cast<uint32_t, float>(std::numeric_limits<int32_t>::min());
        __ LoadConst32(TMP, min_val);
        __ Mtc1(TMP, FTMP);
      } else {
        uint64_t min_val = bit_cast<uint64_t, double>(std::numeric_limits<int32_t>::min());
        __ LoadConst32(TMP, High32Bits(min_val));
        __ Mtc1(ZERO, FTMP);
        __ MoveToFpuHigh(TMP, FTMP);
      }

      if (isR6) {
        if (input_type == Primitive::kPrimFloat) {
          __ CmpLeS(FTMP, FTMP, src);
        } else {
          __ CmpLeD(FTMP, FTMP, src);
        }
        __ Bc1nez(FTMP, &truncate);

        if (input_type == Primitive::kPrimFloat) {
          __ CmpEqS(FTMP, src, src);
        } else {
          __ CmpEqD(FTMP, src, src);
        }
        __ LoadConst32(dst, std::numeric_limits<int32_t>::min());
        __ Mfc1(TMP, FTMP);
        __ And(dst, dst, TMP);
      } else {
        if (input_type == Primitive::kPrimFloat) {
          __ ColeS(0, FTMP, src);
        } else {
          __ ColeD(0, FTMP, src);
        }
        __ Bc1t(0, &truncate);

        if (input_type == Primitive::kPrimFloat) {
          __ CeqS(0, src, src);
        } else {
          __ CeqD(0, src, src);
        }
        __ LoadConst32(dst, std::numeric_limits<int32_t>::min());
        __ Movf(dst, ZERO, 0);
      }

      __ B(&done);

      __ Bind(&truncate);

      if (input_type == Primitive::kPrimFloat) {
        __ TruncWS(FTMP, src);
      } else {
        __ TruncWD(FTMP, src);
      }
      __ Mfc1(dst, FTMP);

      __ Bind(&done);
    }
  } else if (Primitive::IsFloatingPointType(result_type) &&
             Primitive::IsFloatingPointType(input_type)) {
    FRegister dst = locations->Out().AsFpuRegister<FRegister>();
    FRegister src = locations->InAt(0).AsFpuRegister<FRegister>();
    if (result_type == Primitive::kPrimFloat) {
      __ Cvtsd(dst, src);
    } else {
      __ Cvtds(dst, src);
    }
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type
                << " to " << result_type;
  }
}

void LocationsBuilderMIPS::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorMIPS::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderMIPS::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorMIPS::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderMIPS::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::GenPackedSwitchWithCompares(Register value_reg,
                                                               int32_t lower_bound,
                                                               uint32_t num_entries,
                                                               HBasicBlock* switch_block,
                                                               HBasicBlock* default_block) {
  // Create a set of compare/jumps.
  Register temp_reg = TMP;
  __ Addiu32(temp_reg, value_reg, -lower_bound);
  // Jump to default if index is negative
  // Note: We don't check the case that index is positive while value < lower_bound, because in
  // this case, index >= num_entries must be true. So that we can save one branch instruction.
  __ Bltz(temp_reg, codegen_->GetLabelOf(default_block));

  const ArenaVector<HBasicBlock*>& successors = switch_block->GetSuccessors();
  // Jump to successors[0] if value == lower_bound.
  __ Beqz(temp_reg, codegen_->GetLabelOf(successors[0]));
  int32_t last_index = 0;
  for (; num_entries - last_index > 2; last_index += 2) {
    __ Addiu(temp_reg, temp_reg, -2);
    // Jump to successors[last_index + 1] if value < case_value[last_index + 2].
    __ Bltz(temp_reg, codegen_->GetLabelOf(successors[last_index + 1]));
    // Jump to successors[last_index + 2] if value == case_value[last_index + 2].
    __ Beqz(temp_reg, codegen_->GetLabelOf(successors[last_index + 2]));
  }
  if (num_entries - last_index == 2) {
    // The last missing case_value.
    __ Addiu(temp_reg, temp_reg, -1);
    __ Beqz(temp_reg, codegen_->GetLabelOf(successors[last_index + 1]));
  }

  // And the default for any other value.
  if (!codegen_->GoesToNextBlock(switch_block, default_block)) {
    __ B(codegen_->GetLabelOf(default_block));
  }
}

void InstructionCodeGeneratorMIPS::GenTableBasedPackedSwitch(Register value_reg,
                                                             Register constant_area,
                                                             int32_t lower_bound,
                                                             uint32_t num_entries,
                                                             HBasicBlock* switch_block,
                                                             HBasicBlock* default_block) {
  // Create a jump table.
  std::vector<MipsLabel*> labels(num_entries);
  const ArenaVector<HBasicBlock*>& successors = switch_block->GetSuccessors();
  for (uint32_t i = 0; i < num_entries; i++) {
    labels[i] = codegen_->GetLabelOf(successors[i]);
  }
  JumpTable* table = __ CreateJumpTable(std::move(labels));

  // Is the value in range?
  __ Addiu32(TMP, value_reg, -lower_bound);
  if (IsInt<16>(static_cast<int32_t>(num_entries))) {
    __ Sltiu(AT, TMP, num_entries);
    __ Beqz(AT, codegen_->GetLabelOf(default_block));
  } else {
    __ LoadConst32(AT, num_entries);
    __ Bgeu(TMP, AT, codegen_->GetLabelOf(default_block));
  }

  // We are in the range of the table.
  // Load the target address from the jump table, indexing by the value.
  __ LoadLabelAddress(AT, constant_area, table->GetLabel());
  __ Sll(TMP, TMP, 2);
  __ Addu(TMP, TMP, AT);
  __ Lw(TMP, TMP, 0);
  // Compute the absolute target address by adding the table start address
  // (the table contains offsets to targets relative to its start).
  __ Addu(TMP, TMP, AT);
  // And jump.
  __ Jr(TMP);
  __ NopIfNoReordering();
}

void InstructionCodeGeneratorMIPS::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  Register value_reg = locations->InAt(0).AsRegister<Register>();
  HBasicBlock* switch_block = switch_instr->GetBlock();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  if (codegen_->GetInstructionSetFeatures().IsR6() &&
      num_entries > kPackedSwitchJumpTableThreshold) {
    // R6 uses PC-relative addressing to access the jump table.
    // R2, OTOH, requires an HMipsComputeBaseMethodAddress input to access
    // the jump table and it is implemented by changing HPackedSwitch to
    // HMipsPackedSwitch, which bears HMipsComputeBaseMethodAddress.
    // See VisitMipsPackedSwitch() for the table-based implementation on R2.
    GenTableBasedPackedSwitch(value_reg,
                              ZERO,
                              lower_bound,
                              num_entries,
                              switch_block,
                              default_block);
  } else {
    GenPackedSwitchWithCompares(value_reg,
                                lower_bound,
                                num_entries,
                                switch_block,
                                default_block);
  }
}

void LocationsBuilderMIPS::VisitMipsPackedSwitch(HMipsPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  // Constant area pointer (HMipsComputeBaseMethodAddress).
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitMipsPackedSwitch(HMipsPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  Register value_reg = locations->InAt(0).AsRegister<Register>();
  Register constant_area = locations->InAt(1).AsRegister<Register>();
  HBasicBlock* switch_block = switch_instr->GetBlock();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  // This is an R2-only path. HPackedSwitch has been changed to
  // HMipsPackedSwitch, which bears HMipsComputeBaseMethodAddress
  // required to address the jump table relative to PC.
  GenTableBasedPackedSwitch(value_reg,
                            constant_area,
                            lower_bound,
                            num_entries,
                            switch_block,
                            default_block);
}

void LocationsBuilderMIPS::VisitMipsComputeBaseMethodAddress(
    HMipsComputeBaseMethodAddress* insn) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(insn, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitMipsComputeBaseMethodAddress(
    HMipsComputeBaseMethodAddress* insn) {
  LocationSummary* locations = insn->GetLocations();
  Register reg = locations->Out().AsRegister<Register>();

  CHECK(!codegen_->GetInstructionSetFeatures().IsR6());

  // Generate a dummy PC-relative call to obtain PC.
  __ Nal();
  // Grab the return address off RA.
  __ Move(reg, RA);
  // TODO: Can we share this code with that of VisitMipsDexCacheArraysBase()?

  // Remember this offset (the obtained PC value) for later use with constant area.
  __ BindPcRelBaseLabel();
}

void LocationsBuilderMIPS::VisitMipsDexCacheArraysBase(HMipsDexCacheArraysBase* base) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(base);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitMipsDexCacheArraysBase(HMipsDexCacheArraysBase* base) {
  Register reg = base->GetLocations()->Out().AsRegister<Register>();
  CodeGeneratorMIPS::PcRelativePatchInfo* info =
      codegen_->NewPcRelativeDexCacheArrayPatch(base->GetDexFile(), base->GetElementOffset());
  bool reordering = __ SetReorder(false);
  if (codegen_->GetInstructionSetFeatures().IsR6()) {
    __ Bind(&info->high_label);
    __ Bind(&info->pc_rel_label);
    // Add a 32-bit offset to PC.
    __ Auipc(reg, /* placeholder */ 0x1234);
    __ Addiu(reg, reg, /* placeholder */ 0x5678);
  } else {
    // Generate a dummy PC-relative call to obtain PC.
    __ Nal();
    __ Bind(&info->high_label);
    __ Lui(reg, /* placeholder */ 0x1234);
    __ Bind(&info->pc_rel_label);
    __ Ori(reg, reg, /* placeholder */ 0x5678);
    // Add a 32-bit offset to PC.
    __ Addu(reg, reg, RA);
    // TODO: Can we share this code with that of VisitMipsComputeBaseMethodAddress()?
  }
  __ SetReorder(reordering);
}

void LocationsBuilderMIPS::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorMIPS::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderMIPS::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kMipsPointerSize).SizeValue();
    __ LoadFromOffset(kLoadWord,
                      locations->Out().AsRegister<Register>(),
                      locations->InAt(0).AsRegister<Register>(),
                      method_offset);
  } else {
    uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
        instruction->GetIndex(), kMipsPointerSize));
    __ LoadFromOffset(kLoadWord,
                      locations->Out().AsRegister<Register>(),
                      locations->InAt(0).AsRegister<Register>(),
                      mirror::Class::ImtPtrOffset(kMipsPointerSize).Uint32Value());
    __ LoadFromOffset(kLoadWord,
                      locations->Out().AsRegister<Register>(),
                      locations->Out().AsRegister<Register>(),
                      method_offset);
  }
}

#undef __
#undef QUICK_ENTRY_POINT

}  // namespace mips
}  // namespace art
