/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "methods.h"

#include <stdio.h>

#include "base/macros.h"
#include "jni.h"
#include "openjdkjvmti/jvmti.h"
#include "ScopedLocalRef.h"

#include "ti-agent/common_helper.h"
#include "ti-agent/common_load.h"

namespace art {
namespace Test910Methods {

extern "C" JNIEXPORT jobjectArray JNICALL Java_Main_getMethodName(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  char* name;
  char* sig;
  char* gen;
  jvmtiError result = jvmti_env->GetMethodName(id, &name, &sig, &gen);
  if (result != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result, &err);
    printf("Failure running GetMethodName: %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return nullptr;
  }

  auto callback = [&](jint i) {
    if (i == 0) {
      return name == nullptr ? nullptr : env->NewStringUTF(name);
    } else if (i == 1) {
      return sig == nullptr ? nullptr : env->NewStringUTF(sig);
    } else {
      return gen == nullptr ? nullptr : env->NewStringUTF(gen);
    }
  };
  jobjectArray ret = CreateObjectArray(env, 3, "java/lang/String", callback);

  // Need to deallocate the strings.
  if (name != nullptr) {
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(name));
  }
  if (sig != nullptr) {
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(sig));
  }
  if (gen != nullptr) {
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(gen));
  }

  // Also run GetMethodName with all parameter pointers null to check for segfaults.
  jvmtiError result2 = jvmti_env->GetMethodName(id, nullptr, nullptr, nullptr);
  if (result2 != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result2, &err);
    printf("Failure running GetMethodName(null, null, null): %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return nullptr;
  }

  return ret;
}

extern "C" JNIEXPORT jclass JNICALL Java_Main_getMethodDeclaringClass(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jclass declaring_class;
  jvmtiError result = jvmti_env->GetMethodDeclaringClass(id, &declaring_class);
  if (result != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result, &err);
    printf("Failure running GetMethodDeclaringClass: %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return nullptr;
  }

  return declaring_class;
}

extern "C" JNIEXPORT jint JNICALL Java_Main_getMethodModifiers(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jint modifiers;
  jvmtiError result = jvmti_env->GetMethodModifiers(id, &modifiers);
  if (result != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result, &err);
    printf("Failure running GetMethodModifiers: %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return 0;
  }

  return modifiers;
}

static bool ErrorToException(JNIEnv* env, jvmtiError error) {
  if (error == JVMTI_ERROR_NONE) {
    return false;
  }

  ScopedLocalRef<jclass> rt_exception(env, env->FindClass("java/lang/RuntimeException"));
  if (rt_exception.get() == nullptr) {
    // CNFE should be pending.
    return true;
  }

  char* err;
  jvmti_env->GetErrorName(error, &err);

  env->ThrowNew(rt_exception.get(), err);

  jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
  return true;
}

extern "C" JNIEXPORT jint JNICALL Java_Main_getMaxLocals(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jint max_locals;
  jvmtiError result = jvmti_env->GetMaxLocals(id, &max_locals);
  if (ErrorToException(env, result)) {
    return -1;
  }

  return max_locals;
}

extern "C" JNIEXPORT jint JNICALL Java_Main_getArgumentsSize(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jint arguments;
  jvmtiError result = jvmti_env->GetArgumentsSize(id, &arguments);
  if (ErrorToException(env, result)) {
    return -1;
  }

  return arguments;
}

extern "C" JNIEXPORT jlong JNICALL Java_Main_getMethodLocationStart(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jlong start;
  jlong end;
  jvmtiError result = jvmti_env->GetMethodLocation(id, &start, &end);
  if (ErrorToException(env, result)) {
    return -1;
  }

  return start;
}

extern "C" JNIEXPORT jlong JNICALL Java_Main_getMethodLocationEnd(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jobject method) {
  jmethodID id = env->FromReflectedMethod(method);

  jlong start;
  jlong end;
  jvmtiError result = jvmti_env->GetMethodLocation(id, &start, &end);
  if (ErrorToException(env, result)) {
    return -1;
  }

  return end;
}

// Don't do anything
jint OnLoad(JavaVM* vm,
            char* options ATTRIBUTE_UNUSED,
            void* reserved ATTRIBUTE_UNUSED) {
  if (vm->GetEnv(reinterpret_cast<void**>(&jvmti_env), JVMTI_VERSION_1_0)) {
    printf("Unable to get jvmti env!\n");
    return 1;
  }
  SetAllCapabilities(jvmti_env);
  return 0;
}

}  // namespace Test910Methods
}  // namespace art
