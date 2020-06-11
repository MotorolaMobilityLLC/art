/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef ART_PERFETTO_HPROF_PERFETTO_HPROF_H_
#define ART_PERFETTO_HPROF_PERFETTO_HPROF_H_

#include <ostream>

namespace perfetto_hprof {

enum class State {
  // Worker thread not spawned.
  kUninitialized,
  // Worker thread spawned, waiting for ACK.
  kWaitForListener,
  // Worker thread ready, waiting for data-source.
  kWaitForStart,
  // These are only in the forked process:
  // Data source received, start dump.
  kStart,
  // Dump finished. Kill forked child.
  kEnd,
};

std::ostream& operator<<(std::ostream& os, State state);

}  // namespace perfetto_hprof

#endif  // ART_PERFETTO_HPROF_PERFETTO_HPROF_H_
