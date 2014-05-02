/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_READ_BARRIER_OPTION_H_
#define ART_RUNTIME_READ_BARRIER_OPTION_H_
namespace art {

// Options for performing a read barrier or not.
enum ReadBarrierOption {
  kWithReadBarrier,     // Perform a read barrier.
  kWithoutReadBarrier,  // Don't perform a read barrier.
};

}  // namespace art

#endif  // ART_RUNTIME_READ_BARRIER_OPTION_H_
