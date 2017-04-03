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

#ifndef ART_TEST_TI_AGENT_AGENT_STARTUP_H_
#define ART_TEST_TI_AGENT_AGENT_STARTUP_H_

#include <functional>

#include "jni.h"
#include "jvmti.h"

namespace art {

using StartCallback = void(*)(jvmtiEnv*, JNIEnv*);

// Ensure binding of the Main class when the agent is started through OnLoad.
void BindOnLoad(JavaVM* vm, StartCallback callback);

// Ensure binding of the Main class when the agent is started through OnAttach.
void BindOnAttach(JavaVM* vm, StartCallback callback);

}  // namespace art

#endif  // ART_TEST_TI_AGENT_AGENT_STARTUP_H_
