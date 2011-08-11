// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_FILE_H_
#define ART_SRC_DEX_FILE_H_

#include <map>
#include <vector>

#include "globals.h"
#include "leb128.h"
#include "logging.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "strutil.h"
#include "utils.h"

namespace art {

union JValue;

// TODO: move all of the macro functionality into the DexCache class.
class DexFile {
 public:
  static const byte kDexMagic[];
  static const byte kDexMagicVersion[];
  static const size_t kSha1DigestSize = 20;

  static const byte kEncodedValueTypeMask = 0x1f;  // 0b11111
  static const byte kEncodedValueArgShift = 5;

  // The value of an invalid index.
  static const uint32_t kDexNoIndex = 0xFFFFFFFF;

  enum ValueType {
    kByte = 0x00,
    kShort = 0x02,
    kChar = 0x03,
    kInt = 0x04,
    kLong = 0x06,
    kFloat = 0x10,
    kDouble = 0x11,
    kString = 0x17,
    kType = 0x18,
    kField = 0x19,
    kMethod = 0x1a,
    kEnum = 0x1b,
    kArray = 0x1c,
    kAnnotation = 0x1d,
    kNull = 0x1e,
    kBoolean = 0x1f
  };

  // Raw header_item.
  struct Header {
    uint8_t magic_[8];
    uint32_t checksum_;
    uint8_t signature_[kSha1DigestSize];
    uint32_t file_size_;  // length of entire file
    uint32_t header_size_;  // offset to start of next section
    uint32_t endian_tag_;
    uint32_t link_size_;
    uint32_t link_off_;
    uint32_t map_off_;
    uint32_t string_ids_size_;
    uint32_t string_ids_off_;
    uint32_t type_ids_size_;
    uint32_t type_ids_off_;
    uint32_t proto_ids_size_;
    uint32_t proto_ids_off_;
    uint32_t field_ids_size_;
    uint32_t field_ids_off_;
    uint32_t method_ids_size_;
    uint32_t method_ids_off_;
    uint32_t class_defs_size_;
    uint32_t class_defs_off_;
    uint32_t data_size_;
    uint32_t data_off_;
  };

  // Raw string_id_item.
  struct StringId {
    uint32_t string_data_off_;  // offset in bytes from the base address
  };

  // Raw type_id_item.
  struct TypeId {
    uint32_t descriptor_idx_;  // index into string_ids
  };

  // Raw field_id_item.
  struct FieldId {
    uint16_t class_idx_;  // index into type_ids_ list for defining class
    uint16_t type_idx_;  // index into type_ids_ for field type
    uint32_t name_idx_;  // index into string_ids_ for field name
  };

  // Raw method_id_item.
  struct MethodId {
    uint16_t class_idx_;  // index into type_ids_ list for defining class
    uint16_t proto_idx_;  // index into proto_ids_ for method prototype
    uint32_t name_idx_;  // index into string_ids_ for method name
  };

  // Raw proto_id_item.
  struct ProtoId {
    uint32_t shorty_idx_;  // index into string_ids for shorty descriptor
    uint32_t return_type_idx_;  // index into type_ids list for return type
    uint32_t parameters_off_;  // file offset to type_list for parameter types
  };

  // Raw class_def_item.
  struct ClassDef {
    uint32_t class_idx_;  // index into type_ids_ for this class
    uint32_t access_flags_;
    uint32_t superclass_idx_;  // index into type_ids_ for superclass
    uint32_t interfaces_off_;  // file offset to TypeList
    uint32_t source_file_idx_;  // index into string_ids_ for source file name
    uint32_t annotations_off_;  // file offset to annotations_directory_item
    uint32_t class_data_off_;  // file offset to class_data_item
    uint32_t static_values_off_;  // file offset to EncodedArray
  };

  // Raw type_item.
  struct TypeItem {
    uint16_t type_idx_;  // index into type_ids section
  };

  // Raw type_list.
  class TypeList {
   public:
    uint32_t Size() const {
      return size_;
    }

    const TypeItem& GetTypeItem(uint32_t idx) const {
      CHECK_LT(idx, this->size_);
      return this->list_[idx];
    }

   private:
    uint32_t size_;  // size of the list, in entries
    TypeItem list_[1];  // elements of the list
  };

  class ParameterIterator {  // TODO: stream
   public:
    ParameterIterator(const DexFile& dex_file, const ProtoId& proto_id)
        : dex_file_(dex_file), size_(0), pos_(0) {
      type_list_ = dex_file_.GetProtoParameters(proto_id);
      if (type_list_ != NULL) {
        size_ = type_list_->Size();
      }
    }
    bool HasNext() const { return pos_ != size_; }
    void Next() { ++pos_; }
    const char* GetDescriptor() {
      uint32_t type_idx = type_list_->GetTypeItem(pos_).type_idx_;
      return dex_file_.dexStringByTypeIdx(type_idx);
    }
   private:
    const DexFile& dex_file_;
    const TypeList* type_list_;
    uint32_t size_;
    uint32_t pos_;
    DISALLOW_IMPLICIT_CONSTRUCTORS(ParameterIterator);
  };

  ParameterIterator* GetParameterIterator(const ProtoId& proto_id) const {
    return new ParameterIterator(*this, proto_id);
  }

  const char* GetReturnTypeDescriptor(const ProtoId& proto_id) const {
    return dexStringByTypeIdx(proto_id.return_type_idx_);
  }

  // Raw code_item.
  struct CodeItem {
    uint16_t registers_size_;
    uint16_t ins_size_;
    uint16_t outs_size_;
    uint16_t tries_size_;
    uint32_t debug_info_off_;  // file offset to debug info stream
    uint32_t insns_size_;  // size of the insns array, in 2 byte code units
    uint16_t insns_[1];
  };

  struct CatchHandlerItem {
    uint32_t type_idx_;    // type index of the caught exception type
    uint32_t address_;     // handler address
  };

  // Raw try_item.
  struct TryItem {
    uint32_t start_addr_;
    uint16_t insn_count_;
    uint16_t handler_off_;
  };

  class CatchHandlerIterator {
    public:
      CatchHandlerIterator() {
        remaining_count_ = -1;
        catch_all_ = false;
      }

      CatchHandlerIterator(const byte* handler_data) {
        current_data_ = handler_data;
        remaining_count_ = DecodeUnsignedLeb128(&current_data_);

        // If remaining_count_ is non-positive, then it is the negative of
        // the number of catch types, and the catches are followed by a
        // catch-all handler.
        if (remaining_count_ <= 0) {
          catch_all_ = true;
          remaining_count_ = -remaining_count_;
        } else {
          catch_all_ = false;
        }
        Next();
      }

      CatchHandlerItem& Get() {
        return handler_;
      }

      void Next() {
        if (remaining_count_ > 0) {
          handler_.type_idx_ = DecodeUnsignedLeb128(&current_data_);
          handler_.address_  = DecodeUnsignedLeb128(&current_data_);
          remaining_count_--;
          return;
        }

        if (catch_all_) {
          handler_.type_idx_ = kDexNoIndex;
          handler_.address_  = DecodeUnsignedLeb128(&current_data_);
          catch_all_ = false;
          return;
        }

        // no more handler
        remaining_count_ = -1;
      }

      bool End() const {
        return remaining_count_ < 0 && catch_all_ == false;
      }

    private:
      CatchHandlerItem handler_;
      const byte *current_data_;  // the current handlder in dex file.
      int32_t remaining_count_;   // number of handler not read.
      bool catch_all_;            // is there a handler that will catch all exceptions in case
                                  // that all typed handler does not match.
  };

  // Partially decoded form of class_data_item.
  struct ClassDataHeader {
    uint32_t static_fields_size_;  // the number of static fields
    uint32_t instance_fields_size_;  // the number of instance fields
    uint32_t direct_methods_size_;  // the number of direct methods
    uint32_t virtual_methods_size_;  // the number of virtual methods
  };

  // Decoded form of encoded_field.
  struct Field {
    uint32_t field_idx_;  // index into the field_ids list for the identity of this field
    uint32_t access_flags_;  // access flags for the field
  };

  // Decoded form of encoded_method.
  struct Method {
    uint32_t method_idx_;
    uint32_t access_flags_;
    uint32_t code_off_;
  };

  typedef std::pair<const DexFile*, const DexFile::ClassDef*> ClassPathEntry;
  typedef std::vector<const DexFile*> ClassPath;

  // Search a collection of DexFiles for a descriptor
  static ClassPathEntry FindInClassPath(const StringPiece& descriptor,
                                        ClassPath& class_path);

  // Opens a .dex file from the file system.
  static DexFile* OpenFile(const std::string& filename);

  // Opens a .jar, .zip, or .apk file from the file system.
  static DexFile* OpenZip(const std::string& filename);

  // Opens a .dex file from a new allocated pointer
  static DexFile* OpenPtr(byte* ptr, size_t length);

  // Closes a .dex file.
  virtual ~DexFile();

  const Header& GetHeader() {
    CHECK(header_ != NULL);
    return *header_;
  }

  // Looks up a class definition by its class descriptor.
  const ClassDef* FindClassDef(const StringPiece& descriptor) const;

  // Returns the number of string identifiers in the .dex file.
  size_t NumStringIds() const {
    CHECK(header_ != NULL);
    return header_->string_ids_size_;
  }

  // Returns the number of type identifiers in the .dex file.
  size_t NumTypeIds() const {
    CHECK(header_ != NULL);
    return header_->type_ids_size_;
  }

  // Returns the number of prototype identifiers in the .dex file.
  size_t NumProtoIds() const {
    CHECK(header_ != NULL);
    return header_->proto_ids_size_;
  }

  // Returns the number of field identifiers in the .dex file.
  size_t NumFieldIds() const {
    CHECK(header_ != NULL);
    return header_->field_ids_size_;
  }

  // Returns the number of method identifiers in the .dex file.
  size_t NumMethodIds() const {
    CHECK(header_ != NULL);
    return header_->method_ids_size_;
  }

  // Returns the number of class definitions in the .dex file.
  size_t NumClassDefs() const {
    CHECK(header_ != NULL);
    return header_->class_defs_size_;
  }

  // Returns a pointer to the memory mapped class data.
  // TODO: return a stream
  const byte* GetClassData(const ClassDef& class_def) const {
    if (class_def.class_data_off_ == 0) {
      return NULL;
    } else {
      return base_ + class_def.class_data_off_;
    }
  }

  // Decodes the header section from the class data bytes.
  ClassDataHeader ReadClassDataHeader(const byte** class_data) const {
    CHECK(class_data != NULL);
    ClassDataHeader header;
    memset(&header, 0, sizeof(ClassDataHeader));
    if (*class_data != NULL) {
      header.static_fields_size_ = DecodeUnsignedLeb128(class_data);
      header.instance_fields_size_ = DecodeUnsignedLeb128(class_data);
      header.direct_methods_size_ = DecodeUnsignedLeb128(class_data);
      header.virtual_methods_size_ = DecodeUnsignedLeb128(class_data);
    }
    return header;
  }

  // Returns the class descriptor string of a class definition.
  const char* GetClassDescriptor(const ClassDef& class_def) const {
    return dexStringByTypeIdx(class_def.class_idx_);
  }

  // Returns the StringId at the specified index.
  const StringId& GetStringId(uint32_t idx) const {
    CHECK_LT(idx, NumStringIds());
    return string_ids_[idx];
  }

  // Returns the TypeId at the specified index.
  const TypeId& GetTypeId(uint32_t idx) const {
    CHECK_LT(idx, NumTypeIds());
    return type_ids_[idx];
  }

  // Returns the FieldId at the specified index.
  const FieldId& GetFieldId(uint32_t idx) const {
    CHECK_LT(idx, NumFieldIds());
    return field_ids_[idx];
  }

  // Returns the MethodId at the specified index.
  const MethodId& GetMethodId(uint32_t idx) const {
    CHECK_LT(idx, NumMethodIds());
    return method_ids_[idx];
  }

  // Returns the ProtoId at the specified index.
  const ProtoId& GetProtoId(uint32_t idx) const {
    CHECK_LT(idx, NumProtoIds());
    return proto_ids_[idx];
  }

  // Returns the ClassDef at the specified index.
  const ClassDef& GetClassDef(uint32_t idx) const {
    CHECK_LT(idx, NumClassDefs());
    return class_defs_[idx];
  }

  const TypeList* GetInterfacesList(const ClassDef& class_def) const {
    if (class_def.interfaces_off_ == 0) {
        return NULL;
    } else {
      const byte* addr = base_ + class_def.interfaces_off_;
      return reinterpret_cast<const TypeList*>(addr);
    }
  }

  const CodeItem* GetCodeItem(const Method& method) const {
    return GetCodeItem(method.code_off_);
  }

  const CodeItem* GetCodeItem(const uint32_t code_off_) const {
    if (code_off_ == 0) {
      return NULL;  // native or abstract method
    } else {
      const byte* addr = base_ + code_off_;
      return reinterpret_cast<const CodeItem*>(addr);
    }
  }

  // Returns the short form method descriptor for the given prototype.
  const char* GetShorty(uint32_t proto_idx) const {
    const ProtoId& proto_id = GetProtoId(proto_idx);
    return dexStringById(proto_id.shorty_idx_);
  }

  const TypeList* GetProtoParameters(const ProtoId& proto_id) const {
    if (proto_id.parameters_off_ == 0) {
      return NULL;
    } else {
      const byte* addr = base_ + proto_id.parameters_off_;
      return reinterpret_cast<const TypeList*>(addr);
    }
  }

  char* CreateMethodDescriptor(uint32_t proto_idx,
                               int32_t* unicode_length) const;

  const byte* GetEncodedArray(const ClassDef& class_def) const {
    if (class_def.static_values_off_ == 0) {
      return 0;
    } else {
      return base_ + class_def.static_values_off_;
    }
  }

  int32_t GetStringLength(const StringId& string_id) const {
    const byte* ptr = base_ + string_id.string_data_off_;
    return DecodeUnsignedLeb128(&ptr);
  }

  ValueType ReadEncodedValue(const byte** encoded_value, JValue* value) const;

  // From libdex...

  // Returns a pointer to the UTF-8 string data referred to by the
  // given string_id.
  const char* GetStringData(const StringId& string_id, int32_t* length) const {
    CHECK(length != NULL);
    const byte* ptr = base_ + string_id.string_data_off_;
    *length = DecodeUnsignedLeb128(&ptr);
    return reinterpret_cast<const char*>(ptr);
  }

  const char* GetStringData(const StringId& string_id) const {
    int32_t length;
    return GetStringData(string_id, &length);
  }

  // return the UTF-8 encoded string with the specified string_id index
  const char* dexStringById(uint32_t idx, int32_t* unicode_length) const {
    const StringId& string_id = GetStringId(idx);
    return GetStringData(string_id, unicode_length);
  }

  const char* dexStringById(uint32_t idx) const {
    int32_t unicode_length;
    return dexStringById(idx, &unicode_length);
  }

  // Get the descriptor string associated with a given type index.
  const char* dexStringByTypeIdx(uint32_t idx, int32_t* unicode_length) const {
    const TypeId& type_id = GetTypeId(idx);
    return dexStringById(type_id.descriptor_idx_, unicode_length);
  }

  const char* dexStringByTypeIdx(uint32_t idx) const {
    const TypeId& type_id = GetTypeId(idx);
    return dexStringById(type_id.descriptor_idx_);
  }

  // TODO: encoded_field is actually a stream of bytes
  void dexReadClassDataField(const byte** encoded_field,
                             DexFile::Field* field,
                             uint32_t* last_idx) const {
    uint32_t idx = *last_idx + DecodeUnsignedLeb128(encoded_field);
    field->access_flags_ = DecodeUnsignedLeb128(encoded_field);
    field->field_idx_ = idx;
    *last_idx = idx;
  }

  // TODO: encoded_method is actually a stream of bytes
  void dexReadClassDataMethod(const byte** encoded_method,
                              DexFile::Method* method,
                              uint32_t* last_idx) const {
    uint32_t idx = *last_idx + DecodeUnsignedLeb128(encoded_method);
    method->access_flags_ = DecodeUnsignedLeb128(encoded_method);
    method->code_off_ = DecodeUnsignedLeb128(encoded_method);
    method->method_idx_ = idx;
    *last_idx = idx;
  }

  const TryItem* dexGetTryItems(const CodeItem& code_item, uint32_t offset) const {
    const uint16_t* insns_end_ = &code_item.insns_[code_item.insns_size_];
    return reinterpret_cast<const TryItem*>
        (RoundUp(reinterpret_cast<uint32_t>(insns_end_), 4)) + offset;
  }

  // Get the base of the encoded data for the given DexCode.
  const byte* dexGetCatchHandlerData(const CodeItem& code_item, uint32_t offset) const {
    const byte* handler_data = reinterpret_cast<const byte*>
        (dexGetTryItems(code_item, code_item.tries_size_));
    return handler_data + offset;
  }

  // Find the handler associated with a given address, if any.
  // Initializes the given iterator and returns true if a match is
  // found. Returns end if there is no applicable handler.
  CatchHandlerIterator dexFindCatchHandler(const CodeItem& code_item, uint32_t address) const {
    CatchHandlerItem handler;
    handler.address_ = -1;
    int32_t offset = -1;

    // Short-circuit the overwhelmingly common cases.
    switch (code_item.tries_size_) {
      case 0:
        break;
      case 1: {
        const TryItem* tries = dexGetTryItems(code_item, 0);
        uint32_t start = tries->start_addr_;
        if (address < start)
          break;

        uint32_t end = start + tries->insn_count_;
        if (address >= end)
          break;

        offset = tries->handler_off_;
        break;
      }
      default:
        offset = dexFindCatchHandlerOffset0(code_item, code_item.tries_size_, address);
    }

    if (offset >= 0) {
      const byte* handler_data = dexGetCatchHandlerData(code_item, offset);
      return CatchHandlerIterator(handler_data);
    }
    return CatchHandlerIterator();
  }

  int32_t dexFindCatchHandlerOffset0(const CodeItem &code_item,
                                     int32_t tries_size,
                                     uint32_t address) const {
    // Note: Signed type is important for max and min.
    int32_t min = 0;
    int32_t max = tries_size - 1;

    while (max >= min) {
      int32_t guess = (min + max) >> 1;
      const TryItem* pTry = dexGetTryItems(code_item, guess);
      uint32_t start = pTry->start_addr_;

      if (address < start) {
        max = guess - 1;
        continue;
      }

      uint32_t end = start + pTry->insn_count_;
      if (address >= end) {
        min = guess + 1;
        continue;
      }

      // We have a winner!
      return (int32_t) pTry->handler_off_;
    }

    // No match.
    return -1;
  }


  // TODO: const reference
  uint32_t dexGetIndexForClassDef(const ClassDef* class_def) const {
    CHECK_GE(class_def, class_defs_);
    CHECK_LT(class_def, class_defs_ + header_->class_defs_size_);
    return class_def - class_defs_;
  }

  const char* dexGetSourceFile(const ClassDef& class_def) const {
    if (class_def.source_file_idx_ == 0xffffffff) {
      return NULL;
    } else {
      return dexStringById(class_def.source_file_idx_);
    }
  }

 private:
  // Helper class to deallocate underlying storage.
  class Closer {
   public:
    virtual ~Closer();
  };

  // Helper class to deallocate mmap-backed .dex files.
  class MmapCloser : public Closer {
   public:
    MmapCloser(void* addr, size_t length);
    virtual ~MmapCloser();
   private:
    void* addr_;
    size_t length_;
  };

  // Helper class for deallocating new/delete-backed .dex files.
  class PtrCloser : public Closer {
   public:
    PtrCloser(byte* addr);
    virtual ~PtrCloser();
   private:
    byte* addr_;
  };

  // Opens a .dex file at a the given address.
  static DexFile* Open(const byte* dex_file, size_t length, Closer* closer);

  DexFile(const byte* addr, size_t length, Closer* closer)
      : base_(addr),
        length_(length),
        closer_(closer),
        header_(0),
        string_ids_(0),
        type_ids_(0),
        field_ids_(0),
        method_ids_(0),
        proto_ids_(0),
        class_defs_(0) {}

  // Top-level initializer that calls other Init methods.
  bool Init();

  // Caches pointers into to the various file sections.
  void InitMembers();

  // Builds the index of descriptors to class definitions.
  void InitIndex();

  // Returns true if the byte string equals the magic value.
  bool CheckMagic(const byte* magic);

  // Returns true if the header magic is of the expected value.
  bool IsMagicValid();

  // The index of descriptors to class definitions.
  typedef std::map<const StringPiece, const DexFile::ClassDef*> Index;
  Index index_;

  // The base address of the memory mapping.
  const byte* base_;

  // The size of the underlying memory allocation in bytes.
  size_t length_;

  // Helper object to free the underlying allocation.
  scoped_ptr<Closer> closer_;

  // Points to the header section.
  const Header* header_;

  // Points to the base of the string identifier list.
  const StringId* string_ids_;

  // Points to the base of the type identifier list.
  const TypeId* type_ids_;

  // Points to the base of the field identifier list.
  const FieldId* field_ids_;

  // Points to the base of the method identifier list.
  const MethodId* method_ids_;

  // Points to the base of the prototype identifier list.
  const ProtoId* proto_ids_;

  // Points to the base of the class definition list.
  const ClassDef* class_defs_;
};

}  // namespace art

#endif  // ART_SRC_DEX_FILE_H_
