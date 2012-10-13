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

#include "heap.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <limits>
#include <vector>

#include "debugger.h"
#include "gc/atomic_stack.h"
#include "gc/card_table.h"
#include "gc/heap_bitmap.h"
#include "gc/large_object_space.h"
#include "gc/mark_sweep.h"
#include "gc/mod_union_table.h"
#include "gc/space.h"
#include "image.h"
#include "object.h"
#include "object_utils.h"
#include "os.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sirt_ref.h"
#include "stl_util.h"
#include "thread_list.h"
#include "timing_logger.h"
#include "UniquePtr.h"
#include "well_known_classes.h"

namespace art {

const double Heap::kDefaultTargetUtilization = 0.5;

static bool GenerateImage(const std::string& image_file_name) {
  const std::string boot_class_path_string(Runtime::Current()->GetBootClassPathString());
  std::vector<std::string> boot_class_path;
  Split(boot_class_path_string, ':', boot_class_path);
  if (boot_class_path.empty()) {
    LOG(FATAL) << "Failed to generate image because no boot class path specified";
  }

  std::vector<char*> arg_vector;

  std::string dex2oat_string(GetAndroidRoot());
  dex2oat_string += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  const char* dex2oat = dex2oat_string.c_str();
  arg_vector.push_back(strdup(dex2oat));

  std::string image_option_string("--image=");
  image_option_string += image_file_name;
  const char* image_option = image_option_string.c_str();
  arg_vector.push_back(strdup(image_option));

  arg_vector.push_back(strdup("--runtime-arg"));
  arg_vector.push_back(strdup("-Xms64m"));

  arg_vector.push_back(strdup("--runtime-arg"));
  arg_vector.push_back(strdup("-Xmx64m"));

  for (size_t i = 0; i < boot_class_path.size(); i++) {
    std::string dex_file_option_string("--dex-file=");
    dex_file_option_string += boot_class_path[i];
    const char* dex_file_option = dex_file_option_string.c_str();
    arg_vector.push_back(strdup(dex_file_option));
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += image_file_name;
  oat_file_option_string.erase(oat_file_option_string.size() - 3);
  oat_file_option_string += "oat";
  const char* oat_file_option = oat_file_option_string.c_str();
  arg_vector.push_back(strdup(oat_file_option));

  arg_vector.push_back(strdup("--base=0x60000000"));

  std::string command_line(Join(arg_vector, ' '));
  LOG(INFO) << command_line;

  arg_vector.push_back(NULL);
  char** argv = &arg_vector[0];

  // fork and exec dex2oat
  pid_t pid = fork();
  if (pid == 0) {
    // no allocation allowed between fork and exec

    // change process groups, so we don't get reaped by ProcessManager
    setpgid(0, 0);

    execv(dex2oat, argv);

    PLOG(FATAL) << "execv(" << dex2oat << ") failed";
    return false;
  } else {
    STLDeleteElements(&arg_vector);

    // wait for dex2oat to finish
    int status;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << dex2oat << " failed: " << command_line;
      return false;
    }
  }
  return true;
}

void Heap::UnReserveOatFileAddressRange() {
  oat_file_map_.reset(NULL);
}

Heap::Heap(size_t initial_size, size_t growth_limit, size_t min_free, size_t max_free,
           double target_utilization, size_t capacity,
           const std::string& original_image_file_name, bool concurrent_gc)
    : alloc_space_(NULL),
      card_table_(NULL),
      concurrent_gc_(concurrent_gc),
      have_zygote_space_(false),
      card_marking_disabled_(false),
      is_gc_running_(false),
      last_gc_type_(kGcTypeNone),
      enforce_heap_growth_rate_(false),
      growth_limit_(growth_limit),
      max_allowed_footprint_(initial_size),
      concurrent_start_size_(128 * KB),
      concurrent_min_free_(256 * KB),
      concurrent_start_bytes_(initial_size - concurrent_start_size_),
      sticky_gc_count_(0),
      total_bytes_freed_(0),
      total_objects_freed_(0),
      large_object_threshold_(3 * kPageSize),
      num_bytes_allocated_(0),
      verify_missing_card_marks_(false),
      verify_system_weaks_(false),
      verify_pre_gc_heap_(false),
      verify_post_gc_heap_(false),
      verify_mod_union_table_(false),
      partial_gc_frequency_(10),
      min_alloc_space_size_for_sticky_gc_(2 * MB),
      min_remaining_space_for_sticky_gc_(1 * MB),
      last_trim_time_(0),
      requesting_gc_(false),
      max_allocation_stack_size_(MB),
      reference_referent_offset_(0),
      reference_queue_offset_(0),
      reference_queueNext_offset_(0),
      reference_pendingNext_offset_(0),
      finalizer_reference_zombie_offset_(0),
      min_free_(min_free),
      max_free_(max_free),
      target_utilization_(target_utilization),
      total_paused_time_(0),
      total_wait_time_(0),
      measure_allocation_time_(false),
      total_allocation_time_(0),
      verify_objects_(false) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }

  live_bitmap_.reset(new HeapBitmap(this));
  mark_bitmap_.reset(new HeapBitmap(this));

  // Requested begin for the alloc space, to follow the mapped image and oat files
  byte* requested_begin = NULL;
  std::string image_file_name(original_image_file_name);
  if (!image_file_name.empty()) {
    ImageSpace* image_space = NULL;

    if (OS::FileExists(image_file_name.c_str())) {
      // If the /system file exists, it should be up-to-date, don't try to generate
      image_space = ImageSpace::Create(image_file_name);
    } else {
      // If the /system file didn't exist, we need to use one from the art-cache.
      // If the cache file exists, try to open, but if it fails, regenerate.
      // If it does not exist, generate.
      image_file_name = GetArtCacheFilenameOrDie(image_file_name);
      if (OS::FileExists(image_file_name.c_str())) {
        image_space = ImageSpace::Create(image_file_name);
      }
      if (image_space == NULL) {
        CHECK(GenerateImage(image_file_name)) << "Failed to generate image: " << image_file_name;
        image_space = ImageSpace::Create(image_file_name);
      }
    }

    CHECK(image_space != NULL) << "Failed to create space from " << image_file_name;
    AddSpace(image_space);
    // Oat files referenced by image files immediately follow them in memory, ensure alloc space
    // isn't going to get in the middle
    byte* oat_end_addr = image_space->GetImageHeader().GetOatEnd();
    CHECK_GT(oat_end_addr, image_space->End());

    // Reserve address range from image_space->End() to image_space->GetImageHeader().GetOatEnd()
    uintptr_t reserve_begin = RoundUp(reinterpret_cast<uintptr_t>(image_space->End()), kPageSize);
    uintptr_t reserve_end = RoundUp(reinterpret_cast<uintptr_t>(oat_end_addr), kPageSize);
    oat_file_map_.reset(MemMap::MapAnonymous("oat file reserve",
                                             reinterpret_cast<byte*>(reserve_begin),
                                             reserve_end - reserve_begin, PROT_READ));

    if (oat_end_addr > requested_begin) {
      requested_begin = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(oat_end_addr),
                                                          kPageSize));
    }
  }

  // Allocate the large object space.
  large_object_space_.reset(FreeListSpace::Create("large object space", NULL, capacity));
  live_bitmap_->SetLargeObjects(large_object_space_->GetLiveObjects());
  mark_bitmap_->SetLargeObjects(large_object_space_->GetMarkObjects());

  UniquePtr<DlMallocSpace> alloc_space(DlMallocSpace::Create("alloc space", initial_size,
                                                             growth_limit, capacity,
                                                             requested_begin));
  alloc_space_ = alloc_space.release();
  alloc_space_->SetFootprintLimit(alloc_space_->Capacity());
  CHECK(alloc_space_ != NULL) << "Failed to create alloc space";
  AddSpace(alloc_space_);

  // Spaces are sorted in order of Begin().
  byte* heap_begin = spaces_.front()->Begin();
  size_t heap_capacity = spaces_.back()->End() - spaces_.front()->Begin();
  if (spaces_.back()->IsAllocSpace()) {
    heap_capacity += spaces_.back()->AsAllocSpace()->NonGrowthLimitCapacity();
  }

  // Mark image objects in the live bitmap
  // TODO: C++0x
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsImageSpace()) {
      ImageSpace* image_space = space->AsImageSpace();
      image_space->RecordImageAllocations(image_space->GetLiveBitmap());
    }
  }

  // Allocate the card table.
  card_table_.reset(CardTable::Create(heap_begin, heap_capacity));
  CHECK(card_table_.get() != NULL) << "Failed to create card table";

  mod_union_table_.reset(new ModUnionTableToZygoteAllocspace<ModUnionTableReferenceCache>(this));
  CHECK(mod_union_table_.get() != NULL) << "Failed to create mod-union table";

  zygote_mod_union_table_.reset(new ModUnionTableCardCache(this));
  CHECK(zygote_mod_union_table_.get() != NULL) << "Failed to create Zygote mod-union table";

  // TODO: Count objects in the image space here.
  num_bytes_allocated_ = 0;

  // Max stack size in bytes.
  static const size_t default_mark_stack_size = 64 * KB;
  mark_stack_.reset(ObjectStack::Create("dalvik-mark-stack", default_mark_stack_size));
  allocation_stack_.reset(ObjectStack::Create("dalvik-allocation-stack",
                                              max_allocation_stack_size_));
  live_stack_.reset(ObjectStack::Create("dalvik-live-stack",
                                      max_allocation_stack_size_));

  // It's still too early to take a lock because there are no threads yet,
  // but we can create the heap lock now. We don't create it earlier to
  // make it clear that you can't use locks during heap initialization.
  gc_complete_lock_ = new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable"));

  // Set up the cumulative timing loggers.
  for (size_t i = static_cast<size_t>(kGcTypeSticky); i < static_cast<size_t>(kGcTypeMax);
       ++i) {
    std::ostringstream name;
    name << static_cast<GcType>(i);
    cumulative_timings_.Put(static_cast<GcType>(i),
                            new CumulativeLogger(name.str().c_str(), true));
  }

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}

// Sort spaces based on begin address
struct SpaceSorter {
  bool operator ()(const ContinuousSpace* a, const ContinuousSpace* b) const {
    return a->Begin() < b->Begin();
  }
};

void Heap::AddSpace(ContinuousSpace* space) {
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  DCHECK(space != NULL);
  DCHECK(space->GetLiveBitmap() != NULL);
  live_bitmap_->AddSpaceBitmap(space->GetLiveBitmap());
  DCHECK(space->GetMarkBitmap() != NULL);
  mark_bitmap_->AddSpaceBitmap(space->GetMarkBitmap());
  spaces_.push_back(space);
  if (space->IsAllocSpace()) {
    alloc_space_ = space->AsAllocSpace();
  }

  // Ensure that spaces remain sorted in increasing order of start address (required for CMS finger)
  std::sort(spaces_.begin(), spaces_.end(), SpaceSorter());

  // Ensure that ImageSpaces < ZygoteSpaces < AllocSpaces so that we can do address based checks to
  // avoid redundant marking.
  bool seen_zygote = false, seen_alloc = false;
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsImageSpace()) {
      DCHECK(!seen_zygote);
      DCHECK(!seen_alloc);
    } else if (space->IsZygoteSpace()) {
      DCHECK(!seen_alloc);
      seen_zygote = true;
    } else if (space->IsAllocSpace()) {
      seen_alloc = true;
    }
  }
}

void Heap::DumpGcPerformanceInfo() {
  // Dump cumulative timings.
  LOG(INFO) << "Dumping cumulative Gc timings";
  uint64_t total_duration = 0;
  for (CumulativeTimings::iterator it = cumulative_timings_.begin();
      it != cumulative_timings_.end(); ++it) {
    CumulativeLogger* logger = it->second;
    if (logger->GetTotalNs() != 0) {
      logger->Dump();
      total_duration += logger->GetTotalNs();
    }
  }
  uint64_t allocation_time = static_cast<uint64_t>(total_allocation_time_) * kTimeAdjust;
  size_t total_objects_allocated = GetTotalObjectsAllocated();
  size_t total_bytes_allocated = GetTotalBytesAllocated();
  if (total_duration != 0) {
    const double total_seconds = double(total_duration / 1000) / 1000000.0;
    LOG(INFO) << "Total time spent in GC: " << PrettyDuration(total_duration);
    LOG(INFO) << "Mean GC size throughput: "
              << PrettySize(GetTotalBytesFreed() / total_seconds) << "/s";
    LOG(INFO) << "Mean GC object throughput: " << GetTotalObjectsFreed() / total_seconds << "/s";
  }
  LOG(INFO) << "Total number of allocations: " << total_objects_allocated;
  LOG(INFO) << "Total bytes allocated " << PrettySize(total_bytes_allocated);
  if (measure_allocation_time_) {
    LOG(INFO) << "Total time spent allocating: " << PrettyDuration(allocation_time);
    LOG(INFO) << "Mean allocation time: "
              << PrettyDuration(allocation_time / total_objects_allocated);
  }
  LOG(INFO) << "Total mutator paused time: " << PrettyDuration(total_paused_time_);
  LOG(INFO) << "Total waiting for Gc to complete time: " << PrettyDuration(total_wait_time_);
}

Heap::~Heap() {
  // If we don't reset then the mark stack complains in it's destructor.
  allocation_stack_->Reset();
  live_stack_->Reset();

  VLOG(heap) << "~Heap()";
  // We can't take the heap lock here because there might be a daemon thread suspended with the
  // heap lock held. We know though that no non-daemon threads are executing, and we know that
  // all daemon threads are suspended, and we also know that the threads list have been deleted, so
  // those threads can't resume. We're the only running thread, and we can do whatever we like...
  STLDeleteElements(&spaces_);
  delete gc_complete_lock_;
  STLDeleteValues(&cumulative_timings_);
}

ContinuousSpace* Heap::FindSpaceFromObject(const Object* obj) const {
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->Contains(obj)) {
      return *it;
    }
  }
  LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  return NULL;
}

ImageSpace* Heap::GetImageSpace() {
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->IsImageSpace()) {
      return (*it)->AsImageSpace();
    }
  }
  return NULL;
}

DlMallocSpace* Heap::GetAllocSpace() {
  return alloc_space_;
}

static void MSpaceChunkCallback(void* start, void* end, size_t used_bytes, void* arg) {
  size_t chunk_size = reinterpret_cast<uint8_t*>(end) - reinterpret_cast<uint8_t*>(start);
  if (used_bytes < chunk_size) {
    size_t chunk_free_bytes = chunk_size - used_bytes;
    size_t& max_contiguous_allocation = *reinterpret_cast<size_t*>(arg);
    max_contiguous_allocation = std::max(max_contiguous_allocation, chunk_free_bytes);
  }
}

Object* Heap::AllocObject(Thread* self, Class* c, size_t byte_count) {
  DCHECK(c == NULL || (c->IsClassClass() && byte_count >= sizeof(Class)) ||
         (c->IsVariableSize() || c->GetObjectSize() == byte_count) ||
         strlen(ClassHelper(c).GetDescriptor()) == 0);
  DCHECK_GE(byte_count, sizeof(Object));

  Object* obj = NULL;
  size_t size = 0;
  uint64_t allocation_start = 0;
  if (measure_allocation_time_) {
    allocation_start = NanoTime();
  }

  // We need to have a zygote space or else our newly allocated large object can end up in the
  // Zygote resulting in it being prematurely freed.
  // We can only do this for primive objects since large objects will not be within the card table
  // range. This also means that we rely on SetClass not dirtying the object's card.
  if (byte_count >= large_object_threshold_ && have_zygote_space_ && c->IsPrimitiveArray()) {
    size = RoundUp(byte_count, kPageSize);
    obj = Allocate(self, large_object_space_.get(), size);
    // Make sure that our large object didn't get placed anywhere within the space interval or else
    // it breaks the immune range.
    DCHECK(obj == NULL ||
           reinterpret_cast<byte*>(obj) < spaces_.front()->Begin() ||
           reinterpret_cast<byte*>(obj) >= spaces_.back()->End());
  } else {
    obj = Allocate(self, alloc_space_, byte_count);

    // Ensure that we did not allocate into a zygote space.
    DCHECK(obj == NULL || !have_zygote_space_ || !FindSpaceFromObject(obj)->IsZygoteSpace());
    size = alloc_space_->AllocationSize(obj);
  }

  if (LIKELY(obj != NULL)) {
    obj->SetClass(c);

    // Record allocation after since we want to use the atomic add for the atomic fence to guard
    // the SetClass since we do not want the class to appear NULL in another thread.
    RecordAllocation(size, obj);

    if (Dbg::IsAllocTrackingEnabled()) {
      Dbg::RecordAllocation(c, byte_count);
    }
    if (static_cast<size_t>(num_bytes_allocated_) >= concurrent_start_bytes_) {
      // We already have a request pending, no reason to start more until we update
      // concurrent_start_bytes_.
      concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
      // The SirtRef is necessary since the calls in RequestConcurrentGC are a safepoint.
      SirtRef<Object> ref(self, obj);
      RequestConcurrentGC(self);
    }
    VerifyObject(obj);

    if (measure_allocation_time_) {
      total_allocation_time_ += (NanoTime() - allocation_start) / kTimeAdjust;
    }

    return obj;
  }
  int64_t total_bytes_free = GetFreeMemory();
  size_t max_contiguous_allocation = 0;
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->IsAllocSpace()) {
      (*it)->AsAllocSpace()->Walk(MSpaceChunkCallback, &max_contiguous_allocation);
    }
  }

  std::string msg(StringPrintf("Failed to allocate a %zd-byte %s (%lld total bytes free; largest possible contiguous allocation %zd bytes)",
                               byte_count, PrettyDescriptor(c).c_str(), total_bytes_free, max_contiguous_allocation));
  self->ThrowOutOfMemoryError(msg.c_str());
  return NULL;
}

bool Heap::IsHeapAddress(const Object* obj) {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would
  // require taking the lock.
  if (obj == NULL) {
    return true;
  }
  if (!IsAligned<kObjectAlignment>(obj)) {
    return false;
  }
  for (size_t i = 0; i < spaces_.size(); ++i) {
    if (spaces_[i]->Contains(obj)) {
      return true;
    }
  }
  // TODO: Find a way to check large object space without using a lock.
  return true;
}

bool Heap::IsLiveObjectLocked(const Object* obj) {
  Locks::heap_bitmap_lock_->AssertReaderHeld(Thread::Current());
  return IsHeapAddress(obj) && GetLiveBitmap()->Test(obj);
}

#if VERIFY_OBJECT_ENABLED
void Heap::VerifyObject(const Object* obj) {
  if (obj == NULL || this == NULL || !verify_objects_ || Runtime::Current()->IsShuttingDown() ||
      Thread::Current() == NULL ||
      Runtime::Current()->GetThreadList()->GetLockOwner() == Thread::Current()->GetTid()) {
    return;
  }
  VerifyObjectBody(obj);
}
#endif

void Heap::DumpSpaces() {
  // TODO: C++0x auto
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    ContinuousSpace* space = *it;
    SpaceBitmap* live_bitmap = space->GetLiveBitmap();
    SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    LOG(INFO) << space << " " << *space << "\n"
              << live_bitmap << " " << *live_bitmap << "\n"
              << mark_bitmap << " " << *mark_bitmap;
  }
  // TODO: Dump large object space?
}

void Heap::VerifyObjectBody(const Object* obj) {
  if (!IsAligned<kObjectAlignment>(obj)) {
    LOG(FATAL) << "Object isn't aligned: " << obj;
  }

  // TODO: the bitmap tests below are racy if VerifyObjectBody is called without the
  //       heap_bitmap_lock_.
  if (!GetLiveBitmap()->Test(obj)) {
    // Check the allocation stack / live stack.
    if (!std::binary_search(live_stack_->Begin(), live_stack_->End(), obj) &&
        std::find(allocation_stack_->Begin(), allocation_stack_->End(), obj) ==
            allocation_stack_->End()) {
      if (large_object_space_->GetLiveObjects()->Test(obj)) {
        DumpSpaces();
        LOG(FATAL) << "Object is dead: " << obj;
      }
    }
  }

  // Ignore early dawn of the universe verifications
  if (!VERIFY_OBJECT_FAST && GetObjectsAllocated() > 10) {
    const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
        Object::ClassOffset().Int32Value();
    const Class* c = *reinterpret_cast<Class* const *>(raw_addr);
    if (c == NULL) {
      LOG(FATAL) << "Null class in object: " << obj;
    } else if (!IsAligned<kObjectAlignment>(c)) {
      LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
    } else if (!GetLiveBitmap()->Test(c)) {
      LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
    }
    // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
    // Note: we don't use the accessors here as they have internal sanity checks
    // that we don't want to run
    raw_addr = reinterpret_cast<const byte*>(c) + Object::ClassOffset().Int32Value();
    const Class* c_c = *reinterpret_cast<Class* const *>(raw_addr);
    raw_addr = reinterpret_cast<const byte*>(c_c) + Object::ClassOffset().Int32Value();
    const Class* c_c_c = *reinterpret_cast<Class* const *>(raw_addr);
    CHECK_EQ(c_c, c_c_c);
  }
}

void Heap::VerificationCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  reinterpret_cast<Heap*>(arg)->VerifyObjectBody(obj);
}

void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Walk(Heap::VerificationCallback, this);
}

void Heap::RecordAllocation(size_t size, Object* obj) {
  DCHECK(obj != NULL);
  DCHECK_GT(size, 0u);
  num_bytes_allocated_ += size;

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    ++thread_stats->allocated_objects;
    thread_stats->allocated_bytes += size;

    // TODO: Update these atomically.
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    ++global_stats->allocated_objects;
    global_stats->allocated_bytes += size;
  }

  // This is safe to do since the GC will never free objects which are neither in the allocation
  // stack or the live bitmap.
  while (!allocation_stack_->AtomicPushBack(obj)) {
    Thread* self = Thread::Current();
    self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
    // If we actually ran a different type of Gc than requested, we can skip the index forwards.
    CollectGarbageInternal(kGcTypeSticky, kGcCauseForAlloc, false);
    self->TransitionFromSuspendedToRunnable();
  }
}

void Heap::RecordFree(size_t freed_objects, size_t freed_bytes) {
  DCHECK_LE(freed_bytes, static_cast<size_t>(num_bytes_allocated_));
  num_bytes_allocated_ -= freed_bytes;

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    thread_stats->freed_objects += freed_objects;
    thread_stats->freed_bytes += freed_bytes;

    // TODO: Do this concurrently.
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    global_stats->freed_objects += freed_objects;
    global_stats->freed_bytes += freed_bytes;
  }
}

Object* Heap::TryToAllocate(Thread* self, AllocSpace* space, size_t alloc_size, bool grow) {
  // Should we try to use a CAS here and fix up num_bytes_allocated_ later with AllocationSize?
  if (enforce_heap_growth_rate_ && num_bytes_allocated_ + alloc_size > max_allowed_footprint_) {
    if (grow) {
      // Grow the heap by alloc_size extra bytes.
      max_allowed_footprint_ = std::min(max_allowed_footprint_ + alloc_size, growth_limit_);
      VLOG(gc) << "Grow heap to " << PrettySize(max_allowed_footprint_)
               << " for a " << PrettySize(alloc_size) << " allocation";
    } else {
      return NULL;
    }
  }

  if (num_bytes_allocated_ + alloc_size > growth_limit_) {
    // Completely out of memory.
    return NULL;
  }

  return space->Alloc(self, alloc_size);
}

Object* Heap::Allocate(Thread* self, AllocSpace* space, size_t alloc_size) {
  // Since allocation can cause a GC which will need to SuspendAll, make sure all allocations are
  // done in the runnable state where suspension is expected.
  DCHECK_EQ(self->GetState(), kRunnable);
  self->AssertThreadSuspensionIsAllowable();

  Object* ptr = TryToAllocate(self, space, alloc_size, false);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed. If the GC is running, block until it completes, and then retry the
  // allocation.
  GcType last_gc = WaitForConcurrentGcToComplete(self);
  if (last_gc != kGcTypeNone) {
    // A GC was in progress and we blocked, retry allocation now that memory has been freed.
    ptr = TryToAllocate(self, space, alloc_size, false);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Loop through our different Gc types and try to Gc until we get enough free memory.
  for (size_t i = static_cast<size_t>(last_gc) + 1; i < static_cast<size_t>(kGcTypeMax); ++i) {
    bool run_gc = false;
    GcType gc_type = static_cast<GcType>(i);
    switch (gc_type) {
      case kGcTypeSticky: {
          const size_t alloc_space_size = alloc_space_->Size();
          run_gc = alloc_space_size > min_alloc_space_size_for_sticky_gc_ &&
              alloc_space_->Capacity() - alloc_space_size >= min_remaining_space_for_sticky_gc_;
          break;
        }
      case kGcTypePartial:
        run_gc = have_zygote_space_;
        break;
      case kGcTypeFull:
        run_gc = true;
        break;
      default:
        break;
    }

    if (run_gc) {
      self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);

      // If we actually ran a different type of Gc than requested, we can skip the index forwards.
      GcType gc_type_ran = CollectGarbageInternal(gc_type, kGcCauseForAlloc, false);
      DCHECK(static_cast<size_t>(gc_type_ran) >= i);
      i = static_cast<size_t>(gc_type_ran);
      self->TransitionFromSuspendedToRunnable();

      // Did we free sufficient memory for the allocation to succeed?
      ptr = TryToAllocate(self, space, alloc_size, false);
      if (ptr != NULL) {
        return ptr;
      }
    }
  }

  // Allocations have failed after GCs;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = TryToAllocate(self, space, alloc_size, true);
  if (ptr != NULL) {
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is really full, really fragmented,
  // or the requested size is really big. Do another GC, collecting SoftReferences this time. The
  // VM spec requires that all SoftReferences have been collected and cleared before throwing OOME.

  // OLD-TODO: wait for the finalizers from the previous GC to finish
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size)
           << " allocation";

  // We don't need a WaitForConcurrentGcToComplete here either.
  self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
  CollectGarbageInternal(kGcTypeFull, kGcCauseForAlloc, true);
  self->TransitionFromSuspendedToRunnable();
  return TryToAllocate(self, space, alloc_size, true);
}

void Heap::SetTargetHeapUtilization(float target) {
  DCHECK_GT(target, 0.0f);  // asserted in Java code
  DCHECK_LT(target, 1.0f);
  target_utilization_ = target;
}

int64_t Heap::GetMaxMemory() const {
  return growth_limit_;
}

int64_t Heap::GetTotalMemory() const {
  return GetMaxMemory();
}

int64_t Heap::GetFreeMemory() const {
  return GetMaxMemory() - num_bytes_allocated_;
}

size_t Heap::GetTotalBytesFreed() const {
  return total_bytes_freed_;
}

size_t Heap::GetTotalObjectsFreed() const {
  return total_objects_freed_;
}

size_t Heap::GetTotalObjectsAllocated() const {
  size_t total = large_object_space_->GetTotalObjectsAllocated();
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsAllocSpace()) {
      total += space->AsAllocSpace()->GetTotalObjectsAllocated();
    }
  }
  return total;
}

size_t Heap::GetTotalBytesAllocated() const {
  size_t total = large_object_space_->GetTotalBytesAllocated();
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsAllocSpace()) {
      total += space->AsAllocSpace()->GetTotalBytesAllocated();
    }
  }
  return total;
}

class InstanceCounter {
 public:
  InstanceCounter(Class* c, bool count_assignable, size_t* const count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : class_(c), count_assignable_(count_assignable), count_(count) {

  }

  void operator()(const Object* o) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const Class* instance_class = o->GetClass();
    if (count_assignable_) {
      if (instance_class == class_) {
        ++*count_;
      }
    } else {
      if (instance_class != NULL && class_->IsAssignableFrom(instance_class)) {
        ++*count_;
      }
    }
  }

 private:
  Class* class_;
  bool count_assignable_;
  size_t* const count_;
};

int64_t Heap::CountInstances(Class* c, bool count_assignable) {
  size_t count = 0;
  InstanceCounter counter(c, count_assignable, &count);
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Visit(counter);
  return count;
}

void Heap::CollectGarbage(bool clear_soft_references) {
  // Even if we waited for a GC we still need to do another GC since weaks allocated during the
  // last GC will not have necessarily been cleared.
  Thread* self = Thread::Current();
  WaitForConcurrentGcToComplete(self);
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  // CollectGarbageInternal(have_zygote_space_ ? kGcTypePartial : kGcTypeFull, clear_soft_references);
  CollectGarbageInternal(kGcTypeFull, kGcCauseExplicit, clear_soft_references);
}

void Heap::PreZygoteFork() {
  static Mutex zygote_creation_lock_("zygote creation lock", kZygoteCreationLock);
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);

  // Try to see if we have any Zygote spaces.
  if (have_zygote_space_) {
    return;
  }

  VLOG(heap) << "Starting PreZygoteFork with alloc space size " << PrettySize(alloc_space_->Size());

  {
    // Flush the alloc stack.
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    FlushAllocStack();
  }

  // Replace the first alloc space we find with a zygote space.
  // TODO: C++0x auto
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->IsAllocSpace()) {
      DlMallocSpace* zygote_space = (*it)->AsAllocSpace();

      // Turns the current alloc space into a Zygote space and obtain the new alloc space composed
      // of the remaining available heap memory.
      alloc_space_ = zygote_space->CreateZygoteSpace();
      alloc_space_->SetFootprintLimit(alloc_space_->Capacity());

      // Change the GC retention policy of the zygote space to only collect when full.
      zygote_space->SetGcRetentionPolicy(kGcRetentionPolicyFullCollect);
      AddSpace(alloc_space_);
      have_zygote_space_ = true;
      break;
    }
  }

  // Reset the cumulative loggers since we now have a few additional timing phases.
  // TODO: C++0x
  for (CumulativeTimings::iterator it = cumulative_timings_.begin();
       it != cumulative_timings_.end(); ++it) {
    it->second->Reset();
  }
}

void Heap::FlushAllocStack() {
  MarkAllocStack(alloc_space_->GetLiveBitmap(), large_object_space_->GetLiveObjects(),
                 allocation_stack_.get());
  allocation_stack_->Reset();
}

size_t Heap::GetUsedMemorySize() const {
  return num_bytes_allocated_;
}

void Heap::MarkAllocStack(SpaceBitmap* bitmap, SpaceSetMap* large_objects, ObjectStack* stack) {
  Object** limit = stack->End();
  for (Object** it = stack->Begin(); it != limit; ++it) {
    const Object* obj = *it;
    DCHECK(obj != NULL);
    if (LIKELY(bitmap->HasAddress(obj))) {
      bitmap->Set(obj);
    } else {
      large_objects->Set(obj);
    }
  }
}

void Heap::UnMarkAllocStack(SpaceBitmap* bitmap, SpaceSetMap* large_objects, ObjectStack* stack) {
  Object** limit = stack->End();
  for (Object** it = stack->Begin(); it != limit; ++it) {
    const Object* obj = *it;
    DCHECK(obj != NULL);
    if (LIKELY(bitmap->HasAddress(obj))) {
      bitmap->Clear(obj);
    } else {
      large_objects->Clear(obj);
    }
  }
}

GcType Heap::CollectGarbageInternal(GcType gc_type, GcCause gc_cause, bool clear_soft_references) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  DCHECK_EQ(self->GetState(), kWaitingPerformingGc);

  if (self->IsHandlingStackOverflow()) {
    LOG(WARNING) << "Performing GC on a thread that is handling a stack overflow.";
  }

  // Ensure there is only one GC at a time.
  bool start_collect = false;
  while (!start_collect) {
    {
      MutexLock mu(self, *gc_complete_lock_);
      if (!is_gc_running_) {
        is_gc_running_ = true;
        start_collect = true;
      }
    }
    if (!start_collect) {
      WaitForConcurrentGcToComplete(self);
      // TODO: if another thread beat this one to do the GC, perhaps we should just return here?
      //       Not doing at the moment to ensure soft references are cleared.
    }
  }
  gc_complete_lock_->AssertNotHeld(self);

  if (gc_cause == kGcCauseForAlloc && Runtime::Current()->HasStatsEnabled()) {
    ++Runtime::Current()->GetStats()->gc_for_alloc_count;
    ++Thread::Current()->GetStats()->gc_for_alloc_count;
  }

  // We need to do partial GCs every now and then to avoid the heap growing too much and
  // fragmenting.
  if (gc_type == kGcTypeSticky && ++sticky_gc_count_ > partial_gc_frequency_) {
    gc_type = kGcTypePartial;
  }
  if (gc_type != kGcTypeSticky) {
    sticky_gc_count_ = 0;
  }

  if (concurrent_gc_) {
    CollectGarbageConcurrentMarkSweepPlan(self, gc_type, gc_cause, clear_soft_references);
  } else {
    CollectGarbageMarkSweepPlan(self, gc_type, gc_cause, clear_soft_references);
  }
  bytes_since_last_gc_ = 0;

  {
    MutexLock mu(self, *gc_complete_lock_);
    is_gc_running_ = false;
    last_gc_type_ = gc_type;
    // Wake anyone who may have been waiting for the GC to complete.
    gc_complete_cond_->Broadcast();
  }
  // Inform DDMS that a GC completed.
  Dbg::GcDidFinish();
  return gc_type;
}

void Heap::CollectGarbageMarkSweepPlan(Thread* self, GcType gc_type, GcCause gc_cause,
                                       bool clear_soft_references) {
  TimingLogger timings("CollectGarbageInternal", true);

  std::stringstream gc_type_str;
  gc_type_str << gc_type << " ";

  // Suspend all threads are get exclusive access to the heap.
  uint64_t start_time = NanoTime();
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();
  timings.AddSplit("SuspendAll");
  Locks::mutator_lock_->AssertExclusiveHeld(self);

  size_t bytes_freed = 0;
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep(mark_stack_.get());
    mark_sweep.Init();
    timings.AddSplit("Init");

    if (verify_pre_gc_heap_) {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      if (!VerifyHeapReferences()) {
        LOG(FATAL) << "Pre " << gc_type_str.str() << "Gc verification failed";
      }
      timings.AddSplit("VerifyHeapReferencesPreGC");
    }

    // Swap allocation stack and live stack, enabling us to have new allocations during this GC.
    SwapStacks();

    // We will need to know which cards were dirty for doing concurrent processing of dirty cards.
    // TODO: Investigate using a mark stack instead of a vector.
    std::vector<byte*> dirty_cards;
    if (gc_type == kGcTypeSticky) {
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        card_table_->GetDirtyCards(*it, dirty_cards);
      }
    }

    // Clear image space cards and keep track of cards we cleared in the mod-union table.
    ClearCards(timings);

    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    if (gc_type == kGcTypePartial) {
      // Copy the mark bits over from the live bits, do this as early as possible or else we can
      // accidentally un-mark roots.
      // Needed for scanning dirty objects.
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        if ((*it)->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect) {
          mark_sweep.BindLiveToMarkBitmap(*it);
        }
      }
      timings.AddSplit("BindLiveToMarked");

      // We can assume that everything from the start of the first space to the alloc space is marked.
      mark_sweep.SetImmuneRange(reinterpret_cast<Object*>(spaces_[0]->Begin()),
                                reinterpret_cast<Object*>(alloc_space_->Begin()));
    } else if (gc_type == kGcTypeSticky) {
      for (Spaces::iterator it = spaces_.begin();it != spaces_.end(); ++it) {
        if ((*it)->GetGcRetentionPolicy() != kGcRetentionPolicyNeverCollect) {
          mark_sweep.BindLiveToMarkBitmap(*it);
        }
      }
      timings.AddSplit("BindLiveToMarkBitmap");
      large_object_space_->CopyLiveToMarked();
      timings.AddSplit("CopyLiveToMarked");
      mark_sweep.SetImmuneRange(reinterpret_cast<Object*>(spaces_[0]->Begin()),
                                reinterpret_cast<Object*>(alloc_space_->Begin()));
    }
    mark_sweep.FindDefaultMarkBitmap();

    mark_sweep.MarkRoots();
    timings.AddSplit("MarkRoots");

    // Roots are marked on the bitmap and the mark_stack is empty.
    DCHECK(mark_stack_->IsEmpty());

    UpdateAndMarkModUnion(&mark_sweep, timings, gc_type);

    if (gc_type != kGcTypeSticky) {
      MarkAllocStack(alloc_space_->GetLiveBitmap(), large_object_space_->GetLiveObjects(),
                     live_stack_.get());
      timings.AddSplit("MarkStackAsLive");
    }

    if (verify_mod_union_table_) {
      zygote_mod_union_table_->Update();
      zygote_mod_union_table_->Verify();
      mod_union_table_->Update();
      mod_union_table_->Verify();
    }

    // Recursively mark all the non-image bits set in the mark bitmap.
    if (gc_type != kGcTypeSticky) {
      mark_sweep.RecursiveMark(gc_type == kGcTypePartial, timings);
    } else {
      mark_sweep.RecursiveMarkCards(card_table_.get(), dirty_cards, timings);
    }
    mark_sweep.DisableFinger();

    // Need to process references before the swap since it uses IsMarked.
    mark_sweep.ProcessReferences(clear_soft_references);
    timings.AddSplit("ProcessReferences");

#ifndef NDEBUG
    // Verify that we only reach marked objects from the image space
    mark_sweep.VerifyImageRoots();
    timings.AddSplit("VerifyImageRoots");
#endif

    if (gc_type != kGcTypeSticky) {
      mark_sweep.Sweep(gc_type == kGcTypePartial, false);
      timings.AddSplit("Sweep");
      mark_sweep.SweepLargeObjects(false);
      timings.AddSplit("SweepLargeObjects");
    } else {
      mark_sweep.SweepArray(timings, live_stack_.get(), false);
      timings.AddSplit("SweepArray");
    }
    live_stack_->Reset();

    // Unbind the live and mark bitmaps.
    mark_sweep.UnBindBitmaps();

    const bool swap = true;
    if (swap) {
      if (gc_type == kGcTypeSticky) {
        SwapLargeObjects();
      } else {
        SwapBitmaps(gc_type);
      }
    }

    if (verify_system_weaks_) {
      mark_sweep.VerifySystemWeaks();
      timings.AddSplit("VerifySystemWeaks");
    }

    cleared_references = mark_sweep.GetClearedReferences();
    bytes_freed = mark_sweep.GetFreedBytes();
    total_bytes_freed_ += bytes_freed;
    total_objects_freed_ += mark_sweep.GetFreedObjects();
  }

  if (verify_post_gc_heap_) {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    if (!VerifyHeapReferences()) {
      LOG(FATAL) << "Post " + gc_type_str.str() + "Gc verification failed";
    }
    timings.AddSplit("VerifyHeapReferencesPostGC");
  }

  GrowForUtilization();
  timings.AddSplit("GrowForUtilization");

  thread_list->ResumeAll();
  timings.AddSplit("ResumeAll");

  EnqueueClearedReferences(&cleared_references);
  RequestHeapTrim();
  timings.AddSplit("Finish");

  // If the GC was slow, then print timings in the log.
  uint64_t duration = (NanoTime() - start_time) / 1000 * 1000;
  total_paused_time_ += duration / kTimeAdjust;
  if (duration > MsToNs(50)) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetUsedMemorySize();
    const size_t total_memory = GetTotalMemory();
    LOG(INFO) << gc_cause << " " << gc_type_str.str()
              << "GC freed " << PrettySize(bytes_freed) << ", " << percent_free << "% free, "
              << PrettySize(current_heap_size) << "/" << PrettySize(total_memory) << ", "
              << "paused " << PrettyDuration(duration);
    if (VLOG_IS_ON(heap)) {
      timings.Dump();
    }
  }

  CumulativeLogger* logger = cumulative_timings_.Get(gc_type);
  logger->Start();
  logger->AddLogger(timings);
  logger->End(); // Next iteration.
}

void Heap::UpdateAndMarkModUnion(MarkSweep* mark_sweep, TimingLogger& timings, GcType gc_type) {
  if (gc_type == kGcTypeSticky) {
    // Don't need to do anything for mod union table in this case since we are only scanning dirty
    // cards.
    return;
  }

  // Update zygote mod union table.
  if (gc_type == kGcTypePartial) {
    zygote_mod_union_table_->Update();
    timings.AddSplit("UpdateZygoteModUnionTable");

    zygote_mod_union_table_->MarkReferences(mark_sweep);
    timings.AddSplit("ZygoteMarkReferences");
  }

  // Processes the cards we cleared earlier and adds their objects into the mod-union table.
  mod_union_table_->Update();
  timings.AddSplit("UpdateModUnionTable");

  // Scans all objects in the mod-union table.
  mod_union_table_->MarkReferences(mark_sweep);
  timings.AddSplit("MarkImageToAllocSpaceReferences");
}

void Heap::RootMatchesObjectVisitor(const Object* root, void* arg) {
  Object* obj = reinterpret_cast<Object*>(arg);
  if (root == obj) {
    LOG(INFO) << "Object " << obj << " is a root";
  }
}

class ScanVisitor {
 public:
  void operator ()(const Object* obj) const {
    LOG(INFO) << "Would have rescanned object " << obj;
  }
};

class VerifyReferenceVisitor {
 public:
  VerifyReferenceVisitor(Heap* heap, bool* failed)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap),
        failed_(failed) {
  }

  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for smarter
  // analysis.
  void operator ()(const Object* obj, const Object* ref, const MemberOffset& /* offset */,
                     bool /* is_static */) const NO_THREAD_SAFETY_ANALYSIS {
    // Verify that the reference is live.
    if (ref != NULL && !IsLive(ref)) {
      CardTable* card_table = heap_->GetCardTable();
      ObjectStack* alloc_stack = heap_->allocation_stack_.get();
      ObjectStack* live_stack = heap_->live_stack_.get();

      byte* card_addr = card_table->CardFromAddr(obj);
      LOG(ERROR) << "Object " << obj << " references dead object " << ref << "\n"
                 << "IsDirty = " << (*card_addr == CardTable::kCardDirty) << "\n"
                 << "Obj type " << PrettyTypeOf(obj) << "\n"
                 << "Ref type " << PrettyTypeOf(ref);
      card_table->CheckAddrIsInCardTable(reinterpret_cast<const byte*>(obj));
      void* cover_begin = card_table->AddrFromCard(card_addr);
      void* cover_end = reinterpret_cast<void*>(reinterpret_cast<size_t>(cover_begin) +
          CardTable::kCardSize);
      LOG(ERROR) << "Card " << reinterpret_cast<void*>(card_addr) << " covers " << cover_begin
                 << "-" << cover_end;
      SpaceBitmap* bitmap = heap_->GetLiveBitmap()->GetSpaceBitmap(obj);

      // Print out how the object is live.
      if (bitmap->Test(obj)) {
        LOG(ERROR) << "Object " << obj << " found in live bitmap";
      }
      if (std::binary_search(alloc_stack->Begin(), alloc_stack->End(), obj)) {
        LOG(ERROR) << "Object " << obj << " found in allocation stack";
      }
      if (std::binary_search(live_stack->Begin(), live_stack->End(), obj)) {
        LOG(ERROR) << "Object " << obj << " found in live stack";
      }
      if (std::binary_search(live_stack->Begin(), live_stack->End(), ref)) {
        LOG(ERROR) << "Reference " << ref << " found in live stack!";
      }

      // Attempt to see if the card table missed the reference.
      ScanVisitor scan_visitor;
      byte* byte_cover_begin = reinterpret_cast<byte*>(card_table->AddrFromCard(card_addr));
      card_table->Scan(bitmap, byte_cover_begin, byte_cover_begin + CardTable::kCardSize,
                       scan_visitor, IdentityFunctor());

      // Try and see if a mark sweep collector scans the reference.
      ObjectStack* mark_stack = heap_->mark_stack_.get();
      MarkSweep ms(mark_stack);
      ms.Init();
      mark_stack->Reset();
      ms.DisableFinger();

      // All the references should end up in the mark stack.
      ms.ScanRoot(obj);
      if (std::find(mark_stack->Begin(), mark_stack->End(), ref)) {
        LOG(ERROR) << "Ref found in the mark_stack when rescanning the object!";
      } else {
        LOG(ERROR) << "Dumping mark stack contents";
        for (Object** it = mark_stack->Begin(); it != mark_stack->End(); ++it) {
          LOG(ERROR) << *it;
        }
      }
      mark_stack->Reset();

      // Search to see if any of the roots reference our object.
      void* arg = const_cast<void*>(reinterpret_cast<const void*>(obj));
      Runtime::Current()->VisitRoots(&Heap::RootMatchesObjectVisitor, arg);
      *failed_ = true;
    }
  }

  bool IsLive(const Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    SpaceBitmap* bitmap = heap_->GetLiveBitmap()->GetSpaceBitmap(obj);
    if (bitmap != NULL) {
      if (bitmap->Test(obj)) {
        return true;
      }
    } else if (heap_->GetLargeObjectsSpace()->Contains(obj)) {
      return true;
    } else {
      heap_->DumpSpaces();
      LOG(ERROR) << "Object " << obj << " not found in any spaces";
    }
    ObjectStack* alloc_stack = heap_->allocation_stack_.get();
    // At this point we need to search the allocation since things in the live stack may get swept.
    if (std::binary_search(alloc_stack->Begin(), alloc_stack->End(), const_cast<Object*>(obj))) {
      return true;
    }
    // Not either in the live bitmap or allocation stack, so the object must be dead.
    return false;
  }

 private:
  Heap* heap_;
  bool* failed_;
};

class VerifyObjectVisitor {
 public:
  VerifyObjectVisitor(Heap* heap)
      : heap_(heap),
        failed_(false) {

  }

  void operator ()(const Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceVisitor visitor(heap_, const_cast<bool*>(&failed_));
    MarkSweep::VisitObjectReferences(obj, visitor);
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* heap_;
  bool failed_;
};

// Must do this with mutators suspended since we are directly accessing the allocation stacks.
bool Heap::VerifyHeapReferences() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  // Lets sort our allocation stacks so that we can efficiently binary search them.
  std::sort(allocation_stack_->Begin(), allocation_stack_->End());
  std::sort(live_stack_->Begin(), live_stack_->End());
  // Perform the verification.
  VerifyObjectVisitor visitor(this);
  GetLiveBitmap()->Visit(visitor);
  // We don't want to verify the objects in the allocation stack since they themselves may be
  // pointing to dead objects if they are not reachable.
  if (visitor.Failed()) {
    DumpSpaces();
    return false;
  }
  return true;
}

class VerifyReferenceCardVisitor {
 public:
  VerifyReferenceCardVisitor(Heap* heap, bool* failed)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap),
        failed_(failed) {
  }

  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for smarter
  // analysis.
  void operator ()(const Object* obj, const Object* ref, const MemberOffset& offset,
                     bool is_static) const NO_THREAD_SAFETY_ANALYSIS {
    if (ref != NULL && !obj->GetClass()->IsPrimitiveArray()) {
      CardTable* card_table = heap_->GetCardTable();
      // If the object is not dirty and it is referencing something in the live stack other than
      // class, then it must be on a dirty card.
      if (!card_table->AddrIsInCardTable(obj)) {
        LOG(ERROR) << "Object " << obj << " is not in the address range of the card table";
        *failed_ = true;
      } else if (!card_table->IsDirty(obj)) {
        ObjectStack* live_stack = heap_->live_stack_.get();
        if (std::binary_search(live_stack->Begin(), live_stack->End(), ref) && !ref->IsClass()) {
          if (std::binary_search(live_stack->Begin(), live_stack->End(), obj)) {
            LOG(ERROR) << "Object " << obj << " found in live stack";
          }
          if (heap_->GetLiveBitmap()->Test(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live bitmap";
          }
          LOG(ERROR) << "Object " << obj << " " << PrettyTypeOf(obj)
                    << " references " << ref << " " << PrettyTypeOf(ref) << " in live stack";

          // Print which field of the object is dead.
          if (!obj->IsObjectArray()) {
            const Class* klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != NULL);
            const ObjectArray<Field>* fields = is_static ? klass->GetSFields() : klass->GetIFields();
            CHECK(fields != NULL);
            for (int32_t i = 0; i < fields->GetLength(); ++i) {
              const Field* cur = fields->Get(i);
              if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                          << PrettyField(cur);
                break;
              }
            }
          } else {
            const ObjectArray<Object>* object_array = obj->AsObjectArray<Object>();
            for (int32_t i = 0; i < object_array->GetLength(); ++i) {
              if (object_array->Get(i) == ref) {
                LOG(ERROR) << (is_static ? "Static " : "") << "obj[" << i << "] = ref";
              }
            }
          }

          *failed_ = true;
        }
      }
    }
  }

 private:
  Heap* heap_;
  bool* failed_;
};

class VerifyLiveStackReferences {
 public:
  VerifyLiveStackReferences(Heap* heap)
      : heap_(heap),
        failed_(false) {

  }

  void operator ()(const Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceCardVisitor visitor(heap_, const_cast<bool*>(&failed_));
    MarkSweep::VisitObjectReferences(obj, visitor);
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* heap_;
  bool failed_;
};

bool Heap::VerifyMissingCardMarks() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());

  VerifyLiveStackReferences visitor(this);
  GetLiveBitmap()->Visit(visitor);

  // We can verify objects in the live stack since none of these should reference dead objects.
  for (Object** it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
    visitor(*it);
  }

  if (visitor.Failed()) {
    DumpSpaces();
    return false;
  }
  return true;
}

void Heap::SwapBitmaps(GcType gc_type) {
  // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
  // these bitmaps. The bitmap swapping is an optimization so that we do not need to clear the live
  // bits of dead objects in the live bitmap.
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    ContinuousSpace* space = *it;
    // We never allocate into zygote spaces.
    if (space->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect ||
        (gc_type == kGcTypeFull &&
            space->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect)) {
      live_bitmap_->ReplaceBitmap(space->GetLiveBitmap(), space->GetMarkBitmap());
      mark_bitmap_->ReplaceBitmap(space->GetMarkBitmap(), space->GetLiveBitmap());
      space->AsAllocSpace()->SwapBitmaps();
    }
  }
  SwapLargeObjects();
}

void Heap::SwapLargeObjects() {
  large_object_space_->SwapBitmaps();
  live_bitmap_->SetLargeObjects(large_object_space_->GetLiveObjects());
  mark_bitmap_->SetLargeObjects(large_object_space_->GetMarkObjects());
}

void Heap::SwapStacks() {
  ObjectStack* temp = allocation_stack_.release();
  allocation_stack_.reset(live_stack_.release());
  live_stack_.reset(temp);

  // Sort the live stack so that we can quickly binary search it later.
  if (VERIFY_OBJECT_ENABLED) {
    std::sort(live_stack_->Begin(), live_stack_->End());
  }
}

void Heap::ClearCards(TimingLogger& timings) {
  // Clear image space cards and keep track of cards we cleared in the mod-union table.
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    ContinuousSpace* space = *it;
    if (space->IsImageSpace()) {
      mod_union_table_->ClearCards(*it);
      timings.AddSplit("ModUnionClearCards");
    } else if (space->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect) {
      zygote_mod_union_table_->ClearCards(space);
      timings.AddSplit("ZygoteModUnionClearCards");
    } else {
      card_table_->ClearSpaceCards(space);
      timings.AddSplit("ClearCards");
    }
  }
}

void Heap::CollectGarbageConcurrentMarkSweepPlan(Thread* self, GcType gc_type, GcCause gc_cause,
                                                 bool clear_soft_references) {
  TimingLogger timings("ConcurrentCollectGarbageInternal", true);
  uint64_t root_begin = NanoTime(), root_end = 0, dirty_begin = 0, dirty_end = 0;
  std::stringstream gc_type_str;
  gc_type_str << gc_type << " ";

  // Suspend all threads are get exclusive access to the heap.
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();
  timings.AddSplit("SuspendAll");
  Locks::mutator_lock_->AssertExclusiveHeld(self);

  size_t bytes_freed = 0;
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep(mark_stack_.get());
    timings.AddSplit("ctor");

    mark_sweep.Init();
    timings.AddSplit("Init");

    if (verify_pre_gc_heap_) {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      if (!VerifyHeapReferences()) {
        LOG(FATAL) << "Pre " << gc_type_str.str() << "Gc verification failed";
      }
      timings.AddSplit("VerifyHeapReferencesPreGC");
    }

    // Swap the stacks, this is safe since all the mutators are suspended at this point.
    SwapStacks();

    // Check that all objects which reference things in the live stack are on dirty cards.
    if (verify_missing_card_marks_) {
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Sort the live stack so that we can quickly binary search it later.
      std::sort(live_stack_->Begin(), live_stack_->End());
      if (!VerifyMissingCardMarks()) {
        LOG(FATAL) << "Pre GC verification of missing card marks failed";
      }
    }

    // We will need to know which cards were dirty for doing concurrent processing of dirty cards.
    // TODO: Investigate using a mark stack instead of a vector.
    std::vector<byte*> dirty_cards;
    if (gc_type == kGcTypeSticky) {
      dirty_cards.reserve(4 * KB);
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        card_table_->GetDirtyCards(*it, dirty_cards);
      }
      timings.AddSplit("GetDirtyCards");
    }

    // Clear image space cards and keep track of cards we cleared in the mod-union table.
    ClearCards(timings);

    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);

      for (Object** it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
        DCHECK(!GetLiveBitmap()->Test(*it));
      }

      if (gc_type == kGcTypePartial) {
        // Copy the mark bits over from the live bits, do this as early as possible or else we can
        // accidentally un-mark roots.
        // Needed for scanning dirty objects.
        for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
          if ((*it)->GetGcRetentionPolicy() == kGcRetentionPolicyFullCollect) {
            mark_sweep.BindLiveToMarkBitmap(*it);
          }
        }
        timings.AddSplit("BindLiveToMark");
        mark_sweep.SetImmuneRange(reinterpret_cast<Object*>(spaces_.front()->Begin()),
                                  reinterpret_cast<Object*>(alloc_space_->Begin()));
      } else if (gc_type == kGcTypeSticky) {
        for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
          if ((*it)->GetGcRetentionPolicy() != kGcRetentionPolicyNeverCollect) {
            mark_sweep.BindLiveToMarkBitmap(*it);
          }
        }
        timings.AddSplit("BindLiveToMark");
        large_object_space_->CopyLiveToMarked();
        timings.AddSplit("CopyLiveToMarked");
        mark_sweep.SetImmuneRange(reinterpret_cast<Object*>(spaces_.front()->Begin()),
                                  reinterpret_cast<Object*>(alloc_space_->Begin()));
      }
      mark_sweep.FindDefaultMarkBitmap();

      // Marking roots is not necessary for sticky mark bits since we only actually require the
      // remarking of roots.
      if (gc_type != kGcTypeSticky) {
        mark_sweep.MarkRoots();
        timings.AddSplit("MarkRoots");
      }

      if (verify_mod_union_table_) {
        zygote_mod_union_table_->Update();
        zygote_mod_union_table_->Verify();
        mod_union_table_->Update();
        mod_union_table_->Verify();
      }
    }

    // Roots are marked on the bitmap and the mark_stack is empty.
    DCHECK(mark_stack_->IsEmpty());

    // Allow mutators to go again, acquire share on mutator_lock_ to continue.
    thread_list->ResumeAll();
    {
      ReaderMutexLock reader_lock(self, *Locks::mutator_lock_);
      root_end = NanoTime();
      timings.AddSplit("RootEnd");

      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      UpdateAndMarkModUnion(&mark_sweep, timings, gc_type);

      if (gc_type != kGcTypeSticky) {
        // Mark everything allocated since the last as GC live so that we can sweep concurrently,
        // knowing that new allocations won't be marked as live.
        MarkAllocStack(alloc_space_->GetLiveBitmap(), large_object_space_->GetLiveObjects(),
                       live_stack_.get());
        timings.AddSplit("MarkStackAsLive");
      }

      if (gc_type != kGcTypeSticky) {
        // Recursively mark all the non-image bits set in the mark bitmap.
        mark_sweep.RecursiveMark(gc_type == kGcTypePartial, timings);
      } else {
        mark_sweep.RecursiveMarkCards(card_table_.get(), dirty_cards, timings);
      }
      mark_sweep.DisableFinger();
    }
    // Release share on mutator_lock_ and then get exclusive access.
    dirty_begin = NanoTime();
    thread_list->SuspendAll();
    timings.AddSplit("ReSuspend");
    Locks::mutator_lock_->AssertExclusiveHeld(self);

    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);

      // Re-mark root set.
      mark_sweep.ReMarkRoots();
      timings.AddSplit("ReMarkRoots");

      // Scan dirty objects, this is only required if we are not doing concurrent GC.
      mark_sweep.RecursiveMarkDirtyObjects(false);
      timings.AddSplit("RecursiveMarkDirtyObjects");
    }

    {
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);

      mark_sweep.ProcessReferences(clear_soft_references);
      timings.AddSplit("ProcessReferences");
    }

    // Only need to do this if we have the card mark verification on, and only during concurrent GC.
    if (verify_missing_card_marks_) {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      mark_sweep.SweepArray(timings, allocation_stack_.get(), false);
    } else {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // We only sweep over the live stack, and the live stack should not intersect with the
      // allocation stack, so it should be safe to UnMark anything in the allocation stack as live.
      UnMarkAllocStack(alloc_space_->GetMarkBitmap(), large_object_space_->GetMarkObjects(),
                      allocation_stack_.get());
      timings.AddSplit("UnMarkAllocStack");
#ifndef NDEBUG
      if (gc_type == kGcTypeSticky) {
        // Make sure everything in the live stack isn't something we unmarked.
        std::sort(allocation_stack_->Begin(), allocation_stack_->End());
        for (Object** it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
          DCHECK(!std::binary_search(allocation_stack_->Begin(), allocation_stack_->End(), *it))
              << "Unmarked object " << *it << " in the live stack";
        }
      } else {
        for (Object** it = allocation_stack_->Begin(); it != allocation_stack_->End(); ++it) {
          DCHECK(!GetLiveBitmap()->Test(*it)) << "Object " << *it << " is marked as live";
        }
      }
#endif
    }

    if (kIsDebugBuild) {
      // Verify that we only reach marked objects from the image space.
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      mark_sweep.VerifyImageRoots();
      timings.AddSplit("VerifyImageRoots");
    }

    if (verify_post_gc_heap_) {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      SwapBitmaps(gc_type);
      if (!VerifyHeapReferences()) {
        LOG(FATAL) << "Post " << gc_type_str.str() << "Gc verification failed";
      }
      SwapBitmaps(gc_type);
      timings.AddSplit("VerifyHeapReferencesPostGC");
    }

    thread_list->ResumeAll();
    dirty_end = NanoTime();
    Locks::mutator_lock_->AssertNotHeld(self);

    {
      // TODO: this lock shouldn't be necessary (it's why we did the bitmap flip above).
      if (gc_type != kGcTypeSticky) {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        mark_sweep.Sweep(gc_type == kGcTypePartial, false);
        timings.AddSplit("Sweep");
        mark_sweep.SweepLargeObjects(false);
        timings.AddSplit("SweepLargeObjects");
      } else {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        mark_sweep.SweepArray(timings, live_stack_.get(), false);
        timings.AddSplit("SweepArray");
      }
      live_stack_->Reset();
    }

    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Unbind the live and mark bitmaps.
      mark_sweep.UnBindBitmaps();

      // Swap the live and mark bitmaps for each space which we modified space. This is an
      // optimization that enables us to not clear live bits inside of the sweep.
      const bool swap = true;
      if (swap) {
        if (gc_type == kGcTypeSticky) {
          SwapLargeObjects();
        } else {
          SwapBitmaps(gc_type);
        }
      }
    }

    if (verify_system_weaks_) {
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      mark_sweep.VerifySystemWeaks();
      timings.AddSplit("VerifySystemWeaks");
    }

    cleared_references = mark_sweep.GetClearedReferences();
    bytes_freed = mark_sweep.GetFreedBytes();
    total_bytes_freed_ += bytes_freed;
    total_objects_freed_ += mark_sweep.GetFreedObjects();
  }

  GrowForUtilization();
  timings.AddSplit("GrowForUtilization");

  EnqueueClearedReferences(&cleared_references);
  timings.AddSplit("EnqueueClearedReferences");

  RequestHeapTrim();
  timings.AddSplit("Finish");

  // If the GC was slow, then print timings in the log.
  uint64_t pause_roots = (root_end - root_begin) / 1000 * 1000;
  uint64_t pause_dirty = (dirty_end - dirty_begin) / 1000 * 1000;
  uint64_t duration = (NanoTime() - root_begin) / 1000 * 1000;
  total_paused_time_ += (pause_roots + pause_dirty) / kTimeAdjust;
  if (pause_roots > MsToNs(5) || pause_dirty > MsToNs(5) ||
      (gc_cause == kGcCauseForAlloc && duration > MsToNs(20))) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetUsedMemorySize();
    const size_t total_memory = GetTotalMemory();
    LOG(INFO) << gc_cause << " " << gc_type_str.str()
              << "Concurrent GC freed " << PrettySize(bytes_freed) << ", " << percent_free
              << "% free, " << PrettySize(current_heap_size) << "/"
              << PrettySize(total_memory) << ", " << "paused " << PrettyDuration(pause_roots)
              << "+" << PrettyDuration(pause_dirty) << " total " << PrettyDuration(duration);
    if (VLOG_IS_ON(heap)) {
      timings.Dump();
    }
  }

  CumulativeLogger* logger = cumulative_timings_.Get(gc_type);
  logger->Start();
  logger->AddLogger(timings);
  logger->End(); // Next iteration.
}

GcType Heap::WaitForConcurrentGcToComplete(Thread* self) {
  GcType last_gc_type = kGcTypeNone;
  if (concurrent_gc_) {
    bool do_wait;
    uint64_t wait_start = NanoTime();
    {
      // Check if GC is running holding gc_complete_lock_.
      MutexLock mu(self, *gc_complete_lock_);
      do_wait = is_gc_running_;
    }
    if (do_wait) {
      uint64_t wait_time;
      // We must wait, change thread state then sleep on gc_complete_cond_;
      ScopedThreadStateChange tsc(Thread::Current(), kWaitingForGcToComplete);
      {
        MutexLock mu(self, *gc_complete_lock_);
        while (is_gc_running_) {
          gc_complete_cond_->Wait(self, *gc_complete_lock_);
        }
        last_gc_type = last_gc_type_;
        wait_time = NanoTime() - wait_start;;
        total_wait_time_ += wait_time;
      }
      if (wait_time > MsToNs(5)) {
        LOG(INFO) << "WaitForConcurrentGcToComplete blocked for " << PrettyDuration(wait_time);
      }
    }
  }
  return last_gc_type;
}

void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetUsedMemorySize()) << "/"
     << PrettySize(GetTotalMemory()) << "; " << GetObjectsAllocated() << " objects\n";
  DumpGcPerformanceInfo();
}

size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(GetFreeMemory()) / GetTotalMemory());
}

void Heap::SetIdealFootprint(size_t max_allowed_footprint) {
  if (max_allowed_footprint > GetMaxMemory()) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(max_allowed_footprint) << " to "
             << PrettySize(GetMaxMemory());
    max_allowed_footprint = GetMaxMemory();
  }
  max_allowed_footprint_ = max_allowed_footprint;
}

void Heap::GrowForUtilization() {
  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more when necessary.
  size_t target_size = num_bytes_allocated_ / Heap::GetTargetHeapUtilization();
  if (target_size > num_bytes_allocated_ + max_free_) {
    target_size = num_bytes_allocated_ + max_free_;
  } else if (target_size < num_bytes_allocated_ + min_free_) {
    target_size = num_bytes_allocated_ + min_free_;
  }

  // Calculate when to perform the next ConcurrentGC.
  if (GetFreeMemory() < concurrent_min_free_) {
    // Not enough free memory to perform concurrent GC.
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  } else {
    // Start a concurrent Gc when we get close to the target size.
    concurrent_start_bytes_ = target_size - concurrent_start_size_;
  }

  SetIdealFootprint(target_size);
}

void Heap::ClearGrowthLimit() {
  WaitForConcurrentGcToComplete(Thread::Current());
  alloc_space_->ClearGrowthLimit();
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

void Heap::AddFinalizerReference(Thread* self, Object* object) {
  ScopedObjectAccess soa(self);
  JValue args[1];
  args[0].SetL(object);
  soa.DecodeMethod(WellKnownClasses::java_lang_ref_FinalizerReference_add)->Invoke(self, NULL, args,
                                                                                   NULL);
}

size_t Heap::GetBytesAllocated() const {
  return num_bytes_allocated_;
}

size_t Heap::GetObjectsAllocated() const {
  size_t total = 0;
  // TODO: C++0x
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsAllocSpace()) {
      total += space->AsAllocSpace()->GetNumObjectsAllocated();
    }
  }
  return total;
}

size_t Heap::GetConcurrentStartSize() const {
  return concurrent_start_size_;
}

size_t Heap::GetConcurrentMinFree() const {
  return concurrent_min_free_;
}

void Heap::EnqueueClearedReferences(Object** cleared) {
  DCHECK(cleared != NULL);
  if (*cleared != NULL) {
    ScopedObjectAccess soa(Thread::Current());
    JValue args[1];
    args[0].SetL(*cleared);
    soa.DecodeMethod(WellKnownClasses::java_lang_ref_ReferenceQueue_add)->Invoke(soa.Self(), NULL,
                                                                                 args, NULL);
    *cleared = NULL;
  }
}

void Heap::RequestConcurrentGC(Thread* self) {
  // Make sure that we can do a concurrent GC.
  Runtime* runtime = Runtime::Current();
  if (requesting_gc_ || runtime == NULL || !runtime->IsFinishedStarting() ||
      !runtime->IsConcurrentGcEnabled()) {
    return;
  }
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDown()) {
      return;
    }
  }
  if (self->IsHandlingStackOverflow()) {
    return;
  }

  requesting_gc_ = true;
  JNIEnv* env = self->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != NULL);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestGC != NULL);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestGC);
  CHECK(!env->ExceptionCheck());
  requesting_gc_ = false;
}

void Heap::ConcurrentGC(Thread* self) {
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    if (Runtime::Current()->IsShuttingDown() || !concurrent_gc_) {
      return;
    }
  }

  if (WaitForConcurrentGcToComplete(self) == kGcTypeNone) {
    // Start a concurrent GC as one wasn't in progress
    ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
    if (alloc_space_->Size() > min_alloc_space_size_for_sticky_gc_) {
      CollectGarbageInternal(kGcTypeSticky, kGcCauseBackground, false);
    } else {
      CollectGarbageInternal(kGcTypePartial, kGcCauseBackground, false);
    }
  }
}

void Heap::Trim(Thread* self) {
  WaitForConcurrentGcToComplete(self);
  alloc_space_->Trim();
}

void Heap::RequestHeapTrim() {
  // We don't have a good measure of how worthwhile a trim might be. We can't use the live bitmap
  // because that only marks object heads, so a large array looks like lots of empty space. We
  // don't just call dlmalloc all the time, because the cost of an _attempted_ trim is proportional
  // to utilization (which is probably inversely proportional to how much benefit we can expect).
  // We could try mincore(2) but that's only a measure of how many pages we haven't given away,
  // not how much use we're making of those pages.
  uint64_t ms_time = NsToMs(NanoTime());
  float utilization =
      static_cast<float>(alloc_space_->GetNumBytesAllocated()) / alloc_space_->Size();
  if ((utilization > 0.75f) || ((ms_time - last_trim_time_) < 2 * 1000)) {
    // Don't bother trimming the alloc space if it's more than 75% utilized, or if a
    // heap trim occurred in the last two seconds.
    return;
  }

  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    if (runtime == NULL || !runtime->IsFinishedStarting() || runtime->IsShuttingDown()) {
      // Heap trimming isn't supported without a Java runtime or Daemons (such as at dex2oat time)
      // Also: we do not wish to start a heap trim if the runtime is shutting down (a racy check
      // as we don't hold the lock while requesting the trim).
      return;
    }
  }
  last_trim_time_ = ms_time;
  JNIEnv* env = self->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != NULL);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestHeapTrim != NULL);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestHeapTrim);
  CHECK(!env->ExceptionCheck());
}

}  // namespace art
