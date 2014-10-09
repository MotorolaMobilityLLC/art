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

#ifndef ART_RUNTIME_BASE_BIT_FIELD_H_
#define ART_RUNTIME_BASE_BIT_FIELD_H_

#include "globals.h"
#include "logging.h"

namespace art {

static constexpr uintptr_t kUintPtrTOne = 1U;

// BitField is a template for encoding and decoding a bit field inside
// an unsigned machine word.
template<typename T, int position, int size>
class BitField {
 public:
  // Tells whether the provided value fits into the bit field.
  static bool IsValid(T value) {
    return (static_cast<uintptr_t>(value) & ~((kUintPtrTOne << size) - 1)) == 0;
  }

  // Returns a uword mask of the bit field.
  static uintptr_t Mask() {
    return (kUintPtrTOne << size) - 1;
  }

  // Returns a uword mask of the bit field which can be applied directly to
  // the raw unshifted bits.
  static uintptr_t MaskInPlace() {
    return ((kUintPtrTOne << size) - 1) << position;
  }

  // Returns the shift count needed to right-shift the bit field to
  // the least-significant bits.
  static int Shift() {
    return position;
  }

  // Returns the size of the bit field.
  static int BitSize() {
    return size;
  }

  // Returns a uword with the bit field value encoded.
  static uintptr_t Encode(T value) {
    DCHECK(IsValid(value));
    return static_cast<uintptr_t>(value) << position;
  }

  // Extracts the bit field from the value.
  static T Decode(uintptr_t value) {
    return static_cast<T>((value >> position) & ((kUintPtrTOne << size) - 1));
  }

  // Returns a uword with the bit field value encoded based on the
  // original value. Only the bits corresponding to this bit field
  // will be changed.
  static uintptr_t Update(T value, uintptr_t original) {
    DCHECK(IsValid(value));
    return (static_cast<uintptr_t>(value) << position) |
        (~MaskInPlace() & original);
  }
};

}  // namespace art

#endif  // ART_RUNTIME_BASE_BIT_FIELD_H_
