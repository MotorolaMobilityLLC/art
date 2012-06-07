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

#ifndef ART_SRC_UTILS_LLVM_H_
#define ART_SRC_UTILS_LLVM_H_

#include "stringprintf.h"

#include <llvm/Analysis/Verifier.h>

#include <stdint.h>
#include <string>

namespace art {

#ifndef NDEBUG
#define VERIFY_LLVM_FUNCTION(func) llvm::verifyFunction(func, llvm::PrintMessageAction)
#else
#define VERIFY_LLVM_FUNCTION(func)
#endif

inline static std::string ElfFuncName(uint16_t elf_func_idx) {
  return StringPrintf("Art%u", static_cast<unsigned int>(elf_func_idx));
}

class CStringLessThanComparator {
 public:
  bool operator()(const char* lhs, const char* rhs) const {
    return (strcmp(lhs, rhs) < 0);
  }
};

}  // namespace art

#endif  // ART_SRC_UTILS_LLVM_H_
