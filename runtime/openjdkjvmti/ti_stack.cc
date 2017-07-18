/* Copyright (C) 2016 The Android Open Source Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file implements interfaces from the file jvmti.h. This implementation
 * is licensed under the same terms as the file jvmti.h.  The
 * copyright and license information for the file jvmti.h follows.
 *
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "ti_stack.h"

#include <algorithm>
#include <list>
#include <unordered_map>
#include <vector>

#include "art_field-inl.h"
#include "art_jvmti.h"
#include "art_method-inl.h"
#include "barrier.h"
#include "base/bit_utils.h"
#include "base/enums.h"
#include "base/mutex.h"
#include "dex_file.h"
#include "dex_file_annotations.h"
#include "handle_scope-inl.h"
#include "jni_env_ext.h"
#include "jni_internal.h"
#include "mirror/class.h"
#include "mirror/dex_cache.h"
#include "nativehelper/ScopedLocalRef.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "thread-current-inl.h"
#include "thread_list.h"
#include "thread_pool.h"
#include "well_known_classes.h"

namespace openjdkjvmti {

template <typename FrameFn>
struct GetStackTraceVisitor : public art::StackVisitor {
  GetStackTraceVisitor(art::Thread* thread_in,
                       size_t start_,
                       size_t stop_,
                       FrameFn fn_)
      : StackVisitor(thread_in, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        fn(fn_),
        start(start_),
        stop(stop_) {}
  GetStackTraceVisitor(const GetStackTraceVisitor&) = default;
  GetStackTraceVisitor(GetStackTraceVisitor&&) = default;

  bool VisitFrame() REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;
    }

    if (start == 0) {
      m = m->GetInterfaceMethodIfProxy(art::kRuntimePointerSize);
      jmethodID id = art::jni::EncodeArtMethod(m);

      uint32_t dex_pc = GetDexPc(false);
      jlong dex_location = (dex_pc == art::DexFile::kDexNoIndex) ? -1 : static_cast<jlong>(dex_pc);

      jvmtiFrameInfo info = { id, dex_location };
      fn(info);

      if (stop == 1) {
        return false;  // We're done.
      } else if (stop > 0) {
        stop--;
      }
    } else {
      start--;
    }

    return true;
  }

  FrameFn fn;
  size_t start;
  size_t stop;
};

template <typename FrameFn>
GetStackTraceVisitor<FrameFn> MakeStackTraceVisitor(art::Thread* thread_in,
                                                    size_t start,
                                                    size_t stop,
                                                    FrameFn fn) {
  return GetStackTraceVisitor<FrameFn>(thread_in, start, stop, fn);
}

struct GetStackTraceVectorClosure : public art::Closure {
 public:
  GetStackTraceVectorClosure(size_t start, size_t stop)
      : start_input(start),
        stop_input(stop),
        start_result(0),
        stop_result(0) {}

  void Run(art::Thread* self) OVERRIDE REQUIRES_SHARED(art::Locks::mutator_lock_) {
    auto frames_fn = [&](jvmtiFrameInfo info) {
      frames.push_back(info);
    };
    auto visitor = MakeStackTraceVisitor(self, start_input, stop_input, frames_fn);
    visitor.WalkStack(/* include_transitions */ false);

    start_result = visitor.start;
    stop_result = visitor.stop;
  }

  const size_t start_input;
  const size_t stop_input;

  std::vector<jvmtiFrameInfo> frames;
  size_t start_result;
  size_t stop_result;
};

static jvmtiError TranslateFrameVector(const std::vector<jvmtiFrameInfo>& frames,
                                       jint start_depth,
                                       size_t start_result,
                                       jint max_frame_count,
                                       jvmtiFrameInfo* frame_buffer,
                                       jint* count_ptr) {
  size_t collected_frames = frames.size();

  // Assume we're here having collected something.
  DCHECK_GT(max_frame_count, 0);

  // Frames from the top.
  if (start_depth >= 0) {
    if (start_result != 0) {
      // Not enough frames.
      return ERR(ILLEGAL_ARGUMENT);
    }
    DCHECK_LE(collected_frames, static_cast<size_t>(max_frame_count));
    if (frames.size() > 0) {
      memcpy(frame_buffer, frames.data(), collected_frames * sizeof(jvmtiFrameInfo));
    }
    *count_ptr = static_cast<jint>(frames.size());
    return ERR(NONE);
  }

  // Frames from the bottom.
  if (collected_frames < static_cast<size_t>(-start_depth)) {
    return ERR(ILLEGAL_ARGUMENT);
  }

  size_t count = std::min(static_cast<size_t>(-start_depth), static_cast<size_t>(max_frame_count));
  memcpy(frame_buffer,
         &frames.data()[collected_frames + start_depth],
         count * sizeof(jvmtiFrameInfo));
  *count_ptr = static_cast<jint>(count);
  return ERR(NONE);
}

struct GetStackTraceDirectClosure : public art::Closure {
 public:
  GetStackTraceDirectClosure(jvmtiFrameInfo* frame_buffer_, size_t start, size_t stop)
      : frame_buffer(frame_buffer_),
        start_input(start),
        stop_input(stop),
        index(0) {
    DCHECK_GE(start_input, 0u);
  }

  void Run(art::Thread* self) OVERRIDE REQUIRES_SHARED(art::Locks::mutator_lock_) {
    auto frames_fn = [&](jvmtiFrameInfo info) {
      frame_buffer[index] = info;
      ++index;
    };
    auto visitor = MakeStackTraceVisitor(self, start_input, stop_input, frames_fn);
    visitor.WalkStack(/* include_transitions */ false);
  }

  jvmtiFrameInfo* frame_buffer;

  const size_t start_input;
  const size_t stop_input;

  size_t index = 0;
};

static jvmtiError GetThread(JNIEnv* env,
                            art::ScopedObjectAccessAlreadyRunnable& soa,
                            jthread java_thread,
                            art::Thread** thread)
    REQUIRES_SHARED(art::Locks::mutator_lock_)  // Needed for FromManagedThread.
    REQUIRES(art::Locks::thread_list_lock_) {   // Needed for FromManagedThread.
  if (java_thread == nullptr) {
    *thread = art::Thread::Current();
    if (*thread == nullptr) {
      // GetStackTrace can only be run during the live phase, so the current thread should be
      // attached and thus available. Getting a null for current means we're starting up or
      // dying.
      return ERR(WRONG_PHASE);
    }
  } else {
    if (!env->IsInstanceOf(java_thread, art::WellKnownClasses::java_lang_Thread)) {
      return ERR(INVALID_THREAD);
    }

    // TODO: Need non-aborting call here, to return JVMTI_ERROR_INVALID_THREAD.
    *thread = art::Thread::FromManagedThread(soa, java_thread);
    if (*thread == nullptr) {
      return ERR(THREAD_NOT_ALIVE);
    }
  }
  return ERR(NONE);
}

jvmtiError StackUtil::GetStackTrace(jvmtiEnv* jvmti_env ATTRIBUTE_UNUSED,
                                    jthread java_thread,
                                    jint start_depth,
                                    jint max_frame_count,
                                    jvmtiFrameInfo* frame_buffer,
                                    jint* count_ptr) {
  // It is not great that we have to hold these locks for so long, but it is necessary to ensure
  // that the thread isn't dying on us.
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);

  art::Thread* thread;
  jvmtiError thread_error = GetThread(art::Thread::Current()->GetJniEnv(),
                                      soa,
                                      java_thread,
                                      &thread);
  if (thread_error != ERR(NONE)) {
    return thread_error;
  }
  DCHECK(thread != nullptr);

  art::ThreadState state = thread->GetState();
  if (state == art::ThreadState::kStarting ||
      state == art::ThreadState::kTerminated ||
      thread->IsStillStarting()) {
    return ERR(THREAD_NOT_ALIVE);
  }

  if (max_frame_count < 0) {
    return ERR(ILLEGAL_ARGUMENT);
  }
  if (frame_buffer == nullptr || count_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  if (max_frame_count == 0) {
    *count_ptr = 0;
    return ERR(NONE);
  }

  if (start_depth >= 0) {
    // Fast path: Regular order of stack trace. Fill into the frame_buffer directly.
    GetStackTraceDirectClosure closure(frame_buffer,
                                       static_cast<size_t>(start_depth),
                                       static_cast<size_t>(max_frame_count));
    thread->RequestSynchronousCheckpoint(&closure);
    *count_ptr = static_cast<jint>(closure.index);
    if (closure.index < static_cast<size_t>(start_depth)) {
      return ERR(ILLEGAL_ARGUMENT);
    }
    return ERR(NONE);
  }

  GetStackTraceVectorClosure closure(0, 0);
  thread->RequestSynchronousCheckpoint(&closure);

  return TranslateFrameVector(closure.frames,
                              start_depth,
                              closure.start_result,
                              max_frame_count,
                              frame_buffer,
                              count_ptr);
}

template <typename Data>
struct GetAllStackTracesVectorClosure : public art::Closure {
  GetAllStackTracesVectorClosure(size_t stop, Data* data_)
      : barrier(0), stop_input(stop), data(data_) {}

  void Run(art::Thread* thread) OVERRIDE
      REQUIRES_SHARED(art::Locks::mutator_lock_)
      REQUIRES(!data->mutex) {
    art::Thread* self = art::Thread::Current();
    Work(thread, self);
    barrier.Pass(self);
  }

  void Work(art::Thread* thread, art::Thread* self)
      REQUIRES_SHARED(art::Locks::mutator_lock_)
      REQUIRES(!data->mutex) {
    // Skip threads that are still starting.
    if (thread->IsStillStarting()) {
      return;
    }

    std::vector<jvmtiFrameInfo>* thread_frames = data->GetFrameStorageFor(self, thread);
    if (thread_frames == nullptr) {
      return;
    }

    // Now collect the data.
    auto frames_fn = [&](jvmtiFrameInfo info) {
      thread_frames->push_back(info);
    };
    auto visitor = MakeStackTraceVisitor(thread, 0u, stop_input, frames_fn);
    visitor.WalkStack(/* include_transitions */ false);
  }

  art::Barrier barrier;
  const size_t stop_input;
  Data* data;
};

template <typename Data>
static void RunCheckpointAndWait(Data* data, size_t max_frame_count) {
  GetAllStackTracesVectorClosure<Data> closure(max_frame_count, data);
  size_t barrier_count = art::Runtime::Current()->GetThreadList()->RunCheckpoint(&closure, nullptr);
  if (barrier_count == 0) {
    return;
  }
  art::Thread* self = art::Thread::Current();
  art::ScopedThreadStateChange tsc(self, art::ThreadState::kWaitingForCheckPointsToRun);
  closure.barrier.Increment(self, barrier_count);
}

jvmtiError StackUtil::GetAllStackTraces(jvmtiEnv* env,
                                        jint max_frame_count,
                                        jvmtiStackInfo** stack_info_ptr,
                                        jint* thread_count_ptr) {
  if (max_frame_count < 0) {
    return ERR(ILLEGAL_ARGUMENT);
  }
  if (stack_info_ptr == nullptr || thread_count_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  struct AllStackTracesData {
    AllStackTracesData() : mutex("GetAllStackTraces", art::LockLevel::kAbortLock) {}
    ~AllStackTracesData() {
      JNIEnv* jni_env = art::Thread::Current()->GetJniEnv();
      for (jthread global_thread_ref : thread_peers) {
        jni_env->DeleteGlobalRef(global_thread_ref);
      }
    }

    std::vector<jvmtiFrameInfo>* GetFrameStorageFor(art::Thread* self, art::Thread* thread)
        REQUIRES_SHARED(art::Locks::mutator_lock_)
        REQUIRES(!mutex) {
      art::MutexLock mu(self, mutex);

      threads.push_back(thread);

      jthread peer = art::Runtime::Current()->GetJavaVM()->AddGlobalRef(
          self, thread->GetPeerFromOtherThread());
      thread_peers.push_back(peer);

      frames.emplace_back(new std::vector<jvmtiFrameInfo>());
      return frames.back().get();
    }

    art::Mutex mutex;

    // Storage. Only access directly after completion.

    std::vector<art::Thread*> threads;
    // "thread_peers" contains global references to their peers.
    std::vector<jthread> thread_peers;

    std::vector<std::unique_ptr<std::vector<jvmtiFrameInfo>>> frames;
  };

  AllStackTracesData data;
  RunCheckpointAndWait(&data, static_cast<size_t>(max_frame_count));

  art::Thread* current = art::Thread::Current();

  // Convert the data into our output format.

  // Note: we use an array of jvmtiStackInfo for convenience. The spec says we need to
  //       allocate one big chunk for this and the actual frames, which means we need
  //       to either be conservative or rearrange things later (the latter is implemented).
  std::unique_ptr<jvmtiStackInfo[]> stack_info_array(new jvmtiStackInfo[data.frames.size()]);
  std::vector<std::unique_ptr<jvmtiFrameInfo[]>> frame_infos;
  frame_infos.reserve(data.frames.size());

  // Now run through and add data for each thread.
  size_t sum_frames = 0;
  for (size_t index = 0; index < data.frames.size(); ++index) {
    jvmtiStackInfo& stack_info = stack_info_array.get()[index];
    memset(&stack_info, 0, sizeof(jvmtiStackInfo));

    const std::vector<jvmtiFrameInfo>& thread_frames = *data.frames[index].get();

    // For the time being, set the thread to null. We'll fix it up in the second stage.
    stack_info.thread = nullptr;
    stack_info.state = JVMTI_THREAD_STATE_SUSPENDED;

    size_t collected_frames = thread_frames.size();
    if (max_frame_count == 0 || collected_frames == 0) {
      stack_info.frame_count = 0;
      stack_info.frame_buffer = nullptr;
      continue;
    }
    DCHECK_LE(collected_frames, static_cast<size_t>(max_frame_count));

    jvmtiFrameInfo* frame_info = new jvmtiFrameInfo[collected_frames];
    frame_infos.emplace_back(frame_info);

    jint count;
    jvmtiError translate_result = TranslateFrameVector(thread_frames,
                                                       0,
                                                       0,
                                                       static_cast<jint>(collected_frames),
                                                       frame_info,
                                                       &count);
    DCHECK(translate_result == JVMTI_ERROR_NONE);
    stack_info.frame_count = static_cast<jint>(collected_frames);
    stack_info.frame_buffer = frame_info;
    sum_frames += static_cast<size_t>(count);
  }

  // No errors, yet. Now put it all into an output buffer.
  size_t rounded_stack_info_size = art::RoundUp(sizeof(jvmtiStackInfo) * data.frames.size(),
                                                alignof(jvmtiFrameInfo));
  size_t chunk_size = rounded_stack_info_size + sum_frames * sizeof(jvmtiFrameInfo);
  unsigned char* chunk_data;
  jvmtiError alloc_result = env->Allocate(chunk_size, &chunk_data);
  if (alloc_result != ERR(NONE)) {
    return alloc_result;
  }

  jvmtiStackInfo* stack_info = reinterpret_cast<jvmtiStackInfo*>(chunk_data);
  // First copy in all the basic data.
  memcpy(stack_info, stack_info_array.get(), sizeof(jvmtiStackInfo) * data.frames.size());

  // Now copy the frames and fix up the pointers.
  jvmtiFrameInfo* frame_info = reinterpret_cast<jvmtiFrameInfo*>(
      chunk_data + rounded_stack_info_size);
  for (size_t i = 0; i < data.frames.size(); ++i) {
    jvmtiStackInfo& old_stack_info = stack_info_array.get()[i];
    jvmtiStackInfo& new_stack_info = stack_info[i];

    // Translate the global ref into a local ref.
    new_stack_info.thread =
        static_cast<JNIEnv*>(current->GetJniEnv())->NewLocalRef(data.thread_peers[i]);

    if (old_stack_info.frame_count > 0) {
      // Only copy when there's data - leave the nullptr alone.
      size_t frames_size = static_cast<size_t>(old_stack_info.frame_count) * sizeof(jvmtiFrameInfo);
      memcpy(frame_info, old_stack_info.frame_buffer, frames_size);
      new_stack_info.frame_buffer = frame_info;
      frame_info += old_stack_info.frame_count;
    }
  }

  *stack_info_ptr = stack_info;
  *thread_count_ptr = static_cast<jint>(data.frames.size());

  return ERR(NONE);
}

jvmtiError StackUtil::GetThreadListStackTraces(jvmtiEnv* env,
                                               jint thread_count,
                                               const jthread* thread_list,
                                               jint max_frame_count,
                                               jvmtiStackInfo** stack_info_ptr) {
  if (max_frame_count < 0) {
    return ERR(ILLEGAL_ARGUMENT);
  }
  if (thread_count < 0) {
    return ERR(ILLEGAL_ARGUMENT);
  }
  if (thread_count == 0) {
    *stack_info_ptr = nullptr;
    return ERR(NONE);
  }
  if (stack_info_ptr == nullptr || stack_info_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  art::Thread* current = art::Thread::Current();
  art::ScopedObjectAccess soa(current);      // Now we know we have the shared lock.

  struct SelectStackTracesData {
    SelectStackTracesData() : mutex("GetSelectStackTraces", art::LockLevel::kAbortLock) {}

    std::vector<jvmtiFrameInfo>* GetFrameStorageFor(art::Thread* self, art::Thread* thread)
              REQUIRES_SHARED(art::Locks::mutator_lock_)
              REQUIRES(!mutex) {
      art::ObjPtr<art::mirror::Object> peer = thread->GetPeerFromOtherThread();
      for (size_t index = 0; index != handles.size(); ++index) {
        if (peer == handles[index].Get()) {
          // Found the thread.
          art::MutexLock mu(self, mutex);

          threads.push_back(thread);
          thread_list_indices.push_back(index);

          frames.emplace_back(new std::vector<jvmtiFrameInfo>());
          return frames.back().get();
        }
      }
      return nullptr;
    }

    art::Mutex mutex;

    // Selection data.

    std::vector<art::Handle<art::mirror::Object>> handles;

    // Storage. Only access directly after completion.

    std::vector<art::Thread*> threads;
    std::vector<size_t> thread_list_indices;

    std::vector<std::unique_ptr<std::vector<jvmtiFrameInfo>>> frames;
  };

  SelectStackTracesData data;

  // Decode all threads to raw pointers. Put them into a handle scope to avoid any moving GC bugs.
  art::VariableSizedHandleScope hs(current);
  for (jint i = 0; i != thread_count; ++i) {
    if (thread_list[i] == nullptr) {
      return ERR(INVALID_THREAD);
    }
    if (!soa.Env()->IsInstanceOf(thread_list[i], art::WellKnownClasses::java_lang_Thread)) {
      return ERR(INVALID_THREAD);
    }
    data.handles.push_back(hs.NewHandle(soa.Decode<art::mirror::Object>(thread_list[i])));
  }

  RunCheckpointAndWait(&data, static_cast<size_t>(max_frame_count));

  // Convert the data into our output format.

  // Note: we use an array of jvmtiStackInfo for convenience. The spec says we need to
  //       allocate one big chunk for this and the actual frames, which means we need
  //       to either be conservative or rearrange things later (the latter is implemented).
  std::unique_ptr<jvmtiStackInfo[]> stack_info_array(new jvmtiStackInfo[data.frames.size()]);
  std::vector<std::unique_ptr<jvmtiFrameInfo[]>> frame_infos;
  frame_infos.reserve(data.frames.size());

  // Now run through and add data for each thread.
  size_t sum_frames = 0;
  for (size_t index = 0; index < data.frames.size(); ++index) {
    jvmtiStackInfo& stack_info = stack_info_array.get()[index];
    memset(&stack_info, 0, sizeof(jvmtiStackInfo));

    art::Thread* self = data.threads[index];
    const std::vector<jvmtiFrameInfo>& thread_frames = *data.frames[index].get();

    // For the time being, set the thread to null. We don't have good ScopedLocalRef
    // infrastructure.
    DCHECK(self->GetPeerFromOtherThread() != nullptr);
    stack_info.thread = nullptr;
    stack_info.state = JVMTI_THREAD_STATE_SUSPENDED;

    size_t collected_frames = thread_frames.size();
    if (max_frame_count == 0 || collected_frames == 0) {
      stack_info.frame_count = 0;
      stack_info.frame_buffer = nullptr;
      continue;
    }
    DCHECK_LE(collected_frames, static_cast<size_t>(max_frame_count));

    jvmtiFrameInfo* frame_info = new jvmtiFrameInfo[collected_frames];
    frame_infos.emplace_back(frame_info);

    jint count;
    jvmtiError translate_result = TranslateFrameVector(thread_frames,
                                                       0,
                                                       0,
                                                       static_cast<jint>(collected_frames),
                                                       frame_info,
                                                       &count);
    DCHECK(translate_result == JVMTI_ERROR_NONE);
    stack_info.frame_count = static_cast<jint>(collected_frames);
    stack_info.frame_buffer = frame_info;
    sum_frames += static_cast<size_t>(count);
  }

  // No errors, yet. Now put it all into an output buffer. Note that this is not frames.size(),
  // potentially.
  size_t rounded_stack_info_size = art::RoundUp(sizeof(jvmtiStackInfo) * thread_count,
                                                alignof(jvmtiFrameInfo));
  size_t chunk_size = rounded_stack_info_size + sum_frames * sizeof(jvmtiFrameInfo);
  unsigned char* chunk_data;
  jvmtiError alloc_result = env->Allocate(chunk_size, &chunk_data);
  if (alloc_result != ERR(NONE)) {
    return alloc_result;
  }

  jvmtiStackInfo* stack_info = reinterpret_cast<jvmtiStackInfo*>(chunk_data);
  jvmtiFrameInfo* frame_info = reinterpret_cast<jvmtiFrameInfo*>(
      chunk_data + rounded_stack_info_size);

  for (size_t i = 0; i < static_cast<size_t>(thread_count); ++i) {
    // Check whether we found a running thread for this.
    // Note: For simplicity, and with the expectation that the list is usually small, use a simple
    //       search. (The list is *not* sorted!)
    auto it = std::find(data.thread_list_indices.begin(), data.thread_list_indices.end(), i);
    if (it == data.thread_list_indices.end()) {
      // No native thread. Must be new or dead. We need to fill out the stack info now.
      // (Need to read the Java "started" field to know whether this is starting or terminated.)
      art::ObjPtr<art::mirror::Object> peer = soa.Decode<art::mirror::Object>(thread_list[i]);
      art::ObjPtr<art::mirror::Class> klass = peer->GetClass();
      art::ArtField* started_field = klass->FindDeclaredInstanceField("started", "Z");
      CHECK(started_field != nullptr);
      bool started = started_field->GetBoolean(peer) != 0;
      constexpr jint kStartedState = JVMTI_JAVA_LANG_THREAD_STATE_NEW;
      constexpr jint kTerminatedState = JVMTI_THREAD_STATE_TERMINATED |
          JVMTI_JAVA_LANG_THREAD_STATE_TERMINATED;
      stack_info[i].thread = reinterpret_cast<JNIEnv*>(soa.Env())->NewLocalRef(thread_list[i]);
      stack_info[i].state = started ? kTerminatedState : kStartedState;
      stack_info[i].frame_count = 0;
      stack_info[i].frame_buffer = nullptr;
    } else {
      // Had a native thread and frames.
      size_t f_index = it - data.thread_list_indices.begin();

      jvmtiStackInfo& old_stack_info = stack_info_array.get()[f_index];
      jvmtiStackInfo& new_stack_info = stack_info[i];

      memcpy(&new_stack_info, &old_stack_info, sizeof(jvmtiStackInfo));
      new_stack_info.thread = reinterpret_cast<JNIEnv*>(soa.Env())->NewLocalRef(thread_list[i]);
      if (old_stack_info.frame_count > 0) {
        // Only copy when there's data - leave the nullptr alone.
        size_t frames_size =
            static_cast<size_t>(old_stack_info.frame_count) * sizeof(jvmtiFrameInfo);
        memcpy(frame_info, old_stack_info.frame_buffer, frames_size);
        new_stack_info.frame_buffer = frame_info;
        frame_info += old_stack_info.frame_count;
      }
    }
  }

  *stack_info_ptr = stack_info;

  return ERR(NONE);
}

// Walks up the stack counting Java frames. This is not StackVisitor::ComputeNumFrames, as
// runtime methods and transitions must not be counted.
struct GetFrameCountVisitor : public art::StackVisitor {
  explicit GetFrameCountVisitor(art::Thread* thread)
      : art::StackVisitor(thread, nullptr, art::StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        count(0) {}

  bool VisitFrame() REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::ArtMethod* m = GetMethod();
    const bool do_count = !(m == nullptr || m->IsRuntimeMethod());
    if (do_count) {
      count++;
    }
    return true;
  }

  size_t count;
};

struct GetFrameCountClosure : public art::Closure {
 public:
  GetFrameCountClosure() : count(0) {}

  void Run(art::Thread* self) OVERRIDE REQUIRES_SHARED(art::Locks::mutator_lock_) {
    GetFrameCountVisitor visitor(self);
    visitor.WalkStack(false);

    count = visitor.count;
  }

  size_t count;
};

jvmtiError StackUtil::GetFrameCount(jvmtiEnv* env ATTRIBUTE_UNUSED,
                                    jthread java_thread,
                                    jint* count_ptr) {
  // It is not great that we have to hold these locks for so long, but it is necessary to ensure
  // that the thread isn't dying on us.
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);

  art::Thread* thread;
  jvmtiError thread_error = GetThread(art::Thread::Current()->GetJniEnv(),
                                      soa,
                                      java_thread,
                                      &thread);

  if (thread_error != ERR(NONE)) {
    return thread_error;
  }
  DCHECK(thread != nullptr);

  if (count_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  GetFrameCountClosure closure;
  thread->RequestSynchronousCheckpoint(&closure);

  *count_ptr = closure.count;
  return ERR(NONE);
}

// Walks up the stack 'n' callers, when used with Thread::WalkStack.
struct GetLocationVisitor : public art::StackVisitor {
  GetLocationVisitor(art::Thread* thread, size_t n_in)
      : art::StackVisitor(thread, nullptr, art::StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        n(n_in),
        count(0),
        caller(nullptr),
        caller_dex_pc(0) {}

  bool VisitFrame() REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::ArtMethod* m = GetMethod();
    const bool do_count = !(m == nullptr || m->IsRuntimeMethod());
    if (do_count) {
      DCHECK(caller == nullptr);
      if (count == n) {
        caller = m;
        caller_dex_pc = GetDexPc(false);
        return false;
      }
      count++;
    }
    return true;
  }

  const size_t n;
  size_t count;
  art::ArtMethod* caller;
  uint32_t caller_dex_pc;
};

struct GetLocationClosure : public art::Closure {
 public:
  explicit GetLocationClosure(size_t n_in) : n(n_in), method(nullptr), dex_pc(0) {}

  void Run(art::Thread* self) OVERRIDE REQUIRES_SHARED(art::Locks::mutator_lock_) {
    GetLocationVisitor visitor(self, n);
    visitor.WalkStack(false);

    method = visitor.caller;
    dex_pc = visitor.caller_dex_pc;
  }

  const size_t n;
  art::ArtMethod* method;
  uint32_t dex_pc;
};

jvmtiError StackUtil::GetFrameLocation(jvmtiEnv* env ATTRIBUTE_UNUSED,
                                       jthread java_thread,
                                       jint depth,
                                       jmethodID* method_ptr,
                                       jlocation* location_ptr) {
  // It is not great that we have to hold these locks for so long, but it is necessary to ensure
  // that the thread isn't dying on us.
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);

  art::Thread* thread;
  jvmtiError thread_error = GetThread(art::Thread::Current()->GetJniEnv(),
                                      soa,
                                      java_thread,
                                      &thread);
  if (thread_error != ERR(NONE)) {
    return thread_error;
  }
  DCHECK(thread != nullptr);

  if (depth < 0) {
    return ERR(ILLEGAL_ARGUMENT);
  }
  if (method_ptr == nullptr || location_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  GetLocationClosure closure(static_cast<size_t>(depth));
  thread->RequestSynchronousCheckpoint(&closure);

  if (closure.method == nullptr) {
    return ERR(NO_MORE_FRAMES);
  }

  *method_ptr = art::jni::EncodeArtMethod(closure.method);
  if (closure.method->IsNative()) {
    *location_ptr = -1;
  } else {
    if (closure.dex_pc == art::DexFile::kDexNoIndex) {
      return ERR(INTERNAL);
    }
    *location_ptr = static_cast<jlocation>(closure.dex_pc);
  }

  return ERR(NONE);
}

}  // namespace openjdkjvmti
