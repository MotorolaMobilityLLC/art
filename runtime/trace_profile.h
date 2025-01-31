/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef ART_RUNTIME_TRACE_PROFILE_H_
#define ART_RUNTIME_TRACE_PROFILE_H_

#include <unordered_set>

#include "base/locks.h"
#include "base/macros.h"
#include "base/os.h"
#include "thread.h"
#include "thread_pool.h"

namespace art HIDDEN {

class ArtMethod;

// TODO(mythria): A randomly chosen value. Tune it later based on the number of
// entries required in the buffer.
static constexpr size_t kAlwaysOnTraceBufSize = 2048;

// The typical frequency at which the timestamp counters are updated is 24576000.
// 2^22 (4194304) corresponds to about 170ms at that frequency.
static constexpr size_t kLongRunningMethodThreshold = 1 << 22;

enum class LowOverheadTraceType {
  kLongRunningMethods,
  kAllMethods,
  kNone
};

class TraceData {
 public:
  explicit TraceData(LowOverheadTraceType trace_type)
      : trace_type_(trace_type),
        trace_end_time_(0),
        trace_data_lock_("Trace Data lock", LockLevel::kGenericBottomLock) {}

  LowOverheadTraceType GetTraceType() const {
    return trace_type_;
  }

  uint64_t GetTraceEndTime() const {
    return trace_end_time_;
  }

  void SetTraceEndTime(uint64_t end_time) {
    trace_end_time_ = end_time;
  }

  // Dumps events collected in long_running_methods_ and the information about
  // threads and methods into the output stream.
  void DumpData(std::ostringstream& os);

  void AppendToLongRunningMethods(const std::string& str) {
    MutexLock mu(Thread::Current(), trace_data_lock_);
    long_running_methods_.append(str);
  }

  void AddTracedMethods(std::unordered_set<ArtMethod*>& methods) {
    MutexLock mu(Thread::Current(), trace_data_lock_);
    traced_methods_.merge(methods);
  }

  void AddTracedMethod(ArtMethod* method) {
    MutexLock mu(Thread::Current(), trace_data_lock_);
    traced_methods_.insert(method);
  }

  void AddTracedThread(Thread* thread);

 private:
  // This is used to hold the initial methods on stack and also long running methods when there is a
  // buffer overflow.
  std::string long_running_methods_ GUARDED_BY(trace_data_lock_);

  LowOverheadTraceType trace_type_;

  uint64_t trace_end_time_;

  // These hold the methods and threads see so far. These are used to generate information about
  // the methods and threads.
  std::unordered_set<ArtMethod*> traced_methods_ GUARDED_BY(trace_data_lock_);

  // Threads might exit before we dump the data, so record thread id and name when we see a new
  // thread.
  std::unordered_map<size_t, std::string> traced_threads_ GUARDED_BY(trace_data_lock_);

  // Lock to synchronize access to traced_methods_, traced_threads_ and long_running_methods_ which
  // can be accessed simultaneously by multiple threads when running TraceDumpCheckpoint.
  Mutex trace_data_lock_;
};

class TraceDumpCheckpoint final : public Closure {
 public:
  explicit TraceDumpCheckpoint(TraceData* trace_data) : barrier_(0), trace_data_(trace_data) {}

  void Run(Thread* thread) override REQUIRES_SHARED(Locks::mutator_lock_);
  void WaitForThreadsToRunThroughCheckpoint(size_t threads_running_checkpoint);

 private:
  // The barrier to be passed through and for the requestor to wait upon.
  Barrier barrier_;

  // Trace data to record the data from each thread.
  TraceData* trace_data_;
};

// This class implements low-overhead tracing. This feature is available only when
// always_enable_profile_code is enabled which is a build time flag defined in
// build/flags/art-flags.aconfig. When this flag is enabled, AOT and JITed code can record events
// on each method execution. When a profile is started, method entry / exit events are recorded in
// a per-thread circular buffer. When requested the recorded events in the buffer are dumped into a
// file. The buffers are released when the profile is stopped.
class TraceProfiler {
 public:
  // Starts profiling by allocating a per-thread buffer for all the threads.
  static void Start();

  // Starts recording long running methods. A long running method means any
  // method that executes for more than kLongRunningMethodDuration.
  static void StartTraceLongRunningMethods(uint64_t trace_duration_ns);

  // Releases all the buffers.
  static void Stop();

  // Dumps the recorded events in the buffer from all threads in the specified file.
  static void Dump(int fd);
  static void Dump(const char* trace_filename);

  // Get the long running methods as a string. This is used in the sigquit handler to record
  // information about long running methods.
  static std::string GetLongRunningMethodsString();

  // Called when thread is exiting to release the allocated buffer.
  static void ReleaseThreadBuffer(Thread* self) REQUIRES(Locks::trace_lock_);

  static bool IsTraceProfileInProgress() REQUIRES(Locks::trace_lock_);

  // Allocates a buffer for the specified thread.
  static void AllocateBuffer(Thread* thread);

  // Used to flush the long running method buffer when it is full. This method flushes all methods
  // that have already seen an exit and records them into a string. If we don't have sufficient free
  // entries after this processing (for example: if we have a really deep call stack) then we record
  // a placeholder method exit event and flush all events.
  static void FlushBufferAndRecordTraceEvent(ArtMethod* method, Thread* thread, bool is_entry);

  static LowOverheadTraceType GetTraceType();

  // Callback that is run when the specified duration for the long running trace has elapsed. If the
  // trace is still running then tracing is stopped and all buffers are released. If the trace
  // has already stopped then this request is ignored.
  static void TraceTimeElapsed();

 private:
  // Starts tracing.
  static void Start(LowOverheadTraceType trace_type, uint64_t trace_duration_ns);

  // Dumps the tracing data into the specified trace_file
  static void Dump(std::unique_ptr<File>&& trace_file);

  // Stops tracing.
  static void StopLocked() REQUIRES(Locks::trace_lock_);

  // Returns the information about long running methods as a string. Used both by Dump
  // and GetLongRunningMethodsString.
  static std::string GetLongRunningMethodsStringLocked() REQUIRES(Locks::trace_lock_);

  // Dumps the events from all threads into the trace_file.
  static void DumpTrace(std::unique_ptr<File>&& trace_file) REQUIRES(Locks::trace_lock_);

  // Dumps the long running methods from all threads into the trace_file.
  static void DumpLongRunningMethods(std::unique_ptr<File>&& trace_file)
      REQUIRES(Locks::trace_lock_);

  // This method goes over all the events in the thread_buffer and stores the encoded event in the
  // buffer. It returns the pointer to the next free entry in the buffer.
  // This also records the ArtMethods from the events in the thread_buffer in a set. This set is
  // used to dump the information about the methods once buffers from all threads have been
  // processed.
  static uint8_t* DumpBuffer(uint32_t thread_id,
                             uintptr_t* thread_buffer,
                             uint8_t* buffer /* out */,
                             std::unordered_set<ArtMethod*>& methods /* out */);

  // Dumps all the events in the buffer into the file. Also records the ArtMethods from the events
  // which is then used to record information about these methods.
  static void DumpLongRunningMethodBuffer(uint32_t thread_id,
                                          uintptr_t* thread_buffer,
                                          uintptr_t* end_buffer,
                                          std::unordered_set<ArtMethod*>& methods /* out */,
                                          std::ostringstream& os);

  static bool profile_in_progress_ GUARDED_BY(Locks::trace_lock_);

  static TraceData* trace_data_ GUARDED_BY(Locks::trace_lock_);

  friend class TraceDumpCheckpoint;
  DISALLOW_COPY_AND_ASSIGN(TraceProfiler);
};

}  // namespace art

#endif  // ART_RUNTIME_TRACE_PROFILE_H_
