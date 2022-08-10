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

#ifndef ART_RUNTIME_READ_BARRIER_CONFIG_H_
#define ART_RUNTIME_READ_BARRIER_CONFIG_H_

// This is a mixed C-C++ header file that has a global section at the start
// and a C++ section at the end, because asm_support.h is a C header file and
// cannot include any C++ syntax.

// Global (C) part.

// Uncomment one of the following two and the two fields in
// Object.java (libcore) to enable baker, or
// table-lookup read barriers.

#ifdef ART_USE_READ_BARRIER
#if ART_READ_BARRIER_TYPE_IS_BAKER
#define USE_BAKER_READ_BARRIER
#elif ART_READ_BARRIER_TYPE_IS_TABLELOOKUP
#define USE_TABLE_LOOKUP_READ_BARRIER
#else
#error "ART read barrier type must be set"
#endif
#endif  // ART_USE_READ_BARRIER

#if defined(USE_BAKER_READ_BARRIER) || defined(USE_TABLE_LOOKUP_READ_BARRIER)
#define USE_READ_BARRIER
#endif


// C++-specific configuration part..

#ifdef __cplusplus

#include "base/globals.h"

namespace art {

#ifdef USE_BAKER_READ_BARRIER
static constexpr bool kUseBakerReadBarrier = true;
#else
static constexpr bool kUseBakerReadBarrier = false;
#endif

#ifdef USE_TABLE_LOOKUP_READ_BARRIER
static constexpr bool kUseTableLookupReadBarrier = true;
#else
static constexpr bool kUseTableLookupReadBarrier = false;
#endif

extern const bool gUseReadBarrier;
extern const bool gUseUserfaultfd;

// Disabled for performance reasons.
static constexpr bool kCheckDebugDisallowReadBarrierCount = kIsDebugBuild;

}  // namespace art

#endif  // __cplusplus

#endif  // ART_RUNTIME_READ_BARRIER_CONFIG_H_
