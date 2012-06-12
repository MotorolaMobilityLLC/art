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

#include "dex_file.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "class_linker.h"
#include "dex_file_verifier.h"
#include "globals.h"
#include "leb128.h"
#include "logging.h"
#include "object.h"
#include "os.h"
#include "safe_map.h"
#include "stringprintf.h"
#include "thread.h"
#include "UniquePtr.h"
#include "utf.h"
#include "utils.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

const byte DexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const byte DexFile::kDexMagicVersion[] = { '0', '3', '5', '\0' };

DexFile::ClassPathEntry DexFile::FindInClassPath(const StringPiece& descriptor,
                                                 const ClassPath& class_path) {
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
    if (dex_class_def != NULL) {
      return ClassPathEntry(dex_file, dex_class_def);
    }
  }
  // TODO: remove reinterpret_cast when issue with -std=gnu++0x host issue resolved
  return ClassPathEntry(reinterpret_cast<const DexFile*>(NULL),
                        reinterpret_cast<const DexFile::ClassDef*>(NULL));
}

bool DexFile::GetChecksum(const std::string& filename, uint32_t& checksum) {
  if (IsValidZipFilename(filename)) {
    UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(filename));
    if (zip_archive.get() == NULL) {
      return false;
    }
    UniquePtr<ZipEntry> zip_entry(zip_archive->Find(kClassesDex));
    if (zip_entry.get() == NULL) {
      return false;
    }
    checksum = zip_entry->GetCrc32();
    return true;
  }
  if (IsValidDexFilename(filename)) {
    UniquePtr<const DexFile> dex_file(DexFile::OpenFile(filename, filename, false));
    if (dex_file.get() == NULL) {
      return false;
    }
    checksum = dex_file->GetHeader().checksum_;
    return true;
  }
  return false;
}

const DexFile* DexFile::Open(const std::string& filename,
                             const std::string& location) {
  if (IsValidZipFilename(filename)) {
    return DexFile::OpenZip(filename, location);
  }
  if (!IsValidDexFilename(filename)) {
    LOG(WARNING) << "Attempting to open dex file with unknown extension '" << filename << "'";
  }
  return DexFile::OpenFile(filename, location, true);
}

void DexFile::ChangePermissions(int prot) const {
  CHECK(mem_map_->Protect(prot)) << GetLocation();
}

const DexFile* DexFile::OpenFile(const std::string& filename,
                                 const std::string& location,
                                 bool verify) {
  CHECK(!location.empty()) << filename;
  int fd = open(filename.c_str(), O_RDONLY);  // TODO: scoped_fd
  if (fd == -1) {
    PLOG(ERROR) << "open(\"" << filename << "\", O_RDONLY) failed";
    return NULL;
  }
  struct stat sbuf;
  memset(&sbuf, 0, sizeof(sbuf));
  if (fstat(fd, &sbuf) == -1) {
    PLOG(ERROR) << "fstat \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  if (S_ISDIR(sbuf.st_mode)) {
    LOG(ERROR) << "attempt to mmap directory \"" << filename << "\"";
    return NULL;
  }
  size_t length = sbuf.st_size;
  UniquePtr<MemMap> map(MemMap::MapFile(length, PROT_READ, MAP_PRIVATE, fd, 0));
  if (map.get() == NULL) {
    LOG(ERROR) << "mmap \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  close(fd);

  if (map->Size() < sizeof(DexFile::Header)) {
    LOG(ERROR) << "Failed to open dex file '" << filename << "' that is too short to have a header";
    return NULL;
  }

  const Header* dex_header = reinterpret_cast<const Header*>(map->Begin());

  const DexFile* dex_file = OpenMemory(location, dex_header->checksum_, map.release());
  if (dex_file == NULL) {
    LOG(ERROR) << "Failed to open dex file '" << filename << "' from memory";
    return NULL;
  }

  if (verify && !DexFileVerifier::Verify(dex_file, dex_file->Begin(), dex_file->Size())) {
    LOG(ERROR) << "Failed to verify dex file '" << filename << "'";
    return NULL;
  }

  return dex_file;
}

const char* DexFile::kClassesDex = "classes.dex";

// Open classes.dex from within a .zip, .jar, .apk, ...
const DexFile* DexFile::OpenZip(const std::string& filename,
                                const std::string& location) {
  UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(filename));
  if (zip_archive.get() == NULL) {
    LOG(ERROR) << "Failed to open " << filename << " when looking for classes.dex";
    return NULL;
  }
  return DexFile::Open(*zip_archive.get(), location);
}

const DexFile* DexFile::Open(const ZipArchive& zip_archive, const std::string& location) {
  CHECK(!location.empty());
  UniquePtr<ZipEntry> zip_entry(zip_archive.Find(kClassesDex));
  if (zip_entry.get() == NULL) {
    LOG(ERROR) << "Failed to find classes.dex within " << location;
    return NULL;
  }

  uint32_t length = zip_entry->GetUncompressedLength();
  std::string name("classes.dex extracted in memory from ");
  name += location;
  UniquePtr<MemMap> map(MemMap::MapAnonymous(name.c_str(), NULL, length, PROT_READ | PROT_WRITE));
  if (map.get() == NULL) {
    LOG(ERROR) << "mmap classes.dex for \"" << location << "\" failed";
    return NULL;
  }

  // Extract classes.dex
  bool success = zip_entry->ExtractToMemory(*map.get());
  if (!success) {
    LOG(ERROR) << "Failed to extract classes.dex from '" << location << "' to memory";
    return NULL;
  }

  const DexFile* dex_file = OpenMemory(location, zip_entry->GetCrc32(), map.release());
  if (dex_file == NULL) {
    LOG(ERROR) << "Failed to open dex file '" << location << "' from memory";
    return NULL;
  }

  if (!DexFileVerifier::Verify(dex_file, dex_file->Begin(), dex_file->Size())) {
    LOG(ERROR) << "Failed to verify dex file '" << location << "'";
    return NULL;
  }

  return dex_file;
}

const DexFile* DexFile::OpenMemory(const byte* base,
                                   size_t size,
                                   const std::string& location,
                                   uint32_t location_checksum,
                                   MemMap* mem_map) {
  CHECK_ALIGNED(base, 4); // various dex file structures must be word aligned
  UniquePtr<DexFile> dex_file(new DexFile(base, size, location, location_checksum, mem_map));
  if (!dex_file->Init()) {
    return NULL;
  } else {
    return dex_file.release();
  }
}

DexFile::~DexFile() {
  // We don't call DeleteGlobalRef on dex_object_ because we're only called by DestroyJavaVM, and
  // that's only called after DetachCurrentThread, which means there's no JNIEnv. We could
  // re-attach, but cleaning up these global references is not obviously useful. It's not as if
  // the global reference table is otherwise empty!
}

jobject DexFile::GetDexObject(JNIEnv* env) const {
  MutexLock mu(dex_object_lock_);
  if (dex_object_ != NULL) {
    return dex_object_;
  }

  void* address = const_cast<void*>(reinterpret_cast<const void*>(begin_));
  jobject byte_buffer = env->NewDirectByteBuffer(address, size_);
  if (byte_buffer == NULL) {
    return NULL;
  }

  jvalue args[1];
  args[0].l = byte_buffer;
  jobject local = env->CallStaticObjectMethodA(WellKnownClasses::com_android_dex_Dex,
                                               WellKnownClasses::com_android_dex_Dex_create,
                                               args);
  if (local == NULL) {
    return NULL;
  }

  dex_object_ = env->NewGlobalRef(local);
  return dex_object_;
}

bool DexFile::Init() {
  InitMembers();
  if (!CheckMagicAndVersion()) {
    return false;
  }
  InitIndex();
  return true;
}

void DexFile::InitMembers() {
  const byte* b = begin_;
  header_ = reinterpret_cast<const Header*>(b);
  const Header* h = header_;
  string_ids_ = reinterpret_cast<const StringId*>(b + h->string_ids_off_);
  type_ids_ = reinterpret_cast<const TypeId*>(b + h->type_ids_off_);
  field_ids_ = reinterpret_cast<const FieldId*>(b + h->field_ids_off_);
  method_ids_ = reinterpret_cast<const MethodId*>(b + h->method_ids_off_);
  proto_ids_ = reinterpret_cast<const ProtoId*>(b + h->proto_ids_off_);
  class_defs_ = reinterpret_cast<const ClassDef*>(b + h->class_defs_off_);
  DCHECK_EQ(size_, header_->file_size_);
}

bool DexFile::CheckMagicAndVersion() const {
  CHECK(header_->magic_ != NULL) << GetLocation();
  if (!IsMagicValid(header_->magic_)) {
    LOG(ERROR) << "Unrecognized magic number in "  << GetLocation() << ":"
            << " " << header_->magic_[0]
            << " " << header_->magic_[1]
            << " " << header_->magic_[2]
            << " " << header_->magic_[3];
    return false;
  }
  if (!IsVersionValid(header_->magic_)) {
    LOG(ERROR) << "Unrecognized version number in "  << GetLocation() << ":"
            << " " << header_->magic_[4]
            << " " << header_->magic_[5]
            << " " << header_->magic_[6]
            << " " << header_->magic_[7];
    return false;
  }
  return true;
}

bool DexFile::IsMagicValid(const byte* magic) {
  return (memcmp(magic, kDexMagic, sizeof(kDexMagic)) == 0);
}

bool DexFile::IsVersionValid(const byte* magic) {
  const byte* version = &magic[sizeof(kDexMagic)];
  return (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) == 0);
}

uint32_t DexFile::GetVersion() const {
  const char* version = reinterpret_cast<const char*>(&GetHeader().magic_[sizeof(kDexMagic)]);
  return atoi(version);
}

int32_t DexFile::GetStringLength(const StringId& string_id) const {
  const byte* ptr = begin_ + string_id.string_data_off_;
  return DecodeUnsignedLeb128(&ptr);
}

// Returns a pointer to the UTF-8 string data referred to by the given string_id.
const char* DexFile::GetStringDataAndLength(const StringId& string_id, uint32_t* length) const {
  DCHECK(length != NULL) << GetLocation();
  const byte* ptr = begin_ + string_id.string_data_off_;
  *length = DecodeUnsignedLeb128(&ptr);
  return reinterpret_cast<const char*>(ptr);
}

void DexFile::InitIndex() {
  CHECK_EQ(index_.size(), 0U) << GetLocation();
  for (size_t i = 0; i < NumClassDefs(); ++i) {
    const ClassDef& class_def = GetClassDef(i);
    const char* descriptor = GetClassDescriptor(class_def);
    index_.Put(descriptor, i);
  }
}

bool DexFile::FindClassDefIndex(const StringPiece& descriptor, uint32_t& idx) const {
  Index::const_iterator it = index_.find(descriptor);
  if (it == index_.end()) {
    return false;
  }
  idx = it->second;
  return true;
}

const DexFile::ClassDef* DexFile::FindClassDef(const StringPiece& descriptor) const {
  uint32_t idx;
  if (FindClassDefIndex(descriptor, idx)) {
    return &GetClassDef(idx);
  }
  return NULL;
}

const DexFile::FieldId* DexFile::FindFieldId(const DexFile::TypeId& declaring_klass,
                                              const DexFile::StringId& name,
                                              const DexFile::TypeId& type) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t type_idx = GetIndexForTypeId(type);
  uint32_t lo = 0;
  uint32_t hi = NumFieldIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::FieldId& field = GetFieldId(mid);
    if (class_idx > field.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < field.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > field.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < field.name_idx_) {
        hi = mid - 1;
      } else {
        if (type_idx > field.type_idx_) {
          lo = mid + 1;
        } else if (type_idx < field.type_idx_) {
          hi = mid - 1;
        } else {
          return &field;
        }
      }
    }
  }
  return NULL;
}

const DexFile::MethodId* DexFile::FindMethodId(const DexFile::TypeId& declaring_klass,
                                               const DexFile::StringId& name,
                                               const DexFile::ProtoId& signature) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t proto_idx = GetIndexForProtoId(signature);
  uint32_t lo = 0;
  uint32_t hi = NumMethodIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::MethodId& method = GetMethodId(mid);
    if (class_idx > method.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < method.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > method.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < method.name_idx_) {
        hi = mid - 1;
      } else {
        if (proto_idx > method.proto_idx_) {
          lo = mid + 1;
        } else if (proto_idx < method.proto_idx_) {
          hi = mid - 1;
        } else {
          return &method;
        }
      }
    }
  }
  return NULL;
}

const DexFile::StringId* DexFile::FindStringId(const std::string& string) const {
  uint32_t lo = 0;
  uint32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    uint32_t length;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringDataAndLength(str_id, &length);
    int compare = CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(string.c_str(), str);
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &str_id;
    }
  }
  return NULL;
}

const DexFile::TypeId* DexFile::FindTypeId(uint32_t string_idx) const {
  uint32_t lo = 0;
  uint32_t hi = NumTypeIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const TypeId& type_id = GetTypeId(mid);
    if (string_idx > type_id.descriptor_idx_) {
      lo = mid + 1;
    } else if (string_idx < type_id.descriptor_idx_) {
      hi = mid - 1;
    } else {
      return &type_id;
    }
  }
  return NULL;
}

const DexFile::ProtoId* DexFile::FindProtoId(uint16_t return_type_idx,
                                         const std::vector<uint16_t>& signature_type_idxs) const {
  uint32_t lo = 0;
  uint32_t hi = NumProtoIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::ProtoId& proto = GetProtoId(mid);
    int compare = return_type_idx - proto.return_type_idx_;
    if (compare == 0) {
      DexFileParameterIterator it(*this, proto);
      size_t i = 0;
      while (it.HasNext() && i < signature_type_idxs.size() && compare == 0) {
        compare = signature_type_idxs[i] - it.GetTypeIdx();
        it.Next();
        i++;
      }
      if (compare == 0) {
        if (it.HasNext()) {
          compare = -1;
        } else if (i < signature_type_idxs.size()) {
          compare = 1;
        }
      }
    }
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &proto;
    }
  }
  return NULL;
}

// Given a signature place the type ids into the given vector
bool DexFile::CreateTypeList(uint16_t* return_type_idx, std::vector<uint16_t>* param_type_idxs,
                             const std::string& signature) const {
  if (signature[0] != '(') {
    return false;
  }
  size_t offset = 1;
  size_t end = signature.size();
  bool process_return = false;
  while (offset < end) {
    char c = signature[offset];
    offset++;
    if (c == ')') {
      process_return = true;
      continue;
    }
    std::string descriptor;
    descriptor += c;
    while (c == '[') {  // process array prefix
      if (offset >= end) {  // expect some descriptor following [
        return false;
      }
      c = signature[offset];
      offset++;
      descriptor += c;
    }
    if (c == 'L') {  // process type descriptors
      do {
        if (offset >= end) {  // unexpected early termination of descriptor
          return false;
        }
        c = signature[offset];
        offset++;
        descriptor += c;
      } while (c != ';');
    }
    const DexFile::StringId* string_id = FindStringId(descriptor);
    if (string_id == NULL) {
      return false;
    }
    const DexFile::TypeId* type_id = FindTypeId(GetIndexForStringId(*string_id));
    if (type_id == NULL) {
      return false;
    }
    uint16_t type_idx = GetIndexForTypeId(*type_id);
    if (!process_return) {
      param_type_idxs->push_back(type_idx);
    } else {
      *return_type_idx = type_idx;
      return offset == end;  // return true if the signature had reached a sensible end
    }
  }
  return false;  // failed to correctly parse return type
}

// Materializes the method descriptor for a method prototype.  Method
// descriptors are not stored directly in the dex file.  Instead, one
// must assemble the descriptor from references in the prototype.
std::string DexFile::CreateMethodSignature(uint32_t proto_idx, int32_t* unicode_length) const {
  const ProtoId& proto_id = GetProtoId(proto_idx);
  std::string descriptor;
  descriptor.push_back('(');
  const TypeList* type_list = GetProtoParameters(proto_id);
  size_t parameter_length = 0;
  if (type_list != NULL) {
    // A non-zero number of arguments.  Append the type names.
    for (size_t i = 0; i < type_list->Size(); ++i) {
      const TypeItem& type_item = type_list->GetTypeItem(i);
      uint32_t type_idx = type_item.type_idx_;
      uint32_t type_length;
      const char* name = StringByTypeIdx(type_idx, &type_length);
      parameter_length += type_length;
      descriptor.append(name);
    }
  }
  descriptor.push_back(')');
  uint32_t return_type_idx = proto_id.return_type_idx_;
  uint32_t return_type_length;
  const char* name = StringByTypeIdx(return_type_idx, &return_type_length);
  descriptor.append(name);
  if (unicode_length != NULL) {
    *unicode_length = parameter_length + return_type_length + 2;  // 2 for ( and )
  }
  return descriptor;
}

int32_t DexFile::GetLineNumFromPC(const Method* method, uint32_t rel_pc) const {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (method->GetCodeItemOffset() == 0) {
    return -2;
  }

  const CodeItem* code_item = GetCodeItem(method->GetCodeItemOffset());
  DCHECK(code_item != NULL) << GetLocation();

  // A method with no line number info should return -1
  LineNumFromPcContext context(rel_pc, -1);
  DecodeDebugInfo(code_item, method->IsStatic(), method->GetDexMethodIndex(), LineNumForPcCb,
                  NULL, &context);
  return context.line_num_;
}

int32_t DexFile::FindCatchHandlerOffset(const CodeItem &code_item, int32_t tries_size,
                                        uint32_t address) {
  // Note: Signed type is important for max and min.
  int32_t min = 0;
  int32_t max = tries_size - 1;

  while (max >= min) {
    int32_t mid = (min + max) / 2;
    const TryItem* pTry = DexFile::GetTryItems(code_item, mid);
    uint32_t start = pTry->start_addr_;
    if (address < start) {
      max = mid - 1;
    } else {
      uint32_t end = start + pTry->insn_count_;
      if (address >= end) {
        min = mid + 1;
      } else {  // We have a winner!
        return (int32_t) pTry->handler_off_;
      }
    }
  }
  // No match.
  return -1;
}

void DexFile::DecodeDebugInfo0(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                               DexDebugNewPositionCb position_cb, DexDebugNewLocalCb local_cb,
                               void* context, const byte* stream, LocalInfo* local_in_reg) const {
  uint32_t line = DecodeUnsignedLeb128(&stream);
  uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
  uint16_t arg_reg = code_item->registers_size_ - code_item->ins_size_;
  uint32_t address = 0;
  bool need_locals = (local_cb != NULL);

  if (!is_static) {
    if (need_locals) {
      const char* descriptor = GetMethodDeclaringClassDescriptor(GetMethodId(method_idx));
      local_in_reg[arg_reg].name_ = "this";
      local_in_reg[arg_reg].descriptor_ = descriptor;
      local_in_reg[arg_reg].signature_ = NULL;
      local_in_reg[arg_reg].start_address_ = 0;
      local_in_reg[arg_reg].is_live_ = true;
    }
    arg_reg++;
  }

  DexFileParameterIterator it(*this, GetMethodPrototype(GetMethodId(method_idx)));
  for (uint32_t i = 0; i < parameters_size && it.HasNext(); ++i, it.Next()) {
    if (arg_reg >= code_item->registers_size_) {
      LOG(ERROR) << "invalid stream - arg reg >= reg size (" << arg_reg
                 << " >= " << code_item->registers_size_ << ") in " << GetLocation();
      return;
    }
    uint32_t id = DecodeUnsignedLeb128P1(&stream);
    const char* descriptor = it.GetDescriptor();
    if (need_locals && id != kDexNoIndex) {
      const char* name = StringDataByIdx(id);
      local_in_reg[arg_reg].name_ = name;
      local_in_reg[arg_reg].descriptor_ = descriptor;
      local_in_reg[arg_reg].signature_ = NULL;
      local_in_reg[arg_reg].start_address_ = address;
      local_in_reg[arg_reg].is_live_ = true;
    }
    switch (*descriptor) {
      case 'D':
      case 'J':
        arg_reg += 2;
        break;
      default:
        arg_reg += 1;
        break;
    }
  }

  if (it.HasNext()) {
    LOG(ERROR) << "invalid stream - problem with parameter iterator in " << GetLocation();
    return;
  }

  for (;;)  {
    uint8_t opcode = *stream++;
    uint16_t reg;
    uint16_t name_idx;
    uint16_t descriptor_idx;
    uint16_t signature_idx = 0;

    switch (opcode) {
      case DBG_END_SEQUENCE:
        return;

      case DBG_ADVANCE_PC:
        address += DecodeUnsignedLeb128(&stream);
        break;

      case DBG_ADVANCE_LINE:
        line += DecodeSignedLeb128(&stream);
        break;

      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        name_idx = DecodeUnsignedLeb128P1(&stream);
        descriptor_idx = DecodeUnsignedLeb128P1(&stream);
        if (opcode == DBG_START_LOCAL_EXTENDED) {
          signature_idx = DecodeUnsignedLeb128P1(&stream);
        }

        // Emit what was previously there, if anything
        if (need_locals) {
          InvokeLocalCbIfLive(context, reg, address, local_in_reg, local_cb);

          local_in_reg[reg].name_ = StringDataByIdx(name_idx);
          local_in_reg[reg].descriptor_ = StringByTypeIdx(descriptor_idx);
          if (opcode == DBG_START_LOCAL_EXTENDED) {
            local_in_reg[reg].signature_ = StringDataByIdx(signature_idx);
          }
          local_in_reg[reg].start_address_ = address;
          local_in_reg[reg].is_live_ = true;
        }
        break;

      case DBG_END_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        if (need_locals) {
          InvokeLocalCbIfLive(context, reg, address, local_in_reg, local_cb);
          local_in_reg[reg].is_live_ = false;
        }
        break;

      case DBG_RESTART_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        if (need_locals) {
          if (local_in_reg[reg].name_ == NULL || local_in_reg[reg].descriptor_ == NULL) {
            LOG(ERROR) << "invalid stream - no name or descriptor in " << GetLocation();
            return;
          }

          // If the register is live, the "restart" is superfluous,
          // and we don't want to mess with the existing start address.
          if (!local_in_reg[reg].is_live_) {
            local_in_reg[reg].start_address_ = address;
            local_in_reg[reg].is_live_ = true;
          }
        }
        break;

      case DBG_SET_PROLOGUE_END:
      case DBG_SET_EPILOGUE_BEGIN:
      case DBG_SET_FILE:
        break;

      default: {
        int adjopcode = opcode - DBG_FIRST_SPECIAL;

        address += adjopcode / DBG_LINE_RANGE;
        line += DBG_LINE_BASE + (adjopcode % DBG_LINE_RANGE);

        if (position_cb != NULL) {
          if (position_cb(context, address, line)) {
            // early exit
            return;
          }
        }
        break;
      }
    }
  }
}

void DexFile::DecodeDebugInfo(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                              DexDebugNewPositionCb position_cb, DexDebugNewLocalCb local_cb,
                              void* context) const {
  const byte* stream = GetDebugInfoStream(code_item);
  UniquePtr<LocalInfo[]> local_in_reg(local_cb != NULL ? new LocalInfo[code_item->registers_size_] : NULL);
  if (stream != NULL) {
    DecodeDebugInfo0(code_item, is_static, method_idx, position_cb, local_cb, context, stream, &local_in_reg[0]);
  }
  for (int reg = 0; reg < code_item->registers_size_; reg++) {
    InvokeLocalCbIfLive(context, reg, code_item->insns_size_in_code_units_, &local_in_reg[0], local_cb);
  }
}

bool DexFile::LineNumForPcCb(void* raw_context, uint32_t address, uint32_t line_num) {
  LineNumFromPcContext* context = reinterpret_cast<LineNumFromPcContext*>(raw_context);

  // We know that this callback will be called in
  // ascending address order, so keep going until we find
  // a match or we've just gone past it.
  if (address > context->address_) {
    // The line number from the previous positions callback
    // wil be the final result.
    return true;
  } else {
    context->line_num_ = line_num;
    return address == context->address_;
  }
}

// Decodes the header section from the class data bytes.
void ClassDataItemIterator::ReadClassDataHeader() {
  CHECK(ptr_pos_ != NULL);
  header_.static_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.instance_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.direct_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.virtual_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
}

void ClassDataItemIterator::ReadClassDataField() {
  field_.field_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  field_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  if (last_idx_ != 0 && field_.field_idx_delta_ == 0) {
    LOG(WARNING) << "Duplicate field " << PrettyField(GetMemberIndex(), dex_file_)
                 << " in " << dex_file_.GetLocation();
  }
}

void ClassDataItemIterator::ReadClassDataMethod() {
  method_.method_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.code_off_ = DecodeUnsignedLeb128(&ptr_pos_);
  if (last_idx_ != 0 && method_.method_idx_delta_ == 0) {
    LOG(WARNING) << "Duplicate method " << PrettyMethod(GetMemberIndex(), dex_file_)
                 << " in " << dex_file_.GetLocation();
  }
}

// Read a signed integer.  "zwidth" is the zero-based byte count.
static int32_t ReadSignedInt(const byte* ptr, int zwidth) {
  int32_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint32_t)val >> 8) | (((int32_t)*ptr++) << 24);
  }
  val >>= (3 - zwidth) * 8;
  return val;
}

// Read an unsigned integer.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint32_t ReadUnsignedInt(const byte* ptr, int zwidth, bool fill_on_right) {
  uint32_t val = 0;
  if (!fill_on_right) {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint32_t)*ptr++) << 24);
    }
    val >>= (3 - zwidth) * 8;
  } else {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint32_t)*ptr++) << 24);
    }
  }
  return val;
}

// Read a signed long.  "zwidth" is the zero-based byte count.
static int64_t ReadSignedLong(const byte* ptr, int zwidth) {
  int64_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint64_t)val >> 8) | (((int64_t)*ptr++) << 56);
  }
  val >>= (7 - zwidth) * 8;
  return val;
}

// Read an unsigned long.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint64_t ReadUnsignedLong(const byte* ptr, int zwidth, bool fill_on_right) {
  uint64_t val = 0;
  if (!fill_on_right) {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint64_t)*ptr++) << 56);
    }
    val >>= (7 - zwidth) * 8;
  } else {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint64_t)*ptr++) << 56);
    }
  }
  return val;
}

EncodedStaticFieldValueIterator::EncodedStaticFieldValueIterator(const DexFile& dex_file,
    DexCache* dex_cache, ClassLinker* linker, const DexFile::ClassDef& class_def) :
    dex_file_(dex_file), dex_cache_(dex_cache), linker_(linker), array_size_(), pos_(-1), type_(0) {
  ptr_ = dex_file.GetEncodedStaticFieldValuesArray(class_def);
  if (ptr_ == NULL) {
    array_size_ = 0;
  } else {
    array_size_ = DecodeUnsignedLeb128(&ptr_);
  }
  if (array_size_ > 0) {
    Next();
  }
}

void EncodedStaticFieldValueIterator::Next() {
  pos_++;
  if (pos_ >= array_size_) {
    return;
  }
  byte value_type = *ptr_++;
  byte value_arg = value_type >> kEncodedValueArgShift;
  size_t width = value_arg + 1;  // assume and correct later
  type_ = value_type & kEncodedValueTypeMask;
  switch (type_) {
  case kBoolean:
    jval_.i = (value_arg != 0) ? 1 : 0;
    width = 0;
    break;
  case kByte:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt(8, jval_.i));
    break;
  case kShort:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt(16, jval_.i));
    break;
  case kChar:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    CHECK(IsUint(16, jval_.i));
    break;
  case kInt:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    break;
  case kLong:
    jval_.j = ReadSignedLong(ptr_, value_arg);
    break;
  case kFloat:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, true);
    break;
  case kDouble:
    jval_.j = ReadUnsignedLong(ptr_, value_arg, true);
    break;
  case kString:
  case kType:
  case kMethod:
  case kEnum:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    break;
  case kField:
  case kArray:
  case kAnnotation:
    UNIMPLEMENTED(FATAL) << ": type " << type_;
    break;
  case kNull:
    jval_.l = NULL;
    width = 0;
    break;
  default:
    LOG(FATAL) << "Unreached";
  }
  ptr_ += width;
}

void EncodedStaticFieldValueIterator::ReadValueToField(Field* field) const {
  switch (type_) {
    case kBoolean: field->SetBoolean(NULL, jval_.z); break;
    case kByte:    field->SetByte(NULL, jval_.b); break;
    case kShort:   field->SetShort(NULL, jval_.s); break;
    case kChar:    field->SetChar(NULL, jval_.c); break;
    case kInt:     field->SetInt(NULL, jval_.i); break;
    case kLong:    field->SetLong(NULL, jval_.j); break;
    case kFloat:   field->SetFloat(NULL, jval_.f); break;
    case kDouble:  field->SetDouble(NULL, jval_.d); break;
    case kNull:    field->SetObject(NULL, NULL); break;
    case kString: {
      String* resolved = linker_->ResolveString(dex_file_, jval_.i, dex_cache_);
      field->SetObject(NULL, resolved);
      break;
    }
    default: UNIMPLEMENTED(FATAL) << ": type " << type_;
  }
}

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item, uint32_t address) {
  handler_.address_ = -1;
  int32_t offset = -1;

  // Short-circuit the overwhelmingly common cases.
  switch (code_item.tries_size_) {
    case 0:
      break;
    case 1: {
      const DexFile::TryItem* tries = DexFile::GetTryItems(code_item, 0);
      uint32_t start = tries->start_addr_;
      if (address >= start) {
        uint32_t end = start + tries->insn_count_;
        if (address < end) {
          offset = tries->handler_off_;
        }
      }
      break;
    }
    default:
      offset = DexFile::FindCatchHandlerOffset(code_item, code_item.tries_size_, address);
  }
  Init(code_item, offset);
}

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item,
                                           const DexFile::TryItem& try_item) {
  handler_.address_ = -1;
  Init(code_item, try_item.handler_off_);
}

void CatchHandlerIterator::Init(const DexFile::CodeItem& code_item,
                                int32_t offset) {
  if (offset >= 0) {
    Init(DexFile::GetCatchHandlerData(code_item, offset));
  } else {
    // Not found, initialize as empty
    current_data_ = NULL;
    remaining_count_ = -1;
    catch_all_ = false;
    DCHECK(!HasNext());
  }
}

void CatchHandlerIterator::Init(const byte* handler_data) {
  current_data_ = handler_data;
  remaining_count_ = DecodeSignedLeb128(&current_data_);

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

void CatchHandlerIterator::Next() {
  if (remaining_count_ > 0) {
    handler_.type_idx_ = DecodeUnsignedLeb128(&current_data_);
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    remaining_count_--;
    return;
  }

  if (catch_all_) {
    handler_.type_idx_ = DexFile::kDexNoIndex16;
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    catch_all_ = false;
    return;
  }

  // no more handler
  remaining_count_ = -1;
}

}  // namespace art
