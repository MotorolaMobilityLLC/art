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

#include "atomic.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/timing_logger.h"
#include "gc_root.h"
#include "jni.h"
#include "object_callbacks.h"
#include "offline_profiling_info.h"
#include "thread_pool.h"

namespace art {

class ArtMethod;
struct RuntimeArgumentMap;

namespace jit {

class JitCodeCache;
class JitInstrumentationCache;
class JitOptions;

class Jit {
 public:
  static constexpr bool kStressMode = kIsDebugBuild;
  static constexpr size_t kDefaultCompileThreshold = kStressMode ? 2 : 10000;

  virtual ~Jit();
  static Jit* Create(JitOptions* options, std::string* error_msg);
  bool CompileMethod(ArtMethod* method, Thread* self, bool osr)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CreateInstrumentationCache(size_t compile_threshold,
                                  size_t warmup_threshold,
                                  size_t osr_threshold);
  void CreateThreadPool();
  const JitCodeCache* GetCodeCache() const {
    return code_cache_.get();
  }
  JitCodeCache* GetCodeCache() {
    return code_cache_.get();
  }
  void DeleteThreadPool();
  // Dump interesting info: #methods compiled, code vs data size, compile / verify cumulative
  // loggers.
  void DumpInfo(std::ostream& os);
  // Add a timing logger to cumulative_timings_.
  void AddTimingLogger(const TimingLogger& logger);
  JitInstrumentationCache* GetInstrumentationCache() const {
    return instrumentation_cache_.get();
  }

  // Starts the profile saver if the config options allow profile recording.
  // The profile will be stored in the specified `filename` and will contain
  // information collected from the given `code_paths` (a set of dex locations).
  // The `foreign_dex_profile_path` is the path where the saver will put the
  // profile markers for loaded dex files which are not owned by the application.
  // The `app_dir` is the application directory and is used to decide which
  // dex files belong to the application.
  void StartProfileSaver(const std::string& filename,
                         const std::vector<std::string>& code_paths,
                         const std::string& foreign_dex_profile_path,
                         const std::string& app_dir);
  void StopProfileSaver();

  void DumpForSigQuit(std::ostream& os) {
    DumpInfo(os);
  }

  static void NewTypeLoadedIfUsingJit(mirror::Class* type)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // If debug info generation is turned on then write the type information for types already loaded
  // into the specified class linker to the jit debug interface,
  void DumpTypeInfoForLoadedTypes(ClassLinker* linker);

  // Return whether we should try to JIT compiled code as soon as an ArtMethod is invoked.
  bool JitAtFirstUse();

  // Return whether we can invoke JIT code for `method`.
  bool CanInvokeCompiledCode(ArtMethod* method);

  // If an OSR compiled version is available for `method`,
  // and `dex_pc + dex_pc_offset` is an entry point of that compiled
  // version, this method will jump to the compiled code, let it run,
  // and return true afterwards. Return false otherwise.
  static bool MaybeDoOnStackReplacement(Thread* thread,
                                        ArtMethod* method,
                                        uint32_t dex_pc,
                                        int32_t dex_pc_offset,
                                        JValue* result)
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  Jit();
  bool LoadCompiler(std::string* error_msg);

  // JIT compiler
  void* jit_library_handle_;
  void* jit_compiler_handle_;
  void* (*jit_load_)(bool*);
  void (*jit_unload_)(void*);
  bool (*jit_compile_method_)(void*, ArtMethod*, Thread*, bool);
  void (*jit_types_loaded_)(void*, mirror::Class**, size_t count);

  // Performance monitoring.
  bool dump_info_on_shutdown_;
  CumulativeLogger cumulative_timings_;

  std::unique_ptr<jit::JitInstrumentationCache> instrumentation_cache_;
  std::unique_ptr<jit::JitCodeCache> code_cache_;

  bool save_profiling_info_;
  bool generate_debug_info_;

  DISALLOW_COPY_AND_ASSIGN(Jit);
};

class JitOptions {
 public:
  static JitOptions* CreateFromRuntimeArguments(const RuntimeArgumentMap& options);
  size_t GetCompileThreshold() const {
    return compile_threshold_;
  }
  size_t GetWarmupThreshold() const {
    return warmup_threshold_;
  }
  size_t GetOsrThreshold() const {
    return osr_threshold_;
  }
  size_t GetCodeCacheInitialCapacity() const {
    return code_cache_initial_capacity_;
  }
  size_t GetCodeCacheMaxCapacity() const {
    return code_cache_max_capacity_;
  }
  bool DumpJitInfoOnShutdown() const {
    return dump_info_on_shutdown_;
  }
  bool GetSaveProfilingInfo() const {
    return save_profiling_info_;
  }
  bool UseJIT() const {
    return use_jit_;
  }
  void SetUseJIT(bool b) {
    use_jit_ = b;
  }
  void SetSaveProfilingInfo(bool b) {
    save_profiling_info_ = b;
  }
  void SetJitAtFirstUse() {
    use_jit_ = true;
    compile_threshold_ = 0;
  }

 private:
  bool use_jit_;
  size_t code_cache_initial_capacity_;
  size_t code_cache_max_capacity_;
  size_t compile_threshold_;
  size_t warmup_threshold_;
  size_t osr_threshold_;
  bool dump_info_on_shutdown_;
  bool save_profiling_info_;

  JitOptions()
      : use_jit_(false),
        code_cache_initial_capacity_(0),
        code_cache_max_capacity_(0),
        compile_threshold_(0),
        dump_info_on_shutdown_(false),
        save_profiling_info_(false) { }

  DISALLOW_COPY_AND_ASSIGN(JitOptions);
};

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_H_
