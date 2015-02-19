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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_

#include <sstream>
#include <string>

#include "atomic.h"

namespace art {

enum MethodCompilationStat {
  kAttemptCompilation = 0,
  kCompiledBaseline,
  kCompiledOptimized,
  kInlinedInvoke,
  kNotCompiledUnsupportedIsa,
  kNotCompiledPathological,
  kNotCompiledHugeMethod,
  kNotCompiledLargeMethodNoBranches,
  kNotCompiledCannotBuildSSA,
  kNotCompiledNoCodegen,
  kNotCompiledUnresolvedMethod,
  kNotCompiledUnresolvedField,
  kNotCompiledNonSequentialRegPair,
  kNotOptimizedTryCatch,
  kNotOptimizedDisabled,
  kNotCompiledCantAccesType,
  kNotOptimizedRegisterAllocator,
  kNotCompiledUnhandledInstruction,
  kRemovedCheckedCast,
  kRemovedNullCheck,
  kLastStat
};

class OptimizingCompilerStats {
 public:
  OptimizingCompilerStats() {}

  void RecordStat(MethodCompilationStat stat) {
    compile_stats_[stat]++;
  }

  void Log() const {
    if (compile_stats_[kAttemptCompilation] == 0) {
      LOG(INFO) << "Did not compile any method.";
    } else {
      size_t unoptimized_percent =
          compile_stats_[kCompiledBaseline] * 100 / compile_stats_[kAttemptCompilation];
      size_t optimized_percent =
          compile_stats_[kCompiledOptimized] * 100 / compile_stats_[kAttemptCompilation];
      std::ostringstream oss;
      oss << "Attempted compilation of " << compile_stats_[kAttemptCompilation] << " methods: "
          << unoptimized_percent << "% (" << compile_stats_[kCompiledBaseline] << ") unoptimized, "
          << optimized_percent << "% (" << compile_stats_[kCompiledOptimized] << ") optimized.";
      for (int i = 0; i < kLastStat; i++) {
        if (compile_stats_[i] != 0) {
          oss << "\n" << PrintMethodCompilationStat(i) << ": " << compile_stats_[i];
        }
      }
      LOG(INFO) << oss.str();
    }
  }

 private:
  std::string PrintMethodCompilationStat(int stat) const {
    switch (stat) {
      case kAttemptCompilation : return "kAttemptCompilation";
      case kCompiledBaseline : return "kCompiledBaseline";
      case kCompiledOptimized : return "kCompiledOptimized";
      case kInlinedInvoke : return "kInlinedInvoke";
      case kNotCompiledUnsupportedIsa : return "kNotCompiledUnsupportedIsa";
      case kNotCompiledPathological : return "kNotCompiledPathological";
      case kNotCompiledHugeMethod : return "kNotCompiledHugeMethod";
      case kNotCompiledLargeMethodNoBranches : return "kNotCompiledLargeMethodNoBranches";
      case kNotCompiledCannotBuildSSA : return "kNotCompiledCannotBuildSSA";
      case kNotCompiledNoCodegen : return "kNotCompiledNoCodegen";
      case kNotCompiledUnresolvedMethod : return "kNotCompiledUnresolvedMethod";
      case kNotCompiledUnresolvedField : return "kNotCompiledUnresolvedField";
      case kNotCompiledNonSequentialRegPair : return "kNotCompiledNonSequentialRegPair";
      case kNotOptimizedDisabled : return "kNotOptimizedDisabled";
      case kNotOptimizedTryCatch : return "kNotOptimizedTryCatch";
      case kNotCompiledCantAccesType : return "kNotCompiledCantAccesType";
      case kNotOptimizedRegisterAllocator : return "kNotOptimizedRegisterAllocator";
      case kNotCompiledUnhandledInstruction : return "kNotCompiledUnhandledInstruction";
      case kRemovedCheckedCast: return "kRemovedCheckedCast";
      case kRemovedNullCheck: return "kRemovedNullCheck";
      default: LOG(FATAL) << "invalid stat";
    }
    return "";
  }

  AtomicInteger compile_stats_[kLastStat];

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompilerStats);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_
