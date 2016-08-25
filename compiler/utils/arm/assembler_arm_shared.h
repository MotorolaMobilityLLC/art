/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_SHARED_H_
#define ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_SHARED_H_

namespace art {
namespace arm {

enum LoadOperandType {
  kLoadSignedByte,
  kLoadUnsignedByte,
  kLoadSignedHalfword,
  kLoadUnsignedHalfword,
  kLoadWord,
  kLoadWordPair,
  kLoadSWord,
  kLoadDWord
};

enum StoreOperandType {
  kStoreByte,
  kStoreHalfword,
  kStoreWord,
  kStoreWordPair,
  kStoreSWord,
  kStoreDWord
};

// Set condition codes request.
enum SetCc {
  kCcDontCare,  // Allows prioritizing 16-bit instructions on Thumb2 whether they set CCs or not.
  kCcSet,
  kCcKeep,
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_SHARED_H_
