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

#include "nodes.h"

#include "ssa_builder.h"
#include "utils/growable_array.h"
#include "scoped_thread_state_change.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.Size());
  blocks_.Add(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  ArenaBitVector visiting(arena_, blocks_.Size(), false);
  VisitBlockForBackEdges(entry_block_, visited, &visiting);
}

static void RemoveAsUser(HInstruction* instruction) {
  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->InputAt(i)->RemoveUser(instruction, i);
  }

  HEnvironment* environment = instruction->GetEnvironment();
  if (environment != nullptr) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HUseListNode<HEnvironment*>* vreg_env_use = environment->GetInstructionEnvUseAt(i);
      if (vreg_env_use != nullptr) {
        HInstruction* vreg = environment->GetInstructionAt(i);
        DCHECK(vreg != nullptr);
        vreg->RemoveEnvironmentUser(vreg_env_use);
      }
    }
  }
}

void HGraph::RemoveInstructionsAsUsersFromDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
    }
  }
}

void HGraph::RemoveBlock(HBasicBlock* block) const {
  for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
    block->GetSuccessors().Get(j)->RemovePredecessor(block);
  }
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    block->RemovePhi(it.Current()->AsPhi());
  }
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    block->RemoveInstruction(it.Current());
  }
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      RemoveBlock(blocks_.Get(i));
    }
  }
}

void HGraph::VisitBlockForBackEdges(HBasicBlock* block,
                                    ArenaBitVector* visited,
                                    ArenaBitVector* visiting) {
  int id = block->GetBlockId();
  if (visited->IsBitSet(id)) return;

  visited->SetBit(id);
  visiting->SetBit(id);
  for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
    HBasicBlock* successor = block->GetSuccessors().Get(i);
    if (visiting->IsBitSet(successor->GetBlockId())) {
      successor->AddBackEdge(block);
    } else {
      VisitBlockForBackEdges(successor, visited, visiting);
    }
  }
  visiting->ClearBit(id);
}

void HGraph::BuildDominatorTree() {
  ArenaBitVector visited(arena_, blocks_.Size(), false);

  // (1) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(&visited);

  // (2) Remove instructions and phis from blocks not visited during
  //     the initial DFS as users from other instructions, so that
  //     users can be safely removed before uses later.
  RemoveInstructionsAsUsersFromDeadBlocks(visited);

  // (3) Remove blocks not visited during the initial DFS.
  //     Step (4) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (4) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (5) Compute the immediate dominator of each block. We visit
  //     the successors of a block only when all its forward branches
  //     have been processed.
  GrowableArray<size_t> visits(arena_, blocks_.Size());
  visits.SetSize(blocks_.Size());
  reverse_post_order_.Add(entry_block_);
  for (size_t i = 0; i < entry_block_->GetSuccessors().Size(); i++) {
    VisitBlockForDominatorTree(entry_block_->GetSuccessors().Get(i), entry_block_, &visits);
  }
}

HBasicBlock* HGraph::FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const {
  ArenaBitVector visited(arena_, blocks_.Size(), false);
  // Walk the dominator tree of the first block and mark the visited blocks.
  while (first != nullptr) {
    visited.SetBit(first->GetBlockId());
    first = first->GetDominator();
  }
  // Walk the dominator tree of the second block until a marked block is found.
  while (second != nullptr) {
    if (visited.IsBitSet(second->GetBlockId())) {
      return second;
    }
    second = second->GetDominator();
  }
  LOG(ERROR) << "Could not find common dominator";
  return nullptr;
}

void HGraph::VisitBlockForDominatorTree(HBasicBlock* block,
                                        HBasicBlock* predecessor,
                                        GrowableArray<size_t>* visits) {
  if (block->GetDominator() == nullptr) {
    block->SetDominator(predecessor);
  } else {
    block->SetDominator(FindCommonDominator(block->GetDominator(), predecessor));
  }

  visits->Increment(block->GetBlockId());
  // Once all the forward edges have been visited, we know the immediate
  // dominator of the block. We can then start visiting its successors.
  if (visits->Get(block->GetBlockId()) ==
      block->GetPredecessors().Size() - block->NumberOfBackEdges()) {
    block->GetDominator()->AddDominatedBlock(block);
    reverse_post_order_.Add(block);
    for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
      VisitBlockForDominatorTree(block->GetSuccessors().Get(i), block, visits);
    }
  }
}

void HGraph::TransformToSsa() {
  DCHECK(!reverse_post_order_.IsEmpty());
  SsaBuilder ssa_builder(this);
  ssa_builder.BuildSsa();
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = new (arena_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  new_block->AddInstruction(new (arena_) HGoto());
  block->ReplaceSuccessor(successor, new_block);
  new_block->AddSuccessor(successor);
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // If there are more than one back edge, make them branch to the same block that
  // will become the only back edge. This simplifies finding natural loops in the
  // graph.
  // Also, if the loop is a do/while (that is the back edge is an if), change the
  // back edge to be a goto. This simplifies code generation of suspend cheks.
  if (info->NumberOfBackEdges() > 1 || info->GetBackEdges().Get(0)->GetLastInstruction()->IsIf()) {
    HBasicBlock* new_back_edge = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(new_back_edge);
    new_back_edge->AddInstruction(new (arena_) HGoto());
    for (size_t pred = 0, e = info->GetBackEdges().Size(); pred < e; ++pred) {
      HBasicBlock* back_edge = info->GetBackEdges().Get(pred);
      back_edge->ReplaceSuccessor(header, new_back_edge);
    }
    info->ClearBackEdges();
    info->AddBackEdge(new_back_edge);
    new_back_edge->AddSuccessor(header);
  }

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop.
  size_t number_of_incomings = header->GetPredecessors().Size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto());

    ArenaBitVector back_edges(arena_, GetBlocks().Size(), false);
    HBasicBlock* back_edge = info->GetBackEdges().Get(0);
    for (size_t pred = 0; pred < header->GetPredecessors().Size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors().Get(pred);
      if (predecessor != back_edge) {
        predecessor->ReplaceSuccessor(header, pre_header);
        pred--;
      }
    }
    pre_header->AddSuccessor(header);
  }

  // Make sure the second predecessor of a loop header is the back edge.
  if (header->GetPredecessors().Get(1) != info->GetBackEdges().Get(0)) {
    header->SwapPredecessors();
  }

  // Place the suspend check at the beginning of the header, so that live registers
  // will be known when allocating registers. Note that code generation can still
  // generate the suspend check at the back edge, but needs to be careful with
  // loop phi spill slots (which are not written to at back edge).
  HInstruction* first_instruction = header->GetFirstInstruction();
  if (!first_instruction->IsSuspendCheck()) {
    HSuspendCheck* check = new (arena_) HSuspendCheck(header->GetDexPc());
    header->InsertInstructionBefore(check, first_instruction);
    first_instruction = check;
  }
  info->SetSuspendCheck(first_instruction->AsSuspendCheck());
}

void HGraph::SimplifyCFG() {
  // Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one back edge, and one preheader.
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->GetSuccessors().Size() > 1) {
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        HBasicBlock* successor = block->GetSuccessors().Get(j);
        if (successor->GetPredecessors().Size() > 1) {
          SplitCriticalEdge(block, successor);
          --j;
        }
      }
    }
    if (block->IsLoopHeader()) {
      SimplifyLoop(block);
    }
  }
}

bool HGraph::AnalyzeNaturalLoops() const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->IsLoopHeader()) {
      HLoopInformation* info = block->GetLoopInformation();
      if (!info->Populate()) {
        // Abort if the loop is non natural. We currently bailout in such cases.
        return false;
      }
    }
  }
  return true;
}

HNullConstant* HGraph::GetNullConstant() {
  if (cached_null_constant_ == nullptr) {
    cached_null_constant_ = new (arena_) HNullConstant();
    entry_block_->InsertInstructionBefore(cached_null_constant_,
                                          entry_block_->GetLastInstruction());
  }
  return cached_null_constant_;
}

void HLoopInformation::Add(HBasicBlock* block) {
  blocks_.SetBit(block->GetBlockId());
}

void HLoopInformation::PopulateRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  blocks_.SetBit(block->GetBlockId());
  block->SetInLoop(this);
  for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
    PopulateRecursive(block->GetPredecessors().Get(i));
  }
}

bool HLoopInformation::Populate() {
  DCHECK_EQ(GetBackEdges().Size(), 1u);
  HBasicBlock* back_edge = GetBackEdges().Get(0);
  DCHECK(back_edge->GetDominator() != nullptr);
  if (!header_->Dominates(back_edge)) {
    // This loop is not natural. Do not bother going further.
    return false;
  }

  // Populate this loop: starting with the back edge, recursively add predecessors
  // that are not already part of that loop. Set the header as part of the loop
  // to end the recursion.
  // This is a recursive implementation of the algorithm described in
  // "Advanced Compiler Design & Implementation" (Muchnick) p192.
  blocks_.SetBit(header_->GetBlockId());
  PopulateRecursive(back_edge);
  return true;
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  DCHECK_EQ(header_->GetPredecessors().Size(), 2u);
  return header_->GetDominator();
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

bool HBasicBlock::Dominates(HBasicBlock* other) const {
  // Walk up the dominator tree from `other`, to find out if `this`
  // is an ancestor.
  HBasicBlock* current = other;
  while (current != nullptr) {
    if (current == this) {
      return true;
    }
    current = current->GetDominator();
  }
  return false;
}

static void UpdateInputsUsers(HInstruction* instruction) {
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    instruction->InputAt(i)->AddUseAt(instruction, i);
  }
  // Environment should be created later.
  DCHECK(!instruction->HasEnvironment());
}

void HBasicBlock::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  instruction->next_ = cursor;
  instruction->previous_ = cursor->previous_;
  cursor->previous_ = instruction;
  if (GetFirstInstruction() == cursor) {
    instructions_.first_instruction_ = instruction;
  } else {
    instruction->previous_->next_ = instruction;
  }
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
}

void HBasicBlock::ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                                  HInstruction* replacement) {
  DCHECK(initial->GetBlock() == this);
  InsertInstructionBefore(replacement, initial);
  initial->ReplaceWith(replacement);
  RemoveInstruction(initial);
}

static void Add(HInstructionList* instruction_list,
                HBasicBlock* block,
                HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(block);
  instruction->SetId(block->GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instruction_list->AddInstruction(instruction);
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  Add(&instructions_, this, instruction);
}

void HBasicBlock::AddPhi(HPhi* phi) {
  Add(&phis_, this, phi);
}

void HBasicBlock::InsertPhiAfter(HPhi* phi, HPhi* cursor) {
  DCHECK_EQ(phi->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  if (cursor->next_ == nullptr) {
    cursor->next_ = phi;
    phi->previous_ = cursor;
    DCHECK(phi->next_ == nullptr);
  } else {
    phi->next_ = cursor->next_;
    phi->previous_ = cursor;
    cursor->next_ = phi;
    phi->next_->previous_ = phi;
  }
  phi->SetBlock(this);
  phi->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(phi);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction) {
  DCHECK_EQ(block, instruction->GetBlock());
  DCHECK(instruction->GetUses().IsEmpty());
  DCHECK(instruction->GetEnvUses().IsEmpty());
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);

  RemoveAsUser(instruction);
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction) {
  Remove(&instructions_, this, instruction);
}

void HBasicBlock::RemovePhi(HPhi* phi) {
  Remove(&phis_, this, phi);
}

void HEnvironment::CopyFrom(HEnvironment* env) {
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

template <typename T>
static void RemoveFromUseList(T user, size_t input_index, HUseList<T>* list) {
  HUseListNode<T>* current;
  for (HUseIterator<HInstruction*> use_it(*list); !use_it.Done(); use_it.Advance()) {
    current = use_it.Current();
    if (current->GetUser() == user && current->GetIndex() == input_index) {
      list->Remove(current);
    }
  }
}

HInstruction* HInstruction::GetNextDisregardingMoves() const {
  HInstruction* next = GetNext();
  while (next != nullptr && next->IsParallelMove()) {
    next = next->GetNext();
  }
  return next;
}

HInstruction* HInstruction::GetPreviousDisregardingMoves() const {
  HInstruction* previous = GetPrevious();
  while (previous != nullptr && previous->IsParallelMove()) {
    previous = previous->GetPrevious();
  }
  return previous;
}

void HInstruction::RemoveUser(HInstruction* user, size_t input_index) {
  RemoveFromUseList(user, input_index, &uses_);
}

void HInstruction::RemoveEnvironmentUser(HUseListNode<HEnvironment*>* use) {
  env_uses_.Remove(use);
}

void HInstructionList::AddInstruction(HInstruction* instruction) {
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
}

void HInstructionList::RemoveInstruction(HInstruction* instruction) {
  if (instruction->previous_ != nullptr) {
    instruction->previous_->next_ = instruction->next_;
  }
  if (instruction->next_ != nullptr) {
    instruction->next_->previous_ = instruction->previous_;
  }
  if (instruction == first_instruction_) {
    first_instruction_ = instruction->next_;
  }
  if (instruction == last_instruction_) {
    last_instruction_ = instruction->previous_;
  }
}

bool HInstructionList::Contains(HInstruction* instruction) const {
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction) {
      return true;
    }
  }
  return false;
}

bool HInstructionList::FoundBefore(const HInstruction* instruction1,
                                   const HInstruction* instruction2) const {
  DCHECK_EQ(instruction1->GetBlock(), instruction2->GetBlock());
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction1) {
      return true;
    }
    if (it.Current() == instruction2) {
      return false;
    }
  }
  LOG(FATAL) << "Did not find an order between two instructions of the same block.";
  return true;
}

bool HInstruction::StrictlyDominates(HInstruction* other_instruction) const {
  if (other_instruction == this) {
    // An instruction does not strictly dominate itself.
    return false;
  }
  HBasicBlock* block = GetBlock();
  HBasicBlock* other_block = other_instruction->GetBlock();
  if (block != other_block) {
    return GetBlock()->Dominates(other_instruction->GetBlock());
  } else {
    // If both instructions are in the same block, ensure this
    // instruction comes before `other_instruction`.
    if (IsPhi()) {
      if (!other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // dominates `other_instruction`.
        return true;
      } else {
        // There is no order among phis.
        LOG(FATAL) << "There is no dominance between phis of a same block.";
        return false;
      }
    } else {
      // `this` is not a phi.
      if (other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // does not dominate `other_instruction`.
        return false;
      } else {
        // Check whether this instruction comes before
        // `other_instruction` in the instruction list.
        return block->GetInstructions().FoundBefore(this, other_instruction);
      }
    }
  }
}

void HInstruction::ReplaceWith(HInstruction* other) {
  DCHECK(other != nullptr);
  for (HUseIterator<HInstruction*> it(GetUses()); !it.Done(); it.Advance()) {
    HUseListNode<HInstruction*>* current = it.Current();
    HInstruction* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawInputAt(input_index, other);
    other->AddUseAt(user, input_index);
  }

  for (HUseIterator<HEnvironment*> it(GetEnvUses()); !it.Done(); it.Advance()) {
    HUseListNode<HEnvironment*>* current = it.Current();
    HEnvironment* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawEnvAt(input_index, other);
    other->AddEnvUseAt(user, input_index);
  }

  uses_.Clear();
  env_uses_.Clear();
}

void HInstruction::ReplaceInput(HInstruction* replacement, size_t index) {
  InputAt(index)->RemoveUser(this, index);
  SetRawInputAt(index, replacement);
  replacement->AddUseAt(this, index);
}

size_t HInstruction::EnvironmentSize() const {
  return HasEnvironment() ? environment_->Size() : 0;
}

void HPhi::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.Add(input);
  input->AddUseAt(this, inputs_.Size() - 1);
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (size_t i = 0 ; i < blocks.Size(); i++) {
    VisitBasicBlock(blocks.Get(i));
  }
}

void HGraphVisitor::VisitReversePostOrder() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
}

void HGraphVisitor::VisitBasicBlock(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

HConstant* HUnaryOperation::TryStaticEvaluation() const {
  if (GetInput()->IsIntConstant()) {
    int32_t value = Evaluate(GetInput()->AsIntConstant()->GetValue());
    return new(GetBlock()->GetGraph()->GetArena()) HIntConstant(value);
  } else if (GetInput()->IsLongConstant()) {
    // TODO: Implement static evaluation of long unary operations.
    //
    // Do not exit with a fatal condition here.  Instead, simply
    // return `nullptr' to notify the caller that this instruction
    // cannot (yet) be statically evaluated.
    return nullptr;
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  if (GetLeft()->IsIntConstant() && GetRight()->IsIntConstant()) {
    int32_t value = Evaluate(GetLeft()->AsIntConstant()->GetValue(),
                             GetRight()->AsIntConstant()->GetValue());
    return new(GetBlock()->GetGraph()->GetArena()) HIntConstant(value);
  } else if (GetLeft()->IsLongConstant() && GetRight()->IsLongConstant()) {
    int64_t value = Evaluate(GetLeft()->AsLongConstant()->GetValue(),
                             GetRight()->AsLongConstant()->GetValue());
    if (GetResultType() == Primitive::kPrimLong) {
      return new(GetBlock()->GetGraph()->GetArena()) HLongConstant(value);
    } else {
      DCHECK_EQ(GetResultType(), Primitive::kPrimInt);
      return new(GetBlock()->GetGraph()->GetArena()) HIntConstant(value);
    }
  }
  return nullptr;
}

bool HCondition::IsBeforeWhenDisregardMoves(HIf* if_) const {
  return this == if_->GetPreviousDisregardingMoves();
}

bool HInstruction::Equals(HInstruction* other) const {
  if (!InstructionTypeEquals(other)) return false;
  DCHECK_EQ(GetKind(), other->GetKind());
  if (!InstructionDataEquals(other)) return false;
  if (GetType() != other->GetType()) return false;
  if (InputCount() != other->InputCount()) return false;

  for (size_t i = 0, e = InputCount(); i < e; ++i) {
    if (InputAt(i) != other->InputAt(i)) return false;
  }
  DCHECK_EQ(ComputeHashCode(), other->ComputeHashCode());
  return true;
}

std::ostream& operator<<(std::ostream& os, const HInstruction::InstructionKind& rhs) {
#define DECLARE_CASE(type, super) case HInstruction::k##type: os << #type; break;
  switch (rhs) {
    FOR_EACH_INSTRUCTION(DECLARE_CASE)
    default:
      os << "Unknown instruction kind " << static_cast<int>(rhs);
      break;
  }
#undef DECLARE_CASE
  return os;
}

void HInstruction::MoveBefore(HInstruction* cursor) {
  next_->previous_ = previous_;
  if (previous_ != nullptr) {
    previous_->next_ = next_;
  }
  if (block_->instructions_.first_instruction_ == this) {
    block_->instructions_.first_instruction_ = next_;
  }
  DCHECK_NE(block_->instructions_.last_instruction_, this);

  previous_ = cursor->previous_;
  if (previous_ != nullptr) {
    previous_->next_ = this;
  }
  next_ = cursor;
  cursor->previous_ = this;
  block_ = cursor->block_;

  if (block_->instructions_.first_instruction_ == cursor) {
    block_->instructions_.first_instruction_ = this;
  }
}

HBasicBlock* HBasicBlock::SplitAfter(HInstruction* cursor) {
  DCHECK(!cursor->IsControlFlow());
  DCHECK_NE(instructions_.last_instruction_, cursor);
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(), GetDexPc());
  new_block->instructions_.first_instruction_ = cursor->GetNext();
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  cursor->next_->previous_ = nullptr;
  cursor->next_ = nullptr;
  instructions_.last_instruction_ = cursor;

  new_block->instructions_.SetBlockOfInstructions(new_block);
  for (size_t i = 0, e = GetSuccessors().Size(); i < e; ++i) {
    HBasicBlock* successor = GetSuccessors().Get(i);
    new_block->successors_.Add(successor);
    successor->predecessors_.Put(successor->GetPredecessorIndexOf(this), new_block);
  }
  successors_.Reset();

  for (size_t i = 0, e = GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = GetDominatedBlocks().Get(i);
    dominated->dominator_ = new_block;
    new_block->dominated_blocks_.Add(dominated);
  }
  dominated_blocks_.Reset();
  return new_block;
}

void HInstructionList::SetBlockOfInstructions(HBasicBlock* block) const {
  for (HInstruction* current = first_instruction_;
       current != nullptr;
       current = current->GetNext()) {
    current->SetBlock(block);
  }
}

void HInstructionList::AddAfter(HInstruction* cursor, const HInstructionList& instruction_list) {
  DCHECK(Contains(cursor));
  if (!instruction_list.IsEmpty()) {
    if (cursor == last_instruction_) {
      last_instruction_ = instruction_list.last_instruction_;
    } else {
      cursor->next_->previous_ = instruction_list.last_instruction_;
    }
    instruction_list.last_instruction_->next_ = cursor->next_;
    cursor->next_ = instruction_list.first_instruction_;
    instruction_list.first_instruction_->previous_ = cursor;
  }
}

void HInstructionList::Add(const HInstructionList& instruction_list) {
  DCHECK(!IsEmpty());
  AddAfter(last_instruction_, instruction_list);
}

void HBasicBlock::MergeWith(HBasicBlock* other) {
  DCHECK(successors_.IsEmpty()) << "Unimplemented block merge scenario";
  DCHECK(dominated_blocks_.IsEmpty()) << "Unimplemented block merge scenario";
  DCHECK(other->GetDominator()->IsEntryBlock() && other->GetGraph() != graph_)
      << "Unimplemented block merge scenario";
  DCHECK(other->GetPhis().IsEmpty());

  successors_.Reset();
  dominated_blocks_.Reset();
  instructions_.Add(other->GetInstructions());
  other->GetInstructions().SetBlockOfInstructions(this);

  while (!other->GetSuccessors().IsEmpty()) {
    HBasicBlock* successor = other->GetSuccessors().Get(0);
    successor->ReplacePredecessor(other, this);
  }

  for (size_t i = 0, e = other->GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = other->GetDominatedBlocks().Get(i);
    dominated_blocks_.Add(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.Reset();
  other->dominator_ = nullptr;
  other->graph_ = nullptr;
}

void HBasicBlock::ReplaceWith(HBasicBlock* other) {
  while (!GetPredecessors().IsEmpty()) {
    HBasicBlock* predecessor = GetPredecessors().Get(0);
    predecessor->ReplaceSuccessor(this, other);
  }
  while (!GetSuccessors().IsEmpty()) {
    HBasicBlock* successor = GetSuccessors().Get(0);
    successor->ReplacePredecessor(this, other);
  }
  for (size_t i = 0; i < dominated_blocks_.Size(); ++i) {
    other->AddDominatedBlock(dominated_blocks_.Get(i));
  }
  GetDominator()->ReplaceDominatedBlock(this, other);
  other->SetDominator(GetDominator());
  dominator_ = nullptr;
  graph_ = nullptr;
}

// Create space in `blocks` for adding `number_of_new_blocks` entries
// starting at location `at`. Blocks after `at` are moved accordingly.
static void MakeRoomFor(GrowableArray<HBasicBlock*>* blocks,
                        size_t number_of_new_blocks,
                        size_t at) {
  size_t old_size = blocks->Size();
  size_t new_size = old_size + number_of_new_blocks;
  blocks->SetSize(new_size);
  for (size_t i = old_size - 1, j = new_size - 1; i > at; --i, --j) {
    blocks->Put(j, blocks->Get(i));
  }
}

void HGraph::InlineInto(HGraph* outer_graph, HInvoke* invoke) {
  // Walk over the entry block and:
  // - Move constants from the entry block to the outer_graph's entry block,
  // - Replace HParameterValue instructions with their real value.
  // - Remove suspend checks, that hold an environment.
  int parameter_index = 0;
  for (HInstructionIterator it(entry_block_->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsConstant()) {
      current->MoveBefore(outer_graph->GetEntryBlock()->GetLastInstruction());
    } else if (current->IsParameterValue()) {
      current->ReplaceWith(invoke->InputAt(parameter_index++));
    } else {
      DCHECK(current->IsGoto() || current->IsSuspendCheck());
      entry_block_->RemoveInstruction(current);
    }
  }

  if (GetBlocks().Size() == 3) {
    // Simple case of an entry block, a body block, and an exit block.
    // Put the body block's instruction into `invoke`'s block.
    HBasicBlock* body = GetBlocks().Get(1);
    DCHECK(GetBlocks().Get(0)->IsEntryBlock());
    DCHECK(GetBlocks().Get(2)->IsExitBlock());
    DCHECK(!body->IsExitBlock());
    HInstruction* last = body->GetLastInstruction();

    invoke->GetBlock()->instructions_.AddAfter(invoke, body->GetInstructions());
    body->GetInstructions().SetBlockOfInstructions(invoke->GetBlock());

    // Replace the invoke with the return value of the inlined graph.
    if (last->IsReturn()) {
      invoke->ReplaceWith(last->InputAt(0));
    } else {
      DCHECK(last->IsReturnVoid());
    }

    invoke->GetBlock()->RemoveInstruction(last);
  } else {
    // Need to inline multiple blocks. We split `invoke`'s block
    // into two blocks, merge the first block of the inlined graph into
    // the first half, and replace the exit block of the inlined graph
    // with the second half.
    ArenaAllocator* allocator = outer_graph->GetArena();
    HBasicBlock* at = invoke->GetBlock();
    HBasicBlock* to = at->SplitAfter(invoke);

    HBasicBlock* first = entry_block_->GetSuccessors().Get(0);
    DCHECK(!first->IsInLoop());
    at->MergeWith(first);
    exit_block_->ReplaceWith(to);

    // Update all predecessors of the exit block (now the `to` block)
    // to not `HReturn` but `HGoto` instead. Also collect the return
    // values if any, and potentially make it a phi if there are multiple
    // predecessors.
    HInstruction* return_value = nullptr;
    for (size_t i = 0, e = to->GetPredecessors().Size(); i < e; ++i) {
      HBasicBlock* predecessor = to->GetPredecessors().Get(i);
      HInstruction* last = predecessor->GetLastInstruction();
      if (!last->IsReturnVoid()) {
        if (return_value != nullptr) {
          if (!return_value->IsPhi()) {
            HPhi* phi = new (allocator) HPhi(allocator, kNoRegNumber, 0, invoke->GetType());
            to->AddPhi(phi);
            phi->AddInput(return_value);
            return_value = phi;
          }
          return_value->AsPhi()->AddInput(last->InputAt(0));
        } else {
          return_value = last->InputAt(0);
        }
      }
      predecessor->AddInstruction(new (allocator) HGoto());
      predecessor->RemoveInstruction(last);
    }

    if (return_value != nullptr) {
      invoke->ReplaceWith(return_value);
    }

    // Update the meta information surrounding blocks:
    // (1) the graph they are now in,
    // (2) the reverse post order of that graph,
    // (3) the potential loop information they are now in.

    // We don't add the entry block, the exit block, and the first block, which
    // has been merged with `at`.
    static constexpr int kNumberOfSkippedBlocksInCallee = 3;

    // We add the `to` block.
    static constexpr int kNumberOfNewBlocksInCaller = 1;
    size_t blocks_added = (reverse_post_order_.Size() - kNumberOfSkippedBlocksInCallee)
        + kNumberOfNewBlocksInCaller;

    // Find the location of `at` in the outer graph's reverse post order. The new
    // blocks will be added after it.
    size_t index_of_at = 0;
    while (outer_graph->reverse_post_order_.Get(index_of_at) != at) {
      index_of_at++;
    }
    MakeRoomFor(&outer_graph->reverse_post_order_, blocks_added, index_of_at);

    // Do a reverse post order of the blocks in the callee and do (1), (2),
    // and (3) to the blocks that apply.
    HLoopInformation* info = at->GetLoopInformation();
    for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
      HBasicBlock* current = it.Current();
      if (current != exit_block_ && current != entry_block_ && current != first) {
        DCHECK(!current->IsInLoop());
        DCHECK(current->GetGraph() == this);
        current->SetGraph(outer_graph);
        outer_graph->AddBlock(current);
        outer_graph->reverse_post_order_.Put(++index_of_at, current);
        if (info != nullptr) {
          info->Add(current);
          current->SetLoopInformation(info);
        }
      }
    }

    // Do (1), (2), and (3) to `to`.
    to->SetGraph(outer_graph);
    outer_graph->AddBlock(to);
    outer_graph->reverse_post_order_.Put(++index_of_at, to);
    if (info != nullptr) {
      info->Add(to);
      to->SetLoopInformation(info);
      if (info->IsBackEdge(at)) {
        // Only `at` can become a back edge, as the inlined blocks
        // are predecessors of `at`.
        DCHECK_EQ(1u, info->NumberOfBackEdges());
        info->ClearBackEdges();
        info->AddBackEdge(to);
      }
    }
  }

  // Finally remove the invoke from the caller.
  invoke->GetBlock()->RemoveInstruction(invoke);
}

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs) {
  ScopedObjectAccess soa(Thread::Current());
  os << "["
     << " is_top=" << rhs.IsTop()
     << " type=" << (rhs.IsTop() ? "?" : PrettyClass(rhs.GetTypeHandle().Get()))
     << " is_exact=" << rhs.IsExact()
     << " ]";
  return os;
}

}  // namespace art
