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

#include "objects.h"

#include <stdio.h>

#include "base/macros.h"
#include "jni.h"
#include "openjdkjvmti/jvmti.h"
#include "ScopedLocalRef.h"

#include "ti-agent/common_helper.h"
#include "ti-agent/common_load.h"

namespace art {
namespace Test920Objects {

extern "C" JNIEXPORT jlong JNICALL Java_Main_getObjectSize(
    JNIEnv* env ATTRIBUTE_UNUSED, jclass klass ATTRIBUTE_UNUSED, jobject object) {
  jlong size;

  jvmtiError result = jvmti_env->GetObjectSize(object, &size);
  if (result != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result, &err);
    printf("Failure running GetObjectSize: %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return -1;
  }

  return size;
}

extern "C" JNIEXPORT jint JNICALL Java_Main_getObjectHashCode(
    JNIEnv* env ATTRIBUTE_UNUSED, jclass klass ATTRIBUTE_UNUSED, jobject object) {
  jint hash;

  jvmtiError result = jvmti_env->GetObjectHashCode(object, &hash);
  if (result != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(result, &err);
    printf("Failure running GetObjectHashCode: %s\n", err);
    jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(err));
    return -1;
  }

  return hash;
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

}  // namespace Test920Objects
}  // namespace art
