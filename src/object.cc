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

#include "object.h"

#include <string.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>

#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "globals.h"
#include "heap.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "logging.h"
#include "monitor.h"
#include "object_utils.h"
#include "runtime.h"
#include "runtime_support.h"
#include "sirt_ref.h"
#include "stack.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {

BooleanArray* Object::AsBooleanArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<BooleanArray*>(this);
}

ByteArray* Object::AsByteArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveByte());
  return down_cast<ByteArray*>(this);
}

CharArray* Object::AsCharArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveChar());
  return down_cast<CharArray*>(this);
}

ShortArray* Object::AsShortArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveShort());
  return down_cast<ShortArray*>(this);
}

IntArray* Object::AsIntArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveInt() ||
         GetClass()->GetComponentType()->IsPrimitiveFloat());
  return down_cast<IntArray*>(this);
}

LongArray* Object::AsLongArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveLong() ||
         GetClass()->GetComponentType()->IsPrimitiveDouble());
  return down_cast<LongArray*>(this);
}

String* Object::AsString() {
  DCHECK(GetClass()->IsStringClass());
  return down_cast<String*>(this);
}

Throwable* Object::AsThrowable() {
  DCHECK(GetClass()->IsThrowableClass());
  return down_cast<Throwable*>(this);
}

Object* Object::Clone(Thread* self) {
  Class* c = GetClass();
  DCHECK(!c->IsClassClass());

  // Object::SizeOf gets the right size even if we're an array.
  // Using c->AllocObject() here would be wrong.
  size_t num_bytes = SizeOf();
  Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<Object> copy(self, heap->AllocObject(self, c, num_bytes));
  if (copy.get() == NULL) {
    return NULL;
  }

  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  byte* src_bytes = reinterpret_cast<byte*>(this);
  byte* dst_bytes = reinterpret_cast<byte*>(copy.get());
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);

  // Perform write barriers on copied object references.
  if (c->IsArrayClass()) {
    if (!c->GetComponentType()->IsPrimitive()) {
      const ObjectArray<Object>* array = copy->AsObjectArray<Object>();
      heap->WriteBarrierArray(copy.get(), 0, array->GetLength());
    }
  } else {
    for (const Class* klass = c; klass != NULL; klass = klass->GetSuperClass()) {
      size_t num_reference_fields = klass->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_reference_fields; ++i) {
        Field* field = klass->GetInstanceField(i);
        MemberOffset field_offset = field->GetOffset();
        const Object* ref = copy->GetFieldObject<const Object*>(field_offset, false);
        heap->WriteBarrierField(copy.get(), field_offset, ref);
      }
    }
  }

  if (c->IsFinalizable()) {
    heap->AddFinalizerReference(Thread::Current(), copy.get());
  }

  return copy.get();
}

uint32_t Object::GetThinLockId() {
  return Monitor::GetThinLockId(monitor_);
}

void Object::MonitorEnter(Thread* thread) {
  Monitor::MonitorEnter(thread, this);
}

bool Object::MonitorExit(Thread* thread) {
  return Monitor::MonitorExit(thread, this);
}

void Object::Notify() {
  Monitor::Notify(Thread::Current(), this);
}

void Object::NotifyAll() {
  Monitor::NotifyAll(Thread::Current(), this);
}

void Object::Wait(int64_t ms, int32_t ns) {
  Monitor::Wait(Thread::Current(), this, ms, ns, true);
}

#if VERIFY_OBJECT_ENABLED
void Object::CheckFieldAssignment(MemberOffset field_offset, const Object* new_value) {
  const Class* c = GetClass();
  if (Runtime::Current()->GetClassLinker() == NULL ||
      !Runtime::Current()->GetHeap()->IsObjectValidationEnabled() ||
      !c->IsResolved()) {
    return;
  }
  for (const Class* cur = c; cur != NULL; cur = cur->GetSuperClass()) {
    ObjectArray<Field>* fields = cur->GetIFields();
    if (fields != NULL) {
      size_t num_ref_ifields = cur->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_ref_ifields; ++i) {
        Field* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  if (c->IsArrayClass()) {
    // Bounds and assign-ability done in the array setter.
    return;
  }
  if (IsClass()) {
    ObjectArray<Field>* fields = AsClass()->GetSFields();
    if (fields != NULL) {
      size_t num_ref_sfields = AsClass()->NumReferenceStaticFields();
      for (size_t i = 0; i < num_ref_sfields; ++i) {
        Field* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  LOG(FATAL) << "Failed to find field for assignment to " << reinterpret_cast<void*>(this)
      << " of type " << PrettyDescriptor(c) << " at offset " << field_offset;
}
#endif

// TODO: get global references for these
Class* Field::java_lang_reflect_Field_ = NULL;

void Field::SetClass(Class* java_lang_reflect_Field) {
  CHECK(java_lang_reflect_Field_ == NULL);
  CHECK(java_lang_reflect_Field != NULL);
  java_lang_reflect_Field_ = java_lang_reflect_Field;
}

void Field::ResetClass() {
  CHECK(java_lang_reflect_Field_ != NULL);
  java_lang_reflect_Field_ = NULL;
}

void Field::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#if 0 // TODO enable later in boot and under !NDEBUG
  FieldHelper fh(this);
  Primitive::Type type = fh.GetTypeAsPrimitiveType();
  if (type == Primitive::kPrimDouble || type == Primitive::kPrimLong) {
    DCHECK_ALIGNED(num_bytes.Uint32Value(), 8);
  }
#endif
  SetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), num_bytes.Uint32Value(), false);
}

uint32_t Field::Get32(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField32(GetOffset(), IsVolatile());
}

void Field::Set32(Object* object, uint32_t new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField32(GetOffset(), new_value, IsVolatile());
}

uint64_t Field::Get64(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField64(GetOffset(), IsVolatile());
}

void Field::Set64(Object* object, uint64_t new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField64(GetOffset(), new_value, IsVolatile());
}

Object* Field::GetObj(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetFieldObject<Object*>(GetOffset(), IsVolatile());
}

void Field::SetObj(Object* object, const Object* new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(IsStatic() == (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetFieldObject(GetOffset(), new_value, IsVolatile());
}

bool Field::GetBoolean(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetBoolean(Object* object, bool z) const {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, z);
}

int8_t Field::GetByte(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetByte(Object* object, int8_t b) const {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, b);
}

uint16_t Field::GetChar(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetChar(Object* object, uint16_t c) const {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, c);
}

int16_t Field::GetShort(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return Get32(object);
}

void Field::SetShort(Object* object, int16_t s) const {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, s);
}

int32_t Field::GetInt(const Object* object) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  return Get32(object);
}

void Field::SetInt(Object* object, int32_t i) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  Set32(object, i);
}

int64_t Field::GetLong(const Object* object) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  return Get64(object);
}

void Field::SetLong(Object* object, int64_t j) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  Set64(object, j);
}

union Bits {
  jdouble d;
  jfloat f;
  jint i;
  jlong j;
};

float Field::GetFloat(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.i = Get32(object);
  return bits.f;
}

void Field::SetFloat(Object* object, float f) const {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.f = f;
  Set32(object, bits.i);
}

double Field::GetDouble(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.j = Get64(object);
  return bits.d;
}

void Field::SetDouble(Object* object, double d) const {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.d = d;
  Set64(object, bits.j);
}

Object* Field::GetObject(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return GetObj(object);
}

void Field::SetObject(Object* object, const Object* l) const {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  SetObj(object, l);
}

// TODO: get global references for these
Class* AbstractMethod::java_lang_reflect_Constructor_ = NULL;
Class* AbstractMethod::java_lang_reflect_Method_ = NULL;

InvokeType AbstractMethod::GetInvokeType() const {
  // TODO: kSuper?
  if (GetDeclaringClass()->IsInterface()) {
    return kInterface;
  } else if (IsStatic()) {
    return kStatic;
  } else if (IsDirect()) {
    return kDirect;
  } else {
    return kVirtual;
  }
}

void AbstractMethod::SetClasses(Class* java_lang_reflect_Constructor, Class* java_lang_reflect_Method) {
  CHECK(java_lang_reflect_Constructor_ == NULL);
  CHECK(java_lang_reflect_Constructor != NULL);
  java_lang_reflect_Constructor_ = java_lang_reflect_Constructor;

  CHECK(java_lang_reflect_Method_ == NULL);
  CHECK(java_lang_reflect_Method != NULL);
  java_lang_reflect_Method_ = java_lang_reflect_Method;
}

void AbstractMethod::ResetClasses() {
  CHECK(java_lang_reflect_Constructor_ != NULL);
  java_lang_reflect_Constructor_ = NULL;

  CHECK(java_lang_reflect_Method_ != NULL);
  java_lang_reflect_Method_ = NULL;
}

ObjectArray<String>* AbstractMethod::GetDexCacheStrings() const {
  return GetFieldObject<ObjectArray<String>*>(
      OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_strings_), false);
}

void AbstractMethod::SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_strings_),
                 new_dex_cache_strings, false);
}

ObjectArray<AbstractMethod>* AbstractMethod::GetDexCacheResolvedMethods() const {
  return GetFieldObject<ObjectArray<AbstractMethod>*>(
      OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_methods_), false);
}

void AbstractMethod::SetDexCacheResolvedMethods(ObjectArray<AbstractMethod>* new_dex_cache_methods) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_methods_),
                 new_dex_cache_methods, false);
}

ObjectArray<Class>* AbstractMethod::GetDexCacheResolvedTypes() const {
  return GetFieldObject<ObjectArray<Class>*>(
      OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_types_), false);
}

void AbstractMethod::SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_classes) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_types_),
                 new_dex_cache_classes, false);
}

ObjectArray<StaticStorageBase>* AbstractMethod::GetDexCacheInitializedStaticStorage() const {
  return GetFieldObject<ObjectArray<StaticStorageBase>*>(
      OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_initialized_static_storage_),
      false);
}

void AbstractMethod::SetDexCacheInitializedStaticStorage(ObjectArray<StaticStorageBase>* new_value) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_initialized_static_storage_),
      new_value, false);
}

size_t AbstractMethod::NumArgRegisters(const StringPiece& shorty) {
  CHECK_LE(1, shorty.length());
  uint32_t num_registers = 0;
  for (int i = 1; i < shorty.length(); ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

bool AbstractMethod::IsProxyMethod() const {
  return GetDeclaringClass()->IsProxyClass();
}

AbstractMethod* AbstractMethod::FindOverriddenMethod() const {
  if (IsStatic()) {
    return NULL;
  }
  Class* declaring_class = GetDeclaringClass();
  Class* super_class = declaring_class->GetSuperClass();
  uint16_t method_index = GetMethodIndex();
  ObjectArray<AbstractMethod>* super_class_vtable = super_class->GetVTable();
  AbstractMethod* result = NULL;
  // Did this method override a super class method? If so load the result from the super class'
  // vtable
  if (super_class_vtable != NULL && method_index < super_class_vtable->GetLength()) {
    result = super_class_vtable->Get(method_index);
  } else {
    // Method didn't override superclass method so search interfaces
    if (IsProxyMethod()) {
      result = GetDexCacheResolvedMethods()->Get(GetDexMethodIndex());
      CHECK_EQ(result,
               Runtime::Current()->GetClassLinker()->FindMethodForProxy(GetDeclaringClass(), this));
    } else {
      MethodHelper mh(this);
      MethodHelper interface_mh;
      IfTable* iftable = GetDeclaringClass()->GetIfTable();
      for (size_t i = 0; i < iftable->Count() && result == NULL; i++) {
        Class* interface = iftable->GetInterface(i);
        for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
          AbstractMethod* interface_method = interface->GetVirtualMethod(j);
          interface_mh.ChangeMethod(interface_method);
          if (mh.HasSameNameAndSignature(&interface_mh)) {
            result = interface_method;
            break;
          }
        }
      }
    }
  }
#ifndef NDEBUG
  MethodHelper result_mh(result);
  DCHECK(result == NULL || MethodHelper(this).HasSameNameAndSignature(&result_mh));
#endif
  return result;
}

static const void* GetOatCode(const AbstractMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  const void* code = m->GetCode();
  // Peel off any method tracing trampoline.
  if (runtime->IsMethodTracingActive() && runtime->GetInstrumentation()->GetSavedCodeFromMap(m) != NULL) {
    code = runtime->GetInstrumentation()->GetSavedCodeFromMap(m);
  }
  // Peel off any resolution stub.
  if (code == runtime->GetResolutionStubArray(Runtime::kStaticMethod)->GetData()) {
    code = runtime->GetClassLinker()->GetOatCodeFor(m);
  }
  return code;
}

uintptr_t AbstractMethod::NativePcOffset(const uintptr_t pc) const {
  return pc - reinterpret_cast<uintptr_t>(GetOatCode(this));
}

// Find the lowest-address native safepoint pc for a given dex pc
uintptr_t AbstractMethod::ToFirstNativeSafepointPc(const uint32_t dex_pc) const {
#if !defined(ART_USE_LLVM_COMPILER)
  const uint32_t* mapping_table = GetPcToDexMappingTable();
  if (mapping_table == NULL) {
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  size_t mapping_table_length = GetPcToDexMappingTableLength();
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    if (mapping_table[i + 1] == dex_pc) {
      return mapping_table[i] + reinterpret_cast<uintptr_t>(GetOatCode(this));
    }
  }
  LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
             << " in " << PrettyMethod(this);
  return 0;
#else
  // Compiler LLVM doesn't use the machine pc, we just use dex pc instead.
  return static_cast<uint32_t>(dex_pc);
#endif
}

uint32_t AbstractMethod::ToDexPc(const uintptr_t pc) const {
#if !defined(ART_USE_LLVM_COMPILER)
  const uint32_t* mapping_table = GetPcToDexMappingTable();
  if (mapping_table == NULL) {
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  size_t mapping_table_length = GetPcToDexMappingTableLength();
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(GetOatCode(this));
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    if (mapping_table[i] == sought_offset) {
      return mapping_table[i + 1];
    }
  }
  LOG(FATAL) << "Failed to find Dex offset for PC offset 0x" << std::hex << sought_offset
             << " in " << PrettyMethod(this);
  return DexFile::kDexNoIndex;
#else
  // Compiler LLVM doesn't use the machine pc, we just use dex pc instead.
  return static_cast<uint32_t>(pc);
#endif
}

uintptr_t AbstractMethod::ToNativePc(const uint32_t dex_pc) const {
  const uint32_t* mapping_table = GetDexToPcMappingTable();
  if (mapping_table == NULL) {
    DCHECK_EQ(dex_pc, 0U);
    return 0;   // Special no mapping/pc == 0 case
  }
  size_t mapping_table_length = GetDexToPcMappingTableLength();
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    uint32_t map_offset = mapping_table[i];
    uint32_t map_dex_offset = mapping_table[i + 1];
    if (map_dex_offset == dex_pc) {
      return reinterpret_cast<uintptr_t>(GetOatCode(this)) + map_offset;
    }
  }
  LOG(FATAL) << "Looking up Dex PC not contained in method, 0x" << std::hex << dex_pc
             << " in " << PrettyMethod(this);
  return 0;
}

uint32_t AbstractMethod::FindCatchBlock(Class* exception_type, uint32_t dex_pc) const {
  MethodHelper mh(this);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, dex_pc); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return it.GetHandlerAddress();
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return it.GetHandlerAddress();
    }
  }
  // Handler not found
  return DexFile::kDexNoIndex;
}

void AbstractMethod::Invoke(Thread* self, Object* receiver, JValue* args, JValue* result) {
  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(kRunnable, self->GetState());
  }

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  // Call the invoke stub associated with the method.
  // Pass everything as arguments.
  AbstractMethod::InvokeStub* stub = GetInvokeStub();

  if (UNLIKELY(!Runtime::Current()->IsStarted())){
    LOG(INFO) << "Not invoking " << PrettyMethod(this) << " for a runtime that isn't started";
    if (result != NULL) {
      result->SetJ(0);
    }
  } else {
    bool interpret = self->ReadFlag(kEnterInterpreter);
    const bool kLogInvocationStartAndReturn = false;
    if (!interpret && GetCode() != NULL && stub != NULL) {
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Invoking '%s' code=%p stub=%p",
                                  PrettyMethod(this).c_str(), GetCode(), stub);
      }
      (*stub)(this, receiver, self, args, result);
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Returned '%s' code=%p stub=%p",
                                  PrettyMethod(this).c_str(), GetCode(), stub);
      }
    } else {
      const bool kInterpretMethodsWithNoCode = false;
      if (interpret || kInterpretMethodsWithNoCode) {
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << "Interpreting " << PrettyMethod(this) << "'";
        }
        art::interpreter::EnterInterpreterFromInvoke(self, this, receiver, args, result);
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << "Returned '" << PrettyMethod(this) << "'";
        }
      } else {
        LOG(INFO) << "Not invoking '" << PrettyMethod(this)
              << "' code=" << reinterpret_cast<const void*>(GetCode())
              << " stub=" << reinterpret_cast<void*>(stub);
        if (result != NULL) {
          result->SetJ(0);
        }
      }
    }
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);
}

bool AbstractMethod::IsRegistered() const {
  void* native_method = GetFieldPtr<void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_), false);
  CHECK(native_method != NULL);
  void* jni_stub = Runtime::Current()->GetJniDlsymLookupStub()->GetData();
  return native_method != jni_stub;
}

void AbstractMethod::RegisterNative(Thread* self, const void* native_method) {
  DCHECK(Thread::Current() == self);
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(native_method != NULL) << PrettyMethod(this);
#if defined(ART_USE_LLVM_COMPILER)
  SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_),
                           native_method, false);
#else
  if (!self->GetJniEnv()->vm->work_around_app_jni_bugs) {
    SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_),
                             native_method, false);
  } else {
    // We've been asked to associate this method with the given native method but are working
    // around JNI bugs, that include not giving Object** SIRT references to native methods. Direct
    // the native method to runtime support and store the target somewhere runtime support will
    // find it.
#if defined(__arm__)
    SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_),
        reinterpret_cast<const void*>(art_work_around_app_jni_bugs), false);
#else
    UNIMPLEMENTED(FATAL);
#endif
    SetFieldPtr<const uint8_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_gc_map_),
        reinterpret_cast<const uint8_t*>(native_method), false);
  }
#endif
}

void AbstractMethod::UnregisterNative(Thread* self) {
  CHECK(IsNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(self, Runtime::Current()->GetJniDlsymLookupStub()->GetData());
}

Class* Class::java_lang_Class_ = NULL;

void Class::SetClassClass(Class* java_lang_Class) {
  CHECK(java_lang_Class_ == NULL) << java_lang_Class_ << " " << java_lang_Class;
  CHECK(java_lang_Class != NULL);
  java_lang_Class_ = java_lang_Class;
}

void Class::ResetClass() {
  CHECK(java_lang_Class_ != NULL);
  java_lang_Class_ = NULL;
}

void Class::SetStatus(Status new_status) {
  CHECK(new_status > GetStatus() || new_status == kStatusError || !Runtime::Current()->IsStarted())
      << PrettyClass(this) << " " << GetStatus() << " -> " << new_status;
  CHECK(sizeof(Status) == sizeof(uint32_t)) << PrettyClass(this);
  if (new_status > kStatusResolved) {
    CHECK_EQ(GetThinLockId(), Thread::Current()->GetThinLockId()) << PrettyClass(this);
  }
  if (new_status == kStatusError) {
    CHECK_NE(GetStatus(), kStatusError) << PrettyClass(this);

    // stash current exception
    Thread* self = Thread::Current();
    SirtRef<Throwable> exception(self, self->GetException());
    CHECK(exception.get() != NULL);

    // clear exception to call FindSystemClass
    self->ClearException();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Class* eiie_class = class_linker->FindSystemClass("Ljava/lang/ExceptionInInitializerError;");
    CHECK(!self->IsExceptionPending());

    // only verification errors, not initialization problems, should set a verify error.
    // this is to ensure that ThrowEarlierClassFailure will throw NoClassDefFoundError in that case.
    Class* exception_class = exception->GetClass();
    if (!eiie_class->IsAssignableFrom(exception_class)) {
      SetVerifyErrorClass(exception_class);
    }

    // restore exception
    self->SetException(exception.get());
  }
  return SetField32(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status, false);
}

DexCache* Class::GetDexCache() const {
  return GetFieldObject<DexCache*>(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), false);
}

void Class::SetDexCache(DexCache* new_dex_cache) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), new_dex_cache, false);
}

Object* Class::AllocObject(Thread* self) {
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  DCHECK(IsInstantiable()) << PrettyClass(this);
  // TODO: decide whether we want this check. It currently fails during bootstrap.
  // DCHECK(!Runtime::Current()->IsStarted() || IsInitializing()) << PrettyClass(this);
  DCHECK_GE(this->object_size_, sizeof(Object));
  return Runtime::Current()->GetHeap()->AllocObject(self, this, this->object_size_);
}

void Class::SetClassSize(size_t new_class_size) {
  DCHECK_GE(new_class_size, GetClassSize()) << " class=" << PrettyTypeOf(this);
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), new_class_size, false);
}

// Return the class' name. The exact format is bizarre, but it's the specified behavior for
// Class.getName: keywords for primitive types, regular "[I" form for primitive arrays (so "int"
// but "[I"), and arrays of reference types written between "L" and ";" but with dots rather than
// slashes (so "java.lang.String" but "[Ljava.lang.String;"). Madness.
String* Class::ComputeName() {
  String* name = GetName();
  if (name != NULL) {
    return name;
  }
  std::string descriptor(ClassHelper(this).GetDescriptor());
  if ((descriptor[0] != 'L') && (descriptor[0] != '[')) {
    // The descriptor indicates that this is the class for
    // a primitive type; special-case the return value.
    const char* c_name = NULL;
    switch (descriptor[0]) {
    case 'Z': c_name = "boolean"; break;
    case 'B': c_name = "byte";    break;
    case 'C': c_name = "char";    break;
    case 'S': c_name = "short";   break;
    case 'I': c_name = "int";     break;
    case 'J': c_name = "long";    break;
    case 'F': c_name = "float";   break;
    case 'D': c_name = "double";  break;
    case 'V': c_name = "void";    break;
    default:
      LOG(FATAL) << "Unknown primitive type: " << PrintableChar(descriptor[0]);
    }
    name = String::AllocFromModifiedUtf8(Thread::Current(), c_name);
  } else {
    // Convert the UTF-8 name to a java.lang.String. The name must use '.' to separate package
    // components.
    if (descriptor.size() > 2 && descriptor[0] == 'L' && descriptor[descriptor.size() - 1] == ';') {
      descriptor.erase(0, 1);
      descriptor.erase(descriptor.size() - 1);
    }
    std::replace(descriptor.begin(), descriptor.end(), '/', '.');
    name = String::AllocFromModifiedUtf8(Thread::Current(), descriptor.c_str());
  }
  SetName(name);
  return name;
}

void Class::DumpClass(std::ostream& os, int flags) const {
  if ((flags & kDumpClassFullDetail) == 0) {
    os << PrettyClass(this);
    if ((flags & kDumpClassClassLoader) != 0) {
      os << ' ' << GetClassLoader();
    }
    if ((flags & kDumpClassInitialized) != 0) {
      os << ' ' << GetStatus();
    }
    os << "\n";
    return;
  }

  Class* super = GetSuperClass();
  ClassHelper kh(this);
  os << "----- " << (IsInterface() ? "interface" : "class") << " "
     << "'" << kh.GetDescriptor() << "' cl=" << GetClassLoader() << " -----\n",
  os << "  objectSize=" << SizeOf() << " "
     << "(" << (super != NULL ? super->SizeOf() : -1) << " from super)\n",
  os << StringPrintf("  access=0x%04x.%04x\n",
      GetAccessFlags() >> 16, GetAccessFlags() & kAccJavaFlagsMask);
  if (super != NULL) {
    os << "  super='" << PrettyClass(super) << "' (cl=" << super->GetClassLoader() << ")\n";
  }
  if (IsArrayClass()) {
    os << "  componentType=" << PrettyClass(GetComponentType()) << "\n";
  }
  if (kh.NumDirectInterfaces() > 0) {
    os << "  interfaces (" << kh.NumDirectInterfaces() << "):\n";
    for (size_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      const ClassLoader* cl = interface->GetClassLoader();
      os << StringPrintf("    %2zd: %s (cl=%p)\n", i, PrettyClass(interface).c_str(), cl);
    }
  }
  os << "  vtable (" << NumVirtualMethods() << " entries, "
     << (super != NULL ? super->NumVirtualMethods() : 0) << " in super):\n";
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    os << StringPrintf("    %2zd: %s\n", i, PrettyMethod(GetVirtualMethodDuringLinking(i)).c_str());
  }
  os << "  direct methods (" << NumDirectMethods() << " entries):\n";
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    os << StringPrintf("    %2zd: %s\n", i, PrettyMethod(GetDirectMethod(i)).c_str());
  }
  if (NumStaticFields() > 0) {
    os << "  static fields (" << NumStaticFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumStaticFields(); ++i) {
        os << StringPrintf("    %2zd: %s\n", i, PrettyField(GetStaticField(i)).c_str());
      }
    } else {
      os << "    <not yet available>";
    }
  }
  if (NumInstanceFields() > 0) {
    os << "  instance fields (" << NumInstanceFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumInstanceFields(); ++i) {
        os << StringPrintf("    %2zd: %s\n", i, PrettyField(GetInstanceField(i)).c_str());
      }
    } else {
      os << "    <not yet available>";
    }
  }
}

void Class::SetReferenceInstanceOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    size_t count = 0;
    for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
      count += c->NumReferenceInstanceFieldsDuringLinking();
    }
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets), count);
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_),
             new_reference_offsets, false);
}

void Class::SetReferenceStaticOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets),
             NumReferenceStaticFieldsDuringLinking());
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_),
             new_reference_offsets, false);
}

bool Class::Implements(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(klass->IsInterface()) << PrettyClass(this);
  // All interfaces implemented directly and by our superclass, and
  // recursively all super-interfaces of those interfaces, are listed
  // in iftable_, so we can just do a linear scan through that.
  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->GetInterface(i) == klass) {
      return true;
    }
  }
  return false;
}

// Determine whether "this" is assignable from "src", where both of these
// are array classes.
//
// Consider an array class, e.g. Y[][], where Y is a subclass of X.
//   Y[][]            = Y[][] --> true (identity)
//   X[][]            = Y[][] --> true (element superclass)
//   Y                = Y[][] --> false
//   Y[]              = Y[][] --> false
//   Object           = Y[][] --> true (everything is an object)
//   Object[]         = Y[][] --> true
//   Object[][]       = Y[][] --> true
//   Object[][][]     = Y[][] --> false (too many []s)
//   Serializable     = Y[][] --> true (all arrays are Serializable)
//   Serializable[]   = Y[][] --> true
//   Serializable[][] = Y[][] --> false (unless Y is Serializable)
//
// Don't forget about primitive types.
//   Object[]         = int[] --> false
//
bool Class::IsArrayAssignableFromArray(const Class* src) const {
  DCHECK(IsArrayClass())  << PrettyClass(this);
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  return GetComponentType()->IsAssignableFrom(src->GetComponentType());
}

bool Class::IsAssignableFromArray(const Class* src) const {
  DCHECK(!IsInterface()) << PrettyClass(this);  // handled first in IsAssignableFrom
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  if (!IsArrayClass()) {
    // If "this" is not also an array, it must be Object.
    // src's super should be java_lang_Object, since it is an array.
    Class* java_lang_Object = src->GetSuperClass();
    DCHECK(java_lang_Object != NULL) << PrettyClass(src);
    DCHECK(java_lang_Object->GetSuperClass() == NULL) << PrettyClass(src);
    return this == java_lang_Object;
  }
  return IsArrayAssignableFromArray(src);
}

bool Class::IsSubClass(const Class* klass) const {
  DCHECK(!IsInterface()) << PrettyClass(this);
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  const Class* current = this;
  do {
    if (current == klass) {
      return true;
    }
    current = current->GetSuperClass();
  } while (current != NULL);
  return false;
}

bool Class::IsInSamePackage(const StringPiece& descriptor1, const StringPiece& descriptor2) {
  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (descriptor1.find('/', i) != StringPiece::npos ||
      descriptor2.find('/', i) != StringPiece::npos) {
    return false;
  } else {
    return true;
  }
}

bool Class::IsInSamePackage(const Class* that) const {
  const Class* klass1 = this;
  const Class* klass2 = that;
  if (klass1 == klass2) {
    return true;
  }
  // Class loaders must match.
  if (klass1->GetClassLoader() != klass2->GetClassLoader()) {
    return false;
  }
  // Arrays are in the same package when their element classes are.
  while (klass1->IsArrayClass()) {
    klass1 = klass1->GetComponentType();
  }
  while (klass2->IsArrayClass()) {
    klass2 = klass2->GetComponentType();
  }
  // Compare the package part of the descriptor string.
  ClassHelper kh(klass1);
  std::string descriptor1(kh.GetDescriptor());
  kh.ChangeClass(klass2);
  std::string descriptor2(kh.GetDescriptor());
  return IsInSamePackage(descriptor1, descriptor2);
}

bool Class::IsClassClass() const {
  Class* java_lang_Class = GetClass()->GetClass();
  return this == java_lang_Class;
}

bool Class::IsStringClass() const {
  return this == String::GetJavaLangString();
}

bool Class::IsThrowableClass() const {
  return WellKnownClasses::ToClass(WellKnownClasses::java_lang_Throwable)->IsAssignableFrom(this);
}

bool Class::IsFieldClass() const {
  Class* java_lang_Class = GetClass();
  Class* java_lang_reflect_Field = java_lang_Class->GetInstanceField(0)->GetClass();
  return this == java_lang_reflect_Field;

}

bool Class::IsMethodClass() const {
  return (this == AbstractMethod::GetMethodClass()) ||
      (this == AbstractMethod::GetConstructorClass());

}

ClassLoader* Class::GetClassLoader() const {
  return GetFieldObject<ClassLoader*>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), false);
}

void Class::SetClassLoader(ClassLoader* new_class_loader) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), new_class_loader, false);
}

AbstractMethod* Class::FindVirtualMethodForInterface(AbstractMethod* method) {
  Class* declaring_class = method->GetDeclaringClass();
  DCHECK(declaring_class != NULL) << PrettyClass(this);
  DCHECK(declaring_class->IsInterface()) << PrettyMethod(method);
  // TODO cache to improve lookup speed
  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->GetInterface(i) == declaring_class) {
      return iftable->GetMethodArray(i)->Get(method->GetMethodIndex());
    }
  }
  return NULL;
}

AbstractMethod* Class::FindInterfaceMethod(const StringPiece& name,  const StringPiece& signature) const {
  // Check the current class before checking the interfaces.
  AbstractMethod* method = FindDeclaredVirtualMethod(name, signature);
  if (method != NULL) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    method = iftable->GetInterface(i)->FindVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindInterfaceMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  // Check the current class before checking the interfaces.
  AbstractMethod* method = FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
  if (method != NULL) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    method = iftable->GetInterface(i)->FindVirtualMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}


AbstractMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const StringPiece& signature) const {
  MethodHelper mh;
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    AbstractMethod* method = GetDirectMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumDirectMethods(); ++i) {
      AbstractMethod* method = GetDirectMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDirectMethod(const StringPiece& name, const StringPiece& signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredDirectMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                         const StringPiece& signature) const {
  MethodHelper mh;
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    AbstractMethod* method = GetVirtualMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumVirtualMethods(); ++i) {
      AbstractMethod* method = GetVirtualMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return NULL;
}

AbstractMethod* Class::FindVirtualMethod(const StringPiece& name, const StringPiece& signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  FieldHelper fh;
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    Field* f = GetInstanceField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumInstanceFields(); ++i) {
      Field* f = GetInstanceField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(dex_cache, dex_field_idx);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& type) {
  DCHECK(type != NULL);
  FieldHelper fh;
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    Field* f = GetStaticField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (dex_cache == GetDexCache()) {
    for (size_t i = 0; i < NumStaticFields(); ++i) {
      Field* f = GetStaticField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredStaticField(dex_cache, dex_field_idx);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(dex_cache, dex_field_idx);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindField(const StringPiece& name, const StringPiece& type) {
  // Find a field using the JLS field resolution order
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
    f = k->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count,
                    size_t component_size) {
  DCHECK(array_class != NULL);
  DCHECK_GE(component_count, 0);
  DCHECK(array_class->IsArrayClass());

  size_t header_size = sizeof(Object) + (component_size == sizeof(int64_t) ? 8 : 4);
  size_t data_size = component_count * component_size;
  size_t size = header_size + data_size;

  // Check for overflow and throw OutOfMemoryError if this was an unreasonable request.
  size_t component_shift = sizeof(size_t) * 8 - 1 - CLZ(component_size);
  if (data_size >> component_shift != size_t(component_count) || size < data_size) {
    self->ThrowNewExceptionF("Ljava/lang/OutOfMemoryError;",
        "%s of length %d would overflow",
        PrettyDescriptor(array_class).c_str(), component_count);
    return NULL;
  }

  Heap* heap = Runtime::Current()->GetHeap();
  Array* array = down_cast<Array*>(heap->AllocObject(self, array_class, size));
  if (array != NULL) {
    DCHECK(array->IsArrayInstance());
    array->SetLength(component_count);
  }
  return array;
}

Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count) {
  DCHECK(array_class->IsArrayClass());
  return Alloc(self, array_class, component_count, array_class->GetComponentSize());
}

// Create a multi-dimensional array of Objects or primitive types.
//
// We have to generate the names for X[], X[][], X[][][], and so on.  The
// easiest way to deal with that is to create the full name once and then
// subtract pieces off.  Besides, we want to start with the outermost
// piece and work our way in.
// Recursively create an array with multiple dimensions.  Elements may be
// Objects or primitive types.
static Array* RecursiveCreateMultiArray(Thread* self, Class* array_class, int current_dimension,
                                        IntArray* dimensions)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  int32_t array_length = dimensions->Get(current_dimension);
  SirtRef<Array> new_array(self, Array::Alloc(self, array_class, array_length));
  if (UNLIKELY(new_array.get() == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;
  }
  if ((current_dimension + 1) < dimensions->GetLength()) {
    // Create a new sub-array in every element of the array.
    for (int32_t i = 0; i < array_length; i++) {
      Array* sub_array = RecursiveCreateMultiArray(self, array_class->GetComponentType(),
                                                   current_dimension + 1, dimensions);
      if (UNLIKELY(sub_array == NULL)) {
        CHECK(self->IsExceptionPending());
        return NULL;
      }
      new_array->AsObjectArray<Array>()->Set(i, sub_array);
    }
  }
  return new_array.get();
}

Array* Array::CreateMultiArray(Thread* self, Class* element_class, IntArray* dimensions) {
  // Verify dimensions.
  //
  // The caller is responsible for verifying that "dimArray" is non-null
  // and has a length > 0 and <= 255.
  int num_dimensions = dimensions->GetLength();
  DCHECK_GT(num_dimensions, 0);
  DCHECK_LE(num_dimensions, 255);

  for (int i = 0; i < num_dimensions; i++) {
    int dimension = dimensions->Get(i);
    if (UNLIKELY(dimension < 0)) {
      self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;",
                               "Dimension %d: %d", i, dimension);
      return NULL;
    }
  }

  // Generate the full name of the array class.
  std::string descriptor(num_dimensions, '[');
  descriptor += ClassHelper(element_class).GetDescriptor();

  // Find/generate the array class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* array_class = class_linker->FindClass(descriptor.c_str(), element_class->GetClassLoader());
  if (UNLIKELY(array_class == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;
  }
  // create the array
  Array* new_array = RecursiveCreateMultiArray(self, array_class, 0, dimensions);
  if (UNLIKELY(new_array == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;
  }
  return new_array;
}

bool Array::ThrowArrayIndexOutOfBoundsException(int32_t index) const {
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
      "length=%i; index=%i", length_, index);
  return false;
}

bool Array::ThrowArrayStoreException(Object* object) const {
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
      "Can't store an element of type %s into an array of type %s",
      PrettyTypeOf(object).c_str(), PrettyTypeOf(this).c_str());
  return false;
}

template<typename T>
PrimitiveArray<T>* PrimitiveArray<T>::Alloc(Thread* self, size_t length) {
  DCHECK(array_class_ != NULL);
  Array* raw_array = Array::Alloc(self, array_class_, length, sizeof(T));
  return down_cast<PrimitiveArray<T>*>(raw_array);
}

template <typename T> Class* PrimitiveArray<T>::array_class_ = NULL;

// Explicitly instantiate all the primitive array types.
template class PrimitiveArray<uint8_t>;   // BooleanArray
template class PrimitiveArray<int8_t>;    // ByteArray
template class PrimitiveArray<uint16_t>;  // CharArray
template class PrimitiveArray<double>;    // DoubleArray
template class PrimitiveArray<float>;     // FloatArray
template class PrimitiveArray<int32_t>;   // IntArray
template class PrimitiveArray<int64_t>;   // LongArray
template class PrimitiveArray<int16_t>;   // ShortArray

// Explicitly instantiate Class[][]
template class ObjectArray<ObjectArray<Class> >;

// TODO: get global references for these
Class* String::java_lang_String_ = NULL;

void String::SetClass(Class* java_lang_String) {
  CHECK(java_lang_String_ == NULL);
  CHECK(java_lang_String != NULL);
  java_lang_String_ = java_lang_String;
}

void String::ResetClass() {
  CHECK(java_lang_String_ != NULL);
  java_lang_String_ = NULL;
}

String* String::Intern() {
  return Runtime::Current()->GetInternTable()->InternWeak(this);
}

int32_t String::GetHashCode() {
  int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), false);
  if (result == 0) {
    ComputeHashCode();
  }
  result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), false);
  DCHECK(result != 0 || ComputeUtf16Hash(GetCharArray(), GetOffset(), GetLength()) == 0)
          << ToModifiedUtf8() << " " << result;
  return result;
}

int32_t String::GetLength() const {
  int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, count_), false);
  DCHECK(result >= 0 && result <= GetCharArray()->GetLength());
  return result;
}

uint16_t String::CharAt(int32_t index) const {
  // TODO: do we need this? Equals is the only caller, and could
  // bounds check itself.
  if (index < 0 || index >= count_) {
    Thread* self = Thread::Current();
    self->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
        "length=%i; index=%i", count_, index);
    return 0;
  }
  return GetCharArray()->Get(index + GetOffset());
}

String* String::AllocFromUtf16(Thread* self,
                               int32_t utf16_length,
                               const uint16_t* utf16_data_in,
                               int32_t hash_code) {
  CHECK(utf16_data_in != NULL || utf16_length == 0);
  String* string = Alloc(self, GetJavaLangString(), utf16_length);
  if (string == NULL) {
    return NULL;
  }
  // TODO: use 16-bit wide memset variant
  CharArray* array = const_cast<CharArray*>(string->GetCharArray());
  if (array == NULL) {
    return NULL;
  }
  for (int i = 0; i < utf16_length; i++) {
    array->Set(i, utf16_data_in[i]);
  }
  if (hash_code != 0) {
    string->SetHashCode(hash_code);
  } else {
    string->ComputeHashCode();
  }
  return string;
}

  String* String::AllocFromModifiedUtf8(Thread* self, const char* utf) {
  if (utf == NULL) {
    return NULL;
  }
  size_t char_count = CountModifiedUtf8Chars(utf);
  return AllocFromModifiedUtf8(self, char_count, utf);
}

String* String::AllocFromModifiedUtf8(Thread* self, int32_t utf16_length,
                                      const char* utf8_data_in) {
  String* string = Alloc(self, GetJavaLangString(), utf16_length);
  if (string == NULL) {
    return NULL;
  }
  uint16_t* utf16_data_out =
      const_cast<uint16_t*>(string->GetCharArray()->GetData());
  ConvertModifiedUtf8ToUtf16(utf16_data_out, utf8_data_in);
  string->ComputeHashCode();
  return string;
}

String* String::Alloc(Thread* self, Class* java_lang_String, int32_t utf16_length) {
  SirtRef<CharArray> array(self, CharArray::Alloc(self, utf16_length));
  if (array.get() == NULL) {
    return NULL;
  }
  return Alloc(self, java_lang_String, array.get());
}

String* String::Alloc(Thread* self, Class* java_lang_String, CharArray* array) {
  // Hold reference in case AllocObject causes GC.
  SirtRef<CharArray> array_ref(self, array);
  String* string = down_cast<String*>(java_lang_String->AllocObject(self));
  if (string == NULL) {
    return NULL;
  }
  string->SetArray(array);
  string->SetCount(array->GetLength());
  return string;
}

bool String::Equals(const String* that) const {
  if (this == that) {
    // Quick reference equality test
    return true;
  } else if (that == NULL) {
    // Null isn't an instanceof anything
    return false;
  } else if (this->GetLength() != that->GetLength()) {
    // Quick length inequality test
    return false;
  } else {
    // Note: don't short circuit on hash code as we're presumably here as the
    // hash code was already equal
    for (int32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const uint16_t* that_chars, int32_t that_offset, int32_t that_length) const {
  if (this->GetLength() != that_length) {
    return false;
  } else {
    for (int32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const char* modified_utf8) const {
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&modified_utf8);
    if (ch == '\0' || ch != CharAt(i)) {
      return false;
    }
  }
  return *modified_utf8 == '\0';
}

bool String::Equals(const StringPiece& modified_utf8) const {
  if (modified_utf8.size() != GetLength()) {
    return false;
  }
  const char* p = modified_utf8.data();
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&p);
    if (ch != CharAt(i)) {
      return false;
    }
  }
  return true;
}

// Create a modified UTF-8 encoded std::string from a java/lang/String object.
std::string String::ToModifiedUtf8() const {
  const uint16_t* chars = GetCharArray()->GetData() + GetOffset();
  size_t byte_count = GetUtfLength();
  std::string result(byte_count, static_cast<char>(0));
  ConvertUtf16ToModifiedUtf8(&result[0], chars, GetLength());
  return result;
}

#ifdef HAVE__MEMCMP16
// "count" is in 16-bit units.
extern "C" uint32_t __memcmp16(const uint16_t* s0, const uint16_t* s1, size_t count);
#define MemCmp16 __memcmp16
#else
static uint32_t MemCmp16(const uint16_t* s0, const uint16_t* s1, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (s0[i] != s1[i]) {
      return static_cast<int32_t>(s0[i]) - static_cast<int32_t>(s1[i]);
    }
  }
  return 0;
}
#endif

int32_t String::CompareTo(String* rhs) const {
  // Quick test for comparison of a string with itself.
  const String* lhs = this;
  if (lhs == rhs) {
    return 0;
  }
  // TODO: is this still true?
  // The annoying part here is that 0x00e9 - 0xffff != 0x00ea,
  // because the interpreter converts the characters to 32-bit integers
  // *without* sign extension before it subtracts them (which makes some
  // sense since "char" is unsigned).  So what we get is the result of
  // 0x000000e9 - 0x0000ffff, which is 0xffff00ea.
  int lhsCount = lhs->GetLength();
  int rhsCount = rhs->GetLength();
  int countDiff = lhsCount - rhsCount;
  int minCount = (countDiff < 0) ? lhsCount : rhsCount;
  const uint16_t* lhsChars = lhs->GetCharArray()->GetData() + lhs->GetOffset();
  const uint16_t* rhsChars = rhs->GetCharArray()->GetData() + rhs->GetOffset();
  int otherRes = MemCmp16(lhsChars, rhsChars, minCount);
  if (otherRes != 0) {
    return otherRes;
  }
  return countDiff;
}

void Throwable::SetCause(Throwable* cause) {
  CHECK(cause != NULL);
  CHECK(cause != this);
  CHECK(GetFieldObject<Throwable*>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), false) == NULL);
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), cause, false);
}

bool Throwable::IsCheckedException() const {
  if (InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_Error))) {
    return false;
  }
  return !InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_RuntimeException));
}

std::string Throwable::Dump() const {
  std::string result(PrettyTypeOf(this));
  result += ": ";
  String* msg = GetDetailMessage();
  if (msg != NULL) {
    result += msg->ToModifiedUtf8();
  }
  result += "\n";
  Object* stack_state = GetStackState();
  // check stack state isn't missing or corrupt
  if (stack_state != NULL && stack_state->IsObjectArray()) {
    // Decode the internal stack trace into the depth and method trace
    ObjectArray<Object>* method_trace = down_cast<ObjectArray<Object>*>(stack_state);
    int32_t depth = method_trace->GetLength() - 1;
    IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));
    MethodHelper mh;
    for (int32_t i = 0; i < depth; ++i) {
      AbstractMethod* method = down_cast<AbstractMethod*>(method_trace->Get(i));
      mh.ChangeMethod(method);
      uint32_t dex_pc = pc_trace->Get(i);
      int32_t line_number = mh.GetLineNumFromDexPC(dex_pc);
      const char* source_file = mh.GetDeclaringClassSourceFile();
      result += StringPrintf("  at %s (%s:%d)\n", PrettyMethod(method, true).c_str(),
                             source_file, line_number);
    }
  }
  Throwable* cause = GetFieldObject<Throwable*>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), false);
  if (cause != NULL && cause != this) {  // Constructor makes cause == this by default.
    result += "Caused by: ";
    result += cause->Dump();
  }
  return result;
}


Class* Throwable::java_lang_Throwable_ = NULL;

void Throwable::SetClass(Class* java_lang_Throwable) {
  CHECK(java_lang_Throwable_ == NULL);
  CHECK(java_lang_Throwable != NULL);
  java_lang_Throwable_ = java_lang_Throwable;
}

void Throwable::ResetClass() {
  CHECK(java_lang_Throwable_ != NULL);
  java_lang_Throwable_ = NULL;
}

Class* StackTraceElement::java_lang_StackTraceElement_ = NULL;

void StackTraceElement::SetClass(Class* java_lang_StackTraceElement) {
  CHECK(java_lang_StackTraceElement_ == NULL);
  CHECK(java_lang_StackTraceElement != NULL);
  java_lang_StackTraceElement_ = java_lang_StackTraceElement;
}

void StackTraceElement::ResetClass() {
  CHECK(java_lang_StackTraceElement_ != NULL);
  java_lang_StackTraceElement_ = NULL;
}

StackTraceElement* StackTraceElement::Alloc(Thread* self,
                                            String* declaring_class,
                                            String* method_name,
                                            String* file_name,
                                            int32_t line_number) {
  StackTraceElement* trace =
      down_cast<StackTraceElement*>(GetStackTraceElement()->AllocObject(self));
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, declaring_class_),
                        const_cast<String*>(declaring_class), false);
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, method_name_),
                        const_cast<String*>(method_name), false);
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, file_name_),
                        const_cast<String*>(file_name), false);
  trace->SetField32(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, line_number_),
                    line_number, false);
  return trace;
}

}  // namespace art
