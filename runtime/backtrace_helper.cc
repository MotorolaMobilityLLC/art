/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "backtrace_helper.h"

#if defined(__linux__)

#include <sys/types.h>
#include <unistd.h>
#include <iomanip>

#include "unwindstack/Regs.h"
#include "unwindstack/RegsGetLocal.h"
#include "unwindstack/Memory.h"
#include "unwindstack/Unwinder.h"

#include "base/bit_utils.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "thread-inl.h"

#else

// For UNUSED
#include "base/macros.h"

#endif

namespace art {

// We only really support libunwindstack on linux which is unfortunate but since this is only for
// gcstress this isn't a huge deal.
#if defined(__linux__)

// Strict integrity check of the backtrace:
// All methods must have a name, all the way to "main".
static constexpr bool kStrictUnwindChecks = false;

struct UnwindHelper : public TLSData {
  static constexpr const char* kTlsKey = "UnwindHelper::kTlsKey";

  explicit UnwindHelper(size_t max_depth)
      : arch_(unwindstack::Regs::CurrentArch()),
        memory_(unwindstack::Memory::CreateProcessMemoryThreadCached(getpid())),
        jit_(unwindstack::CreateJitDebug(arch_, memory_)),
        dex_(unwindstack::CreateDexFiles(arch_, memory_)),
        unwinder_(max_depth, &maps_, memory_) {
    CHECK(maps_.Parse());
    unwinder_.SetArch(arch_);
    unwinder_.SetJitDebug(jit_.get());
    unwinder_.SetDexFiles(dex_.get());
    unwinder_.SetResolveNames(kStrictUnwindChecks);
    unwindstack::Elf::SetCachingEnabled(true);
  }

  // Reparse process mmaps to detect newly loaded libraries.
  bool Reparse() { return maps_.Reparse(); }

  static UnwindHelper* Get(Thread* self, size_t max_depth) {
    UnwindHelper* tls = reinterpret_cast<UnwindHelper*>(self->GetCustomTLS(kTlsKey));
    if (tls == nullptr) {
      tls = new UnwindHelper(max_depth);
      self->SetCustomTLS(kTlsKey, tls);
    }
    return tls;
  }

  unwindstack::Unwinder* Unwinder() { return &unwinder_; }

 private:
  unwindstack::LocalUpdatableMaps maps_;
  unwindstack::ArchEnum arch_;
  std::shared_ptr<unwindstack::Memory> memory_;
  std::unique_ptr<unwindstack::JitDebug> jit_;
  std::unique_ptr<unwindstack::DexFiles> dex_;
  unwindstack::Unwinder unwinder_;
};

void BacktraceCollector::Collect() {
  unwindstack::Unwinder* unwinder = UnwindHelper::Get(Thread::Current(), max_depth_)->Unwinder();
  if (!CollectImpl(unwinder)) {
    // Reparse process mmaps to detect newly loaded libraries and retry.
    UnwindHelper::Get(Thread::Current(), max_depth_)->Reparse();
    if (!CollectImpl(unwinder)) {
      if (kStrictUnwindChecks) {
        LOG(ERROR) << "Failed to unwind stack:";
        std::vector<unwindstack::FrameData>& frames = unwinder->frames();
        for (auto it = frames.begin(); it != frames.end(); it++) {
          if (it == frames.begin() || std::prev(it)->map_name != it->map_name) {
            LOG(ERROR) << "  map_name  " << it->map_name.c_str();
          }
          LOG(ERROR) << "  " << std::setw(8) << std::setfill('0') << std::hex <<
            it->rel_pc << "  " << it->function_name.c_str();
        }
        LOG(FATAL);
      }
    }
  }
}

bool BacktraceCollector::CollectImpl(unwindstack::Unwinder* unwinder) {
  std::unique_ptr<unwindstack::Regs> regs(unwindstack::Regs::CreateFromLocal());
  RegsGetLocal(regs.get());
  unwinder->SetRegs(regs.get());
  unwinder->Unwind();

  num_frames_ = 0;
  if (unwinder->NumFrames() > skip_count_) {
    for (auto it = unwinder->frames().begin() + skip_count_; it != unwinder->frames().end(); ++it) {
      CHECK_LT(num_frames_, max_depth_);
      out_frames_[num_frames_++] = static_cast<uintptr_t>(it->pc);

      // Expected early end: Instrumentation breaks unwinding (b/138296821).
      size_t align = GetInstructionSetAlignment(kRuntimeISA);
      if (RoundUp(it->pc, align) == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc())) {
        return true;
      }

      if (kStrictUnwindChecks) {
        if (it->function_name.empty()) {
          return false;
        } else if (it->function_name == "main" || it->function_name == "start_thread") {
          return true;
        }
      }
    }
  }

  if (unwinder->LastErrorCode() == unwindstack::ERROR_INVALID_MAP) {
    return false;
  }
  if (kStrictUnwindChecks) {
    // We have not found "main". That is ok if we stopped the backtrace early.
    return unwinder->NumFrames() == max_depth_;
  }

  return true;
}

#else

#pragma clang diagnostic push
#pragma clang diagnostic warning "-W#warnings"
#warning "Backtrace collector is not implemented. GCStress cannot be used."
#pragma clang diagnostic pop

// We only have an implementation for linux. On other plaforms just return nothing. This is not
// really correct but we only use this for hashing and gcstress so it's not too big a deal.
void BacktraceCollector::Collect() {
  UNUSED(skip_count_);
  UNUSED(out_frames_);
  UNUSED(max_depth_);
  num_frames_ = 0;
}

#endif

}  // namespace art
