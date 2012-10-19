/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include <iosfwd>
#include <string>
#include <vector>

#include "atomic_integer.h"
#include "gc/atomic_stack.h"
#include "gc/card_table.h"
#include "gc/heap_bitmap.h"
#include "globals.h"
#include "gtest/gtest.h"
#include "locks.h"
#include "offsets.h"
#include "safe_map.h"
#include "timing_logger.h"

#define VERIFY_OBJECT_ENABLED 0

// Fast verification means we do not verify the classes of objects.
#define VERIFY_OBJECT_FAST 1

namespace art {

class AllocSpace;
class Class;
class ConditionVariable;
class DlMallocSpace;
class HeapBitmap;
class ImageSpace;
class LargeObjectSpace;
class MarkSweep;
class ModUnionTable;
class Mutex;
class Object;
class Space;
class SpaceTest;
class Thread;
class TimingLogger;

typedef AtomicStack<Object*> ObjectStack;
typedef std::vector<ContinuousSpace*> Spaces;

// The ordering of the enum matters, it is used to determine which GCs are run first.
enum GcType {
  // No Gc
  kGcTypeNone,
  // Sticky mark bits "generational" GC.
  kGcTypeSticky,
  // Partial GC, over only the alloc space.
  kGcTypePartial,
  // Full GC
  kGcTypeFull,
  // Number of different Gc types.
  kGcTypeMax,
};
std::ostream& operator<<(std::ostream& os, const GcType& policy);

enum GcCause {
  kGcCauseForAlloc,
  kGcCauseBackground,
  kGcCauseExplicit,
};
std::ostream& operator<<(std::ostream& os, const GcCause& policy);

class Heap {
 public:
  static const size_t kDefaultInitialSize = 2 * MB;
  static const size_t kDefaultMaximumSize = 32 * MB;
  static const size_t kDefaultMaxFree = 2 * MB;
  static const size_t kDefaultMinFree = kDefaultMaxFree / 4;

  // Default target utilization.
  static const double kDefaultTargetUtilization;

  // Used so that we don't overflow the allocation time atomic integer.
  static const size_t kTimeAdjust = 1024;

  typedef void (RootVisitor)(const Object* root, void* arg);
  typedef void (VerifyRootVisitor)(const Object* root, void* arg, size_t vreg,
      const AbstractMethod* method);
  typedef bool (IsMarkedTester)(const Object* object, void* arg);

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  explicit Heap(size_t initial_size, size_t growth_limit, size_t min_free,
                size_t max_free, double target_utilization, size_t capacity,
                const std::string& original_image_file_name, bool concurrent_gc);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  Object* AllocObject(Thread* self, Class* klass, size_t num_bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Check sanity of given reference. Requires the heap lock.
#if VERIFY_OBJECT_ENABLED
  void VerifyObject(const Object* o);
#else
  void VerifyObject(const Object*) {}
#endif

  // Check sanity of all live references. Requires the heap lock.
  void VerifyHeap() LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  static void RootMatchesObjectVisitor(const Object* root, void* arg);
  bool VerifyHeapReferences()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool VerifyMissingCardMarks()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsHeapAddress(const Object* obj);

  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(const Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Initiates an explicit garbage collection.
  void CollectGarbage(bool clear_soft_references)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Does a concurrent GC, should only be called by the GC daemon thread
  // through runtime.
  void ConcurrentGC(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);

  // Implements java.lang.Runtime.maxMemory.
  int64_t GetMaxMemory() const;
  // Implements java.lang.Runtime.totalMemory.
  int64_t GetTotalMemory() const;
  // Implements java.lang.Runtime.freeMemory.
  int64_t GetFreeMemory() const;

  // Implements VMDebug.countInstancesOfClass.
  int64_t CountInstances(Class* c, bool count_assignable)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  void ClearGrowthLimit();

  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  double GetTargetHeapUtilization() const {
    return target_utilization_;
  }

  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  void SetTargetHeapUtilization(float target);

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle and returns
  // true if we waited for the GC to complete.
  GcType WaitForConcurrentGcToComplete(Thread* self) LOCKS_EXCLUDED(gc_complete_lock_);

  const Spaces& GetSpaces() {
    return spaces_;
  }

  void SetReferenceOffsets(MemberOffset reference_referent_offset,
                           MemberOffset reference_queue_offset,
                           MemberOffset reference_queueNext_offset,
                           MemberOffset reference_pendingNext_offset,
                           MemberOffset finalizer_reference_zombie_offset);

  Object* GetReferenceReferent(Object* reference);
  void ClearReferenceReferent(Object* reference) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returns true if the reference object has not yet been enqueued.
  bool IsEnqueuable(const Object* ref);
  void EnqueueReference(Object* ref, Object** list) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void EnqueuePendingReference(Object* ref, Object** list)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  Object* DequeuePendingReference(Object** list) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  MemberOffset GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
    return reference_pendingNext_offset_;
  }

  MemberOffset GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
    return finalizer_reference_zombie_offset_;
  }

  void EnableObjectValidation() {
#if VERIFY_OBJECT_ENABLED
    VerifyHeap();
#endif
    verify_objects_ = true;
  }

  void DisableObjectValidation() {
    verify_objects_ = false;
  }

  bool IsObjectValidationEnabled() const {
    return verify_objects_;
  }

  void RecordFree(size_t freed_objects, size_t freed_bytes);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  void WriteBarrierField(const Object* dst, MemberOffset /*offset*/, const Object* /*new_value*/) {
    if (!card_marking_disabled_) {
      card_table_->MarkCard(dst);
    }
  }

  // Write barrier for array operations that update many field positions
  void WriteBarrierArray(const Object* dst, int /*start_offset*/,
                         size_t /*length TODO: element_count or byte_count?*/) {
    if (UNLIKELY(!card_marking_disabled_)) {
      card_table_->MarkCard(dst);
    }
  }

  CardTable* GetCardTable() {
    return card_table_.get();
  }

  void DisableCardMarking() {
    // TODO: we shouldn't need to disable card marking, this is here to help the image_writer
    card_marking_disabled_ = true;
  }

  void AddFinalizerReference(Thread* self, Object* object);

  size_t GetBytesAllocated() const;
  size_t GetObjectsAllocated() const;
  size_t GetConcurrentStartSize() const;
  size_t GetConcurrentMinFree() const;
  size_t GetUsedMemorySize() const;

  // Returns the total number of objects allocated since the heap was created.
  size_t GetTotalObjectsAllocated() const;

  // Returns the total number of bytes allocated since the heap was created.
  size_t GetTotalBytesAllocated() const;

  // Returns the total number of objects freed since the heap was created.
  size_t GetTotalObjectsFreed() const;

  // Returns the total number of bytes freed since the heap was created.
  size_t GetTotalBytesFreed() const;

  // Functions for getting the bitmap which corresponds to an object's address.
  // This is probably slow, TODO: use better data structure like binary tree .
  ContinuousSpace* FindSpaceFromObject(const Object*) const;

  void DumpForSigQuit(std::ostream& os);

  void Trim();

  HeapBitmap* GetLiveBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }

  HeapBitmap* GetMarkBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }

  void PreZygoteFork() LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

  // Mark and empty stack.
  void FlushAllocStack()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Mark all the objects in the allocation stack in the specified bitmap.
  void MarkAllocStack(SpaceBitmap* bitmap, SpaceSetMap* large_objects, ObjectStack* stack)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Unmark all the objects in the allocation stack in the specified bitmap.
  void UnMarkAllocStack(SpaceBitmap* bitmap, SpaceSetMap* large_objects, ObjectStack* stack)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Update and mark mod union table based on gc type.
  void UpdateAndMarkModUnion(MarkSweep* mark_sweep, TimingLogger& timings, GcType gc_type)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // DEPRECATED: Should remove in "near" future when support for multiple image spaces is added.
  // Assumes there is only one image space.
  ImageSpace* GetImageSpace();
  DlMallocSpace* GetAllocSpace();
  LargeObjectSpace* GetLargeObjectsSpace() {
    return large_object_space_.get();
  }
  void DumpSpaces();

  // UnReserve the address range where the oat file will be placed.
  void UnReserveOatFileAddressRange();

  // GC performance measuring
  void DumpGcPerformanceInfo();

 private:
  // Allocates uninitialized storage. Passing in a null space tries to place the object in the
  // large object space.
  Object* Allocate(Thread* self, AllocSpace* space, size_t num_bytes)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Try to allocate a number of bytes, this function never does any GCs.
  Object* TryToAllocate(Thread* self, AllocSpace* space, size_t alloc_size, bool grow)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Pushes a list of cleared references out to the managed heap.
  void EnqueueClearedReferences(Object** cleared_references);

  void RequestHeapTrim() LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  void RequestConcurrentGC(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);

  // Swap bitmaps (if we are a full Gc then we swap the zygote bitmap too).
  void SwapBitmaps(GcType gc_type) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
  void SwapLargeObjects() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void RecordAllocation(size_t size, Object* object)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sometimes CollectGarbageInternal decides to run a different Gc than you requested. Returns
  // which type of Gc was actually ran.
  GcType CollectGarbageInternal(GcType gc_plan, GcCause gc_cause, bool clear_soft_references)
      LOCKS_EXCLUDED(gc_complete_lock_,
                     Locks::heap_bitmap_lock_,
                     Locks::mutator_lock_,
                     Locks::thread_suspend_count_lock_);
  void CollectGarbageMarkSweepPlan(Thread* self, GcType gc_plan, GcCause gc_cause,
                                   bool clear_soft_references)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_,
                     Locks::mutator_lock_);
  void CollectGarbageConcurrentMarkSweepPlan(Thread* self, GcType gc_plan, GcCause gc_cause,
                                             bool clear_soft_references)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_,
                     Locks::mutator_lock_);

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  void GrowForUtilization();

  size_t GetPercentFree();

  void AddSpace(ContinuousSpace* space) LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

  // No thread saftey analysis since we call this everywhere and it is impossible to find a proper
  // lock ordering for it.
  void VerifyObjectBody(const Object *obj) NO_THREAD_SAFETY_ANALYSIS;

  static void VerificationCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSychronization::heap_bitmap_lock_);

  // Swap the allocation stack with the live stack.
  void SwapStacks();

  // Clear cards and update the mod union table.
  void ClearCards(TimingLogger& timings);

  Spaces spaces_;

  // A map that we use to temporarily reserve address range for the oat file.
  UniquePtr<MemMap> oat_file_map_;

  // The alloc space which we are currently allocating into.
  DlMallocSpace* alloc_space_;

  // One cumulative logger for each type of Gc.
  typedef SafeMap<GcType, CumulativeLogger*> CumulativeTimings;
  CumulativeTimings cumulative_timings_;

  // The mod-union table remembers all of the references from the image space to the alloc /
  // zygote spaces.
  UniquePtr<ModUnionTable> mod_union_table_;

  // This table holds all of the references from the zygote space to the alloc space.
  UniquePtr<ModUnionTable> zygote_mod_union_table_;

  UniquePtr<CardTable> card_table_;

  // True for concurrent mark sweep GC, false for mark sweep.
  const bool concurrent_gc_;

  // If we have a zygote space.
  bool have_zygote_space_;

  // Used by the image writer to disable card marking on copied objects
  // TODO: remove
  bool card_marking_disabled_;

  // Guards access to the state of GC, associated conditional variable is used to signal when a GC
  // completes.
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  UniquePtr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);

  // True while the garbage collector is running.
  volatile bool is_gc_running_ GUARDED_BY(gc_complete_lock_);

  // Last Gc type we ran. Used by WaitForConcurrentGc to know which Gc was waited on.
  volatile GcType last_gc_type_ GUARDED_BY(gc_complete_lock_);

  // If enabled, causes Gc for alloc when heap size reaches the current footprint limit before the
  // Gc updates it.
  const bool enforce_heap_growth_rate_;

  // Maximum size that the heap can reach.
  size_t growth_limit_;
  size_t max_allowed_footprint_;

  // Bytes until concurrent GC starts.
  size_t concurrent_start_size_;
  size_t concurrent_min_free_;
  size_t concurrent_start_bytes_;

  // Number of bytes allocated since the last Gc, we use this to help determine when to schedule concurrent GCs.
  size_t bytes_since_last_gc_;
  size_t sticky_gc_count_;

  size_t total_bytes_freed_;
  size_t total_objects_freed_;

  // Primitive objects larger than this size are put in the large object space.
  size_t large_object_threshold_;

  // Large object space.
  UniquePtr<LargeObjectSpace> large_object_space_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  AtomicInteger num_bytes_allocated_;

  // Heap verification flags.
  const bool verify_missing_card_marks_;
  const bool verify_system_weaks_;
  const bool verify_pre_gc_heap_;
  const bool verify_post_gc_heap_;
  const bool verify_mod_union_table_;

  // After how many GCs we force to do a partial GC instead of sticky mark bits GC.
  const size_t partial_gc_frequency_;

  // Sticky mark bits GC has some overhead, so if we have less a few megabytes of AllocSpace then
  // it's probably better to just do a partial GC.
  const size_t min_alloc_space_size_for_sticky_gc_;

  // Minimum remaining size for sticky GC. Since sticky GC doesn't free up as much memory as a
  // normal GC, it is important to not use it when we are almost out of memory.
  const size_t min_remaining_space_for_sticky_gc_;

  // Last trim time
  uint64_t last_trim_time_;

  UniquePtr<HeapBitmap> live_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);
  UniquePtr<HeapBitmap> mark_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);

  // Used to ensure that we don't ever recursively request GC.
  volatile bool requesting_gc_;

  // Mark stack that we reuse to avoid re-allocating the mark stack.
  UniquePtr<ObjectStack> mark_stack_;

  // Allocation stack, new allocations go here so that we can do sticky mark bits. This enables us
  // to use the live bitmap as the old mark bitmap.
  const size_t max_allocation_stack_size_;
  UniquePtr<ObjectStack> allocation_stack_;

  // Second allocation stack so that we can process allocation with the heap unlocked.
  UniquePtr<ObjectStack> live_stack_;

  // offset of java.lang.ref.Reference.referent
  MemberOffset reference_referent_offset_;

  // offset of java.lang.ref.Reference.queue
  MemberOffset reference_queue_offset_;

  // offset of java.lang.ref.Reference.queueNext
  MemberOffset reference_queueNext_offset_;

  // offset of java.lang.ref.Reference.pendingNext
  MemberOffset reference_pendingNext_offset_;

  // offset of java.lang.ref.FinalizerReference.zombie
  MemberOffset finalizer_reference_zombie_offset_;

  // Minimum free guarantees that you always have at least min_free_ free bytes after growing for
  // utilization, regardless of target utilization ratio.
  size_t min_free_;

  // The ideal maximum free size, when we grow the heap for utilization.
  size_t max_free_;

  // Target ideal heap utilization ratio
  double target_utilization_;

  // Total time which mutators are paused or waiting for GC to complete.
  uint64_t total_paused_time_;
  uint64_t total_wait_time_;

  // Total number of objects allocated in microseconds.
  const bool measure_allocation_time_;
  AtomicInteger total_allocation_time_;

  bool verify_objects_;

  friend class MarkSweep;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;
  friend class ScopedHeapLock;
  FRIEND_TEST(SpaceTest, AllocAndFree);
  FRIEND_TEST(SpaceTest, AllocAndFreeList);
  FRIEND_TEST(SpaceTest, ZygoteSpace);
  friend class SpaceTest;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
