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

#ifndef ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
#define ART_COMPILER_DRIVER_COMPILER_DRIVER_H_

#include <atomic>
#include <set>
#include <string>
#include <vector>

#include "android-base/strings.h"

#include "arch/instruction_set.h"
#include "base/array_ref.h"
#include "base/bit_utils.h"
#include "base/hash_set.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/quasi_atomic.h"
#include "base/safe_map.h"
#include "base/timing_logger.h"
#include "class_status.h"
#include "compiler.h"
#include "dex/class_reference.h"
#include "dex/dex_file.h"
#include "dex/dex_file_types.h"
#include "dex/dex_to_dex_compiler.h"
#include "dex/method_reference.h"
#include "driver/compiled_method_storage.h"
#include "thread_pool.h"
#include "utils/atomic_dex_ref_map.h"
#include "utils/dex_cache_arrays_layout.h"

namespace art {

namespace mirror {
class Class;
class DexCache;
}  // namespace mirror

namespace verifier {
class MethodVerifier;
class VerifierDepsTest;
}  // namespace verifier

class ArtField;
class BitVector;
class CompiledMethod;
class CompilerOptions;
class DexCompilationUnit;
template<class T> class Handle;
struct InlineIGetIPutData;
class InstructionSetFeatures;
class InternTable;
enum InvokeType : uint32_t;
class MemberOffset;
template<class MirrorType> class ObjPtr;
class ParallelCompilationManager;
class ProfileCompilationInfo;
class ScopedObjectAccess;
template <class Allocator> class SrcMap;
class TimingLogger;
class VdexFile;
class VerificationResults;
class VerifiedMethod;

enum EntryPointCallingConvention {
  // ABI of invocations to a method's interpreter entry point.
  kInterpreterAbi,
  // ABI of calls to a method's native code, only used for native methods.
  kJniAbi,
  // ABI of calls to a method's quick code entry point.
  kQuickAbi
};

class CompilerDriver {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with null implying all available
  // classes.
  CompilerDriver(const CompilerOptions* compiler_options,
                 VerificationResults* verification_results,
                 Compiler::Kind compiler_kind,
                 HashSet<std::string>* image_classes,
                 size_t thread_count,
                 int swap_fd,
                 const ProfileCompilationInfo* profile_compilation_info);

  ~CompilerDriver();

  // Set dex files classpath.
  void SetClasspathDexFiles(const std::vector<const DexFile*>& dex_files);

  void CompileAll(jobject class_loader,
                  const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  // Compile a single Method. (For testing only.)
  void CompileOne(Thread* self,
                  jobject class_loader,
                  const DexFile& dex_file,
                  uint16_t class_def_idx,
                  uint32_t method_idx,
                  uint32_t access_flags,
                  InvokeType invoke_type,
                  const DexFile::CodeItem* code_item,
                  Handle<mirror::DexCache> dex_cache,
                  Handle<mirror::ClassLoader> h_class_loader)
      REQUIRES(!Locks::mutator_lock_);

  VerificationResults* GetVerificationResults() const;

  const CompilerOptions& GetCompilerOptions() const {
    return *compiler_options_;
  }

  Compiler* GetCompiler() const {
    return compiler_.get();
  }

  // Generate the trampolines that are invoked by unresolved direct methods.
  std::unique_ptr<const std::vector<uint8_t>> CreateJniDlsymLookup() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickGenericJniTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickImtConflictTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickResolutionTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickToInterpreterBridge() const;

  ClassStatus GetClassStatus(const ClassReference& ref) const;
  bool GetCompiledClass(const ClassReference& ref, ClassStatus* status) const;

  CompiledMethod* GetCompiledMethod(MethodReference ref) const;
  // Add a compiled method.
  void AddCompiledMethod(const MethodReference& method_ref, CompiledMethod* const compiled_method);
  CompiledMethod* RemoveCompiledMethod(const MethodReference& method_ref);

  // Resolve compiling method's class. Returns null on failure.
  ObjPtr<mirror::Class> ResolveCompilingMethodsClass(const ScopedObjectAccess& soa,
                                                     Handle<mirror::DexCache> dex_cache,
                                                     Handle<mirror::ClassLoader> class_loader,
                                                     const DexCompilationUnit* mUnit)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::Class> ResolveClass(const ScopedObjectAccess& soa,
                                     Handle<mirror::DexCache> dex_cache,
                                     Handle<mirror::ClassLoader> class_loader,
                                     dex::TypeIndex type_index,
                                     const DexCompilationUnit* mUnit)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolve a field. Returns null on failure, including incompatible class change.
  // NOTE: Unlike ClassLinker's ResolveField(), this method enforces is_static.
  ArtField* ResolveField(const ScopedObjectAccess& soa,
                         Handle<mirror::DexCache> dex_cache,
                         Handle<mirror::ClassLoader> class_loader,
                         uint32_t field_idx,
                         bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can we fast-path an IGET/IPUT access to an instance field? If yes, compute the field offset.
  std::pair<bool, bool> IsFastInstanceField(ObjPtr<mirror::DexCache> dex_cache,
                                            ObjPtr<mirror::Class> referrer_class,
                                            ArtField* resolved_field,
                                            uint16_t field_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void ProcessedInstanceField(bool resolved);
  void ProcessedStaticField(bool resolved, bool local);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                                MemberOffset* field_offset, bool* is_volatile)
      REQUIRES(!Locks::mutator_lock_);

  ArtField* ComputeInstanceFieldInfo(uint32_t field_idx,
                                     const DexCompilationUnit* mUnit,
                                     bool is_put,
                                     const ScopedObjectAccess& soa)
      REQUIRES_SHARED(Locks::mutator_lock_);


  const VerifiedMethod* GetVerifiedMethod(const DexFile* dex_file, uint32_t method_idx) const;
  bool IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc);

  size_t GetThreadCount() const {
    return parallel_thread_count_;
  }

  void SetDedupeEnabled(bool dedupe_enabled) {
    compiled_method_storage_.SetDedupeEnabled(dedupe_enabled);
  }

  bool DedupeEnabled() const {
    return compiled_method_storage_.DedupeEnabled();
  }

  // Checks whether the provided class should be compiled, i.e., is in classes_to_compile_.
  bool IsClassToCompile(const char* descriptor) const;

  // Checks whether profile guided compilation is enabled and if the method should be compiled
  // according to the profile file.
  bool ShouldCompileBasedOnProfile(const MethodReference& method_ref) const;

  // Checks whether profile guided verification is enabled and if the method should be verified
  // according to the profile file.
  bool ShouldVerifyClassBasedOnProfile(const DexFile& dex_file, uint16_t class_idx) const;

  void RecordClassStatus(const ClassReference& ref, ClassStatus status);

  // Checks if the specified method has been verified without failures. Returns
  // false if the method is not in the verification results (GetVerificationResults).
  bool IsMethodVerifiedWithoutFailures(uint32_t method_idx,
                                       uint16_t class_def_idx,
                                       const DexFile& dex_file) const;

  // Get memory usage during compilation.
  std::string GetMemoryUsageString(bool extended) const;

  void SetHadHardVerifierFailure() {
    had_hard_verifier_failure_ = true;
  }
  void AddSoftVerifierFailure() {
    number_of_soft_verifier_failures_++;
  }

  Compiler::Kind GetCompilerKind() {
    return compiler_kind_;
  }

  CompiledMethodStorage* GetCompiledMethodStorage() {
    return &compiled_method_storage_;
  }

  const ProfileCompilationInfo* GetProfileCompilationInfo() const {
    return profile_compilation_info_;
  }

  // Is `boot_image_filename` the name of a core image (small boot
  // image used for ART testing only)?
  static bool IsCoreImageFilename(const std::string& boot_image_filename) {
    // Look for "core.art" or "core-*.art".
    if (android::base::EndsWith(boot_image_filename, "core.art")) {
      return true;
    }
    if (!android::base::EndsWith(boot_image_filename, ".art")) {
      return false;
    }
    size_t slash_pos = boot_image_filename.rfind('/');
    if (slash_pos == std::string::npos) {
      return android::base::StartsWith(boot_image_filename, "core-");
    }
    return boot_image_filename.compare(slash_pos + 1, 5u, "core-") == 0;
  }

  optimizer::DexToDexCompiler& GetDexToDexCompiler() {
    return dex_to_dex_compiler_;
  }

 private:
  void PreCompile(jobject class_loader,
                  const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void LoadImageClasses(TimingLogger* timings) REQUIRES(!Locks::mutator_lock_);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader,
               const std::vector<const DexFile*>& dex_files,
               TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader,
                      const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool,
                      size_t thread_count,
                      TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  // Do fast verification through VerifierDeps if possible. Return whether
  // verification was successful.
  bool FastVerify(jobject class_loader,
                  const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings);

  void Verify(jobject class_loader,
              const std::vector<const DexFile*>& dex_files,
              TimingLogger* timings);

  void VerifyDexFile(jobject class_loader,
                     const DexFile& dex_file,
                     const std::vector<const DexFile*>& dex_files,
                     ThreadPool* thread_pool,
                     size_t thread_count,
                     TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void SetVerified(jobject class_loader,
                   const std::vector<const DexFile*>& dex_files,
                   TimingLogger* timings);
  void SetVerifiedDexFile(jobject class_loader,
                          const DexFile& dex_file,
                          const std::vector<const DexFile*>& dex_files,
                          ThreadPool* thread_pool,
                          size_t thread_count,
                          TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader,
                         const std::vector<const DexFile*>& dex_files,
                         TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);
  void InitializeClasses(jobject class_loader,
                         const DexFile& dex_file,
                         const std::vector<const DexFile*>& dex_files,
                         TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void UpdateImageClasses(TimingLogger* timings) REQUIRES(!Locks::mutator_lock_);

  void Compile(jobject class_loader,
               const std::vector<const DexFile*>& dex_files,
               TimingLogger* timings);

  void InitializeThreadPools();
  void FreeThreadPools();
  void CheckThreadPools();

  // Resolve const string literals that are loaded from dex code. If only_startup_strings is
  // specified, only methods that are marked startup in the profile are resolved.
  void ResolveConstStrings(const std::vector<const DexFile*>& dex_files,
                           bool only_startup_strings,
                           /*inout*/ TimingLogger* timings);

  const CompilerOptions* const compiler_options_;
  VerificationResults* const verification_results_;

  std::unique_ptr<Compiler> compiler_;
  Compiler::Kind compiler_kind_;

  // All class references that this compiler has compiled. Indexed by class defs.
  using ClassStateTable = AtomicDexRefMap<ClassReference, ClassStatus>;
  ClassStateTable compiled_classes_;
  // All class references that are in the classpath. Indexed by class defs.
  ClassStateTable classpath_classes_;

  typedef AtomicDexRefMap<MethodReference, CompiledMethod*> MethodTable;

  // All method references that this compiler has compiled.
  MethodTable compiled_methods_;

  // Image classes to be updated by PreCompile().
  // TODO: Remove this member which is a non-const pointer to the CompilerOptions' data.
  //       Pass this explicitly to the PreCompile() which should be called directly from
  //       Dex2Oat rather than implicitly by CompileAll().
  HashSet<std::string>* image_classes_;

  // Specifies the classes that will be compiled. Note that if classes_to_compile_ is null,
  // all classes are eligible for compilation (duplication filters etc. will still apply).
  // This option may be restricted to the boot image, depending on a flag in the implementation.
  std::unique_ptr<HashSet<std::string>> classes_to_compile_;

  std::atomic<uint32_t> number_of_soft_verifier_failures_;

  bool had_hard_verifier_failure_;

  // A thread pool that can (potentially) run tasks in parallel.
  size_t parallel_thread_count_;
  std::unique_ptr<ThreadPool> parallel_thread_pool_;

  // A thread pool that guarantees running single-threaded on the main thread.
  std::unique_ptr<ThreadPool> single_thread_pool_;

  class AOTCompilationStats;
  std::unique_ptr<AOTCompilationStats> stats_;

  CompiledMethodStorage compiled_method_storage_;

  // Info for profile guided compilation.
  const ProfileCompilationInfo* const profile_compilation_info_;

  size_t max_arena_alloc_;

  // Compiler for dex to dex (quickening).
  optimizer::DexToDexCompiler dex_to_dex_compiler_;

  friend class CommonCompilerTest;
  friend class CompileClassVisitor;
  friend class DexToDexDecompilerTest;
  friend class verifier::VerifierDepsTest;
  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
