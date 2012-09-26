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

#include "runtime_support_builder_arm.h"

#include "ir_builder.h"
#include "thread.h"
#include "utils_llvm.h"

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/InlineAsm.h>
#include <llvm/Module.h>
#include <llvm/Type.h>

#include <vector>

using namespace llvm;

namespace {

char LDRSTRSuffixByType(art::compiler_llvm::IRBuilder& irb, llvm::Type* type) {
  int width = type->isPointerTy() ?
              irb.getSizeOfPtrEquivInt()*8 :
              llvm::cast<IntegerType>(type)->getBitWidth();
  switch (width) {
    case 8:  return 'b';
    case 16: return 'h';
    case 32: return ' ';
    default:
      LOG(FATAL) << "Unsupported width: " << width;
      return ' ';
  }
}

} // namespace

namespace art {
namespace compiler_llvm {

/* Thread */

llvm::Value* RuntimeSupportBuilderARM::EmitGetCurrentThread() {
  Function* ori_func = GetRuntimeSupportFunction(runtime_support::GetCurrentThread);
  InlineAsm* func = InlineAsm::get(ori_func->getFunctionType(), "mov $0, r9", "=r", false);
  CallInst* thread = irb_.CreateCall(func);
  thread->setDoesNotAccessMemory();
  irb_.SetTBAA(thread, kTBAAConstJObject);
  return thread;
}

llvm::Value* RuntimeSupportBuilderARM::EmitLoadFromThreadOffset(int64_t offset, llvm::Type* type,
                                                                TBAASpecialType s_ty) {
  FunctionType* func_ty = FunctionType::get(/*Result=*/type,
                                            /*isVarArg=*/false);
  std::string inline_asm(StringPrintf("ldr%c $0, [r9, #%d]",
                                      LDRSTRSuffixByType(irb_, type),
                                      static_cast<int>(offset)));
  InlineAsm* func = InlineAsm::get(func_ty, inline_asm, "=r", true);
  CallInst* result = irb_.CreateCall(func);
  result->setOnlyReadsMemory();
  irb_.SetTBAA(result, s_ty);
  return result;
}

void RuntimeSupportBuilderARM::EmitStoreToThreadOffset(int64_t offset, llvm::Value* value,
                                                       TBAASpecialType s_ty) {
  FunctionType* func_ty = FunctionType::get(/*Result=*/Type::getVoidTy(context_),
                                            /*Params=*/value->getType(),
                                            /*isVarArg=*/false);
  std::string inline_asm(StringPrintf("str%c $0, [r9, #%d]",
                                      LDRSTRSuffixByType(irb_, value->getType()),
                                      static_cast<int>(offset)));
  InlineAsm* func = InlineAsm::get(func_ty, inline_asm, "r", true);
  CallInst* call_inst = irb_.CreateCall(func, value);
  irb_.SetTBAA(call_inst, s_ty);
}

llvm::Value*
RuntimeSupportBuilderARM::EmitSetCurrentThread(llvm::Value* thread) {
  // Separate to two InlineAsm: The first one produces the return value, while the second,
  // sets the current thread.
  // LLVM can delete the first one if the caller in LLVM IR doesn't use the return value.
  //
  // Here we don't call EmitGetCurrentThread, because we mark it as DoesNotAccessMemory and
  // ConstJObject. We denote side effect to "true" below instead, so LLVM won't
  // reorder these instructions incorrectly.
  Function* ori_func = GetRuntimeSupportFunction(runtime_support::GetCurrentThread);
  InlineAsm* func = InlineAsm::get(ori_func->getFunctionType(), "mov $0, r9", "=r", true);
  CallInst* old_thread_register = irb_.CreateCall(func);
  old_thread_register->setOnlyReadsMemory();

  FunctionType* func_ty = FunctionType::get(/*Result=*/Type::getVoidTy(context_),
                                            /*Params=*/irb_.getJObjectTy(),
                                            /*isVarArg=*/false);
  func = InlineAsm::get(func_ty, "mov r9, $0", "r", true);
  irb_.CreateCall(func, thread);
  return old_thread_register;
}


/* Monitor */

void RuntimeSupportBuilderARM::EmitLockObject(llvm::Value* object) {
  RuntimeSupportBuilder::EmitLockObject(object);
  FunctionType* func_ty = FunctionType::get(/*Result=*/Type::getVoidTy(context_),
                                            /*isVarArg=*/false);
  InlineAsm* func = InlineAsm::get(func_ty, "dmb sy", "", true);
  irb_.CreateCall(func);
}

void RuntimeSupportBuilderARM::EmitUnlockObject(llvm::Value* object) {
  RuntimeSupportBuilder::EmitUnlockObject(object);
  FunctionType* func_ty = FunctionType::get(/*Result=*/Type::getVoidTy(context_),
                                            /*isVarArg=*/false);
  InlineAsm* func = InlineAsm::get(func_ty, "dmb sy", "", true);
  irb_.CreateCall(func);
}

} // namespace compiler_llvm
} // namespace art
