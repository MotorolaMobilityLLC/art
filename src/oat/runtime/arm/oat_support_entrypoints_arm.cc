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

#include "oat/runtime/oat_support_entrypoints.h"

namespace art {

// Alloc entrypoints.
extern "C" void* art_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);
extern "C" void* art_alloc_object_from_code(uint32_t type_idx, void* method);
extern "C" void* art_alloc_object_from_code_with_access_check(uint32_t type_idx, void* method);
extern "C" void* art_check_and_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_check_and_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);

// Cast entrypoints.
extern uint32_t IsAssignableFromCode(const Class* klass, const Class* ref_class);
extern "C" void art_can_put_array_element_from_code(void*, void*);
extern "C" void art_check_cast_from_code(void*, void*);

// Debug entrypoints.
extern void DebugMe(Method* method, uint32_t info);
extern "C" void art_update_debugger(void*, void*, int32_t, void*);

// DexCache entrypoints.
extern "C" void* art_initialize_static_storage_from_code(uint32_t, void*);
extern "C" void* art_initialize_type_from_code(uint32_t, void*);
extern "C" void* art_initialize_type_and_verify_access_from_code(uint32_t, void*);
extern "C" void* art_resolve_string_from_code(void*, uint32_t);

// Field entrypoints.
extern "C" int art_set32_instance_from_code(uint32_t, void*, int32_t);
extern "C" int art_set32_static_from_code(uint32_t, int32_t);
extern "C" int art_set64_instance_from_code(uint32_t, void*, int64_t);
extern "C" int art_set64_static_from_code(uint32_t, int64_t);
extern "C" int art_set_obj_instance_from_code(uint32_t, void*, void*);
extern "C" int art_set_obj_static_from_code(uint32_t, void*);
extern "C" int32_t art_get32_instance_from_code(uint32_t, void*);
extern "C" int32_t art_get32_static_from_code(uint32_t);
extern "C" int64_t art_get64_instance_from_code(uint32_t, void*);
extern "C" int64_t art_get64_static_from_code(uint32_t);
extern "C" void* art_get_obj_instance_from_code(uint32_t, void*);
extern "C" void* art_get_obj_static_from_code(uint32_t);

// FillArray entrypoint.
extern "C" void art_handle_fill_data_from_code(void*, void*);

// JNI entrypoints.
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj);
extern void* FindNativeMethod(Thread* thread);

// Lock entrypoints.
extern "C" void art_lock_object_from_code(void*);
extern "C" void art_unlock_object_from_code(void*);

// Math entrypoints.
extern int32_t CmpgDouble(double a, double b);
extern int32_t CmplDouble(double a, double b);
extern int32_t CmpgFloat(float a, float b);
extern int32_t CmplFloat(float a, float b);

// Math conversions.
extern "C" float __aeabi_i2f(int32_t op1);         // INT_TO_FLOAT
extern "C" int32_t __aeabi_f2iz(float op1);        // FLOAT_TO_INT
extern "C" float __aeabi_d2f(double op1);          // DOUBLE_TO_FLOAT
extern "C" double __aeabi_f2d(float op1);          // FLOAT_TO_DOUBLE
extern "C" double __aeabi_i2d(int32_t op1);        // INT_TO_DOUBLE
extern "C" int32_t __aeabi_d2iz(double op1);       // DOUBLE_TO_INT
extern "C" float __aeabi_l2f(int64_t op1);         // LONG_TO_FLOAT
extern "C" double __aeabi_l2d(int64_t op1);        // LONG_TO_DOUBLE
extern int64_t D2L(double d);
extern int64_t F2L(float f);

// Single-precision FP arithmetics.
extern "C" float __aeabi_fadd(float a, float b);   // ADD_FLOAT[_2ADDR]
extern "C" float __aeabi_fsub(float a, float b);   // SUB_FLOAT[_2ADDR]
extern "C" float __aeabi_fdiv(float a, float b);   // DIV_FLOAT[_2ADDR]
extern "C" float __aeabi_fmul(float a, float b);   // MUL_FLOAT[_2ADDR]
extern "C" float fmodf(float a, float b);          // REM_FLOAT[_2ADDR]

// Double-precision FP arithmetics.
extern "C" double __aeabi_dadd(double a, double b); // ADD_DOUBLE[_2ADDR]
extern "C" double __aeabi_dsub(double a, double b); // SUB_DOUBLE[_2ADDR]
extern "C" double __aeabi_ddiv(double a, double b); // DIV_DOUBLE[_2ADDR]
extern "C" double __aeabi_dmul(double a, double b); // MUL_DOUBLE[_2ADDR]
extern "C" double fmod(double a, double b);         // REM_DOUBLE[_2ADDR]

// Integer arithmetics.
extern "C" int __aeabi_idivmod(int32_t op1, int32_t op2);  // REM_INT[_2ADDR|_LIT8|_LIT16]
extern "C" int __aeabi_idiv(int32_t op1, int32_t op2);     // DIV_INT[_2ADDR|_LIT8|_LIT16]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" long long __aeabi_ldivmod(long long op1, long long op2);
extern "C" long long __aeabi_lmul(long long op1, long long op2);
extern "C" uint64_t art_shl_long(uint64_t, uint32_t);
extern "C" uint64_t art_shr_long(uint64_t, uint32_t);
extern "C" uint64_t art_ushr_long(uint64_t, uint32_t);

// Intrinsic entrypoints.
extern "C" int32_t __memcmp16(void*, void*, int32_t);
extern "C" int32_t art_indexof(void*, uint32_t, uint32_t, uint32_t);
extern "C" int32_t art_string_compareto(void*, void*);

// Invoke entrypoints.
const void* UnresolvedDirectMethodTrampolineFromCode(Method*, Method**, Thread*,
                                                     Runtime::TrampolineType);
extern "C" void art_invoke_direct_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_interface_trampoline(uint32_t, void*);
extern "C" void art_invoke_interface_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_static_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_super_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_virtual_trampoline_with_access_check(uint32_t, void*);

// Thread entrypoints.
extern void CheckSuspendFromCode(Thread* thread);
extern "C" void art_test_suspend();

// Throw entrypoints.
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp);
extern "C" void art_deliver_exception_from_code(void*);
extern "C" void art_throw_array_bounds_from_code(int32_t index, int32_t limit);
extern "C" void art_throw_div_zero_from_code();
extern "C" void art_throw_no_such_method_from_code(int32_t method_idx);
extern "C" void art_throw_null_pointer_exception_from_code();
extern "C" void art_throw_stack_overflow_from_code(void*);
extern "C" void art_throw_verification_error_from_code(int32_t src1, int32_t ref);

// Trace entrypoints.
extern "C" void art_trace_entry_from_code(void*);
extern "C" void art_trace_exit_from_code();

void InitEntryPoints(EntryPoints* points) {
  // Alloc
  points->pAllocArrayFromCode = art_alloc_array_from_code;
  points->pAllocArrayFromCodeWithAccessCheck = art_alloc_array_from_code_with_access_check;
  points->pAllocObjectFromCode = art_alloc_object_from_code;
  points->pAllocObjectFromCodeWithAccessCheck = art_alloc_object_from_code_with_access_check;
  points->pCheckAndAllocArrayFromCode = art_check_and_alloc_array_from_code;
  points->pCheckAndAllocArrayFromCodeWithAccessCheck = art_check_and_alloc_array_from_code_with_access_check;

  // Cast
  points->pInstanceofNonTrivialFromCode = IsAssignableFromCode;
  points->pCanPutArrayElementFromCode = art_can_put_array_element_from_code;
  points->pCheckCastFromCode = art_check_cast_from_code;

  // Debug
  points->pDebugMe = DebugMe;
  points->pUpdateDebuggerFromCode = NULL; // Controlled by SetDebuggerUpdatesEnabled.

  // DexCache
  points->pInitializeStaticStorage = art_initialize_static_storage_from_code;
  points->pInitializeTypeAndVerifyAccessFromCode = art_initialize_type_and_verify_access_from_code;
  points->pInitializeTypeFromCode = art_initialize_type_from_code;
  points->pResolveStringFromCode = art_resolve_string_from_code;

  // Field
  points->pSet32Instance = art_set32_instance_from_code;
  points->pSet32Static = art_set32_static_from_code;
  points->pSet64Instance = art_set64_instance_from_code;
  points->pSet64Static = art_set64_static_from_code;
  points->pSetObjInstance = art_set_obj_instance_from_code;
  points->pSetObjStatic = art_set_obj_static_from_code;
  points->pGet32Instance = art_get32_instance_from_code;
  points->pGet64Instance = art_get64_instance_from_code;
  points->pGetObjInstance = art_get_obj_instance_from_code;
  points->pGet32Static = art_get32_static_from_code;
  points->pGet64Static = art_get64_static_from_code;
  points->pGetObjStatic = art_get_obj_static_from_code;

  // FillArray
  points->pHandleFillArrayDataFromCode = art_handle_fill_data_from_code;

  // JNI
  points->pDecodeJObjectInThread = DecodeJObjectInThread;
  points->pFindNativeMethod = FindNativeMethod;

  // Locks
  points->pLockObjectFromCode = art_lock_object_from_code;
  points->pUnlockObjectFromCode = art_unlock_object_from_code;

  // Math
  points->pCmpgDouble = CmpgDouble;
  points->pCmpgFloat = CmpgFloat;
  points->pCmplDouble = CmplDouble;
  points->pCmplFloat = CmplFloat;
  points->pDadd = __aeabi_dadd;
  points->pDdiv = __aeabi_ddiv;
  points->pDmul = __aeabi_dmul;
  points->pDsub = __aeabi_dsub;
  points->pF2d = __aeabi_f2d;
  points->pFmod = fmod;
  points->pI2d = __aeabi_i2d;
  points->pL2d = __aeabi_l2d;
  points->pD2f = __aeabi_d2f;
  points->pFadd = __aeabi_fadd;
  points->pFdiv = __aeabi_fdiv;
  points->pFmodf = fmodf;
  points->pFmul = __aeabi_fmul;
  points->pFsub = __aeabi_fsub;
  points->pI2f = __aeabi_i2f;
  points->pL2f = __aeabi_l2f;
  points->pD2iz = __aeabi_d2iz;
  points->pF2iz = __aeabi_f2iz;
  points->pIdiv = __aeabi_idivmod;
  points->pIdivmod = __aeabi_idivmod;
  points->pD2l = D2L;
  points->pF2l = F2L;
  points->pLadd = NULL;
  points->pLand = NULL;
  points->pLdivmod = __aeabi_ldivmod;
  points->pLmul = __aeabi_lmul;
  points->pLor = NULL;
  points->pLsub = NULL;
  points->pLxor = NULL;
  points->pShlLong = art_shl_long;
  points->pShrLong = art_shr_long;
  points->pUshrLong = art_ushr_long;

  // Intrinsics
  points->pIndexOf = art_indexof;
  points->pMemcmp16 = __memcmp16;
  points->pStringCompareTo = art_string_compareto;
  points->pMemcpy = memcpy;

  // Invocation
  points->pUnresolvedDirectMethodTrampolineFromCode = UnresolvedDirectMethodTrampolineFromCode;
  points->pInvokeDirectTrampolineWithAccessCheck = art_invoke_direct_trampoline_with_access_check;
  points->pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
  points->pInvokeInterfaceTrampolineWithAccessCheck = art_invoke_interface_trampoline_with_access_check;
  points->pInvokeStaticTrampolineWithAccessCheck = art_invoke_static_trampoline_with_access_check;
  points->pInvokeSuperTrampolineWithAccessCheck = art_invoke_super_trampoline_with_access_check;
  points->pInvokeVirtualTrampolineWithAccessCheck = art_invoke_virtual_trampoline_with_access_check;

  // Thread
  points->pCheckSuspendFromCode = CheckSuspendFromCode;
  points->pTestSuspendFromCode = art_test_suspend;

  // Throws
  points->pDeliverException = art_deliver_exception_from_code;
  points->pThrowAbstractMethodErrorFromCode = ThrowAbstractMethodErrorFromCode;
  points->pThrowArrayBoundsFromCode = art_throw_array_bounds_from_code;
  points->pThrowDivZeroFromCode = art_throw_div_zero_from_code;
  points->pThrowNoSuchMethodFromCode = art_throw_no_such_method_from_code;
  points->pThrowNullPointerFromCode = art_throw_null_pointer_exception_from_code;
  points->pThrowStackOverflowFromCode = art_throw_stack_overflow_from_code;
  points->pThrowVerificationErrorFromCode = art_throw_verification_error_from_code;
};

void ChangeDebuggerEntryPoint(EntryPoints* points, bool enabled) {
  points->pUpdateDebuggerFromCode = (enabled ? art_update_debugger : NULL);
}

bool IsTraceExitPc(uintptr_t pc) {
  uintptr_t trace_exit = reinterpret_cast<uintptr_t>(art_trace_exit_from_code);
  return pc == trace_exit;
}

void* GetLogTraceEntryPoint() {
  return reinterpret_cast<void*>(art_trace_entry_from_code);
}

}  // namespace art
