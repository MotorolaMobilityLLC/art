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

#ifndef ART_RUNTIME_GC_HEAP_H_
#define ART_RUNTIME_GC_HEAP_H_

#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>

#include <android-base/logging.h>

#include "allocator_type.h"
#include "arch/instruction_set.h"
#include "atomic.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/runtime_debug.h"
#include "base/time_utils.h"
#include "gc/collector/gc_type.h"
#include "gc/collector/iteration.h"
#include "gc/collector_type.h"
#include "gc/gc_cause.h"
#include "gc/space/large_object_space.h"
#include "globals.h"
#include "handle.h"
#include "obj_ptr.h"
#include "offsets.h"
#include "process_state.h"
#include "read_barrier_config.h"
#include "safe_map.h"
#include "verify_object.h"

namespace art {

class ConditionVariable;
class IsMarkedVisitor;
class Mutex;
class RootVisitor;
class StackVisitor;
class Thread;
class ThreadPool;
class TimingLogger;
class VariableSizedHandleScope;

namespace mirror {
class Class;
class Object;
}  // namespace mirror

namespace gc {

class AllocationListener;
class AllocRecordObjectMap;
class GcPauseListener;
class ReferenceProcessor;
class TaskProcessor;
class Verification;

namespace accounting {
template <typename T> class AtomicStack;
typedef AtomicStack<mirror::Object> ObjectStack;
class CardTable;
class HeapBitmap;
class ModUnionTable;
class ReadBarrierTable;
class RememberedSet;
}  // namespace accounting

namespace collector {
class ConcurrentCopying;
class GarbageCollector;
class MarkCompact;
class MarkSweep;
class SemiSpace;
}  // namespace collector

namespace allocator {
class RosAlloc;
}  // namespace allocator

namespace space {
class AllocSpace;
class BumpPointerSpace;
class ContinuousMemMapAllocSpace;
class DiscontinuousSpace;
class DlMallocSpace;
class ImageSpace;
class LargeObjectSpace;
class MallocSpace;
class RegionSpace;
class RosAllocSpace;
class Space;
class ZygoteSpace;
}  // namespace space

enum HomogeneousSpaceCompactResult {
  // Success.
  kSuccess,
  // Reject due to disabled moving GC.
  kErrorReject,
  // Unsupported due to the current configuration.
  kErrorUnsupported,
  // System is shutting down.
  kErrorVMShuttingDown,
};

// If true, use rosalloc/RosAllocSpace instead of dlmalloc/DlMallocSpace
static constexpr bool kUseRosAlloc = true;

// If true, use thread-local allocation stack.
static constexpr bool kUseThreadLocalAllocationStack = true;

class Heap {
 public:
  // If true, measure the total allocation time.
  static constexpr size_t kDefaultStartingSize = kPageSize;
  static constexpr size_t kDefaultInitialSize = 2 * MB;
  static constexpr size_t kDefaultMaximumSize = 256 * MB;
  static constexpr size_t kDefaultNonMovingSpaceCapacity = 64 * MB;
  static constexpr size_t kDefaultMaxFree = 2 * MB;
  static constexpr size_t kDefaultMinFree = kDefaultMaxFree / 4;
  static constexpr size_t kDefaultLongPauseLogThreshold = MsToNs(5);
  static constexpr size_t kDefaultLongGCLogThreshold = MsToNs(100);
  static constexpr size_t kDefaultTLABSize = 32 * KB;
  static constexpr double kDefaultTargetUtilization = 0.5;
  static constexpr double kDefaultHeapGrowthMultiplier = 2.0;
  // Primitive arrays larger than this size are put in the large object space.
  static constexpr size_t kMinLargeObjectThreshold = 3 * kPageSize;
  static constexpr size_t kDefaultLargeObjectThreshold = kMinLargeObjectThreshold;
  // Whether or not parallel GC is enabled. If not, then we never create the thread pool.
  static constexpr bool kDefaultEnableParallelGC = false;

  // Whether or not we use the free list large object space. Only use it if USE_ART_LOW_4G_ALLOCATOR
  // since this means that we have to use the slow msync loop in MemMap::MapAnonymous.
  static constexpr space::LargeObjectSpaceType kDefaultLargeObjectSpaceType =
      USE_ART_LOW_4G_ALLOCATOR ?
          space::LargeObjectSpaceType::kFreeList
        : space::LargeObjectSpaceType::kMap;

  // Used so that we don't overflow the allocation time atomic integer.
  static constexpr size_t kTimeAdjust = 1024;

  // How often we allow heap trimming to happen (nanoseconds).
  static constexpr uint64_t kHeapTrimWait = MsToNs(5000);
  // How long we wait after a transition request to perform a collector transition (nanoseconds).
  static constexpr uint64_t kCollectorTransitionWait = MsToNs(5000);
  // Whether the transition-wait applies or not. Zero wait will stress the
  // transition code and collector, but increases jank probability.
  DECLARE_RUNTIME_DEBUG_FLAG(kStressCollectorTransition);

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  Heap(size_t initial_size,
       size_t growth_limit,
       size_t min_free,
       size_t max_free,
       double target_utilization,
       double foreground_heap_growth_multiplier,
       size_t capacity,
       size_t non_moving_space_capacity,
       const std::string& original_image_file_name,
       InstructionSet image_instruction_set,
       CollectorType foreground_collector_type,
       CollectorType background_collector_type,
       space::LargeObjectSpaceType large_object_space_type,
       size_t large_object_threshold,
       size_t parallel_gc_threads,
       size_t conc_gc_threads,
       bool low_memory_mode,
       size_t long_pause_threshold,
       size_t long_gc_threshold,
       bool ignore_max_footprint,
       bool use_tlab,
       bool verify_pre_gc_heap,
       bool verify_pre_sweeping_heap,
       bool verify_post_gc_heap,
       bool verify_pre_gc_rosalloc,
       bool verify_pre_sweeping_rosalloc,
       bool verify_post_gc_rosalloc,
       bool gc_stress_mode,
       bool measure_gc_performance,
       bool use_homogeneous_space_compaction,
       uint64_t min_interval_homogeneous_space_compaction_by_oom);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  template <bool kInstrumented, typename PreFenceVisitor>
  mirror::Object* AllocObject(Thread* self,
                              ObjPtr<mirror::Class> klass,
                              size_t num_bytes,
                              const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !Roles::uninterruptible_) {
    return AllocObjectWithAllocator<kInstrumented, true>(self,
                                                         klass,
                                                         num_bytes,
                                                         GetCurrentAllocator(),
                                                         pre_fence_visitor);
  }

  template <bool kInstrumented, typename PreFenceVisitor>
  mirror::Object* AllocNonMovableObject(Thread* self,
                                        ObjPtr<mirror::Class> klass,
                                        size_t num_bytes,
                                        const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !Roles::uninterruptible_) {
    return AllocObjectWithAllocator<kInstrumented, true>(self,
                                                         klass,
                                                         num_bytes,
                                                         GetCurrentNonMovingAllocator(),
                                                         pre_fence_visitor);
  }

  template <bool kInstrumented, bool kCheckLargeObject, typename PreFenceVisitor>
  ALWAYS_INLINE mirror::Object* AllocObjectWithAllocator(Thread* self,
                                                         ObjPtr<mirror::Class> klass,
                                                         size_t byte_count,
                                                         AllocatorType allocator,
                                                         const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !Roles::uninterruptible_);

  AllocatorType GetCurrentAllocator() const {
    return current_allocator_;
  }

  AllocatorType GetCurrentNonMovingAllocator() const {
    return current_non_moving_allocator_;
  }

  // Visit all of the live objects in the heap.
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjects(Visitor&& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsPaused(Visitor&& visitor)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);

  void CheckPreconditionsForAllocObject(ObjPtr<mirror::Class> c, size_t byte_count)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void RegisterNativeAllocation(JNIEnv* env, size_t bytes)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);
  void RegisterNativeFree(JNIEnv* env, size_t bytes);

  // Change the allocator, updates entrypoints.
  void ChangeAllocator(AllocatorType allocator)
      REQUIRES(Locks::mutator_lock_, !Locks::runtime_shutdown_lock_);

  // Transition the garbage collector during runtime, may copy objects from one space to another.
  void TransitionCollector(CollectorType collector_type) REQUIRES(!*gc_complete_lock_);

  // Change the collector to be one of the possible options (MS, CMS, SS).
  void ChangeCollector(CollectorType collector_type)
      REQUIRES(Locks::mutator_lock_);

  // The given reference is believed to be to an object in the Java heap, check the soundness of it.
  // TODO: NO_THREAD_SAFETY_ANALYSIS since we call this everywhere and it is impossible to find a
  // proper lock ordering for it.
  void VerifyObjectBody(ObjPtr<mirror::Object> o) NO_THREAD_SAFETY_ANALYSIS;

  // Check sanity of all live references.
  void VerifyHeap() REQUIRES(!Locks::heap_bitmap_lock_);
  // Returns how many failures occured.
  size_t VerifyHeapReferences(bool verify_referents = true)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  bool VerifyMissingCardMarks()
      REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsValidObjectAddress(const void* obj) const REQUIRES_SHARED(Locks::mutator_lock_);

  // Faster alternative to IsHeapAddress since finding if an object is in the large object space is
  // very slow.
  bool IsNonDiscontinuousSpaceHeapAddress(const void* addr) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                          bool search_allocation_stack = true,
                          bool search_live_stack = true,
                          bool sorted = false)
      REQUIRES_SHARED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Returns true if there is any chance that the object (obj) will move.
  bool IsMovableObject(ObjPtr<mirror::Object> obj) const REQUIRES_SHARED(Locks::mutator_lock_);

  // Enables us to compacting GC until objects are released.
  void IncrementDisableMovingGC(Thread* self) REQUIRES(!*gc_complete_lock_);
  void DecrementDisableMovingGC(Thread* self) REQUIRES(!*gc_complete_lock_);

  // Temporarily disable thread flip for JNI critical calls.
  void IncrementDisableThreadFlip(Thread* self) REQUIRES(!*thread_flip_lock_);
  void DecrementDisableThreadFlip(Thread* self) REQUIRES(!*thread_flip_lock_);
  void ThreadFlipBegin(Thread* self) REQUIRES(!*thread_flip_lock_);
  void ThreadFlipEnd(Thread* self) REQUIRES(!*thread_flip_lock_);

  // Clear all of the mark bits, doesn't clear bitmaps which have the same live bits as mark bits.
  // Mutator lock is required for GetContinuousSpaces.
  void ClearMarkedObjects()
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Initiates an explicit garbage collection.
  void CollectGarbage(bool clear_soft_references, GcCause cause = kGcCauseExplicit)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);

  // Does a concurrent GC, should only be called by the GC daemon thread
  // through runtime.
  void ConcurrentGC(Thread* self, GcCause cause, bool force_full)
      REQUIRES(!Locks::runtime_shutdown_lock_, !*gc_complete_lock_, !*pending_task_lock_);

  // Implements VMDebug.countInstancesOfClass and JDWP VM_InstanceCount.
  // The boolean decides whether to use IsAssignableFrom or == when comparing classes.
  void CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                      bool use_is_assignable_from,
                      uint64_t* counts)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implements VMDebug.getInstancesOfClasses and JDWP RT_Instances.
  void GetInstances(VariableSizedHandleScope& scope,
                    Handle<mirror::Class> c,
                    bool use_is_assignable_from,
                    int32_t max_count,
                    std::vector<Handle<mirror::Object>>& instances)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implements JDWP OR_ReferringObjects.
  void GetReferringObjects(VariableSizedHandleScope& scope,
                           Handle<mirror::Object> o,
                           int32_t max_count,
                           std::vector<Handle<mirror::Object>>& referring_objects)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  void ClearGrowthLimit();

  // Make the current growth limit the new maximum capacity, unmaps pages at the end of spaces
  // which will never be used. Used to implement dalvik.system.VMRuntime.clampGrowthLimit.
  void ClampGrowthLimit() REQUIRES(!Locks::heap_bitmap_lock_);

  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  double GetTargetHeapUtilization() const {
    return target_utilization_;
  }

  // Data structure memory usage tracking.
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);

  // Set the heap's private space pointers to be the same as the space based on it's type. Public
  // due to usage by tests.
  void SetSpaceAsDefault(space::ContinuousSpace* continuous_space)
      REQUIRES(!Locks::heap_bitmap_lock_);
  void AddSpace(space::Space* space)
      REQUIRES(!Locks::heap_bitmap_lock_)
      REQUIRES(Locks::mutator_lock_);
  void RemoveSpace(space::Space* space)
    REQUIRES(!Locks::heap_bitmap_lock_)
    REQUIRES(Locks::mutator_lock_);

  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  void SetTargetHeapUtilization(float target);

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
  // waited for.
  collector::GcType WaitForGcToComplete(GcCause cause, Thread* self) REQUIRES(!*gc_complete_lock_);

  // Update the heap's process state to a new value, may cause compaction to occur.
  void UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state)
      REQUIRES(!*pending_task_lock_, !*gc_complete_lock_);

  bool HaveContinuousSpaces() const NO_THREAD_SAFETY_ANALYSIS {
    // No lock since vector empty is thread safe.
    return !continuous_spaces_.empty();
  }

  const std::vector<space::ContinuousSpace*>& GetContinuousSpaces() const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return continuous_spaces_;
  }

  const std::vector<space::DiscontinuousSpace*>& GetDiscontinuousSpaces() const {
    return discontinuous_spaces_;
  }

  const collector::Iteration* GetCurrentGcIteration() const {
    return &current_gc_iteration_;
  }
  collector::Iteration* GetCurrentGcIteration() {
    return &current_gc_iteration_;
  }

  // Enable verification of object references when the runtime is sufficiently initialized.
  void EnableObjectValidation() {
    verify_object_mode_ = kVerifyObjectSupport;
    if (verify_object_mode_ > kVerifyObjectModeDisabled) {
      VerifyHeap();
    }
  }

  // Disable object reference verification for image writing.
  void DisableObjectValidation() {
    verify_object_mode_ = kVerifyObjectModeDisabled;
  }

  // Other checks may be performed if we know the heap should be in a sane state.
  bool IsObjectValidationEnabled() const {
    return verify_object_mode_ > kVerifyObjectModeDisabled;
  }

  // Returns true if low memory mode is enabled.
  bool IsLowMemoryMode() const {
    return low_memory_mode_;
  }

  // Returns the heap growth multiplier, this affects how much we grow the heap after a GC.
  // Scales heap growth, min free, and max free.
  double HeapGrowthMultiplier() const;

  // Freed bytes can be negative in cases where we copy objects from a compacted space to a
  // free-list backed space.
  void RecordFree(uint64_t freed_objects, int64_t freed_bytes);

  // Record the bytes freed by thread-local buffer revoke.
  void RecordFreeRevoke();

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if null is stored in the field.
  ALWAYS_INLINE void WriteBarrierField(ObjPtr<mirror::Object> dst,
                                       MemberOffset offset,
                                       ObjPtr<mirror::Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Write barrier for array operations that update many field positions
  ALWAYS_INLINE void WriteBarrierArray(ObjPtr<mirror::Object> dst,
                                       int start_offset,
                                       // TODO: element_count or byte_count?
                                       size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE void WriteBarrierEveryFieldOf(ObjPtr<mirror::Object> obj)
      REQUIRES_SHARED(Locks::mutator_lock_);

  accounting::CardTable* GetCardTable() const {
    return card_table_.get();
  }

  accounting::ReadBarrierTable* GetReadBarrierTable() const {
    return rb_table_.get();
  }

  void AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object);

  // Returns the number of bytes currently allocated.
  size_t GetBytesAllocated() const {
    return num_bytes_allocated_.LoadSequentiallyConsistent();
  }

  // Returns the number of objects currently allocated.
  size_t GetObjectsAllocated() const
      REQUIRES(!Locks::heap_bitmap_lock_);

  // Returns the total number of objects allocated since the heap was created.
  uint64_t GetObjectsAllocatedEver() const;

  // Returns the total number of bytes allocated since the heap was created.
  uint64_t GetBytesAllocatedEver() const;

  // Returns the total number of objects freed since the heap was created.
  uint64_t GetObjectsFreedEver() const {
    return total_objects_freed_ever_;
  }

  // Returns the total number of bytes freed since the heap was created.
  uint64_t GetBytesFreedEver() const {
    return total_bytes_freed_ever_;
  }

  // Implements java.lang.Runtime.maxMemory, returning the maximum amount of memory a program can
  // consume. For a regular VM this would relate to the -Xmx option and would return -1 if no Xmx
  // were specified. Android apps start with a growth limit (small heap size) which is
  // cleared/extended for large apps.
  size_t GetMaxMemory() const {
    // There is some race conditions in the allocation code that can cause bytes allocated to
    // become larger than growth_limit_ in rare cases.
    return std::max(GetBytesAllocated(), growth_limit_);
  }

  // Implements java.lang.Runtime.totalMemory, returning approximate amount of memory currently
  // consumed by an application.
  size_t GetTotalMemory() const;

  // Returns approximately how much free memory we have until the next GC happens.
  size_t GetFreeMemoryUntilGC() const {
    return max_allowed_footprint_ - GetBytesAllocated();
  }

  // Returns approximately how much free memory we have until the next OOME happens.
  size_t GetFreeMemoryUntilOOME() const {
    return growth_limit_ - GetBytesAllocated();
  }

  // Returns how much free memory we have until we need to grow the heap to perform an allocation.
  // Similar to GetFreeMemoryUntilGC. Implements java.lang.Runtime.freeMemory.
  size_t GetFreeMemory() const {
    size_t byte_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
    size_t total_memory = GetTotalMemory();
    // Make sure we don't get a negative number.
    return total_memory - std::min(total_memory, byte_allocated);
  }

  // Get the space that corresponds to an object's address. Current implementation searches all
  // spaces in turn. If fail_ok is false then failing to find a space will cause an abort.
  // TODO: consider using faster data structure like binary tree.
  space::ContinuousSpace* FindContinuousSpaceFromObject(ObjPtr<mirror::Object>, bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  space::ContinuousSpace* FindContinuousSpaceFromAddress(const mirror::Object* addr) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  space::DiscontinuousSpace* FindDiscontinuousSpaceFromObject(ObjPtr<mirror::Object>,
                                                              bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  space::Space* FindSpaceFromObject(ObjPtr<mirror::Object> obj, bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  space::Space* FindSpaceFromAddress(const void* ptr) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpForSigQuit(std::ostream& os) REQUIRES(!*gc_complete_lock_);

  // Do a pending collector transition.
  void DoPendingCollectorTransition() REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);

  // Deflate monitors, ... and trim the spaces.
  void Trim(Thread* self) REQUIRES(!*gc_complete_lock_);

  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeRosAllocThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();
  void AssertThreadLocalBuffersAreRevoked(Thread* thread);
  void AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
  void RosAllocVerification(TimingLogger* timings, const char* name)
      REQUIRES(Locks::mutator_lock_);

  accounting::HeapBitmap* GetLiveBitmap() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }

  accounting::HeapBitmap* GetMarkBitmap() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }

  accounting::ObjectStack* GetLiveStack() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return live_stack_.get();
  }

  void PreZygoteFork() NO_THREAD_SAFETY_ANALYSIS;

  // Mark and empty stack.
  void FlushAllocStack()
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);

  // Revoke all the thread-local allocation stacks.
  void RevokeAllThreadLocalAllocationStacks(Thread* self)
      REQUIRES(Locks::mutator_lock_, !Locks::runtime_shutdown_lock_, !Locks::thread_list_lock_);

  // Mark all the objects in the allocation stack in the specified bitmap.
  // TODO: Refactor?
  void MarkAllocStack(accounting::SpaceBitmap<kObjectAlignment>* bitmap1,
                      accounting::SpaceBitmap<kObjectAlignment>* bitmap2,
                      accounting::SpaceBitmap<kLargeObjectAlignment>* large_objects,
                      accounting::ObjectStack* stack)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);

  // Mark the specified allocation stack as live.
  void MarkAllocStackAsLive(accounting::ObjectStack* stack)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);

  // Unbind any bound bitmaps.
  void UnBindBitmaps()
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns the boot image spaces. There may be multiple boot image spaces.
  const std::vector<space::ImageSpace*>& GetBootImageSpaces() const {
    return boot_image_spaces_;
  }

  bool ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsInBootImageOatFile(const void* p) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  void GetBootImagesSize(uint32_t* boot_image_begin,
                         uint32_t* boot_image_end,
                         uint32_t* boot_oat_begin,
                         uint32_t* boot_oat_end);

  // Permenantly disable moving garbage collection.
  void DisableMovingGc() REQUIRES(!*gc_complete_lock_);

  space::DlMallocSpace* GetDlMallocSpace() const {
    return dlmalloc_space_;
  }

  space::RosAllocSpace* GetRosAllocSpace() const {
    return rosalloc_space_;
  }

  // Return the corresponding rosalloc space.
  space::RosAllocSpace* GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  space::MallocSpace* GetNonMovingSpace() const {
    return non_moving_space_;
  }

  space::LargeObjectSpace* GetLargeObjectsSpace() const {
    return large_object_space_;
  }

  // Returns the free list space that may contain movable objects (the
  // one that's not the non-moving space), either rosalloc_space_ or
  // dlmalloc_space_.
  space::MallocSpace* GetPrimaryFreeListSpace() {
    if (kUseRosAlloc) {
      DCHECK(rosalloc_space_ != nullptr);
      // reinterpret_cast is necessary as the space class hierarchy
      // isn't known (#included) yet here.
      return reinterpret_cast<space::MallocSpace*>(rosalloc_space_);
    } else {
      DCHECK(dlmalloc_space_ != nullptr);
      return reinterpret_cast<space::MallocSpace*>(dlmalloc_space_);
    }
  }

  void DumpSpaces(std::ostream& stream) const REQUIRES_SHARED(Locks::mutator_lock_);
  std::string DumpSpaces() const REQUIRES_SHARED(Locks::mutator_lock_);

  // GC performance measuring
  void DumpGcPerformanceInfo(std::ostream& os)
      REQUIRES(!*gc_complete_lock_);
  void ResetGcPerformanceInfo() REQUIRES(!*gc_complete_lock_);

  // Thread pool.
  void CreateThreadPool();
  void DeleteThreadPool();
  ThreadPool* GetThreadPool() {
    return thread_pool_.get();
  }
  size_t GetParallelGCThreadCount() const {
    return parallel_gc_threads_;
  }
  size_t GetConcGCThreadCount() const {
    return conc_gc_threads_;
  }
  accounting::ModUnionTable* FindModUnionTableFromSpace(space::Space* space);
  void AddModUnionTable(accounting::ModUnionTable* mod_union_table);

  accounting::RememberedSet* FindRememberedSetFromSpace(space::Space* space);
  void AddRememberedSet(accounting::RememberedSet* remembered_set);
  // Also deletes the remebered set.
  void RemoveRememberedSet(space::Space* space);

  bool IsCompilingBoot() const;
  bool HasBootImageSpace() const {
    return !boot_image_spaces_.empty();
  }

  ReferenceProcessor* GetReferenceProcessor() {
    return reference_processor_.get();
  }
  TaskProcessor* GetTaskProcessor() {
    return task_processor_.get();
  }

  bool HasZygoteSpace() const {
    return zygote_space_ != nullptr;
  }

  collector::ConcurrentCopying* ConcurrentCopyingCollector() {
    return concurrent_copying_collector_;
  }

  CollectorType CurrentCollectorType() {
    return collector_type_;
  }

  bool IsGcConcurrentAndMoving() const {
    if (IsGcConcurrent() && IsMovingGc(collector_type_)) {
      // Assume no transition when a concurrent moving collector is used.
      DCHECK_EQ(collector_type_, foreground_collector_type_);
      return true;
    }
    return false;
  }

  bool IsMovingGCDisabled(Thread* self) REQUIRES(!*gc_complete_lock_) {
    MutexLock mu(self, *gc_complete_lock_);
    return disable_moving_gc_count_ > 0;
  }

  // Request an asynchronous trim.
  void RequestTrim(Thread* self) REQUIRES(!*pending_task_lock_);

  // Request asynchronous GC.
  void RequestConcurrentGC(Thread* self, GcCause cause, bool force_full)
      REQUIRES(!*pending_task_lock_);

  // Whether or not we may use a garbage collector, used so that we only create collectors we need.
  bool MayUseCollector(CollectorType type) const;

  // Used by tests to reduce timinig-dependent flakiness in OOME behavior.
  void SetMinIntervalHomogeneousSpaceCompactionByOom(uint64_t interval) {
    min_interval_homogeneous_space_compaction_by_oom_ = interval;
  }

  // Helpers for android.os.Debug.getRuntimeStat().
  uint64_t GetGcCount() const;
  uint64_t GetGcTime() const;
  uint64_t GetBlockingGcCount() const;
  uint64_t GetBlockingGcTime() const;
  void DumpGcCountRateHistogram(std::ostream& os) const REQUIRES(!*gc_complete_lock_);
  void DumpBlockingGcCountRateHistogram(std::ostream& os) const REQUIRES(!*gc_complete_lock_);

  // Allocation tracking support
  // Callers to this function use double-checked locking to ensure safety on allocation_records_
  bool IsAllocTrackingEnabled() const {
    return alloc_tracking_enabled_.LoadRelaxed();
  }

  void SetAllocTrackingEnabled(bool enabled) REQUIRES(Locks::alloc_tracker_lock_) {
    alloc_tracking_enabled_.StoreRelaxed(enabled);
  }

  AllocRecordObjectMap* GetAllocationRecords() const
      REQUIRES(Locks::alloc_tracker_lock_) {
    return allocation_records_.get();
  }

  void SetAllocationRecords(AllocRecordObjectMap* records)
      REQUIRES(Locks::alloc_tracker_lock_);

  void VisitAllocationRecords(RootVisitor* visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);

  void SweepAllocationRecords(IsMarkedVisitor* visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);

  void DisallowNewAllocationRecords() const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);

  void AllowNewAllocationRecords() const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);

  void BroadcastForNewAllocationRecords() const
      REQUIRES(!Locks::alloc_tracker_lock_);

  void DisableGCForShutdown() REQUIRES(!*gc_complete_lock_);

  // Create a new alloc space and compact default alloc space to it.
  HomogeneousSpaceCompactResult PerformHomogeneousSpaceCompact() REQUIRES(!*gc_complete_lock_);
  bool SupportHomogeneousSpaceCompactAndCollectorTransitions() const;

  // Install an allocation listener.
  void SetAllocationListener(AllocationListener* l);
  // Remove an allocation listener. Note: the listener must not be deleted, as for performance
  // reasons, we assume it stays valid when we read it (so that we don't require a lock).
  void RemoveAllocationListener();

  // Install a gc pause listener.
  void SetGcPauseListener(GcPauseListener* l);
  // Get the currently installed gc pause listener, or null.
  GcPauseListener* GetGcPauseListener() {
    return gc_pause_listener_.LoadAcquire();
  }
  // Remove a gc pause listener. Note: the listener must not be deleted, as for performance
  // reasons, we assume it stays valid when we read it (so that we don't require a lock).
  void RemoveGcPauseListener();

  const Verification* GetVerification() const;

 private:
  class ConcurrentGCTask;
  class CollectorTransitionTask;
  class HeapTrimTask;

  // Compact source space to target space. Returns the collector used.
  collector::GarbageCollector* Compact(space::ContinuousMemMapAllocSpace* target_space,
                                       space::ContinuousMemMapAllocSpace* source_space,
                                       GcCause gc_cause)
      REQUIRES(Locks::mutator_lock_);

  void LogGC(GcCause gc_cause, collector::GarbageCollector* collector);
  void StartGC(Thread* self, GcCause cause, CollectorType collector_type)
      REQUIRES(!*gc_complete_lock_);
  void FinishGC(Thread* self, collector::GcType gc_type) REQUIRES(!*gc_complete_lock_);

  // Create a mem map with a preferred base address.
  static MemMap* MapAnonymousPreferredAddress(const char* name, uint8_t* request_begin,
                                              size_t capacity, std::string* out_error_str);

  bool SupportHSpaceCompaction() const {
    // Returns true if we can do hspace compaction
    return main_space_backup_ != nullptr;
  }

  static ALWAYS_INLINE bool AllocatorHasAllocationStack(AllocatorType allocator_type) {
    return
        allocator_type != kAllocatorTypeBumpPointer &&
        allocator_type != kAllocatorTypeTLAB &&
        allocator_type != kAllocatorTypeRegion &&
        allocator_type != kAllocatorTypeRegionTLAB;
  }
  static ALWAYS_INLINE bool AllocatorMayHaveConcurrentGC(AllocatorType allocator_type) {
    if (kUseReadBarrier) {
      // Read barrier may have the TLAB allocator but is always concurrent. TODO: clean this up.
      return true;
    }
    return
        allocator_type != kAllocatorTypeBumpPointer &&
        allocator_type != kAllocatorTypeTLAB;
  }
  static bool IsMovingGc(CollectorType collector_type) {
    return
        collector_type == kCollectorTypeSS ||
        collector_type == kCollectorTypeGSS ||
        collector_type == kCollectorTypeCC ||
        collector_type == kCollectorTypeCCBackground ||
        collector_type == kCollectorTypeMC ||
        collector_type == kCollectorTypeHomogeneousSpaceCompact;
  }
  bool ShouldAllocLargeObject(ObjPtr<mirror::Class> c, size_t byte_count) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  ALWAYS_INLINE void CheckConcurrentGC(Thread* self,
                                       size_t new_num_bytes_allocated,
                                       ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*pending_task_lock_, !*gc_complete_lock_);

  accounting::ObjectStack* GetMarkStack() {
    return mark_stack_.get();
  }

  // We don't force this to be inlined since it is a slow path.
  template <bool kInstrumented, typename PreFenceVisitor>
  mirror::Object* AllocLargeObject(Thread* self,
                                   ObjPtr<mirror::Class>* klass,
                                   size_t byte_count,
                                   const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !*backtrace_lock_);

  // Handles Allocate()'s slow allocation path with GC involved after
  // an initial allocation attempt failed.
  mirror::Object* AllocateInternalWithGc(Thread* self,
                                         AllocatorType allocator,
                                         bool instrumented,
                                         size_t num_bytes,
                                         size_t* bytes_allocated,
                                         size_t* usable_size,
                                         size_t* bytes_tl_bulk_allocated,
                                         ObjPtr<mirror::Class>* klass)
      REQUIRES(!Locks::thread_suspend_count_lock_, !*gc_complete_lock_, !*pending_task_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Allocate into a specific space.
  mirror::Object* AllocateInto(Thread* self,
                               space::AllocSpace* space,
                               ObjPtr<mirror::Class> c,
                               size_t bytes)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Need to do this with mutators paused so that somebody doesn't accidentally allocate into the
  // wrong space.
  void SwapSemiSpaces() REQUIRES(Locks::mutator_lock_);

  // Try to allocate a number of bytes, this function never does any GCs. Needs to be inlined so
  // that the switch statement is constant optimized in the entrypoints.
  template <const bool kInstrumented, const bool kGrow>
  ALWAYS_INLINE mirror::Object* TryToAllocate(Thread* self,
                                              AllocatorType allocator_type,
                                              size_t alloc_size,
                                              size_t* bytes_allocated,
                                              size_t* usable_size,
                                              size_t* bytes_tl_bulk_allocated)
      REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::Object* AllocWithNewTLAB(Thread* self,
                                   size_t alloc_size,
                                   bool grow,
                                   size_t* bytes_allocated,
                                   size_t* usable_size,
                                   size_t* bytes_tl_bulk_allocated)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE bool IsOutOfMemoryOnAllocation(AllocatorType allocator_type,
                                               size_t alloc_size,
                                               bool grow);

  // Run the finalizers. If timeout is non zero, then we use the VMRuntime version.
  void RunFinalization(JNIEnv* env, uint64_t timeout);

  // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
  // waited for.
  collector::GcType WaitForGcToCompleteLocked(GcCause cause, Thread* self)
      REQUIRES(gc_complete_lock_);

  void RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time)
      REQUIRES(!*pending_task_lock_);

  void RequestConcurrentGCAndSaveObject(Thread* self, bool force_full, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*pending_task_lock_);
  bool IsGCRequestPending() const;

  // Sometimes CollectGarbageInternal decides to run a different Gc than you requested. Returns
  // which type of Gc was actually ran.
  collector::GcType CollectGarbageInternal(collector::GcType gc_plan,
                                           GcCause gc_cause,
                                           bool clear_soft_references)
      REQUIRES(!*gc_complete_lock_, !Locks::heap_bitmap_lock_, !Locks::thread_suspend_count_lock_,
               !*pending_task_lock_);

  void PreGcVerification(collector::GarbageCollector* gc)
      REQUIRES(!Locks::mutator_lock_, !*gc_complete_lock_);
  void PreGcVerificationPaused(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  void PrePauseRosAllocVerification(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_);
  void PreSweepingGcVerification(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  void PostGcVerification(collector::GarbageCollector* gc)
      REQUIRES(!Locks::mutator_lock_, !*gc_complete_lock_);
  void PostGcVerificationPaused(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);

  // Find a collector based on GC type.
  collector::GarbageCollector* FindCollectorByGcType(collector::GcType gc_type);

  // Create the main free list malloc space, either a RosAlloc space or DlMalloc space.
  void CreateMainMallocSpace(MemMap* mem_map,
                             size_t initial_size,
                             size_t growth_limit,
                             size_t capacity);

  // Create a malloc space based on a mem map. Does not set the space as default.
  space::MallocSpace* CreateMallocSpaceFromMemMap(MemMap* mem_map,
                                                  size_t initial_size,
                                                  size_t growth_limit,
                                                  size_t capacity,
                                                  const char* name,
                                                  bool can_move_objects);

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection. bytes_allocated_before_gc is used to measure bytes / second for the period which
  // the GC was run.
  void GrowForUtilization(collector::GarbageCollector* collector_ran,
                          uint64_t bytes_allocated_before_gc = 0);

  size_t GetPercentFree();

  // Swap the allocation stack with the live stack.
  void SwapStacks() REQUIRES_SHARED(Locks::mutator_lock_);

  // Clear cards and update the mod union table. When process_alloc_space_cards is true,
  // if clear_alloc_space_cards is true, then we clear cards instead of ageing them. We do
  // not process the alloc space if process_alloc_space_cards is false.
  void ProcessCards(TimingLogger* timings,
                    bool use_rem_sets,
                    bool process_alloc_space_cards,
                    bool clear_alloc_space_cards)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Push an object onto the allocation stack.
  void PushOnAllocationStack(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);
  void PushOnAllocationStackWithInternalGC(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);
  void PushOnThreadLocalAllocationStackWithInternalGC(Thread* thread, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_);

  void ClearConcurrentGCRequest();
  void ClearPendingTrim(Thread* self) REQUIRES(!*pending_task_lock_);
  void ClearPendingCollectorTransition(Thread* self) REQUIRES(!*pending_task_lock_);

  // What kind of concurrency behavior is the runtime after? Currently true for concurrent mark
  // sweep GC, false for other GC types.
  bool IsGcConcurrent() const ALWAYS_INLINE {
    return collector_type_ == kCollectorTypeCMS ||
        collector_type_ == kCollectorTypeCC ||
        collector_type_ == kCollectorTypeCCBackground;
  }

  // Trim the managed and native spaces by releasing unused memory back to the OS.
  void TrimSpaces(Thread* self) REQUIRES(!*gc_complete_lock_);

  // Trim 0 pages at the end of reference tables.
  void TrimIndirectReferenceTables(Thread* self);

  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsInternal(Visitor&& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsInternalRegionSpace(Visitor&& visitor)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);

  void UpdateGcCountRateHistograms() REQUIRES(gc_complete_lock_);

  // GC stress mode attempts to do one GC per unique backtrace.
  void CheckGcStressMode(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !*backtrace_lock_);

  collector::GcType NonStickyGcType() const {
    return HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
  }

  // How large new_native_bytes_allocated_ can grow before we trigger a new
  // GC.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // Reuse max_free_ for the native allocation gc watermark, so that the
    // native heap is treated in the same way as the Java heap in the case
    // where the gc watermark update would exceed max_free_. Using max_free_
    // instead of the target utilization means the watermark doesn't depend on
    // the current number of registered native allocations.
    return max_free_;
  }

  void TraceHeapSize(size_t heap_size);

  // Remove a vlog code from heap-inl.h which is transitively included in half the world.
  static void VlogHeapGrowth(size_t max_allowed_footprint, size_t new_footprint, size_t alloc_size);

  // All-known continuous spaces, where objects lie within fixed bounds.
  std::vector<space::ContinuousSpace*> continuous_spaces_ GUARDED_BY(Locks::mutator_lock_);

  // All-known discontinuous spaces, where objects may be placed throughout virtual memory.
  std::vector<space::DiscontinuousSpace*> discontinuous_spaces_ GUARDED_BY(Locks::mutator_lock_);

  // All-known alloc spaces, where objects may be or have been allocated.
  std::vector<space::AllocSpace*> alloc_spaces_;

  // A space where non-movable objects are allocated, when compaction is enabled it contains
  // Classes, ArtMethods, ArtFields, and non moving objects.
  space::MallocSpace* non_moving_space_;

  // Space which we use for the kAllocatorTypeROSAlloc.
  space::RosAllocSpace* rosalloc_space_;

  // Space which we use for the kAllocatorTypeDlMalloc.
  space::DlMallocSpace* dlmalloc_space_;

  // The main space is the space which the GC copies to and from on process state updates. This
  // space is typically either the dlmalloc_space_ or the rosalloc_space_.
  space::MallocSpace* main_space_;

  // The large object space we are currently allocating into.
  space::LargeObjectSpace* large_object_space_;

  // The card table, dirtied by the write barrier.
  std::unique_ptr<accounting::CardTable> card_table_;

  std::unique_ptr<accounting::ReadBarrierTable> rb_table_;

  // A mod-union table remembers all of the references from the it's space to other spaces.
  AllocationTrackingSafeMap<space::Space*, accounting::ModUnionTable*, kAllocatorTagHeap>
      mod_union_tables_;

  // A remembered set remembers all of the references from the it's space to the target space.
  AllocationTrackingSafeMap<space::Space*, accounting::RememberedSet*, kAllocatorTagHeap>
      remembered_sets_;

  // The current collector type.
  CollectorType collector_type_;
  // Which collector we use when the app is in the foreground.
  CollectorType foreground_collector_type_;
  // Which collector we will use when the app is notified of a transition to background.
  CollectorType background_collector_type_;
  // Desired collector type, heap trimming daemon transitions the heap if it is != collector_type_.
  CollectorType desired_collector_type_;

  // Lock which guards pending tasks.
  Mutex* pending_task_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // How many GC threads we may use for paused parts of garbage collection.
  const size_t parallel_gc_threads_;

  // How many GC threads we may use for unpaused parts of garbage collection.
  const size_t conc_gc_threads_;

  // Boolean for if we are in low memory mode.
  const bool low_memory_mode_;

  // If we get a pause longer than long pause log threshold, then we print out the GC after it
  // finishes.
  const size_t long_pause_log_threshold_;

  // If we get a GC longer than long GC log threshold, then we print out the GC after it finishes.
  const size_t long_gc_log_threshold_;

  // If we ignore the max footprint it lets the heap grow until it hits the heap capacity, this is
  // useful for benchmarking since it reduces time spent in GC to a low %.
  const bool ignore_max_footprint_;

  // Lock which guards zygote space creation.
  Mutex zygote_creation_lock_;

  // Non-null iff we have a zygote space. Doesn't contain the large objects allocated before
  // zygote space creation.
  space::ZygoteSpace* zygote_space_;

  // Minimum allocation size of large object.
  size_t large_object_threshold_;

  // Guards access to the state of GC, associated conditional variable is used to signal when a GC
  // completes.
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::unique_ptr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);

  // Used to synchronize between JNI critical calls and the thread flip of the CC collector.
  Mutex* thread_flip_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::unique_ptr<ConditionVariable> thread_flip_cond_ GUARDED_BY(thread_flip_lock_);
  // This counter keeps track of how many threads are currently in a JNI critical section. This is
  // incremented once per thread even with nested enters.
  size_t disable_thread_flip_count_ GUARDED_BY(thread_flip_lock_);
  bool thread_flip_running_ GUARDED_BY(thread_flip_lock_);

  // Reference processor;
  std::unique_ptr<ReferenceProcessor> reference_processor_;

  // Task processor, proxies heap trim requests to the daemon threads.
  std::unique_ptr<TaskProcessor> task_processor_;

  // Collector type of the running GC.
  volatile CollectorType collector_type_running_ GUARDED_BY(gc_complete_lock_);

  // Cause of the last running GC.
  volatile GcCause last_gc_cause_ GUARDED_BY(gc_complete_lock_);

  // The thread currently running the GC.
  volatile Thread* thread_running_gc_ GUARDED_BY(gc_complete_lock_);

  // Last Gc type we ran. Used by WaitForConcurrentGc to know which Gc was waited on.
  volatile collector::GcType last_gc_type_ GUARDED_BY(gc_complete_lock_);
  collector::GcType next_gc_type_;

  // Maximum size that the heap can reach.
  size_t capacity_;

  // The size the heap is limited to. This is initially smaller than capacity, but for largeHeap
  // programs it is "cleared" making it the same as capacity.
  size_t growth_limit_;

  // When the number of bytes allocated exceeds the footprint TryAllocate returns null indicating
  // a GC should be triggered.
  size_t max_allowed_footprint_;

  // When num_bytes_allocated_ exceeds this amount then a concurrent GC should be requested so that
  // it completes ahead of an allocation failing.
  size_t concurrent_start_bytes_;

  // Since the heap was created, how many bytes have been freed.
  uint64_t total_bytes_freed_ever_;

  // Since the heap was created, how many objects have been freed.
  uint64_t total_objects_freed_ever_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  Atomic<size_t> num_bytes_allocated_;

  // Number of registered native bytes allocated since the last time GC was
  // triggered. Adjusted after each RegisterNativeAllocation and
  // RegisterNativeFree. Used to determine when to trigger GC for native
  // allocations.
  // See the REDESIGN section of go/understanding-register-native-allocation.
  Atomic<size_t> new_native_bytes_allocated_;

  // Number of registered native bytes allocated prior to the last time GC was
  // triggered, for debugging purposes. The current number of registered
  // native bytes is determined by taking the sum of
  // old_native_bytes_allocated_ and new_native_bytes_allocated_.
  Atomic<size_t> old_native_bytes_allocated_;

  // Number of bytes freed by thread local buffer revokes. This will
  // cancel out the ahead-of-time bulk counting of bytes allocated in
  // rosalloc thread-local buffers.  It is temporarily accumulated
  // here to be subtracted from num_bytes_allocated_ later at the next
  // GC.
  Atomic<size_t> num_bytes_freed_revoke_;

  // Info related to the current or previous GC iteration.
  collector::Iteration current_gc_iteration_;

  // Heap verification flags.
  const bool verify_missing_card_marks_;
  const bool verify_system_weaks_;
  const bool verify_pre_gc_heap_;
  const bool verify_pre_sweeping_heap_;
  const bool verify_post_gc_heap_;
  const bool verify_mod_union_table_;
  bool verify_pre_gc_rosalloc_;
  bool verify_pre_sweeping_rosalloc_;
  bool verify_post_gc_rosalloc_;
  const bool gc_stress_mode_;

  // RAII that temporarily disables the rosalloc verification during
  // the zygote fork.
  class ScopedDisableRosAllocVerification {
   private:
    Heap* const heap_;
    const bool orig_verify_pre_gc_;
    const bool orig_verify_pre_sweeping_;
    const bool orig_verify_post_gc_;

   public:
    explicit ScopedDisableRosAllocVerification(Heap* heap)
        : heap_(heap),
          orig_verify_pre_gc_(heap_->verify_pre_gc_rosalloc_),
          orig_verify_pre_sweeping_(heap_->verify_pre_sweeping_rosalloc_),
          orig_verify_post_gc_(heap_->verify_post_gc_rosalloc_) {
      heap_->verify_pre_gc_rosalloc_ = false;
      heap_->verify_pre_sweeping_rosalloc_ = false;
      heap_->verify_post_gc_rosalloc_ = false;
    }
    ~ScopedDisableRosAllocVerification() {
      heap_->verify_pre_gc_rosalloc_ = orig_verify_pre_gc_;
      heap_->verify_pre_sweeping_rosalloc_ = orig_verify_pre_sweeping_;
      heap_->verify_post_gc_rosalloc_ = orig_verify_post_gc_;
    }
  };

  // Parallel GC data structures.
  std::unique_ptr<ThreadPool> thread_pool_;

  // For a GC cycle, a bitmap that is set corresponding to the
  std::unique_ptr<accounting::HeapBitmap> live_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);
  std::unique_ptr<accounting::HeapBitmap> mark_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);

  // Mark stack that we reuse to avoid re-allocating the mark stack.
  std::unique_ptr<accounting::ObjectStack> mark_stack_;

  // Allocation stack, new allocations go here so that we can do sticky mark bits. This enables us
  // to use the live bitmap as the old mark bitmap.
  const size_t max_allocation_stack_size_;
  std::unique_ptr<accounting::ObjectStack> allocation_stack_;

  // Second allocation stack so that we can process allocation with the heap unlocked.
  std::unique_ptr<accounting::ObjectStack> live_stack_;

  // Allocator type.
  AllocatorType current_allocator_;
  const AllocatorType current_non_moving_allocator_;

  // Which GCs we run in order when an allocation fails.
  std::vector<collector::GcType> gc_plan_;

  // Bump pointer spaces.
  space::BumpPointerSpace* bump_pointer_space_;
  // Temp space is the space which the semispace collector copies to.
  space::BumpPointerSpace* temp_space_;

  space::RegionSpace* region_space_;

  // Minimum free guarantees that you always have at least min_free_ free bytes after growing for
  // utilization, regardless of target utilization ratio.
  size_t min_free_;

  // The ideal maximum free size, when we grow the heap for utilization.
  size_t max_free_;

  // Target ideal heap utilization ratio
  double target_utilization_;

  // How much more we grow the heap when we are a foreground app instead of background.
  double foreground_heap_growth_multiplier_;

  // Total time which mutators are paused or waiting for GC to complete.
  uint64_t total_wait_time_;

  // The current state of heap verification, may be enabled or disabled.
  VerifyObjectMode verify_object_mode_;

  // Compacting GC disable count, prevents compacting GC from running iff > 0.
  size_t disable_moving_gc_count_ GUARDED_BY(gc_complete_lock_);

  std::vector<collector::GarbageCollector*> garbage_collectors_;
  collector::SemiSpace* semi_space_collector_;
  collector::MarkCompact* mark_compact_collector_;
  collector::ConcurrentCopying* concurrent_copying_collector_;

  const bool is_running_on_memory_tool_;
  const bool use_tlab_;

  // Pointer to the space which becomes the new main space when we do homogeneous space compaction.
  // Use unique_ptr since the space is only added during the homogeneous compaction phase.
  std::unique_ptr<space::MallocSpace> main_space_backup_;

  // Minimal interval allowed between two homogeneous space compactions caused by OOM.
  uint64_t min_interval_homogeneous_space_compaction_by_oom_;

  // Times of the last homogeneous space compaction caused by OOM.
  uint64_t last_time_homogeneous_space_compaction_by_oom_;

  // Saved OOMs by homogeneous space compaction.
  Atomic<size_t> count_delayed_oom_;

  // Count for requested homogeneous space compaction.
  Atomic<size_t> count_requested_homogeneous_space_compaction_;

  // Count for ignored homogeneous space compaction.
  Atomic<size_t> count_ignored_homogeneous_space_compaction_;

  // Count for performed homogeneous space compaction.
  Atomic<size_t> count_performed_homogeneous_space_compaction_;

  // Whether or not a concurrent GC is pending.
  Atomic<bool> concurrent_gc_pending_;

  // Active tasks which we can modify (change target time, desired collector type, etc..).
  CollectorTransitionTask* pending_collector_transition_ GUARDED_BY(pending_task_lock_);
  HeapTrimTask* pending_heap_trim_ GUARDED_BY(pending_task_lock_);

  // Whether or not we use homogeneous space compaction to avoid OOM errors.
  bool use_homogeneous_space_compaction_for_oom_;

  // True if the currently running collection has made some thread wait.
  bool running_collection_is_blocking_ GUARDED_BY(gc_complete_lock_);
  // The number of blocking GC runs.
  uint64_t blocking_gc_count_;
  // The total duration of blocking GC runs.
  uint64_t blocking_gc_time_;
  // The duration of the window for the GC count rate histograms.
  static constexpr uint64_t kGcCountRateHistogramWindowDuration = MsToNs(10 * 1000);  // 10s.
  // The last time when the GC count rate histograms were updated.
  // This is rounded by kGcCountRateHistogramWindowDuration (a multiple of 10s).
  uint64_t last_update_time_gc_count_rate_histograms_;
  // The running count of GC runs in the last window.
  uint64_t gc_count_last_window_;
  // The running count of blocking GC runs in the last window.
  uint64_t blocking_gc_count_last_window_;
  // The maximum number of buckets in the GC count rate histograms.
  static constexpr size_t kGcCountRateMaxBucketCount = 200;
  // The histogram of the number of GC invocations per window duration.
  Histogram<uint64_t> gc_count_rate_histogram_ GUARDED_BY(gc_complete_lock_);
  // The histogram of the number of blocking GC invocations per window duration.
  Histogram<uint64_t> blocking_gc_count_rate_histogram_ GUARDED_BY(gc_complete_lock_);

  // Allocation tracking support
  Atomic<bool> alloc_tracking_enabled_;
  std::unique_ptr<AllocRecordObjectMap> allocation_records_;

  // GC stress related data structures.
  Mutex* backtrace_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  // Debugging variables, seen backtraces vs unique backtraces.
  Atomic<uint64_t> seen_backtrace_count_;
  Atomic<uint64_t> unique_backtrace_count_;
  // Stack trace hashes that we already saw,
  std::unordered_set<uint64_t> seen_backtraces_ GUARDED_BY(backtrace_lock_);

  // We disable GC when we are shutting down the runtime in case there are daemon threads still
  // allocating.
  bool gc_disabled_for_shutdown_ GUARDED_BY(gc_complete_lock_);

  // Boot image spaces.
  std::vector<space::ImageSpace*> boot_image_spaces_;

  // An installed allocation listener.
  Atomic<AllocationListener*> alloc_listener_;
  // An installed GC Pause listener.
  Atomic<GcPauseListener*> gc_pause_listener_;

  std::unique_ptr<Verification> verification_;

  friend class CollectorTransitionTask;
  friend class collector::GarbageCollector;
  friend class collector::MarkCompact;
  friend class collector::ConcurrentCopying;
  friend class collector::MarkSweep;
  friend class collector::SemiSpace;
  friend class ReferenceQueue;
  friend class ScopedGCCriticalSection;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_H_
