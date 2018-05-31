/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "dexanalyze_experiments.h"

#include <stdint.h>
#include <inttypes.h>
#include <iostream>
#include <map>
#include <vector>

#include "android-base/stringprintf.h"
#include "dex/class_accessor-inl.h"
#include "dex/class_iterator.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_instruction-inl.h"
#include "dex/standard_dex_file.h"
#include "dex/utf-inl.h"

namespace art {

static inline bool IsRange(Instruction::Code code) {
  return code == Instruction::INVOKE_VIRTUAL_RANGE ||
      code == Instruction::INVOKE_DIRECT_RANGE ||
      code == Instruction::INVOKE_SUPER_RANGE ||
      code == Instruction::INVOKE_STATIC_RANGE ||
      code == Instruction::INVOKE_INTERFACE_RANGE;
}

static inline uint16_t NumberOfArgs(const Instruction& inst) {
  return IsRange(inst.Opcode()) ? inst.VRegA_3rc() : inst.VRegA_35c();
}

static inline uint16_t DexMethodIndex(const Instruction& inst) {
  return IsRange(inst.Opcode()) ? inst.VRegB_3rc() : inst.VRegB_35c();
}

std::string Percent(uint64_t value, uint64_t max) {
  if (max == 0) {
    return "0";
  }
  return android::base::StringPrintf(
      "%" PRId64 "(%.2f%%)",
      value,
      static_cast<double>(value * 100) / static_cast<double>(max));
}

std::string PercentDivide(uint64_t value, uint64_t max) {
  if (max == 0) {
    return "0";
  }
  return android::base::StringPrintf(
      "%" PRId64 "/%" PRId64 "(%.2f%%)",
      value,
      max,
      static_cast<double>(value * 100) / static_cast<double>(max));
}

static size_t PrefixLen(const std::string& a, const std::string& b) {
  size_t len = 0;
  for (; len < a.length() && len < b.length() && a[len] == b[len]; ++len) {}
  return len;
}

void AnalyzeStrings::ProcessDexFile(const DexFile& dex_file) {
  std::vector<std::string> strings;
  for (size_t i = 0; i < dex_file.NumStringIds(); ++i) {
    uint32_t length = 0;
    const char* data = dex_file.StringDataAndUtf16LengthByIdx(dex::StringIndex(i), &length);
    // Analyze if the string has any UTF16 chars.
    bool have_wide_char = false;
    const char* ptr = data;
    for (size_t j = 0; j < length; ++j) {
      have_wide_char = have_wide_char || GetUtf16FromUtf8(&ptr) >= 0x100;
    }
    if (have_wide_char) {
      wide_string_bytes_ += 2 * length;
    } else {
      ascii_string_bytes_ += length;
    }
    string_data_bytes_ += ptr - data;

    strings.push_back(data);
  }
  // Note that the strings are probably already sorted.
  std::sort(strings.begin(), strings.end());

  // Tunable parameters.
  static const size_t kMinPrefixLen = 3;
  static const size_t kPrefixConstantCost = 5;
  static const size_t kPrefixIndexCost = 2;

  // Calculate total shared prefix.
  std::vector<size_t> shared_len;
  std::set<std::string> prefixes;
  for (size_t i = 0; i < strings.size(); ++i) {
    size_t best_len = 0;
    if (i > 0) {
      best_len = std::max(best_len, PrefixLen(strings[i], strings[i - 1]));
    }
    if (i < strings.size() - 1) {
      best_len = std::max(best_len, PrefixLen(strings[i], strings[i + 1]));
    }
    std::string prefix;
    if (best_len >= kMinPrefixLen) {
      prefix = strings[i].substr(0, best_len);
      prefixes.insert(prefix);
      total_prefix_savings_ += prefix.length();
    }
    total_prefix_index_cost_ += kPrefixIndexCost;
  }
  total_num_prefixes_ += prefixes.size();
  for (const std::string& s : prefixes) {
    // 4 bytes for an offset, one for length.
    total_prefix_dict_ += s.length();
    total_prefix_table_ += kPrefixConstantCost;
  }
}

void AnalyzeStrings::Dump(std::ostream& os, uint64_t total_size) const {
  os << "Total string data bytes " << Percent(string_data_bytes_, total_size) << "\n";
  os << "UTF-16 string data bytes " << Percent(wide_string_bytes_, total_size) << "\n";
  os << "ASCII string data bytes " << Percent(ascii_string_bytes_, total_size) << "\n";

  // Prefix based strings.
  os << "Total shared prefix bytes " << Percent(total_prefix_savings_, total_size) << "\n";
  os << "Prefix dictionary cost " << Percent(total_prefix_dict_, total_size) << "\n";
  os << "Prefix table cost " << Percent(total_prefix_table_, total_size) << "\n";
  os << "Prefix index cost " << Percent(total_prefix_index_cost_, total_size) << "\n";
  int64_t net_savings = total_prefix_savings_;
  net_savings -= total_prefix_dict_;
  net_savings -= total_prefix_table_;
  net_savings -= total_prefix_index_cost_;
  os << "Prefix net savings " << Percent(net_savings, total_size) << "\n";
  os << "Prefix dictionary elements " << total_num_prefixes_ << "\n";
}

void CountDexIndices::ProcessDexFile(const DexFile& dex_file) {
  num_string_ids_ += dex_file.NumStringIds();
  num_method_ids_ += dex_file.NumMethodIds();
  num_field_ids_ += dex_file.NumFieldIds();
  num_type_ids_ += dex_file.NumTypeIds();
  num_class_defs_ += dex_file.NumClassDefs();
  std::set<size_t> unique_code_items;
  for (ClassAccessor accessor : dex_file.GetClasses()) {
    std::set<size_t> unique_method_ids;
    std::set<size_t> unique_string_ids;
    for (const ClassAccessor::Method& method : accessor.GetMethods()) {
      dex_code_bytes_ += method.GetInstructions().InsnsSizeInBytes();
      unique_code_items.insert(method.GetCodeItemOffset());
      for (const DexInstructionPcPair& inst : method.GetInstructions()) {
        switch (inst->Opcode()) {
          case Instruction::CONST_STRING: {
            const dex::StringIndex string_index(inst->VRegB_21c());
            unique_string_ids.insert(string_index.index_);
            ++num_string_ids_from_code_;
            break;
          }
          case Instruction::CONST_STRING_JUMBO: {
            const dex::StringIndex string_index(inst->VRegB_31c());
            unique_string_ids.insert(string_index.index_);
            ++num_string_ids_from_code_;
            break;
          }
          // Invoke cases.
          case Instruction::INVOKE_VIRTUAL:
          case Instruction::INVOKE_VIRTUAL_RANGE: {
            uint32_t method_idx = DexMethodIndex(inst.Inst());
            if (dex_file.GetMethodId(method_idx).class_idx_ == accessor.GetClassIdx()) {
              ++same_class_virtual_;
            }
            ++total_virtual_;
            unique_method_ids.insert(method_idx);
            break;
          }
          case Instruction::INVOKE_DIRECT:
          case Instruction::INVOKE_DIRECT_RANGE: {
            uint32_t method_idx = DexMethodIndex(inst.Inst());
            if (dex_file.GetMethodId(method_idx).class_idx_ == accessor.GetClassIdx()) {
              ++same_class_direct_;
            }
            ++total_direct_;
            unique_method_ids.insert(method_idx);
            break;
          }
          case Instruction::INVOKE_STATIC:
          case Instruction::INVOKE_STATIC_RANGE: {
            uint32_t method_idx = DexMethodIndex(inst.Inst());
            if (dex_file.GetMethodId(method_idx).class_idx_ == accessor.GetClassIdx()) {
              ++same_class_static_;
            }
            ++total_static_;
            unique_method_ids.insert(method_idx);
            break;
          }
          case Instruction::INVOKE_INTERFACE:
          case Instruction::INVOKE_INTERFACE_RANGE: {
            uint32_t method_idx = DexMethodIndex(inst.Inst());
            if (dex_file.GetMethodId(method_idx).class_idx_ == accessor.GetClassIdx()) {
              ++same_class_interface_;
            }
            ++total_interface_;
            unique_method_ids.insert(method_idx);
            break;
          }
          case Instruction::INVOKE_SUPER:
          case Instruction::INVOKE_SUPER_RANGE: {
            uint32_t method_idx = DexMethodIndex(inst.Inst());
            if (dex_file.GetMethodId(method_idx).class_idx_ == accessor.GetClassIdx()) {
              ++same_class_super_;
            }
            ++total_super_;
            unique_method_ids.insert(method_idx);
            break;
          }
          default:
            break;
        }
      }
    }
    total_unique_method_idx_ += unique_method_ids.size();
    total_unique_string_ids_ += unique_string_ids.size();
  }
  total_unique_code_items_ += unique_code_items.size();
}

void CountDexIndices::Dump(std::ostream& os, uint64_t total_size) const {
  os << "Num string ids: " << num_string_ids_ << "\n";
  os << "Num method ids: " << num_method_ids_ << "\n";
  os << "Num field ids: " << num_field_ids_ << "\n";
  os << "Num type ids: " << num_type_ids_ << "\n";
  os << "Num class defs: " << num_class_defs_ << "\n";
  os << "Direct same class: " << PercentDivide(same_class_direct_, total_direct_) << "\n";
  os << "Virtual same class: " << PercentDivide(same_class_virtual_, total_virtual_) << "\n";
  os << "Static same class: " << PercentDivide(same_class_static_, total_static_) << "\n";
  os << "Interface same class: " << PercentDivide(same_class_interface_, total_interface_) << "\n";
  os << "Super same class: " << PercentDivide(same_class_super_, total_super_) << "\n";
  os << "Num strings accessed from code: " << num_string_ids_from_code_ << "\n";
  os << "Unique(per class) method ids accessed from code: " << total_unique_method_idx_ << "\n";
  os << "Unique(per class) string ids accessed from code: " << total_unique_string_ids_ << "\n";
  const size_t same_class_total =
      same_class_direct_ +
      same_class_virtual_ +
      same_class_static_ +
      same_class_interface_ +
      same_class_super_;
  const size_t other_class_total =
      total_direct_ +
      total_virtual_ +
      total_static_ +
      total_interface_ +
      total_super_;
  os << "Same class invokes: " << PercentDivide(same_class_total, other_class_total) << "\n";
  os << "Invokes from code: " << (same_class_total + other_class_total) << "\n";
  os << "Total Dex code bytes: " << Percent(dex_code_bytes_, total_size) << "\n";
  os << "Total unique code items: " << total_unique_code_items_ << "\n";
  os << "Total Dex size: " << total_size << "\n";
}

void CodeMetrics::ProcessDexFile(const DexFile& dex_file) {
  for (ClassAccessor accessor : dex_file.GetClasses()) {
    for (const ClassAccessor::Method& method : accessor.GetMethods()) {
      bool space_for_out_arg = false;
      for (const DexInstructionPcPair& inst : method.GetInstructions()) {
        switch (inst->Opcode()) {
          case Instruction::INVOKE_VIRTUAL:
          case Instruction::INVOKE_DIRECT:
          case Instruction::INVOKE_SUPER:
          case Instruction::INVOKE_INTERFACE:
          case Instruction::INVOKE_STATIC: {
            const uint32_t args = NumberOfArgs(inst.Inst());
            CHECK_LT(args, kMaxArgCount);
            ++arg_counts_[args];
            space_for_out_arg = args < kMaxArgCount - 1;
            break;
          }
          case Instruction::MOVE_RESULT:
          case Instruction::MOVE_RESULT_OBJECT: {
            if (space_for_out_arg) {
              move_result_savings_ += inst->SizeInCodeUnits() * 2;
            }
            break;
          }
          default:
            space_for_out_arg = false;
            break;
        }
      }
    }
  }
}

void CodeMetrics::Dump(std::ostream& os, uint64_t total_size) const {
  const uint64_t total = std::accumulate(arg_counts_, arg_counts_ + kMaxArgCount, 0u);
  for (size_t i = 0; i < kMaxArgCount; ++i) {
    os << "args=" << i << ": " << Percent(arg_counts_[i], total) << "\n";
  }
  os << "Move result savings: " << Percent(move_result_savings_, total_size) << "\n";
  os << "One byte invoke savings: " << Percent(total, total_size) << "\n";
  const uint64_t low_arg_total = std::accumulate(arg_counts_, arg_counts_ + 3, 0u);
  os << "Low arg savings: " << Percent(low_arg_total * 2, total_size) << "\n";
}

}  // namespace art
