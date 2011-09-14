// Copyright 2011 Google Inc. All Rights Reserved.

#include "compiler.h"

#include "assembler.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "jni_compiler.h"
#include "jni_internal.h"
#include "runtime.h"

extern bool oatCompileMethod(art::Method*, art::InstructionSet);

namespace art {

typedef void (*ThrowAme)(Method*, Thread*);

void ThrowAbstractMethodError(Method* method, Thread* thread) {
  LOG(FATAL) << "Unimplemented Exception Handling. Remove this when ThrowException works.";
  thread->ThrowNewException("Ljava/lang/AbstractMethodError",
                            "abstract method \"%s\"",
                            PrettyMethod(method).c_str());
}

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub(ThrowAme);
}

namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub(ThrowAme);
}

Compiler::Compiler(InstructionSet insns) : instruction_set_(insns), jni_compiler_(insns) {
  if (insns == kArm || insns == kThumb2) {
    abstract_method_error_stub_ = arm::CreateAbstractMethodErrorStub(&ThrowAbstractMethodError);
  } else if (insns == kX86) {
    abstract_method_error_stub_ = x86::CreateAbstractMethodErrorStub(&ThrowAbstractMethodError);
  }
}

void Compiler::CompileAll(const ClassLoader* class_loader) {
  Resolve(class_loader);
  // TODO add verification step
  Compile(class_loader);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::CompileOne(Method* method) {
  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();
  Resolve(class_loader);
  // TODO add verification step
  CompileMethod(method);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::Resolve(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file);
  }
}

void Compiler::ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // Strings are easy, they always are simply resolved to literals in the same file
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
    class_linker->ResolveString(dex_file, i, dex_cache);
  }

  // Class derived values are more complicated, they require the linker and loader
  for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
    class_linker->ResolveType(dex_file, i, dex_cache, class_loader);
  }
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    // unknown if direct or virtual, try both
    Method* method = class_linker->ResolveMethod(dex_file, i, dex_cache, class_loader, false);
    if (method == NULL) {
      class_linker->ResolveMethod(dex_file, i, dex_cache, class_loader, true);
    }
  }
  for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
    // unknown if instance or static, try both
    Field* field = class_linker->ResolveField(dex_file, i, dex_cache, class_loader, false);
    if (field == NULL) {
      class_linker->ResolveField(dex_file, i, dex_cache, class_loader, true);
    }
  }
}

void Compiler::Compile(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file);
  }
}

void Compiler::CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    CHECK(klass != NULL);
    CompileClass(klass);
  }
}

void Compiler::CompileClass(Class* klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    CompileMethod(klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    CompileMethod(klass->GetVirtualMethod(i));
  }
}

namespace arm {
  void ArmCreateInvokeStub(Method* method);
}
namespace x86 {
  void X86CreateInvokeStub(Method* method);
}

void Compiler::CompileMethod(Method* method) {
  if (method->IsNative()) {
    jni_compiler_.Compile(method);
  } else if (method->IsAbstract()) {
    DCHECK(abstract_method_error_stub_ != NULL);
    if (instruction_set_ == kX86) {
      method->SetCode(abstract_method_error_stub_, kX86);
    } else {
      CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
      method->SetCode(abstract_method_error_stub_, kArm);
    }
  } else {
    oatCompileMethod(method, kThumb2);
  }
  CHECK(method->GetCode() != NULL);

  if (instruction_set_ == kX86) {
    art::x86::X86CreateInvokeStub(method);
  } else {
    CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
    // Generates invocation stub using ARM instruction set
    art::arm::ArmCreateInvokeStub(method);
  }
  CHECK(method->GetInvokeStub() != NULL);
}

void Compiler::SetCodeAndDirectMethods(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    SetCodeAndDirectMethodsDexFile(*dex_file);
  }
}

void Compiler::SetCodeAndDirectMethodsDexFile(const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  CodeAndDirectMethods* code_and_direct_methods = dex_cache->GetCodeAndDirectMethods();
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    if (method == NULL) {
      code_and_direct_methods->SetResolvedDirectMethodTrampoline(i);
    } else if (method->IsDirect()) {
      code_and_direct_methods->SetResolvedDirectMethod(i, method);
    } else {
      // TODO: we currently leave the entry blank for resolved
      // non-direct methods.  we could put in an error stub.
    }
  }
}

}  // namespace art
