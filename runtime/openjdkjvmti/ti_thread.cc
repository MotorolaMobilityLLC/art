/* Copyright (C) 2017 The Android Open Source Project
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

#include "ti_thread.h"

#include "art_field.h"
#include "art_jvmti.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "jni_internal.h"
#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "obj_ptr.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"
#include "well_known_classes.h"

namespace openjdkjvmti {

jvmtiError ThreadUtil::GetCurrentThread(jvmtiEnv* env ATTRIBUTE_UNUSED, jthread* thread_ptr) {
  art::Thread* self = art::Thread::Current();

  art::ScopedObjectAccess soa(self);

  jthread thread_peer;
  if (self->IsStillStarting()) {
    thread_peer = nullptr;
  } else {
    thread_peer = soa.AddLocalReference<jthread>(self->GetPeer());
  }

  *thread_ptr = thread_peer;
  return ERR(NONE);
}

// Read the context classloader from a Java thread object. This is a lazy implementation
// that assumes GetThreadInfo isn't called too often. If we instead cache the ArtField,
// we will have to add synchronization as this can't be cached on startup (which is
// potentially runtime startup).
static art::ObjPtr<art::mirror::Object> GetContextClassLoader(art::ObjPtr<art::mirror::Object> peer)
    REQUIRES_SHARED(art::Locks::mutator_lock_) {
  if (peer == nullptr) {
    return nullptr;
  }
  art::ObjPtr<art::mirror::Class> klass = peer->GetClass();
  art::ArtField* cc_field = klass->FindDeclaredInstanceField("contextClassLoader",
                                                             "Ljava/lang/ClassLoader;");
  CHECK(cc_field != nullptr);
  return cc_field->GetObject(peer);
}

// Get the native thread. The spec says a null object denotes the current thread.
static art::Thread* GetNativeThread(jthread thread,
                                    const art::ScopedObjectAccessAlreadyRunnable& soa)
    REQUIRES_SHARED(art::Locks::mutator_lock_) {
  if (thread == nullptr) {
    return art::Thread::Current();
  }

  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
  return art::Thread::FromManagedThread(soa, thread);
}

jvmtiError ThreadUtil::GetThreadInfo(jvmtiEnv* env, jthread thread, jvmtiThreadInfo* info_ptr) {
  if (info_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  art::ScopedObjectAccess soa(art::Thread::Current());

  art::Thread* self = GetNativeThread(thread, soa);
  if (self == nullptr && thread == nullptr) {
    return ERR(INVALID_THREAD);
  }

  JvmtiUniquePtr name_uptr;
  if (self != nullptr) {
    // Have a native thread object, this thread is alive.
    std::string name;
    self->GetThreadName(name);
    jvmtiError name_result = CopyString(
        env, name.c_str(), reinterpret_cast<unsigned char**>(&info_ptr->name));
    if (name_result != ERR(NONE)) {
      return name_result;
    }
    name_uptr = MakeJvmtiUniquePtr(env, info_ptr->name);

    info_ptr->priority = self->GetNativePriority();

    info_ptr->is_daemon = self->IsDaemon();

    art::ObjPtr<art::mirror::Object> peer = self->GetPeer();

    // ThreadGroup.
    if (peer != nullptr) {
      art::ArtField* f = art::jni::DecodeArtField(art::WellKnownClasses::java_lang_Thread_group);
      CHECK(f != nullptr);
      art::ObjPtr<art::mirror::Object> group = f->GetObject(peer);
      info_ptr->thread_group = group == nullptr
                                   ? nullptr
                                   : soa.AddLocalReference<jthreadGroup>(group);
    } else {
      info_ptr->thread_group = nullptr;
    }

    // Context classloader.
    art::ObjPtr<art::mirror::Object> ccl = GetContextClassLoader(peer);
    info_ptr->context_class_loader = ccl == nullptr
                                         ? nullptr
                                         : soa.AddLocalReference<jobject>(ccl);
  } else {
    // Only the peer. This thread has either not been started, or is dead. Read things from
    // the Java side.
    art::ObjPtr<art::mirror::Object> peer = soa.Decode<art::mirror::Object>(thread);

    // Name.
    {
      art::ArtField* f = art::jni::DecodeArtField(art::WellKnownClasses::java_lang_Thread_name);
      CHECK(f != nullptr);
      art::ObjPtr<art::mirror::Object> name = f->GetObject(peer);
      std::string name_cpp;
      const char* name_cstr;
      if (name != nullptr) {
        name_cpp = name->AsString()->ToModifiedUtf8();
        name_cstr = name_cpp.c_str();
      } else {
        name_cstr = "";
      }
      jvmtiError name_result = CopyString(
          env, name_cstr, reinterpret_cast<unsigned char**>(&info_ptr->name));
      if (name_result != ERR(NONE)) {
        return name_result;
      }
      name_uptr = MakeJvmtiUniquePtr(env, info_ptr->name);
    }

    // Priority.
    {
      art::ArtField* f = art::jni::DecodeArtField(art::WellKnownClasses::java_lang_Thread_priority);
      CHECK(f != nullptr);
      info_ptr->priority = static_cast<jint>(f->GetInt(peer));
    }

    // Daemon.
    {
      art::ArtField* f = art::jni::DecodeArtField(art::WellKnownClasses::java_lang_Thread_daemon);
      CHECK(f != nullptr);
      info_ptr->is_daemon = f->GetBoolean(peer) == 0 ? JNI_FALSE : JNI_TRUE;
    }

    // ThreadGroup.
    {
      art::ArtField* f = art::jni::DecodeArtField(art::WellKnownClasses::java_lang_Thread_group);
      CHECK(f != nullptr);
      art::ObjPtr<art::mirror::Object> group = f->GetObject(peer);
      info_ptr->thread_group = group == nullptr
                                   ? nullptr
                                   : soa.AddLocalReference<jthreadGroup>(group);
    }

    // Context classloader.
    art::ObjPtr<art::mirror::Object> ccl = GetContextClassLoader(peer);
    info_ptr->context_class_loader = ccl == nullptr
                                         ? nullptr
                                         : soa.AddLocalReference<jobject>(ccl);
  }

  name_uptr.release();

  return ERR(NONE);
}

// Return the thread's (or current thread, if null) thread state. Return kStarting in case
// there's no native counterpart (thread hasn't been started, yet, or is dead).
static art::ThreadState GetNativeThreadState(jthread thread,
                                             const art::ScopedObjectAccessAlreadyRunnable& soa,
                                             art::Thread** native_thread)
    REQUIRES_SHARED(art::Locks::mutator_lock_) {
  art::Thread* self = nullptr;
  art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
  if (thread == nullptr) {
    self = art::Thread::Current();
  } else {
    self = art::Thread::FromManagedThread(soa, thread);
  }
  *native_thread = self;
  if (self == nullptr || self->IsStillStarting()) {
    return art::ThreadState::kStarting;
  }
  return self->GetState();
}

static jint GetJvmtiThreadStateFromInternal(art::ThreadState internal_thread_state) {
  jint jvmti_state = JVMTI_THREAD_STATE_ALIVE;

  if (internal_thread_state == art::ThreadState::kSuspended) {
    jvmti_state |= JVMTI_THREAD_STATE_SUSPENDED;
    // Note: We do not have data about the previous state. Otherwise we should load the previous
    //       state here.
  }

  if (internal_thread_state == art::ThreadState::kNative) {
    jvmti_state |= JVMTI_THREAD_STATE_IN_NATIVE;
  }

  if (internal_thread_state == art::ThreadState::kRunnable ||
      internal_thread_state == art::ThreadState::kWaitingWeakGcRootRead ||
      internal_thread_state == art::ThreadState::kSuspended) {
    jvmti_state |= JVMTI_THREAD_STATE_RUNNABLE;
  } else if (internal_thread_state == art::ThreadState::kBlocked) {
    jvmti_state |= JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER;
  } else {
    // Should be in waiting state.
    jvmti_state |= JVMTI_THREAD_STATE_WAITING;

    if (internal_thread_state == art::ThreadState::kTimedWaiting ||
        internal_thread_state == art::ThreadState::kSleeping) {
      jvmti_state |= JVMTI_THREAD_STATE_WAITING_WITH_TIMEOUT;
    } else {
      jvmti_state |= JVMTI_THREAD_STATE_WAITING_INDEFINITELY;
    }

    if (internal_thread_state == art::ThreadState::kSleeping) {
      jvmti_state |= JVMTI_THREAD_STATE_SLEEPING;
    }

    if (internal_thread_state == art::ThreadState::kTimedWaiting ||
        internal_thread_state == art::ThreadState::kWaiting) {
      jvmti_state |= JVMTI_THREAD_STATE_IN_OBJECT_WAIT;
    }

    // TODO: PARKED. We'll have to inspect the stack.
  }

  return jvmti_state;
}

static jint GetJavaStateFromInternal(art::ThreadState internal_thread_state) {
  switch (internal_thread_state) {
    case art::ThreadState::kTerminated:
      return JVMTI_JAVA_LANG_THREAD_STATE_TERMINATED;

    case art::ThreadState::kRunnable:
    case art::ThreadState::kNative:
    case art::ThreadState::kWaitingWeakGcRootRead:
    case art::ThreadState::kSuspended:
      return JVMTI_JAVA_LANG_THREAD_STATE_RUNNABLE;

    case art::ThreadState::kTimedWaiting:
    case art::ThreadState::kSleeping:
      return JVMTI_JAVA_LANG_THREAD_STATE_TIMED_WAITING;

    case art::ThreadState::kBlocked:
      return JVMTI_JAVA_LANG_THREAD_STATE_BLOCKED;

    case art::ThreadState::kStarting:
      return JVMTI_JAVA_LANG_THREAD_STATE_NEW;

    case art::ThreadState::kWaiting:
    case art::ThreadState::kWaitingForGcToComplete:
    case art::ThreadState::kWaitingPerformingGc:
    case art::ThreadState::kWaitingForCheckPointsToRun:
    case art::ThreadState::kWaitingForDebuggerSend:
    case art::ThreadState::kWaitingForDebuggerToAttach:
    case art::ThreadState::kWaitingInMainDebuggerLoop:
    case art::ThreadState::kWaitingForDebuggerSuspension:
    case art::ThreadState::kWaitingForDeoptimization:
    case art::ThreadState::kWaitingForGetObjectsAllocated:
    case art::ThreadState::kWaitingForJniOnLoad:
    case art::ThreadState::kWaitingForSignalCatcherOutput:
    case art::ThreadState::kWaitingInMainSignalCatcherLoop:
    case art::ThreadState::kWaitingForMethodTracingStart:
    case art::ThreadState::kWaitingForVisitObjects:
    case art::ThreadState::kWaitingForGcThreadFlip:
      return JVMTI_JAVA_LANG_THREAD_STATE_WAITING;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

jvmtiError ThreadUtil::GetThreadState(jvmtiEnv* env ATTRIBUTE_UNUSED,
                                      jthread thread,
                                      jint* thread_state_ptr) {
  if (thread_state_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  art::ScopedObjectAccess soa(art::Thread::Current());
  art::Thread* native_thread = nullptr;
  art::ThreadState internal_thread_state = GetNativeThreadState(thread, soa, &native_thread);

  if (internal_thread_state == art::ThreadState::kStarting) {
    if (thread == nullptr) {
      // No native thread, and no Java thread? We must be starting up. Report as wrong phase.
      return ERR(WRONG_PHASE);
    }

    // Need to read the Java "started" field to know whether this is starting or terminated.
    art::ObjPtr<art::mirror::Object> peer = soa.Decode<art::mirror::Object>(thread);
    art::ObjPtr<art::mirror::Class> klass = peer->GetClass();
    art::ArtField* started_field = klass->FindDeclaredInstanceField("started", "Z");
    CHECK(started_field != nullptr);
    bool started = started_field->GetBoolean(peer) != 0;
    constexpr jint kStartedState = JVMTI_JAVA_LANG_THREAD_STATE_NEW;
    constexpr jint kTerminatedState = JVMTI_THREAD_STATE_TERMINATED |
                                      JVMTI_JAVA_LANG_THREAD_STATE_TERMINATED;
    *thread_state_ptr = started ? kTerminatedState : kStartedState;
    return ERR(NONE);
  }
  DCHECK(native_thread != nullptr);

  // Translate internal thread state to JVMTI and Java state.
  jint jvmti_state = GetJvmtiThreadStateFromInternal(internal_thread_state);
  if (native_thread->IsInterrupted()) {
    jvmti_state |= JVMTI_THREAD_STATE_INTERRUPTED;
  }

  // Java state is derived from nativeGetState.
  // Note: Our implementation assigns "runnable" to suspended. As such, we will have slightly
  //       different mask. However, this is for consistency with the Java view.
  jint java_state = GetJavaStateFromInternal(internal_thread_state);

  *thread_state_ptr = jvmti_state | java_state;

  return ERR(NONE);
}

}  // namespace openjdkjvmti
