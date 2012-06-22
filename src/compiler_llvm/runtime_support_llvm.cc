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

#include "class_linker.h"
#include "compiler_runtime_func_list.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "nth_caller_visitor.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime_support.h"
#include "runtime_support_func_list.h"
#include "runtime_support_llvm.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "thread_list.h"
#include "utils_llvm.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

#include <algorithm>
#include <cstdarg>
#include <stdint.h>

#include "asm_support.h"

namespace art {

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

// This is used by other runtime support functions, NOT FROM CODE. The REAL GetCurrentThread is
// implemented by IRBuilder. (So, ARM can't return R9 in this function.)
// TODO: Maybe remove these which are implemented by IRBuilder after refactor runtime support.
Thread* art_get_current_thread_from_code() {
#if defined(__i386__)
  Thread* ptr;
  __asm__ __volatile__("movl %%fs:(%1), %0"
      : "=r"(ptr)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  return ptr;
#else
  return Thread::Current();
#endif
}

void* art_set_current_thread_from_code(void* thread_object_addr) {
  // Nothing to be done.
  return NULL;
}

void art_lock_object_from_code(Object* obj, Thread* thread) {
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void art_unlock_object_from_code(Object* obj, Thread* thread) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  obj->MonitorExit(thread);
}

void art_test_suspend_from_code(Thread* thread) {
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

ShadowFrame* art_push_shadow_frame_from_code(Thread* thread, ShadowFrame* new_shadow_frame,
                                             Method* method, uint32_t size) {
  ShadowFrame* old_frame = thread->PushShadowFrame(new_shadow_frame);
  new_shadow_frame->SetMethod(method);
  new_shadow_frame->SetNumberOfReferences(size);
  return old_frame;
}

void art_pop_shadow_frame_from_code(void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

void art_mark_gc_card_from_code(void *, void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

bool art_is_exception_pending_from_code() {
  LOG(FATAL) << "Implemented by IRBuilder.";
  return false;
}

void art_throw_div_zero_from_code() {
  Thread* thread = art_get_current_thread_from_code();
  thread->ThrowNewException("Ljava/lang/ArithmeticException;",
                            "divide by zero");
}

void art_throw_array_bounds_from_code(int32_t index, int32_t length) {
  Thread* thread = art_get_current_thread_from_code();
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", length, index);
}

void art_throw_no_such_method_from_code(int32_t method_idx) {
  Thread* thread = art_get_current_thread_from_code();
  // We need the calling method as context for the method_idx
  Method* method = thread->GetCurrentMethod();
  thread->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            MethodNameFromIndex(method,
                                                method_idx,
                                                verifier::VERIFY_ERROR_REF_METHOD,
                                                false).c_str());
}

void art_throw_null_pointer_exception_from_code(uint32_t dex_pc) {
  Thread* thread = art_get_current_thread_from_code();
  NthCallerVisitor visitor(thread->GetManagedStack(), 0);
  visitor.WalkStack();
  Method* throw_method = visitor.caller;
  ThrowNullPointerExceptionFromDexPC(thread, throw_method, dex_pc);
}

void art_throw_stack_overflow_from_code() {
  Thread* thread = art_get_current_thread_from_code();
  if (Runtime::Current()->IsMethodTracingActive()) {
    TraceMethodUnwindFromCode(thread);
  }
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute.
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;", "stack size %s",
                             PrettySize(thread->GetStackSize()).c_str());
  thread->ResetDefaultStackEnd();  // Return to default stack size.
}

void art_throw_exception_from_code(Object* exception) {
  Thread* thread = art_get_current_thread_from_code();
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(static_cast<Throwable*>(exception));
  }
}

void art_throw_verification_error_from_code(Method* current_method,
                                            int32_t kind,
                                            int32_t ref) {
  ThrowVerificationError(art_get_current_thread_from_code(), current_method, kind, ref);
}

int32_t art_find_catch_block_from_code(Method* current_method,
                                       uint32_t ti_offset) {
  Thread* thread = art_get_current_thread_from_code();
  Class* exception_type = thread->GetException()->GetClass();
  MethodHelper mh(current_method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  DCHECK_LT(ti_offset, code_item->tries_size_);
  const DexFile::TryItem* try_item = DexFile::GetTryItems(*code_item, ti_offset);

  int iter_index = 0;
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, *try_item); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return iter_index;
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return iter_index;
    }
    ++iter_index;
  }
  // Handler not found
  return -1;
}


//----------------------------------------------------------------------------
// Object Space
//----------------------------------------------------------------------------

Object* art_alloc_object_from_code(uint32_t type_idx,
                                   Method* referrer,
                                   Thread* thread) {
  return AllocObjectFromCode(type_idx, referrer, thread, false);
}

Object* art_alloc_object_from_code_with_access_check(uint32_t type_idx,
                                                     Method* referrer,
                                                     Thread* thread) {
  return AllocObjectFromCode(type_idx, referrer, thread, true);
}

Object* art_alloc_array_from_code(uint32_t type_idx,
                                  Method* referrer,
                                  uint32_t length,
                                  Thread* thread) {
  return AllocArrayFromCode(type_idx, referrer, length, thread, false);
}

Object* art_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                    Method* referrer,
                                                    uint32_t length,
                                                    Thread* thread) {
  return AllocArrayFromCode(type_idx, referrer, length, thread, true);
}

Object* art_check_and_alloc_array_from_code(uint32_t type_idx,
                                            Method* referrer,
                                            uint32_t length,
                                            Thread* thread) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, false);
}

Object* art_check_and_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                              Method* referrer,
                                                              uint32_t length,
                                                              Thread* thread) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, true);
}

static Method* FindMethodHelper(uint32_t method_idx, Object* this_object, Method* caller_method,
                                bool access_check, InvokeType type, Thread* thread) {
  Method* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == NULL)) {
    method = FindMethodFromCode(method_idx, this_object, caller_method,
                                thread, access_check, type);
    if (UNLIKELY(method == NULL)) {
      CHECK(thread->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!thread->IsExceptionPending());
  return method;
}

Object* art_find_static_method_from_code_with_access_check(uint32_t method_idx,
                                                           Object* this_object,
                                                           Method* referrer,
                                                           Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kStatic, thread);
}

Object* art_find_direct_method_from_code_with_access_check(uint32_t method_idx,
                                                           Object* this_object,
                                                           Method* referrer,
                                                           Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kDirect, thread);
}

Object* art_find_virtual_method_from_code_with_access_check(uint32_t method_idx,
                                                            Object* this_object,
                                                            Method* referrer,
                                                            Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kVirtual, thread);
}

Object* art_find_super_method_from_code_with_access_check(uint32_t method_idx,
                                                          Object* this_object,
                                                          Method* referrer,
                                                          Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kSuper, thread);
}

Object*
art_find_interface_method_from_code_with_access_check(uint32_t method_idx,
                                                      Object* this_object,
                                                      Method* referrer,
                                                      Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kInterface, thread);
}

Object* art_find_interface_method_from_code(uint32_t method_idx,
                                            Object* this_object,
                                            Method* referrer,
                                            Thread* thread) {
  return FindMethodHelper(method_idx, this_object, referrer, false, kInterface, thread);
}

Object* art_initialize_static_storage_from_code(uint32_t type_idx,
                                                Method* referrer,
                                                Thread* thread) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, true, false);
}

Object* art_initialize_type_from_code(uint32_t type_idx,
                                      Method* referrer,
                                      Thread* thread) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, false);
}

Object* art_initialize_type_and_verify_access_from_code(uint32_t type_idx,
                                                        Method* referrer,
                                                        Thread* thread) {
  // Called when caller isn't guaranteed to have access to a type and the dex cache may be
  // unpopulated
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, true);
}

Object* art_resolve_string_from_code(Method* referrer, uint32_t string_idx) {
  return ResolveStringFromCode(referrer, string_idx);
}

int32_t art_set32_static_from_code(uint32_t field_idx, Method* referrer, int32_t new_value) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, true, true, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set64_static_from_code(uint32_t field_idx, Method* referrer, int64_t new_value) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, true, true, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set_obj_static_from_code(uint32_t field_idx, Method* referrer, Object* new_value) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(NULL, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(NULL, new_value);
    return 0;
  }
  return -1;
}

int32_t art_get32_static_from_code(uint32_t field_idx, Method* referrer) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, true, false, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  return 0;
}

int64_t art_get64_static_from_code(uint32_t field_idx, Method* referrer) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, true, false, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  return 0;
}

Object* art_get_obj_static_from_code(uint32_t field_idx, Method* referrer) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            true, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  return 0;
}

int32_t art_set32_instance_from_code(uint32_t field_idx, Method* referrer,
                                     Object* obj, uint32_t new_value) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, true, true, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set64_instance_from_code(uint32_t field_idx, Method* referrer,
                                     Object* obj, int64_t new_value) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, true, true, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set_obj_instance_from_code(uint32_t field_idx, Method* referrer,
                                       Object* obj, Object* new_value) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_get32_instance_from_code(uint32_t field_idx, Method* referrer, Object* obj) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, true, false, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  return 0;
}

int64_t art_get64_instance_from_code(uint32_t field_idx, Method* referrer, Object* obj) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, true, false, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  return 0;
}

Object* art_get_obj_instance_from_code(uint32_t field_idx, Method* referrer, Object* obj) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            false, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  return 0;
}

Object* art_decode_jobject_in_thread(Thread* self, jobject java_object) {
  if (self->IsExceptionPending()) {
    return NULL;
  }
  Object* o = self->DecodeJObject(java_object);
  if (o == NULL || !self->GetJniEnv()->check_jni) {
    return o;
  }

  if (o == kInvalidIndirectRefObject) {
    JniAbortF(NULL, "invalid reference returned from %s",
              PrettyMethod(self->GetCurrentMethod()).c_str());
  }

  // Make sure that the result is an instance of the type this
  // method was expected to return.
  Method* m = self->GetCurrentMethod();
  MethodHelper mh(m);
  Class* return_type = mh.GetReturnType();

  if (!o->InstanceOf(return_type)) {
    JniAbortF(NULL, "attempt to return an instance of %s from %s",
              PrettyTypeOf(o).c_str(), PrettyMethod(m).c_str());
  }

  return o;
}

void art_fill_array_data_from_code(Method* method, uint32_t dex_pc,
                                   Array* array, uint32_t payload_offset) {
  // Test: Is array equal to null? (Guard NullPointerException)
  if (UNLIKELY(array == NULL)) {
    art_throw_null_pointer_exception_from_code(dex_pc);
    return;
  }

  // Find the payload from the CodeItem
  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();

  DCHECK_GT(code_item->insns_size_in_code_units_, payload_offset);

  const Instruction::ArrayDataPayload* payload =
    reinterpret_cast<const Instruction::ArrayDataPayload*>(
        code_item->insns_ + payload_offset);

  DCHECK_EQ(payload->ident,
            static_cast<uint16_t>(Instruction::kArrayDataSignature));

  // Test: Is array big enough?
  uint32_t array_len = static_cast<uint32_t>(array->GetLength());
  if (UNLIKELY(array_len < payload->element_count)) {
    int32_t last_index = payload->element_count - 1;
    art_throw_array_bounds_from_code(array_len, last_index);
    return;
  }

  // Copy the data
  size_t size = payload->element_width * payload->element_count;
  memcpy(array->GetRawData(payload->element_width), payload->data, size);
}



//----------------------------------------------------------------------------
// Type checking, in the nature of casting
//----------------------------------------------------------------------------

int32_t art_is_assignable_from_code(const Class* dest_type, const Class* src_type) {
  DCHECK(dest_type != NULL);
  DCHECK(src_type != NULL);
  return dest_type->IsAssignableFrom(src_type) ? 1 : 0;
}

void art_check_cast_from_code(const Class* dest_type, const Class* src_type) {
  DCHECK(dest_type->IsClass()) << PrettyClass(dest_type);
  DCHECK(src_type->IsClass()) << PrettyClass(src_type);
  if (UNLIKELY(!dest_type->IsAssignableFrom(src_type))) {
    Thread* thread = art_get_current_thread_from_code();
    thread->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
                               "%s cannot be cast to %s",
                               PrettyDescriptor(src_type).c_str(),
                               PrettyDescriptor(dest_type).c_str());
  }
}

void art_check_put_array_element_from_code(const Object* element, const Object* array) {
  if (element == NULL) {
    return;
  }
  DCHECK(array != NULL);
  Class* array_class = array->GetClass();
  DCHECK(array_class != NULL);
  Class* component_type = array_class->GetComponentType();
  Class* element_class = element->GetClass();
  if (UNLIKELY(!component_type->IsAssignableFrom(element_class))) {
    Thread* thread = art_get_current_thread_from_code();
    thread->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                               "%s cannot be stored in an array of type %s",
                               PrettyDescriptor(element_class).c_str(),
                               PrettyDescriptor(array_class).c_str());
  }
  return;
}

//----------------------------------------------------------------------------
// Runtime Support Function Lookup Callback
//----------------------------------------------------------------------------

#define EXTERNAL_LINKAGE(NAME) \
extern "C" void NAME(...);
COMPILER_RUNTIME_FUNC_LIST_NATIVE(EXTERNAL_LINKAGE)
#undef EXTERNAL_LINKAGE

static void* art_find_compiler_runtime_func(char const* name) {
// TODO: If target support some math func, use the target's version. (e.g. art_d2i -> __aeabi_d2iz)
  static const char* const names[] = {
#define DEFINE_ENTRY(NAME) #NAME ,
    COMPILER_RUNTIME_FUNC_LIST_NATIVE(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static void* const funcs[] = {
#define DEFINE_ENTRY(NAME) reinterpret_cast<void*>(NAME) ,
    COMPILER_RUNTIME_FUNC_LIST_NATIVE(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static const size_t num_entries = sizeof(names) / sizeof(const char* const);

  const char* const* const names_begin = names;
  const char* const* const names_end = names + num_entries;

  const char* const* name_lbound_ptr =
      std::lower_bound(names_begin, names_end, name,
                       CStringLessThanComparator());

  if (name_lbound_ptr < names_end && strcmp(*name_lbound_ptr, name) == 0) {
    return funcs[name_lbound_ptr - names_begin];
  } else {
    return NULL;
  }
}

// TODO: This runtime support function is the temporary solution for the link issue.
// It calls to this function before invoking any function, and this function will check:
// 1. The code address is 0.                       -> Link the code by ELFLoader
// That will solved by in-place linking at image loading.
const void* art_fix_stub_from_code(Method* called) {
  const void* code = called->GetCode();
  // 1. The code address is 0.                       -> Link the code by ELFLoader
  if (UNLIKELY(code == NULL)) {
    Runtime::Current()->GetClassLinker()->LinkOatCodeFor(called);
    return called->GetCode();
  }

  return code;
}

// Handler for invocation on proxy methods. We create a boxed argument array. And we invoke
// the invocation handler which is a field within the proxy object receiver.
void art_proxy_invoke_handler_from_code(Method* proxy_method, ...) {
  va_list ap;
  va_start(ap, proxy_method);

  Object* receiver = va_arg(ap, Object*);
  Thread* thread = va_arg(ap, Thread*);
  MethodHelper proxy_mh(proxy_method);
  const size_t num_params = proxy_mh.NumArgs();

  // Start new JNI local reference state
  JNIEnvExt* env = thread->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);

  // Create local ref. copies of the receiver
  jobject rcvr_jobj = AddLocalReference<jobject>(env, receiver);

  // Convert proxy method into expected interface method
  Method* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);

  // Set up arguments array and place in local IRT during boxing (which may allocate/GC)
  jvalue args_jobj[3];
  args_jobj[0].l = rcvr_jobj;
  args_jobj[1].l = AddLocalReference<jobject>(env, interface_method);
  // Args array, if no arguments then NULL (don't include receiver in argument count)
  args_jobj[2].l = NULL;
  ObjectArray<Object>* args = NULL;
  if ((num_params - 1) > 0) {
    args = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(num_params - 1);
    if (args == NULL) {
      CHECK(thread->IsExceptionPending());
      return;
    }
    args_jobj[2].l = AddLocalReference<jobjectArray>(env, args);
  }

  // Get parameter types.
  const char* shorty = proxy_mh.GetShorty();
  ObjectArray<Class>* param_types = proxy_mh.GetParameterTypes();
  if (param_types == NULL) {
    CHECK(thread->IsExceptionPending());
    return;
  }

  // Box arguments.
  for (size_t i = 0; i < (num_params - 1);++i) {
    JValue val;
    switch (shorty[i+1]) {
      case 'Z':
        val.SetZ(va_arg(ap, jint));
        break;
      case 'B':
        val.SetB(va_arg(ap, jint));
        break;
      case 'C':
        val.SetC(va_arg(ap, jint));
        break;
      case 'S':
        val.SetS(va_arg(ap, jint));
        break;
      case 'I':
        val.SetI(va_arg(ap, jint));
        break;
      case 'F':
        val.SetI(va_arg(ap, jint)); // TODO: is this right?
        break;
      case 'L':
        val.SetL(va_arg(ap, Object*));
        break;
      case 'D':
      case 'J':
        val.SetJ(va_arg(ap, jlong)); // TODO: is this right for double?
        break;
    }
    Class* param_type = param_types->Get(i);
    if (param_type->IsPrimitive()) {
      BoxPrimitive(param_type->GetPrimitiveType(), val);
      if (thread->IsExceptionPending()) {
        return;
      }
    }
    args->Set(i, val.GetL());
  }

  DCHECK(env->IsInstanceOf(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy));

  jobject inv_hand = env->GetObjectField(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy_h);
  // Call InvocationHandler.invoke
  jobject result = env->CallObjectMethodA(inv_hand, WellKnownClasses::java_lang_reflect_InvocationHandler_invoke, args_jobj);

  // Place result in stack args
  if (!thread->IsExceptionPending()) {
    if (shorty[0] == 'V') {
      return;
    }
    Object* result_ref = thread->DecodeJObject(result);
    JValue* result_unboxed = va_arg(ap, JValue*);
    if (result_ref == NULL) {
      result_unboxed->SetL(NULL);
    } else {
      bool unboxed_okay = UnboxPrimitiveForResult(result_ref, proxy_mh.GetReturnType(), *result_unboxed);
      if (!unboxed_okay) {
        thread->ClearException();
        thread->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
                                 "Couldn't convert result of type %s to %s",
                                 PrettyTypeOf(result_ref).c_str(),
                                 PrettyDescriptor(proxy_mh.GetReturnType()).c_str());
        return;
      }
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    Throwable* exception = thread->GetException();
    if (exception->IsCheckedException()) {
      SynthesizedProxyClass* proxy_class =
          down_cast<SynthesizedProxyClass*>(proxy_method->GetDeclaringClass());
      int throws_index = -1;
      size_t num_virt_methods = proxy_class->NumVirtualMethods();
      for (size_t i = 0; i < num_virt_methods; i++) {
        if (proxy_class->GetVirtualMethod(i) == proxy_method) {
          throws_index = i;
          break;
        }
      }
      CHECK_NE(throws_index, -1);
      ObjectArray<Class>* declared_exceptions = proxy_class->GetThrows()->Get(throws_index);
      Class* exception_class = exception->GetClass();
      bool declares_exception = false;
      for (int i = 0; i < declared_exceptions->GetLength() && !declares_exception; i++) {
        Class* declared_exception = declared_exceptions->Get(i);
        declares_exception = declared_exception->IsAssignableFrom(exception_class);
      }
      if (!declares_exception) {
        thread->ThrowNewWrappedException("Ljava/lang/reflect/UndeclaredThrowableException;", NULL);
      }
    }
  }

  va_end(ap);
}

void* art_find_runtime_support_func(void* context, char const* name) {
  struct func_entry_t {
    char const* name;
    size_t name_len;
    void* addr;
  };

  static struct func_entry_t const tab[] = {
#define DEFINE_ENTRY(ID, NAME) \
    { #NAME, sizeof(#NAME) - 1, reinterpret_cast<void*>(NAME) },
    RUNTIME_SUPPORT_FUNC_LIST(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static size_t const tab_size = sizeof(tab) / sizeof(struct func_entry_t);

  // Search the compiler runtime (such as __divdi3)
  void* result = art_find_compiler_runtime_func(name);
  if (result != NULL) {
    return result;
  }

  // Note: Since our table is small, we are using trivial O(n) searching
  // function.  For bigger table, it will be better to use binary
  // search or hash function.
  size_t i;
  size_t name_len = strlen(name);
  for (i = 0; i < tab_size; ++i) {
    if (name_len == tab[i].name_len && strcmp(name, tab[i].name) == 0) {
      return tab[i].addr;
    }
  }

  LOG(FATAL) << "Error: Can't find symbol " << name;
  return 0;
}

}  // namespace art
