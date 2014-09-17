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

#include "monitor.h"

namespace art {

// BEGIN Motorola, IKJBXLINE-4551, w17724, 04/11/2013 */
void Monitor::LogContentionEvent(Thread*, uint32_t, uint32_t, const char*, const char*, int32_t) {
// END IKJBXLINE-4551
}

}  // namespace art
