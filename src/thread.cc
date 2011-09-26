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

#include "thread.h"

#include <dynamic_annotations.h>
#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "class_linker.h"
#include "context.h"
#include "heap.h"
#include "jni_internal.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support.h"
#include "scoped_jni_thread_state.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

static Class* gThrowable = NULL;
static Field* gThread_daemon = NULL;
static Field* gThread_group = NULL;
static Field* gThread_lock = NULL;
static Field* gThread_name = NULL;
static Field* gThread_priority = NULL;
static Field* gThread_uncaughtHandler = NULL;
static Field* gThread_vmData = NULL;
static Field* gThreadGroup_name = NULL;
static Method* gThread_run = NULL;
static Method* gThreadGroup_removeThread = NULL;
static Method* gUncaughtExceptionHandler_uncaughtException = NULL;

// Temporary debugging hook for compiler.
void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
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
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(exception);
  }
  thread->DeliverException();
}

// Deliver an exception that's pending on thread helping set up a callee save frame on the way
extern "C" void artDeliverPendingExceptionFromCode(Thread* thread, Method** sp) {
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->DeliverException();
}

// Called by generated call to throw a NPE exception
extern "C" void artThrowNullPointerExceptionFromCode(Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/NullPointerException;", "unexpected null reference");
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowDivZeroFromCode(Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowArrayBoundsFromCode(int index, int limit, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/ArrayIndexOutOfBoundsException;",
                            "length=%d; index=%d", limit, index);
  thread->DeliverException();
}

// Called by the AbstractMethodError stub (not runtime support)
void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp) {
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/AbstractMethodError;",
                            "abstract method \"%s\"",
                            PrettyMethod(method).c_str());
  thread->DeliverException();
}

extern "C" void artThrowStackOverflowFromCode(Method* method, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute
  thread->ThrowNewException("Ljava/lang/StackOverflowError;",
                            "stack size %zdkb; default stack size: %zdkb",
                            thread->GetStackSize() / KB, runtime->GetDefaultStackSize() / KB);
  thread->ResetDefaultStackEnd();  // Return to default stack size
  thread->DeliverException();
}

extern "C" void artThrowVerificationErrorFromCode(int32_t src1, int32_t ref, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: verifcation error detail message. src1=" << src1 << " ref=" << ref;
  thread->ThrowNewException("Ljava/lang/VerifyError;",
                            "TODO: verifcation error detail message. src1=%d; ref=%d", src1, ref);
  thread->DeliverException();
}

extern "C" void artThrowInternalErrorFromCode(int32_t errnum, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: internal error detail message. errnum=" << errnum;
  thread->ThrowNewException("Ljava/lang/InternalError;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowRuntimeExceptionFromCode(int32_t errnum, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: runtime exception detail message. errnum=" << errnum;
  thread->ThrowNewException("Ljava/lang/RuntimeException;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowNoSuchMethodFromCode(int32_t method_idx, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: no such method exception detail message. method_idx=" << method_idx;
  thread->ThrowNewException("Ljava/lang/NoSuchMethodError;", "method_idx=%d", method_idx);
  thread->DeliverException();
}

extern "C" void artThrowNegArraySizeFromCode(int32_t size, Thread* thread, Method** sp) {
  LOG(WARNING) << "UNTESTED artThrowNegArraySizeFromCode";
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/NegativeArraySizeException;", "%d", size);
  thread->DeliverException();
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

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
extern "C" Object* artAllocObjectFromCode(uint32_t type_idx, Method* method) {
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  return klass->AllocObject();
}

// Helper function to alloc array for OP_FILLED_NEW_ARRAY
extern "C" Array* artCheckAndArrayAllocFromCode(uint32_t type_idx, Method* method,
                                                int32_t component_count) {
  if (component_count < 0) {
    Thread::Current()->ThrowNewException("Ljava/lang/NegativeArraySizeException;", "%d",
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
  }
  if (klass->IsPrimitive() && !klass->IsPrimitiveInt()) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;",
          "Bad filled array request for type %s",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    } else {
      Thread::Current()->ThrowNewException("Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but \'int\'",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    }
    return NULL;  // Failure
  } else {
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
extern "C" Array* artArrayAllocFromCode(uint32_t type_idx, Method* method, int32_t component_count) {
  if (component_count < 0) {
    Thread::Current()->ThrowNewException("Ljava/lang/NegativeArraySizeException;", "%d",
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
extern "C" int artCheckCastFromCode(const Class* a, const Class* b) {
  DCHECK(a->IsClass()) << PrettyClass(a);
  DCHECK(b->IsClass()) << PrettyClass(b);
  if (b->IsAssignableFrom(a)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewException("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s",
        PrettyDescriptor(a->GetDescriptor()).c_str(),
        PrettyDescriptor(b->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

// Tests whether 'element' can be assigned into an array of type 'array_class'.
// Returns 0 on success and -1 if an exception is pending.
extern "C" int artCanPutArrayElementFromCode(const Object* element, const Class* array_class) {
  DCHECK(array_class != NULL);
  // element can't be NULL as we catch this is screened in runtime_support
  Class* element_class = element->GetClass();
  Class* component_type = array_class->GetComponentType();
  if (component_type->IsAssignableFrom(element_class)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewException("Ljava/lang/ArrayStoreException;",
        "Cannot store an object of type %s in to an array of type %s",
        PrettyDescriptor(element_class->GetDescriptor()).c_str(),
        PrettyDescriptor(array_class->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

extern "C" int artUnlockObjectFromCode(Thread* thread, Object* obj) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  return obj->MonitorExit(thread) ? 0 /* Success */ : -1 /* Failure */;
}

void LockObjectFromCode(Thread* thread, Object* obj) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  obj->MonitorEnter(thread);
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

extern "C" void artCheckSuspendFromCode(Thread* thread) {
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
extern "C" int artHandleFillArrayDataFromCode(Array* array, const uint16_t* table) {
  DCHECK_EQ(table[0], 0x0300);
  if (array == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;",
                                         "null array in fill array");
    return -1;  // Error
  }
  DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
  uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
  if (static_cast<int32_t>(size) > array->GetLength()) {
    Thread::Current()->ThrowNewException("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                         "failed array fill. length=%d; index=%d",
                                         array->GetLength(), size);
    return -1;  // Error
  }
  uint16_t width = table[1];
  uint32_t size_in_bytes = size * width;
  memcpy((char*)array + Array::DataOffset().Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

// See comments in runtime_support.S
extern "C" uint64_t artFindInterfaceMethodInCacheFromCode(uint32_t method_idx,
                                                          Object* this_object ,
                                                          Method* caller_method) {
  Thread* thread = Thread::Current();
  if (this_object == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;",
                              "null receiver during interface dispatch");
    return 0;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
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

// TODO: move to more appropriate location
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

// Return value helper for jobject return types
static Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  return thread->DecodeJObject(obj);
}

void Thread::InitFunctionPointers() {
#if defined(__arm__)
  pShlLong = art_shl_long;
  pShrLong = art_shr_long;
  pUshrLong = art_ushr_long;
  pIdiv = __aeabi_idiv;
  pIdivmod = __aeabi_idivmod;
  pI2f = __aeabi_i2f;
  pF2iz = __aeabi_f2iz;
  pD2f = __aeabi_d2f;
  pF2d = __aeabi_f2d;
  pD2iz = __aeabi_d2iz;
  pL2f = __aeabi_l2f;
  pL2d = __aeabi_l2d;
  pFadd = __aeabi_fadd;
  pFsub = __aeabi_fsub;
  pFdiv = __aeabi_fdiv;
  pFmul = __aeabi_fmul;
  pFmodf = fmodf;
  pDadd = __aeabi_dadd;
  pDsub = __aeabi_dsub;
  pDdiv = __aeabi_ddiv;
  pDmul = __aeabi_dmul;
  pFmod = fmod;
  pLdivmod = __aeabi_ldivmod;
  pLmul = __aeabi_lmul;
  pAllocObjectFromCode = art_alloc_object_from_code;
  pArrayAllocFromCode = art_array_alloc_from_code;
  pCanPutArrayElementFromCode = art_can_put_array_element_from_code;
  pCheckAndArrayAllocFromCode = art_check_and_array_alloc_from_code;
  pCheckCastFromCode = art_check_cast_from_code;
  pHandleFillArrayDataFromCode = art_handle_fill_data_from_code;
  pInitializeStaticStorage = art_initialize_static_storage_from_code;
  pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
  pTestSuspendFromCode = art_test_suspend;
  pThrowArrayBoundsFromCode = art_throw_array_bounds_from_code;
  pThrowDivZeroFromCode = art_throw_div_zero_from_code;
  pThrowInternalErrorFromCode = art_throw_internal_error_from_code;
  pThrowNegArraySizeFromCode = art_throw_neg_array_size_from_code;
  pThrowNoSuchMethodFromCode = art_throw_no_such_method_from_code;
  pThrowNullPointerFromCode = art_throw_null_pointer_exception_from_code;
  pThrowRuntimeExceptionFromCode = art_throw_runtime_exception_from_code;
  pThrowStackOverflowFromCode = art_throw_stack_overflow_from_code;
  pThrowVerificationErrorFromCode = art_throw_verification_error_from_code;
  pUnlockObjectFromCode = art_unlock_object_from_code;
#endif
  pDeliverException = art_deliver_exception_from_code;
  pThrowAbstractMethodErrorFromCode = ThrowAbstractMethodErrorFromCode;
  pF2l = F2L;
  pD2l = D2L;
  pMemcpy = memcpy;
  pGet32Static = Field::Get32StaticFromCode;
  pSet32Static = Field::Set32StaticFromCode;
  pGet64Static = Field::Get64StaticFromCode;
  pSet64Static = Field::Set64StaticFromCode;
  pGetObjStatic = Field::GetObjStaticFromCode;
  pSetObjStatic = Field::SetObjStaticFromCode;
  pInitializeTypeFromCode = InitializeTypeFromCode;
  pResolveMethodFromCode = ResolveMethodFromCode;
  pInstanceofNonTrivialFromCode = Object::InstanceOf;
  pLockObjectFromCode = LockObjectFromCode;
  pFindInstanceFieldFromCode = Field::FindInstanceFieldFromCode;
  pCheckSuspendFromCode = artCheckSuspendFromCode;
  pFindNativeMethod = FindNativeMethod;
  pDecodeJObjectInThread = DecodeJObjectInThread;
  pDebugMe = DebugMe;
}

void Frame::Next() {
  size_t frame_size = GetMethod()->GetFrameSizeInBytes();
  DCHECK_NE(frame_size, 0u);
  DCHECK_LT(frame_size, 1024u);
  byte* next_sp = reinterpret_cast<byte*>(sp_) + frame_size;
  sp_ = reinterpret_cast<Method**>(next_sp);
  if (*sp_ != NULL) {
    DCHECK((*sp_)->GetClass() == Method::GetMethodClass() ||
        (*sp_)->GetClass() == Method::GetConstructorClass());
  }
}

bool Frame::HasMethod() const {
  return GetMethod() != NULL && (!GetMethod()->IsPhony());
}

uintptr_t Frame::GetReturnPC() const {
  byte* pc_addr = reinterpret_cast<byte*>(sp_) + GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

uintptr_t Frame::LoadCalleeSave(int num) const {
  // Callee saves are held at the top of the frame
  Method* method = GetMethod();
  DCHECK(method != NULL);
  size_t frame_size = method->GetFrameSizeInBytes();
  byte* save_addr = reinterpret_cast<byte*>(sp_) + frame_size - ((num + 1) * kPointerSize);
#if defined(__i386__)
  save_addr -= kPointerSize;  // account for return address
#endif
  return *reinterpret_cast<uintptr_t*>(save_addr);
}

Method* Frame::NextMethod() const {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  return *reinterpret_cast<Method**>(next_sp);
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();

  self->Attach(runtime);

  String* thread_name = reinterpret_cast<String*>(gThread_name->GetObject(self->peer_));
  if (thread_name != NULL) {
    SetThreadName(thread_name->ToModifiedUtf8().c_str());
  }

  // Wait until it's safe to start running code. (There may have been a suspend-all
  // in progress while we were starting up.)
  runtime->GetThreadList()->WaitForGo();

  // TODO: say "hi" to the debugger.
  //if (gDvm.debuggerConnected) {
  //  dvmDbgPostThreadStart(self);
  //}

  // Invoke the 'run' method of our java.lang.Thread.
  CHECK(self->peer_ != NULL);
  Object* receiver = self->peer_;
  Method* m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(gThread_run);
  m->Invoke(self, receiver, NULL, NULL);

  // Detach.
  runtime->GetThreadList()->Unregister();

  return NULL;
}

void SetVmData(Object* managed_thread, Thread* native_thread) {
  gThread_vmData->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

Thread* Thread::FromManagedThread(JNIEnv* env, jobject java_thread) {
  Object* thread = Decode<Object*>(env, java_thread);
  return reinterpret_cast<Thread*>(static_cast<uintptr_t>(gThread_vmData->GetInt(thread)));
}

void Thread::Create(Object* peer, size_t stack_size) {
  CHECK(peer != NULL);

  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

  Thread* native_thread = new Thread;
  native_thread->peer_ = peer;

  // Thread.start is synchronized, so we know that vmData is 0,
  // and know that we're not racing to assign it.
  SetVmData(peer, native_thread);

  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED), "PTHREAD_CREATE_DETACHED");
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
  CHECK_PTHREAD_CALL(pthread_create, (&native_thread->pthread_, &attr, Thread::CreateCallback, native_thread), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");

  // Let the child know when it's safe to start running.
  Runtime::Current()->GetThreadList()->SignalGo(native_thread);
}

void Thread::Attach(const Runtime* runtime) {
  InitCpu();
  InitFunctionPointers();

  thin_lock_id_ = Runtime::Current()->GetThreadList()->AllocThreadId();

  tid_ = ::art::GetTid();
  pthread_ = pthread_self();

  InitStackHwm();

  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach");

  jni_env_ = new JNIEnvExt(this, runtime->GetJavaVM());

  runtime->GetThreadList()->Register();
}

Thread* Thread::Attach(const Runtime* runtime, const char* name, bool as_daemon) {
  LOG(INFO) << "Thread::Attach '" << name << "'";
  Thread* self = new Thread;
  self->Attach(runtime);

  self->SetState(Thread::kNative);

  SetThreadName(name);

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  if (self->thin_lock_id_ != ThreadList::kMainId) {
    self->CreatePeer(name, as_daemon);
  }

  return self;
}

jobject GetWellKnownThreadGroup(JNIEnv* env, const char* field_name) {
  jclass thread_group_class = env->FindClass("java/lang/ThreadGroup");
  jfieldID fid = env->GetStaticFieldID(thread_group_class, field_name, "Ljava/lang/ThreadGroup;");
  jobject thread_group = env->GetStaticObjectField(thread_group_class, fid);
  // This will be null in the compiler (and tests), but never in a running system.
  //CHECK(thread_group != NULL) << "java.lang.ThreadGroup." << field_name << " not initialized";
  return thread_group;
}

void Thread::CreatePeer(const char* name, bool as_daemon) {
  JNIEnv* env = jni_env_;

  const char* field_name = (GetThinLockId() == ThreadList::kMainId) ? "mMain" : "mSystem";
  jobject thread_group = GetWellKnownThreadGroup(env, field_name);
  jobject thread_name = env->NewStringUTF(name);
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  jclass c = env->FindClass("java/lang/Thread");
  jmethodID mid = env->GetMethodID(c, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/String;IZ)V");

  jobject peer = env->NewObject(c, mid, thread_group, thread_name, thread_priority, thread_is_daemon);
  peer_ = DecodeJObject(peer);
  SetVmData(peer_, Thread::Current());

  // Because we mostly run without code available (in the compiler, in tests), we
  // manually assign the fields the constructor should have set.
  // TODO: lose this.
  gThread_daemon->SetBoolean(peer_, thread_is_daemon);
  gThread_group->SetObject(peer_, Decode<Object*>(env, thread_group));
  gThread_name->SetObject(peer_, Decode<Object*>(env, thread_name));
  gThread_priority->SetInt(peer_, thread_priority);
}

void Thread::InitStackHwm() {
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (pthread_, &attributes), __FUNCTION__);

  void* temp_stack_base;
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, &temp_stack_base, &stack_size_),
                     __FUNCTION__);
  stack_base_ = reinterpret_cast<byte*>(temp_stack_base);

  if (stack_size_ <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "attempt to attach a thread with a too-small stack (" << stack_size_ << " bytes)";
  }

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  ResetDefaultStackEnd();

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, (void*) stack_end_);

  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
}

void Thread::Dump(std::ostream& os) const {
  DumpState(os);
  DumpStack(os);
}

std::string GetSchedulerGroup(pid_t tid) {
  // /proc/<pid>/group looks like this:
  // 2:devices:/
  // 1:cpuacct,cpu:/
  // We want the third field from the line whose second field contains the "cpu" token.
  std::string cgroup_file;
  if (!ReadFileToString("/proc/self/cgroup", &cgroup_file)) {
    return "";
  }
  std::vector<std::string> cgroup_lines;
  Split(cgroup_file, '\n', cgroup_lines);
  for (size_t i = 0; i < cgroup_lines.size(); ++i) {
    std::vector<std::string> cgroup_fields;
    Split(cgroup_lines[i], ':', cgroup_fields);
    std::vector<std::string> cgroups;
    Split(cgroup_fields[1], ',', cgroups);
    for (size_t i = 0; i < cgroups.size(); ++i) {
      if (cgroups[i] == "cpu") {
        return cgroup_fields[2].substr(1); // Skip the leading slash.
      }
    }
  }
  return "";
}

void Thread::DumpState(std::ostream& os) const {
  std::string thread_name("<native thread without managed peer>");
  std::string group_name;
  int priority;
  bool is_daemon = false;

  if (peer_ != NULL) {
    String* thread_name_string = reinterpret_cast<String*>(gThread_name->GetObject(peer_));
    thread_name = (thread_name_string != NULL) ? thread_name_string->ToModifiedUtf8() : "<null>";
    priority = gThread_priority->GetInt(peer_);
    is_daemon = gThread_daemon->GetBoolean(peer_);

    Object* thread_group = gThread_group->GetObject(peer_);
    if (thread_group != NULL) {
      String* group_name_string = reinterpret_cast<String*>(gThreadGroup_name->GetObject(thread_group));
      group_name = (group_name_string != NULL) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    // This name may be truncated, but it's the best we can do in the absence of a managed peer.
    std::string stats;
    if (ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
      size_t start = stats.find('(') + 1;
      size_t end = stats.find(')') - start;
      thread_name = stats.substr(start, end);
    }
    priority = GetNativePriority();
  }

  int policy;
  sched_param sp;
  CHECK_PTHREAD_CALL(pthread_getschedparam, (pthread_, &policy, &sp), __FUNCTION__);

  std::string scheduler_group(GetSchedulerGroup(GetTid()));
  if (scheduler_group.empty()) {
    scheduler_group = "default";
  }

  os << '"' << thread_name << '"';
  if (is_daemon) {
    os << " daemon";
  }
  os << " prio=" << priority
     << " tid=" << GetThinLockId()
     << " " << GetState() << "\n";

  int debug_suspend_count = 0; // TODO
  os << "  | group=\"" << group_name << "\""
     << " sCount=" << suspend_count_
     << " dsCount=" << debug_suspend_count
     << " obj=" << reinterpret_cast<void*>(peer_)
     << " self=" << reinterpret_cast<const void*>(this) << "\n";
  os << "  | sysTid=" << GetTid()
     << " nice=" << getpriority(PRIO_PROCESS, GetTid())
     << " sched=" << policy << "/" << sp.sched_priority
     << " cgrp=" << scheduler_group
     << " handle=" << GetImpl() << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", GetTid()).c_str(), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1); // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  std::string stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
    // Skip the command, which may contain spaces.
    stats = stats.substr(stats.find(')') + 2);
    // Extract the three fields we care about.
    std::vector<std::string> fields;
    Split(stats, ' ', fields);
    utime = strtoull(fields[11].c_str(), NULL, 10);
    stime = strtoull(fields[12].c_str(), NULL, 10);
    task_cpu = strtoull(fields[36].c_str(), NULL, 10);
  }

  os << "  | schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
}

struct StackDumpVisitor : public Thread::StackVisitor {
  StackDumpVisitor(std::ostream& os) : os(os) {
  }

  virtual ~StackDumpVisitor() {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    if (!frame.HasMethod()) {
      return;
    }
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

    Method* m = frame.GetMethod();
    Class* c = m->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(c->GetDexCache());

    os << "  at " << PrettyMethod(m, false);
    if (m->IsNative()) {
      os << "(Native method)";
    } else {
      int line_number = dex_file.GetLineNumFromPC(m, m->ToDexPC(pc));
      os << "(" << c->GetSourceFile()->ToModifiedUtf8() << ":" << line_number << ")";
    }
    os << "\n";
  }

  std::ostream& os;
};

void Thread::DumpStack(std::ostream& os) const {
  StackDumpVisitor dumper(os);
  WalkStack(&dumper);
}

Thread::State Thread::SetState(Thread::State new_state) {
  Thread::State old_state = state_;
  if (old_state == new_state) {
    return old_state;
  }

  volatile void* raw = reinterpret_cast<volatile void*>(&state_);
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw);

  if (new_state == Thread::kRunnable) {
    /*
     * Change our status to Thread::kRunnable.  The transition requires
     * that we check for pending suspension, because the VM considers
     * us to be "asleep" in all other states, and another thread could
     * be performing a GC now.
     *
     * The order of operations is very significant here.  One way to
     * do this wrong is:
     *
     *   GCing thread                   Our thread (in kNative)
     *   ------------                   ----------------------
     *                                  check suspend count (== 0)
     *   SuspendAllThreads()
     *   grab suspend-count lock
     *   increment all suspend counts
     *   release suspend-count lock
     *   check thread state (== kNative)
     *   all are suspended, begin GC
     *                                  set state to kRunnable
     *                                  (continue executing)
     *
     * We can correct this by grabbing the suspend-count lock and
     * performing both of our operations (check suspend count, set
     * state) while holding it, now we need to grab a mutex on every
     * transition to kRunnable.
     *
     * What we do instead is change the order of operations so that
     * the transition to kRunnable happens first.  If we then detect
     * that the suspend count is nonzero, we switch to kSuspended.
     *
     * Appropriate compiler and memory barriers are required to ensure
     * that the operations are observed in the expected order.
     *
     * This does create a small window of opportunity where a GC in
     * progress could observe what appears to be a running thread (if
     * it happens to look between when we set to kRunnable and when we
     * switch to kSuspended).  At worst this only affects assertions
     * and thread logging.  (We could work around it with some sort
     * of intermediate "pre-running" state that is generally treated
     * as equivalent to running, but that doesn't seem worthwhile.)
     *
     * We can also solve this by combining the "status" and "suspend
     * count" fields into a single 32-bit value.  This trades the
     * store/load barrier on transition to kRunnable for an atomic RMW
     * op on all transitions and all suspend count updates (also, all
     * accesses to status or the thread count require bit-fiddling).
     * It also eliminates the brief transition through kRunnable when
     * the thread is supposed to be suspended.  This is possibly faster
     * on SMP and slightly more correct, but less convenient.
     */
    android_atomic_acquire_store(new_state, addr);
    if (ANNOTATE_UNPROTECTED_READ(suspend_count_) != 0) {
      Runtime::Current()->GetThreadList()->FullSuspendCheck(this);
    }
  } else {
    /*
     * Not changing to Thread::kRunnable. No additional work required.
     *
     * We use a releasing store to ensure that, if we were runnable,
     * any updates we previously made to objects on the managed heap
     * will be observed before the state change.
     */
    android_atomic_release_store(new_state, addr);
  }

  return old_state;
}

void Thread::WaitUntilSuspended() {
  // TODO: dalvik dropped the waiting thread's priority after a while.
  // TODO: dalvik timed out and aborted.
  useconds_t delay = 0;
  while (GetState() == Thread::kRunnable) {
    useconds_t new_delay = delay * 2;
    CHECK_GE(new_delay, delay);
    delay = new_delay;
    if (delay == 0) {
      sched_yield();
      delay = 10000;
    } else {
      usleep(delay);
    }
  }
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
}

void Thread::Startup() {
  // Allocate a TLS slot.
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback), "self key");

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(FATAL) << "newly-created pthread TLS slot is not NULL";
  }
}

void Thread::FinishStartup() {
  // Now the ClassLinker is ready, we can find the various Class*, Field*, and Method*s we need.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* boolean_class = class_linker->FindPrimitiveClass('Z');
  Class* int_class = class_linker->FindPrimitiveClass('I');
  Class* String_class = class_linker->FindSystemClass("Ljava/lang/String;");
  Class* Thread_class = class_linker->FindSystemClass("Ljava/lang/Thread;");
  Class* ThreadGroup_class = class_linker->FindSystemClass("Ljava/lang/ThreadGroup;");
  Class* ThreadLock_class = class_linker->FindSystemClass("Ljava/lang/ThreadLock;");
  Class* UncaughtExceptionHandler_class = class_linker->FindSystemClass("Ljava/lang/Thread$UncaughtExceptionHandler;");
  gThrowable = class_linker->FindSystemClass("Ljava/lang/Throwable;");
  gThread_daemon = Thread_class->FindDeclaredInstanceField("daemon", boolean_class);
  gThread_group = Thread_class->FindDeclaredInstanceField("group", ThreadGroup_class);
  gThread_lock = Thread_class->FindDeclaredInstanceField("lock", ThreadLock_class);
  gThread_name = Thread_class->FindDeclaredInstanceField("name", String_class);
  gThread_priority = Thread_class->FindDeclaredInstanceField("priority", int_class);
  gThread_run = Thread_class->FindVirtualMethod("run", "()V");
  gThread_uncaughtHandler = Thread_class->FindDeclaredInstanceField("uncaughtHandler", UncaughtExceptionHandler_class);
  gThread_vmData = Thread_class->FindDeclaredInstanceField("vmData", int_class);
  gThreadGroup_name = ThreadGroup_class->FindDeclaredInstanceField("name", String_class);
  gThreadGroup_removeThread = ThreadGroup_class->FindVirtualMethod("removeThread", "(Ljava/lang/Thread;)V");
  gUncaughtExceptionHandler_uncaughtException =
      UncaughtExceptionHandler_class->FindVirtualMethod("uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V");

  // Finish attaching the main thread.
  Thread::Current()->CreatePeer("main", false);
}

void Thread::Shutdown() {
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
}

Thread::Thread()
    : peer_(NULL),
      wait_mutex_(new Mutex("Thread wait mutex")),
      wait_cond_(new ConditionVariable("Thread wait condition variable")),
      wait_monitor_(NULL),
      interrupted_(false),
      wait_next_(NULL),
      card_table_(0),
      stack_end_(NULL),
      top_of_managed_stack_(),
      top_of_managed_stack_pc_(0),
      native_to_managed_record_(NULL),
      top_sirt_(NULL),
      jni_env_(NULL),
      state_(Thread::kUnknown),
      self_(NULL),
      runtime_(NULL),
      exception_(NULL),
      suspend_count_(0),
      class_loader_override_(NULL),
      long_jump_context_(NULL) {
}

void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  entered_monitor->MonitorExit(Thread::Current());
}

Thread::~Thread() {
  SetState(Thread::kRunnable);

  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);
  }

  if (peer_ != NULL) {
    Object* group = gThread_group->GetObject(peer_);

    // Handle any pending exception.
    if (IsExceptionPending()) {
      // Get and clear the exception.
      Object* exception = GetException();
      ClearException();

      // If the thread has its own handler, use that.
      Object* handler = gThread_uncaughtHandler->GetObject(peer_);
      if (handler == NULL) {
        // Otherwise use the thread group's default handler.
        handler = group;
      }

      // Call the handler.
      Method* m = handler->GetClass()->FindVirtualMethodForVirtualOrInterface(gUncaughtExceptionHandler_uncaughtException);
      Object* args[2];
      args[0] = peer_;
      args[1] = exception;
      m->Invoke(this, handler, reinterpret_cast<byte*>(&args), NULL);

      // If the handler threw, clear that exception too.
      ClearException();
    }

    // this.group.removeThread(this);
    // group can be null if we're in the compiler or a test.
    if (group != NULL) {
      Method* m = group->GetClass()->FindVirtualMethodForVirtualOrInterface(gThreadGroup_removeThread);
      Object* args = peer_;
      m->Invoke(this, group, reinterpret_cast<byte*>(&args), NULL);
    }

    // this.vmData = 0;
    SetVmData(peer_, NULL);

    // TODO: say "bye" to the debugger.
    //if (gDvm.debuggerConnected) {
    //  dvmDbgPostThreadDeath(self);
    //}

    // Thread.join() is implemented as an Object.wait() on the Thread.lock
    // object. Signal anyone who is waiting.
    Thread* self = Thread::Current();
    Object* lock = gThread_lock->GetObject(peer_);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != NULL) {
      lock->MonitorEnter(self);
      lock->NotifyAll();
      lock->MonitorExit(self);
    }
  }

  delete jni_env_;
  jni_env_ = NULL;

  SetState(Thread::kTerminated);

  delete wait_cond_;
  delete wait_mutex_;

  delete long_jump_context_;
}

size_t Thread::NumSirtReferences() {
  size_t count = 0;
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->Link()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::SirtContains(jobject obj) {
  Object** sirt_entry = reinterpret_cast<Object**>(obj);
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->Link()) {
    size_t num_refs = cur->NumberOfReferences();
    // A SIRT should always have a jobject/jclass as a native method is passed
    // in a this pointer or a class
    DCHECK_GT(num_refs, 0u);
    if ((&cur->References()[0] <= sirt_entry) &&
        (sirt_entry <= (&cur->References()[num_refs - 1]))) {
      return true;
    }
  }
  return false;
}

void Thread::PopSirt() {
  CHECK(top_sirt_ != NULL);
  top_sirt_ = top_sirt_->Link();
}

Object* Thread::DecodeJObject(jobject obj) {
  DCHECK(CanAccessDirectReferences());
  if (obj == NULL) {
    return NULL;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  Object* result;
  switch (kind) {
  case kLocal:
    {
      IndirectReferenceTable& locals = jni_env_->locals;
      result = const_cast<Object*>(locals.Get(ref));
      break;
    }
  case kGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& globals = vm->globals;
      MutexLock mu(vm->globals_lock);
      result = const_cast<Object*>(globals.Get(ref));
      break;
    }
  case kWeakGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& weak_globals = vm->weak_globals;
      MutexLock mu(vm->weak_globals_lock);
      result = const_cast<Object*>(weak_globals.Get(ref));
      if (result == kClearedJniWeakGlobal) {
        // This is a special case where it's okay to return NULL.
        return NULL;
      }
      break;
    }
  case kSirtOrInvalid:
  default:
    // TODO: make stack indirect reference table lookup more efficient
    // Check if this is a local reference in the SIRT
    if (SirtContains(obj)) {
      result = *reinterpret_cast<Object**>(obj);  // Read from SIRT
    } else if (jni_env_->work_around_app_jni_bugs) {
      // Assume an invalid local reference is actually a direct pointer.
      result = reinterpret_cast<Object*>(obj);
    } else {
      result = kInvalidIndirectRefObject;
    }
  }

  if (result == NULL) {
    LOG(ERROR) << "JNI ERROR (app bug): use of deleted " << kind << ": " << obj;
    JniAbort(NULL);
  } else {
    if (result != kInvalidIndirectRefObject) {
      Heap::VerifyObject(result);
    }
  }
  return result;
}

class CountStackDepthVisitor : public Thread::StackVisitor {
 public:
  CountStackDepthVisitor() : depth_(0), skip_depth_(0), skipping_(true) {}

  virtual void VisitFrame(const Frame& frame, uintptr_t pc) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    DCHECK(gThrowable != NULL);
    if (skipping_ && frame.HasMethod() && !gThrowable->IsAssignableFrom(frame.GetMethod()->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      ++depth_;
    } else {
      ++skip_depth_;
    }
  }

  int GetDepth() const {
    return depth_;
  }

  int GetSkipDepth() const {
    return skip_depth_;
  }

 private:
  uint32_t depth_;
  uint32_t skip_depth_;
  bool skipping_;
};

class BuildInternalStackTraceVisitor : public Thread::StackVisitor {
 public:
  explicit BuildInternalStackTraceVisitor(int depth, int skip_depth, ScopedJniThreadState& ts)
      : skip_depth_(skip_depth), count_(0) {
    // Allocate method trace with an extra slot that will hold the PC trace
    method_trace_ = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(depth + 1);
    // Register a local reference as IntArray::Alloc may trigger GC
    local_ref_ = AddLocalReference<jobject>(ts.Env(), method_trace_);
    pc_trace_ = IntArray::Alloc(depth);
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    method_trace = Decode<ObjectArray<Object>*>(ts.Env(), local_ref_);
#endif
    // Save PC trace in last element of method trace, also places it into the
    // object graph.
    method_trace_->Set(depth, pc_trace_);
  }

  virtual ~BuildInternalStackTraceVisitor() {}

  virtual void VisitFrame(const Frame& frame, uintptr_t pc) {
    if (skip_depth_ > 0) {
      skip_depth_--;
      return;
    }
    method_trace_->Set(count_, frame.GetMethod());
    pc_trace_->Set(count_, pc);
    ++count_;
  }

  jobject GetInternalStackTrace() const {
    return local_ref_;
  }

 private:
  // How many more frames to skip.
  int32_t skip_depth_;
  // Current position down stack trace
  uint32_t count_;
  // Array of return PC values
  IntArray* pc_trace_;
  // An array of the methods on the stack, the last entry is a reference to the
  // PC trace
  ObjectArray<Object>* method_trace_;
  // Local indirect reference table entry for method trace
  jobject local_ref_;
};

void Thread::WalkStack(StackVisitor* visitor) const {
  Frame frame = GetTopOfStack();
  uintptr_t pc = top_of_managed_stack_pc_;
  // TODO: enable this CHECK after native_to_managed_record_ is initialized during startup.
  // CHECK(native_to_managed_record_ != NULL);
  NativeToManagedRecord* record = native_to_managed_record_;

  while (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      DCHECK(frame.GetMethod()->IsWithinCode(pc));
      visitor->VisitFrame(frame, pc);
      pc = frame.GetReturnPC();
    }
    if (record == NULL) {
      break;
    }
    // last_tos should return Frame instead of sp?
    frame.SetSP(reinterpret_cast<Method**>(record->last_top_of_managed_stack_));
    pc = record->last_top_of_managed_stack_pc_;
    record = record->link_;
  }
}

void Thread::WalkStackUntilUpCall(StackVisitor* visitor, bool include_upcall) const {
  Frame frame = GetTopOfStack();
  uintptr_t pc = top_of_managed_stack_pc_;

  if (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      DCHECK(frame.GetMethod()->IsWithinCode(pc));
      visitor->VisitFrame(frame, pc);
      pc = frame.GetReturnPC();
    }
    if (include_upcall) {
      visitor->VisitFrame(frame, pc);
    }
  }
}

jobject Thread::CreateInternalStackTrace(JNIEnv* env) const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor;
  WalkStack(&count_visitor);
  int32_t depth = count_visitor.GetDepth();
  int32_t skip_depth = count_visitor.GetSkipDepth();

  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);

  // Build internal stack trace
  BuildInternalStackTraceVisitor build_trace_visitor(depth, skip_depth, ts);
  WalkStack(&build_trace_visitor);

  return build_trace_visitor.GetInternalStackTrace();
}

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
    jobjectArray output_array, int* stack_depth) {
  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);

  // Decode the internal stack trace into the depth, method trace and PC trace
  ObjectArray<Object>* method_trace =
      down_cast<ObjectArray<Object>*>(Decode<Object*>(ts.Env(), internal));
  int32_t depth = method_trace->GetLength()-1;
  IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  jobjectArray result;
  ObjectArray<StackTraceElement>* java_traces;
  if (output_array != NULL) {
    // Reuse the array we were given.
    result = output_array;
    java_traces = reinterpret_cast<ObjectArray<StackTraceElement>*>(Decode<Array*>(env,
        output_array));
    // ...adjusting the number of frames we'll write to not exceed the array length.
    depth = std::min(depth, java_traces->GetLength());
  } else {
    // Create java_trace array and place in local reference table
    java_traces = class_linker->AllocStackTraceElementArray(depth);
    result = AddLocalReference<jobjectArray>(ts.Env(), java_traces);
  }

  if (stack_depth != NULL) {
    *stack_depth = depth;
  }

  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    Method* method = down_cast<Method*>(method_trace->Get(i));
    uint32_t native_pc = pc_trace->Get(i);
    Class* klass = method->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(klass->GetDexCache());
    std::string class_name(PrettyDescriptor(klass->GetDescriptor()));

    // Allocate element, potentially triggering GC
    StackTraceElement* obj =
        StackTraceElement::Alloc(String::AllocFromModifiedUtf8(class_name.c_str()),
                                 method->GetName(),
                                 klass->GetSourceFile(),
                                 dex_file.GetLineNumFromPC(method,
                                     method->ToDexPC(native_pc)));
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    java_traces = Decode<ObjectArray<Object>*>(ts.Env(), result);
    method_trace = down_cast<ObjectArray<Object>*>(Decode<Object*>(ts.Env(), internal));
    pc_trace = down_cast<IntArray*>(method_trace->Get(depth));
#endif
    java_traces->Set(i, obj);
  }
  return result;
}

void Thread::ThrowNewException(const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(exception_class_descriptor, fmt, args);
  va_end(args);
}

void Thread::ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);

  // Convert "Ljava/lang/Exception;" into JNI-style "java/lang/Exception".
  CHECK_EQ('L', exception_class_descriptor[0]);
  std::string descriptor(exception_class_descriptor + 1);
  CHECK_EQ(';', descriptor[descriptor.length() - 1]);
  descriptor.erase(descriptor.length() - 1);

  JNIEnv* env = GetJniEnv();
  jclass exception_class = env->FindClass(descriptor.c_str());
  CHECK(exception_class != NULL) << "descriptor=\"" << descriptor << "\"";
  int rc = env->ThrowNew(exception_class, msg.c_str());
  CHECK_EQ(rc, JNI_OK);
  env->DeleteLocalRef(exception_class);
}

void Thread::ThrowOutOfMemoryError() {
  UNIMPLEMENTED(FATAL);
}

class CatchBlockStackVisitor : public Thread::StackVisitor {
 public:
  CatchBlockStackVisitor(Class* to_find, Context* ljc)
      : found_(false), to_find_(to_find), long_jump_context_(ljc), native_method_count_(0) {
#ifndef NDEBUG
    handler_pc_ = 0xEBADC0DE;
    handler_frame_.SetSP(reinterpret_cast<Method**>(0xEBADF00D));
#endif
  }

  virtual void VisitFrame(const Frame& fr, uintptr_t pc) {
    if (!found_) {
      Method* method = fr.GetMethod();
      if (method == NULL) {
        // This is the upcall, we remember the frame and last_pc so that we may
        // long jump to them
        handler_pc_ = pc;
        handler_frame_ = fr;
        return;
      }
      uint32_t dex_pc = DexFile::kDexNoIndex;
      if (method->IsPhony()) {
        // ignore callee save method
      } else if (method->IsNative()) {
        native_method_count_++;
      } else {
        // Move the PC back 2 bytes as a call will frequently terminate the
        // decoding of a particular instruction and we want to make sure we
        // get the Dex PC of the instruction with the call and not the
        // instruction following.
        pc -= 2;
        dex_pc = method->ToDexPC(pc);
      }
      if (dex_pc != DexFile::kDexNoIndex) {
        uint32_t found_dex_pc = method->FindCatchBlock(to_find_, dex_pc);
        if (found_dex_pc != DexFile::kDexNoIndex) {
          found_ = true;
          handler_pc_ = method->ToNativePC(found_dex_pc);
          handler_frame_ = fr;
        }
      }
      if (!found_) {
        // Caller may be handler, fill in callee saves in context
        long_jump_context_->FillCalleeSaves(fr);
      }
    }
  }

  // Did we find a catch block yet?
  bool found_;
  // The type of the exception catch block to find
  Class* to_find_;
  // Frame with found handler or last frame if no handler found
  Frame handler_frame_;
  // PC to branch to for the handler
  uintptr_t handler_pc_;
  // Context that will be the target of the long jump
  Context* long_jump_context_;
  // Number of native methods passed in crawl (equates to number of SIRTs to pop)
  uint32_t native_method_count_;
};

void Thread::DeliverException() {
  Throwable *exception = GetException();  // Set exception on thread
  CHECK(exception != NULL);

  Context* long_jump_context = GetLongJumpContext();
  CatchBlockStackVisitor catch_finder(exception->GetClass(), long_jump_context);
  WalkStackUntilUpCall(&catch_finder, true);

  // Pop any SIRT
  if (catch_finder.native_method_count_ == 1) {
    PopSirt();
  } else {
    // We only expect the stack crawl to have passed 1 native method as it's terminated
    // by an up call
    DCHECK_EQ(catch_finder.native_method_count_, 0u);
  }
  long_jump_context->SetSP(reinterpret_cast<intptr_t>(catch_finder.handler_frame_.GetSP()));
  long_jump_context->SetPC(catch_finder.handler_pc_);
  long_jump_context->DoLongJump();
}

Context* Thread::GetLongJumpContext() {
  Context* result = long_jump_context_;
  if (result == NULL) {
    result = Context::Create();
    long_jump_context_ = result;
  }
  return result;
}

bool Thread::HoldsLock(Object* object) {
  if (object == NULL) {
    return false;
  }
  return object->GetLockOwner() == thin_lock_id_;
}

bool Thread::IsDaemon() {
  return gThread_daemon->GetBoolean(peer_);
}

void Thread::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  if (exception_ != NULL) {
    visitor(exception_, arg);
  }
  if (peer_ != NULL) {
    visitor(peer_, arg);
  }
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);
  // visitThreadStack(visitor, thread, arg);
  UNIMPLEMENTED(WARNING) << "some per-Thread roots not visited";
}

static const char* kStateNames[] = {
  "Terminated",
  "Runnable",
  "TimedWaiting",
  "Blocked",
  "Waiting",
  "Initializing",
  "Starting",
  "Native",
  "VmWait",
  "Suspended",
};
std::ostream& operator<<(std::ostream& os, const Thread::State& state) {
  int int_state = static_cast<int>(state);
  if (state >= Thread::kTerminated && state <= Thread::kSuspended) {
    os << kStateNames[int_state];
  } else {
    os << "State[" << int_state << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  os << "Thread[" << &thread
     << ",pthread_t=" << thread.GetImpl()
     << ",tid=" << thread.GetTid()
     << ",id=" << thread.GetThinLockId()
     << ",state=" << thread.GetState()
     << ",peer=" << thread.GetPeer()
     << "]";
  return os;
}

}  // namespace art
