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

#include "arena_allocator.h"
#include "arena_bit_vector.h"

namespace art {

class ArenaBitVectorAllocator : public Allocator {
 public:
  explicit ArenaBitVectorAllocator(ArenaAllocator* arena) : arena_(arena) {}
  ~ArenaBitVectorAllocator() {}

  virtual void* Alloc(size_t size) {
    return arena_->Alloc(size, ArenaAllocator::kAllocGrowableBitMap);
  }

  virtual void Free(void*) {}  // Nop.

  static void* operator new(size_t size, ArenaAllocator* arena) {
    return arena->Alloc(sizeof(ArenaBitVectorAllocator), ArenaAllocator::kAllocGrowableBitMap);
  }
  static void operator delete(void* p) {}  // Nop.

 private:
  ArenaAllocator* arena_;
  DISALLOW_COPY_AND_ASSIGN(ArenaBitVectorAllocator);
};

ArenaBitVector::ArenaBitVector(ArenaAllocator* arena, unsigned int start_bits,
                               bool expandable, OatBitMapKind kind)
  :  BitVector(start_bits, expandable, new (arena) ArenaBitVectorAllocator(arena)), kind_(kind) {
  UNUSED(kind_);
}

}  // namespace art
