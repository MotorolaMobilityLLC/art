/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_JIT_JIT_CODE_CACHE_H_
#define ART_RUNTIME_JIT_JIT_CODE_CACHE_H_

#include "instrumentation.h"

#include "atomic.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "gc/accounting/bitmap.h"
#include "gc_root.h"
#include "jni.h"
#include "oat_file.h"
#include "object_callbacks.h"
#include "safe_map.h"
#include "thread_pool.h"

namespace art {

class ArtMethod;
class LinearAlloc;
class ProfilingInfo;

namespace jit {

class JitInstrumentationCache;

// Alignment in bits that will suit all architectures.
static constexpr int kJitCodeAlignment = 16;
using CodeCacheBitmap = gc::accounting::MemoryRangeBitmap<kJitCodeAlignment>;

class JitCodeCache {
 public:
  static constexpr size_t kMaxCapacity = 64 * MB;
  // Put the default to a very low amount for debug builds to stress the code cache
  // collection.
  static constexpr size_t kInitialCapacity = kIsDebugBuild ? 16 * KB : 64 * KB;

  // Create the code cache with a code + data capacity equal to "capacity", error message is passed
  // in the out arg error_msg.
  static JitCodeCache* Create(size_t initial_capacity, size_t max_capacity, std::string* error_msg);

  // Number of bytes allocated in the code cache.
  size_t CodeCacheSize() REQUIRES(!lock_);

  // Number of bytes allocated in the data cache.
  size_t DataCacheSize() REQUIRES(!lock_);

  // Number of compiled code in the code cache. Note that this is not the number
  // of methods that got JIT compiled, as we might have collected some.
  size_t NumberOfCompiledCode() REQUIRES(!lock_);

  bool NotifyCompilationOf(ArtMethod* method, Thread* self)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  void DoneCompiling(ArtMethod* method, Thread* self)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  // Allocate and write code and its metadata to the code cache.
  uint8_t* CommitCode(Thread* self,
                      ArtMethod* method,
                      const uint8_t* mapping_table,
                      const uint8_t* vmap_table,
                      const uint8_t* gc_map,
                      size_t frame_size_in_bytes,
                      size_t core_spill_mask,
                      size_t fp_spill_mask,
                      const uint8_t* code,
                      size_t code_size)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  // Return true if the code cache contains this pc.
  bool ContainsPc(const void* pc) const;

  // Return true if the code cache contains this method.
  bool ContainsMethod(ArtMethod* method) REQUIRES(!lock_);

  // Reserve a region of data of size at least "size". Returns null if there is no more room.
  uint8_t* ReserveData(Thread* self, size_t size)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  // Clear data from the data portion of the code cache.
  void ClearData(Thread* self, void* data)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  // Add a data array of size (end - begin) with the associated contents, returns null if there
  // is no more room.
  uint8_t* AddDataArray(Thread* self, const uint8_t* begin, const uint8_t* end)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!lock_);

  CodeCacheBitmap* GetLiveBitmap() const {
    return live_bitmap_.get();
  }

  // Perform a collection on the code cache.
  void GarbageCollectCache(Thread* self)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Given the 'pc', try to find the JIT compiled code associated with it.
  // Return null if 'pc' is not in the code cache. 'method' is passed for
  // sanity check.
  OatQuickMethodHeader* LookupMethodHeader(uintptr_t pc, ArtMethod* method)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Remove all methods in our cache that were allocated by 'alloc'.
  void RemoveMethodsIn(Thread* self, const LinearAlloc& alloc)
      REQUIRES(!lock_)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Create a 'ProfileInfo' for 'method'. If 'retry_allocation' is true,
  // will collect and retry if the first allocation is unsuccessful.
  ProfilingInfo* AddProfilingInfo(Thread* self,
                                  ArtMethod* method,
                                  const std::vector<uint32_t>& entries,
                                  bool retry_allocation)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool OwnsSpace(const void* mspace) const NO_THREAD_SAFETY_ANALYSIS {
    return mspace == code_mspace_ || mspace == data_mspace_;
  }

  void* MoreCore(const void* mspace, intptr_t increment);

  // Adds to `methods` all the compiled ArtMethods which are part of any of the given dex locations.
  void GetCompiledArtMethods(const std::set<const std::string>& dex_base_locations,
                             std::set<ArtMethod*>& methods)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  uint64_t GetLastUpdateTimeNs() REQUIRES(!lock_);

 private:
  // Take ownership of maps.
  JitCodeCache(MemMap* code_map,
               MemMap* data_map,
               size_t initial_code_capacity,
               size_t initial_data_capacity,
               size_t max_capacity);

  // Internal version of 'CommitCode' that will not retry if the
  // allocation fails. Return null if the allocation fails.
  uint8_t* CommitCodeInternal(Thread* self,
                              ArtMethod* method,
                              const uint8_t* mapping_table,
                              const uint8_t* vmap_table,
                              const uint8_t* gc_map,
                              size_t frame_size_in_bytes,
                              size_t core_spill_mask,
                              size_t fp_spill_mask,
                              const uint8_t* code,
                              size_t code_size)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  ProfilingInfo* AddProfilingInfoInternal(Thread* self,
                                          ArtMethod* method,
                                          const std::vector<uint32_t>& entries)
      REQUIRES(!lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // If a collection is in progress, wait for it to finish. Return
  // whether the thread actually waited.
  bool WaitForPotentialCollectionToComplete(Thread* self)
      REQUIRES(lock_) REQUIRES(!Locks::mutator_lock_);

  // Free in the mspace allocations taken by 'method'.
  void FreeCode(const void* code_ptr, ArtMethod* method) REQUIRES(lock_);

  // Number of bytes allocated in the code cache.
  size_t CodeCacheSizeLocked() REQUIRES(lock_);

  // Number of bytes allocated in the data cache.
  size_t DataCacheSizeLocked() REQUIRES(lock_);

  // Notify all waiting threads that a collection is done.
  void NotifyCollectionDone(Thread* self) REQUIRES(lock_);

  // Try to increase the current capacity of the code cache. Return whether we
  // succeeded at doing so.
  bool IncreaseCodeCacheCapacity() REQUIRES(lock_);

  // Set the footprint limit of the code cache.
  void SetFootprintLimit(size_t new_footprint) REQUIRES(lock_);

  // Lock for guarding allocations, collections, and the method_code_map_.
  Mutex lock_;
  // Condition to wait on during collection.
  ConditionVariable lock_cond_ GUARDED_BY(lock_);
  // Whether there is a code cache collection in progress.
  bool collection_in_progress_ GUARDED_BY(lock_);
  // Mem map which holds code.
  std::unique_ptr<MemMap> code_map_;
  // Mem map which holds data (stack maps and profiling info).
  std::unique_ptr<MemMap> data_map_;
  // The opaque mspace for allocating code.
  void* code_mspace_ GUARDED_BY(lock_);
  // The opaque mspace for allocating data.
  void* data_mspace_ GUARDED_BY(lock_);
  // Bitmap for collecting code and data.
  std::unique_ptr<CodeCacheBitmap> live_bitmap_;
  // This map holds compiled code associated to the ArtMethod.
  SafeMap<const void*, ArtMethod*> method_code_map_ GUARDED_BY(lock_);
  // ProfilingInfo objects we have allocated.
  std::vector<ProfilingInfo*> profiling_infos_ GUARDED_BY(lock_);

  // The maximum capacity in bytes this code cache can go to.
  size_t max_capacity_ GUARDED_BY(lock_);

  // The current capacity in bytes of the code cache.
  size_t current_capacity_ GUARDED_BY(lock_);

  // The current footprint in bytes of the code portion of the code cache.
  size_t code_end_ GUARDED_BY(lock_);

  // The current footprint in bytes of the data portion of the code cache.
  size_t data_end_ GUARDED_BY(lock_);

  // Whether a collection has already been done on the current capacity.
  bool has_done_one_collection_ GUARDED_BY(lock_);

  // Last time the the code_cache was updated.
  uint64_t last_update_time_ns_ GUARDED_BY(lock_);

  DISALLOW_IMPLICIT_CONSTRUCTORS(JitCodeCache);
};


}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_CODE_CACHE_H_
