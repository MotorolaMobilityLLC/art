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

#ifndef ART_RUNTIME_JIT_JIT_INSTRUMENTATION_H_
#define ART_RUNTIME_JIT_JIT_INSTRUMENTATION_H_

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
namespace mirror {
  class ArtField;
  class ArtMethod;
  class Class;
  class Object;
  class Throwable;
}  // namespace mirror
union JValue;
class Thread;
class ThrowLocation;

namespace jit {

// Keeps track of which methods are hot.
class JitInstrumentationCache {
 public:
  explicit JitInstrumentationCache(size_t hot_method_threshold);
  void AddSamples(Thread* self, mirror::ArtMethod* method, size_t samples)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SignalCompiled(Thread* self, mirror::ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void CreateThreadPool();
  void DeleteThreadPool();

 private:
  Mutex lock_;
  std::unordered_map<jmethodID, size_t> samples_;
  size_t hot_method_threshold_;
  std::unique_ptr<ThreadPool> thread_pool_;
};

class JitInstrumentationListener : public instrumentation::InstrumentationListener {
 public:
  explicit JitInstrumentationListener(JitInstrumentationCache* cache);

  virtual void MethodEntered(Thread* thread, mirror::Object* /*this_object*/,
                             mirror::ArtMethod* method, uint32_t /*dex_pc*/)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    instrumentation_cache_->AddSamples(thread, method, 1);
  }
  virtual void MethodExited(Thread* /*thread*/, mirror::Object* /*this_object*/,
                            mirror::ArtMethod* /*method*/, uint32_t /*dex_pc*/,
                            const JValue& /*return_value*/)
      OVERRIDE { }
  virtual void MethodUnwind(Thread* /*thread*/, mirror::Object* /*this_object*/,
                            mirror::ArtMethod* /*method*/, uint32_t /*dex_pc*/) OVERRIDE { }
  virtual void FieldRead(Thread* /*thread*/, mirror::Object* /*this_object*/,
                         mirror::ArtMethod* /*method*/, uint32_t /*dex_pc*/,
                         mirror::ArtField* /*field*/) OVERRIDE { }
  virtual void FieldWritten(Thread* /*thread*/, mirror::Object* /*this_object*/,
                            mirror::ArtMethod* /*method*/, uint32_t /*dex_pc*/,
                            mirror::ArtField* /*field*/, const JValue& /*field_value*/)
      OVERRIDE { }
  virtual void ExceptionCaught(Thread* /*thread*/, const ThrowLocation& /*throw_location*/,
                               mirror::ArtMethod* /*catch_method*/, uint32_t /*catch_dex_pc*/,
                               mirror::Throwable* /*exception_object*/) OVERRIDE { }

  virtual void DexPcMoved(Thread* /*self*/, mirror::Object* /*this_object*/,
                          mirror::ArtMethod* /*method*/, uint32_t /*new_dex_pc*/) OVERRIDE { }

  // We only care about how many dex instructions were executed in the Jit.
  virtual void BackwardBranch(Thread* thread, mirror::ArtMethod* method, int32_t dex_pc_offset)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK_LE(dex_pc_offset, 0);
    instrumentation_cache_->AddSamples(thread, method, 1);
  }

 private:
  JitInstrumentationCache* const instrumentation_cache_;
};

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_INSTRUMENTATION_H_
