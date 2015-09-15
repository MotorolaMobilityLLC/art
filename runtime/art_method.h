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

#ifndef ART_RUNTIME_ART_METHOD_H_
#define ART_RUNTIME_ART_METHOD_H_

#include "base/casts.h"
#include "dex_file.h"
#include "gc_root.h"
#include "invoke_type.h"
#include "method_reference.h"
#include "modifiers.h"
#include "mirror/object.h"
#include "quick/quick_method_frame_info.h"
#include "read_barrier_option.h"
#include "stack.h"
#include "stack_map.h"
#include "utils.h"

namespace art {

union JValue;
class ProfilingInfo;
class ScopedObjectAccessAlreadyRunnable;
class StringPiece;
class ShadowFrame;

namespace mirror {
class Array;
class Class;
class PointerArray;
}  // namespace mirror

class ArtMethod FINAL {
 public:
  ArtMethod() : access_flags_(0), dex_code_item_offset_(0), dex_method_index_(0),
      method_index_(0) { }

  ArtMethod(const ArtMethod& src, size_t image_pointer_size) {
    CopyFrom(&src, image_pointer_size);
  }

  static ArtMethod* FromReflectedMethod(const ScopedObjectAccessAlreadyRunnable& soa,
                                        jobject jlr_method)
      SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE mirror::Class* GetDeclaringClass() SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE mirror::Class* GetDeclaringClassNoBarrier()
      SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE mirror::Class* GetDeclaringClassUnchecked()
      SHARED_REQUIRES(Locks::mutator_lock_);

  void SetDeclaringClass(mirror::Class *new_declaring_class)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool CASDeclaringClass(mirror::Class* expected_class, mirror::Class* desired_class)
      SHARED_REQUIRES(Locks::mutator_lock_);

  static MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, declaring_class_));
  }

  ALWAYS_INLINE uint32_t GetAccessFlags() SHARED_REQUIRES(Locks::mutator_lock_);

  void SetAccessFlags(uint32_t new_access_flags) {
    // Not called within a transaction.
    access_flags_ = new_access_flags;
  }

  // Approximate what kind of method call would be used for this method.
  InvokeType GetInvokeType() SHARED_REQUIRES(Locks::mutator_lock_);

  // Returns true if the method is declared public.
  bool IsPublic() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  // Returns true if the method is a constructor.
  bool IsConstructor() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccConstructor) != 0;
  }

  // Returns true if the method is a class initializer.
  bool IsClassInitializer() SHARED_REQUIRES(Locks::mutator_lock_) {
    return IsConstructor() && IsStatic();
  }

  // Returns true if the method is static, private, or a constructor.
  bool IsDirect() SHARED_REQUIRES(Locks::mutator_lock_) {
    return IsDirect(GetAccessFlags());
  }

  static bool IsDirect(uint32_t access_flags) {
    return (access_flags & (kAccStatic | kAccPrivate | kAccConstructor)) != 0;
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (GetAccessFlags() & synchonized) != 0;
  }

  bool IsFinal() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  bool IsMiranda() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccMiranda) != 0;
  }

  bool IsNative() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccNative) != 0;
  }

  bool IsFastNative() SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t mask = kAccFastNative | kAccNative;
    return (GetAccessFlags() & mask) == mask;
  }

  bool IsAbstract() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccAbstract) != 0;
  }

  bool IsSynthetic() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccSynthetic) != 0;
  }

  bool IsProxyMethod() SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsPreverified() SHARED_REQUIRES(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPreverified) != 0;
  }

  void SetPreverified() SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(!IsPreverified());
    SetAccessFlags(GetAccessFlags() | kAccPreverified);
  }

  bool IsOptimized(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_) {
    // Temporary solution for detecting if a method has been optimized: the compiler
    // does not create a GC map. Instead, the vmap table contains the stack map
    // (as in stack_map.h).
    return !IsNative()
        && GetEntryPointFromQuickCompiledCodePtrSize(pointer_size) != nullptr
        && GetQuickOatCodePointer(pointer_size) != nullptr
        && GetNativeGcMap(pointer_size) == nullptr;
  }

  bool CheckIncompatibleClassChange(InvokeType type) SHARED_REQUIRES(Locks::mutator_lock_);

  uint16_t GetMethodIndex() SHARED_REQUIRES(Locks::mutator_lock_);

  // Doesn't do erroneous / unresolved class checks.
  uint16_t GetMethodIndexDuringLinking() SHARED_REQUIRES(Locks::mutator_lock_);

  size_t GetVtableIndex() SHARED_REQUIRES(Locks::mutator_lock_) {
    return GetMethodIndex();
  }

  void SetMethodIndex(uint16_t new_method_index) SHARED_REQUIRES(Locks::mutator_lock_) {
    // Not called within a transaction.
    method_index_ = new_method_index;
  }

  static MemberOffset DexMethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_method_index_);
  }

  static MemberOffset MethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_);
  }

  uint32_t GetCodeItemOffset() {
    return dex_code_item_offset_;
  }

  void SetCodeItemOffset(uint32_t new_code_off) {
    // Not called within a transaction.
    dex_code_item_offset_ = new_code_off;
  }

  // Number of 32bit registers that would be required to hold all the arguments
  static size_t NumArgRegisters(const StringPiece& shorty);

  ALWAYS_INLINE uint32_t GetDexMethodIndex() SHARED_REQUIRES(Locks::mutator_lock_);

  void SetDexMethodIndex(uint32_t new_idx) {
    // Not called within a transaction.
    dex_method_index_ = new_idx;
  }

  ALWAYS_INLINE ArtMethod** GetDexCacheResolvedMethods(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  ALWAYS_INLINE ArtMethod* GetDexCacheResolvedMethod(uint16_t method_index, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  ALWAYS_INLINE void SetDexCacheResolvedMethod(uint16_t method_index,
                                               ArtMethod* new_method,
                                               size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  ALWAYS_INLINE void SetDexCacheResolvedMethods(ArtMethod** new_dex_cache_methods, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasDexCacheResolvedMethods(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasSameDexCacheResolvedMethods(ArtMethod* other, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasSameDexCacheResolvedMethods(ArtMethod** other_cache, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  template <bool kWithCheck = true>
  mirror::Class* GetDexCacheResolvedType(uint32_t type_idx, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void SetDexCacheResolvedTypes(GcRoot<mirror::Class>* new_dex_cache_types, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasDexCacheResolvedTypes(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasSameDexCacheResolvedTypes(ArtMethod* other, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool HasSameDexCacheResolvedTypes(GcRoot<mirror::Class>* other_cache, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get the Class* from the type index into this method's dex cache.
  mirror::Class* GetClassFromTypeIndex(uint16_t type_idx, bool resolve, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Find the method that this method overrides.
  ArtMethod* FindOverriddenMethod(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  // Find the method index for this method within other_dexfile. If this method isn't present then
  // return DexFile::kDexNoIndex. The name_and_signature_idx MUST refer to a MethodId with the same
  // name and signature in the other_dexfile, such as the method index used to resolve this method
  // in the other_dexfile.
  uint32_t FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                            uint32_t name_and_signature_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result, const char* shorty)
      SHARED_REQUIRES(Locks::mutator_lock_);

  const void* GetEntryPointFromQuickCompiledCode() {
    return GetEntryPointFromQuickCompiledCodePtrSize(sizeof(void*));
  }
  ALWAYS_INLINE const void* GetEntryPointFromQuickCompiledCodePtrSize(size_t pointer_size) {
    return GetNativePointer<const void*>(
        EntryPointFromQuickCompiledCodeOffset(pointer_size), pointer_size);
  }

  void SetEntryPointFromQuickCompiledCode(const void* entry_point_from_quick_compiled_code) {
    SetEntryPointFromQuickCompiledCodePtrSize(entry_point_from_quick_compiled_code,
                                              sizeof(void*));
  }
  ALWAYS_INLINE void SetEntryPointFromQuickCompiledCodePtrSize(
      const void* entry_point_from_quick_compiled_code, size_t pointer_size) {
    SetNativePointer(EntryPointFromQuickCompiledCodeOffset(pointer_size),
                     entry_point_from_quick_compiled_code, pointer_size);
  }

  uint32_t GetCodeSize() SHARED_REQUIRES(Locks::mutator_lock_);

  // Check whether the given PC is within the quick compiled code associated with this method's
  // quick entrypoint. This code isn't robust for instrumentation, etc. and is only used for
  // debug purposes.
  bool PcIsWithinQuickCode(uintptr_t pc) {
    return PcIsWithinQuickCode(
        reinterpret_cast<uintptr_t>(GetEntryPointFromQuickCompiledCode()), pc);
  }

  void AssertPcIsWithinQuickCode(uintptr_t pc) SHARED_REQUIRES(Locks::mutator_lock_);

  // Returns true if the entrypoint points to the interpreter, as
  // opposed to the compiled code, that is, this method will be
  // interpretered on invocation.
  bool IsEntrypointInterpreter() SHARED_REQUIRES(Locks::mutator_lock_);

  uint32_t GetQuickOatCodeOffset();
  void SetQuickOatCodeOffset(uint32_t code_offset);

  ALWAYS_INLINE static const void* EntryPointToCodePointer(const void* entry_point) {
    uintptr_t code = reinterpret_cast<uintptr_t>(entry_point);
    // TODO: Make this Thumb2 specific. It is benign on other architectures as code is always at
    //       least 2 byte aligned.
    code &= ~0x1;
    return reinterpret_cast<const void*>(code);
  }

  // Actual entry point pointer to compiled oat code or null.
  const void* GetQuickOatEntryPoint(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  // Actual pointer to compiled oat code or null.
  const void* GetQuickOatCodePointer(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return EntryPointToCodePointer(GetQuickOatEntryPoint(pointer_size));
  }

  // Callers should wrap the uint8_t* in a MappingTable instance for convenient access.
  const uint8_t* GetMappingTable(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  const uint8_t* GetMappingTable(const void* code_pointer, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Callers should wrap the uint8_t* in a VmapTable instance for convenient access.
  const uint8_t* GetVmapTable(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  const uint8_t* GetVmapTable(const void* code_pointer, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  const uint8_t* GetQuickenedInfo() SHARED_REQUIRES(Locks::mutator_lock_);

  CodeInfo GetOptimizedCodeInfo() SHARED_REQUIRES(Locks::mutator_lock_);

  // Callers should wrap the uint8_t* in a GcMap instance for convenient access.
  const uint8_t* GetNativeGcMap(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);
  const uint8_t* GetNativeGcMap(const void* code_pointer, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  template <bool kCheckFrameSize = true>
  uint32_t GetFrameSizeInBytes() SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t result = GetQuickFrameInfo().FrameSizeInBytes();
    if (kCheckFrameSize) {
      DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    }
    return result;
  }

  QuickMethodFrameInfo GetQuickFrameInfo() SHARED_REQUIRES(Locks::mutator_lock_);
  QuickMethodFrameInfo GetQuickFrameInfo(const void* code_pointer)
      SHARED_REQUIRES(Locks::mutator_lock_);

  FrameOffset GetReturnPcOffset() SHARED_REQUIRES(Locks::mutator_lock_) {
    return GetReturnPcOffset(GetFrameSizeInBytes());
  }

  FrameOffset GetReturnPcOffset(uint32_t frame_size_in_bytes)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK_EQ(frame_size_in_bytes, GetFrameSizeInBytes());
    return FrameOffset(frame_size_in_bytes - sizeof(void*));
  }

  FrameOffset GetHandleScopeOffset() SHARED_REQUIRES(Locks::mutator_lock_) {
    constexpr size_t handle_scope_offset = sizeof(ArtMethod*);
    DCHECK_LT(handle_scope_offset, GetFrameSizeInBytes());
    return FrameOffset(handle_scope_offset);
  }

  void RegisterNative(const void* native_method, bool is_fast)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void UnregisterNative() SHARED_REQUIRES(Locks::mutator_lock_);

  static MemberOffset DexCacheResolvedMethodsOffset(size_t pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, dex_cache_resolved_methods_) / sizeof(void*) * pointer_size);
  }

  static MemberOffset DexCacheResolvedTypesOffset(size_t pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, dex_cache_resolved_types_) / sizeof(void*) * pointer_size);
  }

  static MemberOffset EntryPointFromJniOffset(size_t pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, entry_point_from_jni_) / sizeof(void*) * pointer_size);
  }

  static MemberOffset EntryPointFromQuickCompiledCodeOffset(size_t pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, entry_point_from_quick_compiled_code_) / sizeof(void*) * pointer_size);
  }

  ProfilingInfo* CreateProfilingInfo() SHARED_REQUIRES(Locks::mutator_lock_);

  ProfilingInfo* GetProfilingInfo(size_t pointer_size) {
    return reinterpret_cast<ProfilingInfo*>(GetEntryPointFromJniPtrSize(pointer_size));
  }

  void* GetEntryPointFromJni() {
    return GetEntryPointFromJniPtrSize(sizeof(void*));
  }

  ALWAYS_INLINE void* GetEntryPointFromJniPtrSize(size_t pointer_size) {
    return GetNativePointer<void*>(EntryPointFromJniOffset(pointer_size), pointer_size);
  }

  void SetEntryPointFromJni(const void* entrypoint) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsNative());
    SetEntryPointFromJniPtrSize(entrypoint, sizeof(void*));
  }

  ALWAYS_INLINE void SetEntryPointFromJniPtrSize(const void* entrypoint, size_t pointer_size) {
    SetNativePointer(EntryPointFromJniOffset(pointer_size), entrypoint, pointer_size);
  }

  // Is this a CalleSaveMethod or ResolutionMethod and therefore doesn't adhere to normal
  // conventions for a method of managed code. Returns false for Proxy methods.
  ALWAYS_INLINE bool IsRuntimeMethod();

  // Is this a hand crafted method used for something like describing callee saves?
  bool IsCalleeSaveMethod() SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsResolutionMethod() SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsImtConflictMethod() SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsImtUnimplementedMethod() SHARED_REQUIRES(Locks::mutator_lock_);

  uintptr_t NativeQuickPcOffset(const uintptr_t pc) SHARED_REQUIRES(Locks::mutator_lock_);
#ifdef NDEBUG
  uintptr_t NativeQuickPcOffset(const uintptr_t pc, const void* quick_entry_point)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return pc - reinterpret_cast<uintptr_t>(quick_entry_point);
  }
#else
  uintptr_t NativeQuickPcOffset(const uintptr_t pc, const void* quick_entry_point)
      SHARED_REQUIRES(Locks::mutator_lock_);
#endif

  // Converts a native PC to a dex PC.
  uint32_t ToDexPc(const uintptr_t pc, bool abort_on_failure = true)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Converts a dex PC to a native PC.
  uintptr_t ToNativeQuickPc(const uint32_t dex_pc,
                            bool is_for_catch_handler,
                            bool abort_on_failure = true)
      SHARED_REQUIRES(Locks::mutator_lock_);

  MethodReference ToMethodReference() SHARED_REQUIRES(Locks::mutator_lock_) {
    return MethodReference(GetDexFile(), GetDexMethodIndex());
  }

  // Find the catch block for the given exception type and dex_pc. When a catch block is found,
  // indicates whether the found catch block is responsible for clearing the exception or whether
  // a move-exception instruction is present.
  uint32_t FindCatchBlock(Handle<mirror::Class> exception_type, uint32_t dex_pc,
                          bool* has_no_move_exception)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // NO_THREAD_SAFETY_ANALYSIS since we don't know what the callback requires.
  template<typename RootVisitorType>
  void VisitRoots(RootVisitorType& visitor, size_t pointer_size) NO_THREAD_SAFETY_ANALYSIS;

  const DexFile* GetDexFile() SHARED_REQUIRES(Locks::mutator_lock_);

  const char* GetDeclaringClassDescriptor() SHARED_REQUIRES(Locks::mutator_lock_);

  const char* GetShorty() SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t unused_length;
    return GetShorty(&unused_length);
  }

  const char* GetShorty(uint32_t* out_length) SHARED_REQUIRES(Locks::mutator_lock_);

  const Signature GetSignature() SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE const char* GetName() SHARED_REQUIRES(Locks::mutator_lock_);

  mirror::String* GetNameAsString(Thread* self) SHARED_REQUIRES(Locks::mutator_lock_);

  const DexFile::CodeItem* GetCodeItem() SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsResolvedTypeIdx(uint16_t type_idx, size_t ptr_size) SHARED_REQUIRES(Locks::mutator_lock_);

  int32_t GetLineNumFromDexPC(uint32_t dex_pc) SHARED_REQUIRES(Locks::mutator_lock_);

  const DexFile::ProtoId& GetPrototype() SHARED_REQUIRES(Locks::mutator_lock_);

  const DexFile::TypeList* GetParameterTypeList() SHARED_REQUIRES(Locks::mutator_lock_);

  const char* GetDeclaringClassSourceFile() SHARED_REQUIRES(Locks::mutator_lock_);

  uint16_t GetClassDefIndex() SHARED_REQUIRES(Locks::mutator_lock_);

  const DexFile::ClassDef& GetClassDef() SHARED_REQUIRES(Locks::mutator_lock_);

  const char* GetReturnTypeDescriptor() SHARED_REQUIRES(Locks::mutator_lock_);

  const char* GetTypeDescriptorFromTypeIdx(uint16_t type_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // May cause thread suspension due to GetClassFromTypeIdx calling ResolveType this caused a large
  // number of bugs at call sites.
  mirror::Class* GetReturnType(bool resolve, size_t ptr_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  mirror::ClassLoader* GetClassLoader() SHARED_REQUIRES(Locks::mutator_lock_);

  mirror::DexCache* GetDexCache() SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE ArtMethod* GetInterfaceMethodIfProxy(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // May cause thread suspension due to class resolution.
  bool EqualParameters(Handle<mirror::ObjectArray<mirror::Class>> params)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Size of an instance of this native class.
  static size_t Size(size_t pointer_size) {
    return RoundUp(OFFSETOF_MEMBER(ArtMethod, ptr_sized_fields_), pointer_size) +
        (sizeof(PtrSizedFields) / sizeof(void*)) * pointer_size;
  }

  // Alignment of an instance of this native class.
  static size_t Alignment(size_t pointer_size) {
    // The ArtMethod alignment is the same as image pointer size. This differs from
    // alignof(ArtMethod) if cross-compiling with pointer_size != sizeof(void*).
    return pointer_size;
  }

  void CopyFrom(const ArtMethod* src, size_t image_pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  ALWAYS_INLINE GcRoot<mirror::Class>* GetDexCacheResolvedTypes(size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_);

  uint16_t IncrementCounter() {
    return ++hotness_count_;
  }

 protected:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class we are a part of.
  GcRoot<mirror::Class> declaring_class_;

  // Access flags; low 16 bits are defined by spec.
  uint32_t access_flags_;

  /* Dex file fields. The defining dex file is available via declaring_class_->dex_cache_ */

  // Offset to the CodeItem.
  uint32_t dex_code_item_offset_;

  // Index into method_ids of the dex file associated with this method.
  uint32_t dex_method_index_;

  /* End of dex file fields. */

  // Entry within a dispatch table for this method. For static/direct methods the index is into
  // the declaringClass.directMethods, for virtual methods the vtable and for interface methods the
  // ifTable.
  uint16_t method_index_;

  // The hotness we measure for this method. Incremented by the interpreter. Not atomic, as we allow
  // missing increments: if the method is hot, we will see it eventually.
  uint16_t hotness_count_;

  // Fake padding field gets inserted here.

  // Must be the last fields in the method.
  // PACKED(4) is necessary for the correctness of
  // RoundUp(OFFSETOF_MEMBER(ArtMethod, ptr_sized_fields_), pointer_size).
  struct PACKED(4) PtrSizedFields {
    // Short cuts to declaring_class_->dex_cache_ member for fast compiled code access.
    ArtMethod** dex_cache_resolved_methods_;

    // Short cuts to declaring_class_->dex_cache_ member for fast compiled code access.
    GcRoot<mirror::Class>* dex_cache_resolved_types_;

    // Pointer to JNI function registered to this method, or a function to resolve the JNI function,
    // or the profiling data for non-native methods.
    void* entry_point_from_jni_;

    // Method dispatch from quick compiled code invokes this pointer which may cause bridging into
    // the interpreter.
    void* entry_point_from_quick_compiled_code_;
  } ptr_sized_fields_;

 private:
  static size_t PtrSizedFieldsOffset(size_t pointer_size) {
    // Round up to pointer size for padding field.
    return RoundUp(OFFSETOF_MEMBER(ArtMethod, ptr_sized_fields_), pointer_size);
  }

  template<typename T>
  ALWAYS_INLINE T GetNativePointer(MemberOffset offset, size_t pointer_size) const {
    static_assert(std::is_pointer<T>::value, "T must be a pointer type");
    DCHECK(ValidPointerSize(pointer_size)) << pointer_size;
    const auto addr = reinterpret_cast<uintptr_t>(this) + offset.Uint32Value();
    if (pointer_size == sizeof(uint32_t)) {
      return reinterpret_cast<T>(*reinterpret_cast<const uint32_t*>(addr));
    } else {
      auto v = *reinterpret_cast<const uint64_t*>(addr);
      return reinterpret_cast<T>(dchecked_integral_cast<uintptr_t>(v));
    }
  }

  template<typename T>
  ALWAYS_INLINE void SetNativePointer(MemberOffset offset, T new_value, size_t pointer_size) {
    static_assert(std::is_pointer<T>::value, "T must be a pointer type");
    DCHECK(ValidPointerSize(pointer_size)) << pointer_size;
    const auto addr = reinterpret_cast<uintptr_t>(this) + offset.Uint32Value();
    if (pointer_size == sizeof(uint32_t)) {
      uintptr_t ptr = reinterpret_cast<uintptr_t>(new_value);
      *reinterpret_cast<uint32_t*>(addr) = dchecked_integral_cast<uint32_t>(ptr);
    } else {
      *reinterpret_cast<uint64_t*>(addr) = reinterpret_cast<uintptr_t>(new_value);
    }
  }

  // Code points to the start of the quick code.
  static uint32_t GetCodeSize(const void* code);

  static bool PcIsWithinQuickCode(uintptr_t code, uintptr_t pc) {
    if (code == 0) {
      return pc == 0;
    }
    /*
     * During a stack walk, a return PC may point past-the-end of the code
     * in the case that the last instruction is a call that isn't expected to
     * return.  Thus, we check <= code + GetCodeSize().
     *
     * NOTE: For Thumb both pc and code are offset by 1 indicating the Thumb state.
     */
    return code <= pc && pc <= code + GetCodeSize(
        EntryPointToCodePointer(reinterpret_cast<const void*>(code)));
  }

  DISALLOW_COPY_AND_ASSIGN(ArtMethod);  // Need to use CopyFrom to deal with 32 vs 64 bits.
};

}  // namespace art

#endif  // ART_RUNTIME_ART_METHOD_H_
