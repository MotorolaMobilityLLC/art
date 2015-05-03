/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_LINEAR_ALLOC_H_
#define ART_RUNTIME_LINEAR_ALLOC_H_

#include "base/arena_allocator.h"

namespace art {

class ArenaPool;

// TODO: Support freeing if we add poor man's class unloading.
class LinearAlloc {
 public:
  explicit LinearAlloc(ArenaPool* pool);

  void* Alloc(Thread* self, size_t size);

  // Allocate and construct an array of structs of type T.
  template<class T>
  T* AllocArray(Thread* self, size_t elements) {
    return reinterpret_cast<T*>(Alloc(self, elements * sizeof(T)));
  }

  // Return the number of bytes used in the allocator.
  size_t GetUsedMemory() const;

 private:
  mutable Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ArenaAllocator allocator_ GUARDED_BY(lock_);

  DISALLOW_IMPLICIT_CONSTRUCTORS(LinearAlloc);
};

}  // namespace art

#endif  // ART_RUNTIME_LINEAR_ALLOC_H_
