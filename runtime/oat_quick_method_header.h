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

#ifndef ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_
#define ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_

#include "arch/instruction_set.h"
#include "base/macros.h"
#include "quick/quick_method_frame_info.h"
#include "stack_map.h"
#include "utils.h"

namespace art {

class ArtMethod;

// OatQuickMethodHeader precedes the raw code chunk generated by the compiler.
class PACKED(4) OatQuickMethodHeader {
 public:
  OatQuickMethodHeader(uint32_t mapping_table_offset = 0U,
                       uint32_t vmap_table_offset = 0U,
                       uint32_t gc_map_offset = 0U,
                       uint32_t frame_size_in_bytes = 0U,
                       uint32_t core_spill_mask = 0U,
                       uint32_t fp_spill_mask = 0U,
                       uint32_t code_size = 0U);

  ~OatQuickMethodHeader();

  static OatQuickMethodHeader* FromCodePointer(const void* code_ptr) {
    uintptr_t code = reinterpret_cast<uintptr_t>(code_ptr);
    uintptr_t header = code - OFFSETOF_MEMBER(OatQuickMethodHeader, code_);
    DCHECK(IsAlignedParam(code, GetInstructionSetAlignment(kRuntimeISA)) ||
           IsAlignedParam(header, GetInstructionSetAlignment(kRuntimeISA)))
        << std::hex << code << " " << std::hex << header;
    return reinterpret_cast<OatQuickMethodHeader*>(header);
  }

  static OatQuickMethodHeader* FromEntryPoint(const void* entry_point) {
    return FromCodePointer(EntryPointToCodePointer(entry_point));
  }

  OatQuickMethodHeader& operator=(const OatQuickMethodHeader&) = default;

  uintptr_t NativeQuickPcOffset(const uintptr_t pc) const {
    return pc - reinterpret_cast<uintptr_t>(GetEntryPoint());
  }

  bool IsOptimized() const {
    return gc_map_offset_ == 0 && vmap_table_offset_ != 0;
  }

  CodeInfo GetOptimizedCodeInfo() const {
    DCHECK(IsOptimized());
    const void* data = reinterpret_cast<const void*>(code_ - vmap_table_offset_);
    return CodeInfo(data);
  }

  const uint8_t* GetCode() const {
    return code_;
  }

  const uint8_t* GetNativeGcMap() const {
    return (gc_map_offset_ == 0) ? nullptr : code_ - gc_map_offset_;
  }

  const uint8_t* GetMappingTable() const {
    return (mapping_table_offset_ == 0) ? nullptr : code_ - mapping_table_offset_;
  }

  const uint8_t* GetVmapTable() const {
    CHECK(!IsOptimized()) << "Unimplemented vmap table for optimizing compiler";
    return (vmap_table_offset_ == 0) ? nullptr : code_ - vmap_table_offset_;
  }

  bool Contains(uintptr_t pc) const {
    uintptr_t code_start = reinterpret_cast<uintptr_t>(code_);
    static_assert(kRuntimeISA != kThumb2, "kThumb2 cannot be a runtime ISA");
    if (kRuntimeISA == kArm) {
      // On Thumb-2, the pc is offset by one.
      code_start++;
    }
    return code_start <= pc && pc <= (code_start + code_size_);
  }

  const uint8_t* GetEntryPoint() const {
    // When the runtime architecture is ARM, `kRuntimeISA` is set to `kArm`
    // (not `kThumb2`), *but* we always generate code for the Thumb-2
    // instruction set anyway. Thumb-2 requires the entrypoint to be of
    // offset 1.
    static_assert(kRuntimeISA != kThumb2, "kThumb2 cannot be a runtime ISA");
    return (kRuntimeISA == kArm)
        ? reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code_) | 1)
        : code_;
  }

  template <bool kCheckFrameSize = true>
  uint32_t GetFrameSizeInBytes() const {
    uint32_t result = frame_info_.FrameSizeInBytes();
    if (kCheckFrameSize) {
      DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    }
    return result;
  }

  QuickMethodFrameInfo GetFrameInfo() const {
    return frame_info_;
  }

  uintptr_t ToNativeQuickPc(ArtMethod* method,
                            const uint32_t dex_pc,
                            bool is_for_catch_handler,
                            bool abort_on_failure = true) const;

  uint32_t ToDexPc(ArtMethod* method, const uintptr_t pc, bool abort_on_failure = true) const;

  // The offset in bytes from the start of the mapping table to the end of the header.
  uint32_t mapping_table_offset_;
  // The offset in bytes from the start of the vmap table to the end of the header.
  uint32_t vmap_table_offset_;
  // The offset in bytes from the start of the gc map to the end of the header.
  uint32_t gc_map_offset_;
  // The stack frame information.
  QuickMethodFrameInfo frame_info_;
  // The code size in bytes.
  uint32_t code_size_;
  // The actual code.
  uint8_t code_[0];
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_
