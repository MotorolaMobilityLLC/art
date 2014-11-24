/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "base/bit_vector-inl.h"
#include "compiler_internals.h"
#include "dataflow_iterator-inl.h"
#include "global_value_numbering.h"
#include "local_value_numbering.h"
#include "mir_field_info.h"
#include "quick/dex_file_method_inliner.h"
#include "quick/dex_file_to_method_inliner_map.h"
#include "stack.h"
#include "utils/scoped_arena_containers.h"

namespace art {

static unsigned int Predecessors(BasicBlock* bb) {
  return bb->predecessors.size();
}

/* Setup a constant value for opcodes thare have the DF_SETS_CONST attribute */
void MIRGraph::SetConstant(int32_t ssa_reg, int32_t value) {
  is_constant_v_->SetBit(ssa_reg);
  constant_values_[ssa_reg] = value;
}

void MIRGraph::SetConstantWide(int32_t ssa_reg, int64_t value) {
  is_constant_v_->SetBit(ssa_reg);
  is_constant_v_->SetBit(ssa_reg + 1);
  constant_values_[ssa_reg] = Low32Bits(value);
  constant_values_[ssa_reg + 1] = High32Bits(value);
}

void MIRGraph::DoConstantPropagation(BasicBlock* bb) {
  MIR* mir;

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    // Skip pass if BB has MIR without SSA representation.
    if (mir->ssa_rep == nullptr) {
       return;
    }

    uint64_t df_attributes = GetDataFlowAttributes(mir);

    MIR::DecodedInstruction* d_insn = &mir->dalvikInsn;

    if (!(df_attributes & DF_HAS_DEFS)) continue;

    /* Handle instructions that set up constants directly */
    if (df_attributes & DF_SETS_CONST) {
      if (df_attributes & DF_DA) {
        int32_t vB = static_cast<int32_t>(d_insn->vB);
        switch (d_insn->opcode) {
          case Instruction::CONST_4:
          case Instruction::CONST_16:
          case Instruction::CONST:
            SetConstant(mir->ssa_rep->defs[0], vB);
            break;
          case Instruction::CONST_HIGH16:
            SetConstant(mir->ssa_rep->defs[0], vB << 16);
            break;
          case Instruction::CONST_WIDE_16:
          case Instruction::CONST_WIDE_32:
            SetConstantWide(mir->ssa_rep->defs[0], static_cast<int64_t>(vB));
            break;
          case Instruction::CONST_WIDE:
            SetConstantWide(mir->ssa_rep->defs[0], d_insn->vB_wide);
            break;
          case Instruction::CONST_WIDE_HIGH16:
            SetConstantWide(mir->ssa_rep->defs[0], static_cast<int64_t>(vB) << 48);
            break;
          default:
            break;
        }
      }
      /* Handle instructions that set up constants directly */
    } else if (df_attributes & DF_IS_MOVE) {
      int i;

      for (i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (!is_constant_v_->IsBitSet(mir->ssa_rep->uses[i])) break;
      }
      /* Move a register holding a constant to another register */
      if (i == mir->ssa_rep->num_uses) {
        SetConstant(mir->ssa_rep->defs[0], constant_values_[mir->ssa_rep->uses[0]]);
        if (df_attributes & DF_A_WIDE) {
          SetConstant(mir->ssa_rep->defs[1], constant_values_[mir->ssa_rep->uses[1]]);
        }
      }
    }
  }
  /* TODO: implement code to handle arithmetic operations */
}

/* Advance to next strictly dominated MIR node in an extended basic block */
MIR* MIRGraph::AdvanceMIR(BasicBlock** p_bb, MIR* mir) {
  BasicBlock* bb = *p_bb;
  if (mir != NULL) {
    mir = mir->next;
    if (mir == NULL) {
      bb = GetBasicBlock(bb->fall_through);
      if ((bb == NULL) || Predecessors(bb) != 1) {
        mir = NULL;
      } else {
      *p_bb = bb;
      mir = bb->first_mir_insn;
      }
    }
  }
  return mir;
}

/*
 * To be used at an invoke mir.  If the logically next mir node represents
 * a move-result, return it.  Else, return NULL.  If a move-result exists,
 * it is required to immediately follow the invoke with no intervening
 * opcodes or incoming arcs.  However, if the result of the invoke is not
 * used, a move-result may not be present.
 */
MIR* MIRGraph::FindMoveResult(BasicBlock* bb, MIR* mir) {
  BasicBlock* tbb = bb;
  mir = AdvanceMIR(&tbb, mir);
  while (mir != NULL) {
    if ((mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE)) {
      break;
    }
    // Keep going if pseudo op, otherwise terminate
    if (MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode)) {
      mir = AdvanceMIR(&tbb, mir);
    } else {
      mir = NULL;
    }
  }
  return mir;
}

BasicBlock* MIRGraph::NextDominatedBlock(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return NULL;
  }
  DCHECK((bb->block_type == kEntryBlock) || (bb->block_type == kDalvikByteCode)
      || (bb->block_type == kExitBlock));
  BasicBlock* bb_taken = GetBasicBlock(bb->taken);
  BasicBlock* bb_fall_through = GetBasicBlock(bb->fall_through);
  if (((bb_fall_through == NULL) && (bb_taken != NULL)) &&
      ((bb_taken->block_type == kDalvikByteCode) || (bb_taken->block_type == kExitBlock))) {
    // Follow simple unconditional branches.
    bb = bb_taken;
  } else {
    // Follow simple fallthrough
    bb = (bb_taken != NULL) ? NULL : bb_fall_through;
  }
  if (bb == NULL || (Predecessors(bb) != 1)) {
    return NULL;
  }
  DCHECK((bb->block_type == kDalvikByteCode) || (bb->block_type == kExitBlock));
  return bb;
}

static MIR* FindPhi(BasicBlock* bb, int ssa_name) {
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi) {
      for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (mir->ssa_rep->uses[i] == ssa_name) {
          return mir;
        }
      }
    }
  }
  return NULL;
}

static SelectInstructionKind SelectKind(MIR* mir) {
  // Work with the case when mir is nullptr.
  if (mir == nullptr) {
    return kSelectNone;
  }
  switch (mir->dalvikInsn.opcode) {
    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      return kSelectMove;
    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      return kSelectConst;
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      return kSelectGoto;
    default:
      return kSelectNone;
  }
}

static constexpr ConditionCode kIfCcZConditionCodes[] = {
    kCondEq, kCondNe, kCondLt, kCondGe, kCondGt, kCondLe
};

static_assert(arraysize(kIfCcZConditionCodes) == Instruction::IF_LEZ - Instruction::IF_EQZ + 1,
              "if_ccz_ccodes_size1");

static constexpr ConditionCode ConditionCodeForIfCcZ(Instruction::Code opcode) {
  return kIfCcZConditionCodes[opcode - Instruction::IF_EQZ];
}

static_assert(ConditionCodeForIfCcZ(Instruction::IF_EQZ) == kCondEq, "if_eqz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_NEZ) == kCondNe, "if_nez ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_LTZ) == kCondLt, "if_ltz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_GEZ) == kCondGe, "if_gez ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_GTZ) == kCondGt, "if_gtz ccode");
static_assert(ConditionCodeForIfCcZ(Instruction::IF_LEZ) == kCondLe, "if_lez ccode");

int MIRGraph::GetSSAUseCount(int s_reg) {
  DCHECK_LT(static_cast<size_t>(s_reg), ssa_subscripts_.size());
  return raw_use_counts_[s_reg];
}

size_t MIRGraph::GetNumBytesForSpecialTemps() const {
  // This logic is written with assumption that Method* is only special temp.
  DCHECK_EQ(max_available_special_compiler_temps_, 1u);
  return sizeof(StackReference<mirror::ArtMethod>);
}

size_t MIRGraph::GetNumAvailableVRTemps() {
  // First take into account all temps reserved for backend.
  if (max_available_non_special_compiler_temps_ < reserved_temps_for_backend_) {
    return 0;
  }

  // Calculate remaining ME temps available.
  size_t remaining_me_temps = max_available_non_special_compiler_temps_ - reserved_temps_for_backend_;

  if (num_non_special_compiler_temps_ >= remaining_me_temps) {
    return 0;
  } else {
    return remaining_me_temps - num_non_special_compiler_temps_;
  }
}

// FIXME - will probably need to revisit all uses of this, as type not defined.
static const RegLocation temp_loc = {kLocCompilerTemp,
                                     0, 1 /*defined*/, 0, 0, 0, 0, 0, 1 /*home*/,
                                     RegStorage(), INVALID_SREG, INVALID_SREG};

CompilerTemp* MIRGraph::GetNewCompilerTemp(CompilerTempType ct_type, bool wide) {
  // Once the compiler temps have been committed, new ones cannot be requested anymore.
  DCHECK_EQ(compiler_temps_committed_, false);
  // Make sure that reserved for BE set is sane.
  DCHECK_LE(reserved_temps_for_backend_, max_available_non_special_compiler_temps_);

  bool verbose = cu_->verbose;
  const char* ct_type_str = nullptr;

  if (verbose) {
    switch (ct_type) {
      case kCompilerTempBackend:
        ct_type_str = "backend";
        break;
      case kCompilerTempSpecialMethodPtr:
        ct_type_str = "method*";
        break;
      case kCompilerTempVR:
        ct_type_str = "VR";
        break;
      default:
        ct_type_str = "unknown";
        break;
    }
    LOG(INFO) << "CompilerTemps: A compiler temp of type " << ct_type_str << " that is "
        << (wide ? "wide is being requested." : "not wide is being requested.");
  }

  CompilerTemp *compiler_temp = static_cast<CompilerTemp *>(arena_->Alloc(sizeof(CompilerTemp),
                                                            kArenaAllocRegAlloc));

  // Create the type of temp requested. Special temps need special handling because
  // they have a specific virtual register assignment.
  if (ct_type == kCompilerTempSpecialMethodPtr) {
    // This has a special location on stack which is 32-bit or 64-bit depending
    // on mode. However, we don't want to overlap with non-special section
    // and thus even for 64-bit, we allow only a non-wide temp to be requested.
    DCHECK_EQ(wide, false);

    // The vreg is always the first special temp for method ptr.
    compiler_temp->v_reg = GetFirstSpecialTempVR();

  } else if (ct_type == kCompilerTempBackend) {
    requested_backend_temp_ = true;

    // Make sure that we are not exceeding temps reserved for BE.
    // Since VR temps cannot be requested once the BE temps are requested, we
    // allow reservation of VR temps as well for BE. We
    size_t available_temps = reserved_temps_for_backend_ + GetNumAvailableVRTemps();
    if (available_temps <= 0 || (available_temps <= 1 && wide)) {
      if (verbose) {
        LOG(INFO) << "CompilerTemps: Not enough temp(s) of type " << ct_type_str << " are available.";
      }
      return nullptr;
    }

    // Update the remaining reserved temps since we have now used them.
    // Note that the code below is actually subtracting to remove them from reserve
    // once they have been claimed. It is careful to not go below zero.
    if (reserved_temps_for_backend_ >= 1) {
      reserved_temps_for_backend_--;
    }
    if (wide && reserved_temps_for_backend_ >= 1) {
      reserved_temps_for_backend_--;
    }

    // The new non-special compiler temp must receive a unique v_reg.
    compiler_temp->v_reg = GetFirstNonSpecialTempVR() + num_non_special_compiler_temps_;
    num_non_special_compiler_temps_++;
  } else if (ct_type == kCompilerTempVR) {
    // Once we start giving out BE temps, we don't allow anymore ME temps to be requested.
    // This is done in order to prevent problems with ssa since these structures are allocated
    // and managed by the ME.
    DCHECK_EQ(requested_backend_temp_, false);

    // There is a limit to the number of non-special temps so check to make sure it wasn't exceeded.
    size_t available_temps = GetNumAvailableVRTemps();
    if (available_temps <= 0 || (available_temps <= 1 && wide)) {
      if (verbose) {
        LOG(INFO) << "CompilerTemps: Not enough temp(s) of type " << ct_type_str << " are available.";
      }
      return nullptr;
    }

    // The new non-special compiler temp must receive a unique v_reg.
    compiler_temp->v_reg = GetFirstNonSpecialTempVR() + num_non_special_compiler_temps_;
    num_non_special_compiler_temps_++;
  } else {
    UNIMPLEMENTED(FATAL) << "No handling for compiler temp type " << ct_type_str << ".";
  }

  // We allocate an sreg as well to make developer life easier.
  // However, if this is requested from an ME pass that will recalculate ssa afterwards,
  // this sreg is no longer valid. The caller should be aware of this.
  compiler_temp->s_reg_low = AddNewSReg(compiler_temp->v_reg);

  if (verbose) {
    LOG(INFO) << "CompilerTemps: New temp of type " << ct_type_str << " with v" << compiler_temp->v_reg
        << " and s" << compiler_temp->s_reg_low << " has been created.";
  }

  if (wide) {
    // Only non-special temps are handled as wide for now.
    // Note that the number of non special temps is incremented below.
    DCHECK(ct_type == kCompilerTempBackend || ct_type == kCompilerTempVR);

    // Ensure that the two registers are consecutive.
    int ssa_reg_low = compiler_temp->s_reg_low;
    int ssa_reg_high = AddNewSReg(compiler_temp->v_reg + 1);
    num_non_special_compiler_temps_++;

    if (verbose) {
      LOG(INFO) << "CompilerTemps: The wide part of temp of type " << ct_type_str << " is v"
          << compiler_temp->v_reg + 1 << " and s" << ssa_reg_high << ".";
    }

    if (reg_location_ != nullptr) {
      reg_location_[ssa_reg_high] = temp_loc;
      reg_location_[ssa_reg_high].high_word = true;
      reg_location_[ssa_reg_high].s_reg_low = ssa_reg_low;
      reg_location_[ssa_reg_high].wide = true;
    }
  }

  // If the register locations have already been allocated, add the information
  // about the temp. We will not overflow because they have been initialized
  // to support the maximum number of temps. For ME temps that have multiple
  // ssa versions, the structures below will be expanded on the post pass cleanup.
  if (reg_location_ != nullptr) {
    int ssa_reg_low = compiler_temp->s_reg_low;
    reg_location_[ssa_reg_low] = temp_loc;
    reg_location_[ssa_reg_low].s_reg_low = ssa_reg_low;
    reg_location_[ssa_reg_low].wide = wide;
  }

  return compiler_temp;
}

/* Do some MIR-level extended basic block optimizations */
bool MIRGraph::BasicBlockOpt(BasicBlock* bb) {
  if (bb->block_type == kDead) {
    return true;
  }
  bool use_lvn = bb->use_lvn && (cu_->disable_opt & (1u << kLocalValueNumbering)) == 0u;
  std::unique_ptr<ScopedArenaAllocator> allocator;
  std::unique_ptr<GlobalValueNumbering> global_valnum;
  std::unique_ptr<LocalValueNumbering> local_valnum;
  if (use_lvn) {
    allocator.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
    global_valnum.reset(new (allocator.get()) GlobalValueNumbering(cu_, allocator.get(),
                                                                   GlobalValueNumbering::kModeLvn));
    local_valnum.reset(new (allocator.get()) LocalValueNumbering(global_valnum.get(), bb->id,
                                                                 allocator.get()));
  }
  while (bb != NULL) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      // TUNING: use the returned value number for CSE.
      if (use_lvn) {
        local_valnum->GetValueNumber(mir);
      }
      // Look for interesting opcodes, skip otherwise
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::CMPL_FLOAT:
        case Instruction::CMPL_DOUBLE:
        case Instruction::CMPG_FLOAT:
        case Instruction::CMPG_DOUBLE:
        case Instruction::CMP_LONG:
          if ((cu_->disable_opt & (1 << kBranchFusing)) != 0) {
            // Bitcode doesn't allow this optimization.
            break;
          }
          if (mir->next != NULL) {
            MIR* mir_next = mir->next;
            // Make sure result of cmp is used by next insn and nowhere else
            if (IsInstructionIfCcZ(mir_next->dalvikInsn.opcode) &&
                (mir->ssa_rep->defs[0] == mir_next->ssa_rep->uses[0]) &&
                (GetSSAUseCount(mir->ssa_rep->defs[0]) == 1)) {
              mir_next->meta.ccode = ConditionCodeForIfCcZ(mir_next->dalvikInsn.opcode);
              switch (opcode) {
                case Instruction::CMPL_FLOAT:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmplFloat);
                  break;
                case Instruction::CMPL_DOUBLE:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmplDouble);
                  break;
                case Instruction::CMPG_FLOAT:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpgFloat);
                  break;
                case Instruction::CMPG_DOUBLE:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpgDouble);
                  break;
                case Instruction::CMP_LONG:
                  mir_next->dalvikInsn.opcode =
                      static_cast<Instruction::Code>(kMirOpFusedCmpLong);
                  break;
                default: LOG(ERROR) << "Unexpected opcode: " << opcode;
              }
              mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              // Copy the SSA information that is relevant.
              mir_next->ssa_rep->num_uses = mir->ssa_rep->num_uses;
              mir_next->ssa_rep->uses = mir->ssa_rep->uses;
              mir_next->ssa_rep->fp_use = mir->ssa_rep->fp_use;
              mir_next->ssa_rep->num_defs = 0;
              mir->ssa_rep->num_uses = 0;
              mir->ssa_rep->num_defs = 0;
              // Copy in the decoded instruction information for potential SSA re-creation.
              mir_next->dalvikInsn.vA = mir->dalvikInsn.vB;
              mir_next->dalvikInsn.vB = mir->dalvikInsn.vC;
            }
          }
          break;
        case Instruction::RETURN_VOID:
        case Instruction::RETURN:
        case Instruction::RETURN_WIDE:
        case Instruction::RETURN_OBJECT:
          if (bb->GetFirstNonPhiInsn() == mir) {
            // This is a simple return BB. Eliminate suspend checks on predecessor back-edges.
            for (BasicBlockId pred_id : bb->predecessors) {
              BasicBlock* pred_bb = GetBasicBlock(pred_id);
              DCHECK(pred_bb != nullptr);
              if (IsBackedge(pred_bb, bb->id) && pred_bb->last_mir_insn != nullptr &&
                  (IsInstructionIfCc(pred_bb->last_mir_insn->dalvikInsn.opcode) ||
                   IsInstructionIfCcZ(pred_bb->last_mir_insn->dalvikInsn.opcode) ||
                   IsInstructionGoto(pred_bb->last_mir_insn->dalvikInsn.opcode))) {
                pred_bb->last_mir_insn->optimization_flags |= MIR_IGNORE_SUSPEND_CHECK;
                if (cu_->verbose) {
                  LOG(INFO) << "Suppressed suspend check on branch to return at 0x" << std::hex
                            << pred_bb->last_mir_insn->offset;
                }
              }
            }
          }
          break;
        default:
          break;
      }
      // Is this the select pattern?
      // TODO: flesh out support for Mips.  NOTE: llvm's select op doesn't quite work here.
      // TUNING: expand to support IF_xx compare & branches
      if (!cu_->compiler->IsPortable() &&
          (cu_->instruction_set == kArm64 || cu_->instruction_set == kThumb2 ||
           cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) &&
          IsInstructionIfCcZ(mir->dalvikInsn.opcode)) {
        BasicBlock* ft = GetBasicBlock(bb->fall_through);
        DCHECK(ft != NULL);
        BasicBlock* ft_ft = GetBasicBlock(ft->fall_through);
        BasicBlock* ft_tk = GetBasicBlock(ft->taken);

        BasicBlock* tk = GetBasicBlock(bb->taken);
        DCHECK(tk != NULL);
        BasicBlock* tk_ft = GetBasicBlock(tk->fall_through);
        BasicBlock* tk_tk = GetBasicBlock(tk->taken);

        /*
         * In the select pattern, the taken edge goes to a block that unconditionally
         * transfers to the rejoin block and the fall_though edge goes to a block that
         * unconditionally falls through to the rejoin block.
         */
        if ((tk_ft == NULL) && (ft_tk == NULL) && (tk_tk == ft_ft) &&
            (Predecessors(tk) == 1) && (Predecessors(ft) == 1)) {
          /*
           * Okay - we have the basic diamond shape.  At the very least, we can eliminate the
           * suspend check on the taken-taken branch back to the join point.
           */
          if (SelectKind(tk->last_mir_insn) == kSelectGoto) {
              tk->last_mir_insn->optimization_flags |= (MIR_IGNORE_SUSPEND_CHECK);
          }

          // TODO: Add logic for LONG.
          // Are the block bodies something we can handle?
          if ((ft->first_mir_insn == ft->last_mir_insn) &&
              (tk->first_mir_insn != tk->last_mir_insn) &&
              (tk->first_mir_insn->next == tk->last_mir_insn) &&
              ((SelectKind(ft->first_mir_insn) == kSelectMove) ||
              (SelectKind(ft->first_mir_insn) == kSelectConst)) &&
              (SelectKind(ft->first_mir_insn) == SelectKind(tk->first_mir_insn)) &&
              (SelectKind(tk->last_mir_insn) == kSelectGoto)) {
            // Almost there.  Are the instructions targeting the same vreg?
            MIR* if_true = tk->first_mir_insn;
            MIR* if_false = ft->first_mir_insn;
            // It's possible that the target of the select isn't used - skip those (rare) cases.
            MIR* phi = FindPhi(tk_tk, if_true->ssa_rep->defs[0]);
            if ((phi != NULL) && (if_true->dalvikInsn.vA == if_false->dalvikInsn.vA)) {
              /*
               * We'll convert the IF_EQZ/IF_NEZ to a SELECT.  We need to find the
               * Phi node in the merge block and delete it (while using the SSA name
               * of the merge as the target of the SELECT.  Delete both taken and
               * fallthrough blocks, and set fallthrough to merge block.
               * NOTE: not updating other dataflow info (no longer used at this point).
               * If this changes, need to update i_dom, etc. here (and in CombineBlocks).
               */
              mir->meta.ccode = ConditionCodeForIfCcZ(mir->dalvikInsn.opcode);
              mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpSelect);
              bool const_form = (SelectKind(if_true) == kSelectConst);
              if ((SelectKind(if_true) == kSelectMove)) {
                if (IsConst(if_true->ssa_rep->uses[0]) &&
                    IsConst(if_false->ssa_rep->uses[0])) {
                    const_form = true;
                    if_true->dalvikInsn.vB = ConstantValue(if_true->ssa_rep->uses[0]);
                    if_false->dalvikInsn.vB = ConstantValue(if_false->ssa_rep->uses[0]);
                }
              }
              if (const_form) {
                /*
                 * TODO: If both constants are the same value, then instead of generating
                 * a select, we should simply generate a const bytecode. This should be
                 * considered after inlining which can lead to CFG of this form.
                 */
                // "true" set val in vB
                mir->dalvikInsn.vB = if_true->dalvikInsn.vB;
                // "false" set val in vC
                mir->dalvikInsn.vC = if_false->dalvikInsn.vB;
              } else {
                DCHECK_EQ(SelectKind(if_true), kSelectMove);
                DCHECK_EQ(SelectKind(if_false), kSelectMove);
                int* src_ssa =
                    static_cast<int*>(arena_->Alloc(sizeof(int) * 3, kArenaAllocDFInfo));
                src_ssa[0] = mir->ssa_rep->uses[0];
                src_ssa[1] = if_true->ssa_rep->uses[0];
                src_ssa[2] = if_false->ssa_rep->uses[0];
                mir->ssa_rep->uses = src_ssa;
                mir->ssa_rep->num_uses = 3;
              }
              mir->ssa_rep->num_defs = 1;
              mir->ssa_rep->defs =
                  static_cast<int*>(arena_->Alloc(sizeof(int) * 1, kArenaAllocDFInfo));
              mir->ssa_rep->fp_def =
                  static_cast<bool*>(arena_->Alloc(sizeof(bool) * 1, kArenaAllocDFInfo));
              mir->ssa_rep->fp_def[0] = if_true->ssa_rep->fp_def[0];
              // Match type of uses to def.
              mir->ssa_rep->fp_use =
                  static_cast<bool*>(arena_->Alloc(sizeof(bool) * mir->ssa_rep->num_uses,
                                                   kArenaAllocDFInfo));
              for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
                mir->ssa_rep->fp_use[i] = mir->ssa_rep->fp_def[0];
              }
              /*
               * There is usually a Phi node in the join block for our two cases.  If the
               * Phi node only contains our two cases as input, we will use the result
               * SSA name of the Phi node as our select result and delete the Phi.  If
               * the Phi node has more than two operands, we will arbitrarily use the SSA
               * name of the "true" path, delete the SSA name of the "false" path from the
               * Phi node (and fix up the incoming arc list).
               */
              if (phi->ssa_rep->num_uses == 2) {
                mir->ssa_rep->defs[0] = phi->ssa_rep->defs[0];
                phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              } else {
                int dead_def = if_false->ssa_rep->defs[0];
                int live_def = if_true->ssa_rep->defs[0];
                mir->ssa_rep->defs[0] = live_def;
                BasicBlockId* incoming = phi->meta.phi_incoming;
                for (int i = 0; i < phi->ssa_rep->num_uses; i++) {
                  if (phi->ssa_rep->uses[i] == live_def) {
                    incoming[i] = bb->id;
                  }
                }
                for (int i = 0; i < phi->ssa_rep->num_uses; i++) {
                  if (phi->ssa_rep->uses[i] == dead_def) {
                    int last_slot = phi->ssa_rep->num_uses - 1;
                    phi->ssa_rep->uses[i] = phi->ssa_rep->uses[last_slot];
                    incoming[i] = incoming[last_slot];
                  }
                }
              }
              phi->ssa_rep->num_uses--;
              bb->taken = NullBasicBlockId;
              tk->block_type = kDead;
              for (MIR* tmir = ft->first_mir_insn; tmir != NULL; tmir = tmir->next) {
                tmir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
              }
            }
          }
        }
      }
    }
    bb = ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) ? NextDominatedBlock(bb) : NULL;
  }
  if (use_lvn && UNLIKELY(!global_valnum->Good())) {
    LOG(WARNING) << "LVN overflow in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }

  return true;
}

/* Collect stats on number of checks removed */
void MIRGraph::CountChecks(class BasicBlock* bb) {
  if (bb->data_flow_info != NULL) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      if (mir->ssa_rep == NULL) {
        continue;
      }
      uint64_t df_attributes = GetDataFlowAttributes(mir);
      if (df_attributes & DF_HAS_NULL_CHKS) {
        checkstats_->null_checks++;
        if (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) {
          checkstats_->null_checks_eliminated++;
        }
      }
      if (df_attributes & DF_HAS_RANGE_CHKS) {
        checkstats_->range_checks++;
        if (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) {
          checkstats_->range_checks_eliminated++;
        }
      }
    }
  }
}

/* Try to make common case the fallthrough path. */
bool MIRGraph::LayoutBlocks(BasicBlock* bb) {
  // TODO: For now, just looking for direct throws.  Consider generalizing for profile feedback.
  if (!bb->explicit_throw) {
    return false;
  }

  // If we visited it, we are done.
  if (bb->visited) {
    return false;
  }
  bb->visited = true;

  BasicBlock* walker = bb;
  while (true) {
    // Check termination conditions.
    if ((walker->block_type == kEntryBlock) || (Predecessors(walker) != 1)) {
      break;
    }
    DCHECK(!walker->predecessors.empty());
    BasicBlock* prev = GetBasicBlock(walker->predecessors[0]);

    // If we visited the predecessor, we are done.
    if (prev->visited) {
      return false;
    }
    prev->visited = true;

    if (prev->conditional_branch) {
      if (GetBasicBlock(prev->fall_through) == walker) {
        // Already done - return.
        break;
      }
      DCHECK_EQ(walker, GetBasicBlock(prev->taken));
      // Got one.  Flip it and exit.
      Instruction::Code opcode = prev->last_mir_insn->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::IF_EQ: opcode = Instruction::IF_NE; break;
        case Instruction::IF_NE: opcode = Instruction::IF_EQ; break;
        case Instruction::IF_LT: opcode = Instruction::IF_GE; break;
        case Instruction::IF_GE: opcode = Instruction::IF_LT; break;
        case Instruction::IF_GT: opcode = Instruction::IF_LE; break;
        case Instruction::IF_LE: opcode = Instruction::IF_GT; break;
        case Instruction::IF_EQZ: opcode = Instruction::IF_NEZ; break;
        case Instruction::IF_NEZ: opcode = Instruction::IF_EQZ; break;
        case Instruction::IF_LTZ: opcode = Instruction::IF_GEZ; break;
        case Instruction::IF_GEZ: opcode = Instruction::IF_LTZ; break;
        case Instruction::IF_GTZ: opcode = Instruction::IF_LEZ; break;
        case Instruction::IF_LEZ: opcode = Instruction::IF_GTZ; break;
        default: LOG(FATAL) << "Unexpected opcode " << opcode;
      }
      prev->last_mir_insn->dalvikInsn.opcode = opcode;
      BasicBlockId t_bb = prev->taken;
      prev->taken = prev->fall_through;
      prev->fall_through = t_bb;
      break;
    }
    walker = prev;

    if (walker->visited) {
      break;
    }
  }
  return false;
}

/* Combine any basic blocks terminated by instructions that we now know can't throw */
void MIRGraph::CombineBlocks(class BasicBlock* bb) {
  // Loop here to allow combining a sequence of blocks
  while ((bb->block_type == kDalvikByteCode) &&
      (bb->last_mir_insn != nullptr) &&
      (static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) == kMirOpCheck)) {
    MIR* mir = bb->last_mir_insn;
    DCHECK(bb->first_mir_insn !=  nullptr);

    // Grab the attributes from the paired opcode.
    MIR* throw_insn = mir->meta.throw_insn;
    uint64_t df_attributes = GetDataFlowAttributes(throw_insn);

    // Don't combine if the throw_insn can still throw NPE.
    if ((df_attributes & DF_HAS_NULL_CHKS) != 0 &&
        (throw_insn->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0) {
      break;
    }
    // Now whitelist specific instructions.
    bool ok = false;
    if ((df_attributes & DF_IFIELD) != 0) {
      // Combine only if fast, otherwise weird things can happen.
      const MirIFieldLoweringInfo& field_info = GetIFieldLoweringInfo(throw_insn);
      ok = (df_attributes & DF_DA)  ? field_info.FastGet() : field_info.FastPut();
    } else if ((df_attributes & DF_SFIELD) != 0) {
      // Combine only if fast, otherwise weird things can happen.
      const MirSFieldLoweringInfo& field_info = GetSFieldLoweringInfo(throw_insn);
      bool fast = ((df_attributes & DF_DA)  ? field_info.FastGet() : field_info.FastPut());
      // Don't combine if the SGET/SPUT can call <clinit>().
      bool clinit = !field_info.IsClassInitialized() &&
          (throw_insn->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0;
      ok = fast && !clinit;
    } else if ((df_attributes & DF_HAS_RANGE_CHKS) != 0) {
      // Only AGET/APUT have range checks. We have processed the AGET/APUT null check above.
      DCHECK_NE(throw_insn->optimization_flags & MIR_IGNORE_NULL_CHECK, 0);
      ok = ((throw_insn->optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0);
    } else if ((throw_insn->dalvikInsn.FlagsOf() & Instruction::kThrow) == 0) {
      // We can encounter a non-throwing insn here thanks to inlining or other optimizations.
      ok = true;
    } else if (throw_insn->dalvikInsn.opcode == Instruction::ARRAY_LENGTH ||
        throw_insn->dalvikInsn.opcode == Instruction::FILL_ARRAY_DATA ||
        static_cast<int>(throw_insn->dalvikInsn.opcode) == kMirOpNullCheck) {
      // No more checks for these (null check was processed above).
      ok = true;
    }
    if (!ok) {
      break;
    }

    // OK - got one.  Combine
    BasicBlock* bb_next = GetBasicBlock(bb->fall_through);
    DCHECK(!bb_next->catch_entry);
    DCHECK_EQ(bb_next->predecessors.size(), 1u);

    // Now move instructions from bb_next to bb. Start off with doing a sanity check
    // that kMirOpCheck's throw instruction is first one in the bb_next.
    DCHECK_EQ(bb_next->first_mir_insn, throw_insn);
    // Now move all instructions (throw instruction to last one) from bb_next to bb.
    MIR* last_to_move = bb_next->last_mir_insn;
    bb_next->RemoveMIRList(throw_insn, last_to_move);
    bb->InsertMIRListAfter(bb->last_mir_insn, throw_insn, last_to_move);
    // The kMirOpCheck instruction is not needed anymore.
    mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
    bb->RemoveMIR(mir);

    // Before we overwrite successors, remove their predecessor links to bb.
    bb_next->ErasePredecessor(bb->id);
    if (bb->taken != NullBasicBlockId) {
      DCHECK_EQ(bb->successor_block_list_type, kNotUsed);
      BasicBlock* bb_taken = GetBasicBlock(bb->taken);
      // bb->taken will be overwritten below.
      DCHECK_EQ(bb_taken->block_type, kExceptionHandling);
      DCHECK_EQ(bb_taken->predecessors.size(), 1u);
      DCHECK_EQ(bb_taken->predecessors[0], bb->id);
      bb_taken->predecessors.clear();
      bb_taken->block_type = kDead;
      DCHECK(bb_taken->data_flow_info == nullptr);
    } else {
      DCHECK_EQ(bb->successor_block_list_type, kCatch);
      for (SuccessorBlockInfo* succ_info : bb->successor_blocks) {
        if (succ_info->block != NullBasicBlockId) {
          BasicBlock* succ_bb = GetBasicBlock(succ_info->block);
          DCHECK(succ_bb->catch_entry);
          succ_bb->ErasePredecessor(bb->id);
          if (succ_bb->predecessors.empty()) {
            succ_bb->KillUnreachable(this);
          }
        }
      }
    }
    // Use the successor info from the next block
    bb->successor_block_list_type = bb_next->successor_block_list_type;
    bb->successor_blocks.swap(bb_next->successor_blocks);  // Swap instead of copying.
    bb_next->successor_block_list_type = kNotUsed;
    // Use the ending block linkage from the next block
    bb->fall_through = bb_next->fall_through;
    bb_next->fall_through = NullBasicBlockId;
    bb->taken = bb_next->taken;
    bb_next->taken = NullBasicBlockId;
    /*
     * If lower-half of pair of blocks to combine contained
     * a return or a conditional branch or an explicit throw,
     * move the flag to the newly combined block.
     */
    bb->terminated_by_return = bb_next->terminated_by_return;
    bb->conditional_branch = bb_next->conditional_branch;
    bb->explicit_throw = bb_next->explicit_throw;
    // Merge the use_lvn flag.
    bb->use_lvn |= bb_next->use_lvn;

    // Kill the unused block.
    bb_next->data_flow_info = nullptr;

    /*
     * NOTE: we aren't updating all dataflow info here.  Should either make sure this pass
     * happens after uses of i_dominated, dom_frontier or update the dataflow info here.
     * NOTE: GVN uses bb->data_flow_info->live_in_v which is unaffected by the block merge.
     */

    // Kill bb_next and remap now-dead id to parent.
    bb_next->block_type = kDead;
    bb_next->data_flow_info = nullptr;  // Must be null for dead blocks. (Relied on by the GVN.)
    block_id_map_.Overwrite(bb_next->id, bb->id);
    // Update predecessors in children.
    ChildBlockIterator iter(bb, this);
    for (BasicBlock* child = iter.Next(); child != nullptr; child = iter.Next()) {
      child->UpdatePredecessor(bb_next->id, bb->id);
    }

    // DFS orders are not up to date anymore.
    dfs_orders_up_to_date_ = false;

    // Now, loop back and see if we can keep going
  }
}

bool MIRGraph::EliminateNullChecksGate() {
  if ((cu_->disable_opt & (1 << kNullCheckElimination)) != 0 ||
      (merged_df_flags_ & DF_HAS_NULL_CHKS) == 0) {
    return false;
  }

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.nce.num_vregs = GetNumOfCodeAndTempVRs();
  temp_.nce.work_vregs_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.nce.num_vregs, false, kBitMapNullCheck);
  temp_.nce.ending_vregs_to_check_matrix = static_cast<ArenaBitVector**>(
      temp_scoped_alloc_->Alloc(sizeof(ArenaBitVector*) * GetNumBlocks(), kArenaAllocMisc));
  std::fill_n(temp_.nce.ending_vregs_to_check_matrix, GetNumBlocks(), nullptr);

  // reset MIR_MARK
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      mir->optimization_flags &= ~MIR_MARK;
    }
  }

  return true;
}

/*
 * Eliminate unnecessary null checks for a basic block.
 */
bool MIRGraph::EliminateNullChecks(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock) {
    // Ignore the kExitBlock as well.
    DCHECK(bb->first_mir_insn == nullptr);
    return false;
  }

  ArenaBitVector* vregs_to_check = temp_.nce.work_vregs_to_check;
  /*
   * Set initial state. Catch blocks don't need any special treatment.
   */
  if (bb->block_type == kEntryBlock) {
    vregs_to_check->ClearAllBits();
    // Assume all ins are objects.
    for (uint16_t in_reg = GetFirstInVR();
         in_reg < GetNumOfCodeVRs(); in_reg++) {
      vregs_to_check->SetBit(in_reg);
    }
    if ((cu_->access_flags & kAccStatic) == 0) {
      // If non-static method, mark "this" as non-null.
      int this_reg = GetFirstInVR();
      vregs_to_check->ClearBit(this_reg);
    }
  } else {
    DCHECK_EQ(bb->block_type, kDalvikByteCode);
    // Starting state is union of all incoming arcs.
    bool copied_first = false;
    for (BasicBlockId pred_id : bb->predecessors) {
      if (temp_.nce.ending_vregs_to_check_matrix[pred_id] == nullptr) {
        continue;
      }
      BasicBlock* pred_bb = GetBasicBlock(pred_id);
      DCHECK(pred_bb != nullptr);
      MIR* null_check_insn = nullptr;
      if (pred_bb->block_type == kDalvikByteCode) {
        // Check to see if predecessor had an explicit null-check.
        MIR* last_insn = pred_bb->last_mir_insn;
        if (last_insn != nullptr) {
          Instruction::Code last_opcode = last_insn->dalvikInsn.opcode;
          if ((last_opcode == Instruction::IF_EQZ && pred_bb->fall_through == bb->id) ||
              (last_opcode == Instruction::IF_NEZ && pred_bb->taken == bb->id)) {
            // Remember the null check insn if there's no other predecessor requiring null check.
            if (!copied_first || !vregs_to_check->IsBitSet(last_insn->dalvikInsn.vA)) {
              null_check_insn = last_insn;
            }
          }
        }
      }
      if (!copied_first) {
        copied_first = true;
        vregs_to_check->Copy(temp_.nce.ending_vregs_to_check_matrix[pred_id]);
      } else {
        vregs_to_check->Union(temp_.nce.ending_vregs_to_check_matrix[pred_id]);
      }
      if (null_check_insn != nullptr) {
        vregs_to_check->ClearBit(null_check_insn->dalvikInsn.vA);
      }
    }
    DCHECK(copied_first);  // At least one predecessor must have been processed before this bb.
  }
  // At this point, vregs_to_check shows which sregs have an object definition with
  // no intervening uses.

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    uint64_t df_attributes = GetDataFlowAttributes(mir);

    if ((df_attributes & DF_NULL_TRANSFER_N) != 0u) {
      // The algorithm was written in a phi agnostic way.
      continue;
    }

    // Might need a null check?
    if (df_attributes & DF_HAS_NULL_CHKS) {
      int src_vreg;
      if (df_attributes & DF_NULL_CHK_OUT0) {
        DCHECK_NE(df_attributes & DF_IS_INVOKE, 0u);
        src_vreg = mir->dalvikInsn.vC;
      } else if (df_attributes & DF_NULL_CHK_B) {
        DCHECK_NE(df_attributes & DF_REF_B, 0u);
        src_vreg = mir->dalvikInsn.vB;
      } else {
        DCHECK_NE(df_attributes & DF_NULL_CHK_A, 0u);
        DCHECK_NE(df_attributes & DF_REF_A, 0u);
        src_vreg = mir->dalvikInsn.vA;
      }
      if (!vregs_to_check->IsBitSet(src_vreg)) {
        // Eliminate the null check.
        mir->optimization_flags |= MIR_MARK;
      } else {
        // Do the null check.
        mir->optimization_flags &= ~MIR_MARK;
        // Mark src_vreg as null-checked.
        vregs_to_check->ClearBit(src_vreg);
      }
    }

    if ((df_attributes & DF_A_WIDE) ||
        (df_attributes & (DF_REF_A | DF_SETS_CONST | DF_NULL_TRANSFER)) == 0) {
      continue;
    }

    /*
     * First, mark all object definitions as requiring null check.
     * Note: we can't tell if a CONST definition might be used as an object, so treat
     * them all as object definitions.
     */
    if ((df_attributes & (DF_DA | DF_REF_A)) == (DF_DA | DF_REF_A) ||
        (df_attributes & DF_SETS_CONST))  {
      vregs_to_check->SetBit(mir->dalvikInsn.vA);
    }

    // Then, remove mark from all object definitions we know are non-null.
    if (df_attributes & DF_NON_NULL_DST) {
      // Mark target of NEW* as non-null
      DCHECK_NE(df_attributes & DF_REF_A, 0u);
      vregs_to_check->ClearBit(mir->dalvikInsn.vA);
    }

    // Mark non-null returns from invoke-style NEW*
    if (df_attributes & DF_NON_NULL_RET) {
      MIR* next_mir = mir->next;
      // Next should be an MOVE_RESULT_OBJECT
      if (UNLIKELY(next_mir == nullptr)) {
        // The MethodVerifier makes sure there's no MOVE_RESULT at the catch entry or branch
        // target, so the MOVE_RESULT cannot be broken away into another block.
        LOG(WARNING) << "Unexpected end of block following new";
      } else if (UNLIKELY(next_mir->dalvikInsn.opcode != Instruction::MOVE_RESULT_OBJECT)) {
        LOG(WARNING) << "Unexpected opcode following new: " << next_mir->dalvikInsn.opcode;
      } else {
        // Mark as null checked.
        vregs_to_check->ClearBit(next_mir->dalvikInsn.vA);
      }
    }

    // Propagate null check state on register copies.
    if (df_attributes & DF_NULL_TRANSFER_0) {
      DCHECK_EQ(df_attributes | ~(DF_DA | DF_REF_A | DF_UB | DF_REF_B), static_cast<uint64_t>(-1));
      if (vregs_to_check->IsBitSet(mir->dalvikInsn.vB)) {
        vregs_to_check->SetBit(mir->dalvikInsn.vA);
      } else {
        vregs_to_check->ClearBit(mir->dalvikInsn.vA);
      }
    }
  }

  // Did anything change?
  bool nce_changed = false;
  ArenaBitVector* old_ending_ssa_regs_to_check = temp_.nce.ending_vregs_to_check_matrix[bb->id];
  if (old_ending_ssa_regs_to_check == nullptr) {
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    nce_changed = vregs_to_check->GetHighestBitSet() != -1;
    temp_.nce.ending_vregs_to_check_matrix[bb->id] = vregs_to_check;
    // Create a new vregs_to_check for next BB.
    temp_.nce.work_vregs_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_.nce.num_vregs, false, kBitMapNullCheck);
  } else if (!vregs_to_check->SameBitsSet(old_ending_ssa_regs_to_check)) {
    nce_changed = true;
    temp_.nce.ending_vregs_to_check_matrix[bb->id] = vregs_to_check;
    temp_.nce.work_vregs_to_check = old_ending_ssa_regs_to_check;  // Reuse for next BB.
  }
  return nce_changed;
}

void MIRGraph::EliminateNullChecksEnd() {
  // Clean up temporaries.
  temp_.nce.num_vregs = 0u;
  temp_.nce.work_vregs_to_check = nullptr;
  temp_.nce.ending_vregs_to_check_matrix = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();

  // converge MIR_MARK with MIR_IGNORE_NULL_CHECK
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
      constexpr int kMarkToIgnoreNullCheckShift = kMIRMark - kMIRIgnoreNullCheck;
      static_assert(kMarkToIgnoreNullCheckShift > 0, "Not a valid right-shift");
      uint16_t mirMarkAdjustedToIgnoreNullCheck =
          (mir->optimization_flags & MIR_MARK) >> kMarkToIgnoreNullCheckShift;
      mir->optimization_flags |= mirMarkAdjustedToIgnoreNullCheck;
    }
  }
}

/*
 * Perform type and size inference for a basic block.
 */
bool MIRGraph::InferTypes(BasicBlock* bb) {
  if (bb->data_flow_info == nullptr) return false;

  bool infer_changed = false;
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->ssa_rep == NULL) {
        continue;
    }

    // Propagate type info.
    infer_changed = InferTypeAndSize(bb, mir, infer_changed);
  }

  return infer_changed;
}

bool MIRGraph::EliminateClassInitChecksGate() {
  if ((cu_->disable_opt & (1 << kClassInitCheckElimination)) != 0 ||
      (merged_df_flags_ & DF_CLINIT) == 0) {
    return false;
  }

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));

  // Each insn we use here has at least 2 code units, offset/2 will be a unique index.
  const size_t end = (GetNumDalvikInsns() + 1u) / 2u;
  temp_.cice.indexes = static_cast<uint16_t*>(
      temp_scoped_alloc_->Alloc(end * sizeof(*temp_.cice.indexes), kArenaAllocGrowableArray));
  std::fill_n(temp_.cice.indexes, end, 0xffffu);

  uint32_t unique_class_count = 0u;
  {
    // Get unique_class_count and store indexes in temp_insn_data_ using a map on a nested
    // ScopedArenaAllocator.

    // Embed the map value in the entry to save space.
    struct MapEntry {
      // Map key: the class identified by the declaring dex file and type index.
      const DexFile* declaring_dex_file;
      uint16_t declaring_class_idx;
      // Map value: index into bit vectors of classes requiring initialization checks.
      uint16_t index;
    };
    struct MapEntryComparator {
      bool operator()(const MapEntry& lhs, const MapEntry& rhs) const {
        if (lhs.declaring_class_idx != rhs.declaring_class_idx) {
          return lhs.declaring_class_idx < rhs.declaring_class_idx;
        }
        return lhs.declaring_dex_file < rhs.declaring_dex_file;
      }
    };

    ScopedArenaAllocator allocator(&cu_->arena_stack);
    ScopedArenaSet<MapEntry, MapEntryComparator> class_to_index_map(MapEntryComparator(),
                                                                    allocator.Adapter());

    // First, find all SGET/SPUTs that may need class initialization checks, record INVOKE_STATICs.
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (bb->block_type == kDalvikByteCode) {
        for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
          if (IsInstructionSGetOrSPut(mir->dalvikInsn.opcode)) {
            const MirSFieldLoweringInfo& field_info = GetSFieldLoweringInfo(mir);
            if (!field_info.IsReferrersClass()) {
              DCHECK_LT(class_to_index_map.size(), 0xffffu);
              MapEntry entry = {
                  // Treat unresolved fields as if each had its own class.
                  field_info.IsResolved() ? field_info.DeclaringDexFile()
                                          : nullptr,
                  field_info.IsResolved() ? field_info.DeclaringClassIndex()
                                          : field_info.FieldIndex(),
                  static_cast<uint16_t>(class_to_index_map.size())
              };
              uint16_t index = class_to_index_map.insert(entry).first->index;
              // Using offset/2 for index into temp_.cice.indexes.
              temp_.cice.indexes[mir->offset / 2u] = index;
            }
          } else if (IsInstructionInvokeStatic(mir->dalvikInsn.opcode)) {
            const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(mir);
            DCHECK(method_info.IsStatic());
            if (method_info.FastPath() && !method_info.IsReferrersClass()) {
              MapEntry entry = {
                  method_info.DeclaringDexFile(),
                  method_info.DeclaringClassIndex(),
                  static_cast<uint16_t>(class_to_index_map.size())
              };
              uint16_t index = class_to_index_map.insert(entry).first->index;
              // Using offset/2 for index into temp_.cice.indexes.
              temp_.cice.indexes[mir->offset / 2u] = index;
            }
          }
        }
      }
    }
    unique_class_count = static_cast<uint32_t>(class_to_index_map.size());
  }

  if (unique_class_count == 0u) {
    // All SGET/SPUTs refer to initialized classes. Nothing to do.
    temp_.cice.indexes = nullptr;
    temp_scoped_alloc_.reset();
    return false;
  }

  // 2 bits for each class: is class initialized, is class in dex cache.
  temp_.cice.num_class_bits = 2u * unique_class_count;
  temp_.cice.work_classes_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.cice.num_class_bits, false, kBitMapClInitCheck);
  temp_.cice.ending_classes_to_check_matrix = static_cast<ArenaBitVector**>(
      temp_scoped_alloc_->Alloc(sizeof(ArenaBitVector*) * GetNumBlocks(), kArenaAllocMisc));
  std::fill_n(temp_.cice.ending_classes_to_check_matrix, GetNumBlocks(), nullptr);
  DCHECK_GT(temp_.cice.num_class_bits, 0u);
  return true;
}

/*
 * Eliminate unnecessary class initialization checks for a basic block.
 */
bool MIRGraph::EliminateClassInitChecks(BasicBlock* bb) {
  DCHECK_EQ((cu_->disable_opt & (1 << kClassInitCheckElimination)), 0u);
  if (bb->block_type != kDalvikByteCode && bb->block_type != kEntryBlock) {
    // Ignore the kExitBlock as well.
    DCHECK(bb->first_mir_insn == nullptr);
    return false;
  }

  /*
   * Set initial state.  Catch blocks don't need any special treatment.
   */
  ArenaBitVector* classes_to_check = temp_.cice.work_classes_to_check;
  DCHECK(classes_to_check != nullptr);
  if (bb->block_type == kEntryBlock) {
    classes_to_check->SetInitialBits(temp_.cice.num_class_bits);
  } else {
    // Starting state is union of all incoming arcs.
    bool copied_first = false;
    for (BasicBlockId pred_id : bb->predecessors) {
      if (temp_.cice.ending_classes_to_check_matrix[pred_id] == nullptr) {
        continue;
      }
      if (!copied_first) {
        copied_first = true;
        classes_to_check->Copy(temp_.cice.ending_classes_to_check_matrix[pred_id]);
      } else {
        classes_to_check->Union(temp_.cice.ending_classes_to_check_matrix[pred_id]);
      }
    }
    DCHECK(copied_first);  // At least one predecessor must have been processed before this bb.
  }
  // At this point, classes_to_check shows which classes need clinit checks.

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    uint16_t index = temp_.cice.indexes[mir->offset / 2u];
    if (index != 0xffffu) {
      bool check_initialization = false;
      bool check_dex_cache = false;

      // NOTE: index != 0xffff does not guarantee that this is an SGET/SPUT/INVOKE_STATIC.
      // Dex instructions with width 1 can have the same offset/2.

      if (IsInstructionSGetOrSPut(mir->dalvikInsn.opcode)) {
        check_initialization = true;
        check_dex_cache = true;
      } else if (IsInstructionInvokeStatic(mir->dalvikInsn.opcode)) {
        check_initialization = true;
        // NOTE: INVOKE_STATIC doesn't guarantee that the type will be in the dex cache.
      }

      if (check_dex_cache) {
        uint32_t check_dex_cache_index = 2u * index + 1u;
        if (!classes_to_check->IsBitSet(check_dex_cache_index)) {
          // Eliminate the class init check.
          mir->optimization_flags |= MIR_CLASS_IS_IN_DEX_CACHE;
        } else {
          // Do the class init check.
          mir->optimization_flags &= ~MIR_CLASS_IS_IN_DEX_CACHE;
        }
        classes_to_check->ClearBit(check_dex_cache_index);
      }
      if (check_initialization) {
        uint32_t check_clinit_index = 2u * index;
        if (!classes_to_check->IsBitSet(check_clinit_index)) {
          // Eliminate the class init check.
          mir->optimization_flags |= MIR_CLASS_IS_INITIALIZED;
        } else {
          // Do the class init check.
          mir->optimization_flags &= ~MIR_CLASS_IS_INITIALIZED;
        }
        // Mark the class as initialized.
        classes_to_check->ClearBit(check_clinit_index);
      }
    }
  }

  // Did anything change?
  bool changed = false;
  ArenaBitVector* old_ending_classes_to_check = temp_.cice.ending_classes_to_check_matrix[bb->id];
  if (old_ending_classes_to_check == nullptr) {
    DCHECK(temp_scoped_alloc_.get() != nullptr);
    changed = classes_to_check->GetHighestBitSet() != -1;
    temp_.cice.ending_classes_to_check_matrix[bb->id] = classes_to_check;
    // Create a new classes_to_check for next BB.
    temp_.cice.work_classes_to_check = new (temp_scoped_alloc_.get()) ArenaBitVector(
        temp_scoped_alloc_.get(), temp_.cice.num_class_bits, false, kBitMapClInitCheck);
  } else if (!classes_to_check->Equal(old_ending_classes_to_check)) {
    changed = true;
    temp_.cice.ending_classes_to_check_matrix[bb->id] = classes_to_check;
    temp_.cice.work_classes_to_check = old_ending_classes_to_check;  // Reuse for next BB.
  }
  return changed;
}

void MIRGraph::EliminateClassInitChecksEnd() {
  // Clean up temporaries.
  temp_.cice.num_class_bits = 0u;
  temp_.cice.work_classes_to_check = nullptr;
  temp_.cice.ending_classes_to_check_matrix = nullptr;
  DCHECK(temp_.cice.indexes != nullptr);
  temp_.cice.indexes = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

bool MIRGraph::ApplyGlobalValueNumberingGate() {
  if (GlobalValueNumbering::Skip(cu_)) {
    return false;
  }

  DCHECK(temp_scoped_alloc_ == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.gvn.ifield_ids_ =
      GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), ifield_lowering_infos_);
  temp_.gvn.sfield_ids_ =
      GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), sfield_lowering_infos_);
  DCHECK(temp_.gvn.gvn == nullptr);
  temp_.gvn.gvn = new (temp_scoped_alloc_.get()) GlobalValueNumbering(
      cu_, temp_scoped_alloc_.get(), GlobalValueNumbering::kModeGvn);
  return true;
}

bool MIRGraph::ApplyGlobalValueNumbering(BasicBlock* bb) {
  DCHECK(temp_.gvn.gvn != nullptr);
  LocalValueNumbering* lvn = temp_.gvn.gvn->PrepareBasicBlock(bb);
  if (lvn != nullptr) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      lvn->GetValueNumber(mir);
    }
  }
  bool change = (lvn != nullptr) && temp_.gvn.gvn->FinishBasicBlock(bb);
  return change;
}

void MIRGraph::ApplyGlobalValueNumberingEnd() {
  // Perform modifications.
  DCHECK(temp_.gvn.gvn != nullptr);
  if (temp_.gvn.gvn->Good()) {
    if (max_nested_loops_ != 0u) {
      temp_.gvn.gvn->StartPostProcessing();
      TopologicalSortIterator iter(this);
      for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
        ScopedArenaAllocator allocator(&cu_->arena_stack);  // Reclaim memory after each LVN.
        LocalValueNumbering* lvn = temp_.gvn.gvn->PrepareBasicBlock(bb, &allocator);
        if (lvn != nullptr) {
          for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
            lvn->GetValueNumber(mir);
          }
          bool change = temp_.gvn.gvn->FinishBasicBlock(bb);
          DCHECK(!change) << PrettyMethod(cu_->method_idx, *cu_->dex_file);
        }
      }
    }
    // GVN was successful, running the LVN would be useless.
    cu_->disable_opt |= (1u << kLocalValueNumbering);
  } else {
    LOG(WARNING) << "GVN failed for " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }

  delete temp_.gvn.gvn;
  temp_.gvn.gvn = nullptr;
  temp_.gvn.ifield_ids_ = nullptr;
  temp_.gvn.sfield_ids_ = nullptr;
  DCHECK(temp_scoped_alloc_ != nullptr);
  temp_scoped_alloc_.reset();
}

void MIRGraph::ComputeInlineIFieldLoweringInfo(uint16_t field_idx, MIR* invoke, MIR* iget_or_iput) {
  uint32_t method_index = invoke->meta.method_lowering_info;
  if (temp_.smi.processed_indexes->IsBitSet(method_index)) {
    iget_or_iput->meta.ifield_lowering_info = temp_.smi.lowering_infos[method_index];
    DCHECK_EQ(field_idx, GetIFieldLoweringInfo(iget_or_iput).FieldIndex());
    return;
  }

  const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(invoke);
  MethodReference target = method_info.GetTargetMethod();
  DexCompilationUnit inlined_unit(
      cu_, cu_->class_loader, cu_->class_linker, *target.dex_file,
      nullptr /* code_item not used */, 0u /* class_def_idx not used */, target.dex_method_index,
      0u /* access_flags not used */, nullptr /* verified_method not used */);
  DexMemAccessType type = IGetOrIPutMemAccessType(iget_or_iput->dalvikInsn.opcode);
  MirIFieldLoweringInfo inlined_field_info(field_idx, type);
  MirIFieldLoweringInfo::Resolve(cu_->compiler_driver, &inlined_unit, &inlined_field_info, 1u);
  DCHECK(inlined_field_info.IsResolved());

  uint32_t field_info_index = ifield_lowering_infos_.size();
  ifield_lowering_infos_.push_back(inlined_field_info);
  temp_.smi.processed_indexes->SetBit(method_index);
  temp_.smi.lowering_infos[method_index] = field_info_index;
  iget_or_iput->meta.ifield_lowering_info = field_info_index;
}

bool MIRGraph::InlineSpecialMethodsGate() {
  if ((cu_->disable_opt & (1 << kSuppressMethodInlining)) != 0 ||
      method_lowering_infos_.size() == 0u) {
    return false;
  }
  if (cu_->compiler_driver->GetMethodInlinerMap() == nullptr) {
    // This isn't the Quick compiler.
    return false;
  }
  return true;
}

void MIRGraph::InlineSpecialMethodsStart() {
  // Prepare for inlining getters/setters. Since we're inlining at most 1 IGET/IPUT from
  // each INVOKE, we can index the data by the MIR::meta::method_lowering_info index.

  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_.smi.num_indexes = method_lowering_infos_.size();
  temp_.smi.processed_indexes = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_.smi.num_indexes, false, kBitMapMisc);
  temp_.smi.processed_indexes->ClearAllBits();
  temp_.smi.lowering_infos = static_cast<uint16_t*>(temp_scoped_alloc_->Alloc(
      temp_.smi.num_indexes * sizeof(*temp_.smi.lowering_infos), kArenaAllocGrowableArray));
}

void MIRGraph::InlineSpecialMethods(BasicBlock* bb) {
  if (bb->block_type != kDalvikByteCode) {
    return;
  }
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode)) {
      continue;
    }
    if (!(mir->dalvikInsn.FlagsOf() & Instruction::kInvoke)) {
      continue;
    }
    const MirMethodLoweringInfo& method_info = GetMethodLoweringInfo(mir);
    if (!method_info.FastPath()) {
      continue;
    }

    InvokeType sharp_type = method_info.GetSharpType();
    if ((sharp_type != kDirect) && (sharp_type != kStatic)) {
      continue;
    }

    if (sharp_type == kStatic) {
      bool needs_clinit = !method_info.IsClassInitialized() &&
          ((mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0);
      if (needs_clinit) {
        continue;
      }
    }

    DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
    MethodReference target = method_info.GetTargetMethod();
    if (cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(target.dex_file)
            ->GenInline(this, bb, mir, target.dex_method_index)) {
      if (cu_->verbose || cu_->print_pass) {
        LOG(INFO) << "SpecialMethodInliner: Inlined " << method_info.GetInvokeType() << " ("
            << sharp_type << ") call to \"" << PrettyMethod(target.dex_method_index, *target.dex_file)
            << "\" from \"" << PrettyMethod(cu_->method_idx, *cu_->dex_file)
            << "\" @0x" << std::hex << mir->offset;
      }
    }
  }
}

void MIRGraph::InlineSpecialMethodsEnd() {
  // Clean up temporaries.
  DCHECK(temp_.smi.lowering_infos != nullptr);
  temp_.smi.lowering_infos = nullptr;
  temp_.smi.num_indexes = 0u;
  DCHECK(temp_.smi.processed_indexes != nullptr);
  temp_.smi.processed_indexes = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

void MIRGraph::DumpCheckStats() {
  Checkstats* stats =
      static_cast<Checkstats*>(arena_->Alloc(sizeof(Checkstats), kArenaAllocDFInfo));
  checkstats_ = stats;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    CountChecks(bb);
  }
  if (stats->null_checks > 0) {
    float eliminated = static_cast<float>(stats->null_checks_eliminated);
    float checks = static_cast<float>(stats->null_checks);
    LOG(INFO) << "Null Checks: " << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
              << stats->null_checks_eliminated << " of " << stats->null_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
    }
  if (stats->range_checks > 0) {
    float eliminated = static_cast<float>(stats->range_checks_eliminated);
    float checks = static_cast<float>(stats->range_checks);
    LOG(INFO) << "Range Checks: " << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
              << stats->range_checks_eliminated << " of " << stats->range_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
  }
}

bool MIRGraph::BuildExtendedBBList(class BasicBlock* bb) {
  if (bb->visited) return false;
  if (!((bb->block_type == kEntryBlock) || (bb->block_type == kDalvikByteCode)
      || (bb->block_type == kExitBlock))) {
    // Ignore special blocks
    bb->visited = true;
    return false;
  }
  // Must be head of extended basic block.
  BasicBlock* start_bb = bb;
  extended_basic_blocks_.push_back(bb->id);
  bool terminated_by_return = false;
  bool do_local_value_numbering = false;
  // Visit blocks strictly dominated by this head.
  while (bb != NULL) {
    bb->visited = true;
    terminated_by_return |= bb->terminated_by_return;
    do_local_value_numbering |= bb->use_lvn;
    bb = NextDominatedBlock(bb);
  }
  if (terminated_by_return || do_local_value_numbering) {
    // Do lvn for all blocks in this extended set.
    bb = start_bb;
    while (bb != NULL) {
      bb->use_lvn = do_local_value_numbering;
      bb->dominates_return = terminated_by_return;
      bb = NextDominatedBlock(bb);
    }
  }
  return false;  // Not iterative - return value will be ignored
}

void MIRGraph::BasicBlockOptimization() {
  if ((cu_->disable_opt & (1 << kLocalValueNumbering)) == 0) {
    temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
    temp_.gvn.ifield_ids_ =
        GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), ifield_lowering_infos_);
    temp_.gvn.sfield_ids_ =
        GlobalValueNumbering::PrepareGvnFieldIds(temp_scoped_alloc_.get(), sfield_lowering_infos_);
  }

  if ((cu_->disable_opt & (1 << kSuppressExceptionEdges)) != 0) {
    ClearAllVisitedFlags();
    PreOrderDfsIterator iter2(this);
    for (BasicBlock* bb = iter2.Next(); bb != NULL; bb = iter2.Next()) {
      BuildExtendedBBList(bb);
    }
    // Perform extended basic block optimizations.
    for (unsigned int i = 0; i < extended_basic_blocks_.size(); i++) {
      BasicBlockOpt(GetBasicBlock(extended_basic_blocks_[i]));
    }
  } else {
    PreOrderDfsIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
      BasicBlockOpt(bb);
    }
  }

  // Clean up after LVN.
  temp_.gvn.ifield_ids_ = nullptr;
  temp_.gvn.sfield_ids_ = nullptr;
  temp_scoped_alloc_.reset();
}

}  // namespace art
