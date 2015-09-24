/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "induction_var_range.h"

#include <limits>

namespace art {

/** Returns true if 64-bit constant fits in 32-bit constant. */
static bool CanLongValueFitIntoInt(int64_t c) {
  return std::numeric_limits<int32_t>::min() <= c && c <= std::numeric_limits<int32_t>::max();
}

/** Returns true if 32-bit addition can be done safely. */
static bool IsSafeAdd(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) + static_cast<int64_t>(c2));
}

/** Returns true if 32-bit subtraction can be done safely. */
static bool IsSafeSub(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) - static_cast<int64_t>(c2));
}

/** Returns true if 32-bit multiplication can be done safely. */
static bool IsSafeMul(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) * static_cast<int64_t>(c2));
}

/** Returns true if 32-bit division can be done safely. */
static bool IsSafeDiv(int32_t c1, int32_t c2) {
  return c2 != 0 && CanLongValueFitIntoInt(static_cast<int64_t>(c1) / static_cast<int64_t>(c2));
}

/** Returns true for 32/64-bit integral constant. */
static bool IsIntAndGet(HInstruction* instruction, int32_t* value) {
  if (instruction->IsIntConstant()) {
    *value = instruction->AsIntConstant()->GetValue();
    return true;
  } else if (instruction->IsLongConstant()) {
    const int64_t c = instruction->AsLongConstant()->GetValue();
    if (CanLongValueFitIntoInt(c)) {
      *value = static_cast<int32_t>(c);
      return true;
    }
  }
  return false;
}

/**
 * An upper bound a * (length / a) + b, where a > 0, can be conservatively rewritten as length + b
 * because length >= 0 is true. This makes it more likely the bound is useful to clients.
 */
static InductionVarRange::Value SimplifyMax(InductionVarRange::Value v) {
  int32_t value;
  if (v.a_constant > 1 &&
      v.instruction->IsDiv() &&
      v.instruction->InputAt(0)->IsArrayLength() &&
      IsIntAndGet(v.instruction->InputAt(1), &value) && v.a_constant == value) {
    return InductionVarRange::Value(v.instruction->InputAt(0), 1, v.b_constant);
  }
  return v;
}

//
// Public class methods.
//

InductionVarRange::InductionVarRange(HInductionVarAnalysis* induction_analysis)
    : induction_analysis_(induction_analysis) {
  DCHECK(induction_analysis != nullptr);
}

InductionVarRange::Value InductionVarRange::GetMinInduction(HInstruction* context,
                                                            HInstruction* instruction) {
  HLoopInformation* loop = context->GetBlock()->GetLoopInformation();
  if (loop != nullptr) {
    return GetVal(induction_analysis_->LookupInfo(loop, instruction),
                  GetTripCount(loop, context), /* is_min */ true);
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetMaxInduction(HInstruction* context,
                                                            HInstruction* instruction) {
  HLoopInformation* loop = context->GetBlock()->GetLoopInformation();
  if (loop != nullptr) {
    return SimplifyMax(
        GetVal(induction_analysis_->LookupInfo(loop, instruction),
               GetTripCount(loop, context), /* is_min */ false));
  }
  return Value();
}

//
// Private class methods.
//

HInductionVarAnalysis::InductionInfo* InductionVarRange::GetTripCount(HLoopInformation* loop,
                                                                      HInstruction* context) {
  // The trip-count expression is only valid when the top-test is taken at least once,
  // that means, when the analyzed context appears outside the loop header itself.
  // Early-exit loops are okay, since in those cases, the trip-count is conservative.
  //
  // TODO: deal with runtime safety issues on TCs
  //
  if (context->GetBlock() != loop->GetHeader()) {
    HInductionVarAnalysis::InductionInfo* trip =
        induction_analysis_->LookupInfo(loop, loop->GetHeader()->GetLastInstruction());
    if (trip != nullptr) {
      // Wrap the trip-count representation in its own unusual NOP node, so that range analysis
      // is able to determine the [0, TC - 1] interval without having to construct constants.
      return induction_analysis_->CreateInvariantOp(HInductionVarAnalysis::kNop, trip, trip);
    }
  }
  return nullptr;
}

InductionVarRange::Value InductionVarRange::GetFetch(HInstruction* instruction,
                                                     HInductionVarAnalysis::InductionInfo* trip,
                                                     bool is_min) {
  // Detect constants and chase the fetch a bit deeper into the HIR tree, so that it becomes
  // more likely range analysis will compare the same instructions as terminal nodes.
  int32_t value;
  if (IsIntAndGet(instruction, &value)) {
    return Value(value);
  } else if (instruction->IsAdd()) {
    if (IsIntAndGet(instruction->InputAt(0), &value)) {
      return AddValue(Value(value), GetFetch(instruction->InputAt(1), trip, is_min));
    } else if (IsIntAndGet(instruction->InputAt(1), &value)) {
      return AddValue(GetFetch(instruction->InputAt(0), trip, is_min), Value(value));
    }
  } else if (is_min) {
    // Special case for finding minimum: minimum of trip-count is 1.
    if (trip != nullptr && instruction == trip->op_b->fetch) {
      return Value(1);
    }
  }
  return Value(instruction, 1, 0);
}

InductionVarRange::Value InductionVarRange::GetVal(HInductionVarAnalysis::InductionInfo* info,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool is_min) {
  if (info != nullptr) {
    switch (info->induction_class) {
      case HInductionVarAnalysis::kInvariant:
        // Invariants.
        switch (info->operation) {
          case HInductionVarAnalysis::kNop:  // normalized: 0 or TC-1
            DCHECK_EQ(info->op_a, info->op_b);
            return is_min ? Value(0)
                          : SubValue(GetVal(info->op_b, trip, is_min), Value(1));
          case HInductionVarAnalysis::kAdd:
            return AddValue(GetVal(info->op_a, trip, is_min),
                            GetVal(info->op_b, trip, is_min));
          case HInductionVarAnalysis::kSub:  // second reversed!
            return SubValue(GetVal(info->op_a, trip, is_min),
                            GetVal(info->op_b, trip, !is_min));
          case HInductionVarAnalysis::kNeg:  // second reversed!
            return SubValue(Value(0),
                            GetVal(info->op_b, trip, !is_min));
          case HInductionVarAnalysis::kMul:
            return GetMul(info->op_a, info->op_b, trip, is_min);
          case HInductionVarAnalysis::kDiv:
            return GetDiv(info->op_a, info->op_b, trip, is_min);
          case HInductionVarAnalysis::kFetch:
            return GetFetch(info->fetch, trip, is_min);
        }
        break;
      case HInductionVarAnalysis::kLinear:
        // Linear induction a * i + b, for normalized 0 <= i < TC.
        return AddValue(GetMul(info->op_a, trip, trip, is_min),
                        GetVal(info->op_b, trip, is_min));
      case HInductionVarAnalysis::kWrapAround:
      case HInductionVarAnalysis::kPeriodic:
        // Merge values in the wrap-around/periodic.
        return MergeVal(GetVal(info->op_a, trip, is_min),
                        GetVal(info->op_b, trip, is_min), is_min);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetMul(HInductionVarAnalysis::InductionInfo* info1,
                                                   HInductionVarAnalysis::InductionInfo* info2,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool is_min) {
  Value v1_min = GetVal(info1, trip, /* is_min */ true);
  Value v1_max = GetVal(info1, trip, /* is_min */ false);
  Value v2_min = GetVal(info2, trip, /* is_min */ true);
  Value v2_max = GetVal(info2, trip, /* is_min */ false);
  if (v1_min.is_known && v1_min.a_constant == 0 && v1_min.b_constant >= 0) {
    // Positive range vs. positive or negative range.
    if (v2_min.is_known && v2_min.a_constant == 0 && v2_min.b_constant >= 0) {
      return is_min ? MulValue(v1_min, v2_min)
                    : MulValue(v1_max, v2_max);
    } else if (v2_max.is_known && v2_max.a_constant == 0 && v2_max.b_constant <= 0) {
      return is_min ? MulValue(v1_max, v2_min)
                    : MulValue(v1_min, v2_max);
    }
  } else if (v1_min.is_known && v1_min.a_constant == 0 && v1_min.b_constant <= 0) {
    // Negative range vs. positive or negative range.
    if (v2_min.is_known && v2_min.a_constant == 0 && v2_min.b_constant >= 0) {
      return is_min ? MulValue(v1_min, v2_max)
                    : MulValue(v1_max, v2_min);
    } else if (v2_max.is_known && v2_max.a_constant == 0 && v2_max.b_constant <= 0) {
      return is_min ? MulValue(v1_max, v2_max)
                    : MulValue(v1_min, v2_min);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetDiv(HInductionVarAnalysis::InductionInfo* info1,
                                                   HInductionVarAnalysis::InductionInfo* info2,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool is_min) {
  Value v1_min = GetVal(info1, trip, /* is_min */ true);
  Value v1_max = GetVal(info1, trip, /* is_min */ false);
  Value v2_min = GetVal(info2, trip, /* is_min */ true);
  Value v2_max = GetVal(info2, trip, /* is_min */ false);
  if (v1_min.is_known && v1_min.a_constant == 0 && v1_min.b_constant >= 0) {
    // Positive range vs. positive or negative range.
    if (v2_min.is_known && v2_min.a_constant == 0 && v2_min.b_constant >= 0) {
      return is_min ? DivValue(v1_min, v2_max)
                    : DivValue(v1_max, v2_min);
    } else if (v2_max.is_known && v2_max.a_constant == 0 && v2_max.b_constant <= 0) {
      return is_min ? DivValue(v1_max, v2_max)
                    : DivValue(v1_min, v2_min);
    }
  } else if (v1_min.is_known && v1_min.a_constant == 0 && v1_min.b_constant <= 0) {
    // Negative range vs. positive or negative range.
    if (v2_min.is_known && v2_min.a_constant == 0 && v2_min.b_constant >= 0) {
      return is_min ? DivValue(v1_min, v2_min)
                    : DivValue(v1_max, v2_max);
    } else if (v2_max.is_known && v2_max.a_constant == 0 && v2_max.b_constant <= 0) {
      return is_min ? DivValue(v1_max, v2_min)
                    : DivValue(v1_min, v2_max);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::AddValue(Value v1, Value v2) {
  if (v1.is_known && v2.is_known && IsSafeAdd(v1.b_constant, v2.b_constant)) {
    const int32_t b = v1.b_constant + v2.b_constant;
    if (v1.a_constant == 0) {
      return Value(v2.instruction, v2.a_constant, b);
    } else if (v2.a_constant == 0) {
      return Value(v1.instruction, v1.a_constant, b);
    } else if (v1.instruction == v2.instruction && IsSafeAdd(v1.a_constant, v2.a_constant)) {
      return Value(v1.instruction, v1.a_constant + v2.a_constant, b);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::SubValue(Value v1, Value v2) {
  if (v1.is_known && v2.is_known && IsSafeSub(v1.b_constant, v2.b_constant)) {
    const int32_t b = v1.b_constant - v2.b_constant;
    if (v1.a_constant == 0 && IsSafeSub(0, v2.a_constant)) {
      return Value(v2.instruction, -v2.a_constant, b);
    } else if (v2.a_constant == 0) {
      return Value(v1.instruction, v1.a_constant, b);
    } else if (v1.instruction == v2.instruction && IsSafeSub(v1.a_constant, v2.a_constant)) {
      return Value(v1.instruction, v1.a_constant - v2.a_constant, b);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::MulValue(Value v1, Value v2) {
  if (v1.is_known && v2.is_known) {
    if (v1.a_constant == 0) {
      if (IsSafeMul(v1.b_constant, v2.a_constant) && IsSafeMul(v1.b_constant, v2.b_constant)) {
        return Value(v2.instruction, v1.b_constant * v2.a_constant, v1.b_constant * v2.b_constant);
      }
    } else if (v2.a_constant == 0) {
      if (IsSafeMul(v1.a_constant, v2.b_constant) && IsSafeMul(v1.b_constant, v2.b_constant)) {
        return Value(v1.instruction, v1.a_constant * v2.b_constant, v1.b_constant * v2.b_constant);
      }
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::DivValue(Value v1, Value v2) {
  if (v1.is_known && v2.is_known && v1.a_constant == 0 && v2.a_constant == 0) {
    if (IsSafeDiv(v1.b_constant, v2.b_constant)) {
      return Value(v1.b_constant / v2.b_constant);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::MergeVal(Value v1, Value v2, bool is_min) {
  if (v1.is_known && v2.is_known) {
    if (v1.instruction == v2.instruction && v1.a_constant == v2.a_constant) {
      return Value(v1.instruction, v1.a_constant,
                   is_min ? std::min(v1.b_constant, v2.b_constant)
                          : std::max(v1.b_constant, v2.b_constant));
    }
  }
  return Value();
}

}  // namespace art
