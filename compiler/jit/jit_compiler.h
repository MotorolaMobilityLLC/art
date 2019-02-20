/*
 * Copyright 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_JIT_JIT_COMPILER_H_
#define ART_COMPILER_JIT_JIT_COMPILER_H_

#include "base/mutex.h"

namespace art {

class ArtMethod;
class CompiledMethod;
class Compiler;
class CompilerOptions;
class Thread;

namespace jit {

class JitLogger;

class JitCompiler {
 public:
  static JitCompiler* Create();
  virtual ~JitCompiler();

  // Compilation entrypoint. Returns whether the compilation succeeded.
  bool CompileMethod(Thread* self, ArtMethod* method, bool baseline, bool osr)
      REQUIRES_SHARED(Locks::mutator_lock_);

  const CompilerOptions& GetCompilerOptions() const {
    return *compiler_options_.get();
  }

  void ParseCompilerOptions();

 private:
  std::unique_ptr<CompilerOptions> compiler_options_;
  std::unique_ptr<Compiler> compiler_;
  std::unique_ptr<JitLogger> jit_logger_;

  JitCompiler();

  DISALLOW_COPY_AND_ASSIGN(JitCompiler);
};

}  // namespace jit
}  // namespace art

#endif  // ART_COMPILER_JIT_JIT_COMPILER_H_
