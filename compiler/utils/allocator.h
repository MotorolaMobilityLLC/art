/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ALLOCATOR_H_
#define ART_COMPILER_UTILS_ALLOCATOR_H_

#include "base/macros.h"

namespace art {

class Allocator {
 public:
  static Allocator* GetMallocAllocator();
  static Allocator* GetNoopAllocator();

  Allocator() {}
  virtual ~Allocator() {}

  virtual void* Alloc(size_t) = 0;
  virtual void Free(void*) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(Allocator);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ALLOCATOR_H_
