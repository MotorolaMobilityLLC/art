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
 *
 * Implementation file of the dex layout visualization.
 *
 * This is a tool to read dex files into an internal representation,
 * reorganize the representation, and emit dex files with a better
 * file layout.
 */

#include "dex_visualize.h"

#include <inttypes.h>
#include <stdio.h>

#include <functional>
#include <memory>
#include <vector>

#include "dex_ir.h"
#include "dexlayout.h"
#include "jit/profile_compilation_info.h"

namespace art {

static std::string MultidexName(const std::string& prefix,
                                size_t dex_file_index,
                                const std::string& suffix) {
  return prefix + ((dex_file_index > 0) ? std::to_string(dex_file_index + 1) : "") + suffix;
}

struct FileSection {
 public:
  std::string name_;
  uint16_t type_;
  std::function<uint32_t(const dex_ir::Collections&)> size_fn_;
  std::function<uint32_t(const dex_ir::Collections&)> offset_fn_;
};

static uint32_t HeaderOffset(const dex_ir::Collections& collections ATTRIBUTE_UNUSED) {
  return 0;
}

static uint32_t HeaderSize(const dex_ir::Collections& collections ATTRIBUTE_UNUSED) {
  // Size is in elements, so there is only one header.
  return 1;
}

static const std::vector<FileSection> kFileSections = {
  {
    "Header",
    DexFile::kDexTypeHeaderItem,
    &HeaderSize,
    &HeaderOffset,
  }, {
    "StringId",
    DexFile::kDexTypeStringIdItem,
    &dex_ir::Collections::StringIdsSize,
    &dex_ir::Collections::StringIdsOffset
  }, {
    "TypeId",
    DexFile::kDexTypeTypeIdItem,
    &dex_ir::Collections::TypeIdsSize,
    &dex_ir::Collections::TypeIdsOffset
  }, {
    "ProtoId",
    DexFile::kDexTypeProtoIdItem,
    &dex_ir::Collections::ProtoIdsSize,
    &dex_ir::Collections::ProtoIdsOffset
  }, {
    "FieldId",
    DexFile::kDexTypeFieldIdItem,
    &dex_ir::Collections::FieldIdsSize,
    &dex_ir::Collections::FieldIdsOffset
  }, {
    "MethodId",
    DexFile::kDexTypeMethodIdItem,
    &dex_ir::Collections::MethodIdsSize,
    &dex_ir::Collections::MethodIdsOffset
  }, {
    "ClassDef",
    DexFile::kDexTypeClassDefItem,
    &dex_ir::Collections::ClassDefsSize,
    &dex_ir::Collections::ClassDefsOffset
  }, {
    "StringData",
    DexFile::kDexTypeStringDataItem,
    &dex_ir::Collections::StringDatasSize,
    &dex_ir::Collections::StringDatasOffset
  }, {
    "TypeList",
    DexFile::kDexTypeTypeList,
    &dex_ir::Collections::TypeListsSize,
    &dex_ir::Collections::TypeListsOffset
  }, {
    "EncArr",
    DexFile::kDexTypeEncodedArrayItem,
    &dex_ir::Collections::EncodedArrayItemsSize,
    &dex_ir::Collections::EncodedArrayItemsOffset
  }, {
    "Annotation",
    DexFile::kDexTypeAnnotationItem,
    &dex_ir::Collections::AnnotationItemsSize,
    &dex_ir::Collections::AnnotationItemsOffset
  }, {
    "AnnoSet",
    DexFile::kDexTypeAnnotationSetItem,
    &dex_ir::Collections::AnnotationSetItemsSize,
    &dex_ir::Collections::AnnotationSetItemsOffset
  }, {
    "AnnoSetRL",
    DexFile::kDexTypeAnnotationSetRefList,
    &dex_ir::Collections::AnnotationSetRefListsSize,
    &dex_ir::Collections::AnnotationSetRefListsOffset
  }, {
    "AnnoDir",
    DexFile::kDexTypeAnnotationsDirectoryItem,
    &dex_ir::Collections::AnnotationsDirectoryItemsSize,
    &dex_ir::Collections::AnnotationsDirectoryItemsOffset
  }, {
    "DebugInfo",
    DexFile::kDexTypeDebugInfoItem,
    &dex_ir::Collections::DebugInfoItemsSize,
    &dex_ir::Collections::DebugInfoItemsOffset
  }, {
    "CodeItem",
    DexFile::kDexTypeCodeItem,
    &dex_ir::Collections::CodeItemsSize,
    &dex_ir::Collections::CodeItemsOffset
  }, {
    "ClassData",
    DexFile::kDexTypeClassDataItem,
    &dex_ir::Collections::ClassDatasSize,
    &dex_ir::Collections::ClassDatasOffset
  }
};

static constexpr bool kSortAscending = false;
static constexpr bool kSortDescending = true;

static std::vector<const FileSection*> GetSortedSections(
    const dex_ir::Collections& collections,
    bool sort_descending) {
  std::vector<const FileSection*> sorted_sections;
  // Build the table that will map from offset to color
  for (const FileSection& s : kFileSections) {
    sorted_sections.push_back(&s);
  }
  // Sort by offset.
  std::sort(sorted_sections.begin(),
            sorted_sections.end(),
            [&](const FileSection* a, const FileSection* b) {
              if (sort_descending) {
                return a->offset_fn_(collections) > b->offset_fn_(collections);
              } else {
                return a->offset_fn_(collections) < b->offset_fn_(collections);
              }
            });
  return sorted_sections;
}

class Dumper {
 public:
  // Colors are based on the type of the section in MapList.
  explicit Dumper(const dex_ir::Collections& collections)
      : collections_(collections), out_file_(nullptr),
        sorted_sections_(GetSortedSections(collections, kSortDescending)) { }

  bool OpenAndPrintHeader(size_t dex_index) {
    // Open the file and emit the gnuplot prologue.
    out_file_ = fopen(MultidexName("layout", dex_index, ".gnuplot").c_str(), "w");
    if (out_file_ == nullptr) {
      return false;
    }
    fprintf(out_file_, "set terminal png size 1920,1080\n");
    fprintf(out_file_, "set output \"%s\"\n", MultidexName("layout", dex_index, ".png").c_str());
    fprintf(out_file_, "set title \"%s\"\n", MultidexName("classes", dex_index, ".dex").c_str());
    fprintf(out_file_, "set xlabel \"Page offset into dex\"\n");
    fprintf(out_file_, "set ylabel \"ClassDef index\"\n");
    fprintf(out_file_, "set xtics rotate out (");
    bool printed_one = false;
    for (const FileSection& s : kFileSections) {
      if (s.size_fn_(collections_) > 0) {
        if (printed_one) {
          fprintf(out_file_, ", ");
        }
        fprintf(out_file_, "\"%s\" %d", s.name_.c_str(), s.offset_fn_(collections_) / kPageSize);
        printed_one = true;
      }
    }
    fprintf(out_file_, ")\n");
    fprintf(out_file_,
            "plot \"-\" using 1:2:3:4:5 with vector nohead linewidth 1 lc variable notitle\n");
    return true;
  }

  int GetColor(uint32_t offset) const {
    // The dread linear search to find the right section for the reference.
    uint16_t section = 0;
    for (const FileSection* file_section : sorted_sections_) {
      if (file_section->offset_fn_(collections_) < offset) {
        section = file_section->type_;
        break;
      }
    }
    // And a lookup table from type to color.
    ColorMapType::const_iterator iter = kColorMap.find(section);
    if (iter != kColorMap.end()) {
      return iter->second;
    }
    return 0;
  }

  void DumpAddressRange(uint32_t from, uint32_t size, int class_index) {
    const uint32_t low_page = from / kPageSize;
    const uint32_t high_page = (size > 0) ? (from + size - 1) / kPageSize : low_page;
    const uint32_t size_delta = high_page - low_page;
    fprintf(out_file_, "%d %d %d 0 %d\n", low_page, class_index, size_delta, GetColor(from));
  }

  void DumpAddressRange(const dex_ir::Item* item, int class_index) {
    if (item != nullptr) {
      DumpAddressRange(item->GetOffset(), item->GetSize(), class_index);
    }
  }

  void DumpStringData(const dex_ir::StringData* string_data, int class_index) {
    DumpAddressRange(string_data, class_index);
  }

  void DumpStringId(const dex_ir::StringId* string_id, int class_index) {
    DumpAddressRange(string_id, class_index);
    if (string_id == nullptr) {
      return;
    }
    DumpStringData(string_id->DataItem(), class_index);
  }

  void DumpTypeId(const dex_ir::TypeId* type_id, int class_index) {
    DumpAddressRange(type_id, class_index);
    DumpStringId(type_id->GetStringId(), class_index);
  }

  void DumpFieldId(const dex_ir::FieldId* field_id, int class_index) {
    DumpAddressRange(field_id, class_index);
    if (field_id == nullptr) {
      return;
    }
    DumpTypeId(field_id->Class(), class_index);
    DumpTypeId(field_id->Type(), class_index);
    DumpStringId(field_id->Name(), class_index);
  }

  void DumpFieldItem(const dex_ir::FieldItem* field, int class_index) {
    DumpAddressRange(field, class_index);
    if (field == nullptr) {
      return;
    }
    DumpFieldId(field->GetFieldId(), class_index);
  }

  void DumpProtoId(const dex_ir::ProtoId* proto_id, int class_index) {
    DumpAddressRange(proto_id, class_index);
    if (proto_id == nullptr) {
      return;
    }
    DumpStringId(proto_id->Shorty(), class_index);
    const dex_ir::TypeList* type_list = proto_id->Parameters();
    if (type_list != nullptr) {
      for (const dex_ir::TypeId* t : *type_list->GetTypeList()) {
        DumpTypeId(t, class_index);
      }
    }
    DumpTypeId(proto_id->ReturnType(), class_index);
  }

  void DumpMethodId(const dex_ir::MethodId* method_id, int class_index) {
    DumpAddressRange(method_id, class_index);
    if (method_id == nullptr) {
      return;
    }
    DumpTypeId(method_id->Class(), class_index);
    DumpProtoId(method_id->Proto(), class_index);
    DumpStringId(method_id->Name(), class_index);
  }

  void DumpMethodItem(dex_ir::MethodItem* method,
                      const DexFile* dex_file,
                      int class_index,
                      ProfileCompilationInfo* profile_info) {
    if (profile_info != nullptr) {
      uint32_t method_idx = method->GetMethodId()->GetIndex();
      if (!profile_info->ContainsMethod(MethodReference(dex_file, method_idx))) {
        return;
      }
    }
    DumpAddressRange(method, class_index);
    if (method == nullptr) {
      return;
    }
    DumpMethodId(method->GetMethodId(), class_index);
    const dex_ir::CodeItem* code_item = method->GetCodeItem();
    if (code_item != nullptr) {
      DumpAddressRange(code_item, class_index);
      const dex_ir::CodeFixups* fixups = code_item->GetCodeFixups();
      if (fixups != nullptr) {
        std::vector<dex_ir::TypeId*>* type_ids = fixups->TypeIds();
        for (dex_ir::TypeId* type_id : *type_ids) {
          DumpTypeId(type_id, class_index);
        }
        std::vector<dex_ir::StringId*>* string_ids = fixups->StringIds();
        for (dex_ir::StringId* string_id : *string_ids) {
          DumpStringId(string_id, class_index);
        }
        std::vector<dex_ir::MethodId*>* method_ids = fixups->MethodIds();
        for (dex_ir::MethodId* method_id : *method_ids) {
          DumpMethodId(method_id, class_index);
        }
        std::vector<dex_ir::FieldId*>* field_ids = fixups->FieldIds();
        for (dex_ir::FieldId* field_id : *field_ids) {
          DumpFieldId(field_id, class_index);
        }
      }
    }
  }

  ~Dumper() {
    fclose(out_file_);
  }

 private:
  using ColorMapType = std::map<uint16_t, int>;
  const ColorMapType kColorMap = {
    { DexFile::kDexTypeHeaderItem, 1 },
    { DexFile::kDexTypeStringIdItem, 2 },
    { DexFile::kDexTypeTypeIdItem, 3 },
    { DexFile::kDexTypeProtoIdItem, 4 },
    { DexFile::kDexTypeFieldIdItem, 5 },
    { DexFile::kDexTypeMethodIdItem, 6 },
    { DexFile::kDexTypeClassDefItem, 7 },
    { DexFile::kDexTypeTypeList, 8 },
    { DexFile::kDexTypeAnnotationSetRefList, 9 },
    { DexFile::kDexTypeAnnotationSetItem, 10 },
    { DexFile::kDexTypeClassDataItem, 11 },
    { DexFile::kDexTypeCodeItem, 12 },
    { DexFile::kDexTypeStringDataItem, 13 },
    { DexFile::kDexTypeDebugInfoItem, 14 },
    { DexFile::kDexTypeAnnotationItem, 15 },
    { DexFile::kDexTypeEncodedArrayItem, 16 },
    { DexFile::kDexTypeAnnotationsDirectoryItem, 16 }
  };

  const dex_ir::Collections& collections_;
  FILE* out_file_;
  std::vector<const FileSection*> sorted_sections_;

  DISALLOW_COPY_AND_ASSIGN(Dumper);
};

/*
 * Dumps a gnuplot data file showing the parts of the dex_file that belong to each class.
 * If profiling information is present, it dumps only those classes that are marked as hot.
 */
void VisualizeDexLayout(dex_ir::Header* header,
                        const DexFile* dex_file,
                        size_t dex_file_index,
                        ProfileCompilationInfo* profile_info) {
  std::unique_ptr<Dumper> dumper(new Dumper(header->GetCollections()));
  if (!dumper->OpenAndPrintHeader(dex_file_index)) {
    fprintf(stderr, "Could not open output file.\n");
    return;
  }

  const uint32_t class_defs_size = header->GetCollections().ClassDefsSize();
  for (uint32_t class_index = 0; class_index < class_defs_size; class_index++) {
    dex_ir::ClassDef* class_def = header->GetCollections().GetClassDef(class_index);
    dex::TypeIndex type_idx(class_def->ClassType()->GetIndex());
    if (profile_info != nullptr && !profile_info->ContainsClass(*dex_file, type_idx)) {
      continue;
    }
    dumper->DumpAddressRange(class_def, class_index);
    // Type id.
    dumper->DumpTypeId(class_def->ClassType(), class_index);
    // Superclass type id.
    dumper->DumpTypeId(class_def->Superclass(), class_index);
    // Interfaces.
    // TODO(jeffhao): get TypeList from class_def to use Item interface.
    static constexpr uint32_t kInterfaceSizeKludge = 8;
    dumper->DumpAddressRange(class_def->InterfacesOffset(), kInterfaceSizeKludge, class_index);
    // Source file info.
    dumper->DumpStringId(class_def->SourceFile(), class_index);
    // Annotations.
    dumper->DumpAddressRange(class_def->Annotations(), class_index);
    // TODO(sehr): walk the annotations and dump them.
    // Class data.
    dex_ir::ClassData* class_data = class_def->GetClassData();
    if (class_data != nullptr) {
      dumper->DumpAddressRange(class_data, class_index);
      if (class_data->StaticFields()) {
        for (auto& field_item : *class_data->StaticFields()) {
          dumper->DumpFieldItem(field_item.get(), class_index);
        }
      }
      if (class_data->InstanceFields()) {
        for (auto& field_item : *class_data->InstanceFields()) {
          dumper->DumpFieldItem(field_item.get(), class_index);
        }
      }
      if (class_data->DirectMethods()) {
        for (auto& method_item : *class_data->DirectMethods()) {
          dumper->DumpMethodItem(method_item.get(), dex_file, class_index, profile_info);
        }
      }
      if (class_data->VirtualMethods()) {
        for (auto& method_item : *class_data->VirtualMethods()) {
          dumper->DumpMethodItem(method_item.get(), dex_file, class_index, profile_info);
        }
      }
    }
  }  // for
}

static uint32_t FindNextByteAfterSection(dex_ir::Header* header,
                                         const dex_ir::Collections& collections,
                                         std::vector<const FileSection*>& sorted_sections,
                                         size_t section_index) {
  for (size_t i = section_index + 1; i < sorted_sections.size(); ++i) {
    const FileSection* section = sorted_sections[i];
    if (section->size_fn_(collections) != 0) {
      return section->offset_fn_(collections);
    }
  }
  return header->FileSize();
}

/*
 * Dumps the offset and size of sections within the file.
 */
void ShowDexSectionStatistics(dex_ir::Header* header, size_t dex_file_index) {
  // Compute the (multidex) class file name).
  fprintf(stdout, "%s (%d bytes)\n",
          MultidexName("classes", dex_file_index, ".dex").c_str(),
          header->FileSize());
  fprintf(stdout, "section      offset    items    bytes    pages pct\n");
  const dex_ir::Collections& collections = header->GetCollections();
  std::vector<const FileSection*> sorted_sections(GetSortedSections(collections, kSortAscending));
  for (size_t i = 0; i < sorted_sections.size(); ++i) {
    const FileSection* file_section = sorted_sections[i];
    const char* name = file_section->name_.c_str();
    uint32_t offset = file_section->offset_fn_(collections);
    uint32_t items = file_section->size_fn_(collections);
    uint32_t bytes = 0;
    if (items > 0) {
      bytes = FindNextByteAfterSection(header, collections, sorted_sections, i) - offset;
    }
    fprintf(stdout, "%-10s %8d %8d %8d %8d %%%02d\n", name, offset, items, bytes,
            (bytes + kPageSize - 1) / kPageSize, 100 * bytes / header->FileSize());
  }
  fprintf(stdout, "\n");
}

}  // namespace art
