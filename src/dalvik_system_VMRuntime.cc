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

#include "class_linker.h"
#include "debugger.h"
#include "jni_internal.h"
#include "object.h"
#include "object_utils.h"
#include "scoped_heap_lock.h"
#include "scoped_thread_list_lock.h"
#include "space.h"
#include "thread.h"
#include "thread_list.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.
#include "toStringArray.h"

#include <limits.h>

namespace art {

static jfloat VMRuntime_getTargetHeapUtilization(JNIEnv*, jobject) {
  return Runtime::Current()->GetHeap()->GetTargetHeapUtilization();
}

static void VMRuntime_nativeSetTargetHeapUtilization(JNIEnv*, jobject, jfloat target) {
  Runtime::Current()->GetHeap()->SetTargetHeapUtilization(target);
}

static void VMRuntime_startJitCompilation(JNIEnv*, jobject) {
}

static void VMRuntime_disableJitCompilation(JNIEnv*, jobject) {
}

static jobject VMRuntime_newNonMovableArray(JNIEnv* env, jobject, jclass javaElementClass, jint length) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
#ifdef MOVING_GARBAGE_COLLECTOR
  // TODO: right now, we don't have a copying collector, so there's no need
  // to do anything special here, but we ought to pass the non-movability
  // through to the allocator.
  UNIMPLEMENTED(FATAL);
#endif

  Class* element_class = Decode<Class*>(env, javaElementClass);
  if (element_class == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", "element class == null");
    return NULL;
  }
  if (length < 0) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", length);
    return NULL;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  std::string descriptor;
  descriptor += "[";
  descriptor += ClassHelper(element_class).GetDescriptor();
  Class* array_class = class_linker->FindClass(descriptor.c_str(), NULL);
  Array* result = Array::Alloc(array_class, length);
  if (result == NULL) {
    return NULL;
  }
  return AddLocalReference<jobject>(env, result);
}

static jlong VMRuntime_addressOf(JNIEnv* env, jobject, jobject javaArray) {
  if (javaArray == NULL) {  // Most likely allocation failed
    return 0;
  }
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Array* array = Decode<Array*>(env, javaArray);
  if (!array->IsArrayInstance()) {
    Thread::Current()->ThrowNewException("Ljava/lang/IllegalArgumentException;", "not an array");
    return 0;
  }
  // TODO: we should also check that this is a non-movable array.
  return reinterpret_cast<uintptr_t>(array->GetRawData(array->GetClass()->GetComponentSize()));
}

static void VMRuntime_clearGrowthLimit(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->ClearGrowthLimit();
}

static jboolean VMRuntime_isDebuggerActive(JNIEnv*, jobject) {
  return Dbg::IsDebuggerActive();
}

static jobjectArray VMRuntime_properties(JNIEnv* env, jobject) {
  return toStringArray(env, Runtime::Current()->GetProperties());
}

// This is for backward compatibility with dalvik which returned the
// meaningless "." when no boot classpath or classpath was
// specified. Unfortunately, some tests were using java.class.path to
// lookup relative file locations, so they are counting on this to be
// ".", presumably some applications or libraries could have as well.
static const char* DefaultToDot(const std::string& class_path) {
  return class_path.empty() ? "." : class_path.c_str();
}

static jstring VMRuntime_bootClassPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetBootClassPathString()));
}

static jstring VMRuntime_classPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetClassPathString()));
}

static jstring VMRuntime_vmVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(Runtime::Current()->GetVersion());
}

static void DisableCheckJniCallback(Thread* t, void*) {
  t->GetJniEnv()->SetCheckJniEnabled(false);
}

static void VMRuntime_setTargetSdkVersion(JNIEnv*, jobject, jint targetSdkVersion) {
  // This is the target SDK version of the app we're about to run.
  // Note that targetSdkVersion may be CUR_DEVELOPMENT (10000).
  // Note that targetSdkVersion may be 0, meaning "current".
  if (targetSdkVersion > 0 && targetSdkVersion <= 13 /* honeycomb-mr2 */) {
    Runtime* runtime = Runtime::Current();
    JavaVMExt* vm = runtime->GetJavaVM();

    if (vm->check_jni) {
      LOG(WARNING) << "Turning off CheckJNI so we can turn on JNI app bug workarounds...";
      ScopedThreadListLock thread_list_lock;
      vm->SetCheckJniEnabled(false);
      runtime->GetThreadList()->ForEach(DisableCheckJniCallback, NULL);
    }

    LOG(INFO) << "Turning on JNI app bug workarounds for target SDK version "
              << targetSdkVersion << "...";
    vm->work_around_app_jni_bugs = true;
  }
}

static void VMRuntime_trimHeap(JNIEnv*, jobject) {
  ScopedHeapLock heap_lock;
  Heap* heap = Runtime::Current()->GetHeap();
  size_t alloc_space_size = heap->GetAllocSpace()->Size();
  float utilization = static_cast<float>(heap->GetBytesAllocated()) / alloc_space_size;
  uint64_t start_ns = NanoTime();
  heap->GetAllocSpace()->Trim();
  LOG(INFO) << "Parallel heap trimming took " << PrettyDuration(NanoTime() - start_ns)
            << " on a " << PrettySize(alloc_space_size)
            << " heap with " << static_cast<int>(100 * utilization) << "% utilization";
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMRuntime, addressOf, "(Ljava/lang/Object;)J"),
  NATIVE_METHOD(VMRuntime, bootClassPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, classPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, clearGrowthLimit, "()V"),
  NATIVE_METHOD(VMRuntime, disableJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, getTargetHeapUtilization, "()F"),
  NATIVE_METHOD(VMRuntime, isDebuggerActive, "()Z"),
  NATIVE_METHOD(VMRuntime, nativeSetTargetHeapUtilization, "(F)V"),
  NATIVE_METHOD(VMRuntime, newNonMovableArray, "(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, properties, "()[Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, setTargetSdkVersion, "(I)V"),
  NATIVE_METHOD(VMRuntime, startJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, trimHeap, "()V"),
  NATIVE_METHOD(VMRuntime, vmVersion, "()Ljava/lang/String;"),
};

void register_dalvik_system_VMRuntime(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/VMRuntime", gMethods, NELEM(gMethods));
}

}  // namespace art
