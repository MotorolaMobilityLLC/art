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

#include "compiler_llvm.h"

#include "backend_options.h"
#include "class_linker.h"
#include "compilation_unit.h"
#include "compiled_method.h"
#include "compiler.h"
#include "dex_cache.h"
#include "ir_builder.h"
#include "jni_compiler.h"
#include "method_compiler.h"
#include "oat_compilation_unit.h"
#include "oat_file.h"
#include "stl_util.h"
#include "stub_compiler.h"

#include <llvm/LinkAllPasses.h>
#include <llvm/LinkAllVMCore.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>

namespace llvm {
  extern bool TimePassesIsEnabled;
}

namespace {

pthread_once_t llvm_initialized = PTHREAD_ONCE_INIT;

void InitializeLLVM() {
  // Initialize LLVM internal data structure for multithreading
  llvm::llvm_start_multithreaded();

  // NOTE: Uncomment following line to show the time consumption of LLVM passes
  //llvm::TimePassesIsEnabled = true;

  // Initialize LLVM target-specific options.
  art::compiler_llvm::InitialBackendOptions();

  // Initialize LLVM target, MC subsystem, asm printer, and asm parser
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
  // TODO: Maybe we don't have to initialize "all" targets.

  // Initialize LLVM optimization passes
  llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();

  llvm::initializeCore(registry);
  llvm::initializeScalarOpts(registry);
  llvm::initializeIPO(registry);
  llvm::initializeAnalysis(registry);
  llvm::initializeIPA(registry);
  llvm::initializeTransformUtils(registry);
  llvm::initializeInstCombine(registry);
  llvm::initializeInstrumentation(registry);
  llvm::initializeTarget(registry);
}

// The Guard to Shutdown LLVM
// llvm::llvm_shutdown_obj llvm_guard;
// TODO: We are commenting out this line because this will cause SEGV from
// time to time.
// Two reasons: (1) the order of the destruction of static objects, or
//              (2) dlopen/dlclose side-effect on static objects.

} // anonymous namespace


namespace art {
namespace compiler_llvm {


llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
    : compiler_(compiler), insn_set_(insn_set),
      num_cunits_lock_("compilation unit counter lock"), num_cunits_(0),
      plt_(insn_set) {

  // Initialize LLVM libraries
  pthread_once(&llvm_initialized, InitializeLLVM);
}


CompilerLLVM::~CompilerLLVM() {
}


CompilationUnit* CompilerLLVM::AllocateCompilationUnit() {
  MutexLock GUARD(num_cunits_lock_);
  return new CompilationUnit(this, num_cunits_++);
}


CompiledMethod* CompilerLLVM::
CompileDexMethod(OatCompilationUnit* oat_compilation_unit) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<MethodCompiler> method_compiler(
      new MethodCompiler(cunit.get(), compiler_, oat_compilation_unit));

  return method_compiler->Compile();
}


CompiledMethod* CompilerLLVM::
CompileNativeMethod(OatCompilationUnit* oat_compilation_unit) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<JniCompiler> jni_compiler(
      new JniCompiler(cunit.get(), *compiler_, oat_compilation_unit));

  return jni_compiler->Compile();
}


CompiledInvokeStub* CompilerLLVM::CreateInvokeStub(bool is_static,
                                                   char const *shorty) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<StubCompiler> stub_compiler(
    new StubCompiler(cunit.get(), *compiler_));

  return stub_compiler->CreateInvokeStub(is_static, shorty);
}


CompiledInvokeStub* CompilerLLVM::CreateProxyStub(char const *shorty) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<StubCompiler> stub_compiler(
    new StubCompiler(cunit.get(), *compiler_));

  return stub_compiler->CreateProxyStub(shorty);
}

} // namespace compiler_llvm
} // namespace art

inline static art::compiler_llvm::CompilerLLVM* ContextOf(art::Compiler& compiler) {
  void *compiler_context = compiler.GetCompilerContext();
  CHECK(compiler_context != NULL);
  return reinterpret_cast<art::compiler_llvm::CompilerLLVM*>(compiler_context);
}

inline static const art::compiler_llvm::CompilerLLVM* ContextOf(const art::Compiler& compiler) {
  void *compiler_context = compiler.GetCompilerContext();
  CHECK(compiler_context != NULL);
  return reinterpret_cast<const art::compiler_llvm::CompilerLLVM*>(compiler_context);
}

extern "C" void ArtInitCompilerContext(art::Compiler& compiler) {
  CHECK(compiler.GetCompilerContext() == NULL);

  art::compiler_llvm::CompilerLLVM* compiler_llvm =
      new art::compiler_llvm::CompilerLLVM(&compiler,
                                           compiler.GetInstructionSet());

  compiler.SetCompilerContext(compiler_llvm);
}

extern "C" void ArtUnInitCompilerContext(art::Compiler& compiler) {
  delete ContextOf(compiler);
  compiler.SetCompilerContext(NULL);
}

extern "C" art::CompiledMethod* ArtCompileMethod(art::Compiler& compiler,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags, uint32_t method_idx,
                                                 const art::ClassLoader* class_loader,
                                                 const art::DexFile& dex_file)
{
  art::ClassLinker *class_linker = art::Runtime::Current()->GetClassLinker();
  art::DexCache *dex_cache = class_linker->FindDexCache(dex_file);

  art::OatCompilationUnit oat_compilation_unit(
    class_loader, class_linker, dex_file, *dex_cache, code_item,
    method_idx, access_flags);
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledMethod* result = compiler_llvm->CompileDexMethod(&oat_compilation_unit);
  return result;
}

extern "C" art::CompiledMethod* ArtJniCompileMethod(art::Compiler& compiler,
                                                    uint32_t access_flags, uint32_t method_idx,
                                                    const art::DexFile& dex_file) {
  art::ClassLinker *class_linker = art::Runtime::Current()->GetClassLinker();
  art::DexCache *dex_cache = class_linker->FindDexCache(dex_file);

  art::OatCompilationUnit oat_compilation_unit(
    NULL, class_linker, dex_file, *dex_cache, NULL,
    method_idx, access_flags);

  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledMethod* result = compiler_llvm->CompileNativeMethod(&oat_compilation_unit);
  return result;
}

extern "C" art::CompiledInvokeStub* ArtCreateInvokeStub(art::Compiler& compiler,
                                                        bool is_static,
                                                        const char* shorty,
                                                        uint32_t shorty_len) {
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledInvokeStub* result = compiler_llvm->CreateInvokeStub(is_static, shorty);
  return result;
}

extern "C" art::CompiledInvokeStub* ArtCreateProxyStub(art::Compiler& compiler,
                                                       const char* shorty,
                                                       uint32_t shorty_len) {
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledInvokeStub* result = compiler_llvm->CreateProxyStub(shorty);
  return result;
}

extern "C" void compilerLLVMSetBitcodeFileName(art::Compiler& compiler,
                                               std::string const& filename) {
  ContextOf(compiler)->SetBitcodeFileName(filename);
}
