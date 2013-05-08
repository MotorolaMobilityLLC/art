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

#include "abstract_method.h"

#include "abstract_method-inl.h"
#include "base/stringpiece.h"
#include "class-inl.h"
#include "dex_file-inl.h"
#include "gc/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "jni_internal.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_array-inl.h"
#include "string.h"
#include "object_utils.h"

namespace art {
namespace mirror {

extern "C" void art_portable_invoke_stub(AbstractMethod*, uint32_t*, uint32_t, Thread*, JValue*, char);
extern "C" void art_quick_invoke_stub(AbstractMethod*, uint32_t*, uint32_t, Thread*, JValue*, char);

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

void AbstractMethod::SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_strings_),
                 new_dex_cache_strings, false);
}

void AbstractMethod::SetDexCacheResolvedMethods(ObjectArray<AbstractMethod>* new_dex_cache_methods) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_methods_),
                 new_dex_cache_methods, false);
}

void AbstractMethod::SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_classes) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_types_),
                 new_dex_cache_classes, false);
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

uintptr_t AbstractMethod::NativePcOffset(const uintptr_t pc) const {
  const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
  return pc - reinterpret_cast<uintptr_t>(code);
}

// Find the lowest-address native safepoint pc for a given dex pc
uintptr_t AbstractMethod::ToFirstNativeSafepointPc(const uint32_t dex_pc) const {
#if !defined(ART_USE_PORTABLE_COMPILER)
  const uint32_t* mapping_table = GetPcToDexMappingTable();
  if (mapping_table == NULL) {
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  size_t mapping_table_length = GetPcToDexMappingTableLength();
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    if (mapping_table[i + 1] == dex_pc) {
      const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
      return mapping_table[i] + reinterpret_cast<uintptr_t>(code);
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
#if !defined(ART_USE_PORTABLE_COMPILER)
  const uint32_t* mapping_table = GetPcToDexMappingTable();
  if (mapping_table == NULL) {
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  size_t mapping_table_length = GetPcToDexMappingTableLength();
  const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(code);
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    if (mapping_table[i] == sought_offset) {
      return mapping_table[i + 1];
    }
  }
  LOG(FATAL) << "Failed to find Dex offset for PC offset " << reinterpret_cast<void*>(sought_offset)
             << "(PC " << reinterpret_cast<void*>(pc) << ", code=" << code
             << ") in " << PrettyMethod(this);
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
      const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
      return reinterpret_cast<uintptr_t>(code) + map_offset;
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

void AbstractMethod::Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
                            char result_type) {
  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(kRunnable, self->GetState());
  }

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  Runtime* runtime = Runtime::Current();
  // Call the invoke stub, passing everything as arguments.
  if (UNLIKELY(!runtime->IsStarted())){
    LOG(INFO) << "Not invoking " << PrettyMethod(this) << " for a runtime that isn't started";
    if (result != NULL) {
      result->SetJ(0);
    }
  } else {
    bool interpret = runtime->GetInstrumentation()->InterpretOnly() && !IsNative() &&
        !IsProxyMethod();
    const bool kLogInvocationStartAndReturn = false;
    if (GetCode() != NULL) {
      if (!interpret) {
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << StringPrintf("Invoking '%s' code=%p", PrettyMethod(this).c_str(), GetCode());
        }
#ifdef ART_USE_PORTABLE_COMPILER
        (*art_portable_invoke_stub)(this, args, args_size, self, result, result_type);
#else
        (*art_quick_invoke_stub)(this, args, args_size, self, result, result_type);
#endif
        if (UNLIKELY(reinterpret_cast<int32_t>(self->GetException(NULL)) == -1)) {
          // Unusual case where we were running LLVM generated code and an
          // exception was thrown to force the activations to be removed from the
          // stack. Continue execution in the interpreter.
          self->ClearException();
          ShadowFrame* shadow_frame = self->GetAndClearDeoptimizationShadowFrame(result);
          self->SetTopOfStack(NULL, 0);
          self->SetTopOfShadowStack(shadow_frame);
          interpreter::EnterInterpreterFromDeoptimize(self, shadow_frame, result);
        }
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << StringPrintf("Returned '%s' code=%p", PrettyMethod(this).c_str(), GetCode());
        }
      } else {
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << "Interpreting " << PrettyMethod(this) << "'";
        }
        if (this->IsStatic()) {
          art::interpreter::EnterInterpreterFromInvoke(self, this, NULL, args, result);
        } else {
          Object* receiver = reinterpret_cast<Object*>(args[0]);
          art::interpreter::EnterInterpreterFromInvoke(self, this, receiver, args + 1, result);
        }
        if (kLogInvocationStartAndReturn) {
          LOG(INFO) << "Returned '" << PrettyMethod(this) << "'";
        }
      }
    } else {
      LOG(INFO) << "Not invoking '" << PrettyMethod(this)
          << "' code=" << reinterpret_cast<const void*>(GetCode());
      if (result != NULL) {
        result->SetJ(0);
      }
    }
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);
}

bool AbstractMethod::IsRegistered() const {
  void* native_method = GetFieldPtr<void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_), false);
  CHECK(native_method != NULL);
  void* jni_stub = GetJniDlsymLookupStub();
  return native_method != jni_stub;
}

void AbstractMethod::RegisterNative(Thread* self, const void* native_method) {
  DCHECK(Thread::Current() == self);
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(native_method != NULL) << PrettyMethod(this);
  if (!self->GetJniEnv()->vm->work_around_app_jni_bugs) {
    SetNativeMethod(native_method);
  } else {
    // We've been asked to associate this method with the given native method but are working
    // around JNI bugs, that include not giving Object** SIRT references to native methods. Direct
    // the native method to runtime support and store the target somewhere runtime support will
    // find it.
#if defined(__arm__) && !defined(ART_USE_PORTABLE_COMPILER)
    SetNativeMethod(native_method);
#else
    UNIMPLEMENTED(FATAL);
#endif
    SetFieldPtr<const uint8_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, gc_map_),
        reinterpret_cast<const uint8_t*>(native_method), false);
  }
}

void AbstractMethod::UnregisterNative(Thread* self) {
  CHECK(IsNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(self, GetJniDlsymLookupStub());
}

void AbstractMethod::SetNativeMethod(const void* native_method) {
  SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_),
      native_method, false);
}

}  // namespace mirror
}  // namespace art
