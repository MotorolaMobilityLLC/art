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

#include "base/arena_containers.h"
#include "bounds_check_elimination.h"
#include "nodes.h"

namespace art {

class MonotonicValueRange;

/**
 * A value bound is represented as a pair of value and constant,
 * e.g. array.length - 1.
 */
class ValueBound : public ValueObject {
 public:
  ValueBound(HInstruction* instruction, int32_t constant) {
    if (instruction != nullptr && instruction->IsIntConstant()) {
      // Normalize ValueBound with constant instruction.
      int32_t instr_const = instruction->AsIntConstant()->GetValue();
      if (!WouldAddOverflowOrUnderflow(instr_const, constant)) {
        instruction_ = nullptr;
        constant_ = instr_const + constant;
        return;
      }
    }
    instruction_ = instruction;
    constant_ = constant;
  }

  // Return whether (left + right) overflows or underflows.
  static bool WouldAddOverflowOrUnderflow(int32_t left, int32_t right) {
    if (right == 0) {
      return false;
    }
    if ((right > 0) && (left <= INT_MAX - right)) {
      // No overflow.
      return false;
    }
    if ((right < 0) && (left >= INT_MIN - right)) {
      // No underflow.
      return false;
    }
    return true;
  }

  static bool IsAddOrSubAConstant(HInstruction* instruction,
                                  HInstruction** left_instruction,
                                  int* right_constant) {
    if (instruction->IsAdd() || instruction->IsSub()) {
      HBinaryOperation* bin_op = instruction->AsBinaryOperation();
      HInstruction* left = bin_op->GetLeft();
      HInstruction* right = bin_op->GetRight();
      if (right->IsIntConstant()) {
        *left_instruction = left;
        int32_t c = right->AsIntConstant()->GetValue();
        *right_constant = instruction->IsAdd() ? c : -c;
        return true;
      }
    }
    *left_instruction = nullptr;
    *right_constant = 0;
    return false;
  }

  // Try to detect useful value bound format from an instruction, e.g.
  // a constant or array length related value.
  static ValueBound DetectValueBoundFromValue(HInstruction* instruction, bool* found) {
    DCHECK(instruction != nullptr);
    if (instruction->IsIntConstant()) {
      *found = true;
      return ValueBound(nullptr, instruction->AsIntConstant()->GetValue());
    }

    if (instruction->IsArrayLength()) {
      *found = true;
      return ValueBound(instruction, 0);
    }
    // Try to detect (array.length + c) format.
    HInstruction *left;
    int32_t right;
    if (IsAddOrSubAConstant(instruction, &left, &right)) {
      if (left->IsArrayLength()) {
        *found = true;
        return ValueBound(left, right);
      }
    }

    // No useful bound detected.
    *found = false;
    return ValueBound::Max();
  }

  HInstruction* GetInstruction() const { return instruction_; }
  int32_t GetConstant() const { return constant_; }

  bool IsRelatedToArrayLength() const {
    // Some bounds are created with HNewArray* as the instruction instead
    // of HArrayLength*. They are treated the same.
    return (instruction_ != nullptr) &&
           (instruction_->IsArrayLength() || instruction_->IsNewArray());
  }

  bool IsConstant() const {
    return instruction_ == nullptr;
  }

  static ValueBound Min() { return ValueBound(nullptr, INT_MIN); }
  static ValueBound Max() { return ValueBound(nullptr, INT_MAX); }

  bool Equals(ValueBound bound) const {
    return instruction_ == bound.instruction_ && constant_ == bound.constant_;
  }

  static HInstruction* FromArrayLengthToNewArrayIfPossible(HInstruction* instruction) {
    // Null check on the NewArray should have been eliminated by instruction
    // simplifier already.
    if (instruction->IsArrayLength() && instruction->InputAt(0)->IsNewArray()) {
      return instruction->InputAt(0)->AsNewArray();
    }
    return instruction;
  }

  static bool Equal(HInstruction* instruction1, HInstruction* instruction2) {
    if (instruction1 == instruction2) {
      return true;
    }

    if (instruction1 == nullptr || instruction2 == nullptr) {
      return false;
    }

    // Some bounds are created with HNewArray* as the instruction instead
    // of HArrayLength*. They are treated the same.
    instruction1 = FromArrayLengthToNewArrayIfPossible(instruction1);
    instruction2 = FromArrayLengthToNewArrayIfPossible(instruction2);
    return instruction1 == instruction2;
  }

  // Returns if it's certain this->bound >= `bound`.
  bool GreaterThanOrEqualTo(ValueBound bound) const {
    if (Equal(instruction_, bound.instruction_)) {
      return constant_ >= bound.constant_;
    }
    // Not comparable. Just return false.
    return false;
  }

  // Returns if it's certain this->bound <= `bound`.
  bool LessThanOrEqualTo(ValueBound bound) const {
    if (Equal(instruction_, bound.instruction_)) {
      return constant_ <= bound.constant_;
    }
    // Not comparable. Just return false.
    return false;
  }

  // Try to narrow lower bound. Returns the greatest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowLowerBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.GreaterThanOrEqualTo(bound2)) {
      return bound1;
    }
    if (bound2.GreaterThanOrEqualTo(bound1)) {
      return bound2;
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor constant as lower bound.
    return bound1.IsConstant() ? bound1 : bound2;
  }

  // Try to narrow upper bound. Returns the lowest of the two if possible.
  // Pick one if they are not comparable.
  static ValueBound NarrowUpperBound(ValueBound bound1, ValueBound bound2) {
    if (bound1.LessThanOrEqualTo(bound2)) {
      return bound1;
    }
    if (bound2.LessThanOrEqualTo(bound1)) {
      return bound2;
    }

    // Not comparable. Just pick one. We may lose some info, but that's ok.
    // Favor array length as upper bound.
    return bound1.IsRelatedToArrayLength() ? bound1 : bound2;
  }

  // Add a constant to a ValueBound.
  // `overflow` or `underflow` will return whether the resulting bound may
  // overflow or underflow an int.
  ValueBound Add(int32_t c, bool* overflow, bool* underflow) const {
    *overflow = *underflow = false;
    if (c == 0) {
      return *this;
    }

    int32_t new_constant;
    if (c > 0) {
      if (constant_ > INT_MAX - c) {
        *overflow = true;
        return Max();
      }

      new_constant = constant_ + c;
      // (array.length + non-positive-constant) won't overflow an int.
      if (IsConstant() || (IsRelatedToArrayLength() && new_constant <= 0)) {
        return ValueBound(instruction_, new_constant);
      }
      // Be conservative.
      *overflow = true;
      return Max();
    } else {
      if (constant_ < INT_MIN - c) {
        *underflow = true;
        return Min();
      }

      new_constant = constant_ + c;
      // Regardless of the value new_constant, (array.length+new_constant) will
      // never underflow since array.length is no less than 0.
      if (IsConstant() || IsRelatedToArrayLength()) {
        return ValueBound(instruction_, new_constant);
      }
      // Be conservative.
      *underflow = true;
      return Min();
    }
    return ValueBound(instruction_, new_constant);
  }

 private:
  HInstruction* instruction_;
  int32_t constant_;
};

/**
 * Represent a range of lower bound and upper bound, both being inclusive.
 * Currently a ValueRange may be generated as a result of the following:
 * comparisons related to array bounds, array bounds check, add/sub on top
 * of an existing value range, NewArray or a loop phi corresponding to an
 * incrementing/decrementing array index (MonotonicValueRange).
 */
class ValueRange : public ArenaObject<kArenaAllocMisc> {
 public:
  ValueRange(ArenaAllocator* allocator, ValueBound lower, ValueBound upper)
      : allocator_(allocator), lower_(lower), upper_(upper) {}

  virtual ~ValueRange() {}

  virtual MonotonicValueRange* AsMonotonicValueRange() { return nullptr; }
  bool IsMonotonicValueRange() {
    return AsMonotonicValueRange() != nullptr;
  }

  ArenaAllocator* GetAllocator() const { return allocator_; }
  ValueBound GetLower() const { return lower_; }
  ValueBound GetUpper() const { return upper_; }

  // If it's certain that this value range fits in other_range.
  virtual bool FitsIn(ValueRange* other_range) const {
    if (other_range == nullptr) {
      return true;
    }
    DCHECK(!other_range->IsMonotonicValueRange());
    return lower_.GreaterThanOrEqualTo(other_range->lower_) &&
           upper_.LessThanOrEqualTo(other_range->upper_);
  }

  // Returns the intersection of this and range.
  // If it's not possible to do intersection because some
  // bounds are not comparable, it's ok to pick either bound.
  virtual ValueRange* Narrow(ValueRange* range) {
    if (range == nullptr) {
      return this;
    }

    if (range->IsMonotonicValueRange()) {
      return this;
    }

    return new (allocator_) ValueRange(
        allocator_,
        ValueBound::NarrowLowerBound(lower_, range->lower_),
        ValueBound::NarrowUpperBound(upper_, range->upper_));
  }

  // Shift a range by a constant.
  ValueRange* Add(int32_t constant) const {
    bool overflow, underflow;
    ValueBound lower = lower_.Add(constant, &overflow, &underflow);
    if (underflow) {
      // Lower bound underflow will wrap around to positive values
      // and invalidate the upper bound.
      return nullptr;
    }
    ValueBound upper = upper_.Add(constant, &overflow, &underflow);
    if (overflow) {
      // Upper bound overflow will wrap around to negative values
      // and invalidate the lower bound.
      return nullptr;
    }
    return new (allocator_) ValueRange(allocator_, lower, upper);
  }

 private:
  ArenaAllocator* const allocator_;
  const ValueBound lower_;  // inclusive
  const ValueBound upper_;  // inclusive

  DISALLOW_COPY_AND_ASSIGN(ValueRange);
};

/**
 * A monotonically incrementing/decrementing value range, e.g.
 * the variable i in "for (int i=0; i<array.length; i++)".
 * Special care needs to be taken to account for overflow/underflow
 * of such value ranges.
 */
class MonotonicValueRange : public ValueRange {
 public:
  MonotonicValueRange(ArenaAllocator* allocator,
                      HInstruction* initial,
                      int32_t increment,
                      ValueBound bound)
      // To be conservative, give it full range [INT_MIN, INT_MAX] in case it's
      // used as a regular value range, due to possible overflow/underflow.
      : ValueRange(allocator, ValueBound::Min(), ValueBound::Max()),
        initial_(initial),
        increment_(increment),
        bound_(bound) {}

  virtual ~MonotonicValueRange() {}

  int32_t GetIncrement() const { return increment_; }

  ValueBound GetBound() const { return bound_; }

  MonotonicValueRange* AsMonotonicValueRange() OVERRIDE { return this; }

  // If it's certain that this value range fits in other_range.
  bool FitsIn(ValueRange* other_range) const OVERRIDE {
    if (other_range == nullptr) {
      return true;
    }
    DCHECK(!other_range->IsMonotonicValueRange());
    return false;
  }

  // Try to narrow this MonotonicValueRange given another range.
  // Ideally it will return a normal ValueRange. But due to
  // possible overflow/underflow, that may not be possible.
  ValueRange* Narrow(ValueRange* range) OVERRIDE {
    if (range == nullptr) {
      return this;
    }
    DCHECK(!range->IsMonotonicValueRange());

    if (increment_ > 0) {
      // Monotonically increasing.
      ValueBound lower = ValueBound::NarrowLowerBound(bound_, range->GetLower());

      // We currently conservatively assume max array length is INT_MAX. If we can
      // make assumptions about the max array length, e.g. due to the max heap size,
      // divided by the element size (such as 4 bytes for each integer array), we can
      // lower this number and rule out some possible overflows.
      int32_t max_array_len = INT_MAX;

      // max possible integer value of range's upper value.
      int32_t upper = INT_MAX;
      // Try to lower upper.
      ValueBound upper_bound = range->GetUpper();
      if (upper_bound.IsConstant()) {
        upper = upper_bound.GetConstant();
      } else if (upper_bound.IsRelatedToArrayLength() && upper_bound.GetConstant() <= 0) {
        // Normal case. e.g. <= array.length - 1.
        upper = max_array_len + upper_bound.GetConstant();
      }

      // If we can prove for the last number in sequence of initial_,
      // initial_ + increment_, initial_ + 2 x increment_, ...
      // that's <= upper, (last_num_in_sequence + increment_) doesn't trigger overflow,
      // then this MonoticValueRange is narrowed to a normal value range.

      // Be conservative first, assume last number in the sequence hits upper.
      int32_t last_num_in_sequence = upper;
      if (initial_->IsIntConstant()) {
        int32_t initial_constant = initial_->AsIntConstant()->GetValue();
        if (upper <= initial_constant) {
          last_num_in_sequence = upper;
        } else {
          // Cast to int64_t for the substraction part to avoid int32_t overflow.
          last_num_in_sequence = initial_constant +
              ((int64_t)upper - (int64_t)initial_constant) / increment_ * increment_;
        }
      }
      if (last_num_in_sequence <= INT_MAX - increment_) {
        // No overflow. The sequence will be stopped by the upper bound test as expected.
        return new (GetAllocator()) ValueRange(GetAllocator(), lower, range->GetUpper());
      }

      // There might be overflow. Give up narrowing.
      return this;
    } else {
      DCHECK_NE(increment_, 0);
      // Monotonically decreasing.
      ValueBound upper = ValueBound::NarrowUpperBound(bound_, range->GetUpper());

      // Need to take care of underflow. Try to prove underflow won't happen
      // for common cases.
      if (range->GetLower().IsConstant()) {
        int32_t constant = range->GetLower().GetConstant();
        if (constant >= INT_MIN - increment_) {
          return new (GetAllocator()) ValueRange(GetAllocator(), range->GetLower(), upper);
        }
      }

      // For non-constant lower bound, just assume might be underflow. Give up narrowing.
      return this;
    }
  }

 private:
  HInstruction* const initial_;
  const int32_t increment_;
  ValueBound bound_;  // Additional value bound info for initial_;

  DISALLOW_COPY_AND_ASSIGN(MonotonicValueRange);
};

class BCEVisitor : public HGraphVisitor {
 public:
  explicit BCEVisitor(HGraph* graph)
      : HGraphVisitor(graph),
        maps_(graph->GetBlocks().Size()) {}

 private:
  // Return the map of proven value ranges at the beginning of a basic block.
  ArenaSafeMap<int, ValueRange*>* GetValueRangeMap(HBasicBlock* basic_block) {
    int block_id = basic_block->GetBlockId();
    if (maps_.at(block_id) == nullptr) {
      std::unique_ptr<ArenaSafeMap<int, ValueRange*>> map(
          new ArenaSafeMap<int, ValueRange*>(
              std::less<int>(), GetGraph()->GetArena()->Adapter()));
      maps_.at(block_id) = std::move(map);
    }
    return maps_.at(block_id).get();
  }

  // Traverse up the dominator tree to look for value range info.
  ValueRange* LookupValueRange(HInstruction* instruction, HBasicBlock* basic_block) {
    while (basic_block != nullptr) {
      ArenaSafeMap<int, ValueRange*>* map = GetValueRangeMap(basic_block);
      if (map->find(instruction->GetId()) != map->end()) {
        return map->Get(instruction->GetId());
      }
      basic_block = basic_block->GetDominator();
    }
    // Didn't find any.
    return nullptr;
  }

  // Narrow the value range of `instruction` at the end of `basic_block` with `range`,
  // and push the narrowed value range to `successor`.
  void ApplyRangeFromComparison(HInstruction* instruction, HBasicBlock* basic_block,
                                HBasicBlock* successor, ValueRange* range) {
    ValueRange* existing_range = LookupValueRange(instruction, basic_block);
    if (existing_range == nullptr) {
      if (range != nullptr) {
        GetValueRangeMap(successor)->Overwrite(instruction->GetId(), range);
      }
      return;
    }
    if (existing_range->IsMonotonicValueRange()) {
      DCHECK(instruction->IsLoopHeaderPhi());
      // Make sure the comparison is in the loop header so each increment is
      // checked with a comparison.
      if (instruction->GetBlock() != basic_block) {
        return;
      }
    }
    ValueRange* narrowed_range = existing_range->Narrow(range);
    if (narrowed_range != nullptr) {
      GetValueRangeMap(successor)->Overwrite(instruction->GetId(), narrowed_range);
    }
  }

  // Special case that we may simultaneously narrow two MonotonicValueRange's to
  // regular value ranges.
  void HandleIfBetweenTwoMonotonicValueRanges(HIf* instruction,
                                              HInstruction* left,
                                              HInstruction* right,
                                              IfCondition cond,
                                              MonotonicValueRange* left_range,
                                              MonotonicValueRange* right_range) {
    DCHECK(left->IsLoopHeaderPhi());
    DCHECK(right->IsLoopHeaderPhi());
    if (instruction->GetBlock() != left->GetBlock()) {
      // Comparison needs to be in loop header to make sure it's done after each
      // increment/decrement.
      return;
    }

    // Handle common cases which also don't have overflow/underflow concerns.
    if (left_range->GetIncrement() == 1 &&
        left_range->GetBound().IsConstant() &&
        right_range->GetIncrement() == -1 &&
        right_range->GetBound().IsRelatedToArrayLength() &&
        right_range->GetBound().GetConstant() < 0) {
      HBasicBlock* successor = nullptr;
      int32_t left_compensation = 0;
      int32_t right_compensation = 0;
      if (cond == kCondLT) {
        left_compensation = -1;
        right_compensation = 1;
        successor = instruction->IfTrueSuccessor();
      } else if (cond == kCondLE) {
        successor = instruction->IfTrueSuccessor();
      } else if (cond == kCondGT) {
        successor = instruction->IfFalseSuccessor();
      } else if (cond == kCondGE) {
        left_compensation = -1;
        right_compensation = 1;
        successor = instruction->IfFalseSuccessor();
      } else {
        // We don't handle '=='/'!=' test in case left and right can cross and
        // miss each other.
        return;
      }

      if (successor != nullptr) {
        bool overflow;
        bool underflow;
        ValueRange* new_left_range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            left_range->GetBound(),
            right_range->GetBound().Add(left_compensation, &overflow, &underflow));
        if (!overflow && !underflow) {
          ApplyRangeFromComparison(left, instruction->GetBlock(), successor,
                                   new_left_range);
        }

        ValueRange* new_right_range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            left_range->GetBound().Add(right_compensation, &overflow, &underflow),
            right_range->GetBound());
        if (!overflow && !underflow) {
          ApplyRangeFromComparison(right, instruction->GetBlock(), successor,
                                   new_right_range);
        }
      }
    }
  }

  // Handle "if (left cmp_cond right)".
  void HandleIf(HIf* instruction, HInstruction* left, HInstruction* right, IfCondition cond) {
    HBasicBlock* block = instruction->GetBlock();

    HBasicBlock* true_successor = instruction->IfTrueSuccessor();
    // There should be no critical edge at this point.
    DCHECK_EQ(true_successor->GetPredecessors().Size(), 1u);

    HBasicBlock* false_successor = instruction->IfFalseSuccessor();
    // There should be no critical edge at this point.
    DCHECK_EQ(false_successor->GetPredecessors().Size(), 1u);

    bool found;
    ValueBound bound = ValueBound::DetectValueBoundFromValue(right, &found);
    // Each comparison can establish a lower bound and an upper bound
    // for the left hand side.
    ValueBound lower = bound;
    ValueBound upper = bound;
    if (!found) {
      // No constant or array.length+c format bound found.
      // For i<j, we can still use j's upper bound as i's upper bound. Same for lower.
      ValueRange* right_range = LookupValueRange(right, block);
      if (right_range != nullptr) {
        if (right_range->IsMonotonicValueRange()) {
          ValueRange* left_range = LookupValueRange(left, block);
          if (left_range != nullptr && left_range->IsMonotonicValueRange()) {
            HandleIfBetweenTwoMonotonicValueRanges(instruction, left, right, cond,
                                                   left_range->AsMonotonicValueRange(),
                                                   right_range->AsMonotonicValueRange());
            return;
          }
        }
        lower = right_range->GetLower();
        upper = right_range->GetUpper();
      } else {
        lower = ValueBound::Min();
        upper = ValueBound::Max();
      }
    }

    bool overflow, underflow;
    if (cond == kCondLT || cond == kCondLE) {
      if (!upper.Equals(ValueBound::Max())) {
        int32_t compensation = (cond == kCondLT) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), ValueBound::Min(), new_upper);
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelatedToArrayLength()) {
        int32_t compensation = (cond == kCondLE) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, false_successor, new_range);
      }
    } else if (cond == kCondGT || cond == kCondGE) {
      // array.length as a lower bound isn't considered useful.
      if (!lower.Equals(ValueBound::Min()) && !lower.IsRelatedToArrayLength()) {
        int32_t compensation = (cond == kCondGT) ? 1 : 0;  // lower bound is inclusive
        ValueBound new_lower = lower.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), new_lower, ValueBound::Max());
        ApplyRangeFromComparison(left, block, true_successor, new_range);
      }

      if (!upper.Equals(ValueBound::Max())) {
        int32_t compensation = (cond == kCondGE) ? -1 : 0;  // upper bound is inclusive
        ValueBound new_upper = upper.Add(compensation, &overflow, &underflow);
        if (overflow || underflow) {
          return;
        }
        ValueRange* new_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), ValueBound::Min(), new_upper);
        ApplyRangeFromComparison(left, block, false_successor, new_range);
      }
    }
  }

  void VisitBoundsCheck(HBoundsCheck* bounds_check) {
    HBasicBlock* block = bounds_check->GetBlock();
    HInstruction* index = bounds_check->InputAt(0);
    HInstruction* array_length = bounds_check->InputAt(1);
    DCHECK(array_length->IsIntConstant() || array_length->IsArrayLength());

    if (!index->IsIntConstant()) {
      ValueRange* index_range = LookupValueRange(index, block);
      if (index_range != nullptr) {
        ValueBound lower = ValueBound(nullptr, 0);        // constant 0
        ValueBound upper = ValueBound(array_length, -1);  // array_length - 1
        ValueRange* array_range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), lower, upper);
        if (index_range->FitsIn(array_range)) {
          ReplaceBoundsCheck(bounds_check, index);
          return;
        }
      }
    } else {
      int32_t constant = index->AsIntConstant()->GetValue();
      if (constant < 0) {
        // Will always throw exception.
        return;
      }
      if (array_length->IsIntConstant()) {
        if (constant < array_length->AsIntConstant()->GetValue()) {
          ReplaceBoundsCheck(bounds_check, index);
        }
        return;
      }

      DCHECK(array_length->IsArrayLength());
      ValueRange* existing_range = LookupValueRange(array_length, block);
      if (existing_range != nullptr) {
        ValueBound lower = existing_range->GetLower();
        DCHECK(lower.IsConstant());
        if (constant < lower.GetConstant()) {
          ReplaceBoundsCheck(bounds_check, index);
          return;
        } else {
          // Existing range isn't strong enough to eliminate the bounds check.
          // Fall through to update the array_length range with info from this
          // bounds check.
        }
      }

      // Once we have an array access like 'array[5] = 1', we record array.length >= 6.
      // We currently don't do it for non-constant index since a valid array[i] can't prove
      // a valid array[i-1] yet due to the lower bound side.
      ValueBound lower = ValueBound(nullptr, constant + 1);
      ValueBound upper = ValueBound::Max();
      ValueRange* range = new (GetGraph()->GetArena())
          ValueRange(GetGraph()->GetArena(), lower, upper);
      GetValueRangeMap(block)->Overwrite(array_length->GetId(), range);
    }
  }

  void ReplaceBoundsCheck(HInstruction* bounds_check, HInstruction* index) {
    bounds_check->ReplaceWith(index);
    bounds_check->GetBlock()->RemoveInstruction(bounds_check);
  }

  void VisitPhi(HPhi* phi) {
    if (phi->IsLoopHeaderPhi() && phi->GetType() == Primitive::kPrimInt) {
      DCHECK_EQ(phi->InputCount(), 2U);
      HInstruction* instruction = phi->InputAt(1);
      HInstruction *left;
      int32_t increment;
      if (ValueBound::IsAddOrSubAConstant(instruction, &left, &increment)) {
        if (left == phi) {
          HInstruction* initial_value = phi->InputAt(0);
          ValueRange* range = nullptr;
          if (increment == 0) {
            // Add constant 0. It's really a fixed value.
            range = new (GetGraph()->GetArena()) ValueRange(
                GetGraph()->GetArena(),
                ValueBound(initial_value, 0),
                ValueBound(initial_value, 0));
          } else {
            // Monotonically increasing/decreasing.
            bool found;
            ValueBound bound = ValueBound::DetectValueBoundFromValue(
                initial_value, &found);
            if (!found) {
              // No constant or array.length+c bound found.
              // For i=j, we can still use j's upper bound as i's upper bound.
              // Same for lower.
              ValueRange* initial_range = LookupValueRange(initial_value, phi->GetBlock());
              if (initial_range != nullptr) {
                bound = increment > 0 ? initial_range->GetLower() :
                                        initial_range->GetUpper();
              } else {
                bound = increment > 0 ? ValueBound::Min() : ValueBound::Max();
              }
            }
            range = new (GetGraph()->GetArena()) MonotonicValueRange(
                GetGraph()->GetArena(),
                initial_value,
                increment,
                bound);
          }
          GetValueRangeMap(phi->GetBlock())->Overwrite(phi->GetId(), range);
        }
      }
    }
  }

  void VisitIf(HIf* instruction) {
    if (instruction->InputAt(0)->IsCondition()) {
      HCondition* cond = instruction->InputAt(0)->AsCondition();
      IfCondition cmp = cond->GetCondition();
      if (cmp == kCondGT || cmp == kCondGE ||
          cmp == kCondLT || cmp == kCondLE) {
        HInstruction* left = cond->GetLeft();
        HInstruction* right = cond->GetRight();
        HandleIf(instruction, left, right, cmp);
      }
    }
  }

  void VisitAdd(HAdd* add) {
    HInstruction* right = add->GetRight();
    if (right->IsIntConstant()) {
      ValueRange* left_range = LookupValueRange(add->GetLeft(), add->GetBlock());
      if (left_range == nullptr) {
        return;
      }
      ValueRange* range = left_range->Add(right->AsIntConstant()->GetValue());
      if (range != nullptr) {
        GetValueRangeMap(add->GetBlock())->Overwrite(add->GetId(), range);
      }
    }
  }

  void VisitSub(HSub* sub) {
    HInstruction* left = sub->GetLeft();
    HInstruction* right = sub->GetRight();
    if (right->IsIntConstant()) {
      ValueRange* left_range = LookupValueRange(left, sub->GetBlock());
      if (left_range == nullptr) {
        return;
      }
      ValueRange* range = left_range->Add(-right->AsIntConstant()->GetValue());
      if (range != nullptr) {
        GetValueRangeMap(sub->GetBlock())->Overwrite(sub->GetId(), range);
        return;
      }
    }

    // Here we are interested in the typical triangular case of nested loops,
    // such as the inner loop 'for (int j=0; j<array.length-i; j++)' where i
    // is the index for outer loop. In this case, we know j is bounded by array.length-1.

    // Try to handle (array.length - i) or (array.length + c - i) format.
    HInstruction* left_of_left;  // left input of left.
    int32_t right_const = 0;
    if (ValueBound::IsAddOrSubAConstant(left, &left_of_left, &right_const)) {
      left = left_of_left;
    }
    // The value of left input of the sub equals (left + right_const).

    if (left->IsArrayLength()) {
      HInstruction* array_length = left->AsArrayLength();
      ValueRange* right_range = LookupValueRange(right, sub->GetBlock());
      if (right_range != nullptr) {
        ValueBound lower = right_range->GetLower();
        ValueBound upper = right_range->GetUpper();
        if (lower.IsConstant() && upper.IsRelatedToArrayLength()) {
          HInstruction* upper_inst = upper.GetInstruction();
          // Make sure it's the same array.
          if (ValueBound::Equal(array_length, upper_inst)) {
            int32_t c0 = right_const;
            int32_t c1 = lower.GetConstant();
            int32_t c2 = upper.GetConstant();
            // (array.length + c0 - v) where v is in [c1, array.length + c2]
            // gets [c0 - c2, array.length + c0 - c1] as its value range.
            if (!ValueBound::WouldAddOverflowOrUnderflow(c0, -c2) &&
                !ValueBound::WouldAddOverflowOrUnderflow(c0, -c1)) {
              if ((c0 - c1) <= 0) {
                // array.length + (c0 - c1) won't overflow/underflow.
                ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
                    GetGraph()->GetArena(),
                    ValueBound(nullptr, right_const - upper.GetConstant()),
                    ValueBound(array_length, right_const - lower.GetConstant()));
                GetValueRangeMap(sub->GetBlock())->Overwrite(sub->GetId(), range);
              }
            }
          }
        }
      }
    }
  }

  void FindAndHandlePartialArrayLength(HBinaryOperation* instruction) {
    DCHECK(instruction->IsDiv() || instruction->IsShr() || instruction->IsUShr());
    HInstruction* right = instruction->GetRight();
    int32_t right_const;
    if (right->IsIntConstant()) {
      right_const = right->AsIntConstant()->GetValue();
      // Detect division by two or more.
      if ((instruction->IsDiv() && right_const <= 1) ||
          (instruction->IsShr() && right_const < 1) ||
          (instruction->IsUShr() && right_const < 1)) {
        return;
      }
    } else {
      return;
    }

    // Try to handle array.length/2 or (array.length-1)/2 format.
    HInstruction* left = instruction->GetLeft();
    HInstruction* left_of_left;  // left input of left.
    int32_t c = 0;
    if (ValueBound::IsAddOrSubAConstant(left, &left_of_left, &c)) {
      left = left_of_left;
    }
    // The value of left input of instruction equals (left + c).

    // (array_length + 1) or smaller divided by two or more
    // always generate a value in [INT_MIN, array_length].
    // This is true even if array_length is INT_MAX.
    if (left->IsArrayLength() && c <= 1) {
      if (instruction->IsUShr() && c < 0) {
        // Make sure for unsigned shift, left side is not negative.
        // e.g. if array_length is 2, ((array_length - 3) >>> 2) is way bigger
        // than array_length.
        return;
      }
      ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
          GetGraph()->GetArena(),
          ValueBound(nullptr, INT_MIN),
          ValueBound(left, 0));
      GetValueRangeMap(instruction->GetBlock())->Overwrite(instruction->GetId(), range);
    }
  }

  void VisitDiv(HDiv* div) {
    FindAndHandlePartialArrayLength(div);
  }

  void VisitShr(HShr* shr) {
    FindAndHandlePartialArrayLength(shr);
  }

  void VisitUShr(HUShr* ushr) {
    FindAndHandlePartialArrayLength(ushr);
  }

  void VisitAnd(HAnd* instruction) {
    if (instruction->GetRight()->IsIntConstant()) {
      int32_t constant = instruction->GetRight()->AsIntConstant()->GetValue();
      if (constant > 0) {
        // constant serves as a mask so any number masked with it
        // gets a [0, constant] value range.
        ValueRange* range = new (GetGraph()->GetArena()) ValueRange(
            GetGraph()->GetArena(),
            ValueBound(nullptr, 0),
            ValueBound(nullptr, constant));
        GetValueRangeMap(instruction->GetBlock())->Overwrite(instruction->GetId(), range);
      }
    }
  }

  void VisitNewArray(HNewArray* new_array) {
    HInstruction* len = new_array->InputAt(0);
    if (!len->IsIntConstant()) {
      HInstruction *left;
      int32_t right_const;
      if (ValueBound::IsAddOrSubAConstant(len, &left, &right_const)) {
        // (left + right_const) is used as size to new the array.
        // We record "-right_const <= left <= new_array - right_const";
        ValueBound lower = ValueBound(nullptr, -right_const);
        // We use new_array for the bound instead of new_array.length,
        // which isn't available as an instruction yet. new_array will
        // be treated the same as new_array.length when it's used in a ValueBound.
        ValueBound upper = ValueBound(new_array, -right_const);
        ValueRange* range = new (GetGraph()->GetArena())
            ValueRange(GetGraph()->GetArena(), lower, upper);
        GetValueRangeMap(new_array->GetBlock())->Overwrite(left->GetId(), range);
      }
    }
  }

  std::vector<std::unique_ptr<ArenaSafeMap<int, ValueRange*>>> maps_;

  DISALLOW_COPY_AND_ASSIGN(BCEVisitor);
};

void BoundsCheckElimination::Run() {
  if (!graph_->HasArrayAccesses()) {
    return;
  }

  BCEVisitor visitor(graph_);
  // Reverse post order guarantees a node's dominators are visited first.
  // We want to visit in the dominator-based order since if a value is known to
  // be bounded by a range at one instruction, it must be true that all uses of
  // that value dominated by that instruction fits in that range. Range of that
  // value can be narrowed further down in the dominator tree.
  //
  // TODO: only visit blocks that dominate some array accesses.
  visitor.VisitReversePostOrder();
}

}  // namespace art
