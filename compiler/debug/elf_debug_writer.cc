/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "elf_debug_writer.h"

#include <type_traits>
#include <unordered_map>
#include <vector>

#include "base/array_ref.h"
#include "debug/dwarf/dwarf_constants.h"
#include "debug/elf_compilation_unit.h"
#include "debug/elf_debug_frame_writer.h"
#include "debug/elf_debug_info_writer.h"
#include "debug/elf_debug_line_writer.h"
#include "debug/elf_debug_loc_writer.h"
#include "debug/elf_symtab_writer.h"
#include "debug/method_debug_info.h"
#include "debug/xz_utils.h"
#include "elf.h"
#include "linker/elf_builder.h"
#include "linker/vector_output_stream.h"
#include "oat.h"

namespace art {
namespace debug {

using ElfRuntimeTypes = std::conditional<sizeof(void*) == 4, ElfTypes32, ElfTypes64>::type;

template <typename ElfTypes>
void WriteDebugInfo(linker::ElfBuilder<ElfTypes>* builder,
                    const DebugInfo& debug_info,
                    dwarf::CFIFormat cfi_format,
                    bool write_oat_patches) {
  // Write .strtab and .symtab.
  WriteDebugSymbols(builder, false /* mini-debug-info */, debug_info);

  // Write .debug_frame.
  WriteCFISection(builder, debug_info.compiled_methods, cfi_format, write_oat_patches);

  // Group the methods into compilation units based on class.
  std::unordered_map<const DexFile::ClassDef*, ElfCompilationUnit> class_to_compilation_unit;
  for (const MethodDebugInfo& mi : debug_info.compiled_methods) {
    if (mi.dex_file != nullptr) {
      auto& dex_class_def = mi.dex_file->GetClassDef(mi.class_def_index);
      ElfCompilationUnit& cu = class_to_compilation_unit[&dex_class_def];
      cu.methods.push_back(&mi);
      // All methods must have the same addressing mode otherwise the min/max below does not work.
      DCHECK_EQ(cu.methods.front()->is_code_address_text_relative, mi.is_code_address_text_relative);
      cu.is_code_address_text_relative = mi.is_code_address_text_relative;
      cu.code_address = std::min(cu.code_address, mi.code_address);
      cu.code_end = std::max(cu.code_end, mi.code_address + mi.code_size);
    }
  }

  // Sort compilation units to make the compiler output deterministic.
  std::vector<ElfCompilationUnit> compilation_units;
  compilation_units.reserve(class_to_compilation_unit.size());
  for (auto& it : class_to_compilation_unit) {
    // The .debug_line section requires the methods to be sorted by code address.
    std::stable_sort(it.second.methods.begin(),
                     it.second.methods.end(),
                     [](const MethodDebugInfo* a, const MethodDebugInfo* b) {
                         return a->code_address < b->code_address;
                     });
    compilation_units.push_back(std::move(it.second));
  }
  std::sort(compilation_units.begin(),
            compilation_units.end(),
            [](ElfCompilationUnit& a, ElfCompilationUnit& b) {
                // Sort by index of the first method within the method_infos array.
                // This assumes that the order of method_infos is deterministic.
                // Code address is not good for sorting due to possible duplicates.
                return a.methods.front() < b.methods.front();
            });

  // Write .debug_line section.
  if (!compilation_units.empty()) {
    ElfDebugLineWriter<ElfTypes> line_writer(builder);
    line_writer.Start();
    for (auto& compilation_unit : compilation_units) {
      line_writer.WriteCompilationUnit(compilation_unit);
    }
    line_writer.End(write_oat_patches);
  }

  // Write .debug_info section.
  if (!compilation_units.empty()) {
    ElfDebugInfoWriter<ElfTypes> info_writer(builder);
    info_writer.Start();
    for (const auto& compilation_unit : compilation_units) {
      ElfCompilationUnitWriter<ElfTypes> cu_writer(&info_writer);
      cu_writer.Write(compilation_unit);
    }
    info_writer.End(write_oat_patches);
  }
}

template <typename ElfTypes>
static std::vector<uint8_t> MakeMiniDebugInfoInternal(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    typename ElfTypes::Addr text_section_address,
    size_t text_section_size,
    typename ElfTypes::Addr dex_section_address,
    size_t dex_section_size,
    const DebugInfo& debug_info) {
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  linker::VectorOutputStream out("Mini-debug-info ELF file", &buffer);
  std::unique_ptr<linker::ElfBuilder<ElfTypes>> builder(
      new linker::ElfBuilder<ElfTypes>(isa, features, &out));
  builder->Start(false /* write_program_headers */);
  // Mirror ELF sections as NOBITS since the added symbols will reference them.
  builder->GetText()->AllocateVirtualMemory(text_section_address, text_section_size);
  if (dex_section_size != 0) {
    builder->GetDex()->AllocateVirtualMemory(dex_section_address, dex_section_size);
  }
  WriteDebugSymbols(builder.get(), true /* mini-debug-info */, debug_info);
  WriteCFISection(builder.get(),
                  debug_info.compiled_methods,
                  dwarf::DW_DEBUG_FRAME_FORMAT,
                  false /* write_oat_paches */);
  builder->End();
  CHECK(builder->Good());
  std::vector<uint8_t> compressed_buffer;
  compressed_buffer.reserve(buffer.size() / 4);
  XzCompress(ArrayRef<const uint8_t>(buffer), &compressed_buffer);
  return compressed_buffer;
}

std::vector<uint8_t> MakeMiniDebugInfo(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    uint64_t text_section_address,
    size_t text_section_size,
    uint64_t dex_section_address,
    size_t dex_section_size,
    const DebugInfo& debug_info) {
  if (Is64BitInstructionSet(isa)) {
    return MakeMiniDebugInfoInternal<ElfTypes64>(isa,
                                                 features,
                                                 text_section_address,
                                                 text_section_size,
                                                 dex_section_address,
                                                 dex_section_size,
                                                 debug_info);
  } else {
    return MakeMiniDebugInfoInternal<ElfTypes32>(isa,
                                                 features,
                                                 text_section_address,
                                                 text_section_size,
                                                 dex_section_address,
                                                 dex_section_size,
                                                 debug_info);
  }
}

std::vector<uint8_t> MakeElfFileForJIT(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    bool mini_debug_info,
    const MethodDebugInfo& method_info) {
  using ElfTypes = ElfRuntimeTypes;
  CHECK_EQ(sizeof(ElfTypes::Addr), static_cast<size_t>(GetInstructionSetPointerSize(isa)));
  CHECK_EQ(method_info.is_code_address_text_relative, false);
  DebugInfo debug_info{};
  debug_info.compiled_methods = ArrayRef<const MethodDebugInfo>(&method_info, 1);
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  linker::VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<linker::ElfBuilder<ElfTypes>> builder(
      new linker::ElfBuilder<ElfTypes>(isa, features, &out));
  // No program headers since the ELF file is not linked and has no allocated sections.
  builder->Start(false /* write_program_headers */);
  builder->GetText()->AllocateVirtualMemory(method_info.code_address, method_info.code_size);
  if (mini_debug_info) {
    // The compression is great help for multiple methods but it is not worth it for a
    // single method due to the overheads so skip the compression here for performance.
    WriteDebugSymbols(builder.get(), true /* mini-debug-info */, debug_info);
    WriteCFISection(builder.get(),
                    debug_info.compiled_methods,
                    dwarf::DW_DEBUG_FRAME_FORMAT,
                    false /* write_oat_paches */);
  } else {
    WriteDebugInfo(builder.get(),
                   debug_info,
                   dwarf::DW_DEBUG_FRAME_FORMAT,
                   false /* write_oat_patches */);
  }
  builder->End();
  CHECK(builder->Good());
  return buffer;
}

std::vector<uint8_t> WriteDebugElfFileForClasses(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    const ArrayRef<mirror::Class*>& types)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  using ElfTypes = ElfRuntimeTypes;
  CHECK_EQ(sizeof(ElfTypes::Addr), static_cast<size_t>(GetInstructionSetPointerSize(isa)));
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  linker::VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<linker::ElfBuilder<ElfTypes>> builder(
      new linker::ElfBuilder<ElfTypes>(isa, features, &out));
  // No program headers since the ELF file is not linked and has no allocated sections.
  builder->Start(false /* write_program_headers */);
  ElfDebugInfoWriter<ElfTypes> info_writer(builder.get());
  info_writer.Start();
  ElfCompilationUnitWriter<ElfTypes> cu_writer(&info_writer);
  cu_writer.Write(types);
  info_writer.End(false /* write_oat_patches */);

  builder->End();
  CHECK(builder->Good());
  return buffer;
}

// Explicit instantiations
template void WriteDebugInfo<ElfTypes32>(
    linker::ElfBuilder<ElfTypes32>* builder,
    const DebugInfo& debug_info,
    dwarf::CFIFormat cfi_format,
    bool write_oat_patches);
template void WriteDebugInfo<ElfTypes64>(
    linker::ElfBuilder<ElfTypes64>* builder,
    const DebugInfo& debug_info,
    dwarf::CFIFormat cfi_format,
    bool write_oat_patches);

}  // namespace debug
}  // namespace art
