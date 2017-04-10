/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "agent_startup.h"

#include "android-base/logging.h"
#include "android-base/macros.h"

#include "jni_binder.h"
#include "jvmti_helper.h"
#include "scoped_utf_chars.h"
#include "test_env.h"

namespace art {

static constexpr const char* kMainClass = "art/Main";

static StartCallback gCallback = nullptr;

// TODO: Check this. This may not work on device. The classloader containing the app's classes
//       may not have been created at this point (i.e., if it's not the system classloader).
static void JNICALL VMInitCallback(jvmtiEnv* callback_jvmti_env,
                                   JNIEnv* jni_env,
                                   jthread thread ATTRIBUTE_UNUSED) {
  // Bind kMainClass native methods.
  BindFunctions(callback_jvmti_env, jni_env, kMainClass);

  if (gCallback != nullptr) {
    gCallback(callback_jvmti_env, jni_env);
    gCallback = nullptr;
  }

  // And delete the jvmtiEnv.
  callback_jvmti_env->DisposeEnvironment();
}

// Install a phase callback that will bind JNI functions on VMInit.
void BindOnLoad(JavaVM* vm, StartCallback callback) {
  // Use a new jvmtiEnv. Otherwise we might collide with table changes.
  jvmtiEnv* install_env;
  if (vm->GetEnv(reinterpret_cast<void**>(&install_env), JVMTI_VERSION_1_0) != 0) {
    LOG(FATAL) << "Could not get jvmtiEnv";
  }
  SetAllCapabilities(install_env);

  {
    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(jvmtiEventCallbacks));
    callbacks.VMInit = VMInitCallback;

    CheckJvmtiError(install_env, install_env->SetEventCallbacks(&callbacks, sizeof(callbacks)));
  }

  CheckJvmtiError(install_env, install_env->SetEventNotificationMode(JVMTI_ENABLE,
                                                                     JVMTI_EVENT_VM_INIT,
                                                                     nullptr));

  gCallback = callback;
}

// Ensure binding of the Main class when the agent is started through OnAttach.
void BindOnAttach(JavaVM* vm, StartCallback callback) {
  // Get a JNIEnv. As the thread is attached, we must not destroy it.
  JNIEnv* env;
  CHECK_EQ(0, vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6))
      << "Could not get JNIEnv";

  jvmtiEnv* bind_jvmti_env;
  CHECK_EQ(0, vm->GetEnv(reinterpret_cast<void**>(&bind_jvmti_env), JVMTI_VERSION_1_0))
      << "Could not get jvmtiEnv";
  SetAllCapabilities(bind_jvmti_env);

  BindFunctions(bind_jvmti_env, env, kMainClass);

  if (callback != nullptr) {
    callback(bind_jvmti_env, env);
  }

  if (bind_jvmti_env->DisposeEnvironment() != JVMTI_ERROR_NONE) {
    LOG(FATAL) << "Could not dispose temporary jvmtiEnv";
  }
}

// Utility functions for art.Main shim.
extern "C" JNIEXPORT void JNICALL Java_art_Main_bindAgentJNI(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jstring className, jobject classLoader) {
  ScopedUtfChars name(env, className);
  BindFunctions(jvmti_env, env, name.c_str(), classLoader);
}

extern "C" JNIEXPORT void JNICALL Java_art_Main_bindAgentJNIForClass(
    JNIEnv* env, jclass klass ATTRIBUTE_UNUSED, jclass bindClass) {
  BindFunctionsOnClass(jvmti_env, env, bindClass);
}

}  // namespace art
