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
#include "jni/quick/jni_compiler.h"
#include "mirror/art_method-inl.h"
#include "nodes.h"
#include "prepare_for_register_allocation.h"
#include "register_allocator.h"
#include "side_effects_analysis.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "ssa_liveness_analysis.h"
#include "utils/arena_allocator.h"

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
                                   const DexCompilationUnit& dex_compilation_unit,
                                   const HGraphVisualizer& visualizer) const;

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

static void RunOptimizations(HGraph* graph,
                             CompilerDriver* driver,
                             OptimizingCompilerStats* stats,
                             const DexCompilationUnit& dex_compilation_unit,
                             const HGraphVisualizer& visualizer) {
  SsaRedundantPhiElimination redundant_phi(graph);
  SsaDeadPhiElimination dead_phi(graph);
  HDeadCodeElimination dce(graph);
  HConstantFolding fold1(graph);
  InstructionSimplifier simplify1(graph);

  HInliner inliner(graph, dex_compilation_unit, driver, stats);

  HConstantFolding fold2(graph);
  SideEffectsAnalysis side_effects(graph);
  GVNOptimization gvn(graph, side_effects);
  BoundsCheckElimination bce(graph);
  InstructionSimplifier simplify2(graph);

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
    &bce,
    &simplify2
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    visualizer.DumpGraph(optimization->GetPassName(), /*is_after=*/false);
    optimization->Run();
    visualizer.DumpGraph(optimization->GetPassName(), /*is_after=*/true);
    optimization->Check();
  }
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
                                                     const DexCompilationUnit& dex_compilation_unit,
                                                     const HGraphVisualizer& visualizer) const {
  RunOptimizations(
      graph, compiler_driver, &compilation_stats_, dex_compilation_unit, visualizer);

  PrepareForRegisterAllocation(graph).Run();
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();
  visualizer.DumpGraph(kLivenessPassName);

  RegisterAllocator register_allocator(graph->GetArena(), codegen, liveness);
  register_allocator.AllocateRegisters();
  visualizer.DumpGraph(kRegisterAllocatorPassName);

  CodeVectorAllocator allocator;
  codegen->CompileOptimized(&allocator);

  std::vector<uint8_t> stack_map;
  codegen->BuildStackMaps(&stack_map);

  compilation_stats_.RecordStat(MethodCompilationStat::kCompiledOptimized);

  return CompiledMethod::SwapAllocCompiledMethodStackMap(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(allocator.GetMemory()),
      codegen->GetFrameSize(),
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
  return CompiledMethod::SwapAllocCompiledMethod(compiler_driver,
                                                 codegen->GetInstructionSet(),
                                                 ArrayRef<const uint8_t>(allocator.GetMemory()),
                                                 codegen->GetFrameSize(),
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

  std::string method_name = PrettyMethod(method_idx, dex_file);

  // For testing purposes, we put a special marker on method names that should be compiled
  // with this compiler. This makes sure we're not regressing.
  bool shouldCompile = method_name.find("$opt$") != std::string::npos;
  bool shouldOptimize = method_name.find("$opt$reg$") != std::string::npos;

  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena,
                        &dex_compilation_unit,
                        &dex_compilation_unit,
                        &dex_file,
                        compiler_driver,
                        &compilation_stats_);

  VLOG(compiler) << "Building " << PrettyMethod(method_idx, dex_file);
  HGraph* graph = builder.BuildGraph(*code_item);
  if (graph == nullptr) {
    CHECK(!shouldCompile) << "Could not build graph in optimizing compiler";
    return nullptr;
  }

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

  HGraphVisualizer visualizer(
      visualizer_output_.get(), graph, kStringFilter, *codegen.get(), method_name.c_str());
  visualizer.DumpGraph("builder");

  bool can_optimize = CanOptimize(*code_item);
  bool can_allocate_registers = RegisterAllocator::CanAllocateRegistersFor(*graph, instruction_set);
  CompiledMethod* result = nullptr;
  if (run_optimizations_ && can_optimize && can_allocate_registers) {
    VLOG(compiler) << "Optimizing " << PrettyMethod(method_idx, dex_file);
    if (!graph->TryBuildingSsa()) {
      LOG(INFO) << "Skipping compilation of "
                << PrettyMethod(method_idx, dex_file)
                << ": it contains a non natural loop";
      // We could not transform the graph to SSA, bailout.
      compilation_stats_.RecordStat(MethodCompilationStat::kNotCompiledCannotBuildSSA);
    } else {
      result = CompileOptimized(graph, codegen.get(), compiler_driver, dex_compilation_unit, visualizer);
    }
  } else if (shouldOptimize && RegisterAllocator::Supports(instruction_set)) {
    LOG(FATAL) << "Could not allocate registers in optimizing compiler";
    UNREACHABLE();
  } else {
    VLOG(compiler) << "Compile baseline " << PrettyMethod(method_idx, dex_file);

    if (!run_optimizations_) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedDisabled);
    } else if (!can_optimize) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedTryCatch);
    } else if (!can_allocate_registers) {
      compilation_stats_.RecordStat(MethodCompilationStat::kNotOptimizedRegisterAllocator);
    }

    result = CompileBaseline(codegen.get(), compiler_driver, dex_compilation_unit);
  }
  return result;
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

}  // namespace art
