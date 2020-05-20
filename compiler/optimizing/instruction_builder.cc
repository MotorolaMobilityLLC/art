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

#include "instruction_builder.h"

#include "art_method-inl.h"
#include "base/arena_bit_vector.h"
#include "base/bit_vector-inl.h"
#include "base/logging.h"
#include "block_builder.h"
#include "class_linker-inl.h"
#include "code_generator.h"
#include "data_type-inl.h"
#include "dex/bytecode_utils.h"
#include "dex/dex_instruction-inl.h"
#include "driver/dex_compilation_unit.h"
#include "driver/compiler_options.h"
#include "imtable-inl.h"
#include "jit/jit.h"
#include "mirror/dex_cache.h"
#include "oat_file.h"
#include "optimizing_compiler_stats.h"
#include "quicken_info.h"
#include "scoped_thread_state_change-inl.h"
#include "sharpening.h"
#include "ssa_builder.h"
#include "well_known_classes.h"

namespace art {

HInstructionBuilder::HInstructionBuilder(HGraph* graph,
                                         HBasicBlockBuilder* block_builder,
                                         SsaBuilder* ssa_builder,
                                         const DexFile* dex_file,
                                         const CodeItemDebugInfoAccessor& accessor,
                                         DataType::Type return_type,
                                         const DexCompilationUnit* dex_compilation_unit,
                                         const DexCompilationUnit* outer_compilation_unit,
                                         CodeGenerator* code_generator,
                                         ArrayRef<const uint8_t> interpreter_metadata,
                                         OptimizingCompilerStats* compiler_stats,
                                         ScopedArenaAllocator* local_allocator)
    : allocator_(graph->GetAllocator()),
      graph_(graph),
      dex_file_(dex_file),
      code_item_accessor_(accessor),
      return_type_(return_type),
      block_builder_(block_builder),
      ssa_builder_(ssa_builder),
      code_generator_(code_generator),
      dex_compilation_unit_(dex_compilation_unit),
      outer_compilation_unit_(outer_compilation_unit),
      quicken_info_(interpreter_metadata),
      compilation_stats_(compiler_stats),
      local_allocator_(local_allocator),
      locals_for_(local_allocator->Adapter(kArenaAllocGraphBuilder)),
      current_block_(nullptr),
      current_locals_(nullptr),
      latest_result_(nullptr),
      current_this_parameter_(nullptr),
      loop_headers_(local_allocator->Adapter(kArenaAllocGraphBuilder)),
      class_cache_(std::less<dex::TypeIndex>(), local_allocator->Adapter(kArenaAllocGraphBuilder)) {
  loop_headers_.reserve(kDefaultNumberOfLoops);
}

HBasicBlock* HInstructionBuilder::FindBlockStartingAt(uint32_t dex_pc) const {
  return block_builder_->GetBlockAt(dex_pc);
}

inline ScopedArenaVector<HInstruction*>* HInstructionBuilder::GetLocalsFor(HBasicBlock* block) {
  ScopedArenaVector<HInstruction*>* locals = &locals_for_[block->GetBlockId()];
  const size_t vregs = graph_->GetNumberOfVRegs();
  if (locals->size() == vregs) {
    return locals;
  }
  return GetLocalsForWithAllocation(block, locals, vregs);
}

ScopedArenaVector<HInstruction*>* HInstructionBuilder::GetLocalsForWithAllocation(
    HBasicBlock* block,
    ScopedArenaVector<HInstruction*>* locals,
    const size_t vregs) {
  DCHECK_NE(locals->size(), vregs);
  locals->resize(vregs, nullptr);
  if (block->IsCatchBlock()) {
    // We record incoming inputs of catch phis at throwing instructions and
    // must therefore eagerly create the phis. Phis for undefined vregs will
    // be deleted when the first throwing instruction with the vreg undefined
    // is encountered. Unused phis will be removed by dead phi analysis.
    for (size_t i = 0; i < vregs; ++i) {
      // No point in creating the catch phi if it is already undefined at
      // the first throwing instruction.
      HInstruction* current_local_value = (*current_locals_)[i];
      if (current_local_value != nullptr) {
        HPhi* phi = new (allocator_) HPhi(
            allocator_,
            i,
            0,
            current_local_value->GetType());
        block->AddPhi(phi);
        (*locals)[i] = phi;
      }
    }
  }
  return locals;
}

inline HInstruction* HInstructionBuilder::ValueOfLocalAt(HBasicBlock* block, size_t local) {
  ScopedArenaVector<HInstruction*>* locals = GetLocalsFor(block);
  return (*locals)[local];
}

void HInstructionBuilder::InitializeBlockLocals() {
  current_locals_ = GetLocalsFor(current_block_);

  if (current_block_->IsCatchBlock()) {
    // Catch phis were already created and inputs collected from throwing sites.
    if (kIsDebugBuild) {
      // Make sure there was at least one throwing instruction which initialized
      // locals (guaranteed by HGraphBuilder) and that all try blocks have been
      // visited already (from HTryBoundary scoping and reverse post order).
      bool catch_block_visited = false;
      for (HBasicBlock* current : graph_->GetReversePostOrder()) {
        if (current == current_block_) {
          catch_block_visited = true;
        } else if (current->IsTryBlock()) {
          const HTryBoundary& try_entry = current->GetTryCatchInformation()->GetTryEntry();
          if (try_entry.HasExceptionHandler(*current_block_)) {
            DCHECK(!catch_block_visited) << "Catch block visited before its try block.";
          }
        }
      }
      DCHECK_EQ(current_locals_->size(), graph_->GetNumberOfVRegs())
          << "No instructions throwing into a live catch block.";
    }
  } else if (current_block_->IsLoopHeader()) {
    // If the block is a loop header, we know we only have visited the pre header
    // because we are visiting in reverse post order. We create phis for all initialized
    // locals from the pre header. Their inputs will be populated at the end of
    // the analysis.
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      HInstruction* incoming =
          ValueOfLocalAt(current_block_->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (allocator_) HPhi(
            allocator_,
            local,
            0,
            incoming->GetType());
        current_block_->AddPhi(phi);
        (*current_locals_)[local] = phi;
      }
    }

    // Save the loop header so that the last phase of the analysis knows which
    // blocks need to be updated.
    loop_headers_.push_back(current_block_);
  } else if (current_block_->GetPredecessors().size() > 0) {
    // All predecessors have already been visited because we are visiting in reverse post order.
    // We merge the values of all locals, creating phis if those values differ.
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocalAt(current_block_->GetPredecessors()[0], local);

      for (HBasicBlock* predecessor : current_block_->GetPredecessors()) {
        HInstruction* current = ValueOfLocalAt(predecessor, local);
        if (current == nullptr) {
          one_predecessor_has_no_value = true;
          break;
        } else if (current != value) {
          is_different = true;
        }
      }

      if (one_predecessor_has_no_value) {
        // If one predecessor has no value for this local, we trust the verifier has
        // successfully checked that there is a store dominating any read after this block.
        continue;
      }

      if (is_different) {
        HInstruction* first_input = ValueOfLocalAt(current_block_->GetPredecessors()[0], local);
        HPhi* phi = new (allocator_) HPhi(
            allocator_,
            local,
            current_block_->GetPredecessors().size(),
            first_input->GetType());
        for (size_t i = 0; i < current_block_->GetPredecessors().size(); i++) {
          HInstruction* pred_value = ValueOfLocalAt(current_block_->GetPredecessors()[i], local);
          phi->SetRawInputAt(i, pred_value);
        }
        current_block_->AddPhi(phi);
        value = phi;
      }
      (*current_locals_)[local] = value;
    }
  }
}

void HInstructionBuilder::PropagateLocalsToCatchBlocks() {
  const HTryBoundary& try_entry = current_block_->GetTryCatchInformation()->GetTryEntry();
  for (HBasicBlock* catch_block : try_entry.GetExceptionHandlers()) {
    ScopedArenaVector<HInstruction*>* handler_locals = GetLocalsFor(catch_block);
    DCHECK_EQ(handler_locals->size(), current_locals_->size());
    for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
      HInstruction* handler_value = (*handler_locals)[vreg];
      if (handler_value == nullptr) {
        // Vreg was undefined at a previously encountered throwing instruction
        // and the catch phi was deleted. Do not record the local value.
        continue;
      }
      DCHECK(handler_value->IsPhi());

      HInstruction* local_value = (*current_locals_)[vreg];
      if (local_value == nullptr) {
        // This is the first instruction throwing into `catch_block` where
        // `vreg` is undefined. Delete the catch phi.
        catch_block->RemovePhi(handler_value->AsPhi());
        (*handler_locals)[vreg] = nullptr;
      } else {
        // Vreg has been defined at all instructions throwing into `catch_block`
        // encountered so far. Record the local value in the catch phi.
        handler_value->AsPhi()->AddInput(local_value);
      }
    }
  }
}

void HInstructionBuilder::AppendInstruction(HInstruction* instruction) {
  current_block_->AddInstruction(instruction);
  InitializeInstruction(instruction);
}

void HInstructionBuilder::InsertInstructionAtTop(HInstruction* instruction) {
  if (current_block_->GetInstructions().IsEmpty()) {
    current_block_->AddInstruction(instruction);
  } else {
    current_block_->InsertInstructionBefore(instruction, current_block_->GetFirstInstruction());
  }
  InitializeInstruction(instruction);
}

void HInstructionBuilder::InitializeInstruction(HInstruction* instruction) {
  if (instruction->NeedsEnvironment()) {
    HEnvironment* environment = new (allocator_) HEnvironment(
        allocator_,
        current_locals_->size(),
        graph_->GetArtMethod(),
        instruction->GetDexPc(),
        instruction);
    environment->CopyFrom(ArrayRef<HInstruction* const>(*current_locals_));
    instruction->SetRawEnvironment(environment);
  }
}

HInstruction* HInstructionBuilder::LoadNullCheckedLocal(uint32_t register_index, uint32_t dex_pc) {
  HInstruction* ref = LoadLocal(register_index, DataType::Type::kReference);
  if (!ref->CanBeNull()) {
    return ref;
  }

  HNullCheck* null_check = new (allocator_) HNullCheck(ref, dex_pc);
  AppendInstruction(null_check);
  return null_check;
}

void HInstructionBuilder::SetLoopHeaderPhiInputs() {
  for (size_t i = loop_headers_.size(); i > 0; --i) {
    HBasicBlock* block = loop_headers_[i - 1];
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      size_t vreg = phi->GetRegNumber();
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        HInstruction* value = ValueOfLocalAt(predecessor, vreg);
        if (value == nullptr) {
          // Vreg is undefined at this predecessor. Mark it dead and leave with
          // fewer inputs than predecessors. SsaChecker will fail if not removed.
          phi->SetDead();
          break;
        } else {
          phi->AddInput(value);
        }
      }
    }
  }
}

static bool IsBlockPopulated(HBasicBlock* block) {
  if (block->IsLoopHeader()) {
    // Suspend checks were inserted into loop headers during building of dominator tree.
    DCHECK(block->GetFirstInstruction()->IsSuspendCheck());
    return block->GetFirstInstruction() != block->GetLastInstruction();
  } else {
    return !block->GetInstructions().IsEmpty();
  }
}

bool HInstructionBuilder::Build() {
  DCHECK(code_item_accessor_.HasCodeItem());
  locals_for_.resize(
      graph_->GetBlocks().size(),
      ScopedArenaVector<HInstruction*>(local_allocator_->Adapter(kArenaAllocGraphBuilder)));

  // Find locations where we want to generate extra stackmaps for native debugging.
  // This allows us to generate the info only at interesting points (for example,
  // at start of java statement) rather than before every dex instruction.
  const bool native_debuggable = code_generator_ != nullptr &&
                                 code_generator_->GetCompilerOptions().GetNativeDebuggable();
  ArenaBitVector* native_debug_info_locations = nullptr;
  if (native_debuggable) {
    native_debug_info_locations = FindNativeDebugInfoLocations();
  }

  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    current_block_ = block;
    uint32_t block_dex_pc = current_block_->GetDexPc();

    InitializeBlockLocals();

    if (current_block_->IsEntryBlock()) {
      InitializeParameters();
      AppendInstruction(new (allocator_) HSuspendCheck(0u));
      AppendInstruction(new (allocator_) HGoto(0u));
      continue;
    } else if (current_block_->IsExitBlock()) {
      AppendInstruction(new (allocator_) HExit());
      continue;
    } else if (current_block_->IsLoopHeader()) {
      HSuspendCheck* suspend_check = new (allocator_) HSuspendCheck(current_block_->GetDexPc());
      current_block_->GetLoopInformation()->SetSuspendCheck(suspend_check);
      // This is slightly odd because the loop header might not be empty (TryBoundary).
      // But we're still creating the environment with locals from the top of the block.
      InsertInstructionAtTop(suspend_check);
    }

    if (block_dex_pc == kNoDexPc || current_block_ != block_builder_->GetBlockAt(block_dex_pc)) {
      // Synthetic block that does not need to be populated.
      DCHECK(IsBlockPopulated(current_block_));
      continue;
    }

    DCHECK(!IsBlockPopulated(current_block_));

    uint32_t quicken_index = 0;
    if (CanDecodeQuickenedInfo()) {
      quicken_index = block_builder_->GetQuickenIndex(block_dex_pc);
    }

    for (const DexInstructionPcPair& pair : code_item_accessor_.InstructionsFrom(block_dex_pc)) {
      if (current_block_ == nullptr) {
        // The previous instruction ended this block.
        break;
      }

      const uint32_t dex_pc = pair.DexPc();
      if (dex_pc != block_dex_pc && FindBlockStartingAt(dex_pc) != nullptr) {
        // This dex_pc starts a new basic block.
        break;
      }

      if (current_block_->IsTryBlock() && IsThrowingDexInstruction(pair.Inst())) {
        PropagateLocalsToCatchBlocks();
      }

      if (native_debuggable && native_debug_info_locations->IsBitSet(dex_pc)) {
        AppendInstruction(new (allocator_) HNativeDebugInfo(dex_pc));
      }

      // Note: There may be no Thread for gtests.
      DCHECK(Thread::Current() == nullptr || !Thread::Current()->IsExceptionPending())
          << dex_file_->PrettyMethod(dex_compilation_unit_->GetDexMethodIndex())
          << " " << pair.Inst().Name() << "@" << dex_pc;
      if (!ProcessDexInstruction(pair.Inst(), dex_pc, quicken_index)) {
        return false;
      }
      DCHECK(Thread::Current() == nullptr || !Thread::Current()->IsExceptionPending())
          << dex_file_->PrettyMethod(dex_compilation_unit_->GetDexMethodIndex())
          << " " << pair.Inst().Name() << "@" << dex_pc;

      if (QuickenInfoTable::NeedsIndexForInstruction(&pair.Inst())) {
        ++quicken_index;
      }
    }

    if (current_block_ != nullptr) {
      // Branching instructions clear current_block, so we know the last
      // instruction of the current block is not a branching instruction.
      // We add an unconditional Goto to the next block.
      DCHECK_EQ(current_block_->GetSuccessors().size(), 1u);
      AppendInstruction(new (allocator_) HGoto());
    }
  }

  SetLoopHeaderPhiInputs();

  return true;
}

void HInstructionBuilder::BuildIntrinsic(ArtMethod* method) {
  DCHECK(!code_item_accessor_.HasCodeItem());
  DCHECK(method->IsIntrinsic());

  locals_for_.resize(
      graph_->GetBlocks().size(),
      ScopedArenaVector<HInstruction*>(local_allocator_->Adapter(kArenaAllocGraphBuilder)));

  // Fill the entry block. Do not add suspend check, we do not want a suspend
  // check in intrinsics; intrinsic methods are supposed to be fast.
  current_block_ = graph_->GetEntryBlock();
  InitializeBlockLocals();
  InitializeParameters();
  AppendInstruction(new (allocator_) HGoto(0u));

  // Fill the body.
  current_block_ = current_block_->GetSingleSuccessor();
  InitializeBlockLocals();
  DCHECK(!IsBlockPopulated(current_block_));

  // Add the intermediate representation, if available, or invoke instruction.
  size_t in_vregs = graph_->GetNumberOfInVRegs();
  size_t number_of_arguments =
      in_vregs - std::count(current_locals_->end() - in_vregs, current_locals_->end(), nullptr);
  uint32_t method_idx = dex_compilation_unit_->GetDexMethodIndex();
  const char* shorty = dex_file_->GetMethodShorty(method_idx);
  RangeInstructionOperands operands(graph_->GetNumberOfVRegs() - in_vregs, in_vregs);
  if (!BuildSimpleIntrinsic(method, kNoDexPc, operands, shorty)) {
    // Some intrinsics without intermediate representation still yield a leaf method,
    // so build the invoke. Use HInvokeStaticOrDirect even for methods that would
    // normally use an HInvokeVirtual (sharpen the call).
    MethodReference target_method(dex_file_, method_idx);
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kRuntimeCall,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        /* method_load_data= */ 0u
    };
    InvokeType invoke_type = dex_compilation_unit_->IsStatic() ? kStatic : kDirect;
    HInvokeStaticOrDirect* invoke = new (allocator_) HInvokeStaticOrDirect(
        allocator_,
        number_of_arguments,
        return_type_,
        kNoDexPc,
        method_idx,
        method,
        dispatch_info,
        invoke_type,
        target_method,
        HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
    HandleInvoke(invoke, operands, shorty, /* is_unresolved= */ false);
  }

  // Add the return instruction.
  if (return_type_ == DataType::Type::kVoid) {
    AppendInstruction(new (allocator_) HReturnVoid());
  } else {
    AppendInstruction(new (allocator_) HReturn(latest_result_));
  }

  // Fill the exit block.
  DCHECK_EQ(current_block_->GetSingleSuccessor(), graph_->GetExitBlock());
  current_block_ = graph_->GetExitBlock();
  InitializeBlockLocals();
  AppendInstruction(new (allocator_) HExit());
}

ArenaBitVector* HInstructionBuilder::FindNativeDebugInfoLocations() {
  ArenaBitVector* locations = ArenaBitVector::Create(local_allocator_,
                                                     code_item_accessor_.InsnsSizeInCodeUnits(),
                                                     /* expandable= */ false,
                                                     kArenaAllocGraphBuilder);
  locations->ClearAllBits();
  // The visitor gets called when the line number changes.
  // In other words, it marks the start of new java statement.
  code_item_accessor_.DecodeDebugPositionInfo([&](const DexFile::PositionInfo& entry) {
    locations->SetBit(entry.address_);
    return false;
  });
  // Instruction-specific tweaks.
  for (const DexInstructionPcPair& inst : code_item_accessor_) {
    switch (inst->Opcode()) {
      case Instruction::MOVE_EXCEPTION: {
        // Stop in native debugger after the exception has been moved.
        // The compiler also expects the move at the start of basic block so
        // we do not want to interfere by inserting native-debug-info before it.
        locations->ClearBit(inst.DexPc());
        DexInstructionIterator next = std::next(DexInstructionIterator(inst));
        DCHECK(next.DexPc() != inst.DexPc());
        if (next != code_item_accessor_.end()) {
          locations->SetBit(next.DexPc());
        }
        break;
      }
      default:
        break;
    }
  }
  return locations;
}

HInstruction* HInstructionBuilder::LoadLocal(uint32_t reg_number, DataType::Type type) const {
  HInstruction* value = (*current_locals_)[reg_number];
  DCHECK(value != nullptr);

  // If the operation requests a specific type, we make sure its input is of that type.
  if (type != value->GetType()) {
    if (DataType::IsFloatingPointType(type)) {
      value = ssa_builder_->GetFloatOrDoubleEquivalent(value, type);
    } else if (type == DataType::Type::kReference) {
      value = ssa_builder_->GetReferenceTypeEquivalent(value);
    }
    DCHECK(value != nullptr);
  }

  return value;
}

void HInstructionBuilder::UpdateLocal(uint32_t reg_number, HInstruction* stored_value) {
  DataType::Type stored_type = stored_value->GetType();
  DCHECK_NE(stored_type, DataType::Type::kVoid);

  // Storing into vreg `reg_number` may implicitly invalidate the surrounding
  // registers. Consider the following cases:
  // (1) Storing a wide value must overwrite previous values in both `reg_number`
  //     and `reg_number+1`. We store `nullptr` in `reg_number+1`.
  // (2) If vreg `reg_number-1` holds a wide value, writing into `reg_number`
  //     must invalidate it. We store `nullptr` in `reg_number-1`.
  // Consequently, storing a wide value into the high vreg of another wide value
  // will invalidate both `reg_number-1` and `reg_number+1`.

  if (reg_number != 0) {
    HInstruction* local_low = (*current_locals_)[reg_number - 1];
    if (local_low != nullptr && DataType::Is64BitType(local_low->GetType())) {
      // The vreg we are storing into was previously the high vreg of a pair.
      // We need to invalidate its low vreg.
      DCHECK((*current_locals_)[reg_number] == nullptr);
      (*current_locals_)[reg_number - 1] = nullptr;
    }
  }

  (*current_locals_)[reg_number] = stored_value;
  if (DataType::Is64BitType(stored_type)) {
    // We are storing a pair. Invalidate the instruction in the high vreg.
    (*current_locals_)[reg_number + 1] = nullptr;
  }
}

void HInstructionBuilder::InitializeParameters() {
  DCHECK(current_block_->IsEntryBlock());

  // outer_compilation_unit_ is null only when unit testing.
  if (outer_compilation_unit_ == nullptr) {
    return;
  }

  const char* shorty = dex_compilation_unit_->GetShorty();
  uint16_t number_of_parameters = graph_->GetNumberOfInVRegs();
  uint16_t locals_index = graph_->GetNumberOfLocalVRegs();
  uint16_t parameter_index = 0;

  const dex::MethodId& referrer_method_id =
      dex_file_->GetMethodId(dex_compilation_unit_->GetDexMethodIndex());
  if (!dex_compilation_unit_->IsStatic()) {
    // Add the implicit 'this' argument, not expressed in the signature.
    HParameterValue* parameter = new (allocator_) HParameterValue(*dex_file_,
                                                              referrer_method_id.class_idx_,
                                                              parameter_index++,
                                                              DataType::Type::kReference,
                                                              /* is_this= */ true);
    AppendInstruction(parameter);
    UpdateLocal(locals_index++, parameter);
    number_of_parameters--;
    current_this_parameter_ = parameter;
  } else {
    DCHECK(current_this_parameter_ == nullptr);
  }

  const dex::ProtoId& proto = dex_file_->GetMethodPrototype(referrer_method_id);
  const dex::TypeList* arg_types = dex_file_->GetProtoParameters(proto);
  for (int i = 0, shorty_pos = 1; i < number_of_parameters; i++) {
    HParameterValue* parameter = new (allocator_) HParameterValue(
        *dex_file_,
        arg_types->GetTypeItem(shorty_pos - 1).type_idx_,
        parameter_index++,
        DataType::FromShorty(shorty[shorty_pos]),
        /* is_this= */ false);
    ++shorty_pos;
    AppendInstruction(parameter);
    // Store the parameter value in the local that the dex code will use
    // to reference that parameter.
    UpdateLocal(locals_index++, parameter);
    if (DataType::Is64BitType(parameter->GetType())) {
      i++;
      locals_index++;
      parameter_index++;
    }
  }
}

template<typename T>
void HInstructionBuilder::If_22t(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), DataType::Type::kInt32);
  HInstruction* second = LoadLocal(instruction.VRegB(), DataType::Type::kInt32);
  T* comparison = new (allocator_) T(first, second, dex_pc);
  AppendInstruction(comparison);
  AppendInstruction(new (allocator_) HIf(comparison, dex_pc));
  current_block_ = nullptr;
}

template<typename T>
void HInstructionBuilder::If_21t(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* value = LoadLocal(instruction.VRegA(), DataType::Type::kInt32);
  T* comparison = new (allocator_) T(value, graph_->GetIntConstant(0, dex_pc), dex_pc);
  AppendInstruction(comparison);
  AppendInstruction(new (allocator_) HIf(comparison, dex_pc));
  current_block_ = nullptr;
}

template<typename T>
void HInstructionBuilder::Unop_12x(const Instruction& instruction,
                                   DataType::Type type,
                                   uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  AppendInstruction(new (allocator_) T(type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

void HInstructionBuilder::Conversion_12x(const Instruction& instruction,
                                         DataType::Type input_type,
                                         DataType::Type result_type,
                                         uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), input_type);
  AppendInstruction(new (allocator_) HTypeConversion(result_type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_23x(const Instruction& instruction,
                                    DataType::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), type);
  AppendInstruction(new (allocator_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_23x_shift(const Instruction& instruction,
                                          DataType::Type type,
                                          uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), DataType::Type::kInt32);
  AppendInstruction(new (allocator_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

void HInstructionBuilder::Binop_23x_cmp(const Instruction& instruction,
                                        DataType::Type type,
                                        ComparisonBias bias,
                                        uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), type);
  AppendInstruction(new (allocator_) HCompare(type, first, second, bias, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_12x_shift(const Instruction& instruction,
                                          DataType::Type type,
                                          uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type);
  HInstruction* second = LoadLocal(instruction.VRegB(), DataType::Type::kInt32);
  AppendInstruction(new (allocator_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_12x(const Instruction& instruction,
                                    DataType::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type);
  HInstruction* second = LoadLocal(instruction.VRegB(), type);
  AppendInstruction(new (allocator_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_22s(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), DataType::Type::kInt32);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22s(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  AppendInstruction(new (allocator_) T(DataType::Type::kInt32, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HInstructionBuilder::Binop_22b(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), DataType::Type::kInt32);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22b(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  AppendInstruction(new (allocator_) T(DataType::Type::kInt32, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

// Does the method being compiled need any constructor barriers being inserted?
// (Always 'false' for methods that aren't <init>.)
static bool RequiresConstructorBarrier(const DexCompilationUnit* cu) {
  // Can be null in unit tests only.
  if (UNLIKELY(cu == nullptr)) {
    return false;
  }

  // Constructor barriers are applicable only for <init> methods.
  if (LIKELY(!cu->IsConstructor() || cu->IsStatic())) {
    return false;
  }

  return cu->RequiresConstructorBarrier();
}

// Returns true if `block` has only one successor which starts at the next
// dex_pc after `instruction` at `dex_pc`.
static bool IsFallthroughInstruction(const Instruction& instruction,
                                     uint32_t dex_pc,
                                     HBasicBlock* block) {
  uint32_t next_dex_pc = dex_pc + instruction.SizeInCodeUnits();
  return block->GetSingleSuccessor()->GetDexPc() == next_dex_pc;
}

void HInstructionBuilder::BuildSwitch(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* value = LoadLocal(instruction.VRegA(), DataType::Type::kInt32);
  DexSwitchTable table(instruction, dex_pc);

  if (table.GetNumEntries() == 0) {
    // Empty Switch. Code falls through to the next block.
    DCHECK(IsFallthroughInstruction(instruction, dex_pc, current_block_));
    AppendInstruction(new (allocator_) HGoto(dex_pc));
  } else if (table.ShouldBuildDecisionTree()) {
    for (DexSwitchTableIterator it(table); !it.Done(); it.Advance()) {
      HInstruction* case_value = graph_->GetIntConstant(it.CurrentKey(), dex_pc);
      HEqual* comparison = new (allocator_) HEqual(value, case_value, dex_pc);
      AppendInstruction(comparison);
      AppendInstruction(new (allocator_) HIf(comparison, dex_pc));

      if (!it.IsLast()) {
        current_block_ = FindBlockStartingAt(it.GetDexPcForCurrentIndex());
      }
    }
  } else {
    AppendInstruction(
        new (allocator_) HPackedSwitch(table.GetEntryAt(0), table.GetNumEntries(), value, dex_pc));
  }

  current_block_ = nullptr;
}

void HInstructionBuilder::BuildReturn(const Instruction& instruction,
                                      DataType::Type type,
                                      uint32_t dex_pc) {
  if (type == DataType::Type::kVoid) {
    // Only <init> (which is a return-void) could possibly have a constructor fence.
    // This may insert additional redundant constructor fences from the super constructors.
    // TODO: remove redundant constructor fences (b/36656456).
    if (RequiresConstructorBarrier(dex_compilation_unit_)) {
      // Compiling instance constructor.
      DCHECK_STREQ("<init>", graph_->GetMethodName());

      HInstruction* fence_target = current_this_parameter_;
      DCHECK(fence_target != nullptr);

      AppendInstruction(new (allocator_) HConstructorFence(fence_target, dex_pc, allocator_));
      MaybeRecordStat(
          compilation_stats_,
          MethodCompilationStat::kConstructorFenceGeneratedFinal);
    }
    AppendInstruction(new (allocator_) HReturnVoid(dex_pc));
  } else {
    DCHECK(!RequiresConstructorBarrier(dex_compilation_unit_));
    HInstruction* value = LoadLocal(instruction.VRegA(), type);
    AppendInstruction(new (allocator_) HReturn(value, dex_pc));
  }
  current_block_ = nullptr;
}

static InvokeType GetInvokeTypeFromOpCode(Instruction::Code opcode) {
  switch (opcode) {
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      return kStatic;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      return kDirect;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      return kVirtual;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      return kInterface;
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_SUPER:
      return kSuper;
    default:
      LOG(FATAL) << "Unexpected invoke opcode: " << opcode;
      UNREACHABLE();
  }
}

// Try to resolve a method using the class linker. Return null if a method could
// not be resolved or the resolved method cannot be used for some reason.
// Also retrieve method data needed for creating the invoke intermediate
// representation while we hold the mutator lock here.
static ArtMethod* ResolveMethod(uint16_t method_idx,
                                ArtMethod* referrer,
                                const DexCompilationUnit& dex_compilation_unit,
                                /*inout*/InvokeType* invoke_type,
                                /*out*/MethodReference* target_method,
                                /*out*/bool* is_string_constructor) {
  ScopedObjectAccess soa(Thread::Current());

  ClassLinker* class_linker = dex_compilation_unit.GetClassLinker();
  Handle<mirror::ClassLoader> class_loader = dex_compilation_unit.GetClassLoader();

  ArtMethod* resolved_method =
      class_linker->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
          method_idx,
          dex_compilation_unit.GetDexCache(),
          class_loader,
          referrer,
          *invoke_type);

  if (UNLIKELY(resolved_method == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
    return nullptr;
  }
  DCHECK(!soa.Self()->IsExceptionPending());

  // The referrer may be unresolved for AOT if we're compiling a class that cannot be
  // resolved because, for example, we don't find a superclass in the classpath.
  if (referrer == nullptr) {
    // The class linker cannot check access without a referrer, so we have to do it.
    // Fall back to HInvokeUnresolved if the method isn't public.
    if (!resolved_method->IsPublic()) {
      return nullptr;
    }
  }

  // We have to special case the invoke-super case, as ClassLinker::ResolveMethod does not.
  // We need to look at the referrer's super class vtable. We need to do this to know if we need to
  // make this an invoke-unresolved to handle cross-dex invokes or abstract super methods, both of
  // which require runtime handling.
  if (*invoke_type == kSuper) {
    ObjPtr<mirror::Class> compiling_class = dex_compilation_unit.GetCompilingClass().Get();
    if (compiling_class == nullptr) {
      // We could not determine the method's class we need to wait until runtime.
      DCHECK(Runtime::Current()->IsAotCompiler());
      return nullptr;
    }
    ObjPtr<mirror::Class> referenced_class = class_linker->LookupResolvedType(
        dex_compilation_unit.GetDexFile()->GetMethodId(method_idx).class_idx_,
        dex_compilation_unit.GetDexCache().Get(),
        class_loader.Get());
    DCHECK(referenced_class != nullptr);  // We have already resolved a method from this class.
    if (!referenced_class->IsAssignableFrom(compiling_class)) {
      // We cannot statically determine the target method. The runtime will throw a
      // NoSuchMethodError on this one.
      return nullptr;
    }
    ArtMethod* actual_method;
    if (referenced_class->IsInterface()) {
      actual_method = referenced_class->FindVirtualMethodForInterfaceSuper(
          resolved_method, class_linker->GetImagePointerSize());
    } else {
      uint16_t vtable_index = resolved_method->GetMethodIndex();
      actual_method = compiling_class->GetSuperClass()->GetVTableEntry(
          vtable_index, class_linker->GetImagePointerSize());
    }
    if (actual_method != resolved_method &&
        !IsSameDexFile(*actual_method->GetDexFile(), *dex_compilation_unit.GetDexFile())) {
      // The back-end code generator relies on this check in order to ensure that it will not
      // attempt to read the dex_cache with a dex_method_index that is not from the correct
      // dex_file. If we didn't do this check then the dex_method_index will not be updated in the
      // builder, which means that the code-generator (and sharpening and inliner, maybe)
      // might invoke an incorrect method.
      // TODO: The actual method could still be referenced in the current dex file, so we
      //       could try locating it.
      // TODO: Remove the dex_file restriction.
      return nullptr;
    }
    if (!actual_method->IsInvokable()) {
      // Fail if the actual method cannot be invoked. Otherwise, the runtime resolution stub
      // could resolve the callee to the wrong method.
      return nullptr;
    }
    resolved_method = actual_method;
  }

  if (*invoke_type == kInterface) {
    if (resolved_method->GetDeclaringClass()->IsObjectClass()) {
      // If the resolved method is from j.l.Object, emit a virtual call instead.
      // The IMT conflict stub only handles interface methods.
      *invoke_type = kVirtual;
    } else {
      DCHECK(resolved_method->GetDeclaringClass()->IsInterface());
    }
  }

  if (*invoke_type == kDirect || *invoke_type == kStatic || *invoke_type == kSuper) {
    // Record the target method needed for HInvokeStaticOrDirect.
    *target_method =
        MethodReference(resolved_method->GetDexFile(), resolved_method->GetDexMethodIndex());
  } else if (*invoke_type == kVirtual) {
    // For HInvokeVirtual we need the vtable index.
    *target_method = MethodReference(/*file=*/ nullptr, resolved_method->GetVtableIndex());
  } else {
    DCHECK_EQ(*invoke_type, kInterface);
    // For HInvokeInterface we need the IMT index.
    *target_method = MethodReference(/*file=*/ nullptr, ImTable::GetImtIndex(resolved_method));
  }

  *is_string_constructor =
      resolved_method->IsConstructor() && resolved_method->GetDeclaringClass()->IsStringClass();

  return resolved_method;
}

bool HInstructionBuilder::BuildInvoke(const Instruction& instruction,
                                      uint32_t dex_pc,
                                      uint32_t method_idx,
                                      const InstructionOperands& operands) {
  InvokeType invoke_type = GetInvokeTypeFromOpCode(instruction.Opcode());
  const char* shorty = dex_file_->GetMethodShorty(method_idx);
  DataType::Type return_type = DataType::FromShorty(shorty[0]);

  // Remove the return type from the 'proto'.
  size_t number_of_arguments = strlen(shorty) - 1;
  if (invoke_type != kStatic) {  // instance call
    // One extra argument for 'this'.
    number_of_arguments++;
  }

  MethodReference target_method(nullptr, 0u);
  bool is_string_constructor = false;
  ArtMethod* resolved_method = ResolveMethod(method_idx,
                                             graph_->GetArtMethod(),
                                             *dex_compilation_unit_,
                                             &invoke_type,
                                             &target_method,
                                             &is_string_constructor);

  if (UNLIKELY(resolved_method == nullptr)) {
    DCHECK(!Thread::Current()->IsExceptionPending());
    MaybeRecordStat(compilation_stats_,
                    MethodCompilationStat::kUnresolvedMethod);
    HInvoke* invoke = new (allocator_) HInvokeUnresolved(allocator_,
                                                         number_of_arguments,
                                                         return_type,
                                                         dex_pc,
                                                         method_idx,
                                                         invoke_type);
    return HandleInvoke(invoke, operands, shorty, /* is_unresolved= */ true);
  }

  // Replace calls to String.<init> with StringFactory.
  if (is_string_constructor) {
    uint32_t string_init_entry_point = WellKnownClasses::StringInitToEntryPoint(resolved_method);
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kStringInit,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        dchecked_integral_cast<uint64_t>(string_init_entry_point)
    };
    // We pass null for the resolved_method to ensure optimizations
    // don't rely on it.
    HInvoke* invoke = new (allocator_) HInvokeStaticOrDirect(
        allocator_,
        number_of_arguments - 1,
        /* return_type= */ DataType::Type::kReference,
        dex_pc,
        method_idx,
        /* resolved_method= */ nullptr,
        dispatch_info,
        invoke_type,
        target_method,
        HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
    return HandleStringInit(invoke, operands, shorty);
  }

  // Potential class initialization check, in the case of a static method call.
  HInvokeStaticOrDirect::ClinitCheckRequirement clinit_check_requirement =
      HInvokeStaticOrDirect::ClinitCheckRequirement::kNone;
  HClinitCheck* clinit_check = nullptr;
  if (invoke_type == kStatic) {
    clinit_check = ProcessClinitCheckForInvoke(dex_pc, resolved_method, &clinit_check_requirement);
  }

  // Try to build an HIR replacement for the intrinsic.
  if (UNLIKELY(resolved_method->IsIntrinsic())) {
    // All intrinsics are in the primary boot image, so their class can always be referenced
    // and we do not need to rely on the implicit class initialization check. The class should
    // be initialized but we do not require that here.
    DCHECK_NE(clinit_check_requirement, HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
    if (BuildSimpleIntrinsic(resolved_method, dex_pc, operands, shorty)) {
      return true;
    }
  }

  HInvoke* invoke = nullptr;
  if (invoke_type == kDirect || invoke_type == kStatic || invoke_type == kSuper) {
    if (invoke_type == kSuper) {
      if (IsSameDexFile(*target_method.dex_file, *dex_compilation_unit_->GetDexFile())) {
        // Update the method index to the one resolved. Note that this may be a no-op if
        // we resolved to the method referenced by the instruction.
        method_idx = target_method.index;
      }
    }

    HInvokeStaticOrDirect::DispatchInfo dispatch_info =
        HSharpening::SharpenInvokeStaticOrDirect(resolved_method, code_generator_);
    invoke = new (allocator_) HInvokeStaticOrDirect(allocator_,
                                                    number_of_arguments,
                                                    return_type,
                                                    dex_pc,
                                                    method_idx,
                                                    resolved_method,
                                                    dispatch_info,
                                                    invoke_type,
                                                    target_method,
                                                    clinit_check_requirement);
    if (clinit_check != nullptr) {
      // Add the class initialization check as last input of `invoke`.
      DCHECK_EQ(clinit_check_requirement, HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit);
      size_t clinit_check_index = invoke->InputCount() - 1u;
      DCHECK(invoke->InputAt(clinit_check_index) == nullptr);
      invoke->SetArgumentAt(clinit_check_index, clinit_check);
    }
  } else if (invoke_type == kVirtual) {
    DCHECK(target_method.dex_file == nullptr);
    invoke = new (allocator_) HInvokeVirtual(allocator_,
                                             number_of_arguments,
                                             return_type,
                                             dex_pc,
                                             method_idx,
                                             resolved_method,
                                             /*vtable_index=*/ target_method.index);
  } else {
    DCHECK_EQ(invoke_type, kInterface);
    invoke = new (allocator_) HInvokeInterface(allocator_,
                                               number_of_arguments,
                                               return_type,
                                               dex_pc,
                                               method_idx,
                                               resolved_method,
                                               /*imt_index=*/ target_method.index);
  }
  return HandleInvoke(invoke, operands, shorty, /* is_unresolved= */ false);
}

bool HInstructionBuilder::BuildInvokePolymorphic(uint32_t dex_pc,
                                                 uint32_t method_idx,
                                                 dex::ProtoIndex proto_idx,
                                                 const InstructionOperands& operands) {
  const char* shorty = dex_file_->GetShorty(proto_idx);
  DCHECK_EQ(1 + ArtMethod::NumArgRegisters(shorty), operands.GetNumberOfOperands());
  DataType::Type return_type = DataType::FromShorty(shorty[0]);
  size_t number_of_arguments = strlen(shorty);
  HInvoke* invoke = new (allocator_) HInvokePolymorphic(allocator_,
                                                        number_of_arguments,
                                                        return_type,
                                                        dex_pc,
                                                        method_idx);
  return HandleInvoke(invoke, operands, shorty, /* is_unresolved= */ false);
}


bool HInstructionBuilder::BuildInvokeCustom(uint32_t dex_pc,
                                            uint32_t call_site_idx,
                                            const InstructionOperands& operands) {
  dex::ProtoIndex proto_idx = dex_file_->GetProtoIndexForCallSite(call_site_idx);
  const char* shorty = dex_file_->GetShorty(proto_idx);
  DataType::Type return_type = DataType::FromShorty(shorty[0]);
  size_t number_of_arguments = strlen(shorty) - 1;
  HInvoke* invoke = new (allocator_) HInvokeCustom(allocator_,
                                                   number_of_arguments,
                                                   call_site_idx,
                                                   return_type,
                                                   dex_pc);
  return HandleInvoke(invoke, operands, shorty, /* is_unresolved= */ false);
}

HNewInstance* HInstructionBuilder::BuildNewInstance(dex::TypeIndex type_index, uint32_t dex_pc) {
  ScopedObjectAccess soa(Thread::Current());

  HLoadClass* load_class = BuildLoadClass(type_index, dex_pc);

  HInstruction* cls = load_class;
  Handle<mirror::Class> klass = load_class->GetClass();

  if (!IsInitialized(klass.Get())) {
    cls = new (allocator_) HClinitCheck(load_class, dex_pc);
    AppendInstruction(cls);
  }

  // Only the access check entrypoint handles the finalizable class case. If we
  // need access checks, then we haven't resolved the method and the class may
  // again be finalizable.
  QuickEntrypointEnum entrypoint = kQuickAllocObjectInitialized;
  if (load_class->NeedsAccessCheck() || klass->IsFinalizable() || !klass->IsInstantiable()) {
    entrypoint = kQuickAllocObjectWithChecks;
  }
  // We will always be able to resolve the string class since it is in the BCP.
  if (!klass.IsNull() && klass->IsStringClass()) {
    entrypoint = kQuickAllocStringObject;
  }

  // Consider classes we haven't resolved as potentially finalizable.
  bool finalizable = (klass == nullptr) || klass->IsFinalizable();

  HNewInstance* new_instance = new (allocator_) HNewInstance(
      cls,
      dex_pc,
      type_index,
      *dex_compilation_unit_->GetDexFile(),
      finalizable,
      entrypoint);
  AppendInstruction(new_instance);

  return new_instance;
}

void HInstructionBuilder::BuildConstructorFenceForAllocation(HInstruction* allocation) {
  DCHECK(allocation != nullptr &&
             (allocation->IsNewInstance() ||
              allocation->IsNewArray()));  // corresponding to "new" keyword in JLS.

  if (allocation->IsNewInstance()) {
    // STRING SPECIAL HANDLING:
    // -------------------------------
    // Strings have a real HNewInstance node but they end up always having 0 uses.
    // All uses of a String HNewInstance are always transformed to replace their input
    // of the HNewInstance with an input of the invoke to StringFactory.
    //
    // Do not emit an HConstructorFence here since it can inhibit some String new-instance
    // optimizations (to pass checker tests that rely on those optimizations).
    HNewInstance* new_inst = allocation->AsNewInstance();
    HLoadClass* load_class = new_inst->GetLoadClass();

    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> klass = load_class->GetClass();
    if (klass != nullptr && klass->IsStringClass()) {
      return;
      // Note: Do not use allocation->IsStringAlloc which requires
      // a valid ReferenceTypeInfo, but that doesn't get made until after reference type
      // propagation (and instruction builder is too early).
    }
    // (In terms of correctness, the StringFactory needs to provide its own
    // default initialization barrier, see below.)
  }

  // JLS 17.4.5 "Happens-before Order" describes:
  //
  //   The default initialization of any object happens-before any other actions (other than
  //   default-writes) of a program.
  //
  // In our implementation the default initialization of an object to type T means
  // setting all of its initial data (object[0..size)) to 0, and setting the
  // object's class header (i.e. object.getClass() == T.class).
  //
  // In practice this fence ensures that the writes to the object header
  // are visible to other threads if this object escapes the current thread.
  // (and in theory the 0-initializing, but that happens automatically
  // when new memory pages are mapped in by the OS).
  HConstructorFence* ctor_fence =
      new (allocator_) HConstructorFence(allocation, allocation->GetDexPc(), allocator_);
  AppendInstruction(ctor_fence);
  MaybeRecordStat(
      compilation_stats_,
      MethodCompilationStat::kConstructorFenceGeneratedNew);
}

static bool IsInBootImage(ObjPtr<mirror::Class> cls, const CompilerOptions& compiler_options)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(cls)) {
    return true;
  }
  if (compiler_options.IsBootImage() || compiler_options.IsBootImageExtension()) {
    std::string temp;
    const char* descriptor = cls->GetDescriptor(&temp);
    return compiler_options.IsImageClass(descriptor);
  } else {
    return false;
  }
}

static bool IsSubClass(ObjPtr<mirror::Class> to_test, ObjPtr<mirror::Class> super_class)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return to_test != nullptr && !to_test->IsInterface() && to_test->IsSubClass(super_class);
}

static bool HasTrivialClinit(ObjPtr<mirror::Class> klass, PointerSize pointer_size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Check if the class has encoded fields that trigger bytecode execution.
  // (Encoded fields are just a different representation of <clinit>.)
  if (klass->NumStaticFields() != 0u) {
    DCHECK(klass->GetClassDef() != nullptr);
    EncodedStaticFieldValueIterator it(klass->GetDexFile(), *klass->GetClassDef());
    for (; it.HasNext(); it.Next()) {
      switch (it.GetValueType()) {
        case EncodedArrayValueIterator::ValueType::kBoolean:
        case EncodedArrayValueIterator::ValueType::kByte:
        case EncodedArrayValueIterator::ValueType::kShort:
        case EncodedArrayValueIterator::ValueType::kChar:
        case EncodedArrayValueIterator::ValueType::kInt:
        case EncodedArrayValueIterator::ValueType::kLong:
        case EncodedArrayValueIterator::ValueType::kFloat:
        case EncodedArrayValueIterator::ValueType::kDouble:
        case EncodedArrayValueIterator::ValueType::kNull:
        case EncodedArrayValueIterator::ValueType::kString:
          // Primitive, null or j.l.String initialization is permitted.
          break;
        case EncodedArrayValueIterator::ValueType::kType:
          // Type initialization can load classes and execute bytecode through a class loader
          // which can execute arbitrary bytecode. We do not optimize for known class loaders;
          // kType is rarely used (if ever).
          return false;
        default:
          // Other types in the encoded static field list are rejected by the DexFileVerifier.
          LOG(FATAL) << "Unexpected type " << it.GetValueType();
          UNREACHABLE();
      }
    }
  }
  // Check if the class has <clinit> that executes arbitrary code.
  // Initialization of static fields of the class itself with constants is allowed.
  ArtMethod* clinit = klass->FindClassInitializer(pointer_size);
  if (clinit != nullptr) {
    const DexFile& dex_file = *clinit->GetDexFile();
    CodeItemInstructionAccessor accessor(dex_file, clinit->GetCodeItem());
    for (DexInstructionPcPair it : accessor) {
      switch (it->Opcode()) {
        case Instruction::CONST_4:
        case Instruction::CONST_16:
        case Instruction::CONST:
        case Instruction::CONST_HIGH16:
        case Instruction::CONST_WIDE_16:
        case Instruction::CONST_WIDE_32:
        case Instruction::CONST_WIDE:
        case Instruction::CONST_WIDE_HIGH16:
        case Instruction::CONST_STRING:
        case Instruction::CONST_STRING_JUMBO:
          // Primitive, null or j.l.String initialization is permitted.
          break;
        case Instruction::RETURN_VOID:
        case Instruction::RETURN_VOID_NO_BARRIER:
          break;
        case Instruction::SPUT:
        case Instruction::SPUT_WIDE:
        case Instruction::SPUT_OBJECT:
        case Instruction::SPUT_BOOLEAN:
        case Instruction::SPUT_BYTE:
        case Instruction::SPUT_CHAR:
        case Instruction::SPUT_SHORT:
          // Only initialization of a static field of the same class is permitted.
          if (dex_file.GetFieldId(it->VRegB_21c()).class_idx_ != klass->GetDexTypeIndex()) {
            return false;
          }
          break;
        case Instruction::NEW_ARRAY:
          // Only primitive arrays are permitted.
          if (Primitive::GetType(dex_file.GetTypeDescriptor(dex_file.GetTypeId(
                  dex::TypeIndex(it->VRegC_22c())))[1]) == Primitive::kPrimNot) {
            return false;
          }
          break;
        case Instruction::APUT:
        case Instruction::APUT_WIDE:
        case Instruction::APUT_BOOLEAN:
        case Instruction::APUT_BYTE:
        case Instruction::APUT_CHAR:
        case Instruction::APUT_SHORT:
        case Instruction::FILL_ARRAY_DATA:
        case Instruction::NOP:
          // Allow initialization of primitive arrays (only constants can be stored).
          // Note: We expect NOPs used for fill-array-data-payload but accept all NOPs
          // (even unreferenced switch payloads if they make it through the verifier).
          break;
        default:
          return false;
      }
    }
  }
  return true;
}

static bool HasTrivialInitialization(ObjPtr<mirror::Class> cls,
                                     const CompilerOptions& compiler_options)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  PointerSize pointer_size = runtime->GetClassLinker()->GetImagePointerSize();

  // Check the superclass chain.
  for (ObjPtr<mirror::Class> klass = cls; klass != nullptr; klass = klass->GetSuperClass()) {
    if (klass->IsInitialized() && IsInBootImage(klass, compiler_options)) {
      break;  // `klass` and its superclasses are already initialized in the boot image.
    }
    if (!HasTrivialClinit(klass, pointer_size)) {
      return false;
    }
  }

  // Also check interfaces with default methods as they need to be initialized as well.
  ObjPtr<mirror::IfTable> iftable = cls->GetIfTable();
  DCHECK(iftable != nullptr);
  for (int32_t i = 0, count = iftable->Count(); i != count; ++i) {
    ObjPtr<mirror::Class> iface = iftable->GetInterface(i);
    if (!iface->HasDefaultMethods()) {
      continue;  // Initializing `cls` does not initialize this interface.
    }
    if (iface->IsInitialized() && IsInBootImage(iface, compiler_options)) {
      continue;  // This interface is already initialized in the boot image.
    }
    if (!HasTrivialClinit(iface, pointer_size)) {
      return false;
    }
  }
  return true;
}

bool HInstructionBuilder::IsInitialized(ObjPtr<mirror::Class> cls) const {
  if (cls == nullptr) {
    return false;
  }

  // Check if the class will be initialized at runtime.
  if (cls->IsInitialized()) {
    const CompilerOptions& compiler_options = code_generator_->GetCompilerOptions();
    if (compiler_options.IsAotCompiler()) {
      // Assume loaded only if klass is in the boot image. App classes cannot be assumed
      // loaded because we don't even know what class loader will be used to load them.
      if (IsInBootImage(cls, compiler_options)) {
        return true;
      }
    } else {
      DCHECK(compiler_options.IsJitCompiler());
      if (Runtime::Current()->GetJit()->CanAssumeInitialized(
              cls,
              compiler_options.IsJitCompilerForSharedCode())) {
        // For JIT, the class cannot revert to an uninitialized state.
        return true;
      }
    }
  }

  // We can avoid the class initialization check for `cls` in static methods and constructors
  // in the very same class; invoking a static method involves a class initialization check
  // and so does the instance allocation that must be executed before invoking a constructor.
  // Other instance methods of the same class can run on an escaped instance
  // of an erroneous class. Even a superclass may need to be checked as the subclass
  // can be completely initialized while the superclass is initializing and the subclass
  // remains initialized when the superclass initializer throws afterwards. b/62478025
  // Note: The HClinitCheck+HInvokeStaticOrDirect merging can still apply.
  auto is_static_method_or_constructor_of_cls = [cls](const DexCompilationUnit& compilation_unit)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return (compilation_unit.GetAccessFlags() & (kAccStatic | kAccConstructor)) != 0u &&
           compilation_unit.GetCompilingClass().Get() == cls;
  };
  if (is_static_method_or_constructor_of_cls(*outer_compilation_unit_) ||
      // Check also the innermost method. Though excessive copies of ClinitCheck can be
      // eliminated by GVN, that happens only after the decision whether to inline the
      // graph or not and that may depend on the presence of the ClinitCheck.
      // TODO: We should walk over the entire inlined method chain, but we don't pass that
      // information to the builder.
      is_static_method_or_constructor_of_cls(*dex_compilation_unit_)) {
    return true;
  }

  // Otherwise, we may be able to avoid the check if `cls` is a superclass of a method being
  // compiled here (anywhere in the inlining chain) as the `cls` must have started initializing
  // before calling any `cls` or subclass methods. Static methods require a clinit check and
  // instance methods require an instance which cannot be created before doing a clinit check.
  // When a subclass of `cls` starts initializing, it starts initializing its superclass
  // chain up to `cls` without running any bytecode, i.e. without any opportunity for circular
  // initialization weirdness.
  //
  // If the initialization of `cls` is trivial (`cls` and its superclasses and superinterfaces
  // with default methods initialize only their own static fields using constant values), it must
  // complete, either successfully or by throwing and marking `cls` erroneous, without allocating
  // any instances of `cls` or subclasses (or any other class) and without calling any methods.
  // If it completes by throwing, no instances of `cls` shall be created and no subclass method
  // bytecode shall execute (see above), therefore the instruction we're building shall be
  // unreachable. By reaching the instruction, we know that `cls` was initialized successfully.
  //
  // TODO: We should walk over the entire inlined methods chain, but we don't pass that
  // information to the builder. (We could also check if we're guaranteed a non-null instance
  // of `cls` at this location but that's outside the scope of the instruction builder.)
  bool is_subclass = IsSubClass(outer_compilation_unit_->GetCompilingClass().Get(), cls);
  if (dex_compilation_unit_ != outer_compilation_unit_) {
    is_subclass = is_subclass ||
                  IsSubClass(dex_compilation_unit_->GetCompilingClass().Get(), cls);
  }
  if (is_subclass && HasTrivialInitialization(cls, code_generator_->GetCompilerOptions())) {
    return true;
  }

  return false;
}

HClinitCheck* HInstructionBuilder::ProcessClinitCheckForInvoke(
    uint32_t dex_pc,
    ArtMethod* resolved_method,
    HInvokeStaticOrDirect::ClinitCheckRequirement* clinit_check_requirement) {
  ScopedObjectAccess soa(Thread::Current());
  ObjPtr<mirror::Class> klass = resolved_method->GetDeclaringClass();

  HClinitCheck* clinit_check = nullptr;
  if (IsInitialized(klass)) {
    *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kNone;
  } else {
    Handle<mirror::Class> h_klass = graph_->GetHandleCache()->NewHandle(klass);
    HLoadClass* cls = BuildLoadClass(h_klass->GetDexTypeIndex(),
                                     h_klass->GetDexFile(),
                                     h_klass,
                                     dex_pc,
                                     /* needs_access_check= */ false);
    if (cls != nullptr) {
      *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit;
      clinit_check = new (allocator_) HClinitCheck(cls, dex_pc);
      AppendInstruction(clinit_check);
    } else {
      // Let the invoke handle this with an implicit class initialization check.
      *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit;
    }
  }
  return clinit_check;
}

bool HInstructionBuilder::SetupInvokeArguments(HInstruction* invoke,
                                               const InstructionOperands& operands,
                                               const char* shorty,
                                               ReceiverArg receiver_arg) {
  // Note: The `invoke` can be an intrinsic replacement, so not necessaritly HInvoke.
  // In that case, do not log errors, they shall be reported when we try to build the HInvoke.
  uint32_t shorty_index = 1;  // Skip the return type.
  const size_t number_of_operands = operands.GetNumberOfOperands();
  bool argument_length_error = false;

  size_t start_index = 0u;
  size_t argument_index = 0u;
  if (receiver_arg != ReceiverArg::kNone) {
    if (number_of_operands == 0u) {
      argument_length_error = true;
    } else {
      start_index = 1u;
      if (receiver_arg != ReceiverArg::kIgnored) {
        uint32_t obj_reg = operands.GetOperand(0u);
        HInstruction* arg = (receiver_arg == ReceiverArg::kPlainArg)
            ? LoadLocal(obj_reg, DataType::Type::kReference)
            : LoadNullCheckedLocal(obj_reg, invoke->GetDexPc());
        if (receiver_arg != ReceiverArg::kNullCheckedOnly) {
          invoke->SetRawInputAt(0u, arg);
          argument_index = 1u;
        }
      }
    }
  }

  for (size_t i = start_index; i < number_of_operands; ++i, ++argument_index) {
    // Make sure we don't go over the expected arguments or over the number of
    // dex registers given. If the instruction was seen as dead by the verifier,
    // it hasn't been properly checked.
    if (UNLIKELY(shorty[shorty_index] == 0)) {
      argument_length_error = true;
      break;
    }
    DataType::Type type = DataType::FromShorty(shorty[shorty_index++]);
    bool is_wide = (type == DataType::Type::kInt64) || (type == DataType::Type::kFloat64);
    if (is_wide && ((i + 1 == number_of_operands) ||
                    (operands.GetOperand(i) + 1 != operands.GetOperand(i + 1)))) {
      if (invoke->IsInvoke()) {
        // Longs and doubles should be in pairs, that is, sequential registers. The verifier should
        // reject any class where this is violated. However, the verifier only does these checks
        // on non trivially dead instructions, so we just bailout the compilation.
        VLOG(compiler) << "Did not compile "
                       << dex_file_->PrettyMethod(dex_compilation_unit_->GetDexMethodIndex())
                       << " because of non-sequential dex register pair in wide argument";
        MaybeRecordStat(compilation_stats_,
                        MethodCompilationStat::kNotCompiledMalformedOpcode);
      }
      return false;
    }
    HInstruction* arg = LoadLocal(operands.GetOperand(i), type);
    DCHECK(invoke->InputAt(argument_index) == nullptr);
    invoke->SetRawInputAt(argument_index, arg);
    if (is_wide) {
      ++i;
    }
  }

  argument_length_error = argument_length_error || shorty[shorty_index] != 0;
  if (argument_length_error) {
    if (invoke->IsInvoke()) {
      VLOG(compiler) << "Did not compile "
                     << dex_file_->PrettyMethod(dex_compilation_unit_->GetDexMethodIndex())
                     << " because of wrong number of arguments in invoke instruction";
      MaybeRecordStat(compilation_stats_,
                      MethodCompilationStat::kNotCompiledMalformedOpcode);
    }
    return false;
  }

  if (invoke->IsInvokeStaticOrDirect() &&
      HInvokeStaticOrDirect::NeedsCurrentMethodInput(
          invoke->AsInvokeStaticOrDirect()->GetMethodLoadKind())) {
    DCHECK_EQ(argument_index, invoke->AsInvokeStaticOrDirect()->GetSpecialInputIndex());
    DCHECK(invoke->InputAt(argument_index) == nullptr);
    invoke->SetRawInputAt(argument_index, graph_->GetCurrentMethod());
  }

  return true;
}

bool HInstructionBuilder::HandleInvoke(HInvoke* invoke,
                                       const InstructionOperands& operands,
                                       const char* shorty,
                                       bool is_unresolved) {
  DCHECK(!invoke->IsInvokeStaticOrDirect() || !invoke->AsInvokeStaticOrDirect()->IsStringInit());

  ReceiverArg receiver_arg = (invoke->GetInvokeType() == InvokeType::kStatic)
      ? ReceiverArg::kNone
      : (is_unresolved ? ReceiverArg::kPlainArg : ReceiverArg::kNullCheckedArg);
  if (!SetupInvokeArguments(invoke, operands, shorty, receiver_arg)) {
    return false;
  }

  AppendInstruction(invoke);
  latest_result_ = invoke;

  return true;
}

bool HInstructionBuilder::BuildSimpleIntrinsic(ArtMethod* method,
                                               uint32_t dex_pc,
                                               const InstructionOperands& operands,
                                               const char* shorty) {
  Intrinsics intrinsic = static_cast<Intrinsics>(method->GetIntrinsic());
  DCHECK_NE(intrinsic, Intrinsics::kNone);
  constexpr DataType::Type kInt32 = DataType::Type::kInt32;
  constexpr DataType::Type kInt64 = DataType::Type::kInt64;
  constexpr DataType::Type kFloat32 = DataType::Type::kFloat32;
  constexpr DataType::Type kFloat64 = DataType::Type::kFloat64;
  ReceiverArg receiver_arg = method->IsStatic() ? ReceiverArg::kNone : ReceiverArg::kNullCheckedArg;
  HInstruction* instruction = nullptr;
  switch (intrinsic) {
    case Intrinsics::kIntegerRotateRight:
    case Intrinsics::kIntegerRotateLeft:
      // For rotate left, we negate the distance below.
      instruction = new (allocator_) HRor(kInt32, /*value=*/ nullptr, /*distance=*/ nullptr);
      break;
    case Intrinsics::kLongRotateRight:
    case Intrinsics::kLongRotateLeft:
      // For rotate left, we negate the distance below.
      instruction = new (allocator_) HRor(kInt64, /*value=*/ nullptr, /*distance=*/ nullptr);
      break;
    case Intrinsics::kIntegerCompare:
      instruction = new (allocator_) HCompare(
          kInt32, /*first=*/ nullptr, /*second=*/ nullptr, ComparisonBias::kNoBias, dex_pc);
      break;
    case Intrinsics::kLongCompare:
      instruction = new (allocator_) HCompare(
          kInt64, /*first=*/ nullptr, /*second=*/ nullptr, ComparisonBias::kNoBias, dex_pc);
      break;
    case Intrinsics::kIntegerSignum:
      instruction = new (allocator_) HCompare(
          kInt32, /*first=*/ nullptr, graph_->GetIntConstant(0), ComparisonBias::kNoBias, dex_pc);
      break;
    case Intrinsics::kLongSignum:
      instruction = new (allocator_) HCompare(
          kInt64, /*first=*/ nullptr, graph_->GetLongConstant(0), ComparisonBias::kNoBias, dex_pc);
      break;
    case Intrinsics::kFloatIsNaN:
    case Intrinsics::kDoubleIsNaN: {
      // IsNaN(x) is the same as x != x.
      instruction = new (allocator_) HNotEqual(/*first=*/ nullptr, /*second=*/ nullptr, dex_pc);
      instruction->AsCondition()->SetBias(ComparisonBias::kLtBias);
      break;
    }
    case Intrinsics::kStringCharAt:
      // We treat String as an array to allow DCE and BCE to seamlessly work on strings.
      instruction = new (allocator_) HArrayGet(/*array=*/ nullptr,
                                               /*index=*/ nullptr,
                                               DataType::Type::kUint16,
                                               SideEffects::None(),  // Strings are immutable.
                                               dex_pc,
                                               /*is_string_char_at=*/ true);
      break;
    case Intrinsics::kStringIsEmpty:
    case Intrinsics::kStringLength:
      // We treat String as an array to allow DCE and BCE to seamlessly work on strings.
      // For String.isEmpty(), we add a comparison with 0 below.
      instruction =
          new (allocator_) HArrayLength(/*array=*/ nullptr, dex_pc, /* is_string_length= */ true);
      break;
    case Intrinsics::kUnsafeLoadFence:
      receiver_arg = ReceiverArg::kNullCheckedOnly;
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kLoadAny, dex_pc);
      break;
    case Intrinsics::kUnsafeStoreFence:
      receiver_arg = ReceiverArg::kNullCheckedOnly;
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kAnyStore, dex_pc);
      break;
    case Intrinsics::kUnsafeFullFence:
      receiver_arg = ReceiverArg::kNullCheckedOnly;
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kAnyAny, dex_pc);
      break;
    case Intrinsics::kVarHandleFullFence:
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kAnyAny, dex_pc);
      break;
    case Intrinsics::kVarHandleAcquireFence:
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kLoadAny, dex_pc);
      break;
    case Intrinsics::kVarHandleReleaseFence:
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kAnyStore, dex_pc);
      break;
    case Intrinsics::kVarHandleLoadLoadFence:
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kLoadAny, dex_pc);
      break;
    case Intrinsics::kVarHandleStoreStoreFence:
      instruction = new (allocator_) HMemoryBarrier(MemBarrierKind::kStoreStore, dex_pc);
      break;
    case Intrinsics::kMathMinIntInt:
      instruction = new (allocator_) HMin(kInt32, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMinLongLong:
      instruction = new (allocator_) HMin(kInt64, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMinFloatFloat:
      instruction = new (allocator_) HMin(kFloat32, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMinDoubleDouble:
      instruction = new (allocator_) HMin(kFloat64, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMaxIntInt:
      instruction = new (allocator_) HMax(kInt32, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMaxLongLong:
      instruction = new (allocator_) HMax(kInt64, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMaxFloatFloat:
      instruction = new (allocator_) HMax(kFloat32, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathMaxDoubleDouble:
      instruction = new (allocator_) HMax(kFloat64, /*left=*/ nullptr, /*right=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathAbsInt:
      instruction = new (allocator_) HAbs(kInt32, /*input=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathAbsLong:
      instruction = new (allocator_) HAbs(kInt64, /*input=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathAbsFloat:
      instruction = new (allocator_) HAbs(kFloat32, /*input=*/ nullptr, dex_pc);
      break;
    case Intrinsics::kMathAbsDouble:
      instruction = new (allocator_) HAbs(kFloat64, /*input=*/ nullptr, dex_pc);
      break;
    default:
      // We do not have intermediate representation for other intrinsics.
      return false;
  }
  DCHECK(instruction != nullptr);
  if (!SetupInvokeArguments(instruction, operands, shorty, receiver_arg)) {
    return false;
  }

  switch (intrinsic) {
    case Intrinsics::kIntegerRotateLeft:
    case Intrinsics::kLongRotateLeft: {
      // Negate the distance value for rotate left.
      DCHECK(instruction->IsRor());
      HNeg* neg = new (allocator_) HNeg(kInt32, instruction->InputAt(1u));
      AppendInstruction(neg);
      instruction->SetRawInputAt(1u, neg);
      break;
    }
    case Intrinsics::kFloatIsNaN:
    case Intrinsics::kDoubleIsNaN:
      // Set the second input to be the same as first.
      DCHECK(instruction->IsNotEqual());
      DCHECK(instruction->InputAt(1u) == nullptr);
      instruction->SetRawInputAt(1u, instruction->InputAt(0u));
      break;
    case Intrinsics::kStringCharAt: {
      // Add bounds check.
      HInstruction* array = instruction->InputAt(0u);
      HInstruction* index = instruction->InputAt(1u);
      HInstruction* length =
          new (allocator_) HArrayLength(array, dex_pc, /*is_string_length=*/ true);
      AppendInstruction(length);
      HBoundsCheck* bounds_check =
          new (allocator_) HBoundsCheck(index, length, dex_pc, /*is_string_char_at=*/ true);
      AppendInstruction(bounds_check);
      graph_->SetHasBoundsChecks(true);
      instruction->SetRawInputAt(1u, bounds_check);
      break;
    }
    case Intrinsics::kStringIsEmpty: {
      // Compare the length with 0.
      DCHECK(instruction->IsArrayLength());
      AppendInstruction(instruction);
      HEqual* equal = new (allocator_) HEqual(instruction, graph_->GetIntConstant(0), dex_pc);
      instruction = equal;
      break;
    }
    default:
      break;
  }

  AppendInstruction(instruction);
  latest_result_ = instruction;

  return true;
}

bool HInstructionBuilder::HandleStringInit(HInvoke* invoke,
                                           const InstructionOperands& operands,
                                           const char* shorty) {
  DCHECK(invoke->IsInvokeStaticOrDirect());
  DCHECK(invoke->AsInvokeStaticOrDirect()->IsStringInit());

  if (!SetupInvokeArguments(invoke, operands, shorty, ReceiverArg::kIgnored)) {
    return false;
  }

  AppendInstruction(invoke);

  // This is a StringFactory call, not an actual String constructor. Its result
  // replaces the empty String pre-allocated by NewInstance.
  uint32_t orig_this_reg = operands.GetOperand(0);
  HInstruction* arg_this = LoadLocal(orig_this_reg, DataType::Type::kReference);

  // Replacing the NewInstance might render it redundant. Keep a list of these
  // to be visited once it is clear whether it has remaining uses.
  if (arg_this->IsNewInstance()) {
    ssa_builder_->AddUninitializedString(arg_this->AsNewInstance());
  } else {
    DCHECK(arg_this->IsPhi());
    // We can get a phi as input of a String.<init> if there is a loop between the
    // allocation and the String.<init> call. As we don't know which other phis might alias
    // with `arg_this`, we keep a record of those invocations so we can later replace
    // the allocation with the invocation.
    // Add the actual 'this' input so the analysis knows what is the allocation instruction.
    // The input will be removed during the analysis.
    invoke->AddInput(arg_this);
    ssa_builder_->AddUninitializedStringPhi(invoke);
  }
  // Walk over all vregs and replace any occurrence of `arg_this` with `invoke`.
  for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
    if ((*current_locals_)[vreg] == arg_this) {
      (*current_locals_)[vreg] = invoke;
    }
  }
  return true;
}

static DataType::Type GetFieldAccessType(const DexFile& dex_file, uint16_t field_index) {
  const dex::FieldId& field_id = dex_file.GetFieldId(field_index);
  const char* type = dex_file.GetFieldTypeDescriptor(field_id);
  return DataType::FromShorty(type[0]);
}

bool HInstructionBuilder::BuildInstanceFieldAccess(const Instruction& instruction,
                                                   uint32_t dex_pc,
                                                   bool is_put,
                                                   size_t quicken_index) {
  uint32_t source_or_dest_reg = instruction.VRegA_22c();
  uint32_t obj_reg = instruction.VRegB_22c();
  uint16_t field_index;
  if (instruction.IsQuickened()) {
    if (!CanDecodeQuickenedInfo()) {
      VLOG(compiler) << "Not compiled: Could not decode quickened instruction "
                     << instruction.Opcode();
      return false;
    }
    field_index = LookupQuickenedInfo(quicken_index);
  } else {
    field_index = instruction.VRegC_22c();
  }

  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field = ResolveField(field_index, /* is_static= */ false, is_put);

  // Generate an explicit null check on the reference, unless the field access
  // is unresolved. In that case, we rely on the runtime to perform various
  // checks first, followed by a null check.
  HInstruction* object = (resolved_field == nullptr)
      ? LoadLocal(obj_reg, DataType::Type::kReference)
      : LoadNullCheckedLocal(obj_reg, dex_pc);

  DataType::Type field_type = GetFieldAccessType(*dex_file_, field_index);
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    HInstruction* field_set = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(compilation_stats_,
                      MethodCompilationStat::kUnresolvedField);
      field_set = new (allocator_) HUnresolvedInstanceFieldSet(object,
                                                               value,
                                                               field_type,
                                                               field_index,
                                                               dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_set = new (allocator_) HInstanceFieldSet(object,
                                                     value,
                                                     resolved_field,
                                                     field_type,
                                                     resolved_field->GetOffset(),
                                                     resolved_field->IsVolatile(),
                                                     field_index,
                                                     class_def_index,
                                                     *dex_file_,
                                                     dex_pc);
    }
    AppendInstruction(field_set);
  } else {
    HInstruction* field_get = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(compilation_stats_,
                      MethodCompilationStat::kUnresolvedField);
      field_get = new (allocator_) HUnresolvedInstanceFieldGet(object,
                                                               field_type,
                                                               field_index,
                                                               dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_get = new (allocator_) HInstanceFieldGet(object,
                                                     resolved_field,
                                                     field_type,
                                                     resolved_field->GetOffset(),
                                                     resolved_field->IsVolatile(),
                                                     field_index,
                                                     class_def_index,
                                                     *dex_file_,
                                                     dex_pc);
    }
    AppendInstruction(field_get);
    UpdateLocal(source_or_dest_reg, field_get);
  }

  return true;
}

void HInstructionBuilder::BuildUnresolvedStaticFieldAccess(const Instruction& instruction,
                                                           uint32_t dex_pc,
                                                           bool is_put,
                                                           DataType::Type field_type) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();

  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    AppendInstruction(
        new (allocator_) HUnresolvedStaticFieldSet(value, field_type, field_index, dex_pc));
  } else {
    AppendInstruction(new (allocator_) HUnresolvedStaticFieldGet(field_type, field_index, dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
}

ArtField* HInstructionBuilder::ResolveField(uint16_t field_idx, bool is_static, bool is_put) {
  ScopedObjectAccess soa(Thread::Current());

  ClassLinker* class_linker = dex_compilation_unit_->GetClassLinker();
  Handle<mirror::ClassLoader> class_loader = dex_compilation_unit_->GetClassLoader();

  ArtField* resolved_field = class_linker->ResolveField(field_idx,
                                                        dex_compilation_unit_->GetDexCache(),
                                                        class_loader,
                                                        is_static);
  DCHECK_EQ(resolved_field == nullptr, soa.Self()->IsExceptionPending())
      << "field="
      << ((resolved_field == nullptr) ? "null" : resolved_field->PrettyField())
      << ", exception="
      << (soa.Self()->IsExceptionPending() ? soa.Self()->GetException()->Dump() : "null");
  if (UNLIKELY(resolved_field == nullptr)) {
    // Clean up any exception left by field resolution.
    soa.Self()->ClearException();
    return nullptr;
  }

  // Check static/instance. The class linker has a fast path for looking into the dex cache
  // and does not check static/instance if it hits it.
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    return nullptr;
  }

  // Check access.
  Handle<mirror::Class> compiling_class = dex_compilation_unit_->GetCompilingClass();
  if (compiling_class == nullptr) {
    if (!resolved_field->IsPublic()) {
      return nullptr;
    }
  } else if (!compiling_class->CanAccessResolvedField(resolved_field->GetDeclaringClass(),
                                                      resolved_field,
                                                      dex_compilation_unit_->GetDexCache().Get(),
                                                      field_idx)) {
    return nullptr;
  }

  if (is_put &&
      resolved_field->IsFinal() &&
      (compiling_class.Get() != resolved_field->GetDeclaringClass())) {
    // Final fields can only be updated within their own class.
    // TODO: Only allow it in constructors. b/34966607.
    return nullptr;
  }

  return resolved_field;
}

void HInstructionBuilder::BuildStaticFieldAccess(const Instruction& instruction,
                                                 uint32_t dex_pc,
                                                 bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();

  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field = ResolveField(field_index, /* is_static= */ true, is_put);

  if (resolved_field == nullptr) {
    MaybeRecordStat(compilation_stats_,
                    MethodCompilationStat::kUnresolvedField);
    DataType::Type field_type = GetFieldAccessType(*dex_file_, field_index);
    BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
    return;
  }

  DataType::Type field_type = GetFieldAccessType(*dex_file_, field_index);

  Handle<mirror::Class> klass =
      graph_->GetHandleCache()->NewHandle(resolved_field->GetDeclaringClass());
  HLoadClass* constant = BuildLoadClass(klass->GetDexTypeIndex(),
                                        klass->GetDexFile(),
                                        klass,
                                        dex_pc,
                                        /* needs_access_check= */ false);

  if (constant == nullptr) {
    // The class cannot be referenced from this compiled code. Generate
    // an unresolved access.
    MaybeRecordStat(compilation_stats_,
                    MethodCompilationStat::kUnresolvedFieldNotAFastAccess);
    BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
    return;
  }

  HInstruction* cls = constant;
  if (!IsInitialized(klass.Get())) {
    cls = new (allocator_) HClinitCheck(constant, dex_pc);
    AppendInstruction(cls);
  }

  uint16_t class_def_index = klass->GetDexClassDefIndex();
  if (is_put) {
    // We need to keep the class alive before loading the value.
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    DCHECK_EQ(HPhi::ToPhiType(value->GetType()), HPhi::ToPhiType(field_type));
    AppendInstruction(new (allocator_) HStaticFieldSet(cls,
                                                       value,
                                                       resolved_field,
                                                       field_type,
                                                       resolved_field->GetOffset(),
                                                       resolved_field->IsVolatile(),
                                                       field_index,
                                                       class_def_index,
                                                       *dex_file_,
                                                       dex_pc));
  } else {
    AppendInstruction(new (allocator_) HStaticFieldGet(cls,
                                                       resolved_field,
                                                       field_type,
                                                       resolved_field->GetOffset(),
                                                       resolved_field->IsVolatile(),
                                                       field_index,
                                                       class_def_index,
                                                       *dex_file_,
                                                       dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
}

void HInstructionBuilder::BuildCheckedDivRem(uint16_t out_vreg,
                                             uint16_t first_vreg,
                                             int64_t second_vreg_or_constant,
                                             uint32_t dex_pc,
                                             DataType::Type type,
                                             bool second_is_constant,
                                             bool isDiv) {
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);

  HInstruction* first = LoadLocal(first_vreg, type);
  HInstruction* second = nullptr;
  if (second_is_constant) {
    if (type == DataType::Type::kInt32) {
      second = graph_->GetIntConstant(second_vreg_or_constant, dex_pc);
    } else {
      second = graph_->GetLongConstant(second_vreg_or_constant, dex_pc);
    }
  } else {
    second = LoadLocal(second_vreg_or_constant, type);
  }

  if (!second_is_constant
      || (type == DataType::Type::kInt32 && second->AsIntConstant()->GetValue() == 0)
      || (type == DataType::Type::kInt64 && second->AsLongConstant()->GetValue() == 0)) {
    second = new (allocator_) HDivZeroCheck(second, dex_pc);
    AppendInstruction(second);
  }

  if (isDiv) {
    AppendInstruction(new (allocator_) HDiv(type, first, second, dex_pc));
  } else {
    AppendInstruction(new (allocator_) HRem(type, first, second, dex_pc));
  }
  UpdateLocal(out_vreg, current_block_->GetLastInstruction());
}

void HInstructionBuilder::BuildArrayAccess(const Instruction& instruction,
                                           uint32_t dex_pc,
                                           bool is_put,
                                           DataType::Type anticipated_type) {
  uint8_t source_or_dest_reg = instruction.VRegA_23x();
  uint8_t array_reg = instruction.VRegB_23x();
  uint8_t index_reg = instruction.VRegC_23x();

  HInstruction* object = LoadNullCheckedLocal(array_reg, dex_pc);
  HInstruction* length = new (allocator_) HArrayLength(object, dex_pc);
  AppendInstruction(length);
  HInstruction* index = LoadLocal(index_reg, DataType::Type::kInt32);
  index = new (allocator_) HBoundsCheck(index, length, dex_pc);
  AppendInstruction(index);
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, anticipated_type);
    // TODO: Insert a type check node if the type is Object.
    HArraySet* aset = new (allocator_) HArraySet(object, index, value, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  } else {
    HArrayGet* aget = new (allocator_) HArrayGet(object, index, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArrayGet(aget);
    AppendInstruction(aget);
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
  graph_->SetHasBoundsChecks(true);
}

HNewArray* HInstructionBuilder::BuildNewArray(uint32_t dex_pc,
                                              dex::TypeIndex type_index,
                                              HInstruction* length) {
  HLoadClass* cls = BuildLoadClass(type_index, dex_pc);

  const char* descriptor = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(type_index));
  DCHECK_EQ(descriptor[0], '[');
  size_t component_type_shift = Primitive::ComponentSizeShift(Primitive::GetType(descriptor[1]));

  HNewArray* new_array = new (allocator_) HNewArray(cls, length, dex_pc, component_type_shift);
  AppendInstruction(new_array);
  return new_array;
}

HNewArray* HInstructionBuilder::BuildFilledNewArray(uint32_t dex_pc,
                                                    dex::TypeIndex type_index,
                                                    const InstructionOperands& operands) {
  const size_t number_of_operands = operands.GetNumberOfOperands();
  HInstruction* length = graph_->GetIntConstant(number_of_operands, dex_pc);

  HNewArray* new_array = BuildNewArray(dex_pc, type_index, length);
  const char* descriptor = dex_file_->StringByTypeIdx(type_index);
  DCHECK_EQ(descriptor[0], '[') << descriptor;
  char primitive = descriptor[1];
  DCHECK(primitive == 'I'
      || primitive == 'L'
      || primitive == '[') << descriptor;
  bool is_reference_array = (primitive == 'L') || (primitive == '[');
  DataType::Type type = is_reference_array ? DataType::Type::kReference : DataType::Type::kInt32;

  for (size_t i = 0; i < number_of_operands; ++i) {
    HInstruction* value = LoadLocal(operands.GetOperand(i), type);
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HArraySet* aset = new (allocator_) HArraySet(new_array, index, value, type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
  latest_result_ = new_array;

  return new_array;
}

template <typename T>
void HInstructionBuilder::BuildFillArrayData(HInstruction* object,
                                             const T* data,
                                             uint32_t element_count,
                                             DataType::Type anticipated_type,
                                             uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetIntConstant(data[i], dex_pc);
    HArraySet* aset = new (allocator_) HArraySet(object, index, value, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
}

void HInstructionBuilder::BuildFillArrayData(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* array = LoadNullCheckedLocal(instruction.VRegA_31t(), dex_pc);

  int32_t payload_offset = instruction.VRegB_31t() + dex_pc;
  const Instruction::ArrayDataPayload* payload =
      reinterpret_cast<const Instruction::ArrayDataPayload*>(
          code_item_accessor_.Insns() + payload_offset);
  const uint8_t* data = payload->data;
  uint32_t element_count = payload->element_count;

  if (element_count == 0u) {
    // For empty payload we emit only the null check above.
    return;
  }

  HInstruction* length = new (allocator_) HArrayLength(array, dex_pc);
  AppendInstruction(length);

  // Implementation of this DEX instruction seems to be that the bounds check is
  // done before doing any stores.
  HInstruction* last_index = graph_->GetIntConstant(payload->element_count - 1, dex_pc);
  AppendInstruction(new (allocator_) HBoundsCheck(last_index, length, dex_pc));

  switch (payload->element_width) {
    case 1:
      BuildFillArrayData(array,
                         reinterpret_cast<const int8_t*>(data),
                         element_count,
                         DataType::Type::kInt8,
                         dex_pc);
      break;
    case 2:
      BuildFillArrayData(array,
                         reinterpret_cast<const int16_t*>(data),
                         element_count,
                         DataType::Type::kInt16,
                         dex_pc);
      break;
    case 4:
      BuildFillArrayData(array,
                         reinterpret_cast<const int32_t*>(data),
                         element_count,
                         DataType::Type::kInt32,
                         dex_pc);
      break;
    case 8:
      BuildFillWideArrayData(array,
                             reinterpret_cast<const int64_t*>(data),
                             element_count,
                             dex_pc);
      break;
    default:
      LOG(FATAL) << "Unknown element width for " << payload->element_width;
  }
  graph_->SetHasBoundsChecks(true);
}

void HInstructionBuilder::BuildFillWideArrayData(HInstruction* object,
                                                 const int64_t* data,
                                                 uint32_t element_count,
                                                 uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetLongConstant(data[i], dex_pc);
    HArraySet* aset =
        new (allocator_) HArraySet(object, index, value, DataType::Type::kInt64, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
}

void HInstructionBuilder::BuildLoadString(dex::StringIndex string_index, uint32_t dex_pc) {
  HLoadString* load_string =
      new (allocator_) HLoadString(graph_->GetCurrentMethod(), string_index, *dex_file_, dex_pc);
  HSharpening::ProcessLoadString(load_string,
                                 code_generator_,
                                 *dex_compilation_unit_,
                                 graph_->GetHandleCache()->GetHandles());
  AppendInstruction(load_string);
}

HLoadClass* HInstructionBuilder::BuildLoadClass(dex::TypeIndex type_index, uint32_t dex_pc) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  Handle<mirror::Class> klass = ResolveClass(soa, type_index);
  bool needs_access_check = LoadClassNeedsAccessCheck(klass.Get());
  return BuildLoadClass(type_index, dex_file, klass, dex_pc, needs_access_check);
}

HLoadClass* HInstructionBuilder::BuildLoadClass(dex::TypeIndex type_index,
                                                const DexFile& dex_file,
                                                Handle<mirror::Class> klass,
                                                uint32_t dex_pc,
                                                bool needs_access_check) {
  // Try to find a reference in the compiling dex file.
  const DexFile* actual_dex_file = &dex_file;
  if (!IsSameDexFile(dex_file, *dex_compilation_unit_->GetDexFile())) {
    dex::TypeIndex local_type_index =
        klass->FindTypeIndexInOtherDexFile(*dex_compilation_unit_->GetDexFile());
    if (local_type_index.IsValid()) {
      type_index = local_type_index;
      actual_dex_file = dex_compilation_unit_->GetDexFile();
    }
  }

  // Note: `klass` must be from `graph_->GetHandleCache()`.
  bool is_referrers_class =
      (klass != nullptr) && (outer_compilation_unit_->GetCompilingClass().Get() == klass.Get());
  HLoadClass* load_class = new (allocator_) HLoadClass(
      graph_->GetCurrentMethod(),
      type_index,
      *actual_dex_file,
      klass,
      is_referrers_class,
      dex_pc,
      needs_access_check);

  HLoadClass::LoadKind load_kind = HSharpening::ComputeLoadClassKind(load_class,
                                                                     code_generator_,
                                                                     *dex_compilation_unit_);

  if (load_kind == HLoadClass::LoadKind::kInvalid) {
    // We actually cannot reference this class, we're forced to bail.
    return nullptr;
  }
  // Load kind must be set before inserting the instruction into the graph.
  load_class->SetLoadKind(load_kind);
  AppendInstruction(load_class);
  return load_class;
}

Handle<mirror::Class> HInstructionBuilder::ResolveClass(ScopedObjectAccess& soa,
                                                        dex::TypeIndex type_index) {
  auto it = class_cache_.find(type_index);
  if (it != class_cache_.end()) {
    return it->second;
  }

  ObjPtr<mirror::Class> klass = dex_compilation_unit_->GetClassLinker()->ResolveType(
      type_index, dex_compilation_unit_->GetDexCache(), dex_compilation_unit_->GetClassLoader());
  DCHECK_EQ(klass == nullptr, soa.Self()->IsExceptionPending());
  soa.Self()->ClearException();  // Clean up the exception left by type resolution if any.

  Handle<mirror::Class> h_klass = graph_->GetHandleCache()->NewHandle(klass);
  class_cache_.Put(type_index, h_klass);
  return h_klass;
}

bool HInstructionBuilder::LoadClassNeedsAccessCheck(ObjPtr<mirror::Class> klass) {
  if (klass == nullptr) {
    return true;
  } else if (klass->IsPublic()) {
    return false;
  } else {
    ObjPtr<mirror::Class> compiling_class = dex_compilation_unit_->GetCompilingClass().Get();
    return compiling_class == nullptr || !compiling_class->CanAccess(klass);
  }
}

void HInstructionBuilder::BuildLoadMethodHandle(uint16_t method_handle_index, uint32_t dex_pc) {
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  HLoadMethodHandle* load_method_handle = new (allocator_) HLoadMethodHandle(
      graph_->GetCurrentMethod(), method_handle_index, dex_file, dex_pc);
  AppendInstruction(load_method_handle);
}

void HInstructionBuilder::BuildLoadMethodType(dex::ProtoIndex proto_index, uint32_t dex_pc) {
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  HLoadMethodType* load_method_type =
      new (allocator_) HLoadMethodType(graph_->GetCurrentMethod(), proto_index, dex_file, dex_pc);
  AppendInstruction(load_method_type);
}

void HInstructionBuilder::BuildTypeCheck(const Instruction& instruction,
                                         uint8_t destination,
                                         uint8_t reference,
                                         dex::TypeIndex type_index,
                                         uint32_t dex_pc) {
  HInstruction* object = LoadLocal(reference, DataType::Type::kReference);

  ScopedObjectAccess soa(Thread::Current());
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  Handle<mirror::Class> klass = ResolveClass(soa, type_index);
  bool needs_access_check = LoadClassNeedsAccessCheck(klass.Get());
  TypeCheckKind check_kind = HSharpening::ComputeTypeCheckKind(
      klass.Get(), code_generator_, needs_access_check);

  HInstruction* class_or_null = nullptr;
  HIntConstant* bitstring_path_to_root = nullptr;
  HIntConstant* bitstring_mask = nullptr;
  if (check_kind == TypeCheckKind::kBitstringCheck) {
    // TODO: Allow using the bitstring check also if we need an access check.
    DCHECK(!needs_access_check);
    class_or_null = graph_->GetNullConstant(dex_pc);
    MutexLock subtype_check_lock(Thread::Current(), *Locks::subtype_check_lock_);
    uint32_t path_to_root =
        SubtypeCheck<ObjPtr<mirror::Class>>::GetEncodedPathToRootForTarget(klass.Get());
    uint32_t mask = SubtypeCheck<ObjPtr<mirror::Class>>::GetEncodedPathToRootMask(klass.Get());
    bitstring_path_to_root = graph_->GetIntConstant(static_cast<int32_t>(path_to_root), dex_pc);
    bitstring_mask = graph_->GetIntConstant(static_cast<int32_t>(mask), dex_pc);
  } else {
    class_or_null = BuildLoadClass(type_index, dex_file, klass, dex_pc, needs_access_check);
  }
  DCHECK(class_or_null != nullptr);

  if (instruction.Opcode() == Instruction::INSTANCE_OF) {
    AppendInstruction(new (allocator_) HInstanceOf(object,
                                                   class_or_null,
                                                   check_kind,
                                                   klass,
                                                   dex_pc,
                                                   allocator_,
                                                   bitstring_path_to_root,
                                                   bitstring_mask));
    UpdateLocal(destination, current_block_->GetLastInstruction());
  } else {
    DCHECK_EQ(instruction.Opcode(), Instruction::CHECK_CAST);
    // We emit a CheckCast followed by a BoundType. CheckCast is a statement
    // which may throw. If it succeeds BoundType sets the new type of `object`
    // for all subsequent uses.
    AppendInstruction(
        new (allocator_) HCheckCast(object,
                                    class_or_null,
                                    check_kind,
                                    klass,
                                    dex_pc,
                                    allocator_,
                                    bitstring_path_to_root,
                                    bitstring_mask));
    AppendInstruction(new (allocator_) HBoundType(object, dex_pc));
    UpdateLocal(reference, current_block_->GetLastInstruction());
  }
}

bool HInstructionBuilder::CanDecodeQuickenedInfo() const {
  return !quicken_info_.IsNull();
}

uint16_t HInstructionBuilder::LookupQuickenedInfo(uint32_t quicken_index) {
  DCHECK(CanDecodeQuickenedInfo());
  return quicken_info_.GetData(quicken_index);
}

bool HInstructionBuilder::ProcessDexInstruction(const Instruction& instruction,
                                                uint32_t dex_pc,
                                                size_t quicken_index) {
  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_11n(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21s(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_31i(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_HIGH16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21h() << 16, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_16: {
      int32_t register_index = instruction.VRegA();
      // Get 16 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_21s();
      value <<= 48;
      value >>= 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_32: {
      int32_t register_index = instruction.VRegA();
      // Get 32 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_31i();
      value <<= 32;
      value >>= 32;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE: {
      int32_t register_index = instruction.VRegA();
      HLongConstant* constant = graph_->GetLongConstant(instruction.VRegB_51l(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_HIGH16: {
      int32_t register_index = instruction.VRegA();
      int64_t value = static_cast<int64_t>(instruction.VRegB_21h()) << 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }

    // Note that the SSA building will refine the types.
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), DataType::Type::kInt32);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    // Note that the SSA building will refine the types.
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), DataType::Type::kInt64);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_OBJECT_FROM16: {
      // The verifier has no notion of a null type, so a move-object of constant 0
      // will lead to the same constant 0 in the destination register. To mimic
      // this behavior, we just pretend we haven't seen a type change (int to reference)
      // for the 0 constant and phis. We rely on our type propagation to eventually get the
      // types correct.
      uint32_t reg_number = instruction.VRegB();
      HInstruction* value = (*current_locals_)[reg_number];
      if (value->IsIntConstant()) {
        DCHECK_EQ(value->AsIntConstant()->GetValue(), 0);
      } else if (value->IsPhi()) {
        DCHECK(value->GetType() == DataType::Type::kInt32 ||
               value->GetType() == DataType::Type::kReference);
      } else {
        value = LoadLocal(reg_number, DataType::Type::kReference);
      }
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    case Instruction::RETURN_VOID_NO_BARRIER:
    case Instruction::RETURN_VOID: {
      BuildReturn(instruction, DataType::Type::kVoid, dex_pc);
      break;
    }

#define IF_XX(comparison, cond) \
    case Instruction::IF_##cond: If_22t<comparison>(instruction, dex_pc); break; \
    case Instruction::IF_##cond##Z: If_21t<comparison>(instruction, dex_pc); break

    IF_XX(HEqual, EQ);
    IF_XX(HNotEqual, NE);
    IF_XX(HLessThan, LT);
    IF_XX(HLessThanOrEqual, LE);
    IF_XX(HGreaterThan, GT);
    IF_XX(HGreaterThanOrEqual, GE);

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      AppendInstruction(new (allocator_) HGoto(dex_pc));
      current_block_ = nullptr;
      break;
    }

    case Instruction::RETURN: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::RETURN_OBJECT: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::RETURN_WIDE: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          VLOG(compiler) << "Not compiled: Could not decode quickened instruction "
                         << instruction.Opcode();
          return false;
        }
        method_idx = LookupQuickenedInfo(quicken_index);
      } else {
        method_idx = instruction.VRegB_35c();
      }
      uint32_t args[5];
      uint32_t number_of_vreg_arguments = instruction.GetVarArgs(args);
      VarArgsInstructionOperands operands(args, number_of_vreg_arguments);
      if (!BuildInvoke(instruction, dex_pc, method_idx, operands)) {
        return false;
      }
      break;
    }

    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          VLOG(compiler) << "Not compiled: Could not decode quickened instruction "
                         << instruction.Opcode();
          return false;
        }
        method_idx = LookupQuickenedInfo(quicken_index);
      } else {
        method_idx = instruction.VRegB_3rc();
      }
      RangeInstructionOperands operands(instruction.VRegC(), instruction.VRegA_3rc());
      if (!BuildInvoke(instruction, dex_pc, method_idx, operands)) {
        return false;
      }
      break;
    }

    case Instruction::INVOKE_POLYMORPHIC: {
      uint16_t method_idx = instruction.VRegB_45cc();
      dex::ProtoIndex proto_idx(instruction.VRegH_45cc());
      uint32_t args[5];
      uint32_t number_of_vreg_arguments = instruction.GetVarArgs(args);
      VarArgsInstructionOperands operands(args, number_of_vreg_arguments);
      return BuildInvokePolymorphic(dex_pc, method_idx, proto_idx, operands);
    }

    case Instruction::INVOKE_POLYMORPHIC_RANGE: {
      uint16_t method_idx = instruction.VRegB_4rcc();
      dex::ProtoIndex proto_idx(instruction.VRegH_4rcc());
      RangeInstructionOperands operands(instruction.VRegC_4rcc(), instruction.VRegA_4rcc());
      return BuildInvokePolymorphic(dex_pc, method_idx, proto_idx, operands);
    }

    case Instruction::INVOKE_CUSTOM: {
      uint16_t call_site_idx = instruction.VRegB_35c();
      uint32_t args[5];
      uint32_t number_of_vreg_arguments = instruction.GetVarArgs(args);
      VarArgsInstructionOperands operands(args, number_of_vreg_arguments);
      return BuildInvokeCustom(dex_pc, call_site_idx, operands);
    }

    case Instruction::INVOKE_CUSTOM_RANGE: {
      uint16_t call_site_idx = instruction.VRegB_3rc();
      RangeInstructionOperands operands(instruction.VRegC_3rc(), instruction.VRegA_3rc());
      return BuildInvokeCustom(dex_pc, call_site_idx, operands);
    }

    case Instruction::NEG_INT: {
      Unop_12x<HNeg>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::NEG_LONG: {
      Unop_12x<HNeg>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::NEG_FLOAT: {
      Unop_12x<HNeg>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::NEG_DOUBLE: {
      Unop_12x<HNeg>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::NOT_INT: {
      Unop_12x<HNot>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::NOT_LONG: {
      Unop_12x<HNot>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::INT_TO_LONG: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::INT_TO_FLOAT: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::INT_TO_DOUBLE: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::LONG_TO_INT: {
      Conversion_12x(instruction, DataType::Type::kInt64, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::LONG_TO_FLOAT: {
      Conversion_12x(instruction, DataType::Type::kInt64, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::LONG_TO_DOUBLE: {
      Conversion_12x(instruction, DataType::Type::kInt64, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_INT: {
      Conversion_12x(instruction, DataType::Type::kFloat32, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_LONG: {
      Conversion_12x(instruction, DataType::Type::kFloat32, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_DOUBLE: {
      Conversion_12x(instruction, DataType::Type::kFloat32, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_INT: {
      Conversion_12x(instruction, DataType::Type::kFloat64, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_LONG: {
      Conversion_12x(instruction, DataType::Type::kFloat64, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_FLOAT: {
      Conversion_12x(instruction, DataType::Type::kFloat64, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::INT_TO_BYTE: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kInt8, dex_pc);
      break;
    }

    case Instruction::INT_TO_SHORT: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kInt16, dex_pc);
      break;
    }

    case Instruction::INT_TO_CHAR: {
      Conversion_12x(instruction, DataType::Type::kInt32, DataType::Type::kUint16, dex_pc);
      break;
    }

    case Instruction::ADD_INT: {
      Binop_23x<HAdd>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::ADD_LONG: {
      Binop_23x<HAdd>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::ADD_DOUBLE: {
      Binop_23x<HAdd>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::ADD_FLOAT: {
      Binop_23x<HAdd>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::SUB_INT: {
      Binop_23x<HSub>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SUB_LONG: {
      Binop_23x<HSub>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::SUB_FLOAT: {
      Binop_23x<HSub>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::SUB_DOUBLE: {
      Binop_23x<HSub>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::ADD_INT_2ADDR: {
      Binop_12x<HAdd>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::MUL_INT: {
      Binop_23x<HMul>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::MUL_LONG: {
      Binop_23x<HMul>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::MUL_FLOAT: {
      Binop_23x<HMul>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::MUL_DOUBLE: {
      Binop_23x<HMul>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::DIV_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt32, false, true);
      break;
    }

    case Instruction::DIV_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt64, false, true);
      break;
    }

    case Instruction::DIV_FLOAT: {
      Binop_23x<HDiv>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::DIV_DOUBLE: {
      Binop_23x<HDiv>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::REM_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt32, false, false);
      break;
    }

    case Instruction::REM_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt64, false, false);
      break;
    }

    case Instruction::REM_FLOAT: {
      Binop_23x<HRem>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::REM_DOUBLE: {
      Binop_23x<HRem>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::AND_INT: {
      Binop_23x<HAnd>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::AND_LONG: {
      Binop_23x<HAnd>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::SHL_INT: {
      Binop_23x_shift<HShl>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SHL_LONG: {
      Binop_23x_shift<HShl>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::SHR_INT: {
      Binop_23x_shift<HShr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SHR_LONG: {
      Binop_23x_shift<HShr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::USHR_INT: {
      Binop_23x_shift<HUShr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::USHR_LONG: {
      Binop_23x_shift<HUShr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::OR_INT: {
      Binop_23x<HOr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::OR_LONG: {
      Binop_23x<HOr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::XOR_INT: {
      Binop_23x<HXor>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::XOR_LONG: {
      Binop_23x<HXor>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::ADD_LONG_2ADDR: {
      Binop_12x<HAdd>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::ADD_DOUBLE_2ADDR: {
      Binop_12x<HAdd>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::ADD_FLOAT_2ADDR: {
      Binop_12x<HAdd>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::SUB_INT_2ADDR: {
      Binop_12x<HSub>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SUB_LONG_2ADDR: {
      Binop_12x<HSub>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::SUB_FLOAT_2ADDR: {
      Binop_12x<HSub>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::SUB_DOUBLE_2ADDR: {
      Binop_12x<HSub>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::MUL_INT_2ADDR: {
      Binop_12x<HMul>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::MUL_LONG_2ADDR: {
      Binop_12x<HMul>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::MUL_FLOAT_2ADDR: {
      Binop_12x<HMul>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::MUL_DOUBLE_2ADDR: {
      Binop_12x<HMul>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::DIV_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, DataType::Type::kInt32, false, true);
      break;
    }

    case Instruction::DIV_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, DataType::Type::kInt64, false, true);
      break;
    }

    case Instruction::REM_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, DataType::Type::kInt32, false, false);
      break;
    }

    case Instruction::REM_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, DataType::Type::kInt64, false, false);
      break;
    }

    case Instruction::REM_FLOAT_2ADDR: {
      Binop_12x<HRem>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::REM_DOUBLE_2ADDR: {
      Binop_12x<HRem>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::SHL_INT_2ADDR: {
      Binop_12x_shift<HShl>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SHL_LONG_2ADDR: {
      Binop_12x_shift<HShl>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::SHR_INT_2ADDR: {
      Binop_12x_shift<HShr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::SHR_LONG_2ADDR: {
      Binop_12x_shift<HShr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::USHR_INT_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::USHR_LONG_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::DIV_FLOAT_2ADDR: {
      Binop_12x<HDiv>(instruction, DataType::Type::kFloat32, dex_pc);
      break;
    }

    case Instruction::DIV_DOUBLE_2ADDR: {
      Binop_12x<HDiv>(instruction, DataType::Type::kFloat64, dex_pc);
      break;
    }

    case Instruction::AND_INT_2ADDR: {
      Binop_12x<HAnd>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::AND_LONG_2ADDR: {
      Binop_12x<HAnd>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::OR_INT_2ADDR: {
      Binop_12x<HOr>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::OR_LONG_2ADDR: {
      Binop_12x<HOr>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::XOR_INT_2ADDR: {
      Binop_12x<HXor>(instruction, DataType::Type::kInt32, dex_pc);
      break;
    }

    case Instruction::XOR_LONG_2ADDR: {
      Binop_12x<HXor>(instruction, DataType::Type::kInt64, dex_pc);
      break;
    }

    case Instruction::ADD_INT_LIT16: {
      Binop_22s<HAdd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::AND_INT_LIT16: {
      Binop_22s<HAnd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::OR_INT_LIT16: {
      Binop_22s<HOr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::XOR_INT_LIT16: {
      Binop_22s<HXor>(instruction, false, dex_pc);
      break;
    }

    case Instruction::RSUB_INT: {
      Binop_22s<HSub>(instruction, true, dex_pc);
      break;
    }

    case Instruction::MUL_INT_LIT16: {
      Binop_22s<HMul>(instruction, false, dex_pc);
      break;
    }

    case Instruction::ADD_INT_LIT8: {
      Binop_22b<HAdd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::AND_INT_LIT8: {
      Binop_22b<HAnd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::OR_INT_LIT8: {
      Binop_22b<HOr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::XOR_INT_LIT8: {
      Binop_22b<HXor>(instruction, false, dex_pc);
      break;
    }

    case Instruction::RSUB_INT_LIT8: {
      Binop_22b<HSub>(instruction, true, dex_pc);
      break;
    }

    case Instruction::MUL_INT_LIT8: {
      Binop_22b<HMul>(instruction, false, dex_pc);
      break;
    }

    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt32, true, true);
      break;
    }

    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, DataType::Type::kInt32, true, false);
      break;
    }

    case Instruction::SHL_INT_LIT8: {
      Binop_22b<HShl>(instruction, false, dex_pc);
      break;
    }

    case Instruction::SHR_INT_LIT8: {
      Binop_22b<HShr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::USHR_INT_LIT8: {
      Binop_22b<HUShr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::NEW_INSTANCE: {
      HNewInstance* new_instance =
          BuildNewInstance(dex::TypeIndex(instruction.VRegB_21c()), dex_pc);
      DCHECK(new_instance != nullptr);

      UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
      BuildConstructorFenceForAllocation(new_instance);
      break;
    }

    case Instruction::NEW_ARRAY: {
      dex::TypeIndex type_index(instruction.VRegC_22c());
      HInstruction* length = LoadLocal(instruction.VRegB_22c(), DataType::Type::kInt32);
      HNewArray* new_array = BuildNewArray(dex_pc, type_index, length);

      UpdateLocal(instruction.VRegA_22c(), current_block_->GetLastInstruction());
      BuildConstructorFenceForAllocation(new_array);
      break;
    }

    case Instruction::FILLED_NEW_ARRAY: {
      dex::TypeIndex type_index(instruction.VRegB_35c());
      uint32_t args[5];
      uint32_t number_of_vreg_arguments = instruction.GetVarArgs(args);
      VarArgsInstructionOperands operands(args, number_of_vreg_arguments);
      HNewArray* new_array = BuildFilledNewArray(dex_pc, type_index, operands);
      BuildConstructorFenceForAllocation(new_array);
      break;
    }

    case Instruction::FILLED_NEW_ARRAY_RANGE: {
      dex::TypeIndex type_index(instruction.VRegB_3rc());
      RangeInstructionOperands operands(instruction.VRegC_3rc(), instruction.VRegA_3rc());
      HNewArray* new_array = BuildFilledNewArray(dex_pc, type_index, operands);
      BuildConstructorFenceForAllocation(new_array);
      break;
    }

    case Instruction::FILL_ARRAY_DATA: {
      BuildFillArrayData(instruction, dex_pc);
      break;
    }

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT: {
      DCHECK(latest_result_ != nullptr);
      UpdateLocal(instruction.VRegA(), latest_result_);
      latest_result_ = nullptr;
      break;
    }

    case Instruction::CMP_LONG: {
      Binop_23x_cmp(instruction, DataType::Type::kInt64, ComparisonBias::kNoBias, dex_pc);
      break;
    }

    case Instruction::CMPG_FLOAT: {
      Binop_23x_cmp(instruction, DataType::Type::kFloat32, ComparisonBias::kGtBias, dex_pc);
      break;
    }

    case Instruction::CMPG_DOUBLE: {
      Binop_23x_cmp(instruction, DataType::Type::kFloat64, ComparisonBias::kGtBias, dex_pc);
      break;
    }

    case Instruction::CMPL_FLOAT: {
      Binop_23x_cmp(instruction, DataType::Type::kFloat32, ComparisonBias::kLtBias, dex_pc);
      break;
    }

    case Instruction::CMPL_DOUBLE: {
      Binop_23x_cmp(instruction, DataType::Type::kFloat64, ComparisonBias::kLtBias, dex_pc);
      break;
    }

    case Instruction::NOP:
      break;

    case Instruction::IGET:
    case Instruction::IGET_QUICK:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_WIDE_QUICK:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_OBJECT_QUICK:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BOOLEAN_QUICK:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_BYTE_QUICK:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_CHAR_QUICK:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, /* is_put= */ false, quicken_index)) {
        return false;
      }
      break;
    }

    case Instruction::IPUT:
    case Instruction::IPUT_QUICK:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_WIDE_QUICK:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_BYTE_QUICK:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_CHAR_QUICK:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, /* is_put= */ true, quicken_index)) {
        return false;
      }
      break;
    }

    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT: {
      BuildStaticFieldAccess(instruction, dex_pc, /* is_put= */ false);
      break;
    }

    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
      BuildStaticFieldAccess(instruction, dex_pc, /* is_put= */ true);
      break;
    }

#define ARRAY_XX(kind, anticipated_type)                                          \
    case Instruction::AGET##kind: {                                               \
      BuildArrayAccess(instruction, dex_pc, false, anticipated_type);         \
      break;                                                                      \
    }                                                                             \
    case Instruction::APUT##kind: {                                               \
      BuildArrayAccess(instruction, dex_pc, true, anticipated_type);          \
      break;                                                                      \
    }

    ARRAY_XX(, DataType::Type::kInt32);
    ARRAY_XX(_WIDE, DataType::Type::kInt64);
    ARRAY_XX(_OBJECT, DataType::Type::kReference);
    ARRAY_XX(_BOOLEAN, DataType::Type::kBool);
    ARRAY_XX(_BYTE, DataType::Type::kInt8);
    ARRAY_XX(_CHAR, DataType::Type::kUint16);
    ARRAY_XX(_SHORT, DataType::Type::kInt16);

    case Instruction::ARRAY_LENGTH: {
      HInstruction* object = LoadNullCheckedLocal(instruction.VRegB_12x(), dex_pc);
      AppendInstruction(new (allocator_) HArrayLength(object, dex_pc));
      UpdateLocal(instruction.VRegA_12x(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::CONST_STRING: {
      dex::StringIndex string_index(instruction.VRegB_21c());
      BuildLoadString(string_index, dex_pc);
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::CONST_STRING_JUMBO: {
      dex::StringIndex string_index(instruction.VRegB_31c());
      BuildLoadString(string_index, dex_pc);
      UpdateLocal(instruction.VRegA_31c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::CONST_CLASS: {
      dex::TypeIndex type_index(instruction.VRegB_21c());
      BuildLoadClass(type_index, dex_pc);
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::CONST_METHOD_HANDLE: {
      uint16_t method_handle_idx = instruction.VRegB_21c();
      BuildLoadMethodHandle(method_handle_idx, dex_pc);
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::CONST_METHOD_TYPE: {
      dex::ProtoIndex proto_idx(instruction.VRegB_21c());
      BuildLoadMethodType(proto_idx, dex_pc);
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::MOVE_EXCEPTION: {
      AppendInstruction(new (allocator_) HLoadException(dex_pc));
      UpdateLocal(instruction.VRegA_11x(), current_block_->GetLastInstruction());
      AppendInstruction(new (allocator_) HClearException(dex_pc));
      break;
    }

    case Instruction::THROW: {
      HInstruction* exception = LoadLocal(instruction.VRegA_11x(), DataType::Type::kReference);
      AppendInstruction(new (allocator_) HThrow(exception, dex_pc));
      // We finished building this block. Set the current block to null to avoid
      // adding dead instructions to it.
      current_block_ = nullptr;
      break;
    }

    case Instruction::INSTANCE_OF: {
      uint8_t destination = instruction.VRegA_22c();
      uint8_t reference = instruction.VRegB_22c();
      dex::TypeIndex type_index(instruction.VRegC_22c());
      BuildTypeCheck(instruction, destination, reference, type_index, dex_pc);
      break;
    }

    case Instruction::CHECK_CAST: {
      uint8_t reference = instruction.VRegA_21c();
      dex::TypeIndex type_index(instruction.VRegB_21c());
      BuildTypeCheck(instruction, -1, reference, type_index, dex_pc);
      break;
    }

    case Instruction::MONITOR_ENTER: {
      AppendInstruction(new (allocator_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), DataType::Type::kReference),
          HMonitorOperation::OperationKind::kEnter,
          dex_pc));
      graph_->SetHasMonitorOperations(true);
      break;
    }

    case Instruction::MONITOR_EXIT: {
      AppendInstruction(new (allocator_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), DataType::Type::kReference),
          HMonitorOperation::OperationKind::kExit,
          dex_pc));
      graph_->SetHasMonitorOperations(true);
      break;
    }

    case Instruction::SPARSE_SWITCH:
    case Instruction::PACKED_SWITCH: {
      BuildSwitch(instruction, dex_pc);
      break;
    }

    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
    case Instruction::UNUSED_F3:
    case Instruction::UNUSED_F4:
    case Instruction::UNUSED_F5:
    case Instruction::UNUSED_F6:
    case Instruction::UNUSED_F7:
    case Instruction::UNUSED_F8:
    case Instruction::UNUSED_F9: {
      VLOG(compiler) << "Did not compile "
                     << dex_file_->PrettyMethod(dex_compilation_unit_->GetDexMethodIndex())
                     << " because of unhandled instruction "
                     << instruction.Name();
      MaybeRecordStat(compilation_stats_,
                      MethodCompilationStat::kNotCompiledUnhandledInstruction);
      return false;
    }
  }
  return true;
}  // NOLINT(readability/fn_size)

ObjPtr<mirror::Class> HInstructionBuilder::LookupResolvedType(
    dex::TypeIndex type_index,
    const DexCompilationUnit& compilation_unit) const {
  return compilation_unit.GetClassLinker()->LookupResolvedType(
        type_index, compilation_unit.GetDexCache().Get(), compilation_unit.GetClassLoader().Get());
}

ObjPtr<mirror::Class> HInstructionBuilder::LookupReferrerClass() const {
  // TODO: Cache the result in a Handle<mirror::Class>.
  const dex::MethodId& method_id =
      dex_compilation_unit_->GetDexFile()->GetMethodId(dex_compilation_unit_->GetDexMethodIndex());
  return LookupResolvedType(method_id.class_idx_, *dex_compilation_unit_);
}

}  // namespace art
