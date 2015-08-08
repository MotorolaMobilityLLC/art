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

#ifndef ART_RUNTIME_MIRROR_CLASS_LOADER_H_
#define ART_RUNTIME_MIRROR_CLASS_LOADER_H_

#include "object.h"

namespace art {

struct ClassLoaderOffsets;
class ClassTable;

namespace mirror {

// C++ mirror of java.lang.ClassLoader
class MANAGED ClassLoader : public Object {
 public:
  // Size of an instance of java.lang.ClassLoader.
  static constexpr uint32_t InstanceSize() {
    return sizeof(ClassLoader);
  }
  ClassLoader* GetParent() SHARED_REQUIRES(Locks::mutator_lock_) {
    return GetFieldObject<ClassLoader>(OFFSET_OF_OBJECT_MEMBER(ClassLoader, parent_));
  }
  ClassTable* GetClassTable() SHARED_REQUIRES(Locks::mutator_lock_) {
    return reinterpret_cast<ClassTable*>(
        GetField64(OFFSET_OF_OBJECT_MEMBER(ClassLoader, class_table_)));
  }
  void SetClassTable(ClassTable* class_table) SHARED_REQUIRES(Locks::mutator_lock_) {
    SetField64<false>(OFFSET_OF_OBJECT_MEMBER(ClassLoader, class_table_),
                      reinterpret_cast<uint64_t>(class_table));
  }

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  HeapReference<Object> packages_;
  HeapReference<ClassLoader> parent_;
  HeapReference<Object> proxyCache_;
  // Native pointer to class table, need to zero this out when image writing.
  uint32_t padding_ ATTRIBUTE_UNUSED;
  uint64_t class_table_;

  friend struct art::ClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassLoader);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_CLASS_LOADER_H_
