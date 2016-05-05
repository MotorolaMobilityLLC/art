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

#include "gc/heap.h"
#include "jni.h"
#include "monitor.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {
namespace {

extern "C" JNIEXPORT void JNICALL Java_Main_trim(JNIEnv*, jclass) {
  Runtime::Current()->GetHeap()->Trim(Thread::Current());
}

extern "C" JNIEXPORT jint JNICALL Java_Main_monitorListSize(JNIEnv*, jclass) {
  return Runtime::Current()->GetMonitorList()->Size();
}

}  // namespace
}  // namespace art
