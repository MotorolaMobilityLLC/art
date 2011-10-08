/*
 * Copyright 2011 Google Inc. All Rights Reserved.
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

#include "runtime_support.h"

#include "dex_verifier.h"

namespace art {

// Place a special frame at the TOS that will save the callee saves for the given type
static void  FinishCalleeSaveFrameSetup(Thread* self, Method** sp, Runtime::CalleeSaveType type) {
  // Be aware the store below may well stomp on an incoming argument
  *sp = Runtime::Current()->GetCalleeSaveMethod(type);
  self->SetTopOfStack(sp, 0);
}

// Temporary debugging hook for compiler.
extern void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
}

// Return value helper for jobject return types
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  return thread->DecodeJObject(obj);
}

extern void* FindNativeMethod(Thread* thread) {
  DCHECK(Thread::Current() == thread);

  Method* method = const_cast<Method*>(thread->GetCurrentMethod());
  DCHECK(method != NULL);

  // Lookup symbol address for method, on failure we'll return NULL with an
  // exception set, otherwise we return the address of the method we found.
  void* native_code = thread->GetJniEnv()->vm->FindCodeForNativeMethod(method);
  if (native_code == NULL) {
    DCHECK(thread->IsExceptionPending());
    return NULL;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(native_code);
    return native_code;
  }
}

// Called by generated call to throw an exception
extern "C" void artDeliverExceptionFromCode(Throwable* exception, Thread* thread, Method** sp) {
  /*
   * exception may be NULL, in which case this routine should
   * throw NPE.  NOTE: this is a convenience for generated code,
   * which previously did the null check inline and constructed
   * and threw a NPE if NULL.  This routine responsible for setting
   * exception_ in thread and delivering the exception.
   */
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(exception);
  }
  thread->DeliverException();
}

// Deliver an exception that's pending on thread helping set up a callee save frame on the way
extern "C" void artDeliverPendingExceptionFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->DeliverException();
}

// Called by generated call to throw a NPE exception
extern "C" void artThrowNullPointerExceptionFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewException("Ljava/lang/NullPointerException;", NULL);
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowDivZeroFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowArrayBoundsFromCode(int index, int limit, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", limit, index);
  thread->DeliverException();
}

// Called by the AbstractMethodError stub (not runtime support)
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
  thread->DeliverException();
}

extern "C" void artThrowStackOverflowFromCode(Method* method, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;",
      "stack size %zdkb; default stack size: %zdkb",
      thread->GetStackSize() / KB, Runtime::Current()->GetDefaultStackSize() / KB);
  thread->ResetDefaultStackEnd();  // Return to default stack size
  thread->DeliverException();
}

static std::string ClassNameFromIndex(Method* method, uint32_t ref,
                                      DexVerifier::VerifyErrorRefType ref_type, bool access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  uint16_t type_idx = 0;
  if (ref_type == DexVerifier::VERIFY_ERROR_REF_FIELD) {
    const DexFile::FieldId& id = dex_file.GetFieldId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == DexVerifier::VERIFY_ERROR_REF_METHOD) {
    const DexFile::MethodId& id = dex_file.GetMethodId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == DexVerifier::VERIFY_ERROR_REF_CLASS) {
    type_idx = ref;
  } else {
    CHECK(false) << static_cast<int>(ref_type);
  }

  std::string class_name(PrettyDescriptor(dex_file.dexStringByTypeIdx(type_idx)));
  if (!access) {
    return class_name;
  }

  std::string result;
  result += "tried to access class ";
  result += class_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass()->GetDescriptor());
  return result;
}

static std::string FieldNameFromIndex(const Method* method, uint32_t ref,
                                      DexVerifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(DexVerifier::VERIFY_ERROR_REF_FIELD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::FieldId& id = dex_file.GetFieldId(ref);
  std::string class_name(PrettyDescriptor(dex_file.dexStringByTypeIdx(id.class_idx_)));
  const char* field_name = dex_file.dexStringById(id.name_idx_);
  if (!access) {
    return class_name + "." + field_name;
  }

  std::string result;
  result += "tried to access field ";
  result += class_name + "." + field_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass()->GetDescriptor());
  return result;
}

static std::string MethodNameFromIndex(const Method* method, uint32_t ref,
                                DexVerifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(DexVerifier::VERIFY_ERROR_REF_METHOD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::MethodId& id = dex_file.GetMethodId(ref);
  std::string class_name(PrettyDescriptor(dex_file.dexStringByTypeIdx(id.class_idx_)));
  const char* method_name = dex_file.dexStringById(id.name_idx_);
  if (!access) {
    return class_name + "." + method_name;
  }

  std::string result;
  result += "tried to access method ";
  result += class_name + "." + method_name + ":" +
            dex_file.CreateMethodDescriptor(id.proto_idx_, NULL);
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass()->GetDescriptor());
  return result;
}

extern "C" void artThrowVerificationErrorFromCode(int32_t kind, int32_t ref, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context to interpret 'ref'
  frame.Next();
  Method* method = frame.GetMethod();

  DexVerifier::VerifyErrorRefType ref_type =
      static_cast<DexVerifier::VerifyErrorRefType>(kind >> kVerifyErrorRefTypeShift);

  const char* exception_class = "Ljava/lang/VerifyError;";
  std::string msg;

  switch (static_cast<DexVerifier::VerifyError>(kind & ~(0xff << kVerifyErrorRefTypeShift))) {
  case DexVerifier::VERIFY_ERROR_NO_CLASS:
    exception_class = "Ljava/lang/NoClassDefFoundError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case DexVerifier::VERIFY_ERROR_NO_FIELD:
    exception_class = "Ljava/lang/NoSuchFieldError;";
    msg = FieldNameFromIndex(method, ref, ref_type, false);
    break;
  case DexVerifier::VERIFY_ERROR_NO_METHOD:
    exception_class = "Ljava/lang/NoSuchMethodError;";
    msg = MethodNameFromIndex(method, ref, ref_type, false);
    break;
  case DexVerifier::VERIFY_ERROR_ACCESS_CLASS:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = ClassNameFromIndex(method, ref, ref_type, true);
    break;
  case DexVerifier::VERIFY_ERROR_ACCESS_FIELD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = FieldNameFromIndex(method, ref, ref_type, true);
    break;
  case DexVerifier::VERIFY_ERROR_ACCESS_METHOD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = MethodNameFromIndex(method, ref, ref_type, true);
    break;
  case DexVerifier::VERIFY_ERROR_CLASS_CHANGE:
    exception_class = "Ljava/lang/IncompatibleClassChangeError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case DexVerifier::VERIFY_ERROR_INSTANTIATION:
    exception_class = "Ljava/lang/InstantiationError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case DexVerifier::VERIFY_ERROR_GENERIC:
    // Generic VerifyError; use default exception, no message.
    break;
  case DexVerifier::VERIFY_ERROR_NONE:
    CHECK(false);
    break;
  }
  self->ThrowNewException(exception_class, msg.c_str());
  self->DeliverException();
}

extern "C" void artThrowInternalErrorFromCode(int32_t errnum, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "TODO: internal error detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/InternalError;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowRuntimeExceptionFromCode(int32_t errnum, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "TODO: runtime exception detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/RuntimeException;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowNoSuchMethodFromCode(int32_t method_idx, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context for the method_idx
  frame.Next();
  Method* method = frame.GetMethod();
  self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
      MethodNameFromIndex(method, method_idx, DexVerifier::VERIFY_ERROR_REF_METHOD, false).c_str());
  self->DeliverException();
}

extern "C" void artThrowNegArraySizeFromCode(int32_t size, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  LOG(WARNING) << "UNTESTED artThrowNegArraySizeFromCode";
  thread->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", size);
  thread->DeliverException();
}

void* UnresolvedDirectMethodTrampolineFromCode(int32_t method_idx, void* sp, Thread* thread,
                                               Runtime::TrampolineType type) {
  // TODO: this code is specific to ARM
  // On entry the stack pointed by sp is:
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | R3         |    arg3
  // | R2         |    arg2
  // | R1         |    arg1
  // | R0         | <- sp
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(sp);
  Method** caller_sp = reinterpret_cast<Method**>(&regs[5]);
  uintptr_t caller_pc = regs[4];
  // Record the last top of the managed stack
  thread->SetTopOfStack(caller_sp, caller_pc);
  // Start new JNI local reference state
  JNIEnvExt* env = thread->GetJniEnv();
  uint32_t saved_local_ref_cookie = env->local_ref_cookie;
  env->local_ref_cookie = env->locals.GetSegmentState();
  // Discover shorty (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const char* shorty = linker->MethodShorty(method_idx, *caller_sp);
  size_t shorty_len = strlen(shorty);
  size_t args_in_regs = shorty_len < 3 ? shorty_len : 3;
  if (type == Runtime::kUnknownMethod) {
    uint32_t dex_pc = (*caller_sp)->ToDexPC(caller_pc - 2);
    UNIMPLEMENTED(WARNING) << "Missed argument handlerization in direct method trampoline. "
        "Need to discover method invoke type at " << PrettyMethod(*caller_sp)
        << " PC: " << (void*)dex_pc;
  } else {
    bool is_static = type == Runtime::kStaticMethod;
    // Handlerize references in registers
    int cur_arg = 1;   // skip method_idx in R0, first arg is in R1
    if (!is_static) {
      Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
      cur_arg++;
      AddLocalReference<jobject>(env, obj);
    }
    for(size_t i = 0; i < args_in_regs; i++) {
      char c = shorty[i + 1];  // offset to skip return value
      if (c == 'L') {
        Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
        AddLocalReference<jobject>(env, obj);
      }
      cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
    }
    // Handlerize references in out going arguments
    for(size_t i = 3; i < shorty_len; i++) {
      char c = shorty[i + 1];  // offset to skip return value
      if (c == 'L') {
        Object* obj = reinterpret_cast<Object*>(regs[i + 3]);  // skip R0, LR and Method* of caller
        AddLocalReference<jobject>(env, obj);
      }
      cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
    }
  }
  // Resolve method filling in dex cache
  Method* called = linker->ResolveMethod(method_idx, *caller_sp, true);
  if (!thread->IsExceptionPending()) {
    // We got this far, ensure that the declaring class is initialized
    linker->EnsureInitialized(called->GetDeclaringClass(), true);
  }
  // Restore JNI env state
  env->locals.SetSegmentState(env->local_ref_cookie);
  env->local_ref_cookie = saved_local_ref_cookie;

  void* code;
  if (thread->IsExceptionPending()) {
    // Something went wrong, go into deliver exception with the pending exception in r0
    code = reinterpret_cast<void*>(art_deliver_exception_from_code);
    regs[0] =  reinterpret_cast<uintptr_t>(thread->GetException());
    thread->ClearException();
  } else {
    // Expect class to at least be initializing
    CHECK(called->GetDeclaringClass()->IsInitializing());
    // Set up entry into main method
    regs[0] =  reinterpret_cast<uintptr_t>(called);
    code = const_cast<void*>(called->GetCode());
  }
  return code;
}

// TODO: placeholder.  Helper function to type
Class* InitializeTypeFromCode(uint32_t type_idx, Method* method) {
  /*
   * Should initialize & fix up method->dex_cache_resolved_types_[].
   * Returns initialized type.  Does not return normally if an exception
   * is thrown, but instead initiates the catch.  Should be similar to
   * ClassLinker::InitializeStaticStorageFromCode.
   */
  UNIMPLEMENTED(FATAL);
  return NULL;
}

// TODO: placeholder.  Helper function to resolve virtual method
void ResolveMethodFromCode(Method* method, uint32_t method_idx) {
    /*
     * Slow-path handler on invoke virtual method path in which
     * base method is unresolved at compile-time.  Doesn't need to
     * return anything - just either ensure that
     * method->dex_cache_resolved_methods_(method_idx) != NULL or
     * throw and unwind.  The caller will restart call sequence
     * from the beginning.
     */
}

Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, bool is_static) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* f = class_linker->ResolveField(field_idx, referrer, is_static);
  if (f != NULL) {
    Class* c = f->GetDeclaringClass();
    // If the class is already initializing, we must be inside <clinit>, or
    // we'd still be waiting for the lock.
    if (c->GetStatus() == Class::kStatusInitializing || class_linker->EnsureInitialized(c, true)) {
      return f;
    }
  }
  DCHECK(Thread::Current()->IsExceptionPending()); // Throw exception and unwind
  return NULL;
}

extern "C" Field* artFindInstanceFieldFromCode(uint32_t field_idx, const Method* referrer,
                                               Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return FindFieldFromCode(field_idx, referrer, false);
}

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (!type->IsPrimitive() || type->PrimitiveSize() != sizeof(int64_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of 32-bit primitive on field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->Get32(NULL);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (!type->IsPrimitive() || type->PrimitiveSize() != sizeof(int64_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of 64-bit primitive on field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->Get64(NULL);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (type->IsPrimitive()) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted read of reference on primitive field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      return field->GetObj(NULL);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                       uint32_t new_value, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (!type->IsPrimitive() || type->PrimitiveSize() != sizeof(int32_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of 32-bit primitive to field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->Set32(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                      uint64_t new_value, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (!type->IsPrimitive() || type->PrimitiveSize() != sizeof(int64_t)) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of 64-bit primitive to field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->Set64(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                       Object* new_value, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Field* field = FindFieldFromCode(field_idx, referrer, true);
  if (field != NULL) {
    Class* type = field->GetType();
    if (type->IsPrimitive()) {
      self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                               "Attempted write of reference to primitive field '%s'",
                               PrettyField(field, true).c_str());
    } else {
      field->SetObj(NULL, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
extern "C" Object* artAllocObjectFromCode(uint32_t type_idx, Method* method, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  Runtime* runtime = Runtime::Current();
  if (klass == NULL) {
    klass = runtime->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(self->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (!runtime->GetClassLinker()->EnsureInitialized(klass, true)) {
    DCHECK(self->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject();
}

Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                     Thread* self) {
  if (component_count < 0) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (klass->IsPrimitive() && !klass->IsPrimitiveInt()) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
          "Bad filled array request for type %s",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    } else {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but \'int\'",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    }
    return NULL;  // Failure
  } else {
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

// Helper function to alloc array for OP_FILLED_NEW_ARRAY
extern "C" Array* artCheckAndAllocArrayFromCode(uint32_t type_idx, Method* method,
                                               int32_t component_count, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return CheckAndAllocArrayFromCode(type_idx, method, component_count, self);
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
extern "C" Array* artAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                        Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  if (component_count < 0) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d",
                                         component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  return Array::Alloc(klass, component_count);
}

// Check whether it is safe to cast one class to the other, throw exception and return -1 on failure
extern "C" int artCheckCastFromCode(const Class* a, const Class* b, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK(a->IsClass()) << PrettyClass(a);
  DCHECK(b->IsClass()) << PrettyClass(b);
  if (b->IsAssignableFrom(a)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s",
        PrettyDescriptor(a->GetDescriptor()).c_str(),
        PrettyDescriptor(b->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

// Tests whether 'element' can be assigned into an array of type 'array_class'.
// Returns 0 on success and -1 if an exception is pending.
extern "C" int artCanPutArrayElementFromCode(const Object* element, const Class* array_class,
                                             Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK(array_class != NULL);
  // element can't be NULL as we catch this is screened in runtime_support
  Class* element_class = element->GetClass();
  Class* component_type = array_class->GetComponentType();
  if (component_type->IsAssignableFrom(element_class)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
        "Cannot store an object of type %s in to an array of type %s",
        PrettyDescriptor(element_class->GetDescriptor()).c_str(),
        PrettyDescriptor(array_class->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

Class* InitializeStaticStorage(uint32_t type_idx, const Method* referrer, Thread* self) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (klass == NULL) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referrer->GetDeclaringClass() && referrer->IsClassInitializer()) {
    return klass;
  }
  if (!class_linker->EnsureInitialized(klass, true)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  referrer->GetDexCacheInitializedStaticStorage()->Set(type_idx, klass);
  return klass;
}

extern "C" Class* artInitializeStaticStorageFromCode(uint32_t type_idx, const Method* referrer,
                                                     Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return InitializeStaticStorage(type_idx, referrer, self);
}

String* ResolveStringFromCode(const Method* referrer, uint32_t string_idx) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

extern "C" String* artResolveStringFromCode(Method* referrer, int32_t string_idx,
                                            Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return ResolveStringFromCode(referrer, string_idx);
}

extern "C" int artUnlockObjectFromCode(Object* obj, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  return obj->MonitorExit(self) ? 0 /* Success */ : -1 /* Failure */;
}

extern "C" void artLockObjectFromCode(Object* obj, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsOnly);
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void CheckSuspendFromCode(Thread* thread) {
  // Called when thread->suspend_count_ != 0
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

extern "C" void artTestSuspendFromCode(Thread* thread, Method** sp) {
  // Called when suspend count check value is 0 and thread->suspend_count_ != 0
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsOnly);
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

/*
 * Fill the array with predefined constant values, throwing exceptions if the array is null or
 * not of sufficient length.
 *
 * NOTE: When dealing with a raw dex file, the data to be copied uses
 * little-endian ordering.  Require that oat2dex do any required swapping
 * so this routine can get by with a memcpy().
 *
 * Format of the data:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 */
extern "C" int artHandleFillArrayDataFromCode(Array* array, const uint16_t* table,
                                              Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK_EQ(table[0], 0x0300);
  if (array == NULL) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null array in fill array");
    return -1;  // Error
  }
  DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
  uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
  if (static_cast<int32_t>(size) > array->GetLength()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "failed array fill. length=%d; index=%d", array->GetLength(), size);
    return -1;  // Error
  }
  uint16_t width = table[1];
  uint32_t size_in_bytes = size * width;
  memcpy((char*)array + Array::DataOffset().Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

// See comments in runtime_support_asm.S
extern "C" uint64_t artFindInterfaceMethodInCacheFromCode(uint32_t method_idx,
                                                          Object* this_object ,
                                                          Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsAndArgs);
  if (this_object == NULL) {
    thread->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null receiver during interface dispatch");
    return 0;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Frame frame = thread->GetTopOfStack();  // Compute calling method
  frame.Next();
  Method* caller_method = frame.GetMethod();
  Method* interface_method = class_linker->ResolveMethod(method_idx, caller_method, false);
  if (interface_method == NULL) {
    // Could not resolve interface method. Throw error and unwind
    CHECK(thread->IsExceptionPending());
    return 0;
  }
  Method* method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method);
  if (method == NULL) {
    CHECK(thread->IsExceptionPending());
    return 0;
  }
  const void* code = method->GetCode();

  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
    static const double kMaxLong = (double)(int64_t)0x7fffffffffffffffULL;
    static const double kMinLong = (double)(int64_t)0x8000000000000000ULL;
    if (d >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (d <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (d != d) // NaN case
        return 0;
    else
        return (int64_t)d;
}

int64_t F2L(float f) {
    static const float kMaxLong = (float)(int64_t)0x7fffffffffffffffULL;
    static const float kMinLong = (float)(int64_t)0x8000000000000000ULL;
    if (f >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (f <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (f != f) // NaN case
        return 0;
    else
        return (int64_t)f;
}

}  // namespace art
