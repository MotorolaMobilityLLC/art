/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_LOCK_WORD_INL_H_
#define ART_RUNTIME_LOCK_WORD_INL_H_

#include "lock_word.h"

namespace art {

inline uint32_t LockWord::ThinLockOwner() const {
  DCHECK_EQ(GetState(), kThinLocked);
  return (value_ >> kThinLockOwnerShift) & kThinLockOwnerMask;
}

inline uint32_t LockWord::ThinLockCount() const {
  DCHECK_EQ(GetState(), kThinLocked);
  return (value_ >> kThinLockCountShift) & kThinLockCountMask;
}

inline Monitor* LockWord::FatLockMonitor() const {
  DCHECK_EQ(GetState(), kFatLocked);
  return reinterpret_cast<Monitor*>(value_ << 1);
}

inline LockWord::LockWord() : value_(0) {
  DCHECK_EQ(GetState(), kUnlocked);
}

inline LockWord::LockWord(Monitor* mon)
    : value_((reinterpret_cast<uint32_t>(mon) >> 1) | (kStateFat << kStateShift)) {
  DCHECK_EQ(FatLockMonitor(), mon);
}

}  // namespace art

#endif  // ART_RUNTIME_LOCK_WORD_INL_H_
