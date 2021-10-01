/*
 * Copyright 2019 The Android Open Source Project
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

#include "jit_memory_region.h"

#include <fcntl.h>
#include <unistd.h>

#include <android-base/unique_fd.h>
#include "base/bit_utils.h"  // For RoundDown, RoundUp
#include "base/globals.h"
#include "base/logging.h"  // For VLOG.
#include "base/membarrier.h"
#include "base/memfd.h"
#include "base/systrace.h"
#include "gc/allocator/dlmalloc.h"
#include "jit/jit_scoped_code_cache_write.h"
#include "oat_quick_method_header.h"
#include "palette/palette.h"

using android::base::unique_fd;

namespace art {
namespace jit {

// Data cache will be half of the capacity
// Code cache will be the other half of the capacity.
// TODO: Make this variable?
static constexpr size_t kCodeAndDataCapacityDivider = 2;

bool JitMemoryRegion::Initialize(size_t initial_capacity,
                                 size_t max_capacity,
                                 bool rwx_memory_allowed,
                                 bool is_zygote,
                                 std::string* error_msg) {
  ScopedTrace trace(__PRETTY_FUNCTION__);

  CHECK_GE(max_capacity, initial_capacity);
  CHECK(max_capacity <= 1 * GB) << "The max supported size for JIT code cache is 1GB";
  // Align both capacities to page size, as that's the unit mspaces use.
  initial_capacity_ = RoundDown(initial_capacity, 2 * kPageSize);
  max_capacity_ = RoundDown(max_capacity, 2 * kPageSize);
  current_capacity_ = initial_capacity,
  data_end_ = initial_capacity / kCodeAndDataCapacityDivider;
  exec_end_ = initial_capacity - data_end_;

  const size_t capacity = max_capacity_;
  const size_t data_capacity = capacity / kCodeAndDataCapacityDivider;
  const size_t exec_capacity = capacity - data_capacity;

  // File descriptor enabling dual-view mapping of code section.
  unique_fd mem_fd;

  if (is_zygote) {
    // Because we are not going to GC code generated by the zygote, just use all available.
    current_capacity_ = max_capacity;
    mem_fd = unique_fd(CreateZygoteMemory(capacity, error_msg));
    if (mem_fd.get() < 0) {
      return false;
    }
  } else {
    // Bionic supports memfd_create, but the call may fail on older kernels.
    mem_fd = unique_fd(art::memfd_create("jit-cache", /* flags= */ 0));
    if (mem_fd.get() < 0) {
      std::ostringstream oss;
      oss << "Failed to initialize dual view JIT. memfd_create() error: " << strerror(errno);
      if (!rwx_memory_allowed) {
        // Without using RWX page permissions, the JIT can not fallback to single mapping as it
        // requires tranitioning the code pages to RWX for updates.
        *error_msg = oss.str();
        return false;
      }
      VLOG(jit) << oss.str();
    } else if (ftruncate(mem_fd, capacity) != 0) {
      std::ostringstream oss;
      oss << "Failed to initialize memory file: " << strerror(errno);
      *error_msg = oss.str();
      return false;
    }
  }

  std::string data_cache_name = is_zygote ? "zygote-data-code-cache" : "data-code-cache";
  std::string exec_cache_name = is_zygote ? "zygote-jit-code-cache" : "jit-code-cache";

  std::string error_str;
  // Map name specific for android_os_Debug.cpp accounting.
  // Map in low 4gb to simplify accessing root tables for x86_64.
  // We could do PC-relative addressing to avoid this problem, but that
  // would require reserving code and data area before submitting, which
  // means more windows for the code memory to be RWX.
  int base_flags;
  MemMap data_pages;
  if (mem_fd.get() >= 0) {
    // Dual view of JIT code cache case. Create an initial mapping of data pages large enough
    // for data and non-writable view of JIT code pages. We use the memory file descriptor to
    // enable dual mapping - we'll create a second mapping using the descriptor below. The
    // mappings will look like:
    //
    //       VA                  PA
    //
    //       +---------------+
    //       | non exec code |\
    //       +---------------+ \
    //       | writable data |\ \
    //       +---------------+ \ \
    //       :               :\ \ \
    //       +---------------+.\.\.+---------------+
    //       |  exec code    |  \ \|     code      |
    //       +---------------+...\.+---------------+
    //       | readonly data |    \|     data      |
    //       +---------------+.....+---------------+
    //
    // In this configuration code updates are written to the non-executable view of the code
    // cache, and the executable view of the code cache has fixed RX memory protections.
    //
    // This memory needs to be mapped shared as the code portions will have two mappings.
    //
    // Additionally, the zyzote will create a dual view of the data portion of
    // the cache. This mapping will be read-only, whereas the second mapping
    // will be writable.
    base_flags = MAP_SHARED;
    data_pages = MemMap::MapFile(
        data_capacity + exec_capacity,
        kProtR,
        base_flags,
        mem_fd,
        /* start= */ 0,
        /* low_4gb= */ true,
        data_cache_name.c_str(),
        &error_str);
  } else {
    // Single view of JIT code cache case. Create an initial mapping of data pages large enough
    // for data and JIT code pages. The mappings will look like:
    //
    //       VA                  PA
    //
    //       +---------------+...+---------------+
    //       |  exec code    |   |     code      |
    //       +---------------+...+---------------+
    //       |      data     |   |     data      |
    //       +---------------+...+---------------+
    //
    // In this configuration code updates are written to the executable view of the code cache,
    // and the executable view of the code cache transitions RX to RWX for the update and then
    // back to RX after the update.
    base_flags = MAP_PRIVATE | MAP_ANON;
    data_pages = MemMap::MapAnonymous(
        data_cache_name.c_str(),
        data_capacity + exec_capacity,
        kProtRW,
        /* low_4gb= */ true,
        &error_str);
  }

  if (!data_pages.IsValid()) {
    std::ostringstream oss;
    oss << "Failed to create read write cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return false;
  }

  MemMap exec_pages;
  MemMap non_exec_pages;
  MemMap writable_data_pages;
  if (exec_capacity > 0) {
    uint8_t* const divider = data_pages.Begin() + data_capacity;
    // Set initial permission for executable view to catch any SELinux permission problems early
    // (for processes that cannot map WX pages). Otherwise, this region does not need to be
    // executable as there is no code in the cache yet.
    exec_pages = data_pages.RemapAtEnd(divider,
                                       exec_cache_name.c_str(),
                                       kProtRX,
                                       base_flags | MAP_FIXED,
                                       mem_fd.get(),
                                       (mem_fd.get() >= 0) ? data_capacity : 0,
                                       &error_str);
    if (!exec_pages.IsValid()) {
      std::ostringstream oss;
      oss << "Failed to create read execute code cache: " << error_str << " size=" << capacity;
      *error_msg = oss.str();
      return false;
    }

    if (mem_fd.get() >= 0) {
      // For dual view, create the secondary view of code memory used for updating code. This view
      // is never executable.
      std::string name = exec_cache_name + "-rw";
      non_exec_pages = MemMap::MapFile(exec_capacity,
                                       kIsDebugBuild ? kProtR : kProtRW,
                                       base_flags,
                                       mem_fd,
                                       /* start= */ data_capacity,
                                       /* low_4GB= */ false,
                                       name.c_str(),
                                       &error_str);
      if (!non_exec_pages.IsValid()) {
        static const char* kFailedNxView = "Failed to map non-executable view of JIT code cache";
        if (rwx_memory_allowed) {
          // Log and continue as single view JIT (requires RWX memory).
          VLOG(jit) << kFailedNxView;
        } else {
          *error_msg = kFailedNxView;
          return false;
        }
      }
      // Create a dual view of the data cache.
      name = data_cache_name + "-rw";
      writable_data_pages = MemMap::MapFile(data_capacity,
                                            kProtRW,
                                            base_flags,
                                            mem_fd,
                                            /* start= */ 0,
                                            /* low_4GB= */ false,
                                            name.c_str(),
                                            &error_str);
      if (!writable_data_pages.IsValid()) {
        std::ostringstream oss;
        oss << "Failed to create dual data view: " << error_str;
        *error_msg = oss.str();
        return false;
      }
      if (writable_data_pages.MadviseDontFork() != 0) {
        *error_msg = "Failed to madvise dont fork the writable data view";
        return false;
      }
      if (non_exec_pages.MadviseDontFork() != 0) {
        *error_msg = "Failed to madvise dont fork the writable code view";
        return false;
      }
      // Now that we have created the writable and executable mappings, prevent creating any new
      // ones.
      if (is_zygote && !ProtectZygoteMemory(mem_fd.get(), error_msg)) {
        return false;
      }
    }
  } else {
    // Profiling only. No memory for code required.
  }

  data_pages_ = std::move(data_pages);
  exec_pages_ = std::move(exec_pages);
  non_exec_pages_ = std::move(non_exec_pages);
  writable_data_pages_ = std::move(writable_data_pages);

  VLOG(jit) << "Created JitMemoryRegion"
            << ": data_pages=" << reinterpret_cast<void*>(data_pages_.Begin())
            << ", exec_pages=" << reinterpret_cast<void*>(exec_pages_.Begin())
            << ", non_exec_pages=" << reinterpret_cast<void*>(non_exec_pages_.Begin())
            << ", writable_data_pages=" << reinterpret_cast<void*>(writable_data_pages_.Begin());

  // Now that the pages are initialized, initialize the spaces.

  // Initialize the data heap.
  data_mspace_ = create_mspace_with_base(
      HasDualDataMapping() ? writable_data_pages_.Begin() : data_pages_.Begin(),
      data_end_,
      /* locked= */ false);
  CHECK(data_mspace_ != nullptr) << "create_mspace_with_base (data) failed";

  // Allow mspace to use the full data capacity.
  // It will still only use as litle memory as possible and ask for MoreCore as needed.
  CHECK(IsAlignedParam(data_capacity, kPageSize));
  mspace_set_footprint_limit(data_mspace_, data_capacity);

  // Initialize the code heap.
  MemMap* code_heap = nullptr;
  if (non_exec_pages_.IsValid()) {
    code_heap = &non_exec_pages_;
  } else if (exec_pages_.IsValid()) {
    code_heap = &exec_pages_;
  }
  if (code_heap != nullptr) {
    // Make all pages reserved for the code heap writable. The mspace allocator, that manages the
    // heap, will take and initialize pages in create_mspace_with_base().
    {
      ScopedCodeCacheWrite scc(*this);
      exec_mspace_ = create_mspace_with_base(code_heap->Begin(), exec_end_, false /*locked*/);
    }
    CHECK(exec_mspace_ != nullptr) << "create_mspace_with_base (exec) failed";
    SetFootprintLimit(current_capacity_);
  } else {
    exec_mspace_ = nullptr;
    SetFootprintLimit(current_capacity_);
  }
  return true;
}

void JitMemoryRegion::SetFootprintLimit(size_t new_footprint) {
  size_t data_space_footprint = new_footprint / kCodeAndDataCapacityDivider;
  DCHECK(IsAlignedParam(data_space_footprint, kPageSize));
  DCHECK_EQ(data_space_footprint * kCodeAndDataCapacityDivider, new_footprint);
  if (HasCodeMapping()) {
    ScopedCodeCacheWrite scc(*this);
    mspace_set_footprint_limit(exec_mspace_, new_footprint - data_space_footprint);
  }
}

bool JitMemoryRegion::IncreaseCodeCacheCapacity() {
  if (current_capacity_ == max_capacity_) {
    return false;
  }

  // Double the capacity if we're below 1MB, or increase it by 1MB if
  // we're above.
  if (current_capacity_ < 1 * MB) {
    current_capacity_ *= 2;
  } else {
    current_capacity_ += 1 * MB;
  }
  if (current_capacity_ > max_capacity_) {
    current_capacity_ = max_capacity_;
  }

  VLOG(jit) << "Increasing code cache capacity to " << PrettySize(current_capacity_);

  SetFootprintLimit(current_capacity_);

  return true;
}

// NO_THREAD_SAFETY_ANALYSIS as this is called from mspace code, at which point the lock
// is already held.
void* JitMemoryRegion::MoreCore(const void* mspace, intptr_t increment) NO_THREAD_SAFETY_ANALYSIS {
  if (mspace == exec_mspace_) {
    CHECK(exec_mspace_ != nullptr);
    const MemMap* const code_pages = GetUpdatableCodeMapping();
    void* result = code_pages->Begin() + exec_end_;
    exec_end_ += increment;
    return result;
  } else {
    CHECK_EQ(data_mspace_, mspace);
    const MemMap* const writable_data_pages = GetWritableDataMapping();
    void* result = writable_data_pages->Begin() + data_end_;
    data_end_ += increment;
    return result;
  }
}

const uint8_t* JitMemoryRegion::CommitCode(ArrayRef<const uint8_t> reserved_code,
                                           ArrayRef<const uint8_t> code,
                                           const uint8_t* stack_map,
                                           bool has_should_deoptimize_flag) {
  DCHECK(IsInExecSpace(reserved_code.data()));
  ScopedCodeCacheWrite scc(*this);

  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  size_t header_size = OatQuickMethodHeader::InstructionAlignedSize();
  size_t total_size = header_size + code.size();

  // Each allocation should be on its own set of cache lines.
  // `total_size` covers the OatQuickMethodHeader, the JIT generated machine code,
  // and any alignment padding.
  DCHECK_GT(total_size, header_size);
  DCHECK_LE(total_size, reserved_code.size());
  uint8_t* x_memory = const_cast<uint8_t*>(reserved_code.data());
  uint8_t* w_memory = const_cast<uint8_t*>(GetNonExecutableAddress(x_memory));
  // Ensure the header ends up at expected instruction alignment.
  DCHECK_ALIGNED_PARAM(reinterpret_cast<uintptr_t>(w_memory + header_size), alignment);
  const uint8_t* result = x_memory + header_size;

  // Write the code.
  std::copy(code.begin(), code.end(), w_memory + header_size);

  // Write the header.
  OatQuickMethodHeader* method_header =
      OatQuickMethodHeader::FromCodePointer(w_memory + header_size);
  new (method_header) OatQuickMethodHeader((stack_map != nullptr) ? result - stack_map : 0u);
  if (has_should_deoptimize_flag) {
    method_header->SetHasShouldDeoptimizeFlag();
  }

  // Both instruction and data caches need flushing to the point of unification where both share
  // a common view of memory. Flushing the data cache ensures the dirty cachelines from the
  // newly added code are written out to the point of unification. Flushing the instruction
  // cache ensures the newly written code will be fetched from the point of unification before
  // use. Memory in the code cache is re-cycled as code is added and removed. The flushes
  // prevent stale code from residing in the instruction cache.
  //
  // Caches are flushed before write permission is removed because some ARMv8 Qualcomm kernels
  // may trigger a segfault if a page fault occurs when requesting a cache maintenance
  // operation. This is a kernel bug that we need to work around until affected devices
  // (e.g. Nexus 5X and 6P) stop being supported or their kernels are fixed.
  //
  // For reference, this behavior is caused by this commit:
  // https://android.googlesource.com/kernel/msm/+/3fbe6bc28a6b9939d0650f2f17eb5216c719950c
  //
  bool cache_flush_success = true;
  if (HasDualCodeMapping()) {
    // Flush d-cache for the non-executable mapping.
    cache_flush_success = FlushCpuCaches(w_memory, w_memory + total_size);
  }

  // Invalidate i-cache for the executable mapping.
  if (cache_flush_success) {
    cache_flush_success = FlushCpuCaches(x_memory, x_memory + total_size);
  }

  // If flushing the cache has failed, reject the allocation because we can't guarantee
  // correctness of the instructions present in the processor caches.
  if (!cache_flush_success) {
    PLOG(ERROR) << "Cache flush failed triggering code allocation failure";
    return nullptr;
  }

  // Ensure CPU instruction pipelines are flushed for all cores. This is necessary for
  // correctness as code may still be in instruction pipelines despite the i-cache flush. It is
  // not safe to assume that changing permissions with mprotect (RX->RWX->RX) will cause a TLB
  // shootdown (incidentally invalidating the CPU pipelines by sending an IPI to all cores to
  // notify them of the TLB invalidation). Some architectures, notably ARM and ARM64, have
  // hardware support that broadcasts TLB invalidations and so their kernels have no software
  // based TLB shootdown. The sync-core flavor of membarrier was introduced in Linux 4.16 to
  // address this (see mbarrier(2)). The membarrier here will fail on prior kernels and on
  // platforms lacking the appropriate support.
  art::membarrier(art::MembarrierCommand::kPrivateExpeditedSyncCore);

  return result;
}

static void FillRootTable(uint8_t* roots_data, const std::vector<Handle<mirror::Object>>& roots)
    REQUIRES(Locks::jit_lock_)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  GcRoot<mirror::Object>* gc_roots = reinterpret_cast<GcRoot<mirror::Object>*>(roots_data);
  const uint32_t length = roots.size();
  // Put all roots in `roots_data`.
  for (uint32_t i = 0; i < length; ++i) {
    ObjPtr<mirror::Object> object = roots[i].Get();
    gc_roots[i] = GcRoot<mirror::Object>(object);
  }
  // Store the length of the table at the end. This will allow fetching it from a stack_map
  // pointer.
  reinterpret_cast<uint32_t*>(roots_data)[length] = length;
}

bool JitMemoryRegion::CommitData(ArrayRef<const uint8_t> reserved_data,
                                 const std::vector<Handle<mirror::Object>>& roots,
                                 ArrayRef<const uint8_t> stack_map) {
  DCHECK(IsInDataSpace(reserved_data.data()));
  uint8_t* roots_data = GetWritableDataAddress(reserved_data.data());
  size_t root_table_size = ComputeRootTableSize(roots.size());
  uint8_t* stack_map_data = roots_data + root_table_size;
  DCHECK_LE(root_table_size + stack_map.size(), reserved_data.size());
  FillRootTable(roots_data, roots);
  memcpy(stack_map_data, stack_map.data(), stack_map.size());
  // Flush data cache, as compiled code references literals in it.
  // TODO(oth): establish whether this is necessary.
  if (UNLIKELY(!FlushCpuCaches(roots_data, roots_data + root_table_size + stack_map.size()))) {
    VLOG(jit) << "Failed to flush data in CommitData";
    return false;
  }
  return true;
}

const uint8_t* JitMemoryRegion::AllocateCode(size_t size) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  void* result = mspace_memalign(exec_mspace_, alignment, size);
  if (UNLIKELY(result == nullptr)) {
    return nullptr;
  }
  used_memory_for_code_ += mspace_usable_size(result);
  return reinterpret_cast<uint8_t*>(GetExecutableAddress(result));
}

void JitMemoryRegion::FreeCode(const uint8_t* code) {
  code = GetNonExecutableAddress(code);
  used_memory_for_code_ -= mspace_usable_size(code);
  mspace_free(exec_mspace_, const_cast<uint8_t*>(code));
}

const uint8_t* JitMemoryRegion::AllocateData(size_t data_size) {
  void* result = mspace_malloc(data_mspace_, data_size);
  if (UNLIKELY(result == nullptr)) {
    return nullptr;
  }
  used_memory_for_data_ += mspace_usable_size(result);
  return reinterpret_cast<uint8_t*>(GetNonWritableDataAddress(result));
}

void JitMemoryRegion::FreeData(const uint8_t* data) {
  FreeWritableData(GetWritableDataAddress(data));
}

void JitMemoryRegion::FreeWritableData(uint8_t* writable_data) REQUIRES(Locks::jit_lock_) {
  used_memory_for_data_ -= mspace_usable_size(writable_data);
  mspace_free(data_mspace_, writable_data);
}

#if defined(__BIONIC__) && defined(ART_TARGET)
// The code below only works on bionic on target.

int JitMemoryRegion::CreateZygoteMemory(size_t capacity, std::string* error_msg) {
  if (CacheOperationsMaySegFault()) {
    // Zygote JIT requires dual code mappings by design. We can only do this if the cache flush
    // and invalidate instructions work without raising faults.
    *error_msg = "Zygote memory only works with dual mappings";
    return -1;
  }
  /* Check if kernel support exists, otherwise fall back to ashmem */
  static const char* kRegionName = "jit-zygote-cache";
  if (art::IsSealFutureWriteSupported()) {
    int fd = art::memfd_create(kRegionName, MFD_ALLOW_SEALING);
    if (fd == -1) {
      std::ostringstream oss;
      oss << "Failed to create zygote mapping: " << strerror(errno);
      *error_msg = oss.str();
      return -1;
    }

    if (ftruncate(fd, capacity) != 0) {
      std::ostringstream oss;
      oss << "Failed to create zygote mapping: " << strerror(errno);
      *error_msg = oss.str();
      return -1;
    }

    return fd;
  }

  LOG(INFO) << "Falling back to ashmem implementation for JIT zygote mapping";

  int fd;
  palette_status_t status = PaletteAshmemCreateRegion(kRegionName, capacity, &fd);
  if (status != PALETTE_STATUS_OK) {
    CHECK_EQ(status, PALETTE_STATUS_CHECK_ERRNO);
    std::ostringstream oss;
    oss << "Failed to create zygote mapping: " << strerror(errno);
    *error_msg = oss.str();
    return -1;
  }
  return fd;
}

bool JitMemoryRegion::ProtectZygoteMemory(int fd, std::string* error_msg) {
  if (art::IsSealFutureWriteSupported()) {
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL | F_SEAL_FUTURE_WRITE)
            == -1) {
      std::ostringstream oss;
      oss << "Failed to protect zygote mapping: " << strerror(errno);
      *error_msg = oss.str();
      return false;
    }
  } else {
    palette_status_t status = PaletteAshmemSetProtRegion(fd, PROT_READ);
    if (status != PALETTE_STATUS_OK) {
      CHECK_EQ(status, PALETTE_STATUS_CHECK_ERRNO);
      std::ostringstream oss;
      oss << "Failed to protect zygote mapping: " << strerror(errno);
      *error_msg = oss.str();
      return false;
    }
  }
  return true;
}

#else

int JitMemoryRegion::CreateZygoteMemory(size_t capacity, std::string* error_msg) {
  // To simplify host building, we don't rely on the latest memfd features.
  LOG(WARNING) << "Returning un-sealable region on non-bionic";
  static const char* kRegionName = "/jit-zygote-cache";
  int fd = art::memfd_create(kRegionName, 0);
  if (fd == -1) {
    std::ostringstream oss;
    oss << "Failed to create zygote mapping: " << strerror(errno);
    *error_msg = oss.str();
    return -1;
  }
  if (ftruncate(fd, capacity) != 0) {
    std::ostringstream oss;
    oss << "Failed to create zygote mapping: " << strerror(errno);
    *error_msg = oss.str();
    return -1;
  }
  return fd;
}

bool JitMemoryRegion::ProtectZygoteMemory(int fd ATTRIBUTE_UNUSED,
                                          std::string* error_msg ATTRIBUTE_UNUSED) {
  return true;
}

#endif

}  // namespace jit
}  // namespace art
