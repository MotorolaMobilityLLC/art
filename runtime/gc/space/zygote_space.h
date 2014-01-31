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

#ifndef ART_RUNTIME_GC_SPACE_ZYGOTE_SPACE_H_
#define ART_RUNTIME_GC_SPACE_ZYGOTE_SPACE_H_

#include "malloc_space.h"
#include "mem_map.h"

namespace art {
namespace gc {

namespace accounting {
class SpaceBitmap;
}

namespace space {

// An zygote space is a space which you cannot allocate into or free from.
class ZygoteSpace : public ContinuousMemMapAllocSpace {
 public:
  // Returns the remaining storage in the out_map field.
  static ZygoteSpace* Create(const std::string& name, MemMap* mem_map,
                             accounting::SpaceBitmap* live_bitmap,
                             accounting::SpaceBitmap* mark_bitmap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Dump(std::ostream& os) const;
  virtual SpaceType GetType() const {
    return kSpaceTypeZygoteSpace;
  }
  virtual ZygoteSpace* AsZygoteSpace() {
    return this;
  }
  virtual mirror::Object* AllocWithGrowth(Thread* /*self*/, size_t /*num_bytes*/,
                                          size_t* /*bytes_allocated*/) {
    LOG(FATAL) << "Unimplemented";
    return nullptr;
  }
  virtual mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated) {
    LOG(FATAL) << "Unimplemented";
    return nullptr;
  }
  virtual size_t AllocationSize(const mirror::Object* obj) {
    LOG(FATAL) << "Unimplemented";
    return 0;
  }
  virtual size_t Free(Thread* self, mirror::Object* ptr) {
    LOG(FATAL) << "Unimplemented";
    return 0;
  }
  virtual size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
    LOG(FATAL) << "Unimplemented";
    return 0;
  }
  virtual uint64_t GetBytesAllocated() {
    return Size();
  }
  virtual uint64_t GetObjectsAllocated() {
    return objects_allocated_;
  }

 protected:
  virtual accounting::SpaceBitmap::SweepCallback* GetSweepCallback() {
    return &SweepCallback;
  }

 private:
  ZygoteSpace(const std::string& name, MemMap* mem_map, size_t objects_allocated);
  static void SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg);

  AtomicInteger objects_allocated_;

  friend class Space;
  DISALLOW_COPY_AND_ASSIGN(ZygoteSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_ZYGOTE_SPACE_H_
