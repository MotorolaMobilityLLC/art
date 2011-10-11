// Copyright 2011 Google Inc. All Rights Reserved.

#include "heap.h"

#include <limits>
#include <vector>

#include "UniquePtr.h"
#include "image.h"
#include "mark_sweep.h"
#include "object.h"
#include "space.h"
#include "stl_util.h"
#include "thread_list.h"

namespace art {

std::vector<Space*> Heap::spaces_;

Space* Heap::alloc_space_ = NULL;

size_t Heap::maximum_size_ = 0;

size_t Heap::num_bytes_allocated_ = 0;

size_t Heap::num_objects_allocated_ = 0;

bool Heap::is_gc_running_ = false;

HeapBitmap* Heap::mark_bitmap_ = NULL;

HeapBitmap* Heap::live_bitmap_ = NULL;

Class* Heap::java_lang_ref_FinalizerReference_ = NULL;
Class* Heap::java_lang_ref_ReferenceQueue_ = NULL;

MemberOffset Heap::reference_referent_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queue_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queueNext_offset_ = MemberOffset(0);
MemberOffset Heap::reference_pendingNext_offset_ = MemberOffset(0);
MemberOffset Heap::finalizer_reference_zombie_offset_ = MemberOffset(0);

float Heap::target_utilization_ = 0.5;

Mutex* Heap::lock_ = NULL;

bool Heap::verify_objects_ = false;

class ScopedHeapLock {
 public:
  ScopedHeapLock() {
    Heap::Lock();
  }

  ~ScopedHeapLock() {
    Heap::Unlock();
  }
};

void Heap::Init(size_t initial_size, size_t maximum_size,
                const std::vector<std::string>& image_file_names) {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Heap::Init entering";
  }

  // bounds of all spaces for allocating live and mark bitmaps
  // there will be at least one space (the alloc space),
  // so set to base to max and limit to min to start
  byte* base = reinterpret_cast<byte*>(std::numeric_limits<uintptr_t>::max());
  byte* limit = reinterpret_cast<byte*>(std::numeric_limits<uintptr_t>::min());

  byte* requested_base = NULL;
  std::vector<Space*> image_spaces;
  for (size_t i = 0; i < image_file_names.size(); i++) {
    Space* space = Space::CreateFromImage(image_file_names[i]);
    if (space == NULL) {
      LOG(FATAL) << "Failed to create space from " << image_file_names[i];
    }
    image_spaces.push_back(space);
    spaces_.push_back(space);
    byte* oat_limit_addr = space->GetImageHeader().GetOatLimitAddr();
    if (oat_limit_addr > requested_base) {
      requested_base = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(oat_limit_addr),
                                                       kPageSize));
    }
    base = std::min(base, space->GetBase());
    limit = std::max(limit, space->GetLimit());
  }

  Space* space = Space::Create(initial_size, maximum_size, requested_base);
  if (space == NULL) {
    LOG(FATAL) << "Failed to create alloc space";
  }
  base = std::min(base, space->GetBase());
  limit = std::max(limit, space->GetLimit());
  DCHECK_LT(base, limit);
  size_t num_bytes = limit - base;

  // Allocate the initial live bitmap.
  UniquePtr<HeapBitmap> live_bitmap(HeapBitmap::Create(base, num_bytes));
  if (live_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create live bitmap";
  }

  // Allocate the initial mark bitmap.
  UniquePtr<HeapBitmap> mark_bitmap(HeapBitmap::Create(base, num_bytes));
  if (mark_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create mark bitmap";
  }

  alloc_space_ = space;
  spaces_.push_back(space);
  maximum_size_ = maximum_size;
  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();

  num_bytes_allocated_ = 0;
  num_objects_allocated_ = 0;

  // TODO: allocate the card table

  // Make image objects live (after live_bitmap_ is set)
  for (size_t i = 0; i < image_spaces.size(); i++) {
    RecordImageAllocations(image_spaces[i]);
  }

  Heap::EnableObjectValidation();

  // It's still to early to take a lock because there are no threads yet,
  // but we can create the heap lock now. We don't create it earlier to
  // make it clear that you can't use locks during heap initialization.
  lock_ = new Mutex("Heap lock");

  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Heap::Init exiting";
  }
}

void Heap::Destroy() {
  ScopedHeapLock lock;
  STLDeleteElements(&spaces_);
  if (mark_bitmap_ != NULL) {
    delete mark_bitmap_;
    mark_bitmap_ = NULL;
  }
  if (live_bitmap_ != NULL) {
    delete live_bitmap_;
  }
  live_bitmap_ = NULL;
}

Object* Heap::AllocObject(Class* klass, size_t byte_count) {
  {
    ScopedHeapLock lock;
    DCHECK(klass == NULL || klass->GetDescriptor() == NULL ||
        (klass->IsClassClass() && byte_count >= sizeof(Class)) ||
        (klass->IsVariableSize() || klass->GetObjectSize() == byte_count));
    DCHECK_GE(byte_count, sizeof(Object));
    Object* obj = AllocateLocked(byte_count);
    if (obj != NULL) {
      obj->SetClass(klass);
      return obj;
    }
  }

  Thread::Current()->ThrowOutOfMemoryError(klass, byte_count);
  return NULL;
}

bool Heap::IsHeapAddress(const Object* obj) {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would
  // require taking the lock.
  if (!IsAligned(obj, kObjectAlignment)) {
    return false;
  }
  // TODO
  return true;
}

#if VERIFY_OBJECT_ENABLED
void Heap::VerifyObject(const Object* obj) {
  if (!verify_objects_) {
    return;
  }
  ScopedHeapLock lock;
  Heap::VerifyObjectLocked(obj);
}
#endif

void Heap::VerifyObjectLocked(const Object* obj) {
  lock_->AssertHeld();
  if (obj != NULL) {
    if (!IsAligned(obj, kObjectAlignment)) {
      LOG(FATAL) << "Object isn't aligned: " << obj;
    } else if (!live_bitmap_->Test(obj)) {
      // TODO: we don't hold a lock here as it is assumed the live bit map
      // isn't changing if the mutator is running.
      LOG(FATAL) << "Object is dead: " << obj;
    }
    // Ignore early dawn of the universe verifications
    if (num_objects_allocated_ > 10) {
      const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
          Object::ClassOffset().Int32Value();
      const Class* c = *reinterpret_cast<Class* const *>(raw_addr);
      if (c == NULL) {
        LOG(FATAL) << "Null class" << " in object: " << obj;
      } else if (!IsAligned(c, kObjectAlignment)) {
        LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
      } else if (!live_bitmap_->Test(c)) {
        LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
      }
      // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
      // Note: we don't use the accessors here as they have internal sanity checks
      // that we don't want to run
      raw_addr = reinterpret_cast<const byte*>(c) +
          Object::ClassOffset().Int32Value();
      const Class* c_c = *reinterpret_cast<Class* const *>(raw_addr);
      raw_addr = reinterpret_cast<const byte*>(c_c) +
          Object::ClassOffset().Int32Value();
      const Class* c_c_c = *reinterpret_cast<Class* const *>(raw_addr);
      CHECK_EQ(c_c, c_c_c);
    }
  }
}

void Heap::VerificationCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  Heap::VerifyObjectLocked(obj);
}

void Heap::VerifyHeap() {
  ScopedHeapLock lock;
  live_bitmap_->Walk(Heap::VerificationCallback, NULL);
}

void Heap::RecordAllocationLocked(Space* space, const Object* obj) {
#ifndef NDEBUG
  if (Runtime::Current()->IsStarted()) {
    lock_->AssertHeld();
  }
#endif
  size_t size = space->AllocationSize(obj);
  DCHECK_NE(size, 0u);
  num_bytes_allocated_ += size;
  num_objects_allocated_ += 1;

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    ++global_stats->allocated_objects;
    ++thread_stats->allocated_objects;
    global_stats->allocated_bytes += size;
    thread_stats->allocated_bytes += size;
  }

  live_bitmap_->Set(obj);
}

void Heap::RecordFreeLocked(Space* space, const Object* obj) {
  lock_->AssertHeld();
  size_t size = space->AllocationSize(obj);
  DCHECK_NE(size, 0u);
  if (size < num_bytes_allocated_) {
    num_bytes_allocated_ -= size;
  } else {
    num_bytes_allocated_ = 0;
  }
  live_bitmap_->Clear(obj);
  if (num_objects_allocated_ > 0) {
    num_objects_allocated_ -= 1;
  }

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    ++global_stats->freed_objects;
    ++thread_stats->freed_objects;
    global_stats->freed_bytes += size;
    thread_stats->freed_bytes += size;
  }
}

void Heap::RecordImageAllocations(Space* space) {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Heap::RecordImageAllocations entering";
  }
  DCHECK(!Runtime::Current()->IsStarted());
  CHECK(space != NULL);
  CHECK(live_bitmap_ != NULL);
  byte* current = space->GetBase() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  while (current < space->GetLimit()) {
    DCHECK(IsAligned(current, kObjectAlignment));
    const Object* obj = reinterpret_cast<const Object*>(current);
    live_bitmap_->Set(obj);
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Heap::RecordImageAllocations exiting";
  }
}

Object* Heap::AllocateLocked(size_t size) {
  lock_->AssertHeld();
  DCHECK(alloc_space_ != NULL);
  Space* space = alloc_space_;
  Object* obj = AllocateLocked(space, size);
  if (obj != NULL) {
    RecordAllocationLocked(space, obj);
  }
  return obj;
}

Object* Heap::AllocateLocked(Space* space, size_t size) {
  lock_->AssertHeld();

  // Fail impossible allocations.  TODO: collect soft references.
  if (size > maximum_size_) {
    return NULL;
  }

  Object* ptr = space->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed.  If the GC is running, block until it
  // completes and retry.
  if (is_gc_running_) {
    // The GC is concurrently tracing the heap.  Release the heap
    // lock, wait for the GC to complete, and retrying allocating.
    WaitForConcurrentGcToComplete();
    ptr = space->AllocWithoutGrowth(size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Another failure.  Our thread was starved or there may be too many
  // live objects.  Try a foreground GC.  This will have no effect if
  // the concurrent GC is already running.
  if (Runtime::Current()->HasStatsEnabled()) {
    ++Runtime::Current()->GetStats()->gc_for_alloc_count;
    ++Thread::Current()->GetStats()->gc_for_alloc_count;
  }
  LOG(INFO) << "GC_FOR_ALLOC: AllocWithoutGrowth: TODO: test";
  CollectGarbageInternal();
  ptr = space->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  LOG(INFO) << "GC_FOR_ALLOC: AllocWithGrowth: TODO: test";
  // Even that didn't work;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = space->AllocWithGrowth(size);
  if (ptr != NULL) {
    //size_t new_footprint = dvmHeapSourceGetIdealFootprint();
    size_t new_footprint = space->GetMaxAllowedFootprint();
    // OLD-TODO: may want to grow a little bit more so that the amount of
    //       free space is equal to the old free space + the
    //       utilization slop for the new allocation.
    LOG(INFO) << "Grow heap (frag case) to " << new_footprint / MB
              << "for " << size << "-byte allocation";
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is
  // really full, really fragmented, or the requested size is really
  // big.  Do another GC, collecting SoftReferences this time.  The VM
  // spec requires that all SoftReferences have been collected and
  // cleared before throwing an OOME.

  // OLD-TODO: wait for the finalizers from the previous GC to finish
  LOG(INFO) << "Forcing collection of SoftReferences for "
            << size << "-byte allocation";
  CollectGarbageInternal();
  ptr = space->AllocWithGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  LOG(ERROR) << "Out of memory on a " << size << " byte allocation";

  // TODO: tell the HeapSource to dump its state
  // TODO: dump stack traces for all threads

  return NULL;
}

int64_t Heap::GetMaxMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t Heap::GetTotalMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t Heap::GetFreeMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

class InstanceCounter {
 public:
  InstanceCounter(Class* c, bool count_assignable)
      : class_(c), count_assignable_(count_assignable), count_(0) {
  }

  size_t GetCount() {
    return count_;
  }

  static void Callback(Object* o, void* arg) {
    reinterpret_cast<InstanceCounter*>(arg)->VisitInstance(o);
  }

 private:
  void VisitInstance(Object* o) {
    Class* instance_class = o->GetClass();
    if (count_assignable_) {
      if (instance_class == class_) {
        ++count_;
      }
    } else {
      if (instance_class != NULL && class_->IsAssignableFrom(instance_class)) {
        ++count_;
      }
    }
  }

  Class* class_;
  bool count_assignable_;
  size_t count_;
};

int64_t Heap::CountInstances(Class* c, bool count_assignable) {
  ScopedHeapLock lock;
  InstanceCounter counter(c, count_assignable);
  live_bitmap_->Walk(InstanceCounter::Callback, &counter);
  return counter.GetCount();
}

void Heap::CollectGarbage() {
  ScopedHeapLock lock;
  CollectGarbageInternal();
}

void Heap::CollectGarbageInternal() {
  lock_->AssertHeld();

  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();

  size_t initial_size = num_bytes_allocated_;
  uint64_t t0 = NanoTime();
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep;

    mark_sweep.Init();

    mark_sweep.MarkRoots();

    // Push marked roots onto the mark stack

    // TODO: if concurrent
    //   unlock heap
    //   thread_list->ResumeAll();

    mark_sweep.RecursiveMark();

    // TODO: if concurrent
    //   lock heap
    //   thread_list->SuspendAll();
    //   re-mark root set
    //   scan dirty objects

    mark_sweep.ProcessReferences(false);

    // TODO: if concurrent
    //    swap bitmaps

    mark_sweep.Sweep();

    cleared_references = mark_sweep.GetClearedReferences();
  }

  GrowForUtilization();
  uint64_t t1 = NanoTime();
  thread_list->ResumeAll();

  EnqueueClearedReferences(&cleared_references);

  // TODO: somehow make the specific GC implementation (here MarkSweep) responsible for logging.
  size_t bytes_freed = initial_size - num_bytes_allocated_;
  bool is_small = (bytes_freed > 0 && bytes_freed < 1024);
  size_t kib_freed = (bytes_freed > 0 ? std::max(bytes_freed/1024, 1U) : 0);

  size_t footprint = alloc_space_->Size();
  size_t percentFree = 100 - static_cast<size_t>(100.0f * float(num_bytes_allocated_) / footprint);

  uint32_t duration = (t1 - t0)/1000/1000;
  LOG(INFO) << "GC freed " << (is_small ? "<" : "") << kib_freed << "KiB, "
            << percentFree << "% free "
            << (num_bytes_allocated_/1024) << "KiB/" << (footprint/1024) << "KiB, "
            << "paused " << duration << "ms";
}

void Heap::WaitForConcurrentGcToComplete() {
  lock_->AssertHeld();
}

/* Terminology:
 *  1. Footprint: Capacity we allocate from system.
 *  2. Active space: a.k.a. alloc_space_.
 *  3. Soft footprint: external allocation + spaces footprint + active space footprint
 *  4. Overhead: soft footprint excluding active.
 *
 * Layout: (The spaces below might not be contiguous, but are lumped together to depict size.)
 * |----------------------spaces footprint--------- --------------|----active space footprint----|
 *                                                                |--active space allocated--|
 * |--------------------soft footprint (include active)--------------------------------------|
 * |----------------soft footprint excluding active---------------|
 *                                                                |------------soft limit-------...|
 * |------------------------------------ideal footprint-----------------------------------------...|
 *
 */

// Sets the maximum number of bytes that the heap is allowed to
// allocate from the system.  Clamps to the appropriate maximum
// value.
// Old spaces will count against the ideal size.
//
void Heap::SetIdealFootprint(size_t max_allowed_footprint)
{
  if (max_allowed_footprint > Heap::maximum_size_) {
    LOG(INFO) << "Clamp target GC heap from " << max_allowed_footprint
              << " to " << Heap::maximum_size_;
    max_allowed_footprint = Heap::maximum_size_;
  }

  alloc_space_->SetMaxAllowedFootprint(max_allowed_footprint);
}

// kHeapIdealFree is the ideal maximum free size, when we grow the heap for
// utlization.
static const size_t kHeapIdealFree = 2 * MB;
// kHeapMinFree guarantees that you always have at least 512 KB free, when
// you grow for utilization, regardless of target utilization ratio.
static const size_t kHeapMinFree = kHeapIdealFree / 4;

// Given the current contents of the active space, increase the allowed
// heap footprint to match the target utilization ratio.  This should
// only be called immediately after a full garbage collection.
//
void Heap::GrowForUtilization() {
  lock_->AssertHeld();

  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more
  // when necessary.
  size_t target_size = size_t( num_bytes_allocated_ /
                               Heap::GetTargetHeapUtilization() );

  if (target_size > num_bytes_allocated_ + kHeapIdealFree) {
    target_size = num_bytes_allocated_ + kHeapIdealFree;
  } else if (target_size < num_bytes_allocated_ + kHeapMinFree) {
    target_size = num_bytes_allocated_ + kHeapMinFree;
  }

  SetIdealFootprint(target_size);
}

void Heap::Lock() {
  // TODO: grab the lock, but put ourselves into Thread::kVmWait if it looks like
  // we're going to have to wait on the mutex.
  lock_->Lock();
}

void Heap::Unlock() {
  lock_->Unlock();
}

void Heap::SetWellKnownClasses(Class* java_lang_ref_FinalizerReference,
    Class* java_lang_ref_ReferenceQueue) {
  java_lang_ref_FinalizerReference_ = java_lang_ref_FinalizerReference;
  java_lang_ref_ReferenceQueue_ = java_lang_ref_ReferenceQueue;
  CHECK(java_lang_ref_FinalizerReference_ != NULL);
  CHECK(java_lang_ref_ReferenceQueue_ != NULL);
}

void Heap::SetReferenceOffsets(MemberOffset reference_referent_offset,
    MemberOffset reference_queue_offset,
    MemberOffset reference_queueNext_offset,
    MemberOffset reference_pendingNext_offset,
    MemberOffset finalizer_reference_zombie_offset) {
  reference_referent_offset_ = reference_referent_offset;
  reference_queue_offset_ = reference_queue_offset;
  reference_queueNext_offset_ = reference_queueNext_offset;
  reference_pendingNext_offset_ = reference_pendingNext_offset;
  finalizer_reference_zombie_offset_ = finalizer_reference_zombie_offset;
  CHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queue_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queueNext_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
  CHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
}

Object* Heap::GetReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  return reference->GetFieldObject<Object*>(reference_referent_offset_, true);
}

void Heap::ClearReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  reference->SetFieldObject(reference_referent_offset_, NULL, true);
}

// Returns true if the reference object has not yet been enqueued.
bool Heap::IsEnqueuable(const Object* ref) {
  DCHECK(ref != NULL);
  const Object* queue = ref->GetFieldObject<Object*>(reference_queue_offset_, false);
  const Object* queue_next = ref->GetFieldObject<Object*>(reference_queueNext_offset_, false);
  return (queue != NULL) && (queue_next == NULL);
}

void Heap::EnqueueReference(Object* ref, Object** cleared_reference_list) {
  DCHECK(ref != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queue_offset_, false) != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queueNext_offset_, false) == NULL);
  EnqueuePendingReference(ref, cleared_reference_list);
}

void Heap::EnqueuePendingReference(Object* ref, Object** list) {
  DCHECK(ref != NULL);
  DCHECK(list != NULL);

  if (*list == NULL) {
    ref->SetFieldObject(reference_pendingNext_offset_, ref, false);
    *list = ref;
  } else {
    Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    ref->SetFieldObject(reference_pendingNext_offset_, head, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, ref, false);
  }
}

Object* Heap::DequeuePendingReference(Object** list) {
  DCHECK(list != NULL);
  DCHECK(*list != NULL);
  Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
  Object* ref;
  if (*list == head) {
    ref = *list;
    *list = NULL;
  } else {
    Object* next = head->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, next, false);
    ref = head;
  }
  ref->SetFieldObject(reference_pendingNext_offset_, NULL, false);
  return ref;
}

void Heap::AddFinalizerReference(Object* object) {
  static Method* FinalizerReference_add =
      java_lang_ref_FinalizerReference_->FindDirectMethod("add", "(Ljava/lang/Object;)V");
  DCHECK(FinalizerReference_add != NULL);
  Object* args[] = { object };
  FinalizerReference_add->Invoke(Thread::Current(), NULL, reinterpret_cast<byte*>(&args), NULL);
}

void Heap::EnqueueClearedReferences(Object** cleared) {
  DCHECK(cleared != NULL);
  if (*cleared != NULL) {
    static Method* ReferenceQueue_add =
        java_lang_ref_ReferenceQueue_->FindDirectMethod("add", "(Ljava/lang/ref/Reference;)V");
    DCHECK(ReferenceQueue_add != NULL);

    Thread* self = Thread::Current();
    ScopedThreadStateChange tsc(self, Thread::kRunnable);
    Object* args[] = { *cleared };
    ReferenceQueue_add->Invoke(self, NULL, reinterpret_cast<byte*>(&args), NULL);
    *cleared = NULL;
  }
}

}  // namespace art
