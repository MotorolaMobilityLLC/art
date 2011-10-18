// Copyright 2011 Google Inc. All Rights Reserved.

#include "context_arm.h"

#include "object.h"
#include "runtime_support.h"

namespace art {
namespace arm {

ArmContext::ArmContext() {
#ifndef NDEBUG
  // Initialize registers with easy to spot debug values
  for (int i = 0; i < 16; i++) {
    gprs_[i] = 0xEBAD6070+i;
  }
  for (int i = 0; i < 32; i++) {
    fprs_[i] = 0xEBAD8070+i;
  }
#endif
}

void ArmContext::FillCalleeSaves(const Frame& fr) {
  Method* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  uint32_t fp_core_spills = method->GetFpSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  size_t fp_spill_count = __builtin_popcount(fp_core_spills);
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context
    int j = 1;
    for (int i = 0; i < 16; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.LoadCalleeSave(spill_count - j);
        j++;
      }
    }
  }
  if (fp_spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context
    int j = 1;
    for (int i = 0; i < 32; i++) {
      if (((fp_core_spills >> i) & 1) != 0) {
        fprs_[i] = fr.LoadCalleeSave(spill_count + fp_spill_count - j);
        j++;
      }
    }
  }
}

void ArmContext::DoLongJump() {
#if defined(__arm__)
  art_do_long_jump(&gprs_[0], &fprs_[S0]);
#else
  UNIMPLEMENTED(FATAL);
#endif
}

}  // namespace arm
}  // namespace art
