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

#include "native_dex_file.h"

namespace art {

const uint8_t NativeDexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const uint8_t NativeDexFile::kDexMagicVersions[NativeDexFile::kNumDexVersions]
                                              [NativeDexFile::kDexVersionLen] = {
  {'0', '3', '5', '\0'},
  // Dex version 036 skipped because of an old dalvik bug on some versions of android where dex
  // files with that version number would erroneously be accepted and run.
  {'0', '3', '7', '\0'},
  // Dex version 038: Android "O" and beyond.
  {'0', '3', '8', '\0'},
  // Dex verion 039: Beyond Android "O".
  {'0', '3', '9', '\0'},
};

bool NativeDexFile::IsMagicValid(const uint8_t* magic) {
  return (memcmp(magic, kDexMagic, sizeof(kDexMagic)) == 0);
}

bool NativeDexFile::IsVersionValid(const uint8_t* magic) {
  const uint8_t* version = &magic[sizeof(kDexMagic)];
  for (uint32_t i = 0; i < kNumDexVersions; i++) {
    if (memcmp(version, kDexMagicVersions[i], kDexVersionLen) == 0) {
      return true;
    }
  }
  return false;
}

bool NativeDexFile::IsMagicValid() const {
  return IsMagicValid(header_->magic_);
}

bool NativeDexFile::IsVersionValid() const {
  return IsVersionValid(header_->magic_);
}

}  // namespace art
