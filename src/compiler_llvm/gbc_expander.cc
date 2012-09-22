/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "ir_builder.h"
#include "utils_llvm.h"

#include "compiler.h"
#include "greenland/intrinsic_helper.h"
#include "oat_compilation_unit.h"
#include "object.h"
#include "thread.h"
#include "verifier/method_verifier.h"

#include "compiler/CompilerIR.h"
using art::kMIRIgnoreNullCheck;
using art::kMIRIgnoreRangeCheck;

#include <llvm/ADT/STLExtras.h>
#include <llvm/Intrinsics.h>
#include <llvm/Metadata.h>
#include <llvm/Pass.h>
#include <llvm/Support/CFG.h>
#include <llvm/Support/InstIterator.h>

#include <vector>
#include <map>
#include <utility>

using namespace art::compiler_llvm;

using art::greenland::IntrinsicHelper;

namespace art {
extern char remapShorty(char shortyType);
};

namespace {

class GBCExpanderPass : public llvm::FunctionPass {
 private:
  const IntrinsicHelper& intrinsic_helper_;
  IRBuilder& irb_;

  llvm::LLVMContext& context_;
  RuntimeSupportBuilder& rtb_;

 private:
  llvm::AllocaInst* shadow_frame_;
  llvm::Value* old_shadow_frame_;
  uint32_t shadow_frame_size_;

 private:
  art::Compiler* compiler_;

  const art::DexFile* dex_file_;
  const art::DexFile::CodeItem* code_item_;

  art::OatCompilationUnit* oat_compilation_unit_;

  uint32_t method_idx_;

  llvm::Function* func_;

  std::vector<llvm::BasicBlock*> basic_blocks_;

  std::vector<llvm::BasicBlock*> basic_block_landing_pads_;
  llvm::BasicBlock* current_bb_;
  std::map<llvm::BasicBlock*, std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*> > >
      landing_pad_phi_mapping_;
  llvm::BasicBlock* basic_block_unwind_;

  bool changed_;

 private:
  //----------------------------------------------------------------------------
  // Constant for GBC expansion
  //----------------------------------------------------------------------------
  enum IntegerShiftKind {
    kIntegerSHL,
    kIntegerSHR,
    kIntegerUSHR,
  };

 private:
  //----------------------------------------------------------------------------
  // Helper function for GBC expansion
  //----------------------------------------------------------------------------

  llvm::Value* ExpandToRuntime(runtime_support::RuntimeId rt,
                               llvm::CallInst& inst);

  uint64_t LV2UInt(llvm::Value* lv) {
    return llvm::cast<llvm::ConstantInt>(lv)->getZExtValue();
  }

  int64_t LV2SInt(llvm::Value* lv) {
    return llvm::cast<llvm::ConstantInt>(lv)->getSExtValue();
  }

 private:
  // TODO: Almost all Emit* are directly copy-n-paste from MethodCompiler.
  // Refactor these utility functions from MethodCompiler to avoid forking.

  void EmitStackOverflowCheck(llvm::Instruction* first_non_alloca);

  void RewriteFunction();

  void RewriteBasicBlock(llvm::BasicBlock* original_block);

  void UpdatePhiInstruction(llvm::BasicBlock* old_basic_block,
                            llvm::BasicBlock* new_basic_block);


  //----------------------------------------------------------------------------
  // Dex cache code generation helper function
  //----------------------------------------------------------------------------
  llvm::Value* EmitLoadDexCacheAddr(art::MemberOffset dex_cache_offset);

  llvm::Value* EmitLoadDexCacheStaticStorageFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheResolvedTypeFieldAddr(uint32_t type_idx);

  llvm::Value* EmitLoadDexCacheResolvedMethodFieldAddr(uint32_t method_idx);

  llvm::Value* EmitLoadDexCacheStringFieldAddr(uint32_t string_idx);

  //----------------------------------------------------------------------------
  // Code generation helper function
  //----------------------------------------------------------------------------
  llvm::Value* EmitLoadMethodObjectAddr();

  llvm::Value* EmitLoadArrayLength(llvm::Value* array);

  llvm::Value* EmitLoadSDCalleeMethodObjectAddr(uint32_t callee_method_idx);

  llvm::Value* EmitLoadVirtualCalleeMethodObjectAddr(int vtable_idx,
                                                     llvm::Value* this_addr);

  llvm::Value* EmitArrayGEP(llvm::Value* array_addr,
                            llvm::Value* index_value,
                            JType elem_jty);

 private:
  //----------------------------------------------------------------------------
  // Expand Greenland intrinsics
  //----------------------------------------------------------------------------
  void Expand_TestSuspend(llvm::CallInst& call_inst);

  void Expand_MarkGCCard(llvm::CallInst& call_inst);

  llvm::Value* Expand_GetException();

  llvm::Value* Expand_LoadStringFromDexCache(llvm::Value* string_idx_value);

  llvm::Value* Expand_LoadTypeFromDexCache(llvm::Value* type_idx_value);

  void Expand_LockObject(llvm::Value* obj);

  void Expand_UnlockObject(llvm::Value* obj);

  llvm::Value* Expand_ArrayGet(llvm::Value* array_addr,
                               llvm::Value* index_value,
                               JType elem_jty);

  void Expand_ArrayPut(llvm::Value* new_value,
                       llvm::Value* array_addr,
                       llvm::Value* index_value,
                       JType elem_jty);

  void Expand_FilledNewArray(llvm::CallInst& call_inst);

  llvm::Value* Expand_IGetFast(llvm::Value* field_offset_value,
                               llvm::Value* is_volatile_value,
                               llvm::Value* object_addr,
                               JType field_jty);

  void Expand_IPutFast(llvm::Value* field_offset_value,
                       llvm::Value* is_volatile_value,
                       llvm::Value* object_addr,
                       llvm::Value* new_value,
                       JType field_jty);

  llvm::Value* Expand_SGetFast(llvm::Value* static_storage_addr,
                               llvm::Value* field_offset_value,
                               llvm::Value* is_volatile_value,
                               JType field_jty);

  void Expand_SPutFast(llvm::Value* static_storage_addr,
                       llvm::Value* field_offset_value,
                       llvm::Value* is_volatile_value,
                       llvm::Value* new_value,
                       JType field_jty);

  llvm::Value* Expand_LoadDeclaringClassSSB(llvm::Value* method_object_addr);

  llvm::Value* Expand_LoadClassSSBFromDexCache(llvm::Value* type_idx_value);

  llvm::Value*
  Expand_GetSDCalleeMethodObjAddrFast(llvm::Value* callee_method_idx_value);

  llvm::Value*
  Expand_GetVirtualCalleeMethodObjAddrFast(llvm::Value* vtable_idx_value,
                                           llvm::Value* this_addr);

  llvm::Value* Expand_Invoke(llvm::CallInst& call_inst);

  llvm::Value* Expand_DivRem(llvm::CallInst& call_inst, bool is_div, JType op_jty);

  void Expand_AllocaShadowFrame(llvm::Value* num_entry_value);

  void Expand_SetShadowFrameEntry(llvm::Value* obj, llvm::Value* entry_idx);

  void Expand_PopShadowFrame();

  void Expand_UpdateDexPC(llvm::Value* dex_pc_value);

  //----------------------------------------------------------------------------
  // Quick
  //----------------------------------------------------------------------------

  llvm::Value* Expand_FPCompare(llvm::Value* src1_value,
                                llvm::Value* src2_value,
                                bool gt_bias);

  llvm::Value* Expand_LongCompare(llvm::Value* src1_value, llvm::Value* src2_value);

  llvm::Value* EmitCompareResultSelection(llvm::Value* cmp_eq,
                                          llvm::Value* cmp_lt);

  llvm::Value* EmitLoadConstantClass(uint32_t dex_pc, uint32_t type_idx);
  llvm::Value* EmitLoadStaticStorage(uint32_t dex_pc, uint32_t type_idx);

  llvm::Value* Expand_HLIGet(llvm::CallInst& call_inst, JType field_jty);
  void Expand_HLIPut(llvm::CallInst& call_inst, JType field_jty);

  llvm::Value* Expand_HLSget(llvm::CallInst& call_inst, JType field_jty);
  void Expand_HLSput(llvm::CallInst& call_inst, JType field_jty);

  llvm::Value* Expand_HLArrayGet(llvm::CallInst& call_inst, JType field_jty);
  void Expand_HLArrayPut(llvm::CallInst& call_inst, JType field_jty);

  llvm::Value* Expand_ConstString(llvm::CallInst& call_inst);
  llvm::Value* Expand_ConstClass(llvm::CallInst& call_inst);

  void Expand_MonitorEnter(llvm::CallInst& call_inst);
  void Expand_MonitorExit(llvm::CallInst& call_inst);

  void Expand_HLCheckCast(llvm::CallInst& call_inst);
  llvm::Value* Expand_InstanceOf(llvm::CallInst& call_inst);

  llvm::Value* Expand_NewInstance(llvm::CallInst& call_inst);

  llvm::Value* Expand_HLInvoke(llvm::CallInst& call_inst);

  llvm::Value* Expand_OptArrayLength(llvm::CallInst& call_inst);
  llvm::Value* Expand_NewArray(llvm::CallInst& call_inst);
  llvm::Value* Expand_HLFilledNewArray(llvm::CallInst& call_inst);
  void Expand_HLFillArrayData(llvm::CallInst& call_inst);

  llvm::Value* EmitAllocNewArray(uint32_t dex_pc,
                                 llvm::Value* array_length_value,
                                 uint32_t type_idx,
                                 bool is_filled_new_array);

  llvm::Value* EmitCallRuntimeForCalleeMethodObjectAddr(uint32_t callee_method_idx,
                                                        art::InvokeType invoke_type,
                                                        llvm::Value* this_addr,
                                                        uint32_t dex_pc,
                                                        bool is_fast_path);

  void EmitMarkGCCard(llvm::Value* value, llvm::Value* target_addr);

  void EmitUpdateDexPC(uint32_t dex_pc);

  void EmitGuard_DivZeroException(uint32_t dex_pc,
                                  llvm::Value* denominator,
                                  JType op_jty);

  void EmitGuard_NullPointerException(uint32_t dex_pc,
                                      llvm::Value* object);

  void EmitGuard_ArrayIndexOutOfBoundsException(uint32_t dex_pc,
                                                llvm::Value* array,
                                                llvm::Value* index);

  llvm::FunctionType* GetFunctionType(uint32_t method_idx, bool is_static);

  llvm::BasicBlock* GetBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* CreateBasicBlockWithDexPC(uint32_t dex_pc,
                                              const char* postfix);

  int32_t GetTryItemOffset(uint32_t dex_pc);

  llvm::BasicBlock* GetLandingPadBasicBlock(uint32_t dex_pc);

  llvm::BasicBlock* GetUnwindBasicBlock();

  void EmitGuard_ExceptionLandingPad(uint32_t dex_pc);

  void EmitBranchExceptionLandingPad(uint32_t dex_pc);

  //----------------------------------------------------------------------------
  // Expand Arithmetic Helper Intrinsics
  //----------------------------------------------------------------------------

  llvm::Value* Expand_IntegerShift(llvm::Value* src1_value,
                                   llvm::Value* src2_value,
                                   IntegerShiftKind kind,
                                   JType op_jty);

 public:
  static char ID;

  GBCExpanderPass(const IntrinsicHelper& intrinsic_helper, IRBuilder& irb)
      : llvm::FunctionPass(ID), intrinsic_helper_(intrinsic_helper), irb_(irb),
        context_(irb.getContext()), rtb_(irb.Runtime()),
        shadow_frame_(NULL), old_shadow_frame_(NULL), shadow_frame_size_(0),
        compiler_(NULL), dex_file_(NULL), code_item_(NULL),
        oat_compilation_unit_(NULL), method_idx_(-1u), func_(NULL),
        changed_(false)
  { }

  GBCExpanderPass(const IntrinsicHelper& intrinsic_helper, IRBuilder& irb,
                  art::Compiler* compiler, art::OatCompilationUnit* oat_compilation_unit)
      : llvm::FunctionPass(ID), intrinsic_helper_(intrinsic_helper), irb_(irb),
        context_(irb.getContext()), rtb_(irb.Runtime()),
        shadow_frame_(NULL), old_shadow_frame_(NULL), shadow_frame_size_(0),
        compiler_(compiler),
        dex_file_(oat_compilation_unit->GetDexFile()),
        code_item_(oat_compilation_unit->GetCodeItem()),
        oat_compilation_unit_(oat_compilation_unit),
        method_idx_(oat_compilation_unit->GetDexMethodIndex()),
        func_(NULL), changed_(false)
  { }

  bool runOnFunction(llvm::Function& func);

 private:
  void InsertStackOverflowCheck(llvm::Function& func);

  llvm::Value* ExpandIntrinsic(IntrinsicHelper::IntrinsicId intr_id,
                               llvm::CallInst& call_inst);

};

char GBCExpanderPass::ID = 0;

bool GBCExpanderPass::runOnFunction(llvm::Function& func) {
  // Runtime support or stub
  if (func.getName().startswith("art_") || func.getName().startswith("Art")) {
    return false;
  }

  // Setup rewrite context
  shadow_frame_ = NULL;
  old_shadow_frame_ = NULL;
  shadow_frame_size_ = 0;
  func_ = &func;
  changed_ = false; // Assume unchanged

#if defined(ART_USE_QUICK_COMPILER)
  basic_blocks_.resize(code_item_->insns_size_in_code_units_);
  basic_block_landing_pads_.resize(code_item_->tries_size_, NULL);
  basic_block_unwind_ = NULL;
  for (llvm::Function::iterator bb_iter = func_->begin(), bb_end = func_->end();
       bb_iter != bb_end;
       ++bb_iter) {
    if (bb_iter->begin()->getMetadata("DexOff") == NULL) {
      continue;
    }
    uint32_t dex_pc = LV2UInt(bb_iter->begin()->getMetadata("DexOff")->getOperand(0));
    basic_blocks_[dex_pc] = bb_iter;
  }
#endif

  // Insert stack overflow check
  InsertStackOverflowCheck(func); // TODO: Use intrinsic.

  // Rewrite the intrinsics
  RewriteFunction();

  VERIFY_LLVM_FUNCTION(func);

  return changed_;
}

void GBCExpanderPass::RewriteBasicBlock(llvm::BasicBlock* original_block) {
  llvm::BasicBlock* curr_basic_block = original_block;

  llvm::BasicBlock::iterator inst_iter = original_block->begin();
  llvm::BasicBlock::iterator inst_end = original_block->end();

  while (inst_iter != inst_end) {
    llvm::CallInst* call_inst = llvm::dyn_cast<llvm::CallInst>(inst_iter);
    IntrinsicHelper::IntrinsicId intr_id = IntrinsicHelper::UnknownId;

    if (call_inst) {
      llvm::Function* callee_func = call_inst->getCalledFunction();
      intr_id = intrinsic_helper_.GetIntrinsicId(callee_func);
    }

    if (intr_id == IntrinsicHelper::UnknownId) {
      // This is not intrinsic call.  Skip this instruction.
      ++inst_iter;
      continue;
    }

    // Rewrite the intrinsic and change the function
    changed_ = true;
    irb_.SetInsertPoint(inst_iter);

    // Expand the intrinsic
    if (llvm::Value* new_value = ExpandIntrinsic(intr_id, *call_inst)) {
      inst_iter->replaceAllUsesWith(new_value);
    }

    // Remove the old intrinsic call instruction
    llvm::BasicBlock::iterator old_inst = inst_iter++;
    old_inst->eraseFromParent();

    // Splice the instruction to the new basic block
    llvm::BasicBlock* next_basic_block = irb_.GetInsertBlock();
    if (next_basic_block != curr_basic_block) {
      next_basic_block->getInstList().splice(
          irb_.GetInsertPoint(), curr_basic_block->getInstList(),
          inst_iter, inst_end);
      curr_basic_block = next_basic_block;
      inst_end = curr_basic_block->end();
    }
  }
}


void GBCExpanderPass::RewriteFunction() {
  size_t num_basic_blocks = func_->getBasicBlockList().size();
  // NOTE: We are not using (bb_iter != bb_end) as the for-loop condition,
  // because we will create new basic block while expanding the intrinsics.
  // We only want to iterate through the input basic blocks.

  landing_pad_phi_mapping_.clear();

  for (llvm::Function::iterator bb_iter = func_->begin();
       num_basic_blocks > 0; ++bb_iter, --num_basic_blocks) {
    // Set insert point to current basic block.
    irb_.SetInsertPoint(bb_iter);

    current_bb_ = bb_iter;

    // Rewrite the basic block
    RewriteBasicBlock(bb_iter);

    // Update the phi-instructions in the successor basic block
    llvm::BasicBlock* last_block = irb_.GetInsertBlock();
    if (last_block != bb_iter) {
      UpdatePhiInstruction(bb_iter, last_block);
    }
  }

  typedef std::map<llvm::PHINode*, llvm::PHINode*> HandlerPHIMap;
  HandlerPHIMap handler_phi;
  // Iterate every used landing pad basic block
  for (size_t i = 0, ei = basic_block_landing_pads_.size(); i != ei; ++i) {
    llvm::BasicBlock* lbb = basic_block_landing_pads_[i];
    if (lbb == NULL) {
      continue;
    }

    llvm::TerminatorInst* term_inst = lbb->getTerminator();
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*> >& rewrite_pair
        = landing_pad_phi_mapping_[lbb];
    irb_.SetInsertPoint(lbb->begin());

    // Iterate every succeeding basic block (catch block)
    for (unsigned succ_iter = 0, succ_end = term_inst->getNumSuccessors();
         succ_iter != succ_end; ++succ_iter) {
      llvm::BasicBlock* succ_basic_block = term_inst->getSuccessor(succ_iter);

      // Iterate every phi instructions in the succeeding basic block
      for (llvm::BasicBlock::iterator
           inst_iter = succ_basic_block->begin(),
           inst_end = succ_basic_block->end();
           inst_iter != inst_end; ++inst_iter) {
        llvm::PHINode *phi = llvm::dyn_cast<llvm::PHINode>(inst_iter);

        if (!phi) {
          break; // Meet non-phi instruction.  Done.
        }

        if (handler_phi[phi] == NULL) {
          handler_phi[phi] = llvm::PHINode::Create(phi->getType(), 1);
        }

        // Create new_phi in landing pad
        llvm::PHINode* new_phi = irb_.CreatePHI(phi->getType(), rewrite_pair.size());
        // Insert all incoming value into new_phi by rewrite_pair
        for (size_t j = 0, ej = rewrite_pair.size(); j != ej; ++j) {
          llvm::BasicBlock* old_bb = rewrite_pair[j].first;
          llvm::BasicBlock* new_bb = rewrite_pair[j].second;
          new_phi->addIncoming(phi->getIncomingValueForBlock(old_bb), new_bb);
        }
        // Delete all incoming value from phi by rewrite_pair
        for (size_t j = 0, ej = rewrite_pair.size(); j != ej; ++j) {
          llvm::BasicBlock* old_bb = rewrite_pair[j].first;
          int old_bb_idx = phi->getBasicBlockIndex(old_bb);
          if (old_bb_idx >= 0) {
            phi->removeIncomingValue(old_bb_idx, false);
          }
        }
        // Insert new_phi into new handler phi
        handler_phi[phi]->addIncoming(new_phi, lbb);
      }
    }
  }

  // Replace all handler phi
  // We can't just use the old handler phi, because some exception edges will disappear after we
  // compute fast-path.
  for (HandlerPHIMap::iterator it = handler_phi.begin(); it != handler_phi.end(); ++it) {
    llvm::PHINode* old_phi = it->first;
    llvm::PHINode* new_phi = it->second;
    new_phi->insertBefore(old_phi);
    old_phi->replaceAllUsesWith(new_phi);
    old_phi->eraseFromParent();
  }
}

void GBCExpanderPass::UpdatePhiInstruction(llvm::BasicBlock* old_basic_block,
                                           llvm::BasicBlock* new_basic_block) {
  llvm::TerminatorInst* term_inst = new_basic_block->getTerminator();

  if (!term_inst) {
    return; // No terminating instruction in new_basic_block.  Nothing to do.
  }

  // Iterate every succeeding basic block
  for (unsigned succ_iter = 0, succ_end = term_inst->getNumSuccessors();
       succ_iter != succ_end; ++succ_iter) {
    llvm::BasicBlock* succ_basic_block = term_inst->getSuccessor(succ_iter);

    // Iterate every phi instructions in the succeeding basic block
    for (llvm::BasicBlock::iterator
         inst_iter = succ_basic_block->begin(),
         inst_end = succ_basic_block->end();
         inst_iter != inst_end; ++inst_iter) {
      llvm::PHINode *phi = llvm::dyn_cast<llvm::PHINode>(inst_iter);

      if (!phi) {
        break; // Meet non-phi instruction.  Done.
      }

      // Update the incoming block of this phi instruction
      for (llvm::PHINode::block_iterator
           ibb_iter = phi->block_begin(), ibb_end = phi->block_end();
           ibb_iter != ibb_end; ++ibb_iter) {
        if (*ibb_iter == old_basic_block) {
          *ibb_iter = new_basic_block;
        }
      }
    }
  }
}

llvm::Value* GBCExpanderPass::ExpandToRuntime(runtime_support::RuntimeId rt,
                                              llvm::CallInst& inst) {
  // Some GBC intrinsic can directly replace with IBC runtime. "Directly" means
  // the arguments passed to the GBC intrinsic are as the same as IBC runtime
  // function, therefore only called function is needed to change.
  unsigned num_args = inst.getNumArgOperands();

  if (num_args <= 0) {
    return irb_.CreateCall(irb_.GetRuntime(rt));
  } else {
    std::vector<llvm::Value*> args;
    for (unsigned i = 0; i < num_args; i++) {
      args.push_back(inst.getArgOperand(i));
    }

    return irb_.CreateCall(irb_.GetRuntime(rt), args);
  }
}

void
GBCExpanderPass::EmitStackOverflowCheck(llvm::Instruction* first_non_alloca) {
  llvm::Function* func = first_non_alloca->getParent()->getParent();
  llvm::Module* module = func->getParent();

  // Call llvm intrinsic function to get frame address.
  llvm::Function* frameaddress =
      llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::frameaddress);

  // The type of llvm::frameaddress is: i8* @llvm.frameaddress(i32)
  llvm::Value* frame_address = irb_.CreateCall(frameaddress, irb_.getInt32(0));

  // Cast i8* to int
  frame_address = irb_.CreatePtrToInt(frame_address, irb_.getPtrEquivIntTy());

  // Get thread.stack_end_
  llvm::Value* stack_end =
    irb_.Runtime().EmitLoadFromThreadOffset(art::Thread::StackEndOffset().Int32Value(),
                                            irb_.getPtrEquivIntTy(),
                                            kTBAARuntimeInfo);

  // Check the frame address < thread.stack_end_ ?
  llvm::Value* is_stack_overflow = irb_.CreateICmpULT(frame_address, stack_end);

  llvm::BasicBlock* block_exception =
      llvm::BasicBlock::Create(context_, "stack_overflow", func);

  llvm::BasicBlock* block_continue =
      llvm::BasicBlock::Create(context_, "stack_overflow_cont", func);

  irb_.CreateCondBr(is_stack_overflow, block_exception, block_continue, kUnlikely);

  // If stack overflow, throw exception.
  irb_.SetInsertPoint(block_exception);
  irb_.CreateCall(irb_.GetRuntime(runtime_support::ThrowStackOverflowException));

  // Unwind.
  llvm::Type* ret_type = func->getReturnType();
  if (ret_type->isVoidTy()) {
    irb_.CreateRetVoid();
  } else {
    // The return value is ignored when there's an exception. MethodCompiler
    // returns zero value under the the corresponding return type  in this case.
    // GBCExpander returns LLVM undef value here for brevity
    irb_.CreateRet(llvm::UndefValue::get(ret_type));
  }

  irb_.SetInsertPoint(block_continue);
}

llvm::Value* GBCExpanderPass::EmitLoadDexCacheAddr(art::MemberOffset offset) {
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  return irb_.LoadFromObjectOffset(method_object_addr,
                                   offset.Int32Value(),
                                   irb_.getJObjectTy(),
                                   kTBAAConstJObject);
}

llvm::Value*
GBCExpanderPass::EmitLoadDexCacheStaticStorageFieldAddr(uint32_t type_idx) {
  llvm::Value* static_storage_dex_cache_addr =
    EmitLoadDexCacheAddr(art::AbstractMethod::DexCacheInitializedStaticStorageOffset());

  llvm::Value* type_idx_value = irb_.getPtrEquivInt(type_idx);

  return EmitArrayGEP(static_storage_dex_cache_addr, type_idx_value, kObject);
}

llvm::Value*
GBCExpanderPass::EmitLoadDexCacheResolvedTypeFieldAddr(uint32_t type_idx) {
  llvm::Value* resolved_type_dex_cache_addr =
    EmitLoadDexCacheAddr(art::AbstractMethod::DexCacheResolvedTypesOffset());

  llvm::Value* type_idx_value = irb_.getPtrEquivInt(type_idx);

  return EmitArrayGEP(resolved_type_dex_cache_addr, type_idx_value, kObject);
}

llvm::Value* GBCExpanderPass::
EmitLoadDexCacheResolvedMethodFieldAddr(uint32_t method_idx) {
  llvm::Value* resolved_method_dex_cache_addr =
    EmitLoadDexCacheAddr(art::AbstractMethod::DexCacheResolvedMethodsOffset());

  llvm::Value* method_idx_value = irb_.getPtrEquivInt(method_idx);

  return EmitArrayGEP(resolved_method_dex_cache_addr, method_idx_value, kObject);
}

llvm::Value* GBCExpanderPass::
EmitLoadDexCacheStringFieldAddr(uint32_t string_idx) {
  llvm::Value* string_dex_cache_addr =
    EmitLoadDexCacheAddr(art::AbstractMethod::DexCacheStringsOffset());

  llvm::Value* string_idx_value = irb_.getPtrEquivInt(string_idx);

  return EmitArrayGEP(string_dex_cache_addr, string_idx_value, kObject);
}

llvm::Value* GBCExpanderPass::EmitLoadMethodObjectAddr() {
  llvm::Function* parent_func = irb_.GetInsertBlock()->getParent();
  return parent_func->arg_begin();
}

llvm::Value* GBCExpanderPass::EmitLoadArrayLength(llvm::Value* array) {
  // Load array length
  return irb_.LoadFromObjectOffset(array,
                                   art::Array::LengthOffset().Int32Value(),
                                   irb_.getJIntTy(),
                                   kTBAAConstJObject);

}

llvm::Value*
GBCExpanderPass::EmitLoadSDCalleeMethodObjectAddr(uint32_t callee_method_idx) {
  llvm::Value* callee_method_object_field_addr =
    EmitLoadDexCacheResolvedMethodFieldAddr(callee_method_idx);

  return irb_.CreateLoad(callee_method_object_field_addr, kTBAAJRuntime);
}

llvm::Value* GBCExpanderPass::
EmitLoadVirtualCalleeMethodObjectAddr(int vtable_idx, llvm::Value* this_addr) {
  // Load class object of *this* pointer
  llvm::Value* class_object_addr =
    irb_.LoadFromObjectOffset(this_addr,
                              art::Object::ClassOffset().Int32Value(),
                              irb_.getJObjectTy(),
                              kTBAAConstJObject);

  // Load vtable address
  llvm::Value* vtable_addr =
    irb_.LoadFromObjectOffset(class_object_addr,
                              art::Class::VTableOffset().Int32Value(),
                              irb_.getJObjectTy(),
                              kTBAAConstJObject);

  // Load callee method object
  llvm::Value* vtable_idx_value =
    irb_.getPtrEquivInt(static_cast<uint64_t>(vtable_idx));

  llvm::Value* method_field_addr =
    EmitArrayGEP(vtable_addr, vtable_idx_value, kObject);

  return irb_.CreateLoad(method_field_addr, kTBAAConstJObject);
}

// Emit Array GetElementPtr
llvm::Value* GBCExpanderPass::EmitArrayGEP(llvm::Value* array_addr,
                                           llvm::Value* index_value,
                                           JType elem_jty) {

  int data_offset;
  if (elem_jty == kLong || elem_jty == kDouble ||
      (elem_jty == kObject && sizeof(uint64_t) == sizeof(art::Object*))) {
    data_offset = art::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = art::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  llvm::Constant* data_offset_value =
    irb_.getPtrEquivInt(data_offset);

  llvm::Type* elem_type = irb_.getJType(elem_jty, kArray);

  llvm::Value* array_data_addr =
    irb_.CreatePtrDisp(array_addr, data_offset_value,
                       elem_type->getPointerTo());

  return irb_.CreateGEP(array_data_addr, index_value);
}

void GBCExpanderPass::Expand_TestSuspend(llvm::CallInst& call_inst) {
  irb_.Runtime().EmitTestSuspend();
  return;
}

void GBCExpanderPass::Expand_MarkGCCard(llvm::CallInst& call_inst) {
  irb_.Runtime().EmitMarkGCCard(call_inst.getArgOperand(0), call_inst.getArgOperand(1));
  return;
}

llvm::Value* GBCExpanderPass::Expand_GetException() {
  // Get thread-local exception field address
  llvm::Value* exception_object_addr =
    irb_.Runtime().EmitLoadFromThreadOffset(art::Thread::ExceptionOffset().Int32Value(),
                                            irb_.getJObjectTy(),
                                            kTBAAJRuntime);

  // Set thread-local exception field address to NULL
  irb_.Runtime().EmitStoreToThreadOffset(art::Thread::ExceptionOffset().Int32Value(),
                                         irb_.getJNull(),
                                         kTBAAJRuntime);

  return exception_object_addr;
}

llvm::Value*
GBCExpanderPass::Expand_LoadStringFromDexCache(llvm::Value* string_idx_value) {
  uint32_t string_idx =
    llvm::cast<llvm::ConstantInt>(string_idx_value)->getZExtValue();

  llvm::Value* string_field_addr = EmitLoadDexCacheStringFieldAddr(string_idx);

  return irb_.CreateLoad(string_field_addr, kTBAAJRuntime);
}

llvm::Value*
GBCExpanderPass::Expand_LoadTypeFromDexCache(llvm::Value* type_idx_value) {
  uint32_t type_idx =
    llvm::cast<llvm::ConstantInt>(type_idx_value)->getZExtValue();

  llvm::Value* type_field_addr =
    EmitLoadDexCacheResolvedTypeFieldAddr(type_idx);

  return irb_.CreateLoad(type_field_addr, kTBAAJRuntime);
}

void GBCExpanderPass::Expand_LockObject(llvm::Value* obj) {
  rtb_.EmitLockObject(obj);
  return;
}

void GBCExpanderPass::Expand_UnlockObject(llvm::Value* obj) {
  rtb_.EmitUnlockObject(obj);
  return;
}

llvm::Value* GBCExpanderPass::Expand_ArrayGet(llvm::Value* array_addr,
                                              llvm::Value* index_value,
                                              JType elem_jty) {
  llvm::Value* array_elem_addr =
    EmitArrayGEP(array_addr, index_value, elem_jty);

  return irb_.CreateLoad(array_elem_addr, kTBAAHeapArray, elem_jty);
}

void GBCExpanderPass::Expand_ArrayPut(llvm::Value* new_value,
                                      llvm::Value* array_addr,
                                      llvm::Value* index_value,
                                      JType elem_jty) {
  llvm::Value* array_elem_addr =
    EmitArrayGEP(array_addr, index_value, elem_jty);

  irb_.CreateStore(new_value, array_elem_addr, kTBAAHeapArray, elem_jty);

  return;
}

void GBCExpanderPass::Expand_FilledNewArray(llvm::CallInst& call_inst) {
  // Most of the codes refer to MethodCompiler::EmitInsn_FilledNewArray
  llvm::Value* array = call_inst.getArgOperand(0);

  uint32_t element_jty =
    llvm::cast<llvm::ConstantInt>(call_inst.getArgOperand(1))->getZExtValue();

  DCHECK(call_inst.getNumArgOperands() > 2);
  unsigned num_elements = (call_inst.getNumArgOperands() - 2);

  bool is_elem_int_ty = (static_cast<JType>(element_jty) == kInt);

  uint32_t alignment;
  llvm::Constant* elem_size;
  llvm::PointerType* field_type;

  // NOTE: Currently filled-new-array only supports 'L', '[', and 'I'
  // as the element, thus we are only checking 2 cases: primitive int and
  // non-primitive type.
  if (is_elem_int_ty) {
    alignment = sizeof(int32_t);
    elem_size = irb_.getPtrEquivInt(sizeof(int32_t));
    field_type = irb_.getJIntTy()->getPointerTo();
  } else {
    alignment = irb_.getSizeOfPtrEquivInt();
    elem_size = irb_.getSizeOfPtrEquivIntValue();
    field_type = irb_.getJObjectTy()->getPointerTo();
  }

  llvm::Value* data_field_offset =
    irb_.getPtrEquivInt(art::Array::DataOffset(alignment).Int32Value());

  llvm::Value* data_field_addr =
    irb_.CreatePtrDisp(array, data_field_offset, field_type);

  for (unsigned i = 0; i < num_elements; ++i) {
    // Values to fill the array begin at the 3rd argument
    llvm::Value* reg_value = call_inst.getArgOperand(2 + i);

    irb_.CreateStore(reg_value, data_field_addr, kTBAAHeapArray);

    data_field_addr =
      irb_.CreatePtrDisp(data_field_addr, elem_size, field_type);
  }

  return;
}

llvm::Value* GBCExpanderPass::Expand_IGetFast(llvm::Value* field_offset_value,
                                              llvm::Value* /*is_volatile_value*/,
                                              llvm::Value* object_addr,
                                              JType field_jty) {
  int field_offset =
    llvm::cast<llvm::ConstantInt>(field_offset_value)->getSExtValue();

  DCHECK_GE(field_offset, 0);

  llvm::PointerType* field_type =
    irb_.getJType(field_jty, kField)->getPointerTo();

  field_offset_value = irb_.getPtrEquivInt(field_offset);

  llvm::Value* field_addr =
    irb_.CreatePtrDisp(object_addr, field_offset_value, field_type);

  // TODO: Check is_volatile.  We need to generate atomic load instruction
  // when is_volatile is true.
  return irb_.CreateLoad(field_addr, kTBAAHeapInstance, field_jty);
}

void GBCExpanderPass::Expand_IPutFast(llvm::Value* field_offset_value,
                                      llvm::Value* /* is_volatile_value */,
                                      llvm::Value* object_addr,
                                      llvm::Value* new_value,
                                      JType field_jty) {
  int field_offset =
    llvm::cast<llvm::ConstantInt>(field_offset_value)->getSExtValue();

  DCHECK_GE(field_offset, 0);

  llvm::PointerType* field_type =
    irb_.getJType(field_jty, kField)->getPointerTo();

  field_offset_value = irb_.getPtrEquivInt(field_offset);

  llvm::Value* field_addr =
    irb_.CreatePtrDisp(object_addr, field_offset_value, field_type);

  // TODO: Check is_volatile.  We need to generate atomic store instruction
  // when is_volatile is true.
  irb_.CreateStore(new_value, field_addr, kTBAAHeapInstance, field_jty);

  return;
}

llvm::Value* GBCExpanderPass::Expand_SGetFast(llvm::Value* static_storage_addr,
                                              llvm::Value* field_offset_value,
                                              llvm::Value* /*is_volatile_value*/,
                                              JType field_jty) {
  int field_offset =
    llvm::cast<llvm::ConstantInt>(field_offset_value)->getSExtValue();

  DCHECK_GE(field_offset, 0);

  llvm::Value* static_field_offset_value = irb_.getPtrEquivInt(field_offset);

  llvm::Value* static_field_addr =
    irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                       irb_.getJType(field_jty, kField)->getPointerTo());

  // TODO: Check is_volatile.  We need to generate atomic store instruction
  // when is_volatile is true.
  return irb_.CreateLoad(static_field_addr, kTBAAHeapStatic, field_jty);
}

void GBCExpanderPass::Expand_SPutFast(llvm::Value* static_storage_addr,
                                      llvm::Value* field_offset_value,
                                      llvm::Value* /* is_volatile_value */,
                                      llvm::Value* new_value,
                                      JType field_jty) {
  int field_offset =
    llvm::cast<llvm::ConstantInt>(field_offset_value)->getSExtValue();

  DCHECK_GE(field_offset, 0);

  llvm::Value* static_field_offset_value = irb_.getPtrEquivInt(field_offset);

  llvm::Value* static_field_addr =
    irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                       irb_.getJType(field_jty, kField)->getPointerTo());

  // TODO: Check is_volatile.  We need to generate atomic store instruction
  // when is_volatile is true.
  irb_.CreateStore(new_value, static_field_addr, kTBAAHeapStatic, field_jty);

  return;
}

llvm::Value*
GBCExpanderPass::Expand_LoadDeclaringClassSSB(llvm::Value* method_object_addr) {
  return irb_.LoadFromObjectOffset(method_object_addr,
                                   art::AbstractMethod::DeclaringClassOffset().Int32Value(),
                                   irb_.getJObjectTy(),
                                   kTBAAConstJObject);
}

llvm::Value*
GBCExpanderPass::Expand_LoadClassSSBFromDexCache(llvm::Value* type_idx_value) {
  uint32_t type_idx =
    llvm::cast<llvm::ConstantInt>(type_idx_value)->getZExtValue();

  llvm::Value* storage_field_addr =
    EmitLoadDexCacheStaticStorageFieldAddr(type_idx);

  return irb_.CreateLoad(storage_field_addr, kTBAAJRuntime);
}

llvm::Value*
GBCExpanderPass::Expand_GetSDCalleeMethodObjAddrFast(llvm::Value* callee_method_idx_value) {
  uint32_t callee_method_idx =
    llvm::cast<llvm::ConstantInt>(callee_method_idx_value)->getZExtValue();

  return EmitLoadSDCalleeMethodObjectAddr(callee_method_idx);
}

llvm::Value* GBCExpanderPass::Expand_GetVirtualCalleeMethodObjAddrFast(
    llvm::Value* vtable_idx_value,
    llvm::Value* this_addr) {
  int vtable_idx =
    llvm::cast<llvm::ConstantInt>(vtable_idx_value)->getSExtValue();

  return EmitLoadVirtualCalleeMethodObjectAddr(vtable_idx, this_addr);
}

llvm::Value* GBCExpanderPass::Expand_Invoke(llvm::CallInst& call_inst) {
  // Most of the codes refer to MethodCompiler::EmitInsn_Invoke
  llvm::Value* callee_method_object_addr = call_inst.getArgOperand(0);
  unsigned num_args = call_inst.getNumArgOperands();
  llvm::Type* ret_type = call_inst.getType();

  // Determine the function type of the callee method
  std::vector<llvm::Type*> args_type;
  std::vector<llvm::Value*> args;
  for (unsigned i = 0; i < num_args; i++) {
    args.push_back(call_inst.getArgOperand(i));
    args_type.push_back(args[i]->getType());
  }

  llvm::FunctionType* callee_method_type =
    llvm::FunctionType::get(ret_type, args_type, false);

  llvm::Value* code_addr =
    irb_.LoadFromObjectOffset(callee_method_object_addr,
                              art::AbstractMethod::GetCodeOffset().Int32Value(),
                              callee_method_type->getPointerTo(),
                              kTBAAJRuntime);

  // Invoke callee
  llvm::Value* retval = irb_.CreateCall(code_addr, args);

  return retval;
}

llvm::Value* GBCExpanderPass::Expand_DivRem(llvm::CallInst& call_inst,
                                            bool is_div, JType op_jty) {
  llvm::Value* dividend = call_inst.getArgOperand(0);
  llvm::Value* divisor = call_inst.getArgOperand(1);
#if defined(ART_USE_QUICK_COMPILER)
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  EmitGuard_DivZeroException(dex_pc, divisor, op_jty);
#endif
  // Most of the codes refer to MethodCompiler::EmitIntDivRemResultComputation

  // Check the special case: MININT / -1 = MININT
  // That case will cause overflow, which is undefined behavior in llvm.
  // So we check the divisor is -1 or not, if the divisor is -1, we do
  // the special path to avoid undefined behavior.
  llvm::Type* op_type = irb_.getJType(op_jty, kAccurate);
  llvm::Value* zero = irb_.getJZero(op_jty);
  llvm::Value* neg_one = llvm::ConstantInt::getSigned(op_type, -1);

  llvm::Function* parent = irb_.GetInsertBlock()->getParent();
  llvm::BasicBlock* eq_neg_one = llvm::BasicBlock::Create(context_, "", parent);
  llvm::BasicBlock* ne_neg_one = llvm::BasicBlock::Create(context_, "", parent);
  llvm::BasicBlock* neg_one_cont =
    llvm::BasicBlock::Create(context_, "", parent);

  llvm::Value* is_equal_neg_one = irb_.CreateICmpEQ(divisor, neg_one);
  irb_.CreateCondBr(is_equal_neg_one, eq_neg_one, ne_neg_one, kUnlikely);

  // If divisor == -1
  irb_.SetInsertPoint(eq_neg_one);
  llvm::Value* eq_result;
  if (is_div) {
    // We can just change from "dividend div -1" to "neg dividend". The sub
    // don't care the sign/unsigned because of two's complement representation.
    // And the behavior is what we want:
    //  -(2^n)        (2^n)-1
    //  MININT  < k <= MAXINT    ->     mul k -1  =  -k
    //  MININT == k              ->     mul k -1  =   k
    //
    // LLVM use sub to represent 'neg'
    eq_result = irb_.CreateSub(zero, dividend);
  } else {
    // Everything modulo -1 will be 0.
    eq_result = zero;
  }
  irb_.CreateBr(neg_one_cont);

  // If divisor != -1, just do the division.
  irb_.SetInsertPoint(ne_neg_one);
  llvm::Value* ne_result;
  if (is_div) {
    ne_result = irb_.CreateSDiv(dividend, divisor);
  } else {
    ne_result = irb_.CreateSRem(dividend, divisor);
  }
  irb_.CreateBr(neg_one_cont);

  irb_.SetInsertPoint(neg_one_cont);
  llvm::PHINode* result = irb_.CreatePHI(op_type, 2);
  result->addIncoming(eq_result, eq_neg_one);
  result->addIncoming(ne_result, ne_neg_one);

  return result;
}

void GBCExpanderPass::Expand_AllocaShadowFrame(llvm::Value* num_entry_value) {
  // Most of the codes refer to MethodCompiler::EmitPrologueAllocShadowFrame and
  // MethodCompiler::EmitPushShadowFrame
  shadow_frame_size_ =
    llvm::cast<llvm::ConstantInt>(num_entry_value)->getZExtValue();

  llvm::StructType* shadow_frame_type =
    irb_.getShadowFrameTy(shadow_frame_size_);

  shadow_frame_ = irb_.CreateAlloca(shadow_frame_type);

  // Alloca a pointer to old shadow frame
  old_shadow_frame_ =
    irb_.CreateAlloca(shadow_frame_type->getElementType(0)->getPointerTo());

  // Zero-initialization of the shadow frame table
  llvm::Value* shadow_frame_table =
    irb_.CreateConstGEP2_32(shadow_frame_, 0, 1);
  llvm::Type* table_type = shadow_frame_type->getElementType(1);

  llvm::ConstantAggregateZero* zero_initializer =
    llvm::ConstantAggregateZero::get(table_type);

  irb_.CreateStore(zero_initializer, shadow_frame_table, kTBAAShadowFrame);

  // Push the shadow frame
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  // Push the shadow frame
  llvm::Value* shadow_frame_upcast =
    irb_.CreateConstGEP2_32(shadow_frame_, 0, 0);

  llvm::Value* result = rtb_.EmitPushShadowFrame(shadow_frame_upcast,
                                                 method_object_addr,
                                                 shadow_frame_size_);

  irb_.CreateStore(result, old_shadow_frame_, kTBAARegister);

  return;
}

void GBCExpanderPass::Expand_SetShadowFrameEntry(llvm::Value* obj,
                                                 llvm::Value* entry_idx) {
  DCHECK(shadow_frame_ != NULL);

  llvm::Value* gep_index[] = {
    irb_.getInt32(0), // No pointer displacement
    irb_.getInt32(1), // SIRT
    entry_idx // Pointer field
  };

  llvm::Value* entry_addr = irb_.CreateGEP(shadow_frame_, gep_index);
#if defined(ART_USE_QUICK_COMPILER)
  if (obj->getType() != irb_.getJObjectTy()) {
    obj = irb_.getJNull();
  }
#endif
  irb_.CreateStore(obj, entry_addr, kTBAAShadowFrame);
  return;
}

void GBCExpanderPass::Expand_PopShadowFrame() {
#if defined(ART_USE_QUICK_COMPILER)
  if (old_shadow_frame_ == NULL) {
    return;
  }
#endif
  rtb_.EmitPopShadowFrame(irb_.CreateLoad(old_shadow_frame_, kTBAARegister));
  return;
}

void GBCExpanderPass::Expand_UpdateDexPC(llvm::Value* dex_pc_value) {
  irb_.StoreToObjectOffset(shadow_frame_,
                           art::ShadowFrame::DexPCOffset(),
                           dex_pc_value,
                           kTBAAShadowFrame);
  return;
}

void GBCExpanderPass::InsertStackOverflowCheck(llvm::Function& func) {
  // DexLang generates all alloca instruction in the first basic block of the
  // FUNC and also there's no any alloca instructions after the first non-alloca
  // instruction

  llvm::BasicBlock* first_basic_block = &func.front();

  // Look for first non-alloca instruction
  llvm::BasicBlock::iterator first_non_alloca = first_basic_block->begin();
  while (llvm::isa<llvm::AllocaInst>(first_non_alloca)) {
    ++first_non_alloca;
  }

  irb_.SetInsertPoint(first_non_alloca);

  // Insert stack overflow check codes before first_non_alloca (i.e., after all
  // alloca instructions)
  EmitStackOverflowCheck(&*first_non_alloca);

#if defined(ART_USE_QUICK_COMPILER)
  irb_.Runtime().EmitTestSuspend();
#endif

  llvm::BasicBlock* next_basic_block = irb_.GetInsertBlock();
  if (next_basic_block != first_basic_block) {
    // Splice the rest of the instruction to the continuing basic block
    next_basic_block->getInstList().splice(
        irb_.GetInsertPoint(), first_basic_block->getInstList(),
        first_non_alloca, first_basic_block->end());

    // Rewrite the basic block
    RewriteBasicBlock(next_basic_block);

    // Update the phi-instructions in the successor basic block
    UpdatePhiInstruction(first_basic_block, irb_.GetInsertBlock());
  }

  // We have changed the basic block
  changed_ = true;
}

// ==== High-level intrinsic expander ==========================================

llvm::Value* GBCExpanderPass::Expand_FPCompare(llvm::Value* src1_value,
                                               llvm::Value* src2_value,
                                               bool gt_bias) {
  llvm::Value* cmp_eq = irb_.CreateFCmpOEQ(src1_value, src2_value);
  llvm::Value* cmp_lt;

  if (gt_bias) {
    cmp_lt = irb_.CreateFCmpOLT(src1_value, src2_value);
  } else {
    cmp_lt = irb_.CreateFCmpULT(src1_value, src2_value);
  }

  return EmitCompareResultSelection(cmp_eq, cmp_lt);
}

llvm::Value* GBCExpanderPass::Expand_LongCompare(llvm::Value* src1_value, llvm::Value* src2_value) {
  llvm::Value* cmp_eq = irb_.CreateICmpEQ(src1_value, src2_value);
  llvm::Value* cmp_lt = irb_.CreateICmpSLT(src1_value, src2_value);

  return EmitCompareResultSelection(cmp_eq, cmp_lt);
}

llvm::Value* GBCExpanderPass::EmitCompareResultSelection(llvm::Value* cmp_eq,
                                                         llvm::Value* cmp_lt) {

  llvm::Constant* zero = irb_.getJInt(0);
  llvm::Constant* pos1 = irb_.getJInt(1);
  llvm::Constant* neg1 = irb_.getJInt(-1);

  llvm::Value* result_lt = irb_.CreateSelect(cmp_lt, neg1, pos1);
  llvm::Value* result_eq = irb_.CreateSelect(cmp_eq, zero, result_lt);

  return result_eq;
}

llvm::Value* GBCExpanderPass::Expand_IntegerShift(llvm::Value* src1_value,
                                                  llvm::Value* src2_value,
                                                  IntegerShiftKind kind,
                                                  JType op_jty) {
  DCHECK(op_jty == kInt || op_jty == kLong);

  // Mask and zero-extend RHS properly
  if (op_jty == kInt) {
    src2_value = irb_.CreateAnd(src2_value, 0x1f);
  } else {
    llvm::Value* masked_src2_value = irb_.CreateAnd(src2_value, 0x3f);
    src2_value = irb_.CreateZExt(masked_src2_value, irb_.getJLongTy());
  }

  // Create integer shift llvm instruction
  switch (kind) {
  case kIntegerSHL:
    return irb_.CreateShl(src1_value, src2_value);

  case kIntegerSHR:
    return irb_.CreateAShr(src1_value, src2_value);

  case kIntegerUSHR:
    return irb_.CreateLShr(src1_value, src2_value);

  default:
    LOG(FATAL) << "Unknown integer shift kind: " << kind;
    return NULL;
  }
}

llvm::Value* GBCExpanderPass::Expand_HLArrayGet(llvm::CallInst& call_inst,
                                                JType elem_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* array_addr = call_inst.getArgOperand(1);
  llvm::Value* index_value = call_inst.getArgOperand(2);
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, array_addr);
  }
  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    EmitGuard_ArrayIndexOutOfBoundsException(dex_pc, array_addr, index_value);
  }

  llvm::Value* array_elem_addr = EmitArrayGEP(array_addr, index_value, elem_jty);

  llvm::Value* array_elem_value = irb_.CreateLoad(array_elem_addr, kTBAAHeapArray, elem_jty);

  switch (elem_jty) {
  case kVoid:
    break;

  case kBoolean:
  case kChar:
    array_elem_value = irb_.CreateZExt(array_elem_value, irb_.getJType(elem_jty, kReg));
    break;

  case kByte:
  case kShort:
    array_elem_value = irb_.CreateSExt(array_elem_value, irb_.getJType(elem_jty, kReg));
    break;

  case kInt:
  case kLong:
  case kFloat:
  case kDouble:
  case kObject:
    break;

  default:
    LOG(FATAL) << "Unknown java type: " << elem_jty;
  }

  return array_elem_value;
}


void GBCExpanderPass::Expand_HLArrayPut(llvm::CallInst& call_inst,
                                        JType elem_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* new_value = call_inst.getArgOperand(1);
  llvm::Value* array_addr = call_inst.getArgOperand(2);
  llvm::Value* index_value = call_inst.getArgOperand(3);
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, array_addr);
  }
  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    EmitGuard_ArrayIndexOutOfBoundsException(dex_pc, array_addr, index_value);
  }

  switch (elem_jty) {
  case kVoid:
    break;

  case kBoolean:
  case kChar:
  case kByte:
  case kShort:
    new_value = irb_.CreateTrunc(new_value, irb_.getJType(elem_jty, kArray));
    break;

  case kInt:
  case kLong:
  case kFloat:
  case kDouble:
  case kObject:
    break;

  default:
    LOG(FATAL) << "Unknown java type: " << elem_jty;
  }

  llvm::Value* array_elem_addr = EmitArrayGEP(array_addr, index_value, elem_jty);

  if (elem_jty == kObject) { // If put an object, check the type, and mark GC card table.
    llvm::Function* runtime_func = irb_.GetRuntime(runtime_support::CheckPutArrayElement);

    irb_.CreateCall2(runtime_func, new_value, array_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

    EmitMarkGCCard(new_value, array_addr);
  }

  irb_.CreateStore(new_value, array_elem_addr, kTBAAHeapArray, elem_jty);

  return;
}

llvm::Value* GBCExpanderPass::Expand_HLIGet(llvm::CallInst& call_inst,
                                            JType field_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* object_addr = call_inst.getArgOperand(1);
  uint32_t field_idx = LV2UInt(call_inst.getArgOperand(2));
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, object_addr);
  }

  llvm::Value* field_value;

  int field_offset;
  bool is_volatile;
  bool is_fast_path = compiler_->ComputeInstanceFieldInfo(
    field_idx, oat_compilation_unit_, field_offset, is_volatile, false);

  if (!is_fast_path) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(runtime_support::GetObjectInstance);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(runtime_support::Get64Instance);
    } else {
      runtime_func = irb_.GetRuntime(runtime_support::Get32Instance);
    }

    llvm::ConstantInt* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateDexPC(dex_pc);

    field_value = irb_.CreateCall3(runtime_func, field_idx_value,
                                   method_object_addr, object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    DCHECK_GE(field_offset, 0);

    llvm::PointerType* field_type =
      irb_.getJType(field_jty, kField)->getPointerTo();

    llvm::ConstantInt* field_offset_value = irb_.getPtrEquivInt(field_offset);

    llvm::Value* field_addr =
      irb_.CreatePtrDisp(object_addr, field_offset_value, field_type);

    // TODO: Check is_volatile.  We need to generate atomic load instruction
    // when is_volatile is true.
    field_value = irb_.CreateLoad(field_addr, kTBAAHeapInstance, field_jty);
  }

  if (field_jty == kFloat || field_jty == kDouble) {
    field_value = irb_.CreateBitCast(field_value, irb_.getJType(field_jty, kAccurate));
  }

  return field_value;
}

void GBCExpanderPass::Expand_HLIPut(llvm::CallInst& call_inst,
                                    JType field_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* new_value = call_inst.getArgOperand(1);
  llvm::Value* object_addr = call_inst.getArgOperand(2);
  uint32_t field_idx = LV2UInt(call_inst.getArgOperand(3));
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (field_jty == kFloat || field_jty == kDouble) {
    new_value = irb_.CreateBitCast(new_value, irb_.getJType(field_jty, kField));
  }

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, object_addr);
  }

  int field_offset;
  bool is_volatile;
  bool is_fast_path = compiler_->ComputeInstanceFieldInfo(
    field_idx, oat_compilation_unit_, field_offset, is_volatile, true);

  if (!is_fast_path) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(runtime_support::SetObjectInstance);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(runtime_support::Set64Instance);
    } else {
      runtime_func = irb_.GetRuntime(runtime_support::Set32Instance);
    }

    llvm::Value* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateDexPC(dex_pc);

    irb_.CreateCall4(runtime_func, field_idx_value,
                     method_object_addr, object_addr, new_value);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    DCHECK_GE(field_offset, 0);

    llvm::PointerType* field_type =
      irb_.getJType(field_jty, kField)->getPointerTo();

    llvm::Value* field_offset_value = irb_.getPtrEquivInt(field_offset);

    llvm::Value* field_addr =
      irb_.CreatePtrDisp(object_addr, field_offset_value, field_type);

    // TODO: Check is_volatile.  We need to generate atomic store instruction
    // when is_volatile is true.
    irb_.CreateStore(new_value, field_addr, kTBAAHeapInstance, field_jty);

    if (field_jty == kObject) { // If put an object, mark the GC card table.
      EmitMarkGCCard(new_value, object_addr);
    }
  }

  return;
}

llvm::Value* GBCExpanderPass::EmitLoadConstantClass(uint32_t dex_pc,
                                                    uint32_t type_idx) {
  if (!compiler_->CanAccessTypeWithoutChecks(method_idx_, *dex_file_, type_idx)) {
    llvm::Value* type_idx_value = irb_.getInt32(type_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

    llvm::Function* runtime_func =
      irb_.GetRuntime(runtime_support::InitializeTypeAndVerifyAccess);

    EmitUpdateDexPC(dex_pc);

    llvm::Value* type_object_addr =
      irb_.CreateCall3(runtime_func, type_idx_value, method_object_addr, thread_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

    return type_object_addr;

  } else {
    // Try to load the class (type) object from the test cache.
    llvm::Value* type_field_addr =
      EmitLoadDexCacheResolvedTypeFieldAddr(type_idx);

    llvm::Value* type_object_addr = irb_.CreateLoad(type_field_addr, kTBAAJRuntime);

    if (compiler_->CanAssumeTypeIsPresentInDexCache(*dex_file_, type_idx)) {
      return type_object_addr;
    }

    llvm::BasicBlock* block_original = irb_.GetInsertBlock();

    // Test whether class (type) object is in the dex cache or not
    llvm::Value* equal_null =
      irb_.CreateICmpEQ(type_object_addr, irb_.getJNull());

    llvm::BasicBlock* block_cont =
      CreateBasicBlockWithDexPC(dex_pc, "cont");

    llvm::BasicBlock* block_load_class =
      CreateBasicBlockWithDexPC(dex_pc, "load_class");

    irb_.CreateCondBr(equal_null, block_load_class, block_cont, kUnlikely);

    // Failback routine to load the class object
    irb_.SetInsertPoint(block_load_class);

    llvm::Function* runtime_func = irb_.GetRuntime(runtime_support::InitializeType);

    llvm::Constant* type_idx_value = irb_.getInt32(type_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

    EmitUpdateDexPC(dex_pc);

    llvm::Value* loaded_type_object_addr =
      irb_.CreateCall3(runtime_func, type_idx_value, method_object_addr, thread_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

    llvm::BasicBlock* block_after_load_class = irb_.GetInsertBlock();

    irb_.CreateBr(block_cont);

    // Now the class object must be loaded
    irb_.SetInsertPoint(block_cont);

    llvm::PHINode* phi = irb_.CreatePHI(irb_.getJObjectTy(), 2);

    phi->addIncoming(type_object_addr, block_original);
    phi->addIncoming(loaded_type_object_addr, block_after_load_class);

    return phi;
  }
}

llvm::Value* GBCExpanderPass::EmitLoadStaticStorage(uint32_t dex_pc,
                                                    uint32_t type_idx) {
  llvm::BasicBlock* block_load_static =
    CreateBasicBlockWithDexPC(dex_pc, "load_static");

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  // Load static storage from dex cache
  llvm::Value* storage_field_addr =
    EmitLoadDexCacheStaticStorageFieldAddr(type_idx);

  llvm::Value* storage_object_addr = irb_.CreateLoad(storage_field_addr, kTBAAJRuntime);

  llvm::BasicBlock* block_original = irb_.GetInsertBlock();

  // Test: Is the static storage of this class initialized?
  llvm::Value* equal_null =
    irb_.CreateICmpEQ(storage_object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null, block_load_static, block_cont, kUnlikely);

  // Failback routine to load the class object
  irb_.SetInsertPoint(block_load_static);

  llvm::Function* runtime_func = irb_.GetRuntime(runtime_support::InitializeStaticStorage);

  llvm::Constant* type_idx_value = irb_.getInt32(type_idx);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

  EmitUpdateDexPC(dex_pc);

  llvm::Value* loaded_storage_object_addr =
    irb_.CreateCall3(runtime_func, type_idx_value, method_object_addr, thread_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  llvm::BasicBlock* block_after_load_static = irb_.GetInsertBlock();

  irb_.CreateBr(block_cont);

  // Now the class object must be loaded
  irb_.SetInsertPoint(block_cont);

  llvm::PHINode* phi = irb_.CreatePHI(irb_.getJObjectTy(), 2);

  phi->addIncoming(storage_object_addr, block_original);
  phi->addIncoming(loaded_storage_object_addr, block_after_load_static);

  return phi;
}

llvm::Value* GBCExpanderPass::Expand_HLSget(llvm::CallInst& call_inst,
                                            JType field_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t field_idx = LV2UInt(call_inst.getArgOperand(0));

  int field_offset;
  int ssb_index;
  bool is_referrers_class;
  bool is_volatile;

  bool is_fast_path = compiler_->ComputeStaticFieldInfo(
    field_idx, oat_compilation_unit_, field_offset, ssb_index,
    is_referrers_class, is_volatile, false);

  llvm::Value* static_field_value;

  if (!is_fast_path) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(runtime_support::GetObjectStatic);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(runtime_support::Get64Static);
    } else {
      runtime_func = irb_.GetRuntime(runtime_support::Get32Static);
    }

    llvm::Constant* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateDexPC(dex_pc);

    static_field_value =
      irb_.CreateCall2(runtime_func, field_idx_value, method_object_addr);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    DCHECK_GE(field_offset, 0);

    llvm::Value* static_storage_addr = NULL;

    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

      static_storage_addr =
        irb_.LoadFromObjectOffset(method_object_addr,
                                  art::AbstractMethod::DeclaringClassOffset().Int32Value(),
                                  irb_.getJObjectTy(),
                                  kTBAAConstJObject);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssb_index, 0);
      static_storage_addr = EmitLoadStaticStorage(dex_pc, ssb_index);
    }

    llvm::Value* static_field_offset_value = irb_.getPtrEquivInt(field_offset);

    llvm::Value* static_field_addr =
      irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                         irb_.getJType(field_jty, kField)->getPointerTo());

    // TODO: Check is_volatile.  We need to generate atomic load instruction
    // when is_volatile is true.
    static_field_value = irb_.CreateLoad(static_field_addr, kTBAAHeapStatic, field_jty);
  }

  if (field_jty == kFloat || field_jty == kDouble) {
    static_field_value =
        irb_.CreateBitCast(static_field_value, irb_.getJType(field_jty, kAccurate));
  }

  return static_field_value;
}

void GBCExpanderPass::Expand_HLSput(llvm::CallInst& call_inst,
                                    JType field_jty) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t field_idx = LV2UInt(call_inst.getArgOperand(0));
  llvm::Value* new_value = call_inst.getArgOperand(1);

  if (field_jty == kFloat || field_jty == kDouble) {
    new_value = irb_.CreateBitCast(new_value, irb_.getJType(field_jty, kField));
  }

  int field_offset;
  int ssb_index;
  bool is_referrers_class;
  bool is_volatile;

  bool is_fast_path = compiler_->ComputeStaticFieldInfo(
    field_idx, oat_compilation_unit_, field_offset, ssb_index,
    is_referrers_class, is_volatile, true);

  if (!is_fast_path) {
    llvm::Function* runtime_func;

    if (field_jty == kObject) {
      runtime_func = irb_.GetRuntime(runtime_support::SetObjectStatic);
    } else if (field_jty == kLong || field_jty == kDouble) {
      runtime_func = irb_.GetRuntime(runtime_support::Set64Static);
    } else {
      runtime_func = irb_.GetRuntime(runtime_support::Set32Static);
    }

    llvm::Constant* field_idx_value = irb_.getInt32(field_idx);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateDexPC(dex_pc);

    irb_.CreateCall3(runtime_func, field_idx_value,
                     method_object_addr, new_value);

    EmitGuard_ExceptionLandingPad(dex_pc);

  } else {
    DCHECK_GE(field_offset, 0);

    llvm::Value* static_storage_addr = NULL;

    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

      static_storage_addr =
        irb_.LoadFromObjectOffset(method_object_addr,
                                  art::AbstractMethod::DeclaringClassOffset().Int32Value(),
                                  irb_.getJObjectTy(),
                                  kTBAAConstJObject);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssb_index, 0);
      static_storage_addr = EmitLoadStaticStorage(dex_pc, ssb_index);
    }

    llvm::Value* static_field_offset_value = irb_.getPtrEquivInt(field_offset);

    llvm::Value* static_field_addr =
      irb_.CreatePtrDisp(static_storage_addr, static_field_offset_value,
                         irb_.getJType(field_jty, kField)->getPointerTo());

    // TODO: Check is_volatile.  We need to generate atomic store instruction
    // when is_volatile is true.
    irb_.CreateStore(new_value, static_field_addr, kTBAAHeapStatic, field_jty);

    if (field_jty == kObject) { // If put an object, mark the GC card table.
      EmitMarkGCCard(new_value, static_storage_addr);
    }
  }

  return;
}

llvm::Value* GBCExpanderPass::Expand_ConstString(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t string_idx = LV2UInt(call_inst.getArgOperand(0));

  llvm::Value* string_field_addr = EmitLoadDexCacheStringFieldAddr(string_idx);

  llvm::Value* string_addr = irb_.CreateLoad(string_field_addr, kTBAAJRuntime);

  if (!compiler_->CanAssumeStringIsPresentInDexCache(*dex_file_, string_idx)) {
    llvm::BasicBlock* block_str_exist =
      CreateBasicBlockWithDexPC(dex_pc, "str_exist");

    llvm::BasicBlock* block_str_resolve =
      CreateBasicBlockWithDexPC(dex_pc, "str_resolve");

    llvm::BasicBlock* block_cont =
      CreateBasicBlockWithDexPC(dex_pc, "str_cont");

    // Test: Is the string resolved and in the dex cache?
    llvm::Value* equal_null = irb_.CreateICmpEQ(string_addr, irb_.getJNull());

    irb_.CreateCondBr(equal_null, block_str_resolve, block_str_exist, kUnlikely);

    // String is resolved, go to next basic block.
    irb_.SetInsertPoint(block_str_exist);
    irb_.CreateBr(block_cont);

    // String is not resolved yet, resolve it now.
    irb_.SetInsertPoint(block_str_resolve);

    llvm::Function* runtime_func = irb_.GetRuntime(runtime_support::ResolveString);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Value* string_idx_value = irb_.getInt32(string_idx);

    EmitUpdateDexPC(dex_pc);

    llvm::Value* result = irb_.CreateCall2(runtime_func, method_object_addr,
                                           string_idx_value);

    EmitGuard_ExceptionLandingPad(dex_pc);

    irb_.CreateBr(block_cont);


    llvm::BasicBlock* block_pre_cont = irb_.GetInsertBlock();

    irb_.SetInsertPoint(block_cont);

    llvm::PHINode* phi = irb_.CreatePHI(irb_.getJObjectTy(), 2);

    phi->addIncoming(string_addr, block_str_exist);
    phi->addIncoming(result, block_pre_cont);

    string_addr = phi;
  }

  return string_addr;
}

llvm::Value* GBCExpanderPass::Expand_ConstClass(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(0));

  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, type_idx);

  return type_object_addr;
}

void GBCExpanderPass::Expand_MonitorEnter(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* object_addr = call_inst.getArgOperand(1);
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, object_addr);
  }

  irb_.Runtime().EmitLockObject(object_addr);

  return;
}

void GBCExpanderPass::Expand_MonitorExit(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  llvm::Value* object_addr = call_inst.getArgOperand(1);
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, object_addr);
  }

  EmitUpdateDexPC(dex_pc);

  irb_.Runtime().EmitUnlockObject(object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  return;
}

void GBCExpanderPass::Expand_HLCheckCast(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(0));
  llvm::Value* object_addr = call_inst.getArgOperand(1);

  llvm::BasicBlock* block_test_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_class");

  llvm::BasicBlock* block_test_sub_class =
    CreateBasicBlockWithDexPC(dex_pc, "test_sub_class");

  llvm::BasicBlock* block_cont =
    CreateBasicBlockWithDexPC(dex_pc, "checkcast_cont");

  // Test: Is the reference equal to null?  Act as no-op when it is null.
  llvm::Value* equal_null = irb_.CreateICmpEQ(object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null,
                    block_cont,
                    block_test_class);

  // Test: Is the object instantiated from the given class?
  irb_.SetInsertPoint(block_test_class);
  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, type_idx);
  DCHECK_EQ(art::Object::ClassOffset().Int32Value(), 0);

  llvm::PointerType* jobject_ptr_ty = irb_.getJObjectTy();

  llvm::Value* object_type_field_addr =
    irb_.CreateBitCast(object_addr, jobject_ptr_ty->getPointerTo());

  llvm::Value* object_type_object_addr =
    irb_.CreateLoad(object_type_field_addr, kTBAAConstJObject);

  llvm::Value* equal_class =
    irb_.CreateICmpEQ(type_object_addr, object_type_object_addr);

  irb_.CreateCondBr(equal_class,
                    block_cont,
                    block_test_sub_class);

  // Test: Is the object instantiated from the subclass of the given class?
  irb_.SetInsertPoint(block_test_sub_class);

  EmitUpdateDexPC(dex_pc);

  irb_.CreateCall2(irb_.GetRuntime(runtime_support::CheckCast),
                   type_object_addr, object_type_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  irb_.CreateBr(block_cont);

  irb_.SetInsertPoint(block_cont);

  return;
}

llvm::Value* GBCExpanderPass::Expand_InstanceOf(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(0));
  llvm::Value* object_addr = call_inst.getArgOperand(1);

  llvm::BasicBlock* block_nullp =
      CreateBasicBlockWithDexPC(dex_pc, "nullp");

  llvm::BasicBlock* block_test_class =
      CreateBasicBlockWithDexPC(dex_pc, "test_class");

  llvm::BasicBlock* block_class_equals =
      CreateBasicBlockWithDexPC(dex_pc, "class_eq");

  llvm::BasicBlock* block_test_sub_class =
      CreateBasicBlockWithDexPC(dex_pc, "test_sub_class");

  llvm::BasicBlock* block_cont =
      CreateBasicBlockWithDexPC(dex_pc, "instance_of_cont");

  // Overview of the following code :
  // We check for null, if so, then false, otherwise check for class == . If so
  // then true, otherwise do callout slowpath.
  //
  // Test: Is the reference equal to null?  Set 0 when it is null.
  llvm::Value* equal_null = irb_.CreateICmpEQ(object_addr, irb_.getJNull());

  irb_.CreateCondBr(equal_null, block_nullp, block_test_class);

  irb_.SetInsertPoint(block_nullp);
  irb_.CreateBr(block_cont);

  // Test: Is the object instantiated from the given class?
  irb_.SetInsertPoint(block_test_class);
  llvm::Value* type_object_addr = EmitLoadConstantClass(dex_pc, type_idx);
  DCHECK_EQ(art::Object::ClassOffset().Int32Value(), 0);

  llvm::PointerType* jobject_ptr_ty = irb_.getJObjectTy();

  llvm::Value* object_type_field_addr =
    irb_.CreateBitCast(object_addr, jobject_ptr_ty->getPointerTo());

  llvm::Value* object_type_object_addr =
    irb_.CreateLoad(object_type_field_addr, kTBAAConstJObject);

  llvm::Value* equal_class =
    irb_.CreateICmpEQ(type_object_addr, object_type_object_addr);

  irb_.CreateCondBr(equal_class, block_class_equals, block_test_sub_class);

  irb_.SetInsertPoint(block_class_equals);
  irb_.CreateBr(block_cont);

  // Test: Is the object instantiated from the subclass of the given class?
  irb_.SetInsertPoint(block_test_sub_class);
  llvm::Value* result =
    irb_.CreateCall2(irb_.GetRuntime(runtime_support::IsAssignable),
                     type_object_addr, object_type_object_addr);
  irb_.CreateBr(block_cont);

  irb_.SetInsertPoint(block_cont);

  llvm::PHINode* phi = irb_.CreatePHI(irb_.getJIntTy(), 3);

  phi->addIncoming(irb_.getJInt(0), block_nullp);
  phi->addIncoming(irb_.getJInt(1), block_class_equals);
  phi->addIncoming(result, block_test_sub_class);

  return phi;
}

llvm::Value* GBCExpanderPass::Expand_NewInstance(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(0));

  llvm::Function* runtime_func;
  if (compiler_->CanAccessInstantiableTypeWithoutChecks(method_idx_, *dex_file_, type_idx)) {
    runtime_func = irb_.GetRuntime(runtime_support::AllocObject);
  } else {
    runtime_func = irb_.GetRuntime(runtime_support::AllocObjectWithAccessCheck);
  }

  llvm::Constant* type_index_value = irb_.getInt32(type_idx);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

  EmitUpdateDexPC(dex_pc);

  llvm::Value* object_addr =
    irb_.CreateCall3(runtime_func, type_index_value, method_object_addr, thread_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  return object_addr;
}

llvm::Value* GBCExpanderPass::Expand_HLInvoke(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  art::InvokeType invoke_type = static_cast<art::InvokeType>(LV2UInt(call_inst.getArgOperand(0)));
  bool is_static = (invoke_type == art::kStatic);
  uint32_t callee_method_idx = LV2UInt(call_inst.getArgOperand(1));
  int opt_flags = LV2UInt(call_inst.getArgOperand(2));

  // Compute invoke related information for compiler decision
  int vtable_idx = -1;
  uintptr_t direct_code = 0;
  uintptr_t direct_method = 0;
  bool is_fast_path = compiler_->
    ComputeInvokeInfo(callee_method_idx, oat_compilation_unit_,
                      invoke_type, vtable_idx, direct_code, direct_method);

  // Load *this* actual parameter
  llvm::Value* this_addr = NULL;

  if (!is_static) {
    // Test: Is *this* parameter equal to null?
    this_addr = call_inst.getArgOperand(3);
  }

  // Load the method object
  llvm::Value* callee_method_object_addr = NULL;

  if (!is_fast_path) {
    callee_method_object_addr =
      EmitCallRuntimeForCalleeMethodObjectAddr(callee_method_idx, invoke_type,
                                               this_addr, dex_pc, is_fast_path);

    if (!is_static && !(opt_flags & MIR_IGNORE_NULL_CHECK)) {
      EmitGuard_NullPointerException(dex_pc, this_addr);
    }
  } else {
    if (!is_static && !(opt_flags & MIR_IGNORE_NULL_CHECK)) {
      EmitGuard_NullPointerException(dex_pc, this_addr);
    }

    switch (invoke_type) {
    case art::kStatic:
    case art::kDirect:
      if (direct_method != 0u &&
          direct_method != static_cast<uintptr_t>(-1)) {
        callee_method_object_addr =
          irb_.CreateIntToPtr(irb_.getPtrEquivInt(direct_method),
                              irb_.getJObjectTy());
      } else {
        callee_method_object_addr =
          EmitLoadSDCalleeMethodObjectAddr(callee_method_idx);
      }
      break;

    case art::kVirtual:
      DCHECK(vtable_idx != -1);
      callee_method_object_addr =
        EmitLoadVirtualCalleeMethodObjectAddr(vtable_idx, this_addr);
      break;

    case art::kSuper:
      LOG(FATAL) << "invoke-super should be promoted to invoke-direct in "
                    "the fast path.";
      break;

    case art::kInterface:
      callee_method_object_addr =
        EmitCallRuntimeForCalleeMethodObjectAddr(callee_method_idx,
                                                 invoke_type, this_addr,
                                                 dex_pc, is_fast_path);
      break;
    }
  }

  // Load the actual parameter
  std::vector<llvm::Value*> args;

  args.push_back(callee_method_object_addr); // method object for callee

  for (uint32_t i = 3; i < call_inst.getNumArgOperands(); ++i) {
    args.push_back(call_inst.getArgOperand(i));
  }

  llvm::Value* code_addr;
  if (direct_code != 0u &&
      direct_code != static_cast<uintptr_t>(-1)) {
    code_addr =
      irb_.CreateIntToPtr(irb_.getPtrEquivInt(direct_code),
                          GetFunctionType(callee_method_idx, is_static)->getPointerTo());
  } else {
    code_addr =
      irb_.LoadFromObjectOffset(callee_method_object_addr,
                                art::AbstractMethod::GetCodeOffset().Int32Value(),
                                GetFunctionType(callee_method_idx, is_static)->getPointerTo(),
                                kTBAAJRuntime);
  }

  // Invoke callee
  EmitUpdateDexPC(dex_pc);
  llvm::Value* retval = irb_.CreateCall(code_addr, args);
  EmitGuard_ExceptionLandingPad(dex_pc);

  return retval;
}

llvm::Value* GBCExpanderPass::Expand_OptArrayLength(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  // Get the array object address
  llvm::Value* array_addr = call_inst.getArgOperand(1);
  int opt_flags = LV2UInt(call_inst.getArgOperand(0));

  if (!(opt_flags & MIR_IGNORE_NULL_CHECK)) {
    EmitGuard_NullPointerException(dex_pc, array_addr);
  }

  // Get the array length and store it to the register
  return EmitLoadArrayLength(array_addr);
}

llvm::Value* GBCExpanderPass::Expand_NewArray(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(0));
  llvm::Value* length = call_inst.getArgOperand(1);

  return EmitAllocNewArray(dex_pc, length, type_idx, false);
}

llvm::Value* GBCExpanderPass::Expand_HLFilledNewArray(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  uint32_t type_idx = LV2UInt(call_inst.getArgOperand(1));
  uint32_t length = call_inst.getNumArgOperands() - 3;

  llvm::Value* object_addr =
    EmitAllocNewArray(dex_pc, irb_.getInt32(length), type_idx, true);

  if (length > 0) {
    // Check for the element type
    uint32_t type_desc_len = 0;
    const char* type_desc =
      dex_file_->StringByTypeIdx(type_idx, &type_desc_len);

    DCHECK_GE(type_desc_len, 2u); // should be guaranteed by verifier
    DCHECK_EQ(type_desc[0], '['); // should be guaranteed by verifier
    bool is_elem_int_ty = (type_desc[1] == 'I');

    uint32_t alignment;
    llvm::Constant* elem_size;
    llvm::PointerType* field_type;

    // NOTE: Currently filled-new-array only supports 'L', '[', and 'I'
    // as the element, thus we are only checking 2 cases: primitive int and
    // non-primitive type.
    if (is_elem_int_ty) {
      alignment = sizeof(int32_t);
      elem_size = irb_.getPtrEquivInt(sizeof(int32_t));
      field_type = irb_.getJIntTy()->getPointerTo();
    } else {
      alignment = irb_.getSizeOfPtrEquivInt();
      elem_size = irb_.getSizeOfPtrEquivIntValue();
      field_type = irb_.getJObjectTy()->getPointerTo();
    }

    llvm::Value* data_field_offset =
      irb_.getPtrEquivInt(art::Array::DataOffset(alignment).Int32Value());

    llvm::Value* data_field_addr =
      irb_.CreatePtrDisp(object_addr, data_field_offset, field_type);

    // TODO: Tune this code.  Currently we are generating one instruction for
    // one element which may be very space consuming.  Maybe changing to use
    // memcpy may help; however, since we can't guarantee that the alloca of
    // dalvik register are continuous, we can't perform such optimization yet.
    for (uint32_t i = 0; i < length; ++i) {
      llvm::Value* reg_value = call_inst.getArgOperand(i+3);

      irb_.CreateStore(reg_value, data_field_addr, kTBAAHeapArray);

      data_field_addr =
        irb_.CreatePtrDisp(data_field_addr, elem_size, field_type);
    }
  }

  return object_addr;
}

void GBCExpanderPass::Expand_HLFillArrayData(llvm::CallInst& call_inst) {
  uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));
  int32_t payload_offset = static_cast<int32_t>(dex_pc) +
                           LV2SInt(call_inst.getArgOperand(0));
  llvm::Value* array_addr = call_inst.getArgOperand(1);

  const art::Instruction::ArrayDataPayload* payload =
    reinterpret_cast<const art::Instruction::ArrayDataPayload*>(
        code_item_->insns_ + payload_offset);

  if (payload->element_count == 0) {
    // When the number of the elements in the payload is zero, we don't have
    // to copy any numbers.  However, we should check whether the array object
    // address is equal to null or not.
    EmitGuard_NullPointerException(dex_pc, array_addr);
  } else {
    // To save the code size, we are going to call the runtime function to
    // copy the content from DexFile.

    // NOTE: We will check for the NullPointerException in the runtime.

    llvm::Function* runtime_func = irb_.GetRuntime(runtime_support::FillArrayData);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitUpdateDexPC(dex_pc);

    irb_.CreateCall4(runtime_func,
                     method_object_addr, irb_.getInt32(dex_pc),
                     array_addr, irb_.getInt32(payload_offset));

    EmitGuard_ExceptionLandingPad(dex_pc);
  }

  return;
}

llvm::Value* GBCExpanderPass::EmitAllocNewArray(uint32_t dex_pc,
                                                llvm::Value* array_length_value,
                                                uint32_t type_idx,
                                                bool is_filled_new_array) {
  llvm::Function* runtime_func;

  bool skip_access_check =
    compiler_->CanAccessTypeWithoutChecks(method_idx_, *dex_file_, type_idx);


  if (is_filled_new_array) {
    runtime_func = skip_access_check ?
      irb_.GetRuntime(runtime_support::CheckAndAllocArray) :
      irb_.GetRuntime(runtime_support::CheckAndAllocArrayWithAccessCheck);
  } else {
    runtime_func = skip_access_check ?
      irb_.GetRuntime(runtime_support::AllocArray) :
      irb_.GetRuntime(runtime_support::AllocArrayWithAccessCheck);
  }

  llvm::Constant* type_index_value = irb_.getInt32(type_idx);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

  EmitUpdateDexPC(dex_pc);

  llvm::Value* object_addr =
    irb_.CreateCall4(runtime_func, type_index_value, method_object_addr,
                     array_length_value, thread_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  return object_addr;
}

llvm::Value* GBCExpanderPass::
EmitCallRuntimeForCalleeMethodObjectAddr(uint32_t callee_method_idx,
                                         art::InvokeType invoke_type,
                                         llvm::Value* this_addr,
                                         uint32_t dex_pc,
                                         bool is_fast_path) {

  llvm::Function* runtime_func = NULL;

  switch (invoke_type) {
  case art::kStatic:
    runtime_func = irb_.GetRuntime(runtime_support::FindStaticMethodWithAccessCheck);
    break;

  case art::kDirect:
    runtime_func = irb_.GetRuntime(runtime_support::FindDirectMethodWithAccessCheck);
    break;

  case art::kVirtual:
    runtime_func = irb_.GetRuntime(runtime_support::FindVirtualMethodWithAccessCheck);
    break;

  case art::kSuper:
    runtime_func = irb_.GetRuntime(runtime_support::FindSuperMethodWithAccessCheck);
    break;

  case art::kInterface:
    if (is_fast_path) {
      runtime_func = irb_.GetRuntime(runtime_support::FindInterfaceMethod);
    } else {
      runtime_func = irb_.GetRuntime(runtime_support::FindInterfaceMethodWithAccessCheck);
    }
    break;
  }

  llvm::Value* callee_method_idx_value = irb_.getInt32(callee_method_idx);

  if (this_addr == NULL) {
    DCHECK_EQ(invoke_type, art::kStatic);
    this_addr = irb_.getJNull();
  }

  llvm::Value* caller_method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* thread_object_addr = irb_.Runtime().EmitGetCurrentThread();

  EmitUpdateDexPC(dex_pc);

  llvm::Value* callee_method_object_addr =
    irb_.CreateCall4(runtime_func,
                     callee_method_idx_value,
                     this_addr,
                     caller_method_object_addr,
                     thread_object_addr);

  EmitGuard_ExceptionLandingPad(dex_pc);

  return callee_method_object_addr;
}

void GBCExpanderPass::EmitMarkGCCard(llvm::Value* value, llvm::Value* target_addr) {
  // Using runtime support, let the target can override by InlineAssembly.
  irb_.Runtime().EmitMarkGCCard(value, target_addr);
}

void GBCExpanderPass::EmitUpdateDexPC(uint32_t dex_pc) {
#if defined(ART_USE_QUICK_COMPILER)
  if (shadow_frame_ == NULL) {
    return;
  }
#endif
  irb_.StoreToObjectOffset(shadow_frame_,
                           art::ShadowFrame::DexPCOffset(),
                           irb_.getInt32(dex_pc),
                           kTBAAShadowFrame);
}

void GBCExpanderPass::EmitGuard_DivZeroException(uint32_t dex_pc,
                                                 llvm::Value* denominator,
                                                 JType op_jty) {
  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Constant* zero = irb_.getJZero(op_jty);

  llvm::Value* equal_zero = irb_.CreateICmpEQ(denominator, zero);

  llvm::BasicBlock* block_exception = CreateBasicBlockWithDexPC(dex_pc, "div0");

  llvm::BasicBlock* block_continue = CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_zero, block_exception, block_continue, kUnlikely);

  irb_.SetInsertPoint(block_exception);
  EmitUpdateDexPC(dex_pc);
  irb_.CreateCall(irb_.GetRuntime(runtime_support::ThrowDivZeroException));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}

void GBCExpanderPass::EmitGuard_NullPointerException(uint32_t dex_pc,
                                                     llvm::Value* object) {
  llvm::Value* equal_null = irb_.CreateICmpEQ(object, irb_.getJNull());

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "nullp");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_null, block_exception, block_continue, kUnlikely);

  irb_.SetInsertPoint(block_exception);
  EmitUpdateDexPC(dex_pc);
  irb_.CreateCall(irb_.GetRuntime(runtime_support::ThrowNullPointerException),
                  irb_.getInt32(dex_pc));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}

void
GBCExpanderPass::EmitGuard_ArrayIndexOutOfBoundsException(uint32_t dex_pc,
                                                          llvm::Value* array,
                                                          llvm::Value* index) {
  llvm::Value* array_len = EmitLoadArrayLength(array);

  llvm::Value* cmp = irb_.CreateICmpUGE(index, array_len);

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "overflow");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(cmp, block_exception, block_continue, kUnlikely);

  irb_.SetInsertPoint(block_exception);

  EmitUpdateDexPC(dex_pc);
  irb_.CreateCall2(irb_.GetRuntime(runtime_support::ThrowIndexOutOfBounds), index, array_len);
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}

llvm::FunctionType* GBCExpanderPass::GetFunctionType(uint32_t method_idx,
                                                     bool is_static) {
  // Get method signature
  art::DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx);

  uint32_t shorty_size;
  const char* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Get return type

  char ret_shorty = shorty[0];
#if defined(ART_USE_QUICK_COMPILER)
  ret_shorty = art::remapShorty(ret_shorty);
#endif
  llvm::Type* ret_type = irb_.getJType(ret_shorty, kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static) {
    args_type.push_back(irb_.getJType('L', kAccurate)); // "this" object pointer
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
#if defined(ART_USE_QUICK_COMPILER)
    char shorty_type = art::remapShorty(shorty[i]);
    args_type.push_back(irb_.getJType(shorty_type, kAccurate));
#else
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
#endif
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}


llvm::BasicBlock* GBCExpanderPass::
CreateBasicBlockWithDexPC(uint32_t dex_pc, const char* postfix) {
  std::string name;

#if !defined(NDEBUG)
  art::StringAppendF(&name, "B%04x.%s", dex_pc, postfix);
#endif

  return llvm::BasicBlock::Create(context_, name, func_);
}

llvm::BasicBlock* GBCExpanderPass::GetBasicBlock(uint32_t dex_pc) {
  DCHECK(dex_pc < code_item_->insns_size_in_code_units_);
  CHECK(basic_blocks_[dex_pc] != NULL);
  return basic_blocks_[dex_pc];
}

int32_t GBCExpanderPass::GetTryItemOffset(uint32_t dex_pc) {
  int32_t min = 0;
  int32_t max = code_item_->tries_size_ - 1;

  while (min <= max) {
    int32_t mid = min + (max - min) / 2;

    const art::DexFile::TryItem* ti = art::DexFile::GetTryItems(*code_item_, mid);
    uint32_t start = ti->start_addr_;
    uint32_t end = start + ti->insn_count_;

    if (dex_pc < start) {
      max = mid - 1;
    } else if (dex_pc >= end) {
      min = mid + 1;
    } else {
      return mid; // found
    }
  }

  return -1; // not found
}

llvm::BasicBlock* GBCExpanderPass::GetLandingPadBasicBlock(uint32_t dex_pc) {
  // Find the try item for this address in this method
  int32_t ti_offset = GetTryItemOffset(dex_pc);

  if (ti_offset == -1) {
    return NULL; // No landing pad is available for this address.
  }

  // Check for the existing landing pad basic block
  DCHECK_GT(basic_block_landing_pads_.size(), static_cast<size_t>(ti_offset));
  llvm::BasicBlock* block_lpad = basic_block_landing_pads_[ti_offset];

  if (block_lpad) {
    // We have generated landing pad for this try item already.  Return the
    // same basic block.
    return block_lpad;
  }

  // Get try item from code item
  const art::DexFile::TryItem* ti = art::DexFile::GetTryItems(*code_item_, ti_offset);

  std::string lpadname;

#if !defined(NDEBUG)
  art::StringAppendF(&lpadname, "lpad%d_%04x_to_%04x", ti_offset, ti->start_addr_, ti->handler_off_);
#endif

  // Create landing pad basic block
  block_lpad = llvm::BasicBlock::Create(context_, lpadname, func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(block_lpad);

  // Find catch block with matching type
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* ti_offset_value = irb_.getInt32(ti_offset);

  llvm::Value* catch_handler_index_value =
    irb_.CreateCall2(irb_.GetRuntime(runtime_support::FindCatchBlock),
                     method_object_addr, ti_offset_value);

  // Switch instruction (Go to unwind basic block by default)
  llvm::SwitchInst* sw =
    irb_.CreateSwitch(catch_handler_index_value, GetUnwindBasicBlock());

  // Cases with matched catch block
  art::CatchHandlerIterator iter(*code_item_, ti->start_addr_);

  for (uint32_t c = 0; iter.HasNext(); iter.Next(), ++c) {
    sw->addCase(irb_.getInt32(c), GetBasicBlock(iter.GetHandlerAddress()));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  // Cache this landing pad
  DCHECK_GT(basic_block_landing_pads_.size(), static_cast<size_t>(ti_offset));
  basic_block_landing_pads_[ti_offset] = block_lpad;

  return block_lpad;
}

llvm::BasicBlock* GBCExpanderPass::GetUnwindBasicBlock() {
  // Check the existing unwinding baisc block block
  if (basic_block_unwind_ != NULL) {
    return basic_block_unwind_;
  }

  // Create new basic block for unwinding
  basic_block_unwind_ =
    llvm::BasicBlock::Create(context_, "exception_unwind", func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(basic_block_unwind_);

  // Pop the shadow frame
  Expand_PopShadowFrame();

  // Emit the code to return default value (zero) for the given return type.
  char ret_shorty = oat_compilation_unit_->GetShorty()[0];
#if defined(ART_USE_QUICK_COMPILER)
  ret_shorty = art::remapShorty(ret_shorty);
#endif
  if (ret_shorty == 'V') {
    irb_.CreateRetVoid();
  } else {
    irb_.CreateRet(irb_.getJZero(ret_shorty));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  return basic_block_unwind_;
}

void GBCExpanderPass::EmitBranchExceptionLandingPad(uint32_t dex_pc) {
  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    landing_pad_phi_mapping_[lpad].push_back(std::make_pair(current_bb_->getUniquePredecessor(),
                                                            irb_.GetInsertBlock()));
    irb_.CreateBr(lpad);
  } else {
    irb_.CreateBr(GetUnwindBasicBlock());
  }
}

void GBCExpanderPass::EmitGuard_ExceptionLandingPad(uint32_t dex_pc) {
  llvm::Value* exception_pending = irb_.Runtime().EmitIsExceptionPending();

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    landing_pad_phi_mapping_[lpad].push_back(std::make_pair(current_bb_->getUniquePredecessor(),
                                                            irb_.GetInsertBlock()));
    irb_.CreateCondBr(exception_pending, lpad, block_cont, kUnlikely);
  } else {
    irb_.CreateCondBr(exception_pending, GetUnwindBasicBlock(), block_cont, kUnlikely);
  }

  irb_.SetInsertPoint(block_cont);
}

llvm::Value*
GBCExpanderPass::ExpandIntrinsic(IntrinsicHelper::IntrinsicId intr_id,
                                 llvm::CallInst& call_inst) {
  switch (intr_id) {
    //==- Thread -----------------------------------------------------------==//
    case IntrinsicHelper::GetCurrentThread: {
      return irb_.Runtime().EmitGetCurrentThread();
    }
    case IntrinsicHelper::CheckSuspend: {
      // We will add suspend by ourselves.
      return NULL;
    }
    case IntrinsicHelper::TestSuspend: {
      Expand_TestSuspend(call_inst);
      return NULL;
    }
    case IntrinsicHelper::MarkGCCard: {
      Expand_MarkGCCard(call_inst);
      return NULL;
    }

    //==- Exception --------------------------------------------------------==//
    case IntrinsicHelper::ThrowException: {
      return ExpandToRuntime(runtime_support::ThrowException, call_inst);
    }
    case IntrinsicHelper::HLThrowException: {
      uint32_t dex_pc = LV2UInt(call_inst.getMetadata("DexOff")->getOperand(0));

      EmitUpdateDexPC(dex_pc);

      irb_.CreateCall(irb_.GetRuntime(runtime_support::ThrowException),
                      call_inst.getArgOperand(0));

      EmitGuard_ExceptionLandingPad(dex_pc);
      return NULL;
    }
    case IntrinsicHelper::GetException: {
      return Expand_GetException();
    }
    case IntrinsicHelper::IsExceptionPending: {
      return irb_.Runtime().EmitIsExceptionPending();
    }
    case IntrinsicHelper::FindCatchBlock: {
      return ExpandToRuntime(runtime_support::FindCatchBlock, call_inst);
    }
    case IntrinsicHelper::ThrowDivZeroException: {
      return ExpandToRuntime(runtime_support::ThrowDivZeroException, call_inst);
    }
    case IntrinsicHelper::ThrowNullPointerException: {
      return ExpandToRuntime(runtime_support::ThrowNullPointerException, call_inst);
    }
    case IntrinsicHelper::ThrowIndexOutOfBounds: {
      return ExpandToRuntime(runtime_support::ThrowIndexOutOfBounds, call_inst);
    }

    //==- Const String -----------------------------------------------------==//
    case IntrinsicHelper::ConstString: {
      return Expand_ConstString(call_inst);
    }
    case IntrinsicHelper::LoadStringFromDexCache: {
      return Expand_LoadStringFromDexCache(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::ResolveString: {
      return ExpandToRuntime(runtime_support::ResolveString, call_inst);
    }

    //==- Const Class ------------------------------------------------------==//
    case IntrinsicHelper::ConstClass: {
      return Expand_ConstClass(call_inst);
    }
    case IntrinsicHelper::InitializeTypeAndVerifyAccess: {
      return ExpandToRuntime(runtime_support::InitializeTypeAndVerifyAccess, call_inst);
    }
    case IntrinsicHelper::LoadTypeFromDexCache: {
      return Expand_LoadTypeFromDexCache(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::InitializeType: {
      return ExpandToRuntime(runtime_support::InitializeType, call_inst);
    }

    //==- Lock -------------------------------------------------------------==//
    case IntrinsicHelper::LockObject: {
      Expand_LockObject(call_inst.getArgOperand(0));
      return NULL;
    }
    case IntrinsicHelper::UnlockObject: {
      Expand_UnlockObject(call_inst.getArgOperand(0));
      return NULL;
    }

    //==- Cast -------------------------------------------------------------==//
    case IntrinsicHelper::CheckCast: {
      return ExpandToRuntime(runtime_support::CheckCast, call_inst);
    }
    case IntrinsicHelper::HLCheckCast: {
      Expand_HLCheckCast(call_inst);
      return NULL;
    }
    case IntrinsicHelper::IsAssignable: {
      return ExpandToRuntime(runtime_support::IsAssignable, call_inst);
    }

    //==- Alloc ------------------------------------------------------------==//
    case IntrinsicHelper::AllocObject: {
      return ExpandToRuntime(runtime_support::AllocObject, call_inst);
    }
    case IntrinsicHelper::AllocObjectWithAccessCheck: {
      return ExpandToRuntime(runtime_support::AllocObjectWithAccessCheck, call_inst);
    }

    //==- Instance ---------------------------------------------------------==//
    case IntrinsicHelper::NewInstance: {
      return Expand_NewInstance(call_inst);
    }
    case IntrinsicHelper::InstanceOf: {
      return Expand_InstanceOf(call_inst);
    }

    //==- Array ------------------------------------------------------------==//
    case IntrinsicHelper::NewArray: {
      return Expand_NewArray(call_inst);
    }
    case IntrinsicHelper::OptArrayLength: {
      return Expand_OptArrayLength(call_inst);
    }
    case IntrinsicHelper::ArrayLength: {
      return EmitLoadArrayLength(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::AllocArray: {
      return ExpandToRuntime(runtime_support::AllocArray, call_inst);
    }
    case IntrinsicHelper::AllocArrayWithAccessCheck: {
      return ExpandToRuntime(runtime_support::AllocArrayWithAccessCheck,
                             call_inst);
    }
    case IntrinsicHelper::CheckAndAllocArray: {
      return ExpandToRuntime(runtime_support::CheckAndAllocArray, call_inst);
    }
    case IntrinsicHelper::CheckAndAllocArrayWithAccessCheck: {
      return ExpandToRuntime(runtime_support::CheckAndAllocArrayWithAccessCheck,
                             call_inst);
    }
    case IntrinsicHelper::ArrayGet: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kInt);
    }
    case IntrinsicHelper::ArrayGetWide: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kLong);
    }
    case IntrinsicHelper::ArrayGetObject: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kObject);
    }
    case IntrinsicHelper::ArrayGetBoolean: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kBoolean);
    }
    case IntrinsicHelper::ArrayGetByte: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kByte);
    }
    case IntrinsicHelper::ArrayGetChar: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kChar);
    }
    case IntrinsicHelper::ArrayGetShort: {
      return Expand_ArrayGet(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             kShort);
    }
    case IntrinsicHelper::ArrayPut: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kInt);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutWide: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kLong);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutObject: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kObject);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutBoolean: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kBoolean);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutByte: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kByte);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutChar: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kChar);
      return NULL;
    }
    case IntrinsicHelper::ArrayPutShort: {
      Expand_ArrayPut(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      kShort);
      return NULL;
    }
    case IntrinsicHelper::CheckPutArrayElement: {
      return ExpandToRuntime(runtime_support::CheckPutArrayElement, call_inst);
    }
    case IntrinsicHelper::FilledNewArray: {
      Expand_FilledNewArray(call_inst);
      return NULL;
    }
    case IntrinsicHelper::FillArrayData: {
      return ExpandToRuntime(runtime_support::FillArrayData, call_inst);
    }
    case IntrinsicHelper::HLFillArrayData: {
      Expand_HLFillArrayData(call_inst);
      return NULL;
    }
    case IntrinsicHelper::HLFilledNewArray: {
      return Expand_HLFilledNewArray(call_inst);
    }

    //==- Instance Field ---------------------------------------------------==//
    case IntrinsicHelper::InstanceFieldGet:
    case IntrinsicHelper::InstanceFieldGetBoolean:
    case IntrinsicHelper::InstanceFieldGetByte:
    case IntrinsicHelper::InstanceFieldGetChar:
    case IntrinsicHelper::InstanceFieldGetShort: {
      return ExpandToRuntime(runtime_support::Get32Instance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldGetWide: {
      return ExpandToRuntime(runtime_support::Get64Instance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldGetObject: {
      return ExpandToRuntime(runtime_support::GetObjectInstance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldGetFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kInt);
    }
    case IntrinsicHelper::InstanceFieldGetWideFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kLong);
    }
    case IntrinsicHelper::InstanceFieldGetObjectFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kObject);
    }
    case IntrinsicHelper::InstanceFieldGetBooleanFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kBoolean);
    }
    case IntrinsicHelper::InstanceFieldGetByteFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kByte);
    }
    case IntrinsicHelper::InstanceFieldGetCharFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kChar);
    }
    case IntrinsicHelper::InstanceFieldGetShortFast: {
      return Expand_IGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kShort);
    }
    case IntrinsicHelper::InstanceFieldPut:
    case IntrinsicHelper::InstanceFieldPutBoolean:
    case IntrinsicHelper::InstanceFieldPutByte:
    case IntrinsicHelper::InstanceFieldPutChar:
    case IntrinsicHelper::InstanceFieldPutShort: {
      return ExpandToRuntime(runtime_support::Set32Instance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldPutWide: {
      return ExpandToRuntime(runtime_support::Set64Instance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldPutObject: {
      return ExpandToRuntime(runtime_support::SetObjectInstance, call_inst);
    }
    case IntrinsicHelper::InstanceFieldPutFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kInt);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutWideFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kLong);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutObjectFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kObject);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutBooleanFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kBoolean);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutByteFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kByte);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutCharFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kChar);
      return NULL;
    }
    case IntrinsicHelper::InstanceFieldPutShortFast: {
      Expand_IPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kShort);
      return NULL;
    }

    //==- Static Field -----------------------------------------------------==//
    case IntrinsicHelper::StaticFieldGet:
    case IntrinsicHelper::StaticFieldGetBoolean:
    case IntrinsicHelper::StaticFieldGetByte:
    case IntrinsicHelper::StaticFieldGetChar:
    case IntrinsicHelper::StaticFieldGetShort: {
      return ExpandToRuntime(runtime_support::Get32Static, call_inst);
    }
    case IntrinsicHelper::StaticFieldGetWide: {
      return ExpandToRuntime(runtime_support::Get64Static, call_inst);
    }
    case IntrinsicHelper::StaticFieldGetObject: {
      return ExpandToRuntime(runtime_support::GetObjectStatic, call_inst);
    }
    case IntrinsicHelper::StaticFieldGetFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kInt);
    }
    case IntrinsicHelper::StaticFieldGetWideFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kLong);
    }
    case IntrinsicHelper::StaticFieldGetObjectFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kObject);
    }
    case IntrinsicHelper::StaticFieldGetBooleanFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kBoolean);
    }
    case IntrinsicHelper::StaticFieldGetByteFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kByte);
    }
    case IntrinsicHelper::StaticFieldGetCharFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kChar);
    }
    case IntrinsicHelper::StaticFieldGetShortFast: {
      return Expand_SGetFast(call_inst.getArgOperand(0),
                             call_inst.getArgOperand(1),
                             call_inst.getArgOperand(2),
                             kShort);
    }
    case IntrinsicHelper::StaticFieldPut:
    case IntrinsicHelper::StaticFieldPutBoolean:
    case IntrinsicHelper::StaticFieldPutByte:
    case IntrinsicHelper::StaticFieldPutChar:
    case IntrinsicHelper::StaticFieldPutShort: {
      return ExpandToRuntime(runtime_support::Set32Static, call_inst);
    }
    case IntrinsicHelper::StaticFieldPutWide: {
      return ExpandToRuntime(runtime_support::Set64Static, call_inst);
    }
    case IntrinsicHelper::StaticFieldPutObject: {
      return ExpandToRuntime(runtime_support::SetObjectStatic, call_inst);
    }
    case IntrinsicHelper::StaticFieldPutFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kInt);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutWideFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kLong);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutObjectFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kObject);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutBooleanFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kBoolean);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutByteFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kByte);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutCharFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kChar);
      return NULL;
    }
    case IntrinsicHelper::StaticFieldPutShortFast: {
      Expand_SPutFast(call_inst.getArgOperand(0),
                      call_inst.getArgOperand(1),
                      call_inst.getArgOperand(2),
                      call_inst.getArgOperand(3),
                      kShort);
      return NULL;
    }
    case IntrinsicHelper::LoadDeclaringClassSSB: {
      return Expand_LoadDeclaringClassSSB(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::LoadClassSSBFromDexCache: {
      return Expand_LoadClassSSBFromDexCache(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::InitializeAndLoadClassSSB: {
      return ExpandToRuntime(runtime_support::InitializeStaticStorage, call_inst);
    }

    //==- High-level Array -------------------------------------------------==//
    case IntrinsicHelper::HLArrayGet: {
      return Expand_HLArrayGet(call_inst, kInt);
    }
    case IntrinsicHelper::HLArrayGetBoolean: {
      return Expand_HLArrayGet(call_inst, kBoolean);
    }
    case IntrinsicHelper::HLArrayGetByte: {
      return Expand_HLArrayGet(call_inst, kByte);
    }
    case IntrinsicHelper::HLArrayGetChar: {
      return Expand_HLArrayGet(call_inst, kChar);
    }
    case IntrinsicHelper::HLArrayGetShort: {
      return Expand_HLArrayGet(call_inst, kShort);
    }
    case IntrinsicHelper::HLArrayGetFloat: {
      return Expand_HLArrayGet(call_inst, kFloat);
    }
    case IntrinsicHelper::HLArrayGetWide: {
      return Expand_HLArrayGet(call_inst, kLong);
    }
    case IntrinsicHelper::HLArrayGetDouble: {
      return Expand_HLArrayGet(call_inst, kDouble);
    }
    case IntrinsicHelper::HLArrayGetObject: {
      return Expand_HLArrayGet(call_inst, kObject);
    }
    case IntrinsicHelper::HLArrayPut: {
      Expand_HLArrayPut(call_inst, kInt);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutBoolean: {
      Expand_HLArrayPut(call_inst, kBoolean);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutByte: {
      Expand_HLArrayPut(call_inst, kByte);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutChar: {
      Expand_HLArrayPut(call_inst, kChar);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutShort: {
      Expand_HLArrayPut(call_inst, kShort);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutFloat: {
      Expand_HLArrayPut(call_inst, kFloat);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutWide: {
      Expand_HLArrayPut(call_inst, kLong);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutDouble: {
      Expand_HLArrayPut(call_inst, kDouble);
      return NULL;
    }
    case IntrinsicHelper::HLArrayPutObject: {
      Expand_HLArrayPut(call_inst, kObject);
      return NULL;
    }

    //==- High-level Instance ----------------------------------------------==//
    case IntrinsicHelper::HLIGet: {
      return Expand_HLIGet(call_inst, kInt);
    }
    case IntrinsicHelper::HLIGetBoolean: {
      return Expand_HLIGet(call_inst, kBoolean);
    }
    case IntrinsicHelper::HLIGetByte: {
      return Expand_HLIGet(call_inst, kByte);
    }
    case IntrinsicHelper::HLIGetChar: {
      return Expand_HLIGet(call_inst, kChar);
    }
    case IntrinsicHelper::HLIGetShort: {
      return Expand_HLIGet(call_inst, kShort);
    }
    case IntrinsicHelper::HLIGetFloat: {
      return Expand_HLIGet(call_inst, kFloat);
    }
    case IntrinsicHelper::HLIGetWide: {
      return Expand_HLIGet(call_inst, kLong);
    }
    case IntrinsicHelper::HLIGetDouble: {
      return Expand_HLIGet(call_inst, kDouble);
    }
    case IntrinsicHelper::HLIGetObject: {
      return Expand_HLIGet(call_inst, kObject);
    }
    case IntrinsicHelper::HLIPut: {
      Expand_HLIPut(call_inst, kInt);
      return NULL;
    }
    case IntrinsicHelper::HLIPutBoolean: {
      Expand_HLIPut(call_inst, kBoolean);
      return NULL;
    }
    case IntrinsicHelper::HLIPutByte: {
      Expand_HLIPut(call_inst, kByte);
      return NULL;
    }
    case IntrinsicHelper::HLIPutChar: {
      Expand_HLIPut(call_inst, kChar);
      return NULL;
    }
    case IntrinsicHelper::HLIPutShort: {
      Expand_HLIPut(call_inst, kShort);
      return NULL;
    }
    case IntrinsicHelper::HLIPutFloat: {
      Expand_HLIPut(call_inst, kFloat);
      return NULL;
    }
    case IntrinsicHelper::HLIPutWide: {
      Expand_HLIPut(call_inst, kLong);
      return NULL;
    }
    case IntrinsicHelper::HLIPutDouble: {
      Expand_HLIPut(call_inst, kDouble);
      return NULL;
    }
    case IntrinsicHelper::HLIPutObject: {
      Expand_HLIPut(call_inst, kObject);
      return NULL;
    }

    //==- High-level Invoke ------------------------------------------------==//
    case IntrinsicHelper::HLInvokeVoid:
    case IntrinsicHelper::HLInvokeObj:
    case IntrinsicHelper::HLInvokeInt:
    case IntrinsicHelper::HLInvokeFloat:
    case IntrinsicHelper::HLInvokeLong:
    case IntrinsicHelper::HLInvokeDouble: {
      return Expand_HLInvoke(call_inst);
    }

    //==- Invoke -----------------------------------------------------------==//
    case IntrinsicHelper::FindStaticMethodWithAccessCheck: {
      return ExpandToRuntime(runtime_support::FindStaticMethodWithAccessCheck, call_inst);
    }
    case IntrinsicHelper::FindDirectMethodWithAccessCheck: {
      return ExpandToRuntime(runtime_support::FindDirectMethodWithAccessCheck, call_inst);
    }
    case IntrinsicHelper::FindVirtualMethodWithAccessCheck: {
      return ExpandToRuntime(runtime_support::FindVirtualMethodWithAccessCheck, call_inst);
    }
    case IntrinsicHelper::FindSuperMethodWithAccessCheck: {
      return ExpandToRuntime(runtime_support::FindSuperMethodWithAccessCheck, call_inst);
    }
    case IntrinsicHelper::FindInterfaceMethodWithAccessCheck: {
      return ExpandToRuntime(runtime_support::FindInterfaceMethodWithAccessCheck, call_inst);
    }
    case IntrinsicHelper::GetSDCalleeMethodObjAddrFast: {
      return Expand_GetSDCalleeMethodObjAddrFast(call_inst.getArgOperand(0));
    }
    case IntrinsicHelper::GetVirtualCalleeMethodObjAddrFast: {
      return Expand_GetVirtualCalleeMethodObjAddrFast(
                call_inst.getArgOperand(0), call_inst.getArgOperand(1));
    }
    case IntrinsicHelper::GetInterfaceCalleeMethodObjAddrFast: {
      return ExpandToRuntime(runtime_support::FindInterfaceMethod, call_inst);
    }
    case IntrinsicHelper::InvokeRetVoid:
    case IntrinsicHelper::InvokeRetBoolean:
    case IntrinsicHelper::InvokeRetByte:
    case IntrinsicHelper::InvokeRetChar:
    case IntrinsicHelper::InvokeRetShort:
    case IntrinsicHelper::InvokeRetInt:
    case IntrinsicHelper::InvokeRetLong:
    case IntrinsicHelper::InvokeRetFloat:
    case IntrinsicHelper::InvokeRetDouble:
    case IntrinsicHelper::InvokeRetObject: {
      return Expand_Invoke(call_inst);
    }

    //==- Math -------------------------------------------------------------==//
    case IntrinsicHelper::DivInt: {
      return Expand_DivRem(call_inst, /* is_div */true, kInt);
    }
    case IntrinsicHelper::RemInt: {
      return Expand_DivRem(call_inst, /* is_div */false, kInt);
    }
    case IntrinsicHelper::DivLong: {
      return Expand_DivRem(call_inst, /* is_div */true, kLong);
    }
    case IntrinsicHelper::RemLong: {
      return Expand_DivRem(call_inst, /* is_div */false, kLong);
    }
    case IntrinsicHelper::D2L: {
      return ExpandToRuntime(runtime_support::art_d2l, call_inst);
    }
    case IntrinsicHelper::D2I: {
      return ExpandToRuntime(runtime_support::art_d2i, call_inst);
    }
    case IntrinsicHelper::F2L: {
      return ExpandToRuntime(runtime_support::art_f2l, call_inst);
    }
    case IntrinsicHelper::F2I: {
      return ExpandToRuntime(runtime_support::art_f2i, call_inst);
    }

    //==- High-level Static ------------------------------------------------==//
    case IntrinsicHelper::HLSget: {
      return Expand_HLSget(call_inst, kInt);
    }
    case IntrinsicHelper::HLSgetBoolean: {
      return Expand_HLSget(call_inst, kBoolean);
    }
    case IntrinsicHelper::HLSgetByte: {
      return Expand_HLSget(call_inst, kByte);
    }
    case IntrinsicHelper::HLSgetChar: {
      return Expand_HLSget(call_inst, kChar);
    }
    case IntrinsicHelper::HLSgetShort: {
      return Expand_HLSget(call_inst, kShort);
    }
    case IntrinsicHelper::HLSgetFloat: {
      return Expand_HLSget(call_inst, kFloat);
    }
    case IntrinsicHelper::HLSgetWide: {
      return Expand_HLSget(call_inst, kLong);
    }
    case IntrinsicHelper::HLSgetDouble: {
      return Expand_HLSget(call_inst, kDouble);
    }
    case IntrinsicHelper::HLSgetObject: {
      return Expand_HLSget(call_inst, kObject);
    }
    case IntrinsicHelper::HLSput: {
      Expand_HLSput(call_inst, kInt);
      return NULL;
    }
    case IntrinsicHelper::HLSputBoolean: {
      Expand_HLSput(call_inst, kBoolean);
      return NULL;
    }
    case IntrinsicHelper::HLSputByte: {
      Expand_HLSput(call_inst, kByte);
      return NULL;
    }
    case IntrinsicHelper::HLSputChar: {
      Expand_HLSput(call_inst, kChar);
      return NULL;
    }
    case IntrinsicHelper::HLSputShort: {
      Expand_HLSput(call_inst, kShort);
      return NULL;
    }
    case IntrinsicHelper::HLSputFloat: {
      Expand_HLSput(call_inst, kFloat);
      return NULL;
    }
    case IntrinsicHelper::HLSputWide: {
      Expand_HLSput(call_inst, kLong);
      return NULL;
    }
    case IntrinsicHelper::HLSputDouble: {
      Expand_HLSput(call_inst, kDouble);
      return NULL;
    }
    case IntrinsicHelper::HLSputObject: {
      Expand_HLSput(call_inst, kObject);
      return NULL;
    }

    //==- High-level Monitor -----------------------------------------------==//
    case IntrinsicHelper::MonitorEnter: {
      Expand_MonitorEnter(call_inst);
      return NULL;
    }
    case IntrinsicHelper::MonitorExit: {
      Expand_MonitorExit(call_inst);
      return NULL;
    }

    //==- Shadow Frame -----------------------------------------------------==//
    case IntrinsicHelper::AllocaShadowFrame: {
      Expand_AllocaShadowFrame(call_inst.getArgOperand(0));
      return NULL;
    }
    case IntrinsicHelper::SetShadowFrameEntry: {
      Expand_SetShadowFrameEntry(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1));
      return NULL;
    }
    case IntrinsicHelper::PopShadowFrame: {
      Expand_PopShadowFrame();
      return NULL;
    }
    case IntrinsicHelper::UpdateDexPC: {
      Expand_UpdateDexPC(call_inst.getArgOperand(0));
      return NULL;
    }

    //==- Comparison -------------------------------------------------------==//
    case IntrinsicHelper::CmplFloat:
    case IntrinsicHelper::CmplDouble: {
      return Expand_FPCompare(call_inst.getArgOperand(0),
                              call_inst.getArgOperand(1),
                              false);
    }
    case IntrinsicHelper::CmpgFloat:
    case IntrinsicHelper::CmpgDouble: {
      return Expand_FPCompare(call_inst.getArgOperand(0),
                              call_inst.getArgOperand(1),
                              true);
    }
    case IntrinsicHelper::CmpLong: {
      return Expand_LongCompare(call_inst.getArgOperand(0),
                                call_inst.getArgOperand(1));
    }

    //==- Const ------------------------------------------------------------==//
    case IntrinsicHelper::ConstInt:
    case IntrinsicHelper::ConstLong: {
      return call_inst.getArgOperand(0);
    }
    case IntrinsicHelper::ConstFloat: {
      return irb_.CreateBitCast(call_inst.getArgOperand(0),
                                irb_.getJFloatTy());
    }
    case IntrinsicHelper::ConstDouble: {
      return irb_.CreateBitCast(call_inst.getArgOperand(0),
                                irb_.getJDoubleTy());
    }
    case IntrinsicHelper::ConstObj: {
      CHECK(LV2UInt(call_inst.getArgOperand(0)) == 0);
      return irb_.getJNull();
    }

    //==- Method Info ------------------------------------------------------==//
    case IntrinsicHelper::MethodInfo: {
      // Nothing to be done, because MethodInfo carries optional hints that are
      // not needed by the portable path.
      return NULL;
    }

    //==- Copy -------------------------------------------------------------==//
    case IntrinsicHelper::CopyInt:
    case IntrinsicHelper::CopyFloat:
    case IntrinsicHelper::CopyLong:
    case IntrinsicHelper::CopyDouble:
    case IntrinsicHelper::CopyObj: {
      return call_inst.getArgOperand(0);
    }

    //==- Shift ------------------------------------------------------------==//
    case IntrinsicHelper::SHLLong: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerSHL, kLong);
    }
    case IntrinsicHelper::SHRLong: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerSHR, kLong);
    }
    case IntrinsicHelper::USHRLong: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerUSHR, kLong);
    }
    case IntrinsicHelper::SHLInt: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerSHL, kInt);
    }
    case IntrinsicHelper::SHRInt: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerSHR, kInt);
    }
    case IntrinsicHelper::USHRInt: {
      return Expand_IntegerShift(call_inst.getArgOperand(0),
                                 call_inst.getArgOperand(1),
                                 kIntegerUSHR, kInt);
    }

    //==- Conversion -------------------------------------------------------==//
    case IntrinsicHelper::IntToChar: {
      return irb_.CreateZExt(irb_.CreateTrunc(call_inst.getArgOperand(0), irb_.getJCharTy()),
                             irb_.getJIntTy());
    }
    case IntrinsicHelper::IntToShort: {
      return irb_.CreateSExt(irb_.CreateTrunc(call_inst.getArgOperand(0), irb_.getJShortTy()),
                             irb_.getJIntTy());
    }
    case IntrinsicHelper::IntToByte: {
      return irb_.CreateSExt(irb_.CreateTrunc(call_inst.getArgOperand(0), irb_.getJByteTy()),
                             irb_.getJIntTy());
    }

    //==- Exception --------------------------------------------------------==//
    case IntrinsicHelper::CatchTargets: {
      UpdatePhiInstruction(current_bb_, irb_.GetInsertBlock());
      llvm::SwitchInst* si = llvm::dyn_cast<llvm::SwitchInst>(call_inst.getNextNode());
      CHECK(si != NULL);
      irb_.CreateBr(si->getDefaultDest());
      si->eraseFromParent();
      return call_inst.getArgOperand(0);
    }

    //==- Unknown Cases ----------------------------------------------------==//
    case IntrinsicHelper::MaxIntrinsicId:
    case IntrinsicHelper::UnknownId:
    //default:
      // NOTE: "default" is intentionally commented so that C/C++ compiler will
      // give some warning on unmatched cases.
      // NOTE: We should not implement these cases.
      break;
  }
  UNIMPLEMENTED(FATAL) << "Unexpected GBC intrinsic: " << static_cast<int>(intr_id);
  return NULL;
}

} // anonymous namespace

namespace art {

namespace compiler_llvm {

llvm::FunctionPass*
CreateGBCExpanderPass(const IntrinsicHelper& intrinsic_helper, IRBuilder& irb) {
  return new GBCExpanderPass(intrinsic_helper, irb);
}

llvm::FunctionPass*
CreateGBCExpanderPass(const IntrinsicHelper& intrinsic_helper, IRBuilder& irb,
                      Compiler* compiler, OatCompilationUnit* oat_compilation_unit) {
  if (compiler != NULL) {
    return new GBCExpanderPass(intrinsic_helper, irb,
                               compiler, oat_compilation_unit);
  } else {
    return new GBCExpanderPass(intrinsic_helper, irb);
  }
}

} // namespace compiler_llvm
} // namespace art
