/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "elf_writer_quick.h"

#include <unordered_map>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_builder.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "elf_writer_debug.h"
#include "file_output_stream.h"
#include "globals.h"
#include "leb128.h"
#include "oat.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Create(File* elf_file,
                            OatWriter* oat_writer,
                            const std::vector<const DexFile*>& dex_files,
                            const std::string& android_root,
                            bool is_host,
                            const CompilerDriver& driver) {
  ElfWriterQuick elf_writer(driver, elf_file);
  return elf_writer.Write(oat_writer, dex_files, android_root, is_host);
}

class OatWriterWrapper FINAL : public CodeOutput {
 public:
  explicit OatWriterWrapper(OatWriter* oat_writer) : oat_writer_(oat_writer) {}

  void SetCodeOffset(size_t offset) {
    oat_writer_->SetOatDataOffset(offset);
  }
  bool Write(OutputStream* out) OVERRIDE {
    return oat_writer_->Write(out);
  }
 private:
  OatWriter* const oat_writer_;
};

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
static void WriteDebugSymbols(const CompilerDriver* compiler_driver,
                              ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                                         Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>* builder,
                              OatWriter* oat_writer);

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Write(OatWriter* oat_writer,
                           const std::vector<const DexFile*>& dex_files_unused ATTRIBUTE_UNUSED,
                           const std::string& android_root_unused ATTRIBUTE_UNUSED,
                           bool is_host_unused ATTRIBUTE_UNUSED) {
  constexpr bool debug = false;
  const OatHeader& oat_header = oat_writer->GetOatHeader();
  Elf_Word oat_data_size = oat_header.GetExecutableOffset();
  uint32_t oat_exec_size = oat_writer->GetSize() - oat_data_size;
  uint32_t oat_bss_size = oat_writer->GetBssSize();

  OatWriterWrapper wrapper(oat_writer);

  std::unique_ptr<ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                             Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr> > builder(
      new ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                     Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>(
          &wrapper,
          elf_file_,
          compiler_driver_->GetInstructionSet(),
          0,
          oat_data_size,
          oat_data_size,
          oat_exec_size,
          RoundUp(oat_data_size + oat_exec_size, kPageSize),
          oat_bss_size,
          compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols(),
          debug));

  if (!builder->Init()) {
    return false;
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols() &&
      !oat_writer->GetMethodDebugInfo().empty()) {
    WriteDebugSymbols(compiler_driver_, builder.get(), oat_writer);
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> oat_patches(
        ".oat_patches", SHT_OAT_PATCH, 0, NULL, 0, sizeof(uintptr_t), sizeof(uintptr_t));
    const std::vector<uintptr_t>& locations = oat_writer->GetAbsolutePatchLocations();
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(&locations[0]);
    const uint8_t* end = begin + locations.size() * sizeof(locations[0]);
    oat_patches.GetBuffer()->assign(begin, end);
    if (debug) {
      LOG(INFO) << "Prepared .oat_patches for " << locations.size() << " patches.";
    }
    builder->RegisterRawSection(oat_patches);
  }

  return builder->Write();
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
// Do not inline to avoid Clang stack frame problems. b/18738594
NO_INLINE
static void WriteDebugSymbols(const CompilerDriver* compiler_driver,
                              ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                                         Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>* builder,
                              OatWriter* oat_writer) {
  // Iterate over the compiled methods.
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetMethodDebugInfo();
  ElfSymtabBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Sym, Elf_Shdr>* symtab =
      builder->GetSymtabBuilder();
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    std::string name = PrettyMethod(it->dex_method_index_, *it->dex_file_, true);
    if (it->deduped_) {
      // TODO We should place the DEDUPED tag on the first instance of a deduplicated symbol
      // so that it will show up in a debuggerd crash report.
      name += " [ DEDUPED ]";
    }

    uint32_t low_pc = it->low_pc_;
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    low_pc += it->compiled_method_->CodeDelta();
    symtab->AddSymbol(name, &builder->GetTextBuilder(), low_pc,
                      true, it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    if (it->compiled_method_->GetInstructionSet() == kThumb2) {
      symtab->AddSymbol("$t", &builder->GetTextBuilder(), it->low_pc_ & ~1, true,
                        0, STB_LOCAL, STT_NOTYPE);
    }
  }

  typedef ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> Section;
  Section eh_frame(".eh_frame", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4, 0);
  Section debug_info(".debug_info", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  Section debug_abbrev(".debug_abbrev", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  Section debug_str(".debug_str", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  Section debug_line(".debug_line", SHT_PROGBITS, 0, nullptr, 0, 1, 0);

  dwarf::WriteDebugSections(compiler_driver,
                            oat_writer,
                            builder->GetTextBuilder().GetSection()->sh_addr,
                            eh_frame.GetBuffer(),
                            debug_info.GetBuffer(),
                            debug_abbrev.GetBuffer(),
                            debug_str.GetBuffer(),
                            debug_line.GetBuffer());

  builder->RegisterRawSection(eh_frame);
  builder->RegisterRawSection(debug_info);
  builder->RegisterRawSection(debug_abbrev);
  builder->RegisterRawSection(debug_str);
  builder->RegisterRawSection(debug_line);
}

// Explicit instantiations
template class ElfWriterQuick<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                              Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr>;
template class ElfWriterQuick<Elf64_Word, Elf64_Sword, Elf64_Addr, Elf64_Dyn,
                              Elf64_Sym, Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr>;

}  // namespace art
