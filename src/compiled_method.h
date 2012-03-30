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

#ifndef ART_SRC_COMPILED_METHOD_H_
#define ART_SRC_COMPILED_METHOD_H_

#include <vector>

#include "instruction_set.h"
#include "utils.h"

namespace llvm {
  class Function;
}

namespace art {

class CompiledMethod {
 public:
  // Constructs a CompiledMethod for the non-LLVM compilers.
  CompiledMethod(InstructionSet instruction_set,
                 const std::vector<uint8_t>& code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 const std::vector<uint32_t>& mapping_table,
                 const std::vector<uint16_t>& vmap_table);

  // Sets the GC map for a CompiledMethod.
  void SetGcMap(const std::vector<uint8_t>& gc_map);

  // Constructs a CompiledMethod for the JniCompiler.
  CompiledMethod(InstructionSet instruction_set,
                 const std::vector<uint8_t>& code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask);

  // Constructs a CompiledMethod for the LLVM compiler.
  CompiledMethod(InstructionSet instruction_set, size_t elf_idx);

  ~CompiledMethod();

  InstructionSet GetInstructionSet() const;
  const std::vector<uint8_t>& GetCode() const;
  size_t GetFrameSizeInBytes() const;
  uint32_t GetCoreSpillMask() const;
  uint32_t GetFpSpillMask() const;
  const std::vector<uint32_t>& GetMappingTable() const;
  const std::vector<uint16_t>& GetVmapTable() const;
  const std::vector<uint8_t>& GetGcMap() const;

  // Aligns an offset from a page aligned value to make it suitable
  // for code storage. important to ensure that PC relative value
  // computations work out as expected on ARM.
  uint32_t AlignCode(uint32_t offset) const;
  static uint32_t AlignCode(uint32_t offset, InstructionSet instruction_set);

  // returns the difference between the code address and a usable PC.
  // mainly to cope with kThumb2 where the lower bit must be set.
  size_t CodeDelta() const;

  // Returns a pointer suitable for invoking the code at the argument
  // code_pointer address.  Mainly to cope with kThumb2 where the
  // lower bit must be set to indicate Thumb mode.
  static const void* CodePointer(const void* code_pointer,
                                 InstructionSet instruction_set);

  size_t GetElfIndex() const {
    return elf_idx_;
  }

  bool IsExecutableInElf() const {
    return (elf_idx_ != static_cast<size_t>(-1));
  }

 private:
  // For non-LLVM
  const InstructionSet instruction_set_;
  std::vector<uint8_t> code_;
  const size_t frame_size_in_bytes_;
  const uint32_t core_spill_mask_;
  const uint32_t fp_spill_mask_;
  std::vector<uint32_t> mapping_table_;
  std::vector<uint16_t> vmap_table_;
  std::vector<uint8_t> gc_map_;
  // For LLVM
  size_t elf_idx_;
};

class CompiledInvokeStub {
 public:
  explicit CompiledInvokeStub(std::vector<uint8_t>& code);
#if defined(ART_USE_LLVM_COMPILER)
  explicit CompiledInvokeStub(size_t elf_idx);
#endif
  ~CompiledInvokeStub();

  const std::vector<uint8_t>& GetCode() const;

  size_t GetElfIndex() const {
    return elf_idx_;
  }

  bool IsExecutableInElf() const {
    return (elf_idx_ != static_cast<size_t>(-1));
  }

 private:
  std::vector<uint8_t> code_;
  size_t elf_idx_;
};

}  // namespace art

#endif  // ART_SRC_COMPILED_METHOD_H_
