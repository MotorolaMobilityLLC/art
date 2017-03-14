/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "901-hello-ti-agent/basics.h"

#include <jni.h>
#include <stdio.h>
#include <string.h>
#include "base/macros.h"
#include "jvmti.h"

#include "ti-agent/common_helper.h"
#include "ti-agent/common_load.h"

namespace art {
namespace Test901HelloTi {

static void EnableEvent(jvmtiEnv* env, jvmtiEvent evt) {
  jvmtiError error = env->SetEventNotificationMode(JVMTI_ENABLE, evt, nullptr);
  if (error != JVMTI_ERROR_NONE) {
    printf("Failed to enable event");
  }
}

static void JNICALL VMStartCallback(jvmtiEnv *jenv ATTRIBUTE_UNUSED,
                                     JNIEnv* jni_env ATTRIBUTE_UNUSED) {
  printf("VMStart\n");
}

static void JNICALL VMInitCallback(jvmtiEnv *jvmti_env ATTRIBUTE_UNUSED,
                                   JNIEnv* jni_env ATTRIBUTE_UNUSED,
                                   jthread thread ATTRIBUTE_UNUSED) {
  printf("VMInit\n");
}

static void JNICALL VMDeatchCallback(jvmtiEnv *jenv ATTRIBUTE_UNUSED,
                                     JNIEnv* jni_env ATTRIBUTE_UNUSED) {
  printf("VMDeath\n");
}


static void InstallVMEvents(jvmtiEnv* env) {
  jvmtiEventCallbacks callbacks;
  memset(&callbacks, 0, sizeof(jvmtiEventCallbacks));
  callbacks.VMStart = VMStartCallback;
  callbacks.VMInit = VMInitCallback;
  callbacks.VMDeath = VMDeatchCallback;
  jvmtiError ret = env->SetEventCallbacks(&callbacks, sizeof(callbacks));
  if (ret != JVMTI_ERROR_NONE) {
    printf("Failed to install callbacks");
  }

  EnableEvent(env, JVMTI_EVENT_VM_START);
  EnableEvent(env, JVMTI_EVENT_VM_INIT);
  EnableEvent(env, JVMTI_EVENT_VM_DEATH);
}

jint OnLoad(JavaVM* vm,
            char* options ATTRIBUTE_UNUSED,
            void* reserved ATTRIBUTE_UNUSED) {
  printf("Loaded Agent for test 901-hello-ti-agent\n");
  fsync(1);
  jvmtiEnv* env = nullptr;
  jvmtiEnv* env2 = nullptr;

#define CHECK_CALL_SUCCESS(c) \
  do { \
    if ((c) != JNI_OK) { \
      printf("call " #c " did not succeed\n"); \
      return -1; \
    } \
  } while (false)

  CHECK_CALL_SUCCESS(vm->GetEnv(reinterpret_cast<void**>(&env), JVMTI_VERSION_1_0));
  CHECK_CALL_SUCCESS(vm->GetEnv(reinterpret_cast<void**>(&env2), JVMTI_VERSION_1_0));
  if (env == env2) {
    printf("GetEnv returned same environment twice!\n");
    return -1;
  }
  unsigned char* local_data = nullptr;
  CHECK_CALL_SUCCESS(env->Allocate(8, &local_data));
  strcpy(reinterpret_cast<char*>(local_data), "hello!!");
  CHECK_CALL_SUCCESS(env->SetEnvironmentLocalStorage(local_data));
  unsigned char* get_data = nullptr;
  CHECK_CALL_SUCCESS(env->GetEnvironmentLocalStorage(reinterpret_cast<void**>(&get_data)));
  if (get_data != local_data) {
    printf("Got different data from local storage then what was set!\n");
    return -1;
  }
  CHECK_CALL_SUCCESS(env2->GetEnvironmentLocalStorage(reinterpret_cast<void**>(&get_data)));
  if (get_data != nullptr) {
    printf("env2 did not have nullptr local storage.\n");
    return -1;
  }
  CHECK_CALL_SUCCESS(env->Deallocate(local_data));
  jint version = 0;
  CHECK_CALL_SUCCESS(env->GetVersionNumber(&version));
  if ((version & JVMTI_VERSION_1) != JVMTI_VERSION_1) {
    printf("Unexpected version number!\n");
    return -1;
  }

  InstallVMEvents(env);
  InstallVMEvents(env2);

  CHECK_CALL_SUCCESS(env->DisposeEnvironment());
  CHECK_CALL_SUCCESS(env2->DisposeEnvironment());
#undef CHECK_CALL_SUCCESS

  if (vm->GetEnv(reinterpret_cast<void**>(&jvmti_env), JVMTI_VERSION_1_0)) {
    printf("Unable to get jvmti env!\n");
    return 1;
  }
  SetAllCapabilities(jvmti_env);

  jvmtiPhase current_phase;
  jvmtiError phase_result = jvmti_env->GetPhase(&current_phase);
  if (phase_result != JVMTI_ERROR_NONE) {
    printf("Could not get phase");
    return 1;
  }
  if (current_phase != JVMTI_PHASE_ONLOAD) {
    printf("Wrong phase");
    return 1;
  }

  InstallVMEvents(jvmti_env);

  return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL Java_Main_setVerboseFlag(
    JNIEnv* env, jclass Main_klass ATTRIBUTE_UNUSED, jint iflag, jboolean val) {
  jvmtiVerboseFlag flag = static_cast<jvmtiVerboseFlag>(iflag);
  jvmtiError result = jvmti_env->SetVerboseFlag(flag, val);
  JvmtiErrorToException(env, result);
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_checkLivePhase(
    JNIEnv* env, jclass Main_klass ATTRIBUTE_UNUSED) {
  jvmtiPhase current_phase;
  jvmtiError phase_result = jvmti_env->GetPhase(&current_phase);
  if (JvmtiErrorToException(env, phase_result)) {
    return JNI_FALSE;
  }
  return (current_phase == JVMTI_PHASE_LIVE) ? JNI_TRUE : JNI_FALSE;
}

}  // namespace Test901HelloTi
}  // namespace art
