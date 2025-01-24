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

#include "trace_profile.h"

#include "android-base/stringprintf.h"
#include "arch/context.h"
#include "art_method-inl.h"
#include "base/leb128.h"
#include "base/mutex.h"
#include "base/unix_file/fd_file.h"
#include "com_android_art_flags.h"
#include "dex/descriptors_names.h"
#include "gc/task_processor.h"
#include "oat/oat_quick_method_header.h"
#include "runtime.h"
#include "stack.h"
#include "thread-current-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"
#include "trace_common.h"

namespace art_flags = com::android::art::flags;

namespace art HIDDEN {

using android::base::StringPrintf;

// This specifies the maximum number of bits we need for encoding one entry. Each entry just
// consists of a SLEB encoded value of method and action encodig which is a maximum of
// sizeof(uintptr_t).
static constexpr size_t kMaxBytesPerTraceEntry = sizeof(uintptr_t);

static constexpr size_t kMaxEntriesAfterFlush = kAlwaysOnTraceBufSize / 2;

// We don't handle buffer overflows when processing the raw trace entries. We have a maximum of
// kAlwaysOnTraceBufSize raw entries and we need a maximum of kMaxBytesPerTraceEntry to encode
// each entry. To avoid overflow, we ensure that there are at least kMinBufSizeForEncodedData
// bytes free space in the buffer.
static constexpr size_t kMinBufSizeForEncodedData = kAlwaysOnTraceBufSize * kMaxBytesPerTraceEntry;

static constexpr size_t kProfileMagicValue = 0x4C4F4D54;

// TODO(mythria): 10 is a randomly chosen value. Tune it if required.
static constexpr size_t kBufSizeForEncodedData = kMinBufSizeForEncodedData * 10;

static constexpr size_t kAlwaysOnTraceHeaderSize = 8;

bool TraceProfiler::profile_in_progress_ = false;

int TraceProfiler::num_trace_stop_tasks_ = 0;

TraceData* TraceProfiler::trace_data_ = nullptr;

void TraceData::AddTracedThread(Thread* thread) {
  MutexLock mu(Thread::Current(), trace_data_lock_);
  size_t thread_id = thread->GetTid();
  if (traced_threads_.find(thread_id) != traced_threads_.end()) {
    return;
  }

  std::string thread_name;
  thread->GetThreadName(thread_name);
  traced_threads_.emplace(thread_id, thread_name);
}

void TraceProfiler::AllocateBuffer(Thread* thread) {
  if (!art_flags::always_enable_profile_code()) {
    return;
  }

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::trace_lock_);
  if (!profile_in_progress_) {
    return;
  }

  auto buffer = new uintptr_t[kAlwaysOnTraceBufSize];
  memset(buffer, 0, kAlwaysOnTraceBufSize * sizeof(uintptr_t));
  thread->SetMethodTraceBuffer(buffer, kAlwaysOnTraceBufSize);
}

LowOverheadTraceType TraceProfiler::GetTraceType() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  // LowOverhead trace entry points are configured based on the trace type. When trace_data_ is null
  // then there is no low overhead tracing running, so we use nop entry points.
  if (trace_data_ == nullptr) {
    return LowOverheadTraceType::kNone;
  }

  return trace_data_->GetTraceType();
}

namespace {
void RecordMethodsOnThreadStack(Thread* thread, uintptr_t* method_trace_buffer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  struct MethodEntryStackVisitor final : public StackVisitor {
    MethodEntryStackVisitor(Thread* thread_in, Context* context)
        : StackVisitor(thread_in, context, StackVisitor::StackWalkKind::kSkipInlinedFrames) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      ArtMethod* m = GetMethod();
      if (m != nullptr && !m->IsRuntimeMethod()) {
        if (GetCurrentShadowFrame() != nullptr) {
          // TODO(mythria): Support low-overhead tracing for the switch interpreter.
        } else {
          const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
          if (method_header == nullptr) {
            // TODO(mythria): Consider low-overhead tracing support for the GenericJni stubs.
          } else {
            // Ignore nterp methods. We don't support recording trace events in nterp.
            if (!method_header->IsNterpMethodHeader()) {
              stack_methods_.push_back(m);
            }
          }
        }
      }
      return true;
    }

    std::vector<ArtMethod*> stack_methods_;
  };

  std::unique_ptr<Context> context(Context::Create());
  MethodEntryStackVisitor visitor(thread, context.get());
  visitor.WalkStack(true);

  // Create method entry events for all methods currently on the thread's stack.
  uint64_t init_time = TimestampCounter::GetMicroTime(TimestampCounter::GetTimestamp());
  // Set the lsb to 0 to indicate method entry.
  init_time = init_time & ~1;
  std::ostringstream os;
  os << "Thread:" << thread->GetTid() << "\n";
  size_t index = kAlwaysOnTraceBufSize - 1;
  for (auto smi = visitor.stack_methods_.rbegin(); smi != visitor.stack_methods_.rend(); smi++) {
    method_trace_buffer[index--] = reinterpret_cast<uintptr_t>(*smi);
    method_trace_buffer[index--] = init_time;

    if (index < kMaxEntriesAfterFlush) {
      // To keep the implementation simple, ignore methods deep down the stack. If the call stack
      // unwinds beyond this point then we will see method exits without corresponding method
      // entries.
      break;
    }
  }

  // Record a placeholder method exit event into the buffer so we record method exits for the
  // methods that are currently on stack.
  method_trace_buffer[index] = 0x1;
  thread->SetMethodTraceBufferCurrentEntry(index);
}

// Records the thread and method info.
void DumpThreadMethodInfo(const std::unordered_map<size_t, std::string>& traced_threads,
                          const std::unordered_set<ArtMethod*>& traced_methods,
                          std::ostringstream& os) REQUIRES_SHARED(Locks::mutator_lock_) {
  // Dump data about thread information.
  os << "\n*threads\n";
  for (const auto& it : traced_threads) {
    os << it.first << "\t" << it.second << "\n";
  }

  // Dump data about method information.
  os << "*methods\n";
  for (ArtMethod* method : traced_methods) {
    ArtMethod* method_ptr = reinterpret_cast<ArtMethod*>(method);
    os << method_ptr << "\t" << GetMethodInfoLine(method);
  }

  os << "*end";
}
}  // namespace

class TraceStopTask : public gc::HeapTask {
 public:
  explicit TraceStopTask(uint64_t target_run_time) : gc::HeapTask(target_run_time) {}

  void Run([[maybe_unused]] Thread* self) override { TraceProfiler::TraceTimeElapsed(); }
};

class TraceStartCheckpoint final : public Closure {
 public:
  explicit TraceStartCheckpoint(LowOverheadTraceType type) : trace_type_(type), barrier_(0) {}

  void Run(Thread* thread) override REQUIRES_SHARED(Locks::mutator_lock_) {
    auto buffer = new uintptr_t[kAlwaysOnTraceBufSize];

    if (trace_type_ == LowOverheadTraceType::kLongRunningMethods) {
      // Record methods that are currently on stack.
      RecordMethodsOnThreadStack(thread, buffer);
      thread->UpdateTlsLowOverheadTraceEntrypoints(LowOverheadTraceType::kLongRunningMethods);
    } else {
      memset(buffer, 0, kAlwaysOnTraceBufSize * sizeof(uintptr_t));
      thread->UpdateTlsLowOverheadTraceEntrypoints(LowOverheadTraceType::kAllMethods);
    }
    thread->SetMethodTraceBuffer(buffer, kAlwaysOnTraceBufSize);
    barrier_.Pass(Thread::Current());
  }

  void WaitForThreadsToRunThroughCheckpoint(size_t threads_running_checkpoint) {
    Thread* self = Thread::Current();
    ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
    barrier_.Increment(self, threads_running_checkpoint);
  }

 private:
  LowOverheadTraceType trace_type_;

  // The barrier to be passed through and for the requestor to wait upon.
  Barrier barrier_;

  DISALLOW_COPY_AND_ASSIGN(TraceStartCheckpoint);
};

void TraceProfiler::Start(LowOverheadTraceType trace_type, uint64_t trace_duration_ns) {
  if (!art_flags::always_enable_profile_code()) {
    LOG(ERROR) << "Feature not supported. Please build with ART_ALWAYS_ENABLE_PROFILE_CODE.";
    return;
  }

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::trace_lock_);
  if (profile_in_progress_) {
    LOG(ERROR) << "Profile already in progress. Ignoring this request";
    return;
  }

  if (Trace::IsTracingEnabledLocked()) {
    LOG(ERROR) << "Cannot start a profile when method tracing is in progress";
    return;
  }

  TimestampCounter::InitializeTimestampCounters();
  profile_in_progress_ = true;
  trace_data_ = new TraceData(trace_type);

  Runtime* runtime = Runtime::Current();
  TraceStartCheckpoint checkpoint(trace_type);
  size_t threads_running_checkpoint = runtime->GetThreadList()->RunCheckpoint(&checkpoint);
  if (threads_running_checkpoint != 0) {
    checkpoint.WaitForThreadsToRunThroughCheckpoint(threads_running_checkpoint);
  }

  if (trace_type == LowOverheadTraceType::kLongRunningMethods) {
    // Add a Task that stops the tracing after trace_duration.
    runtime->GetHeap()->AddHeapTask(new TraceStopTask(NanoTime() + trace_duration_ns));
    num_trace_stop_tasks_++;
  }
}

void TraceProfiler::Start() {
  TraceProfiler::Start(LowOverheadTraceType::kAllMethods, /* trace_duration_ns= */ 0);
}

void TraceProfiler::Stop() {
  if (!art_flags::always_enable_profile_code()) {
    LOG(ERROR) << "Feature not supported. Please build with ART_ALWAYS_ENABLE_PROFILE_CODE.";
    return;
  }

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::trace_lock_);
  TraceProfiler::StopLocked();
}

void TraceProfiler::StopLocked() {
  if (!profile_in_progress_) {
    LOG(ERROR) << "No Profile in progress but a stop was requested";
    return;
  }

  static FunctionClosure reset_buffer([](Thread* thread) {
    auto buffer = thread->GetMethodTraceBuffer();
    if (buffer != nullptr) {
      delete[] buffer;
      thread->SetMethodTraceBuffer(/* buffer= */ nullptr, /* offset= */ 0);
    }
    thread->UpdateTlsLowOverheadTraceEntrypoints(LowOverheadTraceType::kNone);
  });

  Runtime::Current()->GetThreadList()->RunCheckpoint(&reset_buffer);
  profile_in_progress_ = false;
  DCHECK_NE(trace_data_, nullptr);
  delete trace_data_;
  trace_data_ = nullptr;
}



uint8_t* TraceProfiler::DumpBuffer(uint32_t thread_id,
                                   uintptr_t* method_trace_entries,
                                   uint8_t* buffer,
                                   std::unordered_set<ArtMethod*>& methods) {
  // Encode header at the end once we compute the number of records.
  uint8_t* curr_buffer_ptr = buffer + kAlwaysOnTraceHeaderSize;

  int num_records = 0;
  uintptr_t prev_method_action_encoding = 0;
  int prev_action = -1;
  for (size_t i = kAlwaysOnTraceBufSize - 1; i > 0; i-=1) {
    uintptr_t method_action_encoding = method_trace_entries[i];
    // 0 value indicates the rest of the entries are empty.
    if (method_action_encoding == 0) {
      break;
    }

    int action = method_action_encoding & ~kMaskTraceAction;
    int64_t diff;
    if (action == TraceAction::kTraceMethodEnter) {
      diff = method_action_encoding - prev_method_action_encoding;

      ArtMethod* method = reinterpret_cast<ArtMethod*>(method_action_encoding & kMaskTraceAction);
      methods.insert(method);
    } else {
      // On a method exit, we don't record the information about method. We just need a 1 in the
      // lsb and the method information can be derived from the last method that entered. To keep
      // the encoded value small just add the smallest value to make the lsb one.
      if (prev_action == TraceAction::kTraceMethodExit) {
        diff = 0;
      } else {
        diff = 1;
      }
    }
    curr_buffer_ptr = EncodeSignedLeb128(curr_buffer_ptr, diff);
    num_records++;
    prev_method_action_encoding = method_action_encoding;
    prev_action = action;
  }

  // Fill in header information:
  // 1 byte of header identifier
  // 4 bytes of thread_id
  // 3 bytes of number of records
  buffer[0] = kEntryHeaderV2;
  Append4LE(buffer + 1, thread_id);
  Append3LE(buffer + 5, num_records);
  return curr_buffer_ptr;
}

void TraceProfiler::Dump(int fd) {
  if (!art_flags::always_enable_profile_code()) {
    LOG(ERROR) << "Feature not supported. Please build with ART_ALWAYS_ENABLE_PROFILE_CODE.";
    return;
  }

  std::unique_ptr<File> trace_file(new File(fd, /*check_usage=*/true));
  Dump(std::move(trace_file));
}

void TraceProfiler::Dump(const char* filename) {
  if (!art_flags::always_enable_profile_code()) {
    LOG(ERROR) << "Feature not supported. Please build with ART_ALWAYS_ENABLE_PROFILE_CODE.";
    return;
  }

  std::unique_ptr<File> trace_file(OS::CreateEmptyFileWriteOnly(filename));
  if (trace_file == nullptr) {
    PLOG(ERROR) << "Unable to open trace file " << filename;
    return;
  }

  Dump(std::move(trace_file));
}

void TraceProfiler::Dump(std::unique_ptr<File>&& trace_file) {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  if (!profile_in_progress_) {
    LOG(ERROR) << "No Profile in progress. Nothing to dump.";
    return;
  }

  if (trace_data_->GetTraceType() == LowOverheadTraceType::kAllMethods) {
    DumpTrace(std::move(trace_file));
  } else {
    DumpLongRunningMethods(std::move(trace_file));
  }
}

void TraceProfiler::DumpTrace(std::unique_ptr<File>&& trace_file) {
  Thread* self = Thread::Current();
  std::unordered_set<ArtMethod*> traced_methods;
  std::unordered_map<size_t, std::string> traced_threads;
  uint8_t* buffer_ptr = new uint8_t[kBufSizeForEncodedData];
  uint8_t* curr_buffer_ptr = buffer_ptr;

  // Add a header for the trace: 4-bits of magic value and 2-bits for the version.
  Append4LE(curr_buffer_ptr, kProfileMagicValue);
  Append2LE(curr_buffer_ptr + 4, /*trace_version=*/ 1);
  curr_buffer_ptr += 6;

  ScopedSuspendAll ssa(__FUNCTION__);
  MutexLock tl(self, *Locks::thread_list_lock_);
  for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
    auto method_trace_entries = thread->GetMethodTraceBuffer();
    if (method_trace_entries == nullptr) {
      continue;
    }

    std::string thread_name;
    thread->GetThreadName(thread_name);
    traced_threads.emplace(thread->GetTid(), thread_name);

    size_t offset = curr_buffer_ptr - buffer_ptr;
    if (offset >= kMinBufSizeForEncodedData) {
      if (!trace_file->WriteFully(buffer_ptr, offset)) {
        PLOG(WARNING) << "Failed streaming a tracing event.";
      }
      curr_buffer_ptr = buffer_ptr;
    }
    curr_buffer_ptr =
        DumpBuffer(thread->GetTid(), method_trace_entries, curr_buffer_ptr, traced_methods);
    // Reset the buffer and continue profiling. We need to set the buffer to
    // zeroes, since we use a circular buffer and detect empty entries by
    // checking for zeroes.
    memset(method_trace_entries, 0, kAlwaysOnTraceBufSize * sizeof(uintptr_t));
    // Reset the current pointer.
    thread->SetMethodTraceBufferCurrentEntry(kAlwaysOnTraceBufSize);
  }

  // Write any remaining data to file and close the file.
  if (curr_buffer_ptr != buffer_ptr) {
    if (!trace_file->WriteFully(buffer_ptr, curr_buffer_ptr - buffer_ptr)) {
      PLOG(WARNING) << "Failed streaming a tracing event.";
    }
  }

  std::ostringstream os;
  DumpThreadMethodInfo(traced_threads, traced_methods, os);
  std::string info = os.str();
  if (!trace_file->WriteFully(info.c_str(), info.length())) {
    PLOG(WARNING) << "Failed writing information to file";
  }

  if (!trace_file->Close()) {
    PLOG(WARNING) << "Failed to close file.";
  }
}

void TraceProfiler::ReleaseThreadBuffer(Thread* self) {
  if (!IsTraceProfileInProgress()) {
    return;
  }
  // TODO(mythria): Maybe it's good to cache these and dump them when requested. For now just
  // relese the buffer when a thread is exiting.
  auto buffer = self->GetMethodTraceBuffer();
  delete[] buffer;
  self->SetMethodTraceBuffer(nullptr, 0);
}

bool TraceProfiler::IsTraceProfileInProgress() {
  return profile_in_progress_;
}

void TraceProfiler::StartTraceLongRunningMethods(uint64_t trace_duration_ns) {
  TraceProfiler::Start(LowOverheadTraceType::kLongRunningMethods, trace_duration_ns);
}

void TraceProfiler::TraceTimeElapsed() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  num_trace_stop_tasks_--;
  if (num_trace_stop_tasks_ == 0) {
    // Only stop the trace if this event corresponds to the currently running trace.
    TraceProfiler::StopLocked();
  }
}

void TraceProfiler::DumpLongRunningMethodBuffer(uint32_t thread_id,
                                                uintptr_t* method_trace_entries,
                                                uintptr_t* end_trace_entries,
                                                std::unordered_set<ArtMethod*>& methods,
                                                std::ostringstream& os) {
  os << "Thread:" << thread_id << "\n";
  for (uintptr_t* ptr = method_trace_entries + kAlwaysOnTraceBufSize - 1;
       ptr >= end_trace_entries;) {
    uintptr_t entry = *ptr;
    if (entry == 0x1) {
      // This is the special placeholder exit we added to record all methods on the stack at the
      // start of the trace. Just ignore this entry.
    } else if (entry & 0x1) {
      // Method exit
      os << "<-" << TimestampCounter::GetMicroTime(entry & ~1) << "\n";
    } else {
      // Method entry
      ArtMethod* method = reinterpret_cast<ArtMethod*>(entry);
      ptr--;
      CHECK(ptr >= end_trace_entries);
      os << "->" << method << " " << TimestampCounter::GetMicroTime(*ptr) << "\n";
      methods.insert(method);
    }
    ptr--;
  }
}

void TraceProfiler::FlushBufferAndRecordTraceEvent(ArtMethod* method,
                                                   Thread* thread,
                                                   bool is_entry) {
  uint64_t timestamp = TimestampCounter::GetTimestamp();
  std::ostringstream os;
  std::unordered_set<ArtMethod*> traced_methods;
  uintptr_t* method_trace_entries = thread->GetMethodTraceBuffer();
  DCHECK(method_trace_entries != nullptr);
  uintptr_t** method_trace_curr_ptr = thread->GetTraceBufferCurrEntryPtr();

  // Find the last method exit event. We can flush all the entries before this event. We cannot
  // flush remaining events because we haven't determined if they are long running or not.
  uintptr_t* processed_events_ptr = nullptr;
  for (uintptr_t* ptr = *method_trace_curr_ptr;
       ptr < method_trace_entries + kAlwaysOnTraceBufSize;) {
    if (*ptr & 0x1) {
      // Method exit. We need to keep events until (including this method exit) here.
      processed_events_ptr = ptr + 1;
      break;
    }
    ptr += 2;
  }

  size_t num_occupied_entries = (processed_events_ptr - *method_trace_curr_ptr);
  size_t index = kAlwaysOnTraceBufSize;
  if (num_occupied_entries > kMaxEntriesAfterFlush) {
    // If we don't have sufficient space just record a placeholder exit and flush all the existing
    // events. We have accurate timestamps to filter out these events in a post-processing step.
    // This would happen only when we have very deeply (~1024) nested code.
    DumpLongRunningMethodBuffer(
        thread->GetTid(), method_trace_entries, *method_trace_curr_ptr, traced_methods, os);

    // Encode a placeholder exit event. This will be ignored when dumping the methods.
    method_trace_entries[--index] = 0x1;
  } else {
    // Flush all the entries till the method exit event.
    DumpLongRunningMethodBuffer(
        thread->GetTid(), method_trace_entries, processed_events_ptr, traced_methods, os);

    // Move the remaining events to the start of the buffer.
    for (uintptr_t* ptr = processed_events_ptr - 1; ptr >= *method_trace_curr_ptr; ptr--) {
      method_trace_entries[--index] = *ptr;
    }
  }

  // Record new entry
  if (is_entry) {
    method_trace_entries[--index] = reinterpret_cast<uintptr_t>(method);
    method_trace_entries[--index] = timestamp & ~1;
  } else {
    if (method_trace_entries[index] & 0x1) {
      method_trace_entries[--index] = timestamp | 1;
    } else {
      size_t prev_timestamp = method_trace_entries[index];
      if (timestamp - prev_timestamp < kLongRunningMethodThreshold) {
        index += 2;
        DCHECK_LT(index, kAlwaysOnTraceBufSize);
      } else {
        method_trace_entries[--index] = timestamp | 1;
      }
    }
  }
  *method_trace_curr_ptr = method_trace_entries + index;

  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  trace_data_->AppendToLongRunningMethods(os.str());
  trace_data_->AddTracedMethods(traced_methods);
  trace_data_->AddTracedThread(thread);
}

std::string TraceProfiler::GetLongRunningMethodsString() {
  if (!art_flags::always_enable_profile_code()) {
    return std::string();
  }

  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  if (!profile_in_progress_) {
    return std::string();
  }

  return GetLongRunningMethodsStringLocked();
}

void TraceDumpCheckpoint::Run(Thread* thread) {
  auto method_trace_entries = thread->GetMethodTraceBuffer();
  if (method_trace_entries != nullptr) {
    std::unordered_set<ArtMethod*> traced_methods;
    uintptr_t* method_trace_curr_ptr = *(thread->GetTraceBufferCurrEntryPtr());
    std::ostringstream os;
    TraceProfiler::DumpLongRunningMethodBuffer(
        thread->GetTid(), method_trace_entries, method_trace_curr_ptr, traced_methods, os);
    trace_data_->AddTracedThread(thread);
    trace_data_->AddTracedMethods(traced_methods);
    trace_data_->AppendToLongRunningMethods(os.str());
  }
  barrier_.Pass(Thread::Current());
}

void TraceDumpCheckpoint::WaitForThreadsToRunThroughCheckpoint(size_t threads_running_checkpoint) {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
  barrier_.Increment(self, threads_running_checkpoint);
}

std::string TraceProfiler::GetLongRunningMethodsStringLocked() {
  Thread* self = Thread::Current();
  std::ostringstream os;
  // Collect long running methods from all the threads;
  Runtime* runtime = Runtime::Current();
  TraceDumpCheckpoint checkpoint(trace_data_);
  size_t threads_running_checkpoint = runtime->GetThreadList()->RunCheckpoint(&checkpoint);
  if (threads_running_checkpoint != 0) {
    checkpoint.WaitForThreadsToRunThroughCheckpoint(threads_running_checkpoint);
  }

  trace_data_->DumpData(os);
  return os.str();
}

void TraceData::DumpData(std::ostringstream& os) {
  MutexLock mu(Thread::Current(), trace_data_lock_);
  if (long_running_methods_.length() > 0) {
    os << long_running_methods_;
  }

  // Dump the information about traced_methods and threads
  {
    ScopedObjectAccess soa(Thread::Current());
    DumpThreadMethodInfo(traced_threads_, traced_methods_, os);
  }
}

void TraceProfiler::DumpLongRunningMethods(std::unique_ptr<File>&& trace_file) {
  std::string info = GetLongRunningMethodsStringLocked();
  if (!trace_file->WriteFully(info.c_str(), info.length())) {
    PLOG(WARNING) << "Failed writing information to file";
  }

  if (!trace_file->Close()) {
    PLOG(WARNING) << "Failed to close file.";
  }
}

}  // namespace art
