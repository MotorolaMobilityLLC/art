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

namespace art {
namespace compiler_llvm {

using namespace runtime_support;


void RuntimeSupportBuilderARM::TargetOptimizeRuntimeSupport() {
  {
    Function* func = GetRuntimeSupportFunction(GetCurrentThread);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    InlineAsm* get_r9 = InlineAsm::get(func->getFunctionType(), "mov $0, r9", "=r", false);
    CallInst* r9 = irb_.CreateCall(get_r9);
    r9->setOnlyReadsMemory();
    irb_.CreateRet(r9);

    VERIFY_LLVM_FUNCTION(*func);
  }

  {
    Function* func = GetRuntimeSupportFunction(SetCurrentThread);
    MakeFunctionInline(func);
    BasicBlock* basic_block = BasicBlock::Create(context_, "entry", func);
    irb_.SetInsertPoint(basic_block);

    InlineAsm* set_r9 = InlineAsm::get(func->getFunctionType(), "mov r9, $0", "r", true);
    Value* thread = func->arg_begin();
    irb_.CreateCall(set_r9, thread);
    irb_.CreateRetVoid();

    VERIFY_LLVM_FUNCTION(*func);
  }
}


} // namespace compiler_llvm
} // namespace art
