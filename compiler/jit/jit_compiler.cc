/*
 * Copyright 2014 The Android Open Source Project
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

#include "jit_compiler.h"

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "compiler_callbacks.h"
#include "dex/pass_manager.h"
#include "dex/quick_compiler_callbacks.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/art_method-inl.h"
#include "oat_file-inl.h"
#include "object_lock.h"
#include "thread_list.h"
#include "verifier/method_verifier-inl.h"

namespace art {
namespace jit {

JitCompiler* JitCompiler::Create() {
  return new JitCompiler();
}

extern "C" void* jit_load(CompilerCallbacks** callbacks) {
  VLOG(jit) << "loading jit compiler";
  auto* const jit_compiler = JitCompiler::Create();
  CHECK(jit_compiler != nullptr);
  *callbacks = jit_compiler->GetCompilerCallbacks();
  VLOG(jit) << "Done loading jit compiler";
  return jit_compiler;
}

extern "C" void jit_unload(void* handle) {
  DCHECK(handle != nullptr);
  delete reinterpret_cast<JitCompiler*>(handle);
}

extern "C" bool jit_compile_method(void* handle, mirror::ArtMethod* method, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  auto* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  return jit_compiler->CompileMethod(self, method);
}

JitCompiler::JitCompiler() : total_time_(0) {
  auto* pass_manager_options = new PassManagerOptions;
  pass_manager_options->SetDisablePassList("GVN,DCE");
  compiler_options_.reset(new CompilerOptions(
      CompilerOptions::kDefaultCompilerFilter,
      CompilerOptions::kDefaultHugeMethodThreshold,
      CompilerOptions::kDefaultLargeMethodThreshold,
      CompilerOptions::kDefaultSmallMethodThreshold,
      CompilerOptions::kDefaultTinyMethodThreshold,
      CompilerOptions::kDefaultNumDexMethodsThreshold,
      false,
      false,
      CompilerOptions::kDefaultTopKProfileThreshold,
      false,  // TODO: Think about debuggability of JIT-compiled code.
      false,
      false,
      false,
      false,
      false,  // pic
      nullptr,
      pass_manager_options,
      nullptr,
      false));
  const InstructionSet instruction_set = kRuntimeISA;
  instruction_set_features_.reset(InstructionSetFeatures::FromCppDefines());
  cumulative_logger_.reset(new CumulativeLogger("jit times"));
  verification_results_.reset(new VerificationResults(compiler_options_.get()));
  method_inliner_map_.reset(new DexFileToMethodInlinerMap);
  callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                              method_inliner_map_.get()));
  compiler_driver_.reset(new CompilerDriver(
      compiler_options_.get(), verification_results_.get(), method_inliner_map_.get(),
      Compiler::kQuick, instruction_set, instruction_set_features_.get(), false,
      nullptr, new std::set<std::string>, 1, false, true,
      std::string(), cumulative_logger_.get(), -1, std::string()));
  // Disable dedupe so we can remove compiled methods.
  compiler_driver_->SetDedupeEnabled(false);
  compiler_driver_->SetSupportBootImageFixup(false);
}

JitCompiler::~JitCompiler() {
}

bool JitCompiler::CompileMethod(Thread* self, mirror::ArtMethod* method) {
  uint64_t start_time = NanoTime();
  StackHandleScope<2> hs(self);
  self->AssertNoPendingException();
  Runtime* runtime = Runtime::Current();
  Handle<mirror::ArtMethod> h_method(hs.NewHandle(method));
  if (runtime->GetJit()->GetCodeCache()->ContainsMethod(method)) {
    VLOG(jit) << "Already compiled " << PrettyMethod(method);
    return true;  // Already compiled
  }
  Handle<mirror::Class> h_class(hs.NewHandle(h_method->GetDeclaringClass()));
  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    VLOG(jit) << "JIT failed to initialize " << PrettyMethod(h_method.Get());
    return false;
  }
  const DexFile* dex_file = h_class->GetDexCache()->GetDexFile();
  MethodReference method_ref(dex_file, h_method->GetDexMethodIndex());
  // Only verify if we don't already have verification results.
  if (verification_results_->GetVerifiedMethod(method_ref) == nullptr) {
    std::string error;
    if (verifier::MethodVerifier::VerifyMethod(h_method.Get(), true, &error) ==
        verifier::MethodVerifier::kHardFailure) {
      VLOG(jit) << "Not compile method " << PrettyMethod(h_method.Get())
          << " due to verification failure " << error;
      return false;
    }
  }
  CompiledMethod* compiled_method(compiler_driver_->CompileMethod(self, h_method.Get()));
  if (compiled_method == nullptr) {
    return false;
  }
  total_time_ += NanoTime() - start_time;
  // Don't add the method if we are supposed to be deoptimized.
  bool result = false;
  if (!runtime->GetInstrumentation()->AreAllMethodsDeoptimized()) {
    const void* code = Runtime::Current()->GetClassLinker()->GetOatMethodQuickCodeFor(
        h_method.Get());
    if (code != nullptr) {
      // Already have some compiled code, just use this instead of linking.
      // TODO: Fix recompilation.
      h_method->SetEntryPointFromQuickCompiledCode(code);
      result = true;
    } else {
      result = MakeExecutable(compiled_method, h_method.Get());
    }
  }
  // Remove the compiled method to save memory.
  compiler_driver_->RemoveCompiledMethod(method_ref);
  return result;
}

CompilerCallbacks* JitCompiler::GetCompilerCallbacks() const {
  return callbacks_.get();
}

uint8_t* JitCompiler::WriteMethodHeaderAndCode(const CompiledMethod* compiled_method,
                                               uint8_t* reserve_begin, uint8_t* reserve_end,
                                               const uint8_t* mapping_table,
                                               const uint8_t* vmap_table,
                                               const uint8_t* gc_map) {
  reserve_begin += sizeof(OatQuickMethodHeader);
  reserve_begin = reinterpret_cast<uint8_t*>(
      compiled_method->AlignCode(reinterpret_cast<uintptr_t>(reserve_begin)));
  const auto* quick_code = compiled_method->GetQuickCode();
  CHECK_LE(reserve_begin, reserve_end);
  CHECK_LE(quick_code->size(), static_cast<size_t>(reserve_end - reserve_begin));
  auto* code_ptr = reserve_begin;
  OatQuickMethodHeader* method_header = reinterpret_cast<OatQuickMethodHeader*>(code_ptr) - 1;
  // Construct the header last.
  const auto frame_size_in_bytes = compiled_method->GetFrameSizeInBytes();
  const auto core_spill_mask = compiled_method->GetCoreSpillMask();
  const auto fp_spill_mask = compiled_method->GetFpSpillMask();
  const auto code_size = quick_code->size();
  CHECK_NE(code_size, 0U);
  std::copy(quick_code->data(), quick_code->data() + code_size, code_ptr);
  // After we are done writing we need to update the method header.
  // Write out the method header last.
  method_header = new(method_header)OatQuickMethodHeader(
      code_ptr - mapping_table, code_ptr - vmap_table, code_ptr - gc_map, frame_size_in_bytes,
      core_spill_mask, fp_spill_mask, code_size);
  // Return the code ptr.
  return code_ptr;
}

bool JitCompiler::AddToCodeCache(mirror::ArtMethod* method, const CompiledMethod* compiled_method,
                                 OatFile::OatMethod* out_method) {
  Runtime* runtime = Runtime::Current();
  JitCodeCache* const code_cache = runtime->GetJit()->GetCodeCache();
  const auto* quick_code = compiled_method->GetQuickCode();
  if (quick_code == nullptr) {
    return false;
  }
  const auto code_size = quick_code->size();
  Thread* const self = Thread::Current();
  const uint8_t* base = code_cache->CodeCachePtr();
  auto* const mapping_table = compiled_method->GetMappingTable();
  auto* const vmap_table = compiled_method->GetVmapTable();
  auto* const gc_map = compiled_method->GetGcMap();
  // Write out pre-header stuff.
  uint8_t* const mapping_table_ptr = code_cache->AddDataArray(
      self, mapping_table->data(), mapping_table->data() + mapping_table->size());
  if (mapping_table == nullptr) {
    return false;  // Out of data cache.
  }
  uint8_t* const vmap_table_ptr = code_cache->AddDataArray(
      self, vmap_table->data(), vmap_table->data() + vmap_table->size());
  if (vmap_table == nullptr) {
    return false;  // Out of data cache.
  }
  uint8_t* const gc_map_ptr = code_cache->AddDataArray(
      self, gc_map->data(), gc_map->data() + gc_map->size());
  if (gc_map == nullptr) {
    return false;  // Out of data cache.
  }
  // Don't touch this until you protect / unprotect the code.
  const size_t reserve_size = sizeof(OatQuickMethodHeader) + quick_code->size() + 32;
  uint8_t* const code_reserve = code_cache->ReserveCode(self, reserve_size);
  if (code_reserve == nullptr) {
    return false;
  }
  auto* code_ptr = WriteMethodHeaderAndCode(
      compiled_method, code_reserve, code_reserve + reserve_size, mapping_table_ptr,
      vmap_table_ptr, gc_map_ptr);

  const size_t thumb_offset = compiled_method->CodeDelta();
  const uint32_t code_offset = code_ptr - base + thumb_offset;
  *out_method = OatFile::OatMethod(base, code_offset);
  DCHECK_EQ(out_method->GetGcMap(), gc_map_ptr);
  DCHECK_EQ(out_method->GetMappingTable(), mapping_table_ptr);
  DCHECK_EQ(out_method->GetVmapTable(), vmap_table_ptr);
  DCHECK_EQ(out_method->GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
  DCHECK_EQ(out_method->GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
  DCHECK_EQ(out_method->GetFpSpillMask(), compiled_method->GetFpSpillMask());
  VLOG(jit)  << "JIT added " << PrettyMethod(method) << "@" << method << " ccache_size="
      << PrettySize(code_cache->CodeCacheSize()) << ": " << reinterpret_cast<void*>(code_ptr)
      << "," << reinterpret_cast<void*>(code_ptr + code_size);
  return true;
}

bool JitCompiler::MakeExecutable(CompiledMethod* compiled_method, mirror::ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(compiled_method != nullptr);
  OatFile::OatMethod oat_method(nullptr, 0);
  if (!AddToCodeCache(method, compiled_method, &oat_method)) {
    return false;
  }
  // TODO: Flush instruction cache.
  oat_method.LinkMethod(method);
  CHECK(Runtime::Current()->GetJit()->GetCodeCache()->ContainsMethod(method))
      << PrettyMethod(method);
  return true;
}

}  // namespace jit
}  // namespace art
