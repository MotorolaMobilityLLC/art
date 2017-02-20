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

#include "driver/compiler_driver.h"

#include <stdint.h>
#include <stdio.h>
#include <memory>

#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_compiler_test.h"
#include "dex_file.h"
#include "dex_file_types.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "handle_scope-inl.h"
#include "jit/profile_compilation_info.h"
#include "scoped_thread_state_change-inl.h"

namespace art {

class CompilerDriverTest : public CommonCompilerTest {
 protected:
  void CompileAll(jobject class_loader) REQUIRES(!Locks::mutator_lock_) {
    TimingLogger timings("CompilerDriverTest::CompileAll", false, false);
    TimingLogger::ScopedTiming t(__FUNCTION__, &timings);
    compiler_driver_->CompileAll(class_loader,
                                 GetDexFiles(class_loader),
                                 /* verifier_deps */ nullptr,
                                 &timings);
    t.NewTiming("MakeAllExecutable");
    MakeAllExecutable(class_loader);
  }

  void EnsureCompiled(jobject class_loader, const char* class_name, const char* method,
                      const char* signature, bool is_virtual)
      REQUIRES(!Locks::mutator_lock_) {
    CompileAll(class_loader);
    Thread::Current()->TransitionFromSuspendedToRunnable();
    bool started = runtime_->Start();
    CHECK(started);
    env_ = Thread::Current()->GetJniEnv();
    class_ = env_->FindClass(class_name);
    CHECK(class_ != nullptr) << "Class not found: " << class_name;
    if (is_virtual) {
      mid_ = env_->GetMethodID(class_, method, signature);
    } else {
      mid_ = env_->GetStaticMethodID(class_, method, signature);
    }
    CHECK(mid_ != nullptr) << "Method not found: " << class_name << "." << method << signature;
  }

  void MakeAllExecutable(jobject class_loader) {
    const std::vector<const DexFile*> class_path = GetDexFiles(class_loader);
    for (size_t i = 0; i != class_path.size(); ++i) {
      const DexFile* dex_file = class_path[i];
      CHECK(dex_file != nullptr);
      MakeDexFileExecutable(class_loader, *dex_file);
    }
  }

  void MakeDexFileExecutable(jobject class_loader, const DexFile& dex_file) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
      const char* descriptor = dex_file.GetClassDescriptor(class_def);
      ScopedObjectAccess soa(Thread::Current());
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::ClassLoader> loader(
          hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
      mirror::Class* c = class_linker->FindClass(soa.Self(), descriptor, loader);
      CHECK(c != nullptr);
      const auto pointer_size = class_linker->GetImagePointerSize();
      for (auto& m : c->GetMethods(pointer_size)) {
        MakeExecutable(&m);
      }
    }
  }

  JNIEnv* env_;
  jclass class_;
  jmethodID mid_;
};

// Disabled due to 10 second runtime on host
// TODO: Update the test for hash-based dex cache arrays. Bug: 30627598
TEST_F(CompilerDriverTest, DISABLED_LARGE_CompileDexLibCore) {
  CompileAll(nullptr);

  // All libcore references should resolve
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  const DexFile& dex = *java_lang_dex_file_;
  ObjPtr<mirror::DexCache> dex_cache = class_linker_->FindDexCache(soa.Self(), dex);
  EXPECT_EQ(dex.NumStringIds(), dex_cache->NumStrings());
  for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
    const mirror::String* string = dex_cache->GetResolvedString(dex::StringIndex(i));
    EXPECT_TRUE(string != nullptr) << "string_idx=" << i;
  }
  EXPECT_EQ(dex.NumTypeIds(), dex_cache->NumResolvedTypes());
  for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
    mirror::Class* type = dex_cache->GetResolvedType(dex::TypeIndex(i));
    EXPECT_TRUE(type != nullptr) << "type_idx=" << i
                              << " " << dex.GetTypeDescriptor(dex.GetTypeId(dex::TypeIndex(i)));
  }
  EXPECT_EQ(dex.NumMethodIds(), dex_cache->NumResolvedMethods());
  auto* cl = Runtime::Current()->GetClassLinker();
  auto pointer_size = cl->GetImagePointerSize();
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    ArtMethod* method = dex_cache->GetResolvedMethod(i, pointer_size);
    EXPECT_TRUE(method != nullptr) << "method_idx=" << i
                                << " " << dex.GetMethodDeclaringClassDescriptor(dex.GetMethodId(i))
                                << " " << dex.GetMethodName(dex.GetMethodId(i));
    EXPECT_TRUE(method->GetEntryPointFromQuickCompiledCode() != nullptr) << "method_idx=" << i
        << " " << dex.GetMethodDeclaringClassDescriptor(dex.GetMethodId(i)) << " "
        << dex.GetMethodName(dex.GetMethodId(i));
  }
  EXPECT_EQ(dex.NumFieldIds(), dex_cache->NumResolvedFields());
  for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
    ArtField* field = cl->GetResolvedField(i, dex_cache);
    EXPECT_TRUE(field != nullptr) << "field_idx=" << i
                               << " " << dex.GetFieldDeclaringClassDescriptor(dex.GetFieldId(i))
                               << " " << dex.GetFieldName(dex.GetFieldId(i));
  }

  // TODO check Class::IsVerified for all classes

  // TODO: check that all Method::GetCode() values are non-null
}

TEST_F(CompilerDriverTest, AbstractMethodErrorStub) {
  TEST_DISABLED_FOR_READ_BARRIER_WITH_OPTIMIZING_FOR_UNSUPPORTED_INSTRUCTION_SETS();
  jobject class_loader;
  {
    ScopedObjectAccess soa(Thread::Current());
    class_loader = LoadDex("AbstractMethod");
  }
  ASSERT_TRUE(class_loader != nullptr);
  EnsureCompiled(class_loader, "AbstractClass", "foo", "()V", true);

  // Create a jobj_ of ConcreteClass, NOT AbstractClass.
  jclass c_class = env_->FindClass("ConcreteClass");

  jmethodID constructor = env_->GetMethodID(c_class, "<init>", "()V");

  jobject jobj_ = env_->NewObject(c_class, constructor);
  ASSERT_TRUE(jobj_ != nullptr);

  // Force non-virtual call to AbstractClass foo, will throw AbstractMethodError exception.
  env_->CallNonvirtualVoidMethod(jobj_, class_, mid_);

  EXPECT_EQ(env_->ExceptionCheck(), JNI_TRUE);
  jthrowable exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  jclass jlame = env_->FindClass("java/lang/AbstractMethodError");
  EXPECT_TRUE(env_->IsInstanceOf(exception, jlame));
  {
    ScopedObjectAccess soa(Thread::Current());
    Thread::Current()->ClearException();
  }
}

class CompilerDriverMethodsTest : public CompilerDriverTest {
 protected:
  std::unordered_set<std::string>* GetCompiledMethods() OVERRIDE {
    return new std::unordered_set<std::string>({
      "byte StaticLeafMethods.identity(byte)",
      "int StaticLeafMethods.sum(int, int, int)",
      "double StaticLeafMethods.sum(double, double, double, double)"
    });
  }
};

TEST_F(CompilerDriverMethodsTest, Selection) {
  TEST_DISABLED_FOR_READ_BARRIER_WITH_OPTIMIZING_FOR_UNSUPPORTED_INSTRUCTION_SETS();
  Thread* self = Thread::Current();
  jobject class_loader;
  {
    ScopedObjectAccess soa(self);
    class_loader = LoadDex("StaticLeafMethods");
  }
  ASSERT_NE(class_loader, nullptr);

  // Need to enable dex-file writability. Methods rejected to be compiled will run through the
  // dex-to-dex compiler.
  for (const DexFile* dex_file : GetDexFiles(class_loader)) {
    ASSERT_TRUE(dex_file->EnableWrite());
  }

  CompileAll(class_loader);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ScopedObjectAccess soa(self);
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> h_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  mirror::Class* klass = class_linker->FindClass(self, "LStaticLeafMethods;", h_loader);
  ASSERT_NE(klass, nullptr);

  std::unique_ptr<std::unordered_set<std::string>> expected(GetCompiledMethods());

  const auto pointer_size = class_linker->GetImagePointerSize();
  for (auto& m : klass->GetDirectMethods(pointer_size)) {
    std::string name = m.PrettyMethod(true);
    const void* code = m.GetEntryPointFromQuickCompiledCodePtrSize(pointer_size);
    ASSERT_NE(code, nullptr);
    if (expected->find(name) != expected->end()) {
      expected->erase(name);
      EXPECT_FALSE(class_linker->IsQuickToInterpreterBridge(code));
    } else {
      EXPECT_TRUE(class_linker->IsQuickToInterpreterBridge(code));
    }
  }
  EXPECT_TRUE(expected->empty());
}

class CompilerDriverProfileTest : public CompilerDriverTest {
 protected:
  ProfileCompilationInfo* GetProfileCompilationInfo() OVERRIDE {
    ScopedObjectAccess soa(Thread::Current());
    std::vector<std::unique_ptr<const DexFile>> dex_files = OpenTestDexFiles("ProfileTestMultiDex");

    ProfileCompilationInfo info;
    for (const std::unique_ptr<const DexFile>& dex_file : dex_files) {
      std::string key = ProfileCompilationInfo::GetProfileDexFileKey(dex_file->GetLocation());
      profile_info_.AddMethodIndex(key, dex_file->GetLocationChecksum(), 1);
      profile_info_.AddMethodIndex(key, dex_file->GetLocationChecksum(), 2);
    }
    return &profile_info_;
  }

  CompilerFilter::Filter GetCompilerFilter() const OVERRIDE {
    // Use a profile based filter.
    return CompilerFilter::kSpeedProfile;
  }

  std::unordered_set<std::string> GetExpectedMethodsForClass(const std::string& clazz) {
    if (clazz == "Main") {
      return std::unordered_set<std::string>({
          "java.lang.String Main.getA()",
          "java.lang.String Main.getB()"});
    } else if (clazz == "Second") {
      return std::unordered_set<std::string>({
          "java.lang.String Second.getX()",
          "java.lang.String Second.getY()"});
    } else {
      return std::unordered_set<std::string>();
    }
  }

  void CheckCompiledMethods(jobject class_loader,
                            const std::string& clazz,
                            const std::unordered_set<std::string>& expected_methods) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    StackHandleScope<1> hs(self);
    Handle<mirror::ClassLoader> h_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
    mirror::Class* klass = class_linker->FindClass(self, clazz.c_str(), h_loader);
    ASSERT_NE(klass, nullptr);

    const auto pointer_size = class_linker->GetImagePointerSize();
    size_t number_of_compiled_methods = 0;
    for (auto& m : klass->GetVirtualMethods(pointer_size)) {
      std::string name = m.PrettyMethod(true);
      const void* code = m.GetEntryPointFromQuickCompiledCodePtrSize(pointer_size);
      ASSERT_NE(code, nullptr);
      if (expected_methods.find(name) != expected_methods.end()) {
        number_of_compiled_methods++;
        EXPECT_FALSE(class_linker->IsQuickToInterpreterBridge(code));
      } else {
        EXPECT_TRUE(class_linker->IsQuickToInterpreterBridge(code));
      }
    }
    EXPECT_EQ(expected_methods.size(), number_of_compiled_methods);
  }

 private:
  ProfileCompilationInfo profile_info_;
};

TEST_F(CompilerDriverProfileTest, ProfileGuidedCompilation) {
  TEST_DISABLED_FOR_READ_BARRIER_WITH_OPTIMIZING_FOR_UNSUPPORTED_INSTRUCTION_SETS();
  Thread* self = Thread::Current();
  jobject class_loader;
  {
    ScopedObjectAccess soa(self);
    class_loader = LoadDex("ProfileTestMultiDex");
  }
  ASSERT_NE(class_loader, nullptr);

  // Need to enable dex-file writability. Methods rejected to be compiled will run through the
  // dex-to-dex compiler.
  for (const DexFile* dex_file : GetDexFiles(class_loader)) {
    ASSERT_TRUE(dex_file->EnableWrite());
  }

  CompileAll(class_loader);

  std::unordered_set<std::string> m = GetExpectedMethodsForClass("Main");
  std::unordered_set<std::string> s = GetExpectedMethodsForClass("Second");
  CheckCompiledMethods(class_loader, "LMain;", m);
  CheckCompiledMethods(class_loader, "LSecond;", s);
}

// TODO: need check-cast test (when stub complete & we can throw/catch

}  // namespace art
