/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
#define ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_

#include "base/bit_vector.h"
#include "base/value_object.h"
#include "memory_region.h"
#include "stack_map.h"
#include "utils/growable_array.h"

namespace art {

/**
 * Collects and builds stack maps for a method. All the stack maps
 * for a method are placed in a CodeInfo object.
 */
class StackMapStream : public ValueObject {
 public:
  explicit StackMapStream(ArenaAllocator* allocator)
      : stack_maps_(allocator, 10),
        dex_register_maps_(allocator, 10 * 4),
        inline_infos_(allocator, 2),
        stack_mask_max_(-1),
        number_of_stack_maps_with_inline_info_(0) {}

  // Compute bytes needed to encode a mask with the given maximum element.
  static uint32_t StackMaskEncodingSize(int max_element) {
    int number_of_bits = max_element + 1;  // Need room for max element too.
    return RoundUp(number_of_bits, kBitsPerByte) / kBitsPerByte;
  }

  // See runtime/stack_map.h to know what these fields contain.
  struct StackMapEntry {
    uint32_t dex_pc;
    uint32_t native_pc_offset;
    uint32_t register_mask;
    BitVector* sp_mask;
    uint32_t num_dex_registers;
    uint8_t inlining_depth;
    size_t dex_register_maps_start_index;
    size_t inline_infos_start_index;
  };

  struct DexRegisterEntry {
    DexRegisterMap::LocationKind kind;
    int32_t value;
  };

  struct InlineInfoEntry {
    uint32_t method_index;
  };

  void AddStackMapEntry(uint32_t dex_pc,
                        uint32_t native_pc_offset,
                        uint32_t register_mask,
                        BitVector* sp_mask,
                        uint32_t num_dex_registers,
                        uint8_t inlining_depth) {
    StackMapEntry entry;
    entry.dex_pc = dex_pc;
    entry.native_pc_offset = native_pc_offset;
    entry.register_mask = register_mask;
    entry.sp_mask = sp_mask;
    entry.num_dex_registers = num_dex_registers;
    entry.inlining_depth = inlining_depth;
    entry.dex_register_maps_start_index = dex_register_maps_.Size();
    entry.inline_infos_start_index = inline_infos_.Size();
    stack_maps_.Add(entry);

    if (sp_mask != nullptr) {
      stack_mask_max_ = std::max(stack_mask_max_, sp_mask->GetHighestBitSet());
    }
    if (inlining_depth > 0) {
      number_of_stack_maps_with_inline_info_++;
    }
  }

  void AddDexRegisterEntry(DexRegisterMap::LocationKind kind, int32_t value) {
    DexRegisterEntry entry;
    entry.kind = kind;
    entry.value = value;
    dex_register_maps_.Add(entry);
  }

  void AddInlineInfoEntry(uint32_t method_index) {
    InlineInfoEntry entry;
    entry.method_index = method_index;
    inline_infos_.Add(entry);
  }

  size_t ComputeNeededSize() const {
    return CodeInfo::kFixedSize
        + ComputeStackMapSize()
        + ComputeDexRegisterMapSize()
        + ComputeInlineInfoSize();
  }

  size_t ComputeStackMapSize() const {
    return stack_maps_.Size() * (StackMap::kFixedSize + StackMaskEncodingSize(stack_mask_max_));
  }

  size_t ComputeDexRegisterMapSize() const {
    // We currently encode all dex register information per stack map.
    return stack_maps_.Size() * DexRegisterMap::kFixedSize
      // For each dex register entry.
      + (dex_register_maps_.Size() * DexRegisterMap::SingleEntrySize());
  }

  size_t ComputeInlineInfoSize() const {
    return inline_infos_.Size() * InlineInfo::SingleEntrySize()
      // For encoding the depth.
      + (number_of_stack_maps_with_inline_info_ * InlineInfo::kFixedSize);
  }

  size_t ComputeInlineInfoStart() const {
    return ComputeDexRegisterMapStart() + ComputeDexRegisterMapSize();
  }

  size_t ComputeDexRegisterMapStart() const {
    return CodeInfo::kFixedSize + ComputeStackMapSize();
  }

  void FillIn(MemoryRegion region) {
    CodeInfo code_info(region);
    code_info.SetOverallSize(region.size());

    size_t stack_mask_size = StackMaskEncodingSize(stack_mask_max_);
    uint8_t* memory_start = region.start();

    MemoryRegion dex_register_maps_region = region.Subregion(
      ComputeDexRegisterMapStart(),
      ComputeDexRegisterMapSize());

    MemoryRegion inline_infos_region = region.Subregion(
      ComputeInlineInfoStart(),
      ComputeInlineInfoSize());

    code_info.SetNumberOfStackMaps(stack_maps_.Size());
    code_info.SetStackMaskSize(stack_mask_size);

    uintptr_t next_dex_register_map_offset = 0;
    uintptr_t next_inline_info_offset = 0;
    for (size_t i = 0, e = stack_maps_.Size(); i < e; ++i) {
      StackMap stack_map = code_info.GetStackMapAt(i);
      StackMapEntry entry = stack_maps_.Get(i);

      stack_map.SetDexPc(entry.dex_pc);
      stack_map.SetNativePcOffset(entry.native_pc_offset);
      stack_map.SetRegisterMask(entry.register_mask);
      if (entry.sp_mask != nullptr) {
        stack_map.SetStackMask(*entry.sp_mask);
      }

      // Set the register map.
      MemoryRegion register_region = dex_register_maps_region.Subregion(
          next_dex_register_map_offset,
          DexRegisterMap::kFixedSize + entry.num_dex_registers * DexRegisterMap::SingleEntrySize());
      next_dex_register_map_offset += register_region.size();
      DexRegisterMap dex_register_map(register_region);
      stack_map.SetDexRegisterMapOffset(register_region.start() - memory_start);

      for (size_t j = 0; j < entry.num_dex_registers; ++j) {
        DexRegisterEntry register_entry =
            dex_register_maps_.Get(j + entry.dex_register_maps_start_index);
        dex_register_map.SetRegisterInfo(j, register_entry.kind, register_entry.value);
      }

      // Set the inlining info.
      if (entry.inlining_depth != 0) {
        MemoryRegion inline_region = inline_infos_region.Subregion(
            next_inline_info_offset,
            InlineInfo::kFixedSize + entry.inlining_depth * InlineInfo::SingleEntrySize());
        next_inline_info_offset += inline_region.size();
        InlineInfo inline_info(inline_region);

        stack_map.SetInlineDescriptorOffset(inline_region.start() - memory_start);

        inline_info.SetDepth(entry.inlining_depth);
        for (size_t j = 0; j < entry.inlining_depth; ++j) {
          InlineInfoEntry inline_entry = inline_infos_.Get(j + entry.inline_infos_start_index);
          inline_info.SetMethodReferenceIndexAtDepth(j, inline_entry.method_index);
        }
      } else {
        stack_map.SetInlineDescriptorOffset(InlineInfo::kNoInlineInfo);
      }
    }
  }

 private:
  GrowableArray<StackMapEntry> stack_maps_;
  GrowableArray<DexRegisterEntry> dex_register_maps_;
  GrowableArray<InlineInfoEntry> inline_infos_;
  int stack_mask_max_;
  size_t number_of_stack_maps_with_inline_info_;

  DISALLOW_COPY_AND_ASSIGN(StackMapStream);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
