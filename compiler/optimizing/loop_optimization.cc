/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "loop_optimization.h"

#include "arch/arm/instruction_set_features_arm.h"
#include "arch/arm64/instruction_set_features_arm64.h"
#include "arch/instruction_set.h"
#include "arch/mips/instruction_set_features_mips.h"
#include "arch/mips64/instruction_set_features_mips64.h"
#include "arch/x86/instruction_set_features_x86.h"
#include "arch/x86_64/instruction_set_features_x86_64.h"
#include "driver/compiler_driver.h"
#include "linear_order.h"

namespace art {

// Enables vectorization (SIMDization) in the loop optimizer.
static constexpr bool kEnableVectorization = true;

// All current SIMD targets want 16-byte alignment.
static constexpr size_t kAlignedBase = 16;

// Remove the instruction from the graph. A bit more elaborate than the usual
// instruction removal, since there may be a cycle in the use structure.
static void RemoveFromCycle(HInstruction* instruction) {
  instruction->RemoveAsUserOfAllInputs();
  instruction->RemoveEnvironmentUsers();
  instruction->GetBlock()->RemoveInstructionOrPhi(instruction, /*ensure_safety=*/ false);
  RemoveEnvironmentUses(instruction);
  ResetEnvironmentInputRecords(instruction);
}

// Detect a goto block and sets succ to the single successor.
static bool IsGotoBlock(HBasicBlock* block, /*out*/ HBasicBlock** succ) {
  if (block->GetPredecessors().size() == 1 &&
      block->GetSuccessors().size() == 1 &&
      block->IsSingleGoto()) {
    *succ = block->GetSingleSuccessor();
    return true;
  }
  return false;
}

// Detect an early exit loop.
static bool IsEarlyExit(HLoopInformation* loop_info) {
  HBlocksInLoopReversePostOrderIterator it_loop(*loop_info);
  for (it_loop.Advance(); !it_loop.Done(); it_loop.Advance()) {
    for (HBasicBlock* successor : it_loop.Current()->GetSuccessors()) {
      if (!loop_info->Contains(*successor)) {
        return true;
      }
    }
  }
  return false;
}

// Detect a sign extension from the given type. Returns the promoted operand on success.
static bool IsSignExtensionAndGet(HInstruction* instruction,
                                  Primitive::Type type,
                                  /*out*/ HInstruction** operand) {
  // Accept any already wider constant that would be handled properly by sign
  // extension when represented in the *width* of the given narrower data type
  // (the fact that char normally zero extends does not matter here).
  int64_t value = 0;
  if (IsInt64AndGet(instruction, /*out*/ &value)) {
    switch (type) {
      case Primitive::kPrimByte:
        if (std::numeric_limits<int8_t>::min() <= value &&
            std::numeric_limits<int8_t>::max() >= value) {
          *operand = instruction;
          return true;
        }
        return false;
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
        if (std::numeric_limits<int16_t>::min() <= value &&
            std::numeric_limits<int16_t>::max() <= value) {
          *operand = instruction;
          return true;
        }
        return false;
      default:
        return false;
    }
  }
  // An implicit widening conversion of a signed integer to an integral type sign-extends
  // the two's-complement representation of the integer value to fill the wider format.
  if (instruction->GetType() == type && (instruction->IsArrayGet() ||
                                         instruction->IsStaticFieldGet() ||
                                         instruction->IsInstanceFieldGet())) {
    switch (type) {
      case Primitive::kPrimByte:
      case Primitive::kPrimShort:
        *operand = instruction;
        return true;
      default:
        return false;
    }
  }
  // TODO: perhaps explicit conversions later too?
  //       (this may return something different from instruction)
  return false;
}

// Detect a zero extension from the given type. Returns the promoted operand on success.
static bool IsZeroExtensionAndGet(HInstruction* instruction,
                                  Primitive::Type type,
                                  /*out*/ HInstruction** operand) {
  // Accept any already wider constant that would be handled properly by zero
  // extension when represented in the *width* of the given narrower data type
  // (the fact that byte/short normally sign extend does not matter here).
  int64_t value = 0;
  if (IsInt64AndGet(instruction, /*out*/ &value)) {
    switch (type) {
      case Primitive::kPrimByte:
        if (std::numeric_limits<uint8_t>::min() <= value &&
            std::numeric_limits<uint8_t>::max() >= value) {
          *operand = instruction;
          return true;
        }
        return false;
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
        if (std::numeric_limits<uint16_t>::min() <= value &&
            std::numeric_limits<uint16_t>::max() <= value) {
          *operand = instruction;
          return true;
        }
        return false;
      default:
        return false;
    }
  }
  // An implicit widening conversion of a char to an integral type zero-extends
  // the representation of the char value to fill the wider format.
  if (instruction->GetType() == type && (instruction->IsArrayGet() ||
                                         instruction->IsStaticFieldGet() ||
                                         instruction->IsInstanceFieldGet())) {
    if (type == Primitive::kPrimChar) {
      *operand = instruction;
      return true;
    }
  }
  // A sign (or zero) extension followed by an explicit removal of just the
  // higher sign bits is equivalent to a zero extension of the underlying operand.
  if (instruction->IsAnd()) {
    int64_t mask = 0;
    HInstruction* a = instruction->InputAt(0);
    HInstruction* b = instruction->InputAt(1);
    // In (a & b) find (mask & b) or (a & mask) with sign or zero extension on the non-mask.
    if ((IsInt64AndGet(a, /*out*/ &mask) && (IsSignExtensionAndGet(b, type, /*out*/ operand) ||
                                             IsZeroExtensionAndGet(b, type, /*out*/ operand))) ||
        (IsInt64AndGet(b, /*out*/ &mask) && (IsSignExtensionAndGet(a, type, /*out*/ operand) ||
                                             IsZeroExtensionAndGet(a, type, /*out*/ operand)))) {
      switch ((*operand)->GetType()) {
        case Primitive::kPrimByte:  return mask == std::numeric_limits<uint8_t>::max();
        case Primitive::kPrimChar:
        case Primitive::kPrimShort: return mask == std::numeric_limits<uint16_t>::max();
        default: return false;
      }
    }
  }
  // TODO: perhaps explicit conversions later too?
  return false;
}

// Detect situations with same-extension narrower operands.
// Returns true on success and sets is_unsigned accordingly.
static bool IsNarrowerOperands(HInstruction* a,
                               HInstruction* b,
                               Primitive::Type type,
                               /*out*/ HInstruction** r,
                               /*out*/ HInstruction** s,
                               /*out*/ bool* is_unsigned) {
  if (IsSignExtensionAndGet(a, type, r) && IsSignExtensionAndGet(b, type, s)) {
    *is_unsigned = false;
    return true;
  } else if (IsZeroExtensionAndGet(a, type, r) && IsZeroExtensionAndGet(b, type, s)) {
    *is_unsigned = true;
    return true;
  }
  return false;
}

// As above, single operand.
static bool IsNarrowerOperand(HInstruction* a,
                              Primitive::Type type,
                              /*out*/ HInstruction** r,
                              /*out*/ bool* is_unsigned) {
  if (IsSignExtensionAndGet(a, type, r)) {
    *is_unsigned = false;
    return true;
  } else if (IsZeroExtensionAndGet(a, type, r)) {
    *is_unsigned = true;
    return true;
  }
  return false;
}

// Detect up to two instructions a and b, and an acccumulated constant c.
static bool IsAddConstHelper(HInstruction* instruction,
                             /*out*/ HInstruction** a,
                             /*out*/ HInstruction** b,
                             /*out*/ int64_t* c,
                             int32_t depth) {
  static constexpr int32_t kMaxDepth = 8;  // don't search too deep
  int64_t value = 0;
  if (IsInt64AndGet(instruction, &value)) {
    *c += value;
    return true;
  } else if (instruction->IsAdd() && depth <= kMaxDepth) {
    return IsAddConstHelper(instruction->InputAt(0), a, b, c, depth + 1) &&
           IsAddConstHelper(instruction->InputAt(1), a, b, c, depth + 1);
  } else if (*a == nullptr) {
    *a = instruction;
    return true;
  } else if (*b == nullptr) {
    *b = instruction;
    return true;
  }
  return false;  // too many non-const operands
}

// Detect a + b + c for an optional constant c.
static bool IsAddConst(HInstruction* instruction,
                       /*out*/ HInstruction** a,
                       /*out*/ HInstruction** b,
                       /*out*/ int64_t* c) {
  if (instruction->IsAdd()) {
    // Try to find a + b and accumulated c.
    if (IsAddConstHelper(instruction->InputAt(0), a, b, c, /*depth*/ 0) &&
        IsAddConstHelper(instruction->InputAt(1), a, b, c, /*depth*/ 0) &&
        *b != nullptr) {
      return true;
    }
    // Found a + b.
    *a = instruction->InputAt(0);
    *b = instruction->InputAt(1);
    *c = 0;
    return true;
  }
  return false;
}

// Detect reductions of the following forms,
// under assumption phi has only *one* use:
//   x = x_phi + ..
//   x = x_phi - ..
//   x = max(x_phi, ..)
//   x = min(x_phi, ..)
static bool HasReductionFormat(HInstruction* reduction, HInstruction* phi) {
  if (reduction->IsAdd()) {
    return reduction->InputAt(0) == phi || reduction->InputAt(1) == phi;
  } else if (reduction->IsSub()) {
    return reduction->InputAt(0) == phi;
  } else if (reduction->IsInvokeStaticOrDirect()) {
    switch (reduction->AsInvokeStaticOrDirect()->GetIntrinsic()) {
      case Intrinsics::kMathMinIntInt:
      case Intrinsics::kMathMinLongLong:
      case Intrinsics::kMathMinFloatFloat:
      case Intrinsics::kMathMinDoubleDouble:
      case Intrinsics::kMathMaxIntInt:
      case Intrinsics::kMathMaxLongLong:
      case Intrinsics::kMathMaxFloatFloat:
      case Intrinsics::kMathMaxDoubleDouble:
        return reduction->InputAt(0) == phi || reduction->InputAt(1) == phi;
      default:
        return false;
    }
  }
  return false;
}

// Translates operation to reduction kind.
static HVecReduce::ReductionKind GetReductionKind(HInstruction* reduction) {
  if (reduction->IsVecAdd() || reduction->IsVecSub()) {
    return HVecReduce::kSum;
  } else if (reduction->IsVecMin()) {
    return HVecReduce::kMin;
  } else if (reduction->IsVecMax()) {
    return HVecReduce::kMax;
  }
  LOG(FATAL) << "Unsupported SIMD reduction";
  UNREACHABLE();
}

// Test vector restrictions.
static bool HasVectorRestrictions(uint64_t restrictions, uint64_t tested) {
  return (restrictions & tested) != 0;
}

// Insert an instruction.
static HInstruction* Insert(HBasicBlock* block, HInstruction* instruction) {
  DCHECK(block != nullptr);
  DCHECK(instruction != nullptr);
  block->InsertInstructionBefore(instruction, block->GetLastInstruction());
  return instruction;
}

// Check that instructions from the induction sets are fully removed: have no uses
// and no other instructions use them.
static bool CheckInductionSetFullyRemoved(ArenaSet<HInstruction*>* iset) {
  for (HInstruction* instr : *iset) {
    if (instr->GetBlock() != nullptr ||
        !instr->GetUses().empty() ||
        !instr->GetEnvUses().empty() ||
        HasEnvironmentUsedByOthers(instr)) {
      return false;
    }
  }
  return true;
}

//
// Public methods.
//

HLoopOptimization::HLoopOptimization(HGraph* graph,
                                     CompilerDriver* compiler_driver,
                                     HInductionVarAnalysis* induction_analysis,
                                     OptimizingCompilerStats* stats)
    : HOptimization(graph, kLoopOptimizationPassName, stats),
      compiler_driver_(compiler_driver),
      induction_range_(induction_analysis),
      loop_allocator_(nullptr),
      global_allocator_(graph_->GetArena()),
      top_loop_(nullptr),
      last_loop_(nullptr),
      iset_(nullptr),
      reductions_(nullptr),
      simplified_(false),
      vector_length_(0),
      vector_refs_(nullptr),
      vector_peeling_candidate_(nullptr),
      vector_runtime_test_a_(nullptr),
      vector_runtime_test_b_(nullptr),
      vector_map_(nullptr),
      vector_permanent_map_(nullptr) {
}

void HLoopOptimization::Run() {
  // Skip if there is no loop or the graph has try-catch/irreducible loops.
  // TODO: make this less of a sledgehammer.
  if (!graph_->HasLoops() || graph_->HasTryCatch() || graph_->HasIrreducibleLoops()) {
    return;
  }

  // Phase-local allocator that draws from the global pool. Since the allocator
  // itself resides on the stack, it is destructed on exiting Run(), which
  // implies its underlying memory is released immediately.
  ArenaAllocator allocator(global_allocator_->GetArenaPool());
  loop_allocator_ = &allocator;

  // Perform loop optimizations.
  LocalRun();
  if (top_loop_ == nullptr) {
    graph_->SetHasLoops(false);  // no more loops
  }

  // Detach.
  loop_allocator_ = nullptr;
  last_loop_ = top_loop_ = nullptr;
}

//
// Loop setup and traversal.
//

void HLoopOptimization::LocalRun() {
  // Build the linear order using the phase-local allocator. This step enables building
  // a loop hierarchy that properly reflects the outer-inner and previous-next relation.
  ArenaVector<HBasicBlock*> linear_order(loop_allocator_->Adapter(kArenaAllocLinearOrder));
  LinearizeGraph(graph_, loop_allocator_, &linear_order);

  // Build the loop hierarchy.
  for (HBasicBlock* block : linear_order) {
    if (block->IsLoopHeader()) {
      AddLoop(block->GetLoopInformation());
    }
  }

  // Traverse the loop hierarchy inner-to-outer and optimize. Traversal can use
  // temporary data structures using the phase-local allocator. All new HIR
  // should use the global allocator.
  if (top_loop_ != nullptr) {
    ArenaSet<HInstruction*> iset(loop_allocator_->Adapter(kArenaAllocLoopOptimization));
    ArenaSafeMap<HInstruction*, HInstruction*> reds(
        std::less<HInstruction*>(), loop_allocator_->Adapter(kArenaAllocLoopOptimization));
    ArenaSet<ArrayReference> refs(loop_allocator_->Adapter(kArenaAllocLoopOptimization));
    ArenaSafeMap<HInstruction*, HInstruction*> map(
        std::less<HInstruction*>(), loop_allocator_->Adapter(kArenaAllocLoopOptimization));
    ArenaSafeMap<HInstruction*, HInstruction*> perm(
        std::less<HInstruction*>(), loop_allocator_->Adapter(kArenaAllocLoopOptimization));
    // Attach.
    iset_ = &iset;
    reductions_ = &reds;
    vector_refs_ = &refs;
    vector_map_ = &map;
    vector_permanent_map_ = &perm;
    // Traverse.
    TraverseLoopsInnerToOuter(top_loop_);
    // Detach.
    iset_ = nullptr;
    reductions_ = nullptr;
    vector_refs_ = nullptr;
    vector_map_ = nullptr;
    vector_permanent_map_ = nullptr;
  }
}

void HLoopOptimization::AddLoop(HLoopInformation* loop_info) {
  DCHECK(loop_info != nullptr);
  LoopNode* node = new (loop_allocator_) LoopNode(loop_info);
  if (last_loop_ == nullptr) {
    // First loop.
    DCHECK(top_loop_ == nullptr);
    last_loop_ = top_loop_ = node;
  } else if (loop_info->IsIn(*last_loop_->loop_info)) {
    // Inner loop.
    node->outer = last_loop_;
    DCHECK(last_loop_->inner == nullptr);
    last_loop_ = last_loop_->inner = node;
  } else {
    // Subsequent loop.
    while (last_loop_->outer != nullptr && !loop_info->IsIn(*last_loop_->outer->loop_info)) {
      last_loop_ = last_loop_->outer;
    }
    node->outer = last_loop_->outer;
    node->previous = last_loop_;
    DCHECK(last_loop_->next == nullptr);
    last_loop_ = last_loop_->next = node;
  }
}

void HLoopOptimization::RemoveLoop(LoopNode* node) {
  DCHECK(node != nullptr);
  DCHECK(node->inner == nullptr);
  if (node->previous != nullptr) {
    // Within sequence.
    node->previous->next = node->next;
    if (node->next != nullptr) {
      node->next->previous = node->previous;
    }
  } else {
    // First of sequence.
    if (node->outer != nullptr) {
      node->outer->inner = node->next;
    } else {
      top_loop_ = node->next;
    }
    if (node->next != nullptr) {
      node->next->outer = node->outer;
      node->next->previous = nullptr;
    }
  }
}

bool HLoopOptimization::TraverseLoopsInnerToOuter(LoopNode* node) {
  bool changed = false;
  for ( ; node != nullptr; node = node->next) {
    // Visit inner loops first. Recompute induction information for this
    // loop if the induction of any inner loop has changed.
    if (TraverseLoopsInnerToOuter(node->inner)) {
      induction_range_.ReVisit(node->loop_info);
    }
    // Repeat simplifications in the loop-body until no more changes occur.
    // Note that since each simplification consists of eliminating code (without
    // introducing new code), this process is always finite.
    do {
      simplified_ = false;
      SimplifyInduction(node);
      SimplifyBlocks(node);
      changed = simplified_ || changed;
    } while (simplified_);
    // Optimize inner loop.
    if (node->inner == nullptr) {
      changed = OptimizeInnerLoop(node) || changed;
    }
  }
  return changed;
}

//
// Optimization.
//

void HLoopOptimization::SimplifyInduction(LoopNode* node) {
  HBasicBlock* header = node->loop_info->GetHeader();
  HBasicBlock* preheader = node->loop_info->GetPreHeader();
  // Scan the phis in the header to find opportunities to simplify an induction
  // cycle that is only used outside the loop. Replace these uses, if any, with
  // the last value and remove the induction cycle.
  // Examples: for (int i = 0; x != null;   i++) { .... no i .... }
  //           for (int i = 0; i < 10; i++, k++) { .... no k .... } return k;
  for (HInstructionIterator it(header->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    if (TrySetPhiInduction(phi, /*restrict_uses*/ true) &&
        TryAssignLastValue(node->loop_info, phi, preheader, /*collect_loop_uses*/ false)) {
      // Note that it's ok to have replaced uses after the loop with the last value, without
      // being able to remove the cycle. Environment uses (which are the reason we may not be
      // able to remove the cycle) within the loop will still hold the right value. We must
      // have tried first, however, to replace outside uses.
      if (CanRemoveCycle()) {
        simplified_ = true;
        for (HInstruction* i : *iset_) {
          RemoveFromCycle(i);
        }
        DCHECK(CheckInductionSetFullyRemoved(iset_));
      }
    }
  }
}

void HLoopOptimization::SimplifyBlocks(LoopNode* node) {
  // Iterate over all basic blocks in the loop-body.
  for (HBlocksInLoopIterator it(*node->loop_info); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // Remove dead instructions from the loop-body.
    RemoveDeadInstructions(block->GetPhis());
    RemoveDeadInstructions(block->GetInstructions());
    // Remove trivial control flow blocks from the loop-body.
    if (block->GetPredecessors().size() == 1 &&
        block->GetSuccessors().size() == 1 &&
        block->GetSingleSuccessor()->GetPredecessors().size() == 1) {
      simplified_ = true;
      block->MergeWith(block->GetSingleSuccessor());
    } else if (block->GetSuccessors().size() == 2) {
      // Trivial if block can be bypassed to either branch.
      HBasicBlock* succ0 = block->GetSuccessors()[0];
      HBasicBlock* succ1 = block->GetSuccessors()[1];
      HBasicBlock* meet0 = nullptr;
      HBasicBlock* meet1 = nullptr;
      if (succ0 != succ1 &&
          IsGotoBlock(succ0, &meet0) &&
          IsGotoBlock(succ1, &meet1) &&
          meet0 == meet1 &&  // meets again
          meet0 != block &&  // no self-loop
          meet0->GetPhis().IsEmpty()) {  // not used for merging
        simplified_ = true;
        succ0->DisconnectAndDelete();
        if (block->Dominates(meet0)) {
          block->RemoveDominatedBlock(meet0);
          succ1->AddDominatedBlock(meet0);
          meet0->SetDominator(succ1);
        }
      }
    }
  }
}

bool HLoopOptimization::OptimizeInnerLoop(LoopNode* node) {
  HBasicBlock* header = node->loop_info->GetHeader();
  HBasicBlock* preheader = node->loop_info->GetPreHeader();
  // Ensure loop header logic is finite.
  int64_t trip_count = 0;
  if (!induction_range_.IsFinite(node->loop_info, &trip_count)) {
    return false;
  }
  // Ensure there is only a single loop-body (besides the header).
  HBasicBlock* body = nullptr;
  for (HBlocksInLoopIterator it(*node->loop_info); !it.Done(); it.Advance()) {
    if (it.Current() != header) {
      if (body != nullptr) {
        return false;
      }
      body = it.Current();
    }
  }
  CHECK(body != nullptr);
  // Ensure there is only a single exit point.
  if (header->GetSuccessors().size() != 2) {
    return false;
  }
  HBasicBlock* exit = (header->GetSuccessors()[0] == body)
      ? header->GetSuccessors()[1]
      : header->GetSuccessors()[0];
  // Ensure exit can only be reached by exiting loop.
  if (exit->GetPredecessors().size() != 1) {
    return false;
  }
  // Detect either an empty loop (no side effects other than plain iteration) or
  // a trivial loop (just iterating once). Replace subsequent index uses, if any,
  // with the last value and remove the loop, possibly after unrolling its body.
  HPhi* main_phi = nullptr;
  if (TrySetSimpleLoopHeader(header, &main_phi)) {
    bool is_empty = IsEmptyBody(body);
    if (reductions_->empty() &&  // TODO: possible with some effort
        (is_empty || trip_count == 1) &&
        TryAssignLastValue(node->loop_info, main_phi, preheader, /*collect_loop_uses*/ true)) {
      if (!is_empty) {
        // Unroll the loop-body, which sees initial value of the index.
        main_phi->ReplaceWith(main_phi->InputAt(0));
        preheader->MergeInstructionsWith(body);
      }
      body->DisconnectAndDelete();
      exit->RemovePredecessor(header);
      header->RemoveSuccessor(exit);
      header->RemoveDominatedBlock(exit);
      header->DisconnectAndDelete();
      preheader->AddSuccessor(exit);
      preheader->AddInstruction(new (global_allocator_) HGoto());
      preheader->AddDominatedBlock(exit);
      exit->SetDominator(preheader);
      RemoveLoop(node);  // update hierarchy
      return true;
    }
  }
  // Vectorize loop, if possible and valid.
  if (kEnableVectorization &&
      TrySetSimpleLoopHeader(header, &main_phi) &&
      ShouldVectorize(node, body, trip_count) &&
      TryAssignLastValue(node->loop_info, main_phi, preheader, /*collect_loop_uses*/ true)) {
    Vectorize(node, body, exit, trip_count);
    graph_->SetHasSIMD(true);  // flag SIMD usage
    MaybeRecordStat(stats_, MethodCompilationStat::kLoopVectorized);
    return true;
  }
  return false;
}

//
// Loop vectorization. The implementation is based on the book by Aart J.C. Bik:
// "The Software Vectorization Handbook. Applying Multimedia Extensions for Maximum Performance."
// Intel Press, June, 2004 (http://www.aartbik.com/).
//

bool HLoopOptimization::ShouldVectorize(LoopNode* node, HBasicBlock* block, int64_t trip_count) {
  // Reset vector bookkeeping.
  vector_length_ = 0;
  vector_refs_->clear();
  vector_peeling_candidate_ = nullptr;
  vector_runtime_test_a_ =
  vector_runtime_test_b_= nullptr;

  // Phis in the loop-body prevent vectorization.
  if (!block->GetPhis().IsEmpty()) {
    return false;
  }

  // Scan the loop-body, starting a right-hand-side tree traversal at each left-hand-side
  // occurrence, which allows passing down attributes down the use tree.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    if (!VectorizeDef(node, it.Current(), /*generate_code*/ false)) {
      return false;  // failure to vectorize a left-hand-side
    }
  }

  // Does vectorization seem profitable?
  if (!IsVectorizationProfitable(trip_count)) {
    return false;
  }

  // Data dependence analysis. Find each pair of references with same type, where
  // at least one is a write. Each such pair denotes a possible data dependence.
  // This analysis exploits the property that differently typed arrays cannot be
  // aliased, as well as the property that references either point to the same
  // array or to two completely disjoint arrays, i.e., no partial aliasing.
  // Other than a few simply heuristics, no detailed subscript analysis is done.
  // The scan over references also finds a suitable dynamic loop peeling candidate.
  const ArrayReference* candidate = nullptr;
  for (auto i = vector_refs_->begin(); i != vector_refs_->end(); ++i) {
    for (auto j = i; ++j != vector_refs_->end(); ) {
      if (i->type == j->type && (i->lhs || j->lhs)) {
        // Found same-typed a[i+x] vs. b[i+y], where at least one is a write.
        HInstruction* a = i->base;
        HInstruction* b = j->base;
        HInstruction* x = i->offset;
        HInstruction* y = j->offset;
        if (a == b) {
          // Found a[i+x] vs. a[i+y]. Accept if x == y (loop-independent data dependence).
          // Conservatively assume a loop-carried data dependence otherwise, and reject.
          if (x != y) {
            return false;
          }
        } else {
          // Found a[i+x] vs. b[i+y]. Accept if x == y (at worst loop-independent data dependence).
          // Conservatively assume a potential loop-carried data dependence otherwise, avoided by
          // generating an explicit a != b disambiguation runtime test on the two references.
          if (x != y) {
            // To avoid excessive overhead, we only accept one a != b test.
            if (vector_runtime_test_a_ == nullptr) {
              // First test found.
              vector_runtime_test_a_ = a;
              vector_runtime_test_b_ = b;
            } else if ((vector_runtime_test_a_ != a || vector_runtime_test_b_ != b) &&
                       (vector_runtime_test_a_ != b || vector_runtime_test_b_ != a)) {
              return false;  // second test would be needed
            }
          }
        }
      }
    }
  }

  // Consider dynamic loop peeling for alignment.
  SetPeelingCandidate(candidate, trip_count);

  // Success!
  return true;
}

void HLoopOptimization::Vectorize(LoopNode* node,
                                  HBasicBlock* block,
                                  HBasicBlock* exit,
                                  int64_t trip_count) {
  Primitive::Type induc_type = Primitive::kPrimInt;
  HBasicBlock* header = node->loop_info->GetHeader();
  HBasicBlock* preheader = node->loop_info->GetPreHeader();

  // Pick a loop unrolling factor for the vector loop.
  uint32_t unroll = GetUnrollingFactor(block, trip_count);
  uint32_t chunk = vector_length_ * unroll;

  // A cleanup loop is needed, at least, for any unknown trip count or
  // for a known trip count with remainder iterations after vectorization.
  bool needs_cleanup = trip_count == 0 || (trip_count % chunk) != 0;

  // Adjust vector bookkeeping.
  HPhi* main_phi = nullptr;
  bool is_simple_loop_header = TrySetSimpleLoopHeader(header, &main_phi);  // refills sets
  DCHECK(is_simple_loop_header);
  vector_header_ = header;
  vector_body_ = block;

  // Generate dynamic loop peeling trip count, if needed, under the assumption
  // that the Android runtime guarantees at least "component size" alignment:
  // ptc = (ALIGN - (&a[initial] % ALIGN)) / type-size
  HInstruction* ptc = nullptr;
  if (vector_peeling_candidate_ != nullptr) {
    DCHECK_LT(vector_length_, trip_count) << "dynamic peeling currently requires known trip count";
    //
    // TODO: Implement this. Compute address of first access memory location and
    //       compute peeling factor to obtain kAlignedBase alignment.
    //
    needs_cleanup = true;
  }

  // Generate loop control:
  // stc = <trip-count>;
  // vtc = stc - (stc - ptc) % chunk;
  // i = 0;
  HInstruction* stc = induction_range_.GenerateTripCount(node->loop_info, graph_, preheader);
  HInstruction* vtc = stc;
  if (needs_cleanup) {
    DCHECK(IsPowerOfTwo(chunk));
    HInstruction* diff = stc;
    if (ptc != nullptr) {
      diff = Insert(preheader, new (global_allocator_) HSub(induc_type, stc, ptc));
    }
    HInstruction* rem = Insert(
        preheader, new (global_allocator_) HAnd(induc_type,
                                                diff,
                                                graph_->GetIntConstant(chunk - 1)));
    vtc = Insert(preheader, new (global_allocator_) HSub(induc_type, stc, rem));
  }
  vector_index_ = graph_->GetIntConstant(0);

  // Generate runtime disambiguation test:
  // vtc = a != b ? vtc : 0;
  if (vector_runtime_test_a_ != nullptr) {
    HInstruction* rt = Insert(
        preheader,
        new (global_allocator_) HNotEqual(vector_runtime_test_a_, vector_runtime_test_b_));
    vtc = Insert(preheader,
                 new (global_allocator_) HSelect(rt, vtc, graph_->GetIntConstant(0), kNoDexPc));
    needs_cleanup = true;
  }

  // Generate dynamic peeling loop for alignment, if needed:
  // for ( ; i < ptc; i += 1)
  //    <loop-body>
  if (ptc != nullptr) {
    vector_mode_ = kSequential;
    GenerateNewLoop(node,
                    block,
                    graph_->TransformLoopForVectorization(vector_header_, vector_body_, exit),
                    vector_index_,
                    ptc,
                    graph_->GetIntConstant(1),
                    /*unroll*/ 1);
  }

  // Generate vector loop, possibly further unrolled:
  // for ( ; i < vtc; i += chunk)
  //    <vectorized-loop-body>
  vector_mode_ = kVector;
  GenerateNewLoop(node,
                  block,
                  graph_->TransformLoopForVectorization(vector_header_, vector_body_, exit),
                  vector_index_,
                  vtc,
                  graph_->GetIntConstant(vector_length_),  // increment per unroll
                  unroll);
  HLoopInformation* vloop = vector_header_->GetLoopInformation();

  // Generate cleanup loop, if needed:
  // for ( ; i < stc; i += 1)
  //    <loop-body>
  if (needs_cleanup) {
    vector_mode_ = kSequential;
    GenerateNewLoop(node,
                    block,
                    graph_->TransformLoopForVectorization(vector_header_, vector_body_, exit),
                    vector_index_,
                    stc,
                    graph_->GetIntConstant(1),
                    /*unroll*/ 1);
  }

  // Link reductions to their final uses.
  for (auto i = reductions_->begin(); i != reductions_->end(); ++i) {
    if (i->first->IsPhi()) {
      i->first->ReplaceWith(ReduceAndExtractIfNeeded(i->second));
    }
  }

  // Remove the original loop by disconnecting the body block
  // and removing all instructions from the header.
  block->DisconnectAndDelete();
  while (!header->GetFirstInstruction()->IsGoto()) {
    header->RemoveInstruction(header->GetFirstInstruction());
  }

  // Update loop hierarchy: the old header now resides in the same outer loop
  // as the old preheader. Note that we don't bother putting sequential
  // loops back in the hierarchy at this point.
  header->SetLoopInformation(preheader->GetLoopInformation());  // outward
  node->loop_info = vloop;
}

void HLoopOptimization::GenerateNewLoop(LoopNode* node,
                                        HBasicBlock* block,
                                        HBasicBlock* new_preheader,
                                        HInstruction* lo,
                                        HInstruction* hi,
                                        HInstruction* step,
                                        uint32_t unroll) {
  DCHECK(unroll == 1 || vector_mode_ == kVector);
  Primitive::Type induc_type = Primitive::kPrimInt;
  // Prepare new loop.
  vector_preheader_ = new_preheader,
  vector_header_ = vector_preheader_->GetSingleSuccessor();
  vector_body_ = vector_header_->GetSuccessors()[1];
  HPhi* phi = new (global_allocator_) HPhi(global_allocator_,
                                           kNoRegNumber,
                                           0,
                                           HPhi::ToPhiType(induc_type));
  // Generate header and prepare body.
  // for (i = lo; i < hi; i += step)
  //    <loop-body>
  HInstruction* cond = new (global_allocator_) HAboveOrEqual(phi, hi);
  vector_header_->AddPhi(phi);
  vector_header_->AddInstruction(cond);
  vector_header_->AddInstruction(new (global_allocator_) HIf(cond));
  vector_index_ = phi;
  vector_permanent_map_->clear();  // preserved over unrolling
  for (uint32_t u = 0; u < unroll; u++) {
    // Generate instruction map.
    vector_map_->clear();
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      bool vectorized_def = VectorizeDef(node, it.Current(), /*generate_code*/ true);
      DCHECK(vectorized_def);
    }
    // Generate body from the instruction map, but in original program order.
    HEnvironment* env = vector_header_->GetFirstInstruction()->GetEnvironment();
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      auto i = vector_map_->find(it.Current());
      if (i != vector_map_->end() && !i->second->IsInBlock()) {
        Insert(vector_body_, i->second);
        // Deal with instructions that need an environment, such as the scalar intrinsics.
        if (i->second->NeedsEnvironment()) {
          i->second->CopyEnvironmentFromWithLoopPhiAdjustment(env, vector_header_);
        }
      }
    }
    // Generate the induction.
    vector_index_ = new (global_allocator_) HAdd(induc_type, vector_index_, step);
    Insert(vector_body_, vector_index_);
  }
  // Finalize phi inputs for the reductions (if any).
  for (auto i = reductions_->begin(); i != reductions_->end(); ++i) {
    if (!i->first->IsPhi()) {
      DCHECK(i->second->IsPhi());
      GenerateVecReductionPhiInputs(i->second->AsPhi(), i->first);
    }
  }
  // Finalize phi inputs for the loop index.
  phi->AddInput(lo);
  phi->AddInput(vector_index_);
  vector_index_ = phi;
}

bool HLoopOptimization::VectorizeDef(LoopNode* node,
                                     HInstruction* instruction,
                                     bool generate_code) {
  // Accept a left-hand-side array base[index] for
  // (1) supported vector type,
  // (2) loop-invariant base,
  // (3) unit stride index,
  // (4) vectorizable right-hand-side value.
  uint64_t restrictions = kNone;
  if (instruction->IsArraySet()) {
    Primitive::Type type = instruction->AsArraySet()->GetComponentType();
    HInstruction* base = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    HInstruction* value = instruction->InputAt(2);
    HInstruction* offset = nullptr;
    if (TrySetVectorType(type, &restrictions) &&
        node->loop_info->IsDefinedOutOfTheLoop(base) &&
        induction_range_.IsUnitStride(instruction, index, graph_, &offset) &&
        VectorizeUse(node, value, generate_code, type, restrictions)) {
      if (generate_code) {
        GenerateVecSub(index, offset);
        GenerateVecMem(instruction, vector_map_->Get(index), vector_map_->Get(value), offset, type);
      } else {
        vector_refs_->insert(ArrayReference(base, offset, type, /*lhs*/ true));
      }
      return true;
    }
    return false;
  }
  // Accept a left-hand-side reduction for
  // (1) supported vector type,
  // (2) vectorizable right-hand-side value.
  auto redit = reductions_->find(instruction);
  if (redit != reductions_->end()) {
    Primitive::Type type = instruction->GetType();
    if (TrySetVectorType(type, &restrictions) &&
        VectorizeUse(node, instruction, generate_code, type, restrictions)) {
      if (generate_code) {
        HInstruction* new_red = vector_map_->Get(instruction);
        vector_permanent_map_->Put(new_red, vector_map_->Get(redit->second));
        vector_permanent_map_->Overwrite(redit->second, new_red);
      }
      return true;
    }
    return false;
  }
  // Branch back okay.
  if (instruction->IsGoto()) {
    return true;
  }
  // Otherwise accept only expressions with no effects outside the immediate loop-body.
  // Note that actual uses are inspected during right-hand-side tree traversal.
  return !IsUsedOutsideLoop(node->loop_info, instruction) && !instruction->DoesAnyWrite();
}

// TODO: saturation arithmetic.
bool HLoopOptimization::VectorizeUse(LoopNode* node,
                                     HInstruction* instruction,
                                     bool generate_code,
                                     Primitive::Type type,
                                     uint64_t restrictions) {
  // Accept anything for which code has already been generated.
  if (generate_code) {
    if (vector_map_->find(instruction) != vector_map_->end()) {
      return true;
    }
  }
  // Continue the right-hand-side tree traversal, passing in proper
  // types and vector restrictions along the way. During code generation,
  // all new nodes are drawn from the global allocator.
  if (node->loop_info->IsDefinedOutOfTheLoop(instruction)) {
    // Accept invariant use, using scalar expansion.
    if (generate_code) {
      GenerateVecInv(instruction, type);
    }
    return true;
  } else if (instruction->IsArrayGet()) {
    // Deal with vector restrictions.
    if (instruction->AsArrayGet()->IsStringCharAt() &&
        HasVectorRestrictions(restrictions, kNoStringCharAt)) {
      return false;
    }
    // Accept a right-hand-side array base[index] for
    // (1) exact matching vector type,
    // (2) loop-invariant base,
    // (3) unit stride index,
    // (4) vectorizable right-hand-side value.
    HInstruction* base = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    HInstruction* offset = nullptr;
    if (type == instruction->GetType() &&
        node->loop_info->IsDefinedOutOfTheLoop(base) &&
        induction_range_.IsUnitStride(instruction, index, graph_, &offset)) {
      if (generate_code) {
        GenerateVecSub(index, offset);
        GenerateVecMem(instruction, vector_map_->Get(index), nullptr, offset, type);
      } else {
        vector_refs_->insert(ArrayReference(base, offset, type, /*lhs*/ false));
      }
      return true;
    }
  } else if (instruction->IsPhi()) {
    // Accept particular phi operations.
    if (reductions_->find(instruction) != reductions_->end()) {
      // Deal with vector restrictions.
      if (HasVectorRestrictions(restrictions, kNoReduction)) {
        return false;
      }
      // Accept a reduction.
      if (generate_code) {
        GenerateVecReductionPhi(instruction->AsPhi());
      }
      return true;
    }
    // TODO: accept right-hand-side induction?
    return false;
  } else if (instruction->IsTypeConversion()) {
    // Accept particular type conversions.
    HTypeConversion* conversion = instruction->AsTypeConversion();
    HInstruction* opa = conversion->InputAt(0);
    Primitive::Type from = conversion->GetInputType();
    Primitive::Type to = conversion->GetResultType();
    if ((to == Primitive::kPrimByte ||
         to == Primitive::kPrimChar ||
         to == Primitive::kPrimShort) && from == Primitive::kPrimInt) {
      // Accept a "narrowing" type conversion from a "wider" computation for
      // (1) conversion into final required type,
      // (2) vectorizable operand,
      // (3) "wider" operations cannot bring in higher order bits.
      if (to == type && VectorizeUse(node, opa, generate_code, type, restrictions | kNoHiBits)) {
        if (generate_code) {
          if (vector_mode_ == kVector) {
            vector_map_->Put(instruction, vector_map_->Get(opa));  // operand pass-through
          } else {
            GenerateVecOp(instruction, vector_map_->Get(opa), nullptr, type);
          }
        }
        return true;
      }
    } else if (to == Primitive::kPrimFloat && from == Primitive::kPrimInt) {
      DCHECK_EQ(to, type);
      // Accept int to float conversion for
      // (1) supported int,
      // (2) vectorizable operand.
      if (TrySetVectorType(from, &restrictions) &&
          VectorizeUse(node, opa, generate_code, from, restrictions)) {
        if (generate_code) {
          GenerateVecOp(instruction, vector_map_->Get(opa), nullptr, type);
        }
        return true;
      }
    }
    return false;
  } else if (instruction->IsNeg() || instruction->IsNot() || instruction->IsBooleanNot()) {
    // Accept unary operator for vectorizable operand.
    HInstruction* opa = instruction->InputAt(0);
    if (VectorizeUse(node, opa, generate_code, type, restrictions)) {
      if (generate_code) {
        GenerateVecOp(instruction, vector_map_->Get(opa), nullptr, type);
      }
      return true;
    }
  } else if (instruction->IsAdd() || instruction->IsSub() ||
             instruction->IsMul() || instruction->IsDiv() ||
             instruction->IsAnd() || instruction->IsOr()  || instruction->IsXor()) {
    // Deal with vector restrictions.
    if ((instruction->IsMul() && HasVectorRestrictions(restrictions, kNoMul)) ||
        (instruction->IsDiv() && HasVectorRestrictions(restrictions, kNoDiv))) {
      return false;
    }
    // Accept binary operator for vectorizable operands.
    HInstruction* opa = instruction->InputAt(0);
    HInstruction* opb = instruction->InputAt(1);
    if (VectorizeUse(node, opa, generate_code, type, restrictions) &&
        VectorizeUse(node, opb, generate_code, type, restrictions)) {
      if (generate_code) {
        GenerateVecOp(instruction, vector_map_->Get(opa), vector_map_->Get(opb), type);
      }
      return true;
    }
  } else if (instruction->IsShl() || instruction->IsShr() || instruction->IsUShr()) {
    // Recognize vectorization idioms.
    if (VectorizeHalvingAddIdiom(node, instruction, generate_code, type, restrictions)) {
      return true;
    }
    // Deal with vector restrictions.
    HInstruction* opa = instruction->InputAt(0);
    HInstruction* opb = instruction->InputAt(1);
    HInstruction* r = opa;
    bool is_unsigned = false;
    if ((HasVectorRestrictions(restrictions, kNoShift)) ||
        (instruction->IsShr() && HasVectorRestrictions(restrictions, kNoShr))) {
      return false;  // unsupported instruction
    } else if (HasVectorRestrictions(restrictions, kNoHiBits)) {
      // Shifts right need extra care to account for higher order bits.
      // TODO: less likely shr/unsigned and ushr/signed can by flipping signess.
      if (instruction->IsShr() &&
          (!IsNarrowerOperand(opa, type, &r, &is_unsigned) || is_unsigned)) {
        return false;  // reject, unless all operands are sign-extension narrower
      } else if (instruction->IsUShr() &&
                 (!IsNarrowerOperand(opa, type, &r, &is_unsigned) || !is_unsigned)) {
        return false;  // reject, unless all operands are zero-extension narrower
      }
    }
    // Accept shift operator for vectorizable/invariant operands.
    // TODO: accept symbolic, albeit loop invariant shift factors.
    DCHECK(r != nullptr);
    if (generate_code && vector_mode_ != kVector) {  // de-idiom
      r = opa;
    }
    int64_t distance = 0;
    if (VectorizeUse(node, r, generate_code, type, restrictions) &&
        IsInt64AndGet(opb, /*out*/ &distance)) {
      // Restrict shift distance to packed data type width.
      int64_t max_distance = Primitive::ComponentSize(type) * 8;
      if (0 <= distance && distance < max_distance) {
        if (generate_code) {
          GenerateVecOp(instruction, vector_map_->Get(r), opb, type);
        }
        return true;
      }
    }
  } else if (instruction->IsInvokeStaticOrDirect()) {
    // Accept particular intrinsics.
    HInvokeStaticOrDirect* invoke = instruction->AsInvokeStaticOrDirect();
    switch (invoke->GetIntrinsic()) {
      case Intrinsics::kMathAbsInt:
      case Intrinsics::kMathAbsLong:
      case Intrinsics::kMathAbsFloat:
      case Intrinsics::kMathAbsDouble: {
        // Deal with vector restrictions.
        HInstruction* opa = instruction->InputAt(0);
        HInstruction* r = opa;
        bool is_unsigned = false;
        if (HasVectorRestrictions(restrictions, kNoAbs)) {
          return false;
        } else if (HasVectorRestrictions(restrictions, kNoHiBits) &&
                   (!IsNarrowerOperand(opa, type, &r, &is_unsigned) || is_unsigned)) {
          return false;  // reject, unless operand is sign-extension narrower
        }
        // Accept ABS(x) for vectorizable operand.
        DCHECK(r != nullptr);
        if (generate_code && vector_mode_ != kVector) {  // de-idiom
          r = opa;
        }
        if (VectorizeUse(node, r, generate_code, type, restrictions)) {
          if (generate_code) {
            GenerateVecOp(instruction, vector_map_->Get(r), nullptr, type);
          }
          return true;
        }
        return false;
      }
      case Intrinsics::kMathMinIntInt:
      case Intrinsics::kMathMinLongLong:
      case Intrinsics::kMathMinFloatFloat:
      case Intrinsics::kMathMinDoubleDouble:
      case Intrinsics::kMathMaxIntInt:
      case Intrinsics::kMathMaxLongLong:
      case Intrinsics::kMathMaxFloatFloat:
      case Intrinsics::kMathMaxDoubleDouble: {
        // Deal with vector restrictions.
        HInstruction* opa = instruction->InputAt(0);
        HInstruction* opb = instruction->InputAt(1);
        HInstruction* r = opa;
        HInstruction* s = opb;
        bool is_unsigned = false;
        if (HasVectorRestrictions(restrictions, kNoMinMax)) {
          return false;
        } else if (HasVectorRestrictions(restrictions, kNoHiBits) &&
                   !IsNarrowerOperands(opa, opb, type, &r, &s, &is_unsigned)) {
          return false;  // reject, unless all operands are same-extension narrower
        }
        // Accept MIN/MAX(x, y) for vectorizable operands.
        DCHECK(r != nullptr && s != nullptr);
        if (generate_code && vector_mode_ != kVector) {  // de-idiom
          r = opa;
          s = opb;
        }
        if (VectorizeUse(node, r, generate_code, type, restrictions) &&
            VectorizeUse(node, s, generate_code, type, restrictions)) {
          if (generate_code) {
            GenerateVecOp(
                instruction, vector_map_->Get(r), vector_map_->Get(s), type, is_unsigned);
          }
          return true;
        }
        return false;
      }
      default:
        return false;
    }  // switch
  }
  return false;
}

bool HLoopOptimization::TrySetVectorType(Primitive::Type type, uint64_t* restrictions) {
  const InstructionSetFeatures* features = compiler_driver_->GetInstructionSetFeatures();
  switch (compiler_driver_->GetInstructionSet()) {
    case kArm:
    case kThumb2:
      // Allow vectorization for all ARM devices, because Android assumes that
      // ARM 32-bit always supports advanced SIMD (64-bit SIMD).
      switch (type) {
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
          *restrictions |= kNoDiv | kNoReduction;
          return TrySetVectorLength(8);
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
          *restrictions |= kNoDiv | kNoStringCharAt | kNoReduction;
          return TrySetVectorLength(4);
        case Primitive::kPrimInt:
          *restrictions |= kNoDiv | kNoReduction;
          return TrySetVectorLength(2);
        default:
          break;
      }
      return false;
    case kArm64:
      // Allow vectorization for all ARM devices, because Android assumes that
      // ARMv8 AArch64 always supports advanced SIMD (128-bit SIMD).
      switch (type) {
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
          *restrictions |= kNoDiv | kNoReduction;
          return TrySetVectorLength(16);
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
          *restrictions |= kNoDiv | kNoReduction;
          return TrySetVectorLength(8);
        case Primitive::kPrimInt:
          *restrictions |= kNoDiv;
          return TrySetVectorLength(4);
        case Primitive::kPrimLong:
          *restrictions |= kNoDiv | kNoMul | kNoMinMax;
          return TrySetVectorLength(2);
        case Primitive::kPrimFloat:
          *restrictions |= kNoReduction;
          return TrySetVectorLength(4);
        case Primitive::kPrimDouble:
          *restrictions |= kNoReduction;
          return TrySetVectorLength(2);
        default:
          return false;
      }
    case kX86:
    case kX86_64:
      // Allow vectorization for SSE4.1-enabled X86 devices only (128-bit SIMD).
      if (features->AsX86InstructionSetFeatures()->HasSSE4_1()) {
        switch (type) {
          case Primitive::kPrimBoolean:
          case Primitive::kPrimByte:
            *restrictions |=
                kNoMul | kNoDiv | kNoShift | kNoAbs | kNoSignedHAdd | kNoUnroundedHAdd | kNoReduction;
            return TrySetVectorLength(16);
          case Primitive::kPrimChar:
          case Primitive::kPrimShort:
            *restrictions |= kNoDiv | kNoAbs | kNoSignedHAdd | kNoUnroundedHAdd | kNoReduction;
            return TrySetVectorLength(8);
          case Primitive::kPrimInt:
            *restrictions |= kNoDiv;
            return TrySetVectorLength(4);
          case Primitive::kPrimLong:
            *restrictions |= kNoMul | kNoDiv | kNoShr | kNoAbs | kNoMinMax;
            return TrySetVectorLength(2);
          case Primitive::kPrimFloat:
            *restrictions |= kNoMinMax | kNoReduction;  // minmax: -0.0 vs +0.0
            return TrySetVectorLength(4);
          case Primitive::kPrimDouble:
            *restrictions |= kNoMinMax | kNoReduction;  // minmax: -0.0 vs +0.0
            return TrySetVectorLength(2);
          default:
            break;
        }  // switch type
      }
      return false;
    case kMips:
      if (features->AsMipsInstructionSetFeatures()->HasMsa()) {
        switch (type) {
          case Primitive::kPrimBoolean:
          case Primitive::kPrimByte:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(16);
          case Primitive::kPrimChar:
          case Primitive::kPrimShort:
            *restrictions |= kNoDiv | kNoStringCharAt | kNoReduction;
            return TrySetVectorLength(8);
          case Primitive::kPrimInt:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(4);
          case Primitive::kPrimLong:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(2);
          case Primitive::kPrimFloat:
            *restrictions |= kNoMinMax | kNoReduction;  // min/max(x, NaN)
            return TrySetVectorLength(4);
          case Primitive::kPrimDouble:
            *restrictions |= kNoMinMax | kNoReduction;  // min/max(x, NaN)
            return TrySetVectorLength(2);
          default:
            break;
        }  // switch type
      }
      return false;
    case kMips64:
      if (features->AsMips64InstructionSetFeatures()->HasMsa()) {
        switch (type) {
          case Primitive::kPrimBoolean:
          case Primitive::kPrimByte:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(16);
          case Primitive::kPrimChar:
          case Primitive::kPrimShort:
            *restrictions |= kNoDiv | kNoStringCharAt | kNoReduction;
            return TrySetVectorLength(8);
          case Primitive::kPrimInt:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(4);
          case Primitive::kPrimLong:
            *restrictions |= kNoDiv | kNoReduction;
            return TrySetVectorLength(2);
          case Primitive::kPrimFloat:
            *restrictions |= kNoMinMax | kNoReduction;  // min/max(x, NaN)
            return TrySetVectorLength(4);
          case Primitive::kPrimDouble:
            *restrictions |= kNoMinMax | kNoReduction;  // min/max(x, NaN)
            return TrySetVectorLength(2);
          default:
            break;
        }  // switch type
      }
      return false;
    default:
      return false;
  }  // switch instruction set
}

bool HLoopOptimization::TrySetVectorLength(uint32_t length) {
  DCHECK(IsPowerOfTwo(length) && length >= 2u);
  // First time set?
  if (vector_length_ == 0) {
    vector_length_ = length;
  }
  // Different types are acceptable within a loop-body, as long as all the corresponding vector
  // lengths match exactly to obtain a uniform traversal through the vector iteration space
  // (idiomatic exceptions to this rule can be handled by further unrolling sub-expressions).
  return vector_length_ == length;
}

void HLoopOptimization::GenerateVecInv(HInstruction* org, Primitive::Type type) {
  if (vector_map_->find(org) == vector_map_->end()) {
    // In scalar code, just use a self pass-through for scalar invariants
    // (viz. expression remains itself).
    if (vector_mode_ == kSequential) {
      vector_map_->Put(org, org);
      return;
    }
    // In vector code, explicit scalar expansion is needed.
    HInstruction* vector = nullptr;
    auto it = vector_permanent_map_->find(org);
    if (it != vector_permanent_map_->end()) {
      vector = it->second;  // reuse during unrolling
    } else {
      vector = new (global_allocator_) HVecReplicateScalar(
          global_allocator_, org, type, vector_length_);
      vector_permanent_map_->Put(org, Insert(vector_preheader_, vector));
    }
    vector_map_->Put(org, vector);
  }
}

void HLoopOptimization::GenerateVecSub(HInstruction* org, HInstruction* offset) {
  if (vector_map_->find(org) == vector_map_->end()) {
    HInstruction* subscript = vector_index_;
    int64_t value = 0;
    if (!IsInt64AndGet(offset, &value) || value != 0) {
      subscript = new (global_allocator_) HAdd(Primitive::kPrimInt, subscript, offset);
      if (org->IsPhi()) {
        Insert(vector_body_, subscript);  // lacks layout placeholder
      }
    }
    vector_map_->Put(org, subscript);
  }
}

void HLoopOptimization::GenerateVecMem(HInstruction* org,
                                       HInstruction* opa,
                                       HInstruction* opb,
                                       HInstruction* offset,
                                       Primitive::Type type) {
  HInstruction* vector = nullptr;
  if (vector_mode_ == kVector) {
    // Vector store or load.
    HInstruction* base = org->InputAt(0);
    if (opb != nullptr) {
      vector = new (global_allocator_) HVecStore(
          global_allocator_, base, opa, opb, type, vector_length_);
    } else  {
      bool is_string_char_at = org->AsArrayGet()->IsStringCharAt();
      vector = new (global_allocator_) HVecLoad(
          global_allocator_, base, opa, type, vector_length_, is_string_char_at);
    }
    // Known dynamically enforced alignment?
    if (vector_peeling_candidate_ != nullptr &&
        vector_peeling_candidate_->base == base &&
        vector_peeling_candidate_->offset == offset) {
      vector->AsVecMemoryOperation()->SetAlignment(Alignment(kAlignedBase, 0));
    }
  } else {
    // Scalar store or load.
    DCHECK(vector_mode_ == kSequential);
    if (opb != nullptr) {
      vector = new (global_allocator_) HArraySet(org->InputAt(0), opa, opb, type, kNoDexPc);
    } else  {
      bool is_string_char_at = org->AsArrayGet()->IsStringCharAt();
      vector = new (global_allocator_) HArrayGet(
          org->InputAt(0), opa, type, kNoDexPc, is_string_char_at);
    }
  }
  vector_map_->Put(org, vector);
}

void HLoopOptimization::GenerateVecReductionPhi(HPhi* phi) {
  DCHECK(reductions_->find(phi) != reductions_->end());
  DCHECK(reductions_->Get(phi->InputAt(1)) == phi);
  HInstruction* vector = nullptr;
  if (vector_mode_ == kSequential) {
    HPhi* new_phi = new (global_allocator_) HPhi(
        global_allocator_, kNoRegNumber, 0, phi->GetType());
    vector_header_->AddPhi(new_phi);
    vector = new_phi;
  } else {
    // Link vector reduction back to prior unrolled update, or a first phi.
    auto it = vector_permanent_map_->find(phi);
    if (it != vector_permanent_map_->end()) {
      vector = it->second;
    } else {
      HPhi* new_phi = new (global_allocator_) HPhi(
          global_allocator_, kNoRegNumber, 0, HVecOperation::kSIMDType);
      vector_header_->AddPhi(new_phi);
      vector = new_phi;
    }
  }
  vector_map_->Put(phi, vector);
}

void HLoopOptimization::GenerateVecReductionPhiInputs(HPhi* phi, HInstruction* reduction) {
  HInstruction* new_phi = vector_map_->Get(phi);
  HInstruction* new_init = reductions_->Get(phi);
  HInstruction* new_red = vector_map_->Get(reduction);
  // Link unrolled vector loop back to new phi.
  for (; !new_phi->IsPhi(); new_phi = vector_permanent_map_->Get(new_phi)) {
    DCHECK(new_phi->IsVecOperation());
  }
  // Prepare the new initialization.
  if (vector_mode_ == kVector) {
    // Generate a [initial, 0, .., 0] vector.
    new_init = Insert(
            vector_preheader_,
            new (global_allocator_) HVecSetScalars(
                global_allocator_, &new_init, phi->GetType(), vector_length_, 1));
  } else {
    new_init = ReduceAndExtractIfNeeded(new_init);
  }
  // Set the phi inputs.
  DCHECK(new_phi->IsPhi());
  new_phi->AsPhi()->AddInput(new_init);
  new_phi->AsPhi()->AddInput(new_red);
  // New feed value for next phi (safe mutation in iteration).
  reductions_->find(phi)->second = new_phi;
}

HInstruction* HLoopOptimization::ReduceAndExtractIfNeeded(HInstruction* instruction) {
  if (instruction->IsPhi()) {
    HInstruction* input = instruction->InputAt(1);
    if (input->IsVecOperation()) {
      Primitive::Type type = input->AsVecOperation()->GetPackedType();
      HBasicBlock* exit = instruction->GetBlock()->GetSuccessors()[0];
      // Generate a vector reduction and scalar extract
      //    x = REDUCE( [x_1, .., x_n] )
      //    y = x_1
      // along the exit of the defining loop.
      HVecReduce::ReductionKind kind = GetReductionKind(input);
      HInstruction* reduce = new (global_allocator_) HVecReduce(
          global_allocator_, instruction, type, vector_length_, kind);
      exit->InsertInstructionBefore(reduce, exit->GetFirstInstruction());
      instruction = new (global_allocator_) HVecExtractScalar(
          global_allocator_, reduce, type, vector_length_, 0);
      exit->InsertInstructionAfter(instruction, reduce);
    }
  }
  return instruction;
}

#define GENERATE_VEC(x, y) \
  if (vector_mode_ == kVector) { \
    vector = (x); \
  } else { \
    DCHECK(vector_mode_ == kSequential); \
    vector = (y); \
  } \
  break;

void HLoopOptimization::GenerateVecOp(HInstruction* org,
                                      HInstruction* opa,
                                      HInstruction* opb,
                                      Primitive::Type type,
                                      bool is_unsigned) {
  if (vector_mode_ == kSequential) {
    // Non-converting scalar code follows implicit integral promotion.
    if (!org->IsTypeConversion() && (type == Primitive::kPrimBoolean ||
                                     type == Primitive::kPrimByte ||
                                     type == Primitive::kPrimChar ||
                                     type == Primitive::kPrimShort)) {
      type = Primitive::kPrimInt;
    }
  }
  HInstruction* vector = nullptr;
  switch (org->GetKind()) {
    case HInstruction::kNeg:
      DCHECK(opb == nullptr);
      GENERATE_VEC(
          new (global_allocator_) HVecNeg(global_allocator_, opa, type, vector_length_),
          new (global_allocator_) HNeg(type, opa));
    case HInstruction::kNot:
      DCHECK(opb == nullptr);
      GENERATE_VEC(
          new (global_allocator_) HVecNot(global_allocator_, opa, type, vector_length_),
          new (global_allocator_) HNot(type, opa));
    case HInstruction::kBooleanNot:
      DCHECK(opb == nullptr);
      GENERATE_VEC(
          new (global_allocator_) HVecNot(global_allocator_, opa, type, vector_length_),
          new (global_allocator_) HBooleanNot(opa));
    case HInstruction::kTypeConversion:
      DCHECK(opb == nullptr);
      GENERATE_VEC(
          new (global_allocator_) HVecCnv(global_allocator_, opa, type, vector_length_),
          new (global_allocator_) HTypeConversion(type, opa, kNoDexPc));
    case HInstruction::kAdd:
      GENERATE_VEC(
          new (global_allocator_) HVecAdd(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HAdd(type, opa, opb));
    case HInstruction::kSub:
      GENERATE_VEC(
          new (global_allocator_) HVecSub(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HSub(type, opa, opb));
    case HInstruction::kMul:
      GENERATE_VEC(
          new (global_allocator_) HVecMul(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HMul(type, opa, opb));
    case HInstruction::kDiv:
      GENERATE_VEC(
          new (global_allocator_) HVecDiv(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HDiv(type, opa, opb, kNoDexPc));
    case HInstruction::kAnd:
      GENERATE_VEC(
          new (global_allocator_) HVecAnd(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HAnd(type, opa, opb));
    case HInstruction::kOr:
      GENERATE_VEC(
          new (global_allocator_) HVecOr(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HOr(type, opa, opb));
    case HInstruction::kXor:
      GENERATE_VEC(
          new (global_allocator_) HVecXor(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HXor(type, opa, opb));
    case HInstruction::kShl:
      GENERATE_VEC(
          new (global_allocator_) HVecShl(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HShl(type, opa, opb));
    case HInstruction::kShr:
      GENERATE_VEC(
          new (global_allocator_) HVecShr(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HShr(type, opa, opb));
    case HInstruction::kUShr:
      GENERATE_VEC(
          new (global_allocator_) HVecUShr(global_allocator_, opa, opb, type, vector_length_),
          new (global_allocator_) HUShr(type, opa, opb));
    case HInstruction::kInvokeStaticOrDirect: {
      HInvokeStaticOrDirect* invoke = org->AsInvokeStaticOrDirect();
      if (vector_mode_ == kVector) {
        switch (invoke->GetIntrinsic()) {
          case Intrinsics::kMathAbsInt:
          case Intrinsics::kMathAbsLong:
          case Intrinsics::kMathAbsFloat:
          case Intrinsics::kMathAbsDouble:
            DCHECK(opb == nullptr);
            vector = new (global_allocator_) HVecAbs(global_allocator_, opa, type, vector_length_);
            break;
          case Intrinsics::kMathMinIntInt:
          case Intrinsics::kMathMinLongLong:
          case Intrinsics::kMathMinFloatFloat:
          case Intrinsics::kMathMinDoubleDouble: {
            vector = new (global_allocator_)
                HVecMin(global_allocator_, opa, opb, type, vector_length_, is_unsigned);
            break;
          }
          case Intrinsics::kMathMaxIntInt:
          case Intrinsics::kMathMaxLongLong:
          case Intrinsics::kMathMaxFloatFloat:
          case Intrinsics::kMathMaxDoubleDouble: {
            vector = new (global_allocator_)
                HVecMax(global_allocator_, opa, opb, type, vector_length_, is_unsigned);
            break;
          }
          default:
            LOG(FATAL) << "Unsupported SIMD intrinsic";
            UNREACHABLE();
        }  // switch invoke
      } else {
        // In scalar code, simply clone the method invoke, and replace its operands with the
        // corresponding new scalar instructions in the loop. The instruction will get an
        // environment while being inserted from the instruction map in original program order.
        DCHECK(vector_mode_ == kSequential);
        size_t num_args = invoke->GetNumberOfArguments();
        HInvokeStaticOrDirect* new_invoke = new (global_allocator_) HInvokeStaticOrDirect(
            global_allocator_,
            num_args,
            invoke->GetType(),
            invoke->GetDexPc(),
            invoke->GetDexMethodIndex(),
            invoke->GetResolvedMethod(),
            invoke->GetDispatchInfo(),
            invoke->GetInvokeType(),
            invoke->GetTargetMethod(),
            invoke->GetClinitCheckRequirement());
        HInputsRef inputs = invoke->GetInputs();
        size_t num_inputs = inputs.size();
        DCHECK_LE(num_args, num_inputs);
        DCHECK_EQ(num_inputs, new_invoke->GetInputs().size());  // both invokes agree
        for (size_t index = 0; index < num_inputs; ++index) {
          HInstruction* new_input = index < num_args
              ? vector_map_->Get(inputs[index])
              : inputs[index];  // beyond arguments: just pass through
          new_invoke->SetArgumentAt(index, new_input);
        }
        new_invoke->SetIntrinsic(invoke->GetIntrinsic(),
                                 kNeedsEnvironmentOrCache,
                                 kNoSideEffects,
                                 kNoThrow);
        vector = new_invoke;
      }
      break;
    }
    default:
      break;
  }  // switch
  CHECK(vector != nullptr) << "Unsupported SIMD operator";
  vector_map_->Put(org, vector);
}

#undef GENERATE_VEC

//
// Vectorization idioms.
//

// Method recognizes the following idioms:
//   rounding halving add (a + b + 1) >> 1 for unsigned/signed operands a, b
//   regular  halving add (a + b)     >> 1 for unsigned/signed operands a, b
// Provided that the operands are promoted to a wider form to do the arithmetic and
// then cast back to narrower form, the idioms can be mapped into efficient SIMD
// implementation that operates directly in narrower form (plus one extra bit).
// TODO: current version recognizes implicit byte/short/char widening only;
//       explicit widening from int to long could be added later.
bool HLoopOptimization::VectorizeHalvingAddIdiom(LoopNode* node,
                                                 HInstruction* instruction,
                                                 bool generate_code,
                                                 Primitive::Type type,
                                                 uint64_t restrictions) {
  // Test for top level arithmetic shift right x >> 1 or logical shift right x >>> 1
  // (note whether the sign bit in wider precision is shifted in has no effect
  // on the narrow precision computed by the idiom).
  if ((instruction->IsShr() ||
       instruction->IsUShr()) &&
      IsInt64Value(instruction->InputAt(1), 1)) {
    // Test for (a + b + c) >> 1 for optional constant c.
    HInstruction* a = nullptr;
    HInstruction* b = nullptr;
    int64_t       c = 0;
    if (IsAddConst(instruction->InputAt(0), /*out*/ &a, /*out*/ &b, /*out*/ &c)) {
      DCHECK(a != nullptr && b != nullptr);
      // Accept c == 1 (rounded) or c == 0 (not rounded).
      bool is_rounded = false;
      if (c == 1) {
        is_rounded = true;
      } else if (c != 0) {
        return false;
      }
      // Accept consistent zero or sign extension on operands a and b.
      HInstruction* r = nullptr;
      HInstruction* s = nullptr;
      bool is_unsigned = false;
      if (!IsNarrowerOperands(a, b, type, &r, &s, &is_unsigned)) {
        return false;
      }
      // Deal with vector restrictions.
      if ((!is_unsigned && HasVectorRestrictions(restrictions, kNoSignedHAdd)) ||
          (!is_rounded && HasVectorRestrictions(restrictions, kNoUnroundedHAdd))) {
        return false;
      }
      // Accept recognized halving add for vectorizable operands. Vectorized code uses the
      // shorthand idiomatic operation. Sequential code uses the original scalar expressions.
      DCHECK(r != nullptr && s != nullptr);
      if (generate_code && vector_mode_ != kVector) {  // de-idiom
        r = instruction->InputAt(0);
        s = instruction->InputAt(1);
      }
      if (VectorizeUse(node, r, generate_code, type, restrictions) &&
          VectorizeUse(node, s, generate_code, type, restrictions)) {
        if (generate_code) {
          if (vector_mode_ == kVector) {
            vector_map_->Put(instruction, new (global_allocator_) HVecHalvingAdd(
                global_allocator_,
                vector_map_->Get(r),
                vector_map_->Get(s),
                type,
                vector_length_,
                is_unsigned,
                is_rounded));
            MaybeRecordStat(stats_, MethodCompilationStat::kLoopVectorizedIdiom);
          } else {
            GenerateVecOp(instruction, vector_map_->Get(r), vector_map_->Get(s), type);
          }
        }
        return true;
      }
    }
  }
  return false;
}

//
// Vectorization heuristics.
//

bool HLoopOptimization::IsVectorizationProfitable(int64_t trip_count) {
  // Current heuristic: non-empty body with sufficient number
  // of iterations (if known).
  // TODO: refine by looking at e.g. operation count, alignment, etc.
  if (vector_length_ == 0) {
    return false;  // nothing found
  } else if (0 < trip_count && trip_count < vector_length_) {
    return false;  // insufficient iterations
  }
  return true;
}

void HLoopOptimization::SetPeelingCandidate(const ArrayReference* candidate,
                                            int64_t trip_count ATTRIBUTE_UNUSED) {
  // Current heuristic: none.
  // TODO: implement
  vector_peeling_candidate_ = candidate;
}

uint32_t HLoopOptimization::GetUnrollingFactor(HBasicBlock* block, int64_t trip_count) {
  // Current heuristic: unroll by 2 on ARM64/X86 for large known trip
  // counts and small loop bodies.
  // TODO: refine with operation count, remaining iterations, etc.
  //       Artem had some really cool ideas for this already.
  switch (compiler_driver_->GetInstructionSet()) {
    case kArm64:
    case kX86:
    case kX86_64: {
      size_t num_instructions = block->GetInstructions().CountSize();
      if (num_instructions <= 10 && trip_count >= 4 * vector_length_) {
        return 2;
      }
      return 1;
    }
    default:
      return 1;
  }
}

//
// Helpers.
//

bool HLoopOptimization::TrySetPhiInduction(HPhi* phi, bool restrict_uses) {
  // Start with empty phi induction.
  iset_->clear();

  // Special case Phis that have equivalent in a debuggable setup. Our graph checker isn't
  // smart enough to follow strongly connected components (and it's probably not worth
  // it to make it so). See b/33775412.
  if (graph_->IsDebuggable() && phi->HasEquivalentPhi()) {
    return false;
  }

  // Lookup phi induction cycle.
  ArenaSet<HInstruction*>* set = induction_range_.LookupCycle(phi);
  if (set != nullptr) {
    for (HInstruction* i : *set) {
      // Check that, other than instructions that are no longer in the graph (removed earlier)
      // each instruction is removable and, when restrict uses are requested, other than for phi,
      // all uses are contained within the cycle.
      if (!i->IsInBlock()) {
        continue;
      } else if (!i->IsRemovable()) {
        return false;
      } else if (i != phi && restrict_uses) {
        // Deal with regular uses.
        for (const HUseListNode<HInstruction*>& use : i->GetUses()) {
          if (set->find(use.GetUser()) == set->end()) {
            return false;
          }
        }
      }
      iset_->insert(i);  // copy
    }
    return true;
  }
  return false;
}

bool HLoopOptimization::TrySetPhiReduction(HPhi* phi) {
  DCHECK(iset_->empty());
  // Only unclassified phi cycles are candidates for reductions.
  if (induction_range_.IsClassified(phi)) {
    return false;
  }
  // Accept operations like x = x + .., provided that the phi and the reduction are
  // used exactly once inside the loop, and by each other.
  HInputsRef inputs = phi->GetInputs();
  if (inputs.size() == 2) {
    HInstruction* reduction = inputs[1];
    if (HasReductionFormat(reduction, phi)) {
      HLoopInformation* loop_info = phi->GetBlock()->GetLoopInformation();
      int32_t use_count = 0;
      bool single_use_inside_loop =
          // Reduction update only used by phi.
          reduction->GetUses().HasExactlyOneElement() &&
          !reduction->HasEnvironmentUses() &&
          // Reduction update is only use of phi inside the loop.
          IsOnlyUsedAfterLoop(loop_info, phi, /*collect_loop_uses*/ true, &use_count) &&
          iset_->size() == 1;
      iset_->clear();  // leave the way you found it
      if (single_use_inside_loop) {
        // Link reduction back, and start recording feed value.
        reductions_->Put(reduction, phi);
        reductions_->Put(phi, phi->InputAt(0));
        return true;
      }
    }
  }
  return false;
}

bool HLoopOptimization::TrySetSimpleLoopHeader(HBasicBlock* block, /*out*/ HPhi** main_phi) {
  // Start with empty phi induction and reductions.
  iset_->clear();
  reductions_->clear();

  // Scan the phis to find the following (the induction structure has already
  // been optimized, so we don't need to worry about trivial cases):
  // (1) optional reductions in loop,
  // (2) the main induction, used in loop control.
  HPhi* phi = nullptr;
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    if (TrySetPhiReduction(it.Current()->AsPhi())) {
      continue;
    } else if (phi == nullptr) {
      // Found the first candidate for main induction.
      phi = it.Current()->AsPhi();
    } else {
      return false;
    }
  }

  // Then test for a typical loopheader:
  //   s:  SuspendCheck
  //   c:  Condition(phi, bound)
  //   i:  If(c)
  if (phi != nullptr && TrySetPhiInduction(phi, /*restrict_uses*/ false)) {
    HInstruction* s = block->GetFirstInstruction();
    if (s != nullptr && s->IsSuspendCheck()) {
      HInstruction* c = s->GetNext();
      if (c != nullptr &&
          c->IsCondition() &&
          c->GetUses().HasExactlyOneElement() &&  // only used for termination
          !c->HasEnvironmentUses()) {  // unlikely, but not impossible
        HInstruction* i = c->GetNext();
        if (i != nullptr && i->IsIf() && i->InputAt(0) == c) {
          iset_->insert(c);
          iset_->insert(s);
          *main_phi = phi;
          return true;
        }
      }
    }
  }
  return false;
}

bool HLoopOptimization::IsEmptyBody(HBasicBlock* block) {
  if (!block->GetPhis().IsEmpty()) {
    return false;
  }
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instruction = it.Current();
    if (!instruction->IsGoto() && iset_->find(instruction) == iset_->end()) {
      return false;
    }
  }
  return true;
}

bool HLoopOptimization::IsUsedOutsideLoop(HLoopInformation* loop_info,
                                          HInstruction* instruction) {
  // Deal with regular uses.
  for (const HUseListNode<HInstruction*>& use : instruction->GetUses()) {
    if (use.GetUser()->GetBlock()->GetLoopInformation() != loop_info) {
      return true;
    }
  }
  return false;
}

bool HLoopOptimization::IsOnlyUsedAfterLoop(HLoopInformation* loop_info,
                                            HInstruction* instruction,
                                            bool collect_loop_uses,
                                            /*out*/ int32_t* use_count) {
  // Deal with regular uses.
  for (const HUseListNode<HInstruction*>& use : instruction->GetUses()) {
    HInstruction* user = use.GetUser();
    if (iset_->find(user) == iset_->end()) {  // not excluded?
      HLoopInformation* other_loop_info = user->GetBlock()->GetLoopInformation();
      if (other_loop_info != nullptr && other_loop_info->IsIn(*loop_info)) {
        // If collect_loop_uses is set, simply keep adding those uses to the set.
        // Otherwise, reject uses inside the loop that were not already in the set.
        if (collect_loop_uses) {
          iset_->insert(user);
          continue;
        }
        return false;
      }
      ++*use_count;
    }
  }
  return true;
}

bool HLoopOptimization::TryReplaceWithLastValue(HLoopInformation* loop_info,
                                                HInstruction* instruction,
                                                HBasicBlock* block) {
  // Try to replace outside uses with the last value.
  if (induction_range_.CanGenerateLastValue(instruction)) {
    HInstruction* replacement = induction_range_.GenerateLastValue(instruction, graph_, block);
    // Deal with regular uses.
    const HUseList<HInstruction*>& uses = instruction->GetUses();
    for (auto it = uses.begin(), end = uses.end(); it != end;) {
      HInstruction* user = it->GetUser();
      size_t index = it->GetIndex();
      ++it;  // increment before replacing
      if (iset_->find(user) == iset_->end()) {  // not excluded?
        if (kIsDebugBuild) {
          // We have checked earlier in 'IsOnlyUsedAfterLoop' that the use is after the loop.
          HLoopInformation* other_loop_info = user->GetBlock()->GetLoopInformation();
          CHECK(other_loop_info == nullptr || !other_loop_info->IsIn(*loop_info));
        }
        user->ReplaceInput(replacement, index);
        induction_range_.Replace(user, instruction, replacement);  // update induction
      }
    }
    // Deal with environment uses.
    const HUseList<HEnvironment*>& env_uses = instruction->GetEnvUses();
    for (auto it = env_uses.begin(), end = env_uses.end(); it != end;) {
      HEnvironment* user = it->GetUser();
      size_t index = it->GetIndex();
      ++it;  // increment before replacing
      if (iset_->find(user->GetHolder()) == iset_->end()) {  // not excluded?
        // Only update environment uses after the loop.
        HLoopInformation* other_loop_info = user->GetHolder()->GetBlock()->GetLoopInformation();
        if (other_loop_info == nullptr || !other_loop_info->IsIn(*loop_info)) {
          user->RemoveAsUserOfInput(index);
          user->SetRawEnvAt(index, replacement);
          replacement->AddEnvUseAt(user, index);
        }
      }
    }
    return true;
  }
  return false;
}

bool HLoopOptimization::TryAssignLastValue(HLoopInformation* loop_info,
                                           HInstruction* instruction,
                                           HBasicBlock* block,
                                           bool collect_loop_uses) {
  // Assigning the last value is always successful if there are no uses.
  // Otherwise, it succeeds in a no early-exit loop by generating the
  // proper last value assignment.
  int32_t use_count = 0;
  return IsOnlyUsedAfterLoop(loop_info, instruction, collect_loop_uses, &use_count) &&
      (use_count == 0 ||
       (!IsEarlyExit(loop_info) && TryReplaceWithLastValue(loop_info, instruction, block)));
}

void HLoopOptimization::RemoveDeadInstructions(const HInstructionList& list) {
  for (HBackwardInstructionIterator i(list); !i.Done(); i.Advance()) {
    HInstruction* instruction = i.Current();
    if (instruction->IsDeadAndRemovable()) {
      simplified_ = true;
      instruction->GetBlock()->RemoveInstructionOrPhi(instruction);
    }
  }
}

bool HLoopOptimization::CanRemoveCycle() {
  for (HInstruction* i : *iset_) {
    // We can never remove instructions that have environment
    // uses when we compile 'debuggable'.
    if (i->HasEnvironmentUses() && graph_->IsDebuggable()) {
      return false;
    }
    // A deoptimization should never have an environment input removed.
    for (const HUseListNode<HEnvironment*>& use : i->GetEnvUses()) {
      if (use.GetUser()->GetHolder()->IsDeoptimize()) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace art
