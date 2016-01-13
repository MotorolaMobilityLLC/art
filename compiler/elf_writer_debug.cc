/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "elf_writer_debug.h"

#include <unordered_set>
#include <vector>

#include "base/casts.h"
#include "base/stl_util.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "dwarf/dedup_vector.h"
#include "dwarf/headers.h"
#include "dwarf/method_debug_info.h"
#include "dwarf/register.h"
#include "elf_builder.h"
#include "linker/vector_output_stream.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/class.h"
#include "oat_writer.h"
#include "stack_map.h"
#include "utils.h"

namespace art {
namespace dwarf {

// The ARM specification defines three special mapping symbols
// $a, $t and $d which mark ARM, Thumb and data ranges respectively.
// These symbols can be used by tools, for example, to pretty
// print instructions correctly.  Objdump will use them if they
// exist, but it will still work well without them.
// However, these extra symbols take space, so let's just generate
// one symbol which marks the whole .text section as code.
constexpr bool kGenerateSingleArmMappingSymbol = true;

static Reg GetDwarfCoreReg(InstructionSet isa, int machine_reg) {
  switch (isa) {
    case kArm:
    case kThumb2:
      return Reg::ArmCore(machine_reg);
    case kArm64:
      return Reg::Arm64Core(machine_reg);
    case kX86:
      return Reg::X86Core(machine_reg);
    case kX86_64:
      return Reg::X86_64Core(machine_reg);
    case kMips:
      return Reg::MipsCore(machine_reg);
    case kMips64:
      return Reg::Mips64Core(machine_reg);
    default:
      LOG(FATAL) << "Unknown instruction set: " << isa;
      UNREACHABLE();
  }
}

static Reg GetDwarfFpReg(InstructionSet isa, int machine_reg) {
  switch (isa) {
    case kArm:
    case kThumb2:
      return Reg::ArmFp(machine_reg);
    case kArm64:
      return Reg::Arm64Fp(machine_reg);
    case kX86:
      return Reg::X86Fp(machine_reg);
    case kX86_64:
      return Reg::X86_64Fp(machine_reg);
    default:
      LOG(FATAL) << "Unknown instruction set: " << isa;
      UNREACHABLE();
  }
}

static void WriteCIE(InstructionSet isa,
                     CFIFormat format,
                     std::vector<uint8_t>* buffer) {
  // Scratch registers should be marked as undefined.  This tells the
  // debugger that its value in the previous frame is not recoverable.
  bool is64bit = Is64BitInstructionSet(isa);
  switch (isa) {
    case kArm:
    case kThumb2: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::ArmCore(13), 0);  // R13(SP).
      // core registers.
      for (int reg = 0; reg < 13; reg++) {
        if (reg < 4 || reg == 12) {
          opcodes.Undefined(Reg::ArmCore(reg));
        } else {
          opcodes.SameValue(Reg::ArmCore(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 16) {
          opcodes.Undefined(Reg::ArmFp(reg));
        } else {
          opcodes.SameValue(Reg::ArmFp(reg));
        }
      }
      auto return_reg = Reg::ArmCore(14);  // R14(LR).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kArm64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::Arm64Core(31), 0);  // R31(SP).
      // core registers.
      for (int reg = 0; reg < 30; reg++) {
        if (reg < 8 || reg == 16 || reg == 17) {
          opcodes.Undefined(Reg::Arm64Core(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Core(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 8 || reg >= 16) {
          opcodes.Undefined(Reg::Arm64Fp(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Fp(reg));
        }
      }
      auto return_reg = Reg::Arm64Core(30);  // R30(LR).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kMips:
    case kMips64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::MipsCore(29), 0);  // R29(SP).
      // core registers.
      for (int reg = 1; reg < 26; reg++) {
        if (reg < 16 || reg == 24 || reg == 25) {  // AT, V*, A*, T*.
          opcodes.Undefined(Reg::MipsCore(reg));
        } else {
          opcodes.SameValue(Reg::MipsCore(reg));
        }
      }
      auto return_reg = Reg::MipsCore(31);  // R31(RA).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kX86: {
      // FIXME: Add fp registers once libunwind adds support for them. Bug: 20491296
      constexpr bool generate_opcodes_for_x86_fp = false;
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86Core(4), 4);   // R4(ESP).
      opcodes.Offset(Reg::X86Core(8), -4);  // R8(EIP).
      // core registers.
      for (int reg = 0; reg < 8; reg++) {
        if (reg <= 3) {
          opcodes.Undefined(Reg::X86Core(reg));
        } else if (reg == 4) {
          // Stack pointer.
        } else {
          opcodes.SameValue(Reg::X86Core(reg));
        }
      }
      // fp registers.
      if (generate_opcodes_for_x86_fp) {
        for (int reg = 0; reg < 8; reg++) {
          opcodes.Undefined(Reg::X86Fp(reg));
        }
      }
      auto return_reg = Reg::X86Core(8);  // R8(EIP).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kX86_64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86_64Core(4), 8);  // R4(RSP).
      opcodes.Offset(Reg::X86_64Core(16), -8);  // R16(RIP).
      // core registers.
      for (int reg = 0; reg < 16; reg++) {
        if (reg == 4) {
          // Stack pointer.
        } else if (reg < 12 && reg != 3 && reg != 5) {  // except EBX and EBP.
          opcodes.Undefined(Reg::X86_64Core(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Core(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 16; reg++) {
        if (reg < 12) {
          opcodes.Undefined(Reg::X86_64Fp(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Fp(reg));
        }
      }
      auto return_reg = Reg::X86_64Core(16);  // R16(RIP).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kNone:
      break;
  }
  LOG(FATAL) << "Can not write CIE frame for ISA " << isa;
  UNREACHABLE();
}

template<typename ElfTypes>
void WriteCFISection(ElfBuilder<ElfTypes>* builder,
                     const ArrayRef<const MethodDebugInfo>& method_infos,
                     CFIFormat format) {
  CHECK(format == DW_DEBUG_FRAME_FORMAT || format == DW_EH_FRAME_FORMAT);
  typedef typename ElfTypes::Addr Elf_Addr;

  if (method_infos.empty()) {
    return;
  }

  std::vector<uint32_t> binary_search_table;
  std::vector<uintptr_t> patch_locations;
  if (format == DW_EH_FRAME_FORMAT) {
    binary_search_table.reserve(2 * method_infos.size());
  } else {
    patch_locations.reserve(method_infos.size());
  }

  // Write .eh_frame/.debug_frame section.
  auto* cfi_section = (format == DW_DEBUG_FRAME_FORMAT
                       ? builder->GetDebugFrame()
                       : builder->GetEhFrame());
  {
    cfi_section->Start();
    const bool is64bit = Is64BitInstructionSet(builder->GetIsa());
    const Elf_Addr text_address = builder->GetText()->Exists()
        ? builder->GetText()->GetAddress()
        : 0;
    const Elf_Addr cfi_address = cfi_section->GetAddress();
    const Elf_Addr cie_address = cfi_address;
    Elf_Addr buffer_address = cfi_address;
    std::vector<uint8_t> buffer;  // Small temporary buffer.
    WriteCIE(builder->GetIsa(), format, &buffer);
    cfi_section->WriteFully(buffer.data(), buffer.size());
    buffer_address += buffer.size();
    buffer.clear();
    for (const MethodDebugInfo& mi : method_infos) {
      if (!mi.deduped_) {  // Only one FDE per unique address.
        ArrayRef<const uint8_t> opcodes = mi.compiled_method_->GetCFIInfo();
        if (!opcodes.empty()) {
          const Elf_Addr code_address = text_address + mi.low_pc_;
          if (format == DW_EH_FRAME_FORMAT) {
            binary_search_table.push_back(
                dchecked_integral_cast<uint32_t>(code_address));
            binary_search_table.push_back(
                dchecked_integral_cast<uint32_t>(buffer_address));
          }
          WriteFDE(is64bit, cfi_address, cie_address,
                   code_address, mi.high_pc_ - mi.low_pc_,
                   opcodes, format, buffer_address, &buffer,
                   &patch_locations);
          cfi_section->WriteFully(buffer.data(), buffer.size());
          buffer_address += buffer.size();
          buffer.clear();
        }
      }
    }
    cfi_section->End();
  }

  if (format == DW_EH_FRAME_FORMAT) {
    auto* header_section = builder->GetEhFrameHdr();
    header_section->Start();
    uint32_t header_address = dchecked_integral_cast<int32_t>(header_section->GetAddress());
    // Write .eh_frame_hdr section.
    std::vector<uint8_t> buffer;
    Writer<> header(&buffer);
    header.PushUint8(1);  // Version.
    // Encoding of .eh_frame pointer - libunwind does not honor datarel here,
    // so we have to use pcrel which means relative to the pointer's location.
    header.PushUint8(DW_EH_PE_pcrel | DW_EH_PE_sdata4);
    // Encoding of binary search table size.
    header.PushUint8(DW_EH_PE_udata4);
    // Encoding of binary search table addresses - libunwind supports only this
    // specific combination, which means relative to the start of .eh_frame_hdr.
    header.PushUint8(DW_EH_PE_datarel | DW_EH_PE_sdata4);
    // .eh_frame pointer
    header.PushInt32(cfi_section->GetAddress() - (header_address + 4u));
    // Binary search table size (number of entries).
    header.PushUint32(dchecked_integral_cast<uint32_t>(binary_search_table.size()/2));
    header_section->WriteFully(buffer.data(), buffer.size());
    // Binary search table.
    for (size_t i = 0; i < binary_search_table.size(); i++) {
      // Make addresses section-relative since we know the header address now.
      binary_search_table[i] -= header_address;
    }
    header_section->WriteFully(binary_search_table.data(), binary_search_table.size());
    header_section->End();
  } else {
    builder->WritePatches(".debug_frame.oat_patches",
                          ArrayRef<const uintptr_t>(patch_locations));
  }
}

namespace {
  struct CompilationUnit {
    std::vector<const MethodDebugInfo*> methods_;
    size_t debug_line_offset_ = 0;
    uintptr_t low_pc_ = std::numeric_limits<uintptr_t>::max();
    uintptr_t high_pc_ = 0;
  };

  typedef std::vector<DexFile::LocalInfo> LocalInfos;

  void LocalInfoCallback(void* ctx, const DexFile::LocalInfo& entry) {
    static_cast<LocalInfos*>(ctx)->push_back(entry);
  }

  typedef std::vector<DexFile::PositionInfo> PositionInfos;

  bool PositionInfoCallback(void* ctx, const DexFile::PositionInfo& entry) {
    static_cast<PositionInfos*>(ctx)->push_back(entry);
    return false;
  }

  std::vector<const char*> GetParamNames(const MethodDebugInfo* mi) {
    std::vector<const char*> names;
    if (mi->code_item_ != nullptr) {
      const uint8_t* stream = mi->dex_file_->GetDebugInfoStream(mi->code_item_);
      if (stream != nullptr) {
        DecodeUnsignedLeb128(&stream);  // line.
        uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
        for (uint32_t i = 0; i < parameters_size; ++i) {
          uint32_t id = DecodeUnsignedLeb128P1(&stream);
          names.push_back(mi->dex_file_->StringDataByIdx(id));
        }
      }
    }
    return names;
  }

  struct VariableLocation {
    uint32_t low_pc;
    uint32_t high_pc;
    DexRegisterLocation reg_lo;  // May be None if the location is unknown.
    DexRegisterLocation reg_hi;  // Most significant bits of 64-bit value.
  };

  // Get the location of given dex register (e.g. stack or machine register).
  // Note that the location might be different based on the current pc.
  // The result will cover all ranges where the variable is in scope.
  std::vector<VariableLocation> GetVariableLocations(const MethodDebugInfo* method_info,
                                                     uint16_t vreg,
                                                     bool is64bitValue,
                                                     uint32_t dex_pc_low,
                                                     uint32_t dex_pc_high) {
    std::vector<VariableLocation> variable_locations;

    // Get stack maps sorted by pc (they might not be sorted internally).
    const CodeInfo code_info(method_info->compiled_method_->GetVmapTable().data());
    const StackMapEncoding encoding = code_info.ExtractEncoding();
    std::map<uint32_t, StackMap> stack_maps;
    for (uint32_t s = 0; s < code_info.GetNumberOfStackMaps(); s++) {
      StackMap stack_map = code_info.GetStackMapAt(s, encoding);
      DCHECK(stack_map.IsValid());
      const uint32_t low_pc = method_info->low_pc_ + stack_map.GetNativePcOffset(encoding);
      DCHECK_LE(low_pc, method_info->high_pc_);
      stack_maps.emplace(low_pc, stack_map);
    }

    // Create entries for the requested register based on stack map data.
    for (auto it = stack_maps.begin(); it != stack_maps.end(); it++) {
      const StackMap& stack_map = it->second;
      const uint32_t low_pc = it->first;
      auto next_it = it;
      next_it++;
      const uint32_t high_pc = next_it != stack_maps.end() ? next_it->first
                                                           : method_info->high_pc_;
      DCHECK_LE(low_pc, high_pc);
      if (low_pc == high_pc) {
        continue;  // Ignore if the address range is empty.
      }

      // Check that the stack map is in the requested range.
      uint32_t dex_pc = stack_map.GetDexPc(encoding);
      if (!(dex_pc_low <= dex_pc && dex_pc < dex_pc_high)) {
        continue;
      }

      // Find the location of the dex register.
      DexRegisterLocation reg_lo = DexRegisterLocation::None();
      DexRegisterLocation reg_hi = DexRegisterLocation::None();
      if (stack_map.HasDexRegisterMap(encoding)) {
        DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(
            stack_map, encoding, method_info->code_item_->registers_size_);
        reg_lo = dex_register_map.GetDexRegisterLocation(
            vreg, method_info->code_item_->registers_size_, code_info, encoding);
        if (is64bitValue) {
          reg_hi = dex_register_map.GetDexRegisterLocation(
              vreg + 1, method_info->code_item_->registers_size_, code_info, encoding);
        }
      }

      // Add location entry for this address range.
      if (!variable_locations.empty() &&
          variable_locations.back().reg_lo == reg_lo &&
          variable_locations.back().reg_hi == reg_hi &&
          variable_locations.back().high_pc == low_pc) {
        // Merge with the previous entry (extend its range).
        variable_locations.back().high_pc = high_pc;
      } else {
        variable_locations.push_back({low_pc, high_pc, reg_lo, reg_hi});
      }
    }

    return variable_locations;
  }

  bool IsFromOptimizingCompiler(const MethodDebugInfo* method_info) {
    return method_info->compiled_method_->GetQuickCode().size() > 0 &&
           method_info->compiled_method_->GetVmapTable().size() > 0 &&
           method_info->compiled_method_->GetGcMap().size() == 0 &&
           method_info->code_item_ != nullptr;
  }
}  // namespace

// Helper class to write .debug_info and its supporting sections.
template<typename ElfTypes>
class DebugInfoWriter {
  typedef typename ElfTypes::Addr Elf_Addr;

  // Helper class to write one compilation unit.
  // It holds helper methods and temporary state.
  class CompilationUnitWriter {
   public:
    explicit CompilationUnitWriter(DebugInfoWriter* owner)
      : owner_(owner),
        info_(Is64BitInstructionSet(owner_->builder_->GetIsa()), &debug_abbrev_) {
    }

    void Write(const CompilationUnit& compilation_unit) {
      CHECK(!compilation_unit.methods_.empty());
      const Elf_Addr text_address = owner_->builder_->GetText()->Exists()
          ? owner_->builder_->GetText()->GetAddress()
          : 0;
      const uintptr_t cu_size = compilation_unit.high_pc_ - compilation_unit.low_pc_;

      info_.StartTag(DW_TAG_compile_unit);
      info_.WriteStrp(DW_AT_producer, owner_->WriteString("Android dex2oat"));
      info_.WriteData1(DW_AT_language, DW_LANG_Java);
      info_.WriteStrp(DW_AT_comp_dir, owner_->WriteString("$JAVA_SRC_ROOT"));
      info_.WriteAddr(DW_AT_low_pc, text_address + compilation_unit.low_pc_);
      info_.WriteUdata(DW_AT_high_pc, dchecked_integral_cast<uint32_t>(cu_size));
      info_.WriteSecOffset(DW_AT_stmt_list, compilation_unit.debug_line_offset_);

      const char* last_dex_class_desc = nullptr;
      for (auto mi : compilation_unit.methods_) {
        const DexFile* dex = mi->dex_file_;
        const DexFile::CodeItem* dex_code = mi->code_item_;
        const DexFile::MethodId& dex_method = dex->GetMethodId(mi->dex_method_index_);
        const DexFile::ProtoId& dex_proto = dex->GetMethodPrototype(dex_method);
        const DexFile::TypeList* dex_params = dex->GetProtoParameters(dex_proto);
        const char* dex_class_desc = dex->GetMethodDeclaringClassDescriptor(dex_method);
        const bool is_static = (mi->access_flags_ & kAccStatic) != 0;

        // Enclose the method in correct class definition.
        if (last_dex_class_desc != dex_class_desc) {
          if (last_dex_class_desc != nullptr) {
            EndClassTag(last_dex_class_desc);
          }
          // Write reference tag for the class we are about to declare.
          size_t reference_tag_offset = info_.StartTag(DW_TAG_reference_type);
          type_cache_.emplace(std::string(dex_class_desc), reference_tag_offset);
          size_t type_attrib_offset = info_.size();
          info_.WriteRef4(DW_AT_type, 0);
          info_.EndTag();
          // Declare the class that owns this method.
          size_t class_offset = StartClassTag(dex_class_desc);
          info_.UpdateUint32(type_attrib_offset, class_offset);
          info_.WriteFlag(DW_AT_declaration, true);
          // Check that each class is defined only once.
          bool unique = owner_->defined_dex_classes_.insert(dex_class_desc).second;
          CHECK(unique) << "Redefinition of " << dex_class_desc;
          last_dex_class_desc = dex_class_desc;
        }

        int start_depth = info_.Depth();
        info_.StartTag(DW_TAG_subprogram);
        WriteName(dex->GetMethodName(dex_method));
        info_.WriteAddr(DW_AT_low_pc, text_address + mi->low_pc_);
        info_.WriteUdata(DW_AT_high_pc, dchecked_integral_cast<uint32_t>(mi->high_pc_-mi->low_pc_));
        uint8_t frame_base[] = { DW_OP_call_frame_cfa };
        info_.WriteExprLoc(DW_AT_frame_base, &frame_base, sizeof(frame_base));
        WriteLazyType(dex->GetReturnTypeDescriptor(dex_proto));

        // Write parameters. DecodeDebugLocalInfo returns them as well, but it does not
        // guarantee order or uniqueness so it is safer to iterate over them manually.
        // DecodeDebugLocalInfo might not also be available if there is no debug info.
        std::vector<const char*> param_names = GetParamNames(mi);
        uint32_t arg_reg = 0;
        if (!is_static) {
          info_.StartTag(DW_TAG_formal_parameter);
          WriteName("this");
          info_.WriteFlag(DW_AT_artificial, true);
          WriteLazyType(dex_class_desc);
          if (dex_code != nullptr) {
            // Write the stack location of the parameter.
            const uint32_t vreg = dex_code->registers_size_ - dex_code->ins_size_ + arg_reg;
            const bool is64bitValue = false;
            WriteRegLocation(mi, vreg, is64bitValue, compilation_unit.low_pc_);
          }
          arg_reg++;
          info_.EndTag();
        }
        if (dex_params != nullptr) {
          for (uint32_t i = 0; i < dex_params->Size(); ++i) {
            info_.StartTag(DW_TAG_formal_parameter);
            // Parameter names may not be always available.
            if (i < param_names.size()) {
              WriteName(param_names[i]);
            }
            // Write the type.
            const char* type_desc = dex->StringByTypeIdx(dex_params->GetTypeItem(i).type_idx_);
            WriteLazyType(type_desc);
            const bool is64bitValue = type_desc[0] == 'D' || type_desc[0] == 'J';
            if (dex_code != nullptr) {
              // Write the stack location of the parameter.
              const uint32_t vreg = dex_code->registers_size_ - dex_code->ins_size_ + arg_reg;
              WriteRegLocation(mi, vreg, is64bitValue, compilation_unit.low_pc_);
            }
            arg_reg += is64bitValue ? 2 : 1;
            info_.EndTag();
          }
          if (dex_code != nullptr) {
            DCHECK_EQ(arg_reg, dex_code->ins_size_);
          }
        }

        // Write local variables.
        LocalInfos local_infos;
        if (dex->DecodeDebugLocalInfo(dex_code,
                                      is_static,
                                      mi->dex_method_index_,
                                      LocalInfoCallback,
                                      &local_infos)) {
          for (const DexFile::LocalInfo& var : local_infos) {
            if (var.reg_ < dex_code->registers_size_ - dex_code->ins_size_) {
              info_.StartTag(DW_TAG_variable);
              WriteName(var.name_);
              WriteLazyType(var.descriptor_);
              bool is64bitValue = var.descriptor_[0] == 'D' || var.descriptor_[0] == 'J';
              WriteRegLocation(mi, var.reg_, is64bitValue, compilation_unit.low_pc_,
                               var.start_address_, var.end_address_);
              info_.EndTag();
            }
          }
        }

        info_.EndTag();
        CHECK_EQ(info_.Depth(), start_depth);  // Balanced start/end.
      }
      if (last_dex_class_desc != nullptr) {
        EndClassTag(last_dex_class_desc);
      }
      CHECK_EQ(info_.Depth(), 1);
      FinishLazyTypes();
      info_.EndTag();  // DW_TAG_compile_unit
      std::vector<uint8_t> buffer;
      buffer.reserve(info_.data()->size() + KB);
      const size_t offset = owner_->builder_->GetDebugInfo()->GetSize();
      const size_t debug_abbrev_offset =
          owner_->debug_abbrev_.Insert(debug_abbrev_.data(), debug_abbrev_.size());
      WriteDebugInfoCU(debug_abbrev_offset, info_, offset, &buffer, &owner_->debug_info_patches_);
      owner_->builder_->GetDebugInfo()->WriteFully(buffer.data(), buffer.size());
    }

    void Write(const ArrayRef<mirror::Class*>& types) SHARED_REQUIRES(Locks::mutator_lock_) {
      info_.StartTag(DW_TAG_compile_unit);
      info_.WriteStrp(DW_AT_producer, owner_->WriteString("Android dex2oat"));
      info_.WriteData1(DW_AT_language, DW_LANG_Java);

      for (mirror::Class* type : types) {
        if (type->IsPrimitive()) {
          // For primitive types the definition and the declaration is the same.
          if (type->GetPrimitiveType() != Primitive::kPrimVoid) {
            WriteTypeDeclaration(type->GetDescriptor(nullptr));
          }
        } else if (type->IsArrayClass()) {
          mirror::Class* element_type = type->GetComponentType();
          uint32_t component_size = type->GetComponentSize();
          uint32_t data_offset = mirror::Array::DataOffset(component_size).Uint32Value();
          uint32_t length_offset = mirror::Array::LengthOffset().Uint32Value();

          info_.StartTag(DW_TAG_array_type);
          std::string descriptor_string;
          WriteLazyType(element_type->GetDescriptor(&descriptor_string));
          info_.WriteUdata(DW_AT_data_member_location, data_offset);
          info_.StartTag(DW_TAG_subrange_type);
          DCHECK_LT(length_offset, 32u);
          uint8_t count[] = {
            DW_OP_push_object_address,
            static_cast<uint8_t>(DW_OP_lit0 + length_offset),
            DW_OP_plus,
            DW_OP_deref_size,
            4  // Array length is always 32-bit wide.
          };
          info_.WriteExprLoc(DW_AT_count, &count, sizeof(count));
          info_.EndTag();  // DW_TAG_subrange_type.
          info_.EndTag();  // DW_TAG_array_type.
        } else {
          std::string descriptor_string;
          const char* desc = type->GetDescriptor(&descriptor_string);
          StartClassTag(desc);

          if (!type->IsVariableSize()) {
            info_.WriteUdata(DW_AT_byte_size, type->GetObjectSize());
          }

          // Base class.
          mirror::Class* base_class = type->GetSuperClass();
          if (base_class != nullptr) {
            info_.StartTag(DW_TAG_inheritance);
            WriteLazyType(base_class->GetDescriptor(&descriptor_string));
            info_.WriteUdata(DW_AT_data_member_location, 0);
            info_.WriteSdata(DW_AT_accessibility, DW_ACCESS_public);
            info_.EndTag();  // DW_TAG_inheritance.
          }

          // Member variables.
          for (uint32_t i = 0, count = type->NumInstanceFields(); i < count; ++i) {
            ArtField* field = type->GetInstanceField(i);
            info_.StartTag(DW_TAG_member);
            WriteName(field->GetName());
            WriteLazyType(field->GetTypeDescriptor());
            info_.WriteUdata(DW_AT_data_member_location, field->GetOffset().Uint32Value());
            uint32_t access_flags = field->GetAccessFlags();
            if (access_flags & kAccPublic) {
              info_.WriteSdata(DW_AT_accessibility, DW_ACCESS_public);
            } else if (access_flags & kAccProtected) {
              info_.WriteSdata(DW_AT_accessibility, DW_ACCESS_protected);
            } else if (access_flags & kAccPrivate) {
              info_.WriteSdata(DW_AT_accessibility, DW_ACCESS_private);
            }
            info_.EndTag();  // DW_TAG_member.
          }

          EndClassTag(desc);
        }
      }

      CHECK_EQ(info_.Depth(), 1);
      FinishLazyTypes();
      info_.EndTag();  // DW_TAG_compile_unit.
      std::vector<uint8_t> buffer;
      buffer.reserve(info_.data()->size() + KB);
      const size_t offset = owner_->builder_->GetDebugInfo()->GetSize();
      const size_t debug_abbrev_offset =
          owner_->debug_abbrev_.Insert(debug_abbrev_.data(), debug_abbrev_.size());
      WriteDebugInfoCU(debug_abbrev_offset, info_, offset, &buffer, &owner_->debug_info_patches_);
      owner_->builder_->GetDebugInfo()->WriteFully(buffer.data(), buffer.size());
    }

    // Write table into .debug_loc which describes location of dex register.
    // The dex register might be valid only at some points and it might
    // move between machine registers and stack.
    void WriteRegLocation(const MethodDebugInfo* method_info,
                          uint16_t vreg,
                          bool is64bitValue,
                          uint32_t compilation_unit_low_pc,
                          uint32_t dex_pc_low = 0,
                          uint32_t dex_pc_high = 0xFFFFFFFF) {
      using Kind = DexRegisterLocation::Kind;
      if (!IsFromOptimizingCompiler(method_info)) {
        return;
      }

      Writer<> debug_loc(&owner_->debug_loc_);
      Writer<> debug_ranges(&owner_->debug_ranges_);
      info_.WriteSecOffset(DW_AT_location, debug_loc.size());
      info_.WriteSecOffset(DW_AT_start_scope, debug_ranges.size());

      std::vector<VariableLocation> variable_locations = GetVariableLocations(
          method_info,
          vreg,
          is64bitValue,
          dex_pc_low,
          dex_pc_high);

      // Write .debug_loc entries.
      const InstructionSet isa = owner_->builder_->GetIsa();
      const bool is64bit = Is64BitInstructionSet(isa);
      for (const VariableLocation& variable_location : variable_locations) {
        // Translate dex register location to DWARF expression.
        // Note that 64-bit value might be split to two distinct locations.
        // (for example, two 32-bit machine registers, or even stack and register)
        uint8_t buffer[64];
        uint8_t* pos = buffer;
        DexRegisterLocation reg_lo = variable_location.reg_lo;
        DexRegisterLocation reg_hi = variable_location.reg_hi;
        for (int piece = 0; piece < (is64bitValue ? 2 : 1); piece++) {
          DexRegisterLocation reg_loc = (piece == 0 ? reg_lo : reg_hi);
          const Kind kind = reg_loc.GetKind();
          const int32_t value = reg_loc.GetValue();
          if (kind == Kind::kInStack) {
            const size_t frame_size = method_info->compiled_method_->GetFrameSizeInBytes();
            *(pos++) = DW_OP_fbreg;
            // The stack offset is relative to SP. Make it relative to CFA.
            pos = EncodeSignedLeb128(pos, value - frame_size);
            if (piece == 0 && reg_hi.GetKind() == Kind::kInStack &&
                reg_hi.GetValue() == value + 4) {
              break;  // the high word is correctly implied by the low word.
            }
          } else if (kind == Kind::kInRegister) {
            pos = WriteOpReg(pos, GetDwarfCoreReg(isa, value).num());
            if (piece == 0 && reg_hi.GetKind() == Kind::kInRegisterHigh &&
                reg_hi.GetValue() == value) {
              break;  // the high word is correctly implied by the low word.
            }
          } else if (kind == Kind::kInFpuRegister) {
            if ((isa == kArm || isa == kThumb2) &&
                piece == 0 && reg_hi.GetKind() == Kind::kInFpuRegister &&
                reg_hi.GetValue() == value + 1 && value % 2 == 0) {
              // Translate S register pair to D register (e.g. S4+S5 to D2).
              pos = WriteOpReg(pos, Reg::ArmDp(value / 2).num());
              break;
            }
            if (isa == kMips || isa == kMips64) {
              // TODO: Find what the DWARF floating point register numbers are on MIPS.
              break;
            }
            pos = WriteOpReg(pos, GetDwarfFpReg(isa, value).num());
            if (piece == 0 && reg_hi.GetKind() == Kind::kInFpuRegisterHigh &&
                reg_hi.GetValue() == reg_lo.GetValue()) {
              break;  // the high word is correctly implied by the low word.
            }
          } else if (kind == Kind::kConstant) {
            *(pos++) = DW_OP_consts;
            pos = EncodeSignedLeb128(pos, value);
            *(pos++) = DW_OP_stack_value;
          } else if (kind == Kind::kNone) {
            break;
          } else {
            // kInStackLargeOffset and kConstantLargeValue are hidden by GetKind().
            // kInRegisterHigh and kInFpuRegisterHigh should be handled by
            // the special cases above and they should not occur alone.
            LOG(ERROR) << "Unexpected register location kind: "
                       << DexRegisterLocation::PrettyDescriptor(kind);
            break;
          }
          if (is64bitValue) {
            // Write the marker which is needed by split 64-bit values.
            // This code is skipped by the special cases.
            *(pos++) = DW_OP_piece;
            pos = EncodeUnsignedLeb128(pos, 4);
          }
        }

        // Check that the buffer is large enough; keep half of it empty for safety.
        DCHECK_LE(static_cast<size_t>(pos - buffer), sizeof(buffer) / 2);
        if (pos > buffer) {
          if (is64bit) {
            debug_loc.PushUint64(variable_location.low_pc - compilation_unit_low_pc);
            debug_loc.PushUint64(variable_location.high_pc - compilation_unit_low_pc);
          } else {
            debug_loc.PushUint32(variable_location.low_pc - compilation_unit_low_pc);
            debug_loc.PushUint32(variable_location.high_pc - compilation_unit_low_pc);
          }
          // Write the expression.
          debug_loc.PushUint16(pos - buffer);
          debug_loc.PushData(buffer, pos - buffer);
        } else {
          // Do not generate .debug_loc if the location is not known.
        }
      }
      // Write end-of-list entry.
      if (is64bit) {
        debug_loc.PushUint64(0);
        debug_loc.PushUint64(0);
      } else {
        debug_loc.PushUint32(0);
        debug_loc.PushUint32(0);
      }

      // Write .debug_ranges entries.
      // This includes ranges where the variable is in scope but the location is not known.
      for (size_t i = 0; i < variable_locations.size(); i++) {
        uint32_t low_pc = variable_locations[i].low_pc;
        uint32_t high_pc = variable_locations[i].high_pc;
        while (i + 1 < variable_locations.size() && variable_locations[i+1].low_pc == high_pc) {
          // Merge address range with the next entry.
          high_pc = variable_locations[++i].high_pc;
        }
        if (is64bit) {
          debug_ranges.PushUint64(low_pc - compilation_unit_low_pc);
          debug_ranges.PushUint64(high_pc - compilation_unit_low_pc);
        } else {
          debug_ranges.PushUint32(low_pc - compilation_unit_low_pc);
          debug_ranges.PushUint32(high_pc - compilation_unit_low_pc);
        }
      }
      // Write end-of-list entry.
      if (is64bit) {
        debug_ranges.PushUint64(0);
        debug_ranges.PushUint64(0);
      } else {
        debug_ranges.PushUint32(0);
        debug_ranges.PushUint32(0);
      }
    }

    // Some types are difficult to define as we go since they need
    // to be enclosed in the right set of namespaces. Therefore we
    // just define all types lazily at the end of compilation unit.
    void WriteLazyType(const char* type_descriptor) {
      if (type_descriptor != nullptr && type_descriptor[0] != 'V') {
        lazy_types_.emplace(std::string(type_descriptor), info_.size());
        info_.WriteRef4(DW_AT_type, 0);
      }
    }

    void FinishLazyTypes() {
      for (const auto& lazy_type : lazy_types_) {
        info_.UpdateUint32(lazy_type.second, WriteTypeDeclaration(lazy_type.first));
      }
      lazy_types_.clear();
    }

   private:
    void WriteName(const char* name) {
      if (name != nullptr) {
        info_.WriteStrp(DW_AT_name, owner_->WriteString(name));
      }
    }

    // Helper which writes DWARF expression referencing a register.
    static uint8_t* WriteOpReg(uint8_t* buffer, uint32_t dwarf_reg_num) {
      if (dwarf_reg_num < 32) {
        *(buffer++) = DW_OP_reg0 + dwarf_reg_num;
      } else {
        *(buffer++) = DW_OP_regx;
        buffer = EncodeUnsignedLeb128(buffer, dwarf_reg_num);
      }
      return buffer;
    }

    // Convert dex type descriptor to DWARF.
    // Returns offset in the compilation unit.
    size_t WriteTypeDeclaration(const std::string& desc) {
      DCHECK(!desc.empty());
      const auto& it = type_cache_.find(desc);
      if (it != type_cache_.end()) {
        return it->second;
      }

      size_t offset;
      if (desc[0] == 'L') {
        // Class type. For example: Lpackage/name;
        size_t class_offset = StartClassTag(desc.c_str());
        info_.WriteFlag(DW_AT_declaration, true);
        EndClassTag(desc.c_str());
        // Reference to the class type.
        offset = info_.StartTag(DW_TAG_reference_type);
        info_.WriteRef(DW_AT_type, class_offset);
        info_.EndTag();
      } else if (desc[0] == '[') {
        // Array type.
        size_t element_type = WriteTypeDeclaration(desc.substr(1));
        size_t array_type = info_.StartTag(DW_TAG_array_type);
        info_.WriteFlag(DW_AT_declaration, true);
        info_.WriteRef(DW_AT_type, element_type);
        info_.EndTag();
        offset = info_.StartTag(DW_TAG_reference_type);
        info_.WriteRef4(DW_AT_type, array_type);
        info_.EndTag();
      } else {
        // Primitive types.
        const char* name;
        uint32_t encoding;
        uint32_t byte_size;
        switch (desc[0]) {
        case 'B':
          name = "byte";
          encoding = DW_ATE_signed;
          byte_size = 1;
          break;
        case 'C':
          name = "char";
          encoding = DW_ATE_UTF;
          byte_size = 2;
          break;
        case 'D':
          name = "double";
          encoding = DW_ATE_float;
          byte_size = 8;
          break;
        case 'F':
          name = "float";
          encoding = DW_ATE_float;
          byte_size = 4;
          break;
        case 'I':
          name = "int";
          encoding = DW_ATE_signed;
          byte_size = 4;
          break;
        case 'J':
          name = "long";
          encoding = DW_ATE_signed;
          byte_size = 8;
          break;
        case 'S':
          name = "short";
          encoding = DW_ATE_signed;
          byte_size = 2;
          break;
        case 'Z':
          name = "boolean";
          encoding = DW_ATE_boolean;
          byte_size = 1;
          break;
        case 'V':
          LOG(FATAL) << "Void type should not be encoded";
          UNREACHABLE();
        default:
          LOG(FATAL) << "Unknown dex type descriptor: \"" << desc << "\"";
          UNREACHABLE();
        }
        offset = info_.StartTag(DW_TAG_base_type);
        WriteName(name);
        info_.WriteData1(DW_AT_encoding, encoding);
        info_.WriteData1(DW_AT_byte_size, byte_size);
        info_.EndTag();
      }

      type_cache_.emplace(desc, offset);
      return offset;
    }

    // Start DW_TAG_class_type tag nested in DW_TAG_namespace tags.
    // Returns offset of the class tag in the compilation unit.
    size_t StartClassTag(const char* desc) {
      DCHECK(desc != nullptr && desc[0] == 'L');
      // Enclose the type in namespace tags.
      const char* end;
      for (desc = desc + 1; (end = strchr(desc, '/')) != nullptr; desc = end + 1) {
        info_.StartTag(DW_TAG_namespace);
        WriteName(std::string(desc, end - desc).c_str());
      }
      // Start the class tag.
      size_t offset = info_.StartTag(DW_TAG_class_type);
      end = strchr(desc, ';');
      CHECK(end != nullptr);
      WriteName(std::string(desc, end - desc).c_str());
      return offset;
    }

    void EndClassTag(const char* desc) {
      DCHECK(desc != nullptr && desc[0] == 'L');
      // End the class tag.
      info_.EndTag();
      // Close namespace tags.
      const char* end;
      for (desc = desc + 1; (end = strchr(desc, '/')) != nullptr; desc = end + 1) {
        info_.EndTag();
      }
    }

    // For access to the ELF sections.
    DebugInfoWriter<ElfTypes>* owner_;
    // Debug abbrevs for this compilation unit only.
    std::vector<uint8_t> debug_abbrev_;
    // Temporary buffer to create and store the entries.
    DebugInfoEntryWriter<> info_;
    // Cache of already translated type descriptors.
    std::map<std::string, size_t> type_cache_;  // type_desc -> definition_offset.
    // 32-bit references which need to be resolved to a type later.
    // Given type may be used multiple times.  Therefore we need a multimap.
    std::multimap<std::string, size_t> lazy_types_;  // type_desc -> patch_offset.
  };

 public:
  explicit DebugInfoWriter(ElfBuilder<ElfTypes>* builder) : builder_(builder) {
  }

  void Start() {
    builder_->GetDebugInfo()->Start();
  }

  void WriteCompilationUnit(const CompilationUnit& compilation_unit) {
    CompilationUnitWriter writer(this);
    writer.Write(compilation_unit);
  }

  void WriteTypes(const ArrayRef<mirror::Class*>& types) SHARED_REQUIRES(Locks::mutator_lock_) {
    CompilationUnitWriter writer(this);
    writer.Write(types);
  }

  void End() {
    builder_->GetDebugInfo()->End();
    builder_->WritePatches(".debug_info.oat_patches",
                           ArrayRef<const uintptr_t>(debug_info_patches_));
    builder_->WriteSection(".debug_abbrev", &debug_abbrev_.Data());
    builder_->WriteSection(".debug_str", &debug_str_.Data());
    builder_->WriteSection(".debug_loc", &debug_loc_);
    builder_->WriteSection(".debug_ranges", &debug_ranges_);
  }

 private:
  size_t WriteString(const char* str) {
    return debug_str_.Insert(reinterpret_cast<const uint8_t*>(str), strlen(str) + 1);
  }

  ElfBuilder<ElfTypes>* builder_;
  std::vector<uintptr_t> debug_info_patches_;
  DedupVector debug_abbrev_;
  DedupVector debug_str_;
  std::vector<uint8_t> debug_loc_;
  std::vector<uint8_t> debug_ranges_;

  std::unordered_set<const char*> defined_dex_classes_;  // For CHECKs only.
};

template<typename ElfTypes>
class DebugLineWriter {
  typedef typename ElfTypes::Addr Elf_Addr;

 public:
  explicit DebugLineWriter(ElfBuilder<ElfTypes>* builder) : builder_(builder) {
  }

  void Start() {
    builder_->GetDebugLine()->Start();
  }

  // Write line table for given set of methods.
  // Returns the number of bytes written.
  size_t WriteCompilationUnit(CompilationUnit& compilation_unit) {
    const bool is64bit = Is64BitInstructionSet(builder_->GetIsa());
    const Elf_Addr text_address = builder_->GetText()->Exists()
        ? builder_->GetText()->GetAddress()
        : 0;

    compilation_unit.debug_line_offset_ = builder_->GetDebugLine()->GetSize();

    std::vector<FileEntry> files;
    std::unordered_map<std::string, size_t> files_map;
    std::vector<std::string> directories;
    std::unordered_map<std::string, size_t> directories_map;
    int code_factor_bits_ = 0;
    int dwarf_isa = -1;
    switch (builder_->GetIsa()) {
      case kArm:  // arm actually means thumb2.
      case kThumb2:
        code_factor_bits_ = 1;  // 16-bit instuctions
        dwarf_isa = 1;  // DW_ISA_ARM_thumb.
        break;
      case kArm64:
      case kMips:
      case kMips64:
        code_factor_bits_ = 2;  // 32-bit instructions
        break;
      case kNone:
      case kX86:
      case kX86_64:
        break;
    }
    DebugLineOpCodeWriter<> opcodes(is64bit, code_factor_bits_);
    for (const MethodDebugInfo* mi : compilation_unit.methods_) {
      // Ignore function if we have already generated line table for the same address.
      // It would confuse the debugger and the DWARF specification forbids it.
      if (mi->deduped_) {
        continue;
      }

      ArrayRef<const SrcMapElem> src_mapping_table;
      std::vector<SrcMapElem> src_mapping_table_from_stack_maps;
      if (IsFromOptimizingCompiler(mi)) {
        // Use stack maps to create mapping table from pc to dex.
        const CodeInfo code_info(mi->compiled_method_->GetVmapTable().data());
        const StackMapEncoding encoding = code_info.ExtractEncoding();
        for (uint32_t s = 0; s < code_info.GetNumberOfStackMaps(); s++) {
          StackMap stack_map = code_info.GetStackMapAt(s, encoding);
          DCHECK(stack_map.IsValid());
          const uint32_t pc = stack_map.GetNativePcOffset(encoding);
          const int32_t dex = stack_map.GetDexPc(encoding);
          src_mapping_table_from_stack_maps.push_back({pc, dex});
        }
        std::sort(src_mapping_table_from_stack_maps.begin(),
                  src_mapping_table_from_stack_maps.end());
        src_mapping_table = ArrayRef<const SrcMapElem>(src_mapping_table_from_stack_maps);
      } else {
        // Use the mapping table provided by the quick compiler.
        src_mapping_table = mi->compiled_method_->GetSrcMappingTable();
      }

      if (src_mapping_table.empty()) {
        continue;
      }

      Elf_Addr method_address = text_address + mi->low_pc_;

      PositionInfos position_infos;
      const DexFile* dex = mi->dex_file_;
      if (!dex->DecodeDebugPositionInfo(mi->code_item_, PositionInfoCallback, &position_infos)) {
        continue;
      }

      if (position_infos.empty()) {
        continue;
      }

      opcodes.SetAddress(method_address);
      if (dwarf_isa != -1) {
        opcodes.SetISA(dwarf_isa);
      }

      // Get and deduplicate directory and filename.
      int file_index = 0;  // 0 - primary source file of the compilation.
      auto& dex_class_def = dex->GetClassDef(mi->class_def_index_);
      const char* source_file = dex->GetSourceFile(dex_class_def);
      if (source_file != nullptr) {
        std::string file_name(source_file);
        size_t file_name_slash = file_name.find_last_of('/');
        std::string class_name(dex->GetClassDescriptor(dex_class_def));
        size_t class_name_slash = class_name.find_last_of('/');
        std::string full_path(file_name);

        // Guess directory from package name.
        int directory_index = 0;  // 0 - current directory of the compilation.
        if (file_name_slash == std::string::npos &&  // Just filename.
            class_name.front() == 'L' &&  // Type descriptor for a class.
            class_name_slash != std::string::npos) {  // Has package name.
          std::string package_name = class_name.substr(1, class_name_slash - 1);
          auto it = directories_map.find(package_name);
          if (it == directories_map.end()) {
            directory_index = 1 + directories.size();
            directories_map.emplace(package_name, directory_index);
            directories.push_back(package_name);
          } else {
            directory_index = it->second;
          }
          full_path = package_name + "/" + file_name;
        }

        // Add file entry.
        auto it2 = files_map.find(full_path);
        if (it2 == files_map.end()) {
          file_index = 1 + files.size();
          files_map.emplace(full_path, file_index);
          files.push_back(FileEntry {
            file_name,
            directory_index,
            0,  // Modification time - NA.
            0,  // File size - NA.
          });
        } else {
          file_index = it2->second;
        }
      }
      opcodes.SetFile(file_index);

      // Generate mapping opcodes from PC to Java lines.
      if (file_index != 0) {
        bool first = true;
        for (SrcMapElem pc2dex : src_mapping_table) {
          uint32_t pc = pc2dex.from_;
          int dex_pc = pc2dex.to_;
          // Find mapping with address with is greater than our dex pc; then go back one step.
          auto ub = std::upper_bound(position_infos.begin(), position_infos.end(), dex_pc,
              [](uint32_t address, const DexFile::PositionInfo& entry) {
                  return address < entry.address_;
              });
          if (ub != position_infos.begin()) {
            int line = (--ub)->line_;
            if (first) {
              first = false;
              if (pc > 0) {
                // Assume that any preceding code is prologue.
                int first_line = position_infos.front().line_;
                // Prologue is not a sensible place for a breakpoint.
                opcodes.NegateStmt();
                opcodes.AddRow(method_address, first_line);
                opcodes.NegateStmt();
                opcodes.SetPrologueEnd();
              }
              opcodes.AddRow(method_address + pc, line);
            } else if (line != opcodes.CurrentLine()) {
              opcodes.AddRow(method_address + pc, line);
            }
          }
        }
      } else {
        // line 0 - instruction cannot be attributed to any source line.
        opcodes.AddRow(method_address, 0);
      }

      opcodes.AdvancePC(text_address + mi->high_pc_);
      opcodes.EndSequence();
    }
    std::vector<uint8_t> buffer;
    buffer.reserve(opcodes.data()->size() + KB);
    size_t offset = builder_->GetDebugLine()->GetSize();
    WriteDebugLineTable(directories, files, opcodes, offset, &buffer, &debug_line_patches);
    builder_->GetDebugLine()->WriteFully(buffer.data(), buffer.size());
    return buffer.size();
  }

  void End() {
    builder_->GetDebugLine()->End();
    builder_->WritePatches(".debug_line.oat_patches",
                           ArrayRef<const uintptr_t>(debug_line_patches));
  }

 private:
  ElfBuilder<ElfTypes>* builder_;
  std::vector<uintptr_t> debug_line_patches;
};

// Get all types loaded by the runtime.
static std::vector<mirror::Class*> GetLoadedRuntimeTypes() SHARED_REQUIRES(Locks::mutator_lock_) {
  std::vector<mirror::Class*> result;
  class CollectClasses : public ClassVisitor {
   public:
    virtual bool Visit(mirror::Class* klass) {
      classes_->push_back(klass);
      return true;
    }
    std::vector<mirror::Class*>* classes_;
  };
  CollectClasses visitor;
  visitor.classes_ = &result;
  Runtime::Current()->GetClassLinker()->VisitClasses(&visitor);
  return result;
}

template<typename ElfTypes>
static void WriteDebugSections(ElfBuilder<ElfTypes>* builder,
                               bool write_loaded_runtime_types,
                               const ArrayRef<const MethodDebugInfo>& method_infos) {
  // Group the methods into compilation units based on source file.
  std::vector<CompilationUnit> compilation_units;
  const char* last_source_file = nullptr;
  for (const MethodDebugInfo& mi : method_infos) {
    auto& dex_class_def = mi.dex_file_->GetClassDef(mi.class_def_index_);
    const char* source_file = mi.dex_file_->GetSourceFile(dex_class_def);
    if (compilation_units.empty() || source_file != last_source_file) {
      compilation_units.push_back(CompilationUnit());
    }
    CompilationUnit& cu = compilation_units.back();
    cu.methods_.push_back(&mi);
    cu.low_pc_ = std::min(cu.low_pc_, mi.low_pc_);
    cu.high_pc_ = std::max(cu.high_pc_, mi.high_pc_);
    last_source_file = source_file;
  }

  // Write .debug_line section.
  if (!compilation_units.empty()) {
    DebugLineWriter<ElfTypes> line_writer(builder);
    line_writer.Start();
    for (auto& compilation_unit : compilation_units) {
      line_writer.WriteCompilationUnit(compilation_unit);
    }
    line_writer.End();
  }

  // Write .debug_info section.
  if (!compilation_units.empty() || write_loaded_runtime_types) {
    DebugInfoWriter<ElfTypes> info_writer(builder);
    info_writer.Start();
    for (const auto& compilation_unit : compilation_units) {
      info_writer.WriteCompilationUnit(compilation_unit);
    }
    if (write_loaded_runtime_types) {
      Thread* self = Thread::Current();
      // The lock prevents the classes being moved by the GC.
      ReaderMutexLock mu(self, *Locks::mutator_lock_);
      std::vector<mirror::Class*> types = GetLoadedRuntimeTypes();
      info_writer.WriteTypes(ArrayRef<mirror::Class*>(types.data(), types.size()));
    }
    info_writer.End();
  }
}

template <typename ElfTypes>
void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder,
                       const ArrayRef<const MethodDebugInfo>& method_infos) {
  bool generated_mapping_symbol = false;
  auto* strtab = builder->GetStrTab();
  auto* symtab = builder->GetSymTab();

  if (method_infos.empty()) {
    return;
  }

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (const MethodDebugInfo& info : method_infos) {
    if (info.deduped_) {
      deduped_addresses.insert(info.low_pc_);
    }
  }

  strtab->Start();
  strtab->Write("");  // strtab should start with empty string.
  for (const MethodDebugInfo& info : method_infos) {
    if (info.deduped_) {
      continue;  // Add symbol only for the first instance.
    }
    std::string name = PrettyMethod(info.dex_method_index_, *info.dex_file_, true);
    if (deduped_addresses.find(info.low_pc_) != deduped_addresses.end()) {
      name += " [DEDUPED]";
    }

    const auto* text = builder->GetText()->Exists() ? builder->GetText() : nullptr;
    const bool is_relative = (text != nullptr);
    uint32_t low_pc = info.low_pc_;
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    low_pc += info.compiled_method_->CodeDelta();
    symtab->Add(strtab->Write(name), text, low_pc,
                is_relative, info.high_pc_ - info.low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    // Note that even if we generate just a single mapping symbol, ARM's Streamline
    // requires it to match function symbol.  Just address 0 does not work.
    if (info.compiled_method_->GetInstructionSet() == kThumb2) {
      if (!generated_mapping_symbol || !kGenerateSingleArmMappingSymbol) {
        symtab->Add(strtab->Write("$t"), text, info.low_pc_ & ~1,
                    is_relative, 0, STB_LOCAL, STT_NOTYPE);
        generated_mapping_symbol = true;
      }
    }
  }
  strtab->End();

  // Symbols are buffered and written after names (because they are smaller).
  // We could also do two passes in this function to avoid the buffering.
  symtab->Start();
  symtab->Write();
  symtab->End();
}

template <typename ElfTypes>
void WriteDebugInfo(ElfBuilder<ElfTypes>* builder,
                    bool write_loaded_runtime_types,
                    const ArrayRef<const MethodDebugInfo>& method_infos,
                    CFIFormat cfi_format) {
  // Add methods to .symtab.
  WriteDebugSymbols(builder, method_infos);
  // Generate CFI (stack unwinding information).
  WriteCFISection(builder, method_infos, cfi_format);
  // Write DWARF .debug_* sections.
  WriteDebugSections(builder, write_loaded_runtime_types, method_infos);
}

template <typename ElfTypes>
static ArrayRef<const uint8_t> WriteDebugElfFileForMethodInternal(
    const dwarf::MethodDebugInfo& method_info) {
  const InstructionSet isa = method_info.compiled_method_->GetInstructionSet();
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(isa, &out));
  builder->Start();
  WriteDebugInfo(builder.get(),
                 false,
                 ArrayRef<const MethodDebugInfo>(&method_info, 1),
                 DW_DEBUG_FRAME_FORMAT);
  builder->End();
  CHECK(builder->Good());
  // Make a copy of the buffer.  We want to shrink it anyway.
  uint8_t* result = new uint8_t[buffer.size()];
  CHECK(result != nullptr);
  memcpy(result, buffer.data(), buffer.size());
  return ArrayRef<const uint8_t>(result, buffer.size());
}

ArrayRef<const uint8_t> WriteDebugElfFileForMethod(const dwarf::MethodDebugInfo& method_info) {
  const InstructionSet isa = method_info.compiled_method_->GetInstructionSet();
  if (Is64BitInstructionSet(isa)) {
    return WriteDebugElfFileForMethodInternal<ElfTypes64>(method_info);
  } else {
    return WriteDebugElfFileForMethodInternal<ElfTypes32>(method_info);
  }
}

template <typename ElfTypes>
static ArrayRef<const uint8_t> WriteDebugElfFileForClassInternal(const InstructionSet isa,
                                                                 mirror::Class* type)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(isa, &out));
  builder->Start();

  DebugInfoWriter<ElfTypes> info_writer(builder.get());
  info_writer.Start();
  info_writer.WriteTypes(ArrayRef<mirror::Class*>(&type, 1));
  info_writer.End();

  builder->End();
  CHECK(builder->Good());
  // Make a copy of the buffer.  We want to shrink it anyway.
  uint8_t* result = new uint8_t[buffer.size()];
  CHECK(result != nullptr);
  memcpy(result, buffer.data(), buffer.size());
  return ArrayRef<const uint8_t>(result, buffer.size());
}

ArrayRef<const uint8_t> WriteDebugElfFileForClass(const InstructionSet isa, mirror::Class* type) {
  if (Is64BitInstructionSet(isa)) {
    return WriteDebugElfFileForClassInternal<ElfTypes64>(isa, type);
  } else {
    return WriteDebugElfFileForClassInternal<ElfTypes32>(isa, type);
  }
}

// Explicit instantiations
template void WriteDebugInfo<ElfTypes32>(
    ElfBuilder<ElfTypes32>* builder,
    bool write_loaded_runtime_types,
    const ArrayRef<const MethodDebugInfo>& method_infos,
    CFIFormat cfi_format);
template void WriteDebugInfo<ElfTypes64>(
    ElfBuilder<ElfTypes64>* builder,
    bool write_loaded_runtime_types,
    const ArrayRef<const MethodDebugInfo>& method_infos,
    CFIFormat cfi_format);

}  // namespace dwarf
}  // namespace art
