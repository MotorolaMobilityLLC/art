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

#ifndef ART_RUNTIME_DEX_FILE_VERIFIER_H_
#define ART_RUNTIME_DEX_FILE_VERIFIER_H_

#include <unordered_set>

#include "dex_file.h"
#include "safe_map.h"

namespace art {

class DexFileVerifier {
 public:
  static bool Verify(const DexFile* dex_file, const uint8_t* begin, size_t size,
                     const char* location, std::string* error_msg);

  const std::string& FailureReason() const {
    return failure_reason_;
  }

 private:
  DexFileVerifier(const DexFile* dex_file, const uint8_t* begin, size_t size, const char* location)
      : dex_file_(dex_file), begin_(begin), size_(size), location_(location),
        header_(&dex_file->GetHeader()), ptr_(nullptr), previous_item_(nullptr)  {
  }

  bool Verify();

  bool CheckShortyDescriptorMatch(char shorty_char, const char* descriptor, bool is_return_type);
  bool CheckListSize(const void* start, size_t count, size_t element_size, const char* label);
  // Check a list. The head is assumed to be at *ptr, and elements to be of size element_size. If
  // successful, the ptr will be moved forward the amount covered by the list.
  bool CheckList(size_t element_size, const char* label, const uint8_t* *ptr);
  // Checks whether the offset is zero (when size is zero) or that the offset falls within the area
  // claimed by the file.
  bool CheckValidOffsetAndSize(uint32_t offset, uint32_t size, const char* label);
  bool CheckIndex(uint32_t field, uint32_t limit, const char* label);

  bool CheckHeader();
  bool CheckMap();

  uint32_t ReadUnsignedLittleEndian(uint32_t size);
  bool CheckAndGetHandlerOffsets(const DexFile::CodeItem* code_item,
                                 uint32_t* handler_offsets, uint32_t handlers_size);
  bool CheckClassDataItemField(uint32_t idx,
                               uint32_t access_flags,
                               uint32_t class_access_flags,
                               uint16_t class_type_index,
                               bool expect_static);
  bool CheckClassDataItemMethod(uint32_t idx,
                                uint32_t access_flags,
                                uint32_t class_access_flags,
                                uint16_t class_type_index,
                                uint32_t code_offset,
                                std::unordered_set<uint32_t>* direct_method_indexes,
                                bool expect_direct);
  bool CheckOrderAndGetClassFlags(bool is_field,
                                  const char* type_descr,
                                  uint32_t curr_index,
                                  uint32_t prev_index,
                                  bool* have_class,
                                  uint16_t* class_type_index,
                                  uint32_t* class_access_flags);

  bool CheckPadding(size_t offset, uint32_t aligned_offset);
  bool CheckEncodedValue();
  bool CheckEncodedArray();
  bool CheckEncodedAnnotation();

  bool CheckIntraClassDataItem();
  // Check all fields of the given type from the given iterator. Load the class data from the first
  // field, if necessary (and return it), or use the given values.
  template <bool kStatic>
  bool CheckIntraClassDataItemFields(ClassDataItemIterator* it,
                                     bool* have_class,
                                     uint16_t* class_type_index,
                                     uint32_t* class_access_flags);
  // Check all methods of the given type from the given iterator. Load the class data from the first
  // method, if necessary (and return it), or use the given values.
  template <bool kDirect>
  bool CheckIntraClassDataItemMethods(ClassDataItemIterator* it,
                                      std::unordered_set<uint32_t>* direct_method_indexes,
                                      bool* have_class,
                                      uint16_t* class_type_index,
                                      uint32_t* class_access_flags);

  bool CheckIntraCodeItem();
  bool CheckIntraStringDataItem();
  bool CheckIntraDebugInfoItem();
  bool CheckIntraAnnotationItem();
  bool CheckIntraAnnotationsDirectoryItem();

  bool CheckIntraSectionIterate(size_t offset, uint32_t count, uint16_t type);
  bool CheckIntraIdSection(size_t offset, uint32_t count, uint16_t type);
  bool CheckIntraDataSection(size_t offset, uint32_t count, uint16_t type);
  bool CheckIntraSection();

  bool CheckOffsetToTypeMap(size_t offset, uint16_t type);

  // Note: as sometimes kDexNoIndex16, being 0xFFFF, is a valid return value, we need an
  // additional out parameter to signal any errors loading an index.
  uint16_t FindFirstClassDataDefiner(const uint8_t* ptr, bool* success);
  uint16_t FindFirstAnnotationsDirectoryDefiner(const uint8_t* ptr, bool* success);

  bool CheckInterStringIdItem();
  bool CheckInterTypeIdItem();
  bool CheckInterProtoIdItem();
  bool CheckInterFieldIdItem();
  bool CheckInterMethodIdItem();
  bool CheckInterClassDefItem();
  bool CheckInterAnnotationSetRefList();
  bool CheckInterAnnotationSetItem();
  bool CheckInterClassDataItem();
  bool CheckInterAnnotationsDirectoryItem();

  bool CheckInterSectionIterate(size_t offset, uint32_t count, uint16_t type);
  bool CheckInterSection();

  // Load a string by (type) index. Checks whether the index is in bounds, printing the error if
  // not. If there is an error, null is returned.
  const char* CheckLoadStringByIdx(uint32_t idx, const char* error_fmt);
  const char* CheckLoadStringByTypeIdx(uint32_t type_idx, const char* error_fmt);

  // Load a field/method Id by index. Checks whether the index is in bounds, printing the error if
  // not. If there is an error, null is returned.
  const DexFile::FieldId* CheckLoadFieldId(uint32_t idx, const char* error_fmt);
  const DexFile::MethodId* CheckLoadMethodId(uint32_t idx, const char* error_fmt);

  void ErrorStringPrintf(const char* fmt, ...)
      __attribute__((__format__(__printf__, 2, 3))) COLD_ATTR;

  // Retrieve class index and class access flag from the given member. index is the member index,
  // which is taken as either a field or a method index (as designated by is_field). The result,
  // if the member and declaring class could be found, is stored in class_type_index and
  // class_access_flags.
  // This is an expensive lookup, as we have to find the class-def by type index, which is a
  // linear search. The output values should thus be cached by the caller.
  bool FindClassFlags(uint32_t index,
                      bool is_field,
                      uint16_t* class_type_index,
                      uint32_t* class_access_flags);

  // Check validity of the given access flags, interpreted for a field in the context of a class
  // with the given second access flags.
  static bool CheckFieldAccessFlags(uint32_t field_access_flags,
                                    uint32_t class_access_flags,
                                    std::string* error_msg);
  // Check validity of the given method and access flags, in the context of a class with the given
  // second access flags.
  bool CheckMethodAccessFlags(uint32_t method_index,
                              uint32_t method_access_flags,
                              uint32_t class_access_flags,
                              bool has_code,
                              bool expect_direct,
                              std::string* error_msg);

  const DexFile* const dex_file_;
  const uint8_t* const begin_;
  const size_t size_;
  const char* const location_;
  const DexFile::Header* const header_;

  struct OffsetTypeMapEmptyFn {
    // Make a hash map slot empty by making the offset 0. Offset 0 is a valid dex file offset that
    // is in the offset of the dex file header. However, we only store data section items in the
    // map, and these are after the header.
    void MakeEmpty(std::pair<uint32_t, uint16_t>& pair) const {
      pair.first = 0u;
    }
    // Check if a hash map slot is empty.
    bool IsEmpty(const std::pair<uint32_t, uint16_t>& pair) const {
      return pair.first == 0;
    }
  };
  struct OffsetTypeMapHashCompareFn {
    // Hash function for offset.
    size_t operator()(const uint32_t key) const {
      return key;
    }
    // std::equal function for offset.
    bool operator()(const uint32_t a, const uint32_t b) const {
      return a == b;
    }
  };
  // Map from offset to dex file type, HashMap for performance reasons.
  AllocationTrackingHashMap<uint32_t,
                            uint16_t,
                            OffsetTypeMapEmptyFn,
                            kAllocatorTagDexFileVerifier,
                            OffsetTypeMapHashCompareFn,
                            OffsetTypeMapHashCompareFn> offset_to_type_map_;
  const uint8_t* ptr_;
  const void* previous_item_;

  std::string failure_reason_;

  // Set of type ids for which there are ClassDef elements in the dex file.
  std::unordered_set<decltype(DexFile::ClassDef::class_idx_)> defined_classes_;
};

}  // namespace art

#endif  // ART_RUNTIME_DEX_FILE_VERIFIER_H_
