// Copyright 2011 Google Inc. All Rights Reserved.

#include "compiled_method.h"

namespace art {

CompiledMethod::CompiledMethod(InstructionSet instruction_set,
                               const std::vector<uint16_t>& short_code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask,
                               const std::vector<uint32_t>& mapping_table,
                               const std::vector<uint16_t>& vmap_table)
    : instruction_set_(instruction_set), frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask) {
  CHECK_NE(short_code.size(), 0U);
  CHECK_GE(vmap_table.size(), 1U);  // should always contain an entry for LR
  CHECK_LE(vmap_table.size(), (1U << 16) - 1); // length must fit in 2^16-1

  size_t code_byte_count = short_code.size() * sizeof(short_code[0]);
  std::vector<uint8_t> byte_code(code_byte_count);
  memcpy(&byte_code[0], &short_code[0], code_byte_count);

  std::vector<uint32_t> length_prefixed_mapping_table;
  length_prefixed_mapping_table.push_back(mapping_table.size());
  length_prefixed_mapping_table.insert(length_prefixed_mapping_table.end(),
                                       mapping_table.begin(),
                                       mapping_table.end());
  DCHECK_EQ(mapping_table.size() + 1, length_prefixed_mapping_table.size());

  std::vector<uint16_t> length_prefixed_vmap_table;
  length_prefixed_vmap_table.push_back(vmap_table.size());
  length_prefixed_vmap_table.insert(length_prefixed_vmap_table.end(),
                                    vmap_table.begin(),
                                    vmap_table.end());
  DCHECK_EQ(vmap_table.size() + 1, length_prefixed_vmap_table.size());
  DCHECK_EQ(vmap_table.size(), length_prefixed_vmap_table[0]);

  code_ = byte_code;
  mapping_table_ = length_prefixed_mapping_table;
  vmap_table_ = length_prefixed_vmap_table;

  DCHECK_EQ(vmap_table_[0], static_cast<uint32_t>(__builtin_popcount(core_spill_mask) + __builtin_popcount(fp_spill_mask)));
}

void CompiledMethod::SetGcMap(const std::vector<uint8_t>& gc_map) {
  CHECK_NE(gc_map.size(), 0U);

  // Should only be used with CompiledMethods created with oatCompileMethod
  CHECK_NE(mapping_table_.size(), 0U);
  CHECK_NE(vmap_table_.size(), 0U);

  gc_map_ = gc_map;
}

CompiledMethod::CompiledMethod(InstructionSet instruction_set,
                               const std::vector<uint8_t>& code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask)
    : instruction_set_(instruction_set), code_(code), frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask) {
  CHECK_NE(code.size(), 0U);
}

CompiledMethod::~CompiledMethod() {}

InstructionSet CompiledMethod::GetInstructionSet() const {
  return instruction_set_;
}

const std::vector<uint8_t>& CompiledMethod::GetCode() const {
  return code_;
}

size_t CompiledMethod::GetFrameSizeInBytes() const {
  return frame_size_in_bytes_;
}

uint32_t CompiledMethod::GetCoreSpillMask() const {
  return core_spill_mask_;
}

uint32_t CompiledMethod::GetFpSpillMask() const {
  return fp_spill_mask_;
}

const std::vector<uint32_t>& CompiledMethod::GetMappingTable() const {
  return mapping_table_;
}

const std::vector<uint16_t>& CompiledMethod::GetVmapTable() const {
  return vmap_table_;
}

const std::vector<uint8_t>& CompiledMethod::GetGcMap() const {
  return gc_map_;
}

uint32_t CompiledMethod::AlignCode(uint32_t offset) const {
  return AlignCode(offset, instruction_set_);
}

uint32_t CompiledMethod::AlignCode(uint32_t offset, InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return RoundUp(offset, kArmAlignment);
    case kX86:
      return offset;
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << static_cast<int>(instruction_set);
      return 0;
  }
}

size_t CompiledMethod::CodeDelta() const {
  switch (instruction_set_) {
    case kArm:
    case kX86:
      return 0;
    case kThumb2: {
      // +1 to set the low-order bit so a BLX will switch to Thumb mode
      return 1;
    }
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << static_cast<int>(instruction_set_);
      return 0;
  }
}

const void* CompiledMethod::CodePointer(const void* code_pointer,
                                        InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kX86:
      return code_pointer;
    case kThumb2: {
      uintptr_t address = reinterpret_cast<uintptr_t>(code_pointer);
      // Set the low-order bit so a BLX will switch to Thumb mode
      address |= 0x1;
      return reinterpret_cast<const void*>(address);
    }
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << static_cast<int>(instruction_set);
      return NULL;
  }
}

CompiledInvokeStub::CompiledInvokeStub(std::vector<uint8_t>& code) {
  CHECK_NE(code.size(), 0U);
  code_ = code;
}

CompiledInvokeStub::~CompiledInvokeStub() {}

const std::vector<uint8_t>& CompiledInvokeStub::GetCode() const {
  return code_;
}

}  // namespace art
