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

#ifndef ART_RUNTIME_GC_COLLECTOR_CONCURRENT_COPYING_INL_H_
#define ART_RUNTIME_GC_COLLECTOR_CONCURRENT_COPYING_INL_H_

#include "concurrent_copying.h"

#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/region_space.h"
#include "lock_word.h"

namespace art {
namespace gc {
namespace collector {

inline mirror::Object* ConcurrentCopying::MarkUnevacFromSpaceRegion(
    mirror::Object* ref, accounting::ContinuousSpaceBitmap* bitmap) {
  // For the Baker-style RB, in a rare case, we could incorrectly change the object from white
  // to gray even though the object has already been marked through. This happens if a mutator
  // thread gets preempted before the AtomicSetReadBarrierPointer below, GC marks through the
  // object (changes it from white to gray and back to white), and the thread runs and
  // incorrectly changes it from white to gray. We need to detect such "false gray" cases and
  // change the objects back to white at the end of marking.
  if (kUseBakerReadBarrier) {
    // Test the bitmap first to reduce the chance of false gray cases.
    if (bitmap->Test(ref)) {
      return ref;
    }
  }
  // This may or may not succeed, which is ok because the object may already be gray.
  bool cas_success = false;
  if (kUseBakerReadBarrier) {
    cas_success = ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(),
                                                   ReadBarrier::GrayPtr());
  }
  if (bitmap->AtomicTestAndSet(ref)) {
    // Already marked.
    if (kUseBakerReadBarrier &&
        cas_success &&
        // The object could be white here if a thread gets preempted after a success at the
        // above AtomicSetReadBarrierPointer, GC has marked through it, and the thread runs up
        // to this point.
        ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr()) {
      // Register a "false-gray" object to change it from gray to white at the end of marking.
      PushOntoFalseGrayStack(ref);
    }
  } else {
    // Newly marked.
    if (kUseBakerReadBarrier) {
      DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::GrayPtr());
    }
    PushOntoMarkStack(ref);
  }
  return ref;
}

template<bool kGrayImmuneObject>
inline mirror::Object* ConcurrentCopying::MarkImmuneSpace(mirror::Object* ref) {
  if (kUseBakerReadBarrier) {
    // The GC-running thread doesn't (need to) gray immune objects except when updating thread roots
    // in the thread flip on behalf of suspended threads (when gc_grays_immune_objects_ is
    // true). Also, a mutator doesn't (need to) gray an immune object after GC has updated all
    // immune space objects (when updated_all_immune_objects_ is true).
    if (kIsDebugBuild) {
      if (Thread::Current() == thread_running_gc_) {
        DCHECK(!kGrayImmuneObject ||
               updated_all_immune_objects_.LoadRelaxed() ||
               gc_grays_immune_objects_);
      } else {
        DCHECK(kGrayImmuneObject);
      }
    }
    if (!kGrayImmuneObject || updated_all_immune_objects_.LoadRelaxed()) {
      return ref;
    }
    // This may or may not succeed, which is ok because the object may already be gray.
    bool success = ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(),
                                                    ReadBarrier::GrayPtr());
    if (success) {
      MutexLock mu(Thread::Current(), immune_gray_stack_lock_);
      immune_gray_stack_.push_back(ref);
    }
  }
  return ref;
}

template<bool kGrayImmuneObject>
inline mirror::Object* ConcurrentCopying::Mark(mirror::Object* from_ref) {
  if (from_ref == nullptr) {
    return nullptr;
  }
  DCHECK(heap_->collector_type_ == kCollectorTypeCC);
  if (UNLIKELY(kUseBakerReadBarrier && !is_active_)) {
    // In the lock word forward address state, the read barrier bits
    // in the lock word are part of the stored forwarding address and
    // invalid. This is usually OK as the from-space copy of objects
    // aren't accessed by mutators due to the to-space
    // invariant. However, during the dex2oat image writing relocation
    // and the zygote compaction, objects can be in the forward
    // address state (to store the forward/relocation addresses) and
    // they can still be accessed and the invalid read barrier bits
    // are consulted. If they look like gray but aren't really, the
    // read barriers slow path can trigger when it shouldn't. To guard
    // against this, return here if the CC collector isn't running.
    return from_ref;
  }
  DCHECK(region_space_ != nullptr) << "Read barrier slow path taken when CC isn't running?";
  space::RegionSpace::RegionType rtype = region_space_->GetRegionType(from_ref);
  switch (rtype) {
    case space::RegionSpace::RegionType::kRegionTypeToSpace:
      // It's already marked.
      return from_ref;
    case space::RegionSpace::RegionType::kRegionTypeFromSpace: {
      mirror::Object* to_ref = GetFwdPtr(from_ref);
      if (kUseBakerReadBarrier) {
        DCHECK_NE(to_ref, ReadBarrier::GrayPtr())
            << "from_ref=" << from_ref << " to_ref=" << to_ref;
      }
      if (to_ref == nullptr) {
        // It isn't marked yet. Mark it by copying it to the to-space.
        to_ref = Copy(from_ref);
      }
      DCHECK(region_space_->IsInToSpace(to_ref) || heap_->non_moving_space_->HasAddress(to_ref))
          << "from_ref=" << from_ref << " to_ref=" << to_ref;
      return to_ref;
    }
    case space::RegionSpace::RegionType::kRegionTypeUnevacFromSpace: {
      return MarkUnevacFromSpaceRegion(from_ref, region_space_bitmap_);
    }
    case space::RegionSpace::RegionType::kRegionTypeNone:
      if (immune_spaces_.ContainsObject(from_ref)) {
        return MarkImmuneSpace<kGrayImmuneObject>(from_ref);
      } else {
        return MarkNonMoving(from_ref);
      }
    default:
      UNREACHABLE();
  }
}

inline mirror::Object* ConcurrentCopying::MarkFromReadBarrier(mirror::Object* from_ref) {
  mirror::Object* ret;
  // TODO: Delete GetMarkBit check when all of the callers properly check the bit. Remaining caller
  // is array allocations.
  if (from_ref == nullptr || from_ref->GetMarkBit()) {
    return from_ref;
  }
  // TODO: Consider removing this check when we are done investigating slow paths. b/30162165
  if (UNLIKELY(mark_from_read_barrier_measurements_)) {
    ret = MarkFromReadBarrierWithMeasurements(from_ref);
  } else {
    ret = Mark(from_ref);
  }
  if (LIKELY(!rb_mark_bit_stack_full_ && ret->AtomicSetMarkBit(0, 1))) {
    // If the mark stack is full, we may temporarily go to mark and back to unmarked. Seeing both
    // values are OK since the only race is doing an unnecessary Mark.
    if (!rb_mark_bit_stack_->AtomicPushBack(ret)) {
      // Mark stack is full, set the bit back to zero.
      CHECK(ret->AtomicSetMarkBit(1, 0));
      // Set rb_mark_bit_stack_full_, this is racy but OK since AtomicPushBack is thread safe.
      rb_mark_bit_stack_full_ = true;
    }
  }
  return ret;
}

inline mirror::Object* ConcurrentCopying::GetFwdPtr(mirror::Object* from_ref) {
  DCHECK(region_space_->IsInFromSpace(from_ref));
  LockWord lw = from_ref->GetLockWord(false);
  if (lw.GetState() == LockWord::kForwardingAddress) {
    mirror::Object* fwd_ptr = reinterpret_cast<mirror::Object*>(lw.ForwardingAddress());
    DCHECK(fwd_ptr != nullptr);
    return fwd_ptr;
  } else {
    return nullptr;
  }
}

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_CONCURRENT_COPYING_INL_H_
