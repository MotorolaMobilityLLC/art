/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "optimizing_compiler.h"

#include <fstream>
#include <stdint.h>

#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/timing_logger.h"
#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "compiler.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "elf_writer_quick.h"
#include "graph_visualizer.h"
#include "gvn.h"
#include "inliner.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "licm.h"
#include "jni/quick/jni_compiler.h"
#include "mirror/art_method-inl.h"
#include "nodes.h"
#include "prepare_for_register_allocation.h"
#include "register_allocator.h"
#include "side_effects_analysis.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "ssa_liveness_analysis.h"
#include "reference_type_propagation.h"

namespace art {

/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator FINAL : public CodeAllocator {
 public:
  CodeVectorAllocator() {}

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.resize(size);
    return &memory_[0];
  }

  size_t GetSize() const { return size_; }
  const std::vector<uint8_t>& GetMemory() const { return memory_; }

 private:
  std::vector<uint8_t> memory_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};

/**
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be dumped.
 */
static const char* kStringFilter = "";

class PassInfo;

class PassInfoPrinter : public ValueObject {
 public:
  PassInfoPrinter(HGraph* graph,
                  const char* method_name,
                  const CodeGenerator& codegen,
                  std::ostream* visualizer_output,
                  CompilerDriver* compiler_driver)
      : method_name_(method_name),
        timing_logger_enabled_(compiler_driver->GetDumpPasses()),
        timing_logger_(method_name, true, true),
        visualizer_enabled_(!compiler_driver->GetDumpCfgFileName().empty()),
        visualizer_(visualizer_output, graph, codegen, method_name_) {
    if (strstr(method_name, kStringFilter) == nullptr) {
      timing_logger_enabled_ = visualizer_enabled_ = false;
    }
  }

  ~PassInfoPrinter() {
    if (timing_logger_enabled_) {
      LOG(INFO) << "TIMINGS " << method_name_;
      LOG(INFO) << Dumpable<TimingLogger>(timing_logger_);
    }
  }

 private:
  void StartPass(const char* pass_name) {
    // Dump graph first, then start timer.
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ false);
    }
    if (timing_logger_enabled_) {
      timing_logger_.StartTiming(pass_name);
    }
  }

  void EndPass(const char* pass_name) {
    // Pause timer first, then dump graph.
    if (timing_logger_enabled_) {
      timing_logger_.EndTiming();
    }
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ true);
    }
  }

  const char* method_name_;

  bool timing_logger_enabled_;
  TimingLogger timing_logger_;

  bool visualizer_enabled_;
  HGraphVisualizer visualizer_;

  friend PassInfo;

  DISALLOW_COPY_AND_ASSIGN(PassInfoPrinter);
};

class PassInfo : public ValueObject {
 public:
  PassInfo(const char *pass_name, PassInfoPrinter* pass_info_printer)
      : pass_name_(pass_name),
        pass_info_printer_(pass_info_printer) {
    pass_info_printer_->StartPass(pass_name_);
  }

  ~PassInfo() {
    pass_info_printer_->EndPass(pass_name_);
  }

 private:
  const char* const pass_name_;
  PassInfoPrinter* const pass_info_printer_;
};

class OptimizingCompiler FINAL : public Compiler {
 public:
  explicit OptimizingCompiler(CompilerDriver* driver);
  ~OptimizingCompiler();

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu) const
      OVERRIDE;

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file) const OVERRIDE;

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE;

  uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteElf(art::File* file,
                OatWriter* oat_writer,
                const std::vector<const art::DexFile*>& dex_files,
                const std::string& android_root,
                bool is_host) const OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void InitCompilationUnit(CompilationUnit& cu ATTRIBUTE_UNUSED) const OVERRIDE {}

  void Init() OVERRIDE;

  void UnInit() const OVERRIDE {}

 private:
  // Whether we should run any optimization or register allocation. If false, will
  // just run the code generation after the graph was built.
  const bool run_optimizations_;

  // Optimize and compile `graph`.
  CompiledMethod* CompileOptimized(HGraph* graph,
                                   CodeGenerator* codegen,
                                   CompilerDriver* driver,
                                   const DexFile& dex_file,
                                   const DexCompilationUnit& dex_compilation_unit,
                                   PassInfoPrinter* pass_info) const;

  // Just compile without doing optimizations.
  CompiledMethod* CompileBaseline(CodeGenerator* codegen,
                                  CompilerDriver* driver,
                                  const DexCompilationUnit& dex_compilation_unit) const;

  mutable OptimizingCompilerStats compilation_stats_;

  std::unique_ptr<std::ostream> visualizer_output_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver)
    : Compiler(driver, kMaximumCompilationTimeBeforeWarning),
      run_optimizations_(
          driver->GetCompilerOptions().GetCompilerFilter() != CompilerOptions::kTime),
      compilation_stats_() {}

void OptimizingCompiler::Init() {
  // Enable C1visualizer output. Must be done in Init() because the compiler
  // driver is not fully initialized when passed to the compiler's constructor.
  CompilerDriver* driver = GetCompilerDriver();
  const std::string cfg_file_name = driver->GetDumpCfgFileName();
  if (!cfg_file_name.empty()) {
    CHECK_EQ(driver->GetThreadCount(), 1U)
      << "Graph visualizer requires the compiler to run single-threaded. "
      << "Invoke the compiler with '-j1'.";
    visualizer_output_.reset(new std::ofstream(cfg_file_name));
  }
}

OptimizingCompiler::~OptimizingCompiler() {
  compilation_stats_.Log();
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx ATTRIBUTE_UNUSED,
                                          const DexFile& dex_file ATTRIBUTE_UNUSED,
                                          CompilationUnit* cu ATTRIBUTE_UNUSED) const {
  return true;
}

CompiledMethod* OptimizingCompiler::JniCompile(uint32_t access_flags,
                                               uint32_t method_idx,
                                               const DexFile& dex_file) const {
  return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
}

uintptr_t OptimizingCompiler::GetEntryPointOf(mirror::ArtMethod* method) const {
  return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
      InstructionSetPointerSize(GetCompilerDriver()->GetInstructionSet())));
}

bool OptimizingCompiler::WriteElf(art::File* file, OatWriter* oat_writer,
                                  const std::vector<const art::DexFile*>& dex_files,
                                  const std::string& android_root, bool is_host) const {
  return art::ElfWriterQuick32::Create(file, oat_writer, dex_files, android_root, is_host,
                                       *GetCompilerDriver());
}

static bool IsInstructionSetSupported(InstructionSet instruction_set) {
  return instruction_set == kArm64
      || (instruction_set == kThumb2 && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kX86
      || instruction_set == kX86_64;
}

static bool CanOptimize(const DexFile::CodeItem& code_item) {
  // TODO: We currently cannot optimize methods with try/catch.
  return code_item.tries_size_ == 0;
}

static void RunOptimizations(HOptimization* optimizations[],
                             size_t length,
                             PassInfoPrinter* pass_info_printer) {
  for (size_t i = 0; i < length; ++i) {
    HOptimization* optimization = optimizations[i];
    {
      PassInfo pass_info(optimization->GetPassName(), pass_info_printer);
      optimization->Run();
    }
    optimization->Check();
  }
}

static void RunOptimizations(HGraph* graph,
                             CompilerDriver* driver,
                             OptimizingCompilerStats* stats,
                             const DexFile& dex_file,
                             const DexCompilationUnit& dex_compilation_unit,
                             PassInfoPrinter* pass_info_printer,
                             StackHandleScopeCollection* handles) {
  SsaRedundantPhiElimination redundant_phi(graph);
  SsaDeadPhiElimination dead_phi(graph);
  HDeadCodeElimination dce(graph);
  HConstantFolding fold1(graph);
  InstructionSimplifier simplify1(graph, stats);

  HInliner inliner(graph, dex_compilation_unit, driver, stats);

  HConstantFolding fold2(graph);
  SideEffectsAnalysis side_effects(graph);
  GVNOptimization gvn(graph, side_effects);
  LICM licm(graph, side_effects);
  BoundsCheckElimination bce(graph);
  ReferenceTypePropagation type_propagation(graph, dex_file, dex_compilation_unit, handles);
  InstructionSimplifier simplify2(graph, stats, "instruction_simplifier_after_types");

  IntrinsicsRecognizer intrinsics(graph, dex_compilation_unit.GetDexFile(), driver);

  HOptimization* optimizations[] = {
    &redundant_phi,
    &dead_phi,
    &intrinsics,
    &dce,
    &fold1,
    &simplify1,
    &inliner,
    &fold2,
    &side_effects,
    &gvn,
    &licm,
    &bce,
    &type_propagation,
    &simplify2
  };

  RunOptimizations(optimizations, arraysize(optimizations), pass_info_printer);
}

// The stack map we generate must be 4-byte aligned on ARM. Since existing
// maps are generated alongside these stack maps, we must also align them.
static ArrayRef<const uint8_t> AlignVectorSize(std::vector<uint8_t>& vector) {
  size_t size = vector.size();
  size_t aligned_size = RoundUp(size, 4);
  for (; size < aligned_size; ++size) {
    vector.push_back(0);
  }
  return ArrayRef<const uint8_t>(vector);
}


CompiledMethod* OptimizingCompiler::CompileOptimized(HGraph* graph,
                                                     CodeGenerator* codegen,
                                                     CompilerDriver* compiler_driver,
                                                     const DexFile& dex_file,
                                                     const DexCompilationUnit& dex_compilation_unit,
                                                     PassInfoPrinter* pass_info_printer) const {
  StackHandleScopeCollection handles(Thread::Current());
  RunOptimizations(graph, compiler_driver, &compilation_stats_,
                   dex_file, dex_compilation_unit, pass_info_printer, &handles);

  PrepareForRegisterAllocation(graph).Run();
  SsaLivenessAnalysis liveness(*graph, codegen);
  {
    PassInfo pass_info(kLivenessPassName, pass_info_printer);
    liveness.Analyze();
  }
  {
    PassInfo pass_info(kRegisterAllocatorPassName, pass_info_printer);
    RegisterAllocator(graph->GetArena(), codegen, liveness).AllocateRegisters();
  }

  CodeVectorAllocator allocator;
  codegen->CompileOptimized(&allocator);

  std::vector<uint8_t> stack_map;
  codegen->BuildStackMaps(&stack_map);

  compilation_stats_.RecordStat(MethodCompilationStat::kCompiledOptimized);

  return CompiledMethod::SwapAllocCompiledMethodStackMap(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(allocator.GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      ArrayRef<const uint8_t>(stack_map));
}


CompiledMethod* OptimizingCompiler::CompileBaseline(
    CodeGenerator* codegen,
    CompilerDriver* compiler_driver,
    const DexCompilationUnit& dex_compilation_unit) const {
  CodeVectorAllocator allocator;
  codegen->CompileBaseline(&allocator);

  std::vector<uint8_t> mapping_table;
  DefaultSrcMap src_mapping_table;
  bool include_debug_symbol = compiler_driver->GetCompilerOptions().GetIncludeDebugSymbols();
  codegen->BuildMappingTable(&mapping_table, include_debug_symbol ? &src_mapping_table : nullptr);
  std::vector<uint8_t> vmap_table;
  codegen->BuildVMapTable(&vmap_table);
  std::vector<uint8_t> gc_map;
  codegen->BuildNativeGCMap(&gc_map, dex_compilation_unit);

  compilation_stats_.RecordStat(MethodCompilationStat::kCompiledBaseline);
  return CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(allocator.GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      &src_mapping_table,
      AlignVectorSize(mapping_table),
      AlignVectorSize(vmap_table),
      AlignVectorSize(gc_map),
      ArrayRef<const uint8_t>());
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject class_loader,
                                            const DexFile& dex_file) const {
  UNUSED(invoke_type);
  std::string method_name = PrettyMethod(method_idx, dex_file);
  compilation_stats_.RecordStat(MethodCompilationStat::kAttemptCompilation);
  CompilerDriver* compiler_driver = GetCompilerDriver();
  InstructionSet instruction_set = compiler_driver->GetInstructionSet();
  // Always use the thumb2 assembler: some runtime functionality (like implicit stack
  // overflow checks) assume thumb2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    compilation_stats_.RecordStat(MethodCompilationStat::kNotCompiledUnsupportedIsa);
    return nullptr;
  }

  if (Compiler::IsPathologicalCase(*code_item, method_idx, dex_file)) {
    compilation_stats_.RecordStat(MethodCompilationStat::kNotCompiledPathological);
    return nullptr;
  }

  DexCompilationUnit dex_compilation_unit(
    nullptr, class_loader, art::Runtime::Current()->GetClassLinker(), dex_file, code_item,
    class_def_idx, method_idx, access_flags,
    compiler_driver->GetVerifiedMethod(&dex_file, method_idx));

  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraph* graph = new (&arena) HGraph(&arena);

  // For testing purposes, we put a special marker on method names that should be compiled
  // with this compiler. This makes sure we're not regressing.
  bool shouldCompile = method_name.find("$opt$") != std::string::npos;
  bool shouldOptimize = method_name.find("$opt$reg$") != std::string::npos;

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            instruction_set,
                            *compiler_driver->GetInstructionSetFeatures(),
                            compiler_driver->GetCompilerOptions()));
  if (codegen.get() == nullptr) {
    CHECK(!shouldCompile) << "Could not find code generator for optimizing compiler";
    compilation_stats_.RecordStat(MethodCompilationStat::kNotCompiledNoCodegen);
    return nullptr;
  }

  PassInfoPrinter pass_info_printer(graph,
                                    method_name.c_str(),
                                    *codegen.get(),
                                    visualizer_output_.get(),
                                    compiler_driver);

  HGraphBuilder builder(graph,
                        &dex_compilation_unit,
                        &dex_compilation_unit,
                        &dex_file,
                        compiler_driver,
                        &compilation_stats_);

  VLOG(compiler) << "Building " << method_name;

  {
    PassInfo pass_info(kBuilderPassName, &pass_info_printer);
    if (!builder.BuildGraph(*code_item)) {
      CHECK(!shouldCompile) << "Could not build graph in optimizing compiler";
      return nullptr;
    }
  }

  bool can_optimize = CanOptimize(*code_item);
  bool can_allocate_registers = RegisterAllocator::CanAllocateRegistersFor(*graph, instruction_set);
  if (run_optimizations_ && can_optimize && can_allocate_registers) {
    VLOG(compiler) << "Optimizing " << method_name;

    {
      PassInfo pass_info(kSsaBuilderPassName, &pass_info_printer);
      if (!graph->TryBuildingSsa()) {
        // We could not transform the graph to SSA, bailout.
        LOG(INFO) << "Skipping compilation of " << method_name << ": it contains a non natural loop";
        compilation_stats_.RecordStat(MethodCompilationStat::kNotCompiledCannotBuildSSA);
        return nullptr;
      }
    }

    return CompileOptimized(graph,
                            codegen.get(),
                            compiler_driver,
                            dex_file,
                            dex_compilation_unit,
                            &pass_info_printer);
  } else if (shouldOptimize && RegisterAllocator::Supports(instruction_set)) {
    LOG(FATAL) << "Could not allocate registers in optimizing compiler";
    UNREACHABLE();
  } else {
    VLOG(compiler) << "Compile baseline " << method_name;

    if (!run_optimizations_) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedDisabled);
    } else if (!can_optimize) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedTryCatch);
    } else if (!can_allocate_registers) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedRegisterAllocator);
    }

    return CompileBaseline(codegen.get(), compiler_driver, dex_compilation_unit);
  }
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

}  // namespace art
