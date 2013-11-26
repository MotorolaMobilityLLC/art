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

#ifndef ART_RUNTIME_GC_HEAP_INL_H_
#define ART_RUNTIME_GC_HEAP_INL_H_

#include "heap.h"

#include "debugger.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "thread.h"
#include "thread-inl.h"

namespace art {
namespace gc {

template <bool kInstrumented, typename PreFenceVisitor>
inline mirror::Object* Heap::AllocObjectWithAllocator(Thread* self, mirror::Class* klass,
                                                      size_t byte_count, AllocatorType allocator,
                                                      const PreFenceVisitor& pre_fence_visitor) {
  DebugCheckPreconditionsForAllocObject(klass, byte_count);
  // Since allocation can cause a GC which will need to SuspendAll, make sure all allocations are
  // done in the runnable state where suspension is expected.
  DCHECK_EQ(self->GetState(), kRunnable);
  self->AssertThreadSuspensionIsAllowable();
  mirror::Object* obj;
  size_t bytes_allocated;
  AllocationTimer alloc_timer(this, &obj);
  if (UNLIKELY(ShouldAllocLargeObject(klass, byte_count))) {
    obj = TryToAllocate<kInstrumented>(self, kAllocatorTypeLOS, byte_count, false,
                                       &bytes_allocated);
    allocator = kAllocatorTypeLOS;
  } else {
    obj = TryToAllocate<kInstrumented>(self, allocator, byte_count, false, &bytes_allocated);
  }

  if (UNLIKELY(obj == nullptr)) {
    SirtRef<mirror::Class> sirt_c(self, klass);
    obj = AllocateInternalWithGc(self, allocator, byte_count, &bytes_allocated);
    if (obj == nullptr) {
      return nullptr;
    } else {
      klass = sirt_c.get();
    }
  }
  obj->SetClass(klass);
  pre_fence_visitor(obj);
  DCHECK_GT(bytes_allocated, 0u);
  const size_t new_num_bytes_allocated =
      static_cast<size_t>(num_bytes_allocated_.fetch_add(bytes_allocated)) + bytes_allocated;
  // TODO: Deprecate.
  if (kInstrumented) {
    if (Runtime::Current()->HasStatsEnabled()) {
      RuntimeStats* thread_stats = self->GetStats();
      ++thread_stats->allocated_objects;
      thread_stats->allocated_bytes += bytes_allocated;
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      ++global_stats->allocated_objects;
      global_stats->allocated_bytes += bytes_allocated;
    }
  } else {
    DCHECK(!Runtime::Current()->HasStatsEnabled());
  }
  if (AllocatorHasAllocationStack(allocator)) {
    // This is safe to do since the GC will never free objects which are neither in the allocation
    // stack or the live bitmap.
    while (!allocation_stack_->AtomicPushBack(obj)) {
      CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
    }
  }
  if (kInstrumented) {
    if (Dbg::IsAllocTrackingEnabled()) {
      Dbg::RecordAllocation(klass, bytes_allocated);
    }
  } else {
    DCHECK(!Dbg::IsAllocTrackingEnabled());
  }
  if (AllocatorHasConcurrentGC(allocator)) {
    CheckConcurrentGC(self, new_num_bytes_allocated, obj);
  }
  if (kIsDebugBuild) {
    if (kDesiredHeapVerification > kNoHeapVerification) {
      VerifyObject(obj);
    }
    self->VerifyStack();
  }
  return obj;
}

template <const bool kInstrumented>
inline mirror::Object* Heap::TryToAllocate(Thread* self, AllocatorType allocator_type,
                                           size_t alloc_size, bool grow,
                                           size_t* bytes_allocated) {
  if (UNLIKELY(IsOutOfMemoryOnAllocation(alloc_size, grow))) {
    return nullptr;
  }
  if (kInstrumented) {
    if (UNLIKELY(running_on_valgrind_ && allocator_type == kAllocatorTypeFreeList)) {
      return non_moving_space_->Alloc(self, alloc_size, bytes_allocated);
    }
  }
  mirror::Object* ret;
  switch (allocator_type) {
    case kAllocatorTypeBumpPointer: {
      DCHECK(bump_pointer_space_ != nullptr);
      alloc_size = RoundUp(alloc_size, space::BumpPointerSpace::kAlignment);
      ret = bump_pointer_space_->AllocNonvirtual(alloc_size);
      if (LIKELY(ret != nullptr)) {
        *bytes_allocated = alloc_size;
      }
      break;
    }
    case kAllocatorTypeFreeList: {
      if (kUseRosAlloc) {
        ret = reinterpret_cast<space::RosAllocSpace*>(non_moving_space_)->AllocNonvirtual(
            self, alloc_size, bytes_allocated);
      } else {
        ret = reinterpret_cast<space::DlMallocSpace*>(non_moving_space_)->AllocNonvirtual(
            self, alloc_size, bytes_allocated);
      }
      break;
    }
    case kAllocatorTypeLOS: {
      ret = large_object_space_->Alloc(self, alloc_size, bytes_allocated);
      // Note that the bump pointer spaces aren't necessarily next to
      // the other continuous spaces like the non-moving alloc space or
      // the zygote space.
      DCHECK(ret == nullptr || large_object_space_->Contains(ret));
      break;
    }
    default: {
      LOG(FATAL) << "Invalid allocator type";
      ret = nullptr;
    }
  }
  return ret;
}

inline void Heap::DebugCheckPreconditionsForAllocObject(mirror::Class* c, size_t byte_count) {
  DCHECK(c == NULL || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
         (c->IsVariableSize() || c->GetObjectSize() == byte_count) ||
         strlen(ClassHelper(c).GetDescriptor()) == 0);
  DCHECK_GE(byte_count, sizeof(mirror::Object));
}

inline Heap::AllocationTimer::AllocationTimer(Heap* heap, mirror::Object** allocated_obj_ptr)
    : heap_(heap), allocated_obj_ptr_(allocated_obj_ptr) {
  if (kMeasureAllocationTime) {
    allocation_start_time_ = NanoTime() / kTimeAdjust;
  }
}

inline Heap::AllocationTimer::~AllocationTimer() {
  if (kMeasureAllocationTime) {
    mirror::Object* allocated_obj = *allocated_obj_ptr_;
    // Only if the allocation succeeded, record the time.
    if (allocated_obj != nullptr) {
      uint64_t allocation_end_time = NanoTime() / kTimeAdjust;
      heap_->total_allocation_time_.fetch_add(allocation_end_time - allocation_start_time_);
    }
  }
};

inline bool Heap::ShouldAllocLargeObject(mirror::Class* c, size_t byte_count) const {
  // We need to have a zygote space or else our newly allocated large object can end up in the
  // Zygote resulting in it being prematurely freed.
  // We can only do this for primitive objects since large objects will not be within the card table
  // range. This also means that we rely on SetClass not dirtying the object's card.
  return byte_count >= kLargeObjectThreshold && have_zygote_space_ && c->IsPrimitiveArray();
}

inline bool Heap::IsOutOfMemoryOnAllocation(size_t alloc_size, bool grow) {
  size_t new_footprint = num_bytes_allocated_ + alloc_size;
  if (UNLIKELY(new_footprint > max_allowed_footprint_)) {
    if (UNLIKELY(new_footprint > growth_limit_)) {
      return true;
    }
    if (!concurrent_gc_) {
      if (!grow) {
        return true;
      } else {
        max_allowed_footprint_ = new_footprint;
      }
    }
  }
  return false;
}

inline void Heap::CheckConcurrentGC(Thread* self, size_t new_num_bytes_allocated,
                                    mirror::Object* obj) {
  if (UNLIKELY(new_num_bytes_allocated >= concurrent_start_bytes_)) {
    // The SirtRef is necessary since the calls in RequestConcurrentGC are a safepoint.
    SirtRef<mirror::Object> ref(self, obj);
    RequestConcurrentGC(self);
  }
}

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_INL_H_
