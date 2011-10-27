/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "debugger.h"
#include "logging.h"
#include "stack.h"
#include "thread_list.h"

#include "JniConstants.h"  // Last to avoid problems with LOG redefinition.
#include "ScopedPrimitiveArray.h"  // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

static void DdmVmInternal_enableRecentAllocations(JNIEnv* env, jclass, jboolean enable) {
  UNIMPLEMENTED(WARNING);
  if (enable) {
    //dvmEnableAllocTracker();
  } else {
    //dvmDisableAllocTracker();
  }
}

static jbyteArray DdmVmInternal_getRecentAllocations(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return NULL;
  //ArrayObject* data = dvmDdmGetRecentAllocations();
  //dvmReleaseTrackedAlloc(data, NULL);
  //return reinterpret_cast<jbyteArray>(addLocalReference(env, data));
}

static jboolean DdmVmInternal_getRecentAllocationStatus(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE;
  //return (gDvm.allocRecords != NULL);
}

static Thread* FindThreadByThinLockId(uint32_t thin_lock_id) {
  struct ThreadFinder {
    ThreadFinder(uint32_t thin_lock_id) : thin_lock_id(thin_lock_id), thread(NULL) {
    }

    static void Callback(Thread* t, void* context) {
      ThreadFinder* finder = reinterpret_cast<ThreadFinder*>(context);
      if (t->GetThinLockId() == finder->thin_lock_id) {
        finder->thread = t;
      }
    }

    uint32_t thin_lock_id;
    Thread* thread;
  };
  ThreadFinder finder(thin_lock_id);
  Runtime::Current()->GetThreadList()->ForEach(ThreadFinder::Callback, &finder);
  return finder.thread;
}

/*
 * Get a stack trace as an array of StackTraceElement objects.  Returns
 * NULL on failure, e.g. if the threadId couldn't be found.
 */
static jobjectArray DdmVmInternal_getStackTraceById(JNIEnv* env, jclass, jint thin_lock_id) {
  ThreadListLock thread_list_lock;
  Thread* thread = FindThreadByThinLockId(static_cast<uint32_t>(thin_lock_id));
  if (thread == NULL) {
    return NULL;
  }
  jobject stack = GetThreadStack(env, thread);
  return (stack != NULL) ? Thread::InternalStackTraceToStackTraceElementArray(env, stack) : NULL;
}

static void ThreadCountCallback(Thread*, void* context) {
  uint16_t& count = *reinterpret_cast<uint16_t*>(context);
  ++count;
}

static const int kThstBytesPerEntry = 18;
static const int kThstHeaderLen = 4;

static void ThreadStatsGetterCallback(Thread* t, void* context) {
  uint8_t** ptr = reinterpret_cast<uint8_t**>(context);
  uint8_t* buf = *ptr;

  /*
   * Generate the contents of a THST chunk.  The data encompasses all known
   * threads.
   *
   * Response has:
   *  (1b) header len
   *  (1b) bytes per entry
   *  (2b) thread count
   * Then, for each thread:
   *  (4b) threadId
   *  (1b) thread status
   *  (4b) tid
   *  (4b) utime
   *  (4b) stime
   *  (1b) is daemon?
   *
   * The length fields exist in anticipation of adding additional fields
   * without wanting to break ddms or bump the full protocol version.  I don't
   * think it warrants full versioning.  They might be extraneous and could
   * be removed from a future version.
   */
  int utime, stime, task_cpu;
  GetTaskStats(t->GetTid(), utime, stime, task_cpu);

  JDWP::set4BE(buf+0, t->GetThinLockId());
  JDWP::set1(buf+4, t->GetState());
  JDWP::set4BE(buf+5, t->GetTid());
  JDWP::set4BE(buf+9, utime);
  JDWP::set4BE(buf+13, stime);
  JDWP::set1(buf+17, t->IsDaemon());

  *ptr += kThstBytesPerEntry;
}

static jbyteArray DdmVmInternal_getThreadStats(JNIEnv* env, jclass) {
  std::vector<uint8_t> bytes;
  {
    ThreadListLock thread_list_lock;
    ThreadList* thread_list = Runtime::Current()->GetThreadList();

    uint16_t thread_count;
    thread_list->ForEach(ThreadCountCallback, &thread_count);

    bytes.resize(kThstHeaderLen + thread_count * kThstBytesPerEntry);

    JDWP::set1(&bytes[0], kThstHeaderLen);
    JDWP::set1(&bytes[1], kThstBytesPerEntry);
    JDWP::set2BE(&bytes[2], thread_count);

    uint8_t* ptr = &bytes[kThstHeaderLen];
    thread_list->ForEach(ThreadStatsGetterCallback, &ptr);
  }

  jbyteArray result = env->NewByteArray(bytes.size());
  env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  return result;
}

static jint DdmVmInternal_heapInfoNotify(JNIEnv* env, jclass, jint when) {
  return Dbg::DdmHandleHpifChunk(static_cast<Dbg::HpifWhen>(when));
}

static jboolean DdmVmInternal_heapSegmentNotify(JNIEnv* env, jclass, jint when, jint what, jboolean native) {
  return Dbg::DdmHandleHpsgNhsgChunk(static_cast<Dbg::HpsgWhen>(when), static_cast<Dbg::HpsgWhat>(what), native);
}

static void DdmVmInternal_threadNotify(JNIEnv* env, jclass, jboolean enable) {
  Dbg::DdmSetThreadNotification(enable);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DdmVmInternal, enableRecentAllocations, "(Z)V"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocations, "()[B"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocationStatus, "()Z"),
  NATIVE_METHOD(DdmVmInternal, getStackTraceById, "(I)[Ljava/lang/StackTraceElement;"),
  NATIVE_METHOD(DdmVmInternal, getThreadStats, "()[B"),
  NATIVE_METHOD(DdmVmInternal, heapInfoNotify, "(I)Z"),
  NATIVE_METHOD(DdmVmInternal, heapSegmentNotify, "(IIZ)Z"),
  NATIVE_METHOD(DdmVmInternal, threadNotify, "(Z)V"),
};

}  // namespace

void register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(JNIEnv* env) {
  jniRegisterNativeMethods(env, "org/apache/harmony/dalvik/ddmc/DdmVmInternal", gMethods, NELEM(gMethods));
}

}  // namespace art
