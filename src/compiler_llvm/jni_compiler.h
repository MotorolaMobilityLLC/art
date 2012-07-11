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

#ifndef ART_SRC_COMPILER_LLVM_JNI_COMPILER_H_
#define ART_SRC_COMPILER_LLVM_JNI_COMPILER_H_

#include <stdint.h>

namespace art {
  class ClassLinker;
  class ClassLoader;
  class CompiledMethod;
  class Compiler;
  class DexCache;
  class DexFile;
  class Method;
  class OatCompilationUnit;
}

namespace llvm {
  class AllocaInst;
  class Function;
  class FunctionType;
  class BasicBlock;
  class LLVMContext;
  class Module;
  class Type;
  class Value;
}

namespace art {
namespace compiler_llvm {

class CompilationUnit;
class IRBuilder;

class JniCompiler {
 public:
  JniCompiler(CompilationUnit* cunit,
              Compiler const& compiler,
              OatCompilationUnit* oat_compilation_unit);

  CompiledMethod* Compile();

 private:
  void CreateFunction();

  llvm::FunctionType* GetFunctionType(uint32_t method_idx,
                                      bool is_static, bool is_target_function);

 private:
  CompilationUnit* cunit_;
  const Compiler* compiler_;

  llvm::Module* module_;
  llvm::LLVMContext* context_;
  IRBuilder& irb_;

  OatCompilationUnit* oat_compilation_unit_;

  uint32_t access_flags_;
  uint32_t method_idx_;
  ClassLinker * class_linker_;
  const ClassLoader* class_loader_;
  const DexCache* dex_cache_;
  const DexFile* dex_file_;
  Method* method_;

  llvm::Function* func_;
  uint16_t elf_func_idx_;
};


} // namespace compiler_llvm
} // namespace art


#endif // ART_SRC_COMPILER_LLVM_JNI_COMPILER_H_
