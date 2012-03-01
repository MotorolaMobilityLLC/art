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

#ifndef ART_SRC_COMPILER_LLVM_LLVM_COMPILER_H_
#define ART_SRC_COMPILER_LLVM_LLVM_COMPILER_H_

#include "constants.h"
#include "dex_file.h"
#include "macros.h"

#include <UniquePtr.h>

#include <string>

namespace art {
  class ClassLoader;
  class CompiledInvokeStub;
  class CompiledMethod;
  class Compiler;
  class OatCompilationUnit;
}


namespace llvm {
  class Function;
  class LLVMContext;
  class Module;
  class PointerType;
  class StructType;
  class Type;
}


namespace art {
namespace compiler_llvm {

class CompilationUnit;
class IRBuilder;

class CompilerLLVM {
 public:
  CompilerLLVM(Compiler* compiler, InstructionSet insn_set);

  ~CompilerLLVM();

  void MaterializeIfThresholdReached();

  void MaterializeRemainder();

  Compiler* GetCompiler() const {
    return compiler_;
  }

  InstructionSet GetInstructionSet() const {
    return insn_set_;
  }

  void SetElfFileName(std::string const& filename) {
    elf_filename_ = filename;
  }

  void SetBitcodeFileName(std::string const& filename) {
    bitcode_filename_ = filename;
  }

  CompiledMethod* CompileDexMethod(OatCompilationUnit* oat_compilation_unit);

  CompiledMethod* CompileNativeMethod(OatCompilationUnit* oat_compilation_unit);

  CompiledInvokeStub* CreateInvokeStub(bool is_static, char const *shorty);

 private:
  void EnsureCompilationUnit(MutexLock& GUARD);

  void Materialize(MutexLock& GUARD);

  bool IsBitcodeFileNameAvailable() const {
    return !bitcode_filename_.empty();
  }

  Compiler* compiler_;

 public:
  Mutex compiler_lock_;

 private:
  InstructionSet insn_set_;

  CompilationUnit* curr_cunit_;

  std::vector<CompilationUnit*> cunits_;

  std::string elf_filename_;

  std::string bitcode_filename_;

  DISALLOW_COPY_AND_ASSIGN(CompilerLLVM);
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_LLVM_COMPILER_H_
