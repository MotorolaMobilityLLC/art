/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <limits.h>

#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/allocator/dlmalloc.h"
#include "gc/heap.h"
#include "gc/space/dlmalloc_space.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "scoped_fast_native_object_access.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "toStringArray.h"

namespace art {

static jfloat VMRuntime_getTargetHeapUtilization(JNIEnv*, jobject) {
  return Runtime::Current()->GetHeap()->GetTargetHeapUtilization();
}

static void VMRuntime_nativeSetTargetHeapUtilization(JNIEnv*, jobject, jfloat target) {
  Runtime::Current()->GetHeap()->SetTargetHeapUtilization(target);
}

static void VMRuntime_startJitCompilation(JNIEnv*, jobject) {
}

static void VMRuntime_disableJitCompilation(JNIEnv*, jobject) {
}

static jobject VMRuntime_newNonMovableArray(JNIEnv* env,
                                            jobject,
                                            jclass javaElementClass,
                                            jint length) {
  ScopedFastNativeObjectAccess soa(env);
#ifdef MOVING_GARBAGE_COLLECTOR
  // TODO: right now, we don't have a copying collector, so there's no need
  // to do anything special here, but we ought to pass the non-movability
  // through to the allocator.
  UNIMPLEMENTED(FATAL);
#endif

  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  if (element_class == NULL) {
    ThrowNullPointerException(NULL, "element class == null");
    return NULL;
  }
  if (length < 0) {
    ThrowNegativeArraySizeException(length);
    return NULL;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  std::string descriptor;
  descriptor += "[";
  descriptor += ClassHelper(element_class).GetDescriptor();
  mirror::Class* array_class = class_linker->FindClass(descriptor.c_str(), NULL);
  mirror::Array* result = mirror::Array::Alloc(soa.Self(), array_class, length);
  return soa.AddLocalReference<jobject>(result);
}

static jlong VMRuntime_addressOf(JNIEnv* env, jobject, jobject javaArray) {
  if (javaArray == NULL) {  // Most likely allocation failed
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  mirror::Array* array = soa.Decode<mirror::Array*>(javaArray);
  if (!array->IsArrayInstance()) {
    ThrowIllegalArgumentException(NULL, "not an array");
    return 0;
  }
  // TODO: we should also check that this is a non-movable array.
  return reinterpret_cast<uintptr_t>(array->GetRawData(array->GetClass()->GetComponentSize()));
}

static void VMRuntime_clearGrowthLimit(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->ClearGrowthLimit();
}

static jboolean VMRuntime_isDebuggerActive(JNIEnv*, jobject) {
  return Dbg::IsDebuggerActive();
}

static jobjectArray VMRuntime_properties(JNIEnv* env, jobject) {
  return toStringArray(env, Runtime::Current()->GetProperties());
}

// This is for backward compatibility with dalvik which returned the
// meaningless "." when no boot classpath or classpath was
// specified. Unfortunately, some tests were using java.class.path to
// lookup relative file locations, so they are counting on this to be
// ".", presumably some applications or libraries could have as well.
static const char* DefaultToDot(const std::string& class_path) {
  return class_path.empty() ? "." : class_path.c_str();
}

static jstring VMRuntime_bootClassPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetBootClassPathString()));
}

static jstring VMRuntime_classPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetClassPathString()));
}

static jstring VMRuntime_vmVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(Runtime::Current()->GetVersion());
}

static jstring VMRuntime_vmLibrary(JNIEnv* env, jobject) {
  return env->NewStringUTF(kIsDebugBuild ? "libartd.so" : "libart.so");
}

static void VMRuntime_setTargetSdkVersion(JNIEnv* env, jobject, jint targetSdkVersion) {
  // This is the target SDK version of the app we're about to run.
  // Note that targetSdkVersion may be CUR_DEVELOPMENT (10000).
  // Note that targetSdkVersion may be 0, meaning "current".
  if (targetSdkVersion > 0 && targetSdkVersion <= 13 /* honeycomb-mr2 */) {
    Runtime* runtime = Runtime::Current();
    JavaVMExt* vm = runtime->GetJavaVM();
    if (vm->check_jni) {
      LOG(INFO) << "CheckJNI enabled: not enabling JNI app bug workarounds.";
    } else {
      LOG(INFO) << "Turning on JNI app bug workarounds for target SDK version "
          << targetSdkVersion << "...";

      vm->work_around_app_jni_bugs = true;
    }
  }
}

static void VMRuntime_registerNativeAllocation(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeAllocation(env, bytes);
}

static void VMRuntime_registerNativeFree(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeFree(env, bytes);
}

static void VMRuntime_trimHeap(JNIEnv*, jobject) {
  uint64_t start_ns = NanoTime();

  // Trim the managed heap.
  gc::Heap* heap = Runtime::Current()->GetHeap();
  float managed_utilization = (static_cast<float>(heap->GetBytesAllocated()) /
                               heap->GetTotalMemory());
  size_t managed_reclaimed = heap->Trim();

  uint64_t gc_heap_end_ns = NanoTime();

  // Trim the native heap.
  dlmalloc_trim(0);
  size_t native_reclaimed = 0;
  dlmalloc_inspect_all(DlmallocMadviseCallback, &native_reclaimed);

  uint64_t end_ns = NanoTime();

  LOG(INFO) << "Heap trim of managed (duration=" << PrettyDuration(gc_heap_end_ns - start_ns)
      << ", advised=" << PrettySize(managed_reclaimed) << ") and native (duration="
      << PrettyDuration(end_ns - gc_heap_end_ns) << ", advised=" << PrettySize(native_reclaimed)
      << ") heaps. Managed heap utilization of " << static_cast<int>(100 * managed_utilization)
      << "%.";
}

static void VMRuntime_concurrentGC(JNIEnv* env, jobject) {
  Thread* self = ThreadForEnv(env);
  Runtime::Current()->GetHeap()->ConcurrentGC(self);
}

typedef std::map<std::string, mirror::String*> StringTable;

static void PreloadDexCachesStringsVisitor(const mirror::Object* root, void* arg) {
  StringTable& table = *reinterpret_cast<StringTable*>(arg);
  mirror::String* string = const_cast<mirror::Object*>(root)->AsString();
  // LOG(INFO) << "VMRuntime.preloadDexCaches interned=" << string->ToModifiedUtf8();
  table[string->ToModifiedUtf8()] = string;
}

// Based on ClassLinker::ResolveString.
static void PreloadDexCachesResolveString(mirror::DexCache* dex_cache,
                                          uint32_t string_idx,
                                          StringTable& strings)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* string = dex_cache->GetResolvedString(string_idx);
  if (string != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  uint32_t utf16Size;
  const char* utf8 = dex_file->StringDataAndLengthByIdx(string_idx, &utf16Size);
  string = strings[utf8];
  if (string == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved string=" << utf8;
  dex_cache->SetResolvedString(string_idx, string);
}

// Based on ClassLinker::ResolveType.
static void PreloadDexCachesResolveType(mirror::DexCache* dex_cache, uint32_t type_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
  if (klass != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const char* class_name = dex_file->StringByTypeIdx(type_idx);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  if (class_name[1] == '\0') {
    klass = linker->FindPrimitiveClass(class_name[0]);
  } else {
    klass = linker->LookupClass(class_name, NULL);
  }
  if (klass == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved klass=" << class_name;
  dex_cache->SetResolvedType(type_idx, klass);
  // Skip uninitialized classes because filled static storage entry implies it is initialized.
  if (!klass->IsInitialized()) {
    // LOG(INFO) << "VMRuntime.preloadDexCaches uninitialized klass=" << class_name;
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches static storage klass=" << class_name;
  dex_cache->GetInitializedStaticStorage()->Set(type_idx, klass);
}

// Based on ClassLinker::ResolveField.
static void PreloadDexCachesResolveField(mirror::DexCache* dex_cache,
                                         uint32_t field_idx,
                                         bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = dex_cache->GetResolvedField(field_idx);
  if (field != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::FieldId& field_id = dex_file->GetFieldId(field_idx);
  mirror::Class* klass = dex_cache->GetResolvedType(field_id.class_idx_);
  if (klass == NULL) {
    return;
  }
  if (is_static) {
    field = klass->FindStaticField(dex_cache, field_idx);
  } else {
    field = klass->FindInstanceField(dex_cache, field_idx);
  }
  if (field == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved field " << PrettyField(field);
  dex_cache->SetResolvedField(field_idx, field);
}

// Based on ClassLinker::ResolveMethod.
static void PreloadDexCachesResolveMethod(mirror::DexCache* dex_cache,
                                          uint32_t method_idx,
                                          InvokeType invoke_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = dex_cache->GetResolvedMethod(method_idx);
  if (method != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
  mirror::Class* klass = dex_cache->GetResolvedType(method_id.class_idx_);
  if (klass == NULL) {
    return;
  }
  switch (invoke_type) {
    case kDirect:
    case kStatic:
      method = klass->FindDirectMethod(dex_cache, method_idx);
      break;
    case kInterface:
      method = klass->FindInterfaceMethod(dex_cache, method_idx);
      break;
    case kSuper:
    case kVirtual:
      method = klass->FindVirtualMethod(dex_cache, method_idx);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << invoke_type;
  }
  if (method == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved method " << PrettyMethod(method);
  dex_cache->SetResolvedMethod(method_idx, method);
}

struct DexCacheStats {
    uint32_t num_strings;
    uint32_t num_types;
    uint32_t num_fields;
    uint32_t num_methods;
    uint32_t num_static_storage;
    DexCacheStats() : num_strings(0),
                      num_types(0),
                      num_fields(0),
                      num_methods(0),
                      num_static_storage(0) {}
};

static const bool kPreloadDexCachesEnabled = true;

// Disabled because it takes a long time (extra half second) but
// gives almost no benefit in terms of saving private dirty pages.
static const bool kPreloadDexCachesStrings = false;

static const bool kPreloadDexCachesTypes = true;
static const bool kPreloadDexCachesFieldsAndMethods = true;

static const bool kPreloadDexCachesCollectStats = true;

static void PreloadDexCachesStatsTotal(DexCacheStats* total) {
  if (!kPreloadDexCachesCollectStats) {
    return;
  }

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    total->num_strings += dex_file->NumStringIds();
    total->num_fields += dex_file->NumFieldIds();
    total->num_methods += dex_file->NumMethodIds();
    total->num_types += dex_file->NumTypeIds();
    total->num_static_storage += dex_file->NumTypeIds();
  }
}

static void PreloadDexCachesStatsFilled(DexCacheStats* filled)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (!kPreloadDexCachesCollectStats) {
      return;
  }
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    mirror::DexCache* dex_cache = linker->FindDexCache(*dex_file);
    for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
      mirror::String* string = dex_cache->GetResolvedString(i);
      if (string != NULL) {
        filled->num_strings++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
      mirror::Class* klass = dex_cache->GetResolvedType(i);
      if (klass != NULL) {
        filled->num_types++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      mirror::ArtField* field = dex_cache->GetResolvedField(i);
      if (field != NULL) {
        filled->num_fields++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
      mirror::ArtMethod* method = dex_cache->GetResolvedMethod(i);
      if (method != NULL) {
        filled->num_methods++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumInitializedStaticStorage(); i++) {
      mirror::StaticStorageBase* klass = dex_cache->GetInitializedStaticStorage()->Get(i);
      if (klass != NULL) {
        filled->num_static_storage++;
      }
    }
  }
}

// TODO: http://b/11309598 This code was ported over based on the
// Dalvik version. However, ART has similar code in other places such
// as the CompilerDriver. This code could probably be refactored to
// serve both uses.
static void VMRuntime_preloadDexCaches(JNIEnv* env, jobject) {
  if (!kPreloadDexCachesEnabled) {
    return;
  }

  ScopedObjectAccess soa(env);

  DexCacheStats total;
  DexCacheStats before;
  if (kPreloadDexCachesCollectStats) {
    LOG(INFO) << "VMRuntime.preloadDexCaches starting";
    PreloadDexCachesStatsTotal(&total);
    PreloadDexCachesStatsFilled(&before);
  }

  Runtime* runtime = Runtime::Current();
  ClassLinker* linker = runtime->GetClassLinker();

  // We use a std::map to avoid heap allocating StringObjects to lookup in gDvm.literalStrings
  StringTable strings;
  if (kPreloadDexCachesStrings) {
    runtime->GetInternTable()->VisitRoots(PreloadDexCachesStringsVisitor, &strings, false, false);
  }

  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    mirror::DexCache* dex_cache = linker->FindDexCache(*dex_file);

    if (kPreloadDexCachesStrings) {
      for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
        PreloadDexCachesResolveString(dex_cache, i, strings);
      }
    }

    if (kPreloadDexCachesTypes) {
      for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
        PreloadDexCachesResolveType(dex_cache, i);
      }
    }

    if (kPreloadDexCachesFieldsAndMethods) {
      for (size_t class_def_index = 0;
           class_def_index < dex_file->NumClassDefs();
           class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        const byte* class_data = dex_file->GetClassData(class_def);
        if (class_data == NULL) {
          continue;
        }
        ClassDataItemIterator it(*dex_file, class_data);
        for (; it.HasNextStaticField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, true);
        }
        for (; it.HasNextInstanceField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, false);
        }
        for (; it.HasNextDirectMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
        for (; it.HasNextVirtualMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
      }
    }
  }

  if (kPreloadDexCachesCollectStats) {
    DexCacheStats after;
    PreloadDexCachesStatsFilled(&after);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches strings total=%d before=%d after=%d",
                              total.num_strings, before.num_strings, after.num_strings);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches types total=%d before=%d after=%d",
                              total.num_types, before.num_types, after.num_types);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches fields total=%d before=%d after=%d",
                              total.num_fields, before.num_fields, after.num_fields);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches methods total=%d before=%d after=%d",
                              total.num_methods, before.num_methods, after.num_methods);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches storage total=%d before=%d after=%d",
                              total.num_static_storage,
                              before.num_static_storage,
                              after.num_static_storage);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches finished");
  }
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMRuntime, addressOf, "!(Ljava/lang/Object;)J"),
  NATIVE_METHOD(VMRuntime, bootClassPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, classPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, clearGrowthLimit, "()V"),
  NATIVE_METHOD(VMRuntime, concurrentGC, "()V"),
  NATIVE_METHOD(VMRuntime, disableJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, getTargetHeapUtilization, "()F"),
  NATIVE_METHOD(VMRuntime, isDebuggerActive, "()Z"),
  NATIVE_METHOD(VMRuntime, nativeSetTargetHeapUtilization, "(F)V"),
  NATIVE_METHOD(VMRuntime, newNonMovableArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, properties, "()[Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, setTargetSdkVersion, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerNativeAllocation, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerNativeFree, "(I)V"),
  NATIVE_METHOD(VMRuntime, startJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, trimHeap, "()V"),
  NATIVE_METHOD(VMRuntime, vmVersion, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, vmLibrary, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, preloadDexCaches, "()V"),
};

void register_dalvik_system_VMRuntime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMRuntime");
}

}  // namespace art
