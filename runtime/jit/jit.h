/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_JIT_JIT_H_
#define ART_RUNTIME_JIT_JIT_H_

#include <unordered_map>

#include "instrumentation.h"

#include "atomic.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "gc_root.h"
#include "jni.h"
#include "object_callbacks.h"
#include "thread_pool.h"

namespace art {

class CompilerCallbacks;
struct RuntimeArgumentMap;

namespace jit {

class JitCodeCache;
class JitInstrumentationCache;
class JitOptions;

class Jit {
 public:
  static constexpr bool kStressMode = kIsDebugBuild;
  static constexpr size_t kDefaultCompileThreshold = kStressMode ? 1 : 1000;

  virtual ~Jit();
  static Jit* Create(JitOptions* options, std::string* error_msg);
  bool CompileMethod(mirror::ArtMethod* method, Thread* self)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void CreateInstrumentationCache(size_t compile_threshold);
  void CreateThreadPool();
  CompilerCallbacks* GetCompilerCallbacks() {
    return compiler_callbacks_;
  }
  const JitCodeCache* GetCodeCache() const {
    return code_cache_.get();
  }
  JitCodeCache* GetCodeCache() {
    return code_cache_.get();
  }
  void DeleteThreadPool();

 private:
  Jit();
  bool LoadCompiler(std::string* error_msg);

  // JIT compiler
  void* jit_library_handle_;
  void* jit_compiler_handle_;
  void* (*jit_load_)(CompilerCallbacks**);
  void (*jit_unload_)(void*);
  bool (*jit_compile_method_)(void*, mirror::ArtMethod*, Thread*);

  std::unique_ptr<jit::JitInstrumentationCache> instrumentation_cache_;
  std::unique_ptr<jit::JitCodeCache> code_cache_;
  CompilerCallbacks* compiler_callbacks_;  // Owned by the jit compiler.
};

class JitOptions {
 public:
  static JitOptions* CreateFromRuntimeArguments(const RuntimeArgumentMap& options);
  size_t GetCompileThreshold() const {
    return compile_threshold_;
  }
  size_t GetCodeCacheCapacity() const {
    return code_cache_capacity_;
  }

 private:
  size_t code_cache_capacity_;
  size_t compile_threshold_;

  JitOptions() : code_cache_capacity_(0), compile_threshold_(0) {
  }
};

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_H_
