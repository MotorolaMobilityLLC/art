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

#ifndef ART_SRC_MEM_MAP_H_
#define ART_SRC_MEM_MAP_H_

#include <sys/mman.h>

#include "utils.h"

namespace art {

// Used to keep track of mmap segments.
class MemMap {
 public:

  // Request an anonymous region of a specified length.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(size_t length, int prot) {
    return Map(NULL, length, prot);
  }

  // Request an anonymous region of a specified length and a requested base address.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(byte* addr, size_t length, int prot) {
    CHECK_NE(0U, length);
    CHECK_NE(0, prot);
    size_t page_aligned_size = RoundUp(length, kPageSize);
    byte* actual = reinterpret_cast<byte*>(mmap(addr,
                                                page_aligned_size,
                                                prot,
                                                MAP_PRIVATE | MAP_ANONYMOUS,
                                                -1,
                                                0));
    if (actual == MAP_FAILED) {
      PLOG(ERROR) << "mmap failed";
      return NULL;
    }
    return new MemMap(actual, length, actual, page_aligned_size);
  }

  // Map part of a file, taking care of non-page aligned offsets.  The
  // "start" offset is absolute, not relative.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(size_t length, int prot, int flags, int fd, off_t start) {
    return Map(NULL, length, prot, flags, fd, start);
  }

  // Map part of a file, taking care of non-page aligned offsets.  The
  // "start" offset is absolute, not relative. This version allows
  // requesting a specific address for the base of the mapping.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(byte* addr, size_t length, int prot, int flags, int fd, off_t start) {
    CHECK_NE(0U, length);
    CHECK_NE(0, prot);
    CHECK(flags & MAP_SHARED || flags & MAP_PRIVATE);
    // adjust to be page-aligned
    int page_offset = start % kPageSize;
    off_t page_aligned_offset = start - page_offset;
    size_t page_aligned_size = RoundUp(length + page_offset, kPageSize);
    byte* actual = reinterpret_cast<byte*>(mmap(addr,
                                                page_aligned_size,
                                                prot,
                                                flags,
                                                fd,
                                                page_aligned_offset));
    if (actual == MAP_FAILED) {
      PLOG(ERROR) << "mmap failed";
      return NULL;
    }
    return new MemMap(actual + page_offset, length, actual, page_aligned_size);
  }

  ~MemMap() {
    Unmap();
  }

  // Release a memory mapping, returning true on success or it was previously unmapped.
  bool Unmap() {
    if (base_addr_ == NULL && base_length_ == 0) {
      return true;
    }
    int result = munmap(base_addr_, base_length_);
    base_addr_ = NULL;
    base_length_ = 0;
    if (result == -1) {
      return false;
    }
    return true;
  }

  byte* GetAddress() const {
    return addr_;
  }

  size_t GetLength() const {
    return length_;
  }

  byte* GetLimit() const {
    return addr_ + length_;
  }

 private:
  MemMap(byte* addr, size_t length, void* base_addr, size_t base_length)
      : addr_(addr), length_(length), base_addr_(base_addr), base_length_(base_length) {
    CHECK(addr_ != NULL);
    CHECK(length_ != 0);
    CHECK(base_addr_ != NULL);
    CHECK(base_length_ != 0);
  };

  byte*  addr_;              // start of data
  size_t length_;            // length of data

  void*  base_addr_;         // page-aligned base address
  size_t base_length_;       // length of mapping
};

}  // namespace art

#endif  // ART_SRC_MEM_MAP_H_
