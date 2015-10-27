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

#include "code_generator.h"
#include "ssa_builder.h"
#include "base/bit_vector-inl.h"
#include "base/bit_utils.h"
#include "base/stl_util.h"
#include "intrinsics.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.size());
  blocks_.push_back(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  // "visited" must be empty on entry, it's an output argument for all visited (i.e. live) blocks.
  DCHECK_EQ(visited->GetHighestBitSet(), -1);

  // Nodes that we're currently visiting, indexed by block id.
  ArenaBitVector visiting(arena_, blocks_.size(), false);
  // Number of successors visited from a given node, indexed by block id.
  ArenaVector<size_t> successors_visited(blocks_.size(), 0u, arena_->Adapter());
  // Stack of nodes that we're currently visiting (same as marked in "visiting" above).
  ArenaVector<HBasicBlock*> worklist(arena_->Adapter());
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  visited->SetBit(entry_block_->GetBlockId());
  visiting.SetBit(entry_block_->GetBlockId());
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      visiting.ClearBit(current_id);
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];
      uint32_t successor_id = successor->GetBlockId();
      if (visiting.IsBitSet(successor_id)) {
        DCHECK(ContainsElement(worklist, successor));
        successor->AddBackEdge(current);
      } else if (!visited->IsBitSet(successor_id)) {
        visited->SetBit(successor_id);
        visiting.SetBit(successor_id);
        worklist.push_back(successor);
      }
    }
  }
}

static void RemoveAsUser(HInstruction* instruction) {
  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->RemoveAsUserOfInput(i);
  }

  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      if (environment->GetInstructionAt(i) != nullptr) {
        environment->RemoveAsUserOfInput(i);
      }
    }
  }
}

void HGraph::RemoveInstructionsAsUsersFromDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      DCHECK(block->GetPhis().IsEmpty()) << "Phis are not inserted at this stage";
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
    }
  }
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      // We only need to update the successor, which might be live.
      for (HBasicBlock* successor : block->GetSuccessors()) {
        successor->RemovePredecessor(block);
      }
      // Remove the block from the list of blocks, so that further analyses
      // never see it.
      blocks_[i] = nullptr;
    }
  }
}

void HGraph::BuildDominatorTree() {
  // (1) Simplify the CFG so that catch blocks have only exceptional incoming
  //     edges. This invariant simplifies building SSA form because Phis cannot
  //     collect both normal- and exceptional-flow values at the same time.
  SimplifyCatchBlocks();

  ArenaBitVector visited(arena_, blocks_.size(), false);

  // (2) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(&visited);

  // (3) Remove instructions and phis from blocks not visited during
  //     the initial DFS as users from other instructions, so that
  //     users can be safely removed before uses later.
  RemoveInstructionsAsUsersFromDeadBlocks(visited);

  // (4) Remove blocks not visited during the initial DFS.
  //     Step (4) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (5) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (6) Compute the dominance information and the reverse post order.
  ComputeDominanceInformation();
}

void HGraph::ClearDominanceInformation() {
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    it.Current()->ClearDominanceInformation();
  }
  reverse_post_order_.clear();
}

void HBasicBlock::ClearDominanceInformation() {
  dominated_blocks_.clear();
  dominator_ = nullptr;
}

void HGraph::ComputeDominanceInformation() {
  DCHECK(reverse_post_order_.empty());
  reverse_post_order_.reserve(blocks_.size());
  reverse_post_order_.push_back(entry_block_);

  // Number of visits of a given node, indexed by block id.
  ArenaVector<size_t> visits(blocks_.size(), 0u, arena_->Adapter());
  // Number of successors visited from a given node, indexed by block id.
  ArenaVector<size_t> successors_visited(blocks_.size(), 0u, arena_->Adapter());
  // Nodes for which we need to visit successors.
  ArenaVector<HBasicBlock*> worklist(arena_->Adapter());
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];

      if (successor->GetDominator() == nullptr) {
        successor->SetDominator(current);
      } else {
        successor->SetDominator(FindCommonDominator(successor->GetDominator(), current));
      }

      // Once all the forward edges have been visited, we know the immediate
      // dominator of the block. We can then start visiting its successors.
      if (++visits[successor->GetBlockId()] ==
          successor->GetPredecessors().size() - successor->NumberOfBackEdges()) {
        successor->GetDominator()->AddDominatedBlock(successor);
        reverse_post_order_.push_back(successor);
        worklist.push_back(successor);
      }
    }
  }
}

HBasicBlock* HGraph::FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const {
  ArenaBitVector visited(arena_, blocks_.size(), false);
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

void HGraph::TransformToSsa() {
  DCHECK(!reverse_post_order_.empty());
  SsaBuilder ssa_builder(this);
  ssa_builder.BuildSsa();
}

HBasicBlock* HGraph::SplitEdge(HBasicBlock* block, HBasicBlock* successor) {
  HBasicBlock* new_block = new (arena_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  // Use `InsertBetween` to ensure the predecessor index and successor index of
  // `block` and `successor` are preserved.
  new_block->InsertBetween(block, successor);
  return new_block;
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = SplitEdge(block, successor);
  new_block->AddInstruction(new (arena_) HGoto(successor->GetDexPc()));
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(*block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop.
  size_t number_of_incomings = header->GetPredecessors().size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto(header->GetDexPc()));

    for (size_t pred = 0; pred < header->GetPredecessors().size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors()[pred];
      if (!info->IsBackEdge(*predecessor)) {
        predecessor->ReplaceSuccessor(header, pre_header);
        pred--;
      }
    }
    pre_header->AddSuccessor(header);
  }

  // Make sure the first predecessor of a loop header is the incoming block.
  if (info->IsBackEdge(*header->GetPredecessors()[0])) {
    HBasicBlock* to_swap = header->GetPredecessors()[0];
    for (size_t pred = 1, e = header->GetPredecessors().size(); pred < e; ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors()[pred];
      if (!info->IsBackEdge(*predecessor)) {
        header->predecessors_[pred] = to_swap;
        header->predecessors_[0] = predecessor;
        break;
      }
    }
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

static bool CheckIfPredecessorAtIsExceptional(const HBasicBlock& block, size_t pred_idx) {
  HBasicBlock* predecessor = block.GetPredecessors()[pred_idx];
  if (!predecessor->EndsWithTryBoundary()) {
    // Only edges from HTryBoundary can be exceptional.
    return false;
  }
  HTryBoundary* try_boundary = predecessor->GetLastInstruction()->AsTryBoundary();
  if (try_boundary->GetNormalFlowSuccessor() == &block) {
    // This block is the normal-flow successor of `try_boundary`, but it could
    // also be one of its exception handlers if catch blocks have not been
    // simplified yet. Predecessors are unordered, so we will consider the first
    // occurrence to be the normal edge and a possible second occurrence to be
    // the exceptional edge.
    return !block.IsFirstIndexOfPredecessor(predecessor, pred_idx);
  } else {
    // This is not the normal-flow successor of `try_boundary`, hence it must be
    // one of its exception handlers.
    DCHECK(try_boundary->HasExceptionHandler(block));
    return true;
  }
}

void HGraph::SimplifyCatchBlocks() {
  // NOTE: We're appending new blocks inside the loop, so we need to use index because iterators
  // can be invalidated. We remember the initial size to avoid iterating over the new blocks.
  for (size_t block_id = 0u, end = blocks_.size(); block_id != end; ++block_id) {
    HBasicBlock* catch_block = blocks_[block_id];
    if (!catch_block->IsCatchBlock()) {
      continue;
    }

    bool exceptional_predecessors_only = true;
    for (size_t j = 0; j < catch_block->GetPredecessors().size(); ++j) {
      if (!CheckIfPredecessorAtIsExceptional(*catch_block, j)) {
        exceptional_predecessors_only = false;
        break;
      }
    }

    if (!exceptional_predecessors_only) {
      // Catch block has normal-flow predecessors and needs to be simplified.
      // Splitting the block before its first instruction moves all its
      // instructions into `normal_block` and links the two blocks with a Goto.
      // Afterwards, incoming normal-flow edges are re-linked to `normal_block`,
      // leaving `catch_block` with the exceptional edges only.
      // Note that catch blocks with normal-flow predecessors cannot begin with
      // a MOVE_EXCEPTION instruction, as guaranteed by the verifier.
      DCHECK(!catch_block->GetFirstInstruction()->IsLoadException());
      HBasicBlock* normal_block = catch_block->SplitBefore(catch_block->GetFirstInstruction());
      for (size_t j = 0; j < catch_block->GetPredecessors().size(); ++j) {
        if (!CheckIfPredecessorAtIsExceptional(*catch_block, j)) {
          catch_block->GetPredecessors()[j]->ReplaceSuccessor(catch_block, normal_block);
          --j;
        }
      }
    }
  }
}

void HGraph::ComputeTryBlockInformation() {
  // Iterate in reverse post order to propagate try membership information from
  // predecessors to their successors.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsEntryBlock() || block->IsCatchBlock()) {
      // Catch blocks after simplification have only exceptional predecessors
      // and hence are never in tries.
      continue;
    }

    // Infer try membership from the first predecessor. Having simplified loops,
    // the first predecessor can never be a back edge and therefore it must have
    // been visited already and had its try membership set.
    HBasicBlock* first_predecessor = block->GetPredecessors()[0];
    DCHECK(!block->IsLoopHeader() || !block->GetLoopInformation()->IsBackEdge(*first_predecessor));
    const HTryBoundary* try_entry = first_predecessor->ComputeTryEntryOfSuccessors();
    if (try_entry != nullptr) {
      block->SetTryCatchInformation(new (arena_) TryCatchInformation(*try_entry));
    }
  }
}

void HGraph::SimplifyCFG() {
  // Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one back edge, and one preheader.
  // NOTE: We're appending new blocks inside the loop, so we need to use index because iterators
  // can be invalidated. We remember the initial size to avoid iterating over the new blocks.
  for (size_t block_id = 0u, end = blocks_.size(); block_id != end; ++block_id) {
    HBasicBlock* block = blocks_[block_id];
    if (block == nullptr) continue;
    if (block->NumberOfNormalSuccessors() > 1) {
      for (size_t j = 0; j < block->GetSuccessors().size(); ++j) {
        HBasicBlock* successor = block->GetSuccessors()[j];
        DCHECK(!successor->IsCatchBlock());
        if (successor->GetPredecessors().size() > 1) {
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
  // Order does not matter.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsLoopHeader()) {
      if (block->IsCatchBlock()) {
        // TODO: Dealing with exceptional back edges could be tricky because
        //       they only approximate the real control flow. Bail out for now.
        return false;
      }
      HLoopInformation* info = block->GetLoopInformation();
      if (!info->Populate()) {
        // Abort if the loop is non natural. We currently bailout in such cases.
        return false;
      }
    }
  }
  return true;
}

void HGraph::InsertConstant(HConstant* constant) {
  // New constants are inserted before the final control-flow instruction
  // of the graph, or at its end if called from the graph builder.
  if (entry_block_->EndsWithControlFlowInstruction()) {
    entry_block_->InsertInstructionBefore(constant, entry_block_->GetLastInstruction());
  } else {
    entry_block_->AddInstruction(constant);
  }
}

HNullConstant* HGraph::GetNullConstant(uint32_t dex_pc) {
  // For simplicity, don't bother reviving the cached null constant if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_null_constant_ == nullptr) || (cached_null_constant_->GetBlock() == nullptr)) {
    cached_null_constant_ = new (arena_) HNullConstant(dex_pc);
    InsertConstant(cached_null_constant_);
  }
  return cached_null_constant_;
}

HCurrentMethod* HGraph::GetCurrentMethod() {
  // For simplicity, don't bother reviving the cached current method if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_current_method_ == nullptr) || (cached_current_method_->GetBlock() == nullptr)) {
    cached_current_method_ = new (arena_) HCurrentMethod(
        Is64BitInstructionSet(instruction_set_) ? Primitive::kPrimLong : Primitive::kPrimInt,
        entry_block_->GetDexPc());
    if (entry_block_->GetFirstInstruction() == nullptr) {
      entry_block_->AddInstruction(cached_current_method_);
    } else {
      entry_block_->InsertInstructionBefore(
          cached_current_method_, entry_block_->GetFirstInstruction());
    }
  }
  return cached_current_method_;
}

HConstant* HGraph::GetConstant(Primitive::Type type, int64_t value, uint32_t dex_pc) {
  switch (type) {
    case Primitive::Type::kPrimBoolean:
      DCHECK(IsUint<1>(value));
      FALLTHROUGH_INTENDED;
    case Primitive::Type::kPrimByte:
    case Primitive::Type::kPrimChar:
    case Primitive::Type::kPrimShort:
    case Primitive::Type::kPrimInt:
      DCHECK(IsInt(Primitive::ComponentSize(type) * kBitsPerByte, value));
      return GetIntConstant(static_cast<int32_t>(value), dex_pc);

    case Primitive::Type::kPrimLong:
      return GetLongConstant(value, dex_pc);

    default:
      LOG(FATAL) << "Unsupported constant type";
      UNREACHABLE();
  }
}

void HGraph::CacheFloatConstant(HFloatConstant* constant) {
  int32_t value = bit_cast<int32_t, float>(constant->GetValue());
  DCHECK(cached_float_constants_.find(value) == cached_float_constants_.end());
  cached_float_constants_.Overwrite(value, constant);
}

void HGraph::CacheDoubleConstant(HDoubleConstant* constant) {
  int64_t value = bit_cast<int64_t, double>(constant->GetValue());
  DCHECK(cached_double_constants_.find(value) == cached_double_constants_.end());
  cached_double_constants_.Overwrite(value, constant);
}

void HLoopInformation::Add(HBasicBlock* block) {
  blocks_.SetBit(block->GetBlockId());
}

void HLoopInformation::Remove(HBasicBlock* block) {
  blocks_.ClearBit(block->GetBlockId());
}

void HLoopInformation::PopulateRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  blocks_.SetBit(block->GetBlockId());
  block->SetInLoop(this);
  for (HBasicBlock* predecessor : block->GetPredecessors()) {
    PopulateRecursive(predecessor);
  }
}

bool HLoopInformation::Populate() {
  DCHECK_EQ(blocks_.NumSetBits(), 0u) << "Loop information has already been populated";
  for (HBasicBlock* back_edge : GetBackEdges()) {
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
  }
  return true;
}

void HLoopInformation::Update() {
  HGraph* graph = header_->GetGraph();
  for (uint32_t id : blocks_.Indexes()) {
    HBasicBlock* block = graph->GetBlocks()[id];
    // Reset loop information of non-header blocks inside the loop, except
    // members of inner nested loops because those should already have been
    // updated by their own LoopInformation.
    if (block->GetLoopInformation() == this && block != header_) {
      block->SetLoopInformation(nullptr);
    }
  }
  blocks_.ClearAllBits();

  if (back_edges_.empty()) {
    // The loop has been dismantled, delete its suspend check and remove info
    // from the header.
    DCHECK(HasSuspendCheck());
    header_->RemoveInstruction(suspend_check_);
    header_->SetLoopInformation(nullptr);
    header_ = nullptr;
    suspend_check_ = nullptr;
  } else {
    if (kIsDebugBuild) {
      for (HBasicBlock* back_edge : back_edges_) {
        DCHECK(header_->Dominates(back_edge));
      }
    }
    // This loop still has reachable back edges. Repopulate the list of blocks.
    bool populate_successful = Populate();
    DCHECK(populate_successful);
  }
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  return header_->GetDominator();
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

size_t HLoopInformation::GetLifetimeEnd() const {
  size_t last_position = 0;
  for (HBasicBlock* back_edge : GetBackEdges()) {
    last_position = std::max(back_edge->GetLifetimeEnd(), last_position);
  }
  return last_position;
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

void HBasicBlock::ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                                  HInstruction* replacement) {
  DCHECK(initial->GetBlock() == this);
  if (initial->IsControlFlow()) {
    // We can only replace a control flow instruction with another control flow instruction.
    DCHECK(replacement->IsControlFlow());
    DCHECK_EQ(replacement->GetId(), -1);
    DCHECK_EQ(replacement->GetType(), Primitive::kPrimVoid);
    DCHECK_EQ(initial->GetBlock(), this);
    DCHECK_EQ(initial->GetType(), Primitive::kPrimVoid);
    DCHECK(initial->GetUses().IsEmpty());
    DCHECK(initial->GetEnvUses().IsEmpty());
    replacement->SetBlock(this);
    replacement->SetId(GetGraph()->GetNextInstructionId());
    instructions_.InsertInstructionBefore(replacement, initial);
    UpdateInputsUsers(replacement);
  } else {
    InsertInstructionBefore(replacement, initial);
    initial->ReplaceWith(replacement);
  }
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

void HBasicBlock::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instructions_.InsertInstructionBefore(instruction, cursor);
}

void HBasicBlock::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  DCHECK(!cursor->IsControlFlow());
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instructions_.InsertInstructionAfter(instruction, cursor);
}

void HBasicBlock::InsertPhiAfter(HPhi* phi, HPhi* cursor) {
  DCHECK_EQ(phi->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  phi->SetBlock(this);
  phi->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(phi);
  phis_.InsertInstructionAfter(phi, cursor);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction,
                   bool ensure_safety) {
  DCHECK_EQ(block, instruction->GetBlock());
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);
  if (ensure_safety) {
    DCHECK(instruction->GetUses().IsEmpty());
    DCHECK(instruction->GetEnvUses().IsEmpty());
    RemoveAsUser(instruction);
  }
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction, bool ensure_safety) {
  DCHECK(!instruction->IsPhi());
  Remove(&instructions_, this, instruction, ensure_safety);
}

void HBasicBlock::RemovePhi(HPhi* phi, bool ensure_safety) {
  Remove(&phis_, this, phi, ensure_safety);
}

void HBasicBlock::RemoveInstructionOrPhi(HInstruction* instruction, bool ensure_safety) {
  if (instruction->IsPhi()) {
    RemovePhi(instruction->AsPhi(), ensure_safety);
  } else {
    RemoveInstruction(instruction, ensure_safety);
  }
}

void HEnvironment::CopyFrom(const ArenaVector<HInstruction*>& locals) {
  for (size_t i = 0; i < locals.size(); i++) {
    HInstruction* instruction = locals[i];
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(this, i);
    }
  }
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

void HEnvironment::CopyFromWithLoopPhiAdjustment(HEnvironment* env,
                                                 HBasicBlock* loop_header) {
  DCHECK(loop_header->IsLoopHeader());
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction == nullptr) {
      continue;
    }
    if (instruction->IsLoopHeaderPhi() && (instruction->GetBlock() == loop_header)) {
      // At the end of the loop pre-header, the corresponding value for instruction
      // is the first input of the phi.
      HInstruction* initial = instruction->AsPhi()->InputAt(0);
      DCHECK(initial->GetBlock()->Dominates(loop_header));
      SetRawEnvAt(i, initial);
      initial->AddEnvUseAt(this, i);
    } else {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

void HEnvironment::RemoveAsUserOfInput(size_t index) const {
  const HUserRecord<HEnvironment*>& user_record = vregs_[index];
  user_record.GetInstruction()->RemoveEnvironmentUser(user_record.GetUseNode());
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

void HInstructionList::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == first_instruction_) {
    cursor->previous_ = instruction;
    instruction->next_ = cursor;
    first_instruction_ = instruction;
  } else {
    instruction->previous_ = cursor->previous_;
    instruction->next_ = cursor;
    cursor->previous_ = instruction;
    instruction->previous_->next_ = instruction;
  }
}

void HInstructionList::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == last_instruction_) {
    cursor->next_ = instruction;
    instruction->previous_ = cursor;
    last_instruction_ = instruction;
  } else {
    instruction->next_ = cursor->next_;
    instruction->previous_ = cursor;
    cursor->next_ = instruction;
    instruction->next_->previous_ = instruction;
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
  RemoveAsUserOfInput(index);
  SetRawInputAt(index, replacement);
  replacement->AddUseAt(this, index);
}

size_t HInstruction::EnvironmentSize() const {
  return HasEnvironment() ? environment_->Size() : 0;
}

void HPhi::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.push_back(HUserRecord<HInstruction*>(input));
  input->AddUseAt(this, inputs_.size() - 1);
}

void HPhi::RemoveInputAt(size_t index) {
  RemoveAsUserOfInput(index);
  inputs_.erase(inputs_.begin() + index);
  for (size_t i = index, e = InputCount(); i < e; ++i) {
    DCHECK_EQ(InputRecordAt(i).GetUseNode()->GetIndex(), i + 1u);
    InputRecordAt(i).GetUseNode()->SetIndex(i);
  }
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const ArenaVector<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (HBasicBlock* block : blocks) {
    if (block != nullptr) {
      VisitBasicBlock(block);
    }
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

HConstant* HTypeConversion::TryStaticEvaluation() const {
  HGraph* graph = GetBlock()->GetGraph();
  if (GetInput()->IsIntConstant()) {
    int32_t value = GetInput()->AsIntConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimLong:
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsLongConstant()) {
    int64_t value = GetInput()->AsLongConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsFloatConstant()) {
    float value = GetInput()->AsFloatConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        if (std::isnan(value))
          return graph->GetIntConstant(0, GetDexPc());
        if (value >= kPrimIntMax)
          return graph->GetIntConstant(kPrimIntMax, GetDexPc());
        if (value <= kPrimIntMin)
          return graph->GetIntConstant(kPrimIntMin, GetDexPc());
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimLong:
        if (std::isnan(value))
          return graph->GetLongConstant(0, GetDexPc());
        if (value >= kPrimLongMax)
          return graph->GetLongConstant(kPrimLongMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin, GetDexPc());
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsDoubleConstant()) {
    double value = GetInput()->AsDoubleConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        if (std::isnan(value))
          return graph->GetIntConstant(0, GetDexPc());
        if (value >= kPrimIntMax)
          return graph->GetIntConstant(kPrimIntMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetIntConstant(kPrimIntMin, GetDexPc());
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimLong:
        if (std::isnan(value))
          return graph->GetLongConstant(0, GetDexPc());
        if (value >= kPrimLongMax)
          return graph->GetLongConstant(kPrimLongMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin, GetDexPc());
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      default:
        return nullptr;
    }
  }
  return nullptr;
}

HConstant* HUnaryOperation::TryStaticEvaluation() const {
  if (GetInput()->IsIntConstant()) {
    return Evaluate(GetInput()->AsIntConstant());
  } else if (GetInput()->IsLongConstant()) {
    return Evaluate(GetInput()->AsLongConstant());
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  if (GetLeft()->IsIntConstant()) {
    if (GetRight()->IsIntConstant()) {
      return Evaluate(GetLeft()->AsIntConstant(), GetRight()->AsIntConstant());
    } else if (GetRight()->IsLongConstant()) {
      return Evaluate(GetLeft()->AsIntConstant(), GetRight()->AsLongConstant());
    }
  } else if (GetLeft()->IsLongConstant()) {
    if (GetRight()->IsIntConstant()) {
      return Evaluate(GetLeft()->AsLongConstant(), GetRight()->AsIntConstant());
    } else if (GetRight()->IsLongConstant()) {
      return Evaluate(GetLeft()->AsLongConstant(), GetRight()->AsLongConstant());
    }
  }
  return nullptr;
}

HConstant* HBinaryOperation::GetConstantRight() const {
  if (GetRight()->IsConstant()) {
    return GetRight()->AsConstant();
  } else if (IsCommutative() && GetLeft()->IsConstant()) {
    return GetLeft()->AsConstant();
  } else {
    return nullptr;
  }
}

// If `GetConstantRight()` returns one of the input, this returns the other
// one. Otherwise it returns null.
HInstruction* HBinaryOperation::GetLeastConstantLeft() const {
  HInstruction* most_constant_right = GetConstantRight();
  if (most_constant_right == nullptr) {
    return nullptr;
  } else if (most_constant_right == GetLeft()) {
    return GetRight();
  } else {
    return GetLeft();
  }
}

bool HCondition::IsBeforeWhenDisregardMoves(HInstruction* instruction) const {
  return this == instruction->GetPreviousDisregardingMoves();
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

HBasicBlock* HBasicBlock::SplitBefore(HInstruction* cursor) {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented";
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(),
                                                                    cursor->GetDexPc());
  new_block->instructions_.first_instruction_ = cursor;
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  instructions_.last_instruction_ = cursor->previous_;
  if (cursor->previous_ == nullptr) {
    instructions_.first_instruction_ = nullptr;
  } else {
    cursor->previous_->next_ = nullptr;
    cursor->previous_ = nullptr;
  }

  new_block->instructions_.SetBlockOfInstructions(new_block);
  AddInstruction(new (GetGraph()->GetArena()) HGoto(new_block->GetDexPc()));

  for (HBasicBlock* successor : GetSuccessors()) {
    new_block->successors_.push_back(successor);
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  successors_.clear();
  AddSuccessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
}

HBasicBlock* HBasicBlock::CreateImmediateDominator() {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented";
  DCHECK(!IsCatchBlock()) << "Support for updating try/catch information not implemented.";

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(), GetDexPc());

  for (HBasicBlock* predecessor : GetPredecessors()) {
    new_block->predecessors_.push_back(predecessor);
    predecessor->successors_[predecessor->GetSuccessorIndexOf(this)] = new_block;
  }
  predecessors_.clear();
  AddPredecessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
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
  for (HBasicBlock* successor : GetSuccessors()) {
    new_block->successors_.push_back(successor);
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  successors_.clear();

  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    dominated->dominator_ = new_block;
    new_block->dominated_blocks_.push_back(dominated);
  }
  dominated_blocks_.clear();
  return new_block;
}

const HTryBoundary* HBasicBlock::ComputeTryEntryOfSuccessors() const {
  if (EndsWithTryBoundary()) {
    HTryBoundary* try_boundary = GetLastInstruction()->AsTryBoundary();
    if (try_boundary->IsEntry()) {
      DCHECK(!IsTryBlock());
      return try_boundary;
    } else {
      DCHECK(IsTryBlock());
      DCHECK(try_catch_information_->GetTryEntry().HasSameExceptionHandlersAs(*try_boundary));
      return nullptr;
    }
  } else if (IsTryBlock()) {
    return &try_catch_information_->GetTryEntry();
  } else {
    return nullptr;
  }
}

bool HBasicBlock::HasThrowingInstructions() const {
  for (HInstructionIterator it(GetInstructions()); !it.Done(); it.Advance()) {
    if (it.Current()->CanThrow()) {
      return true;
    }
  }
  return false;
}

static bool HasOnlyOneInstruction(const HBasicBlock& block) {
  return block.GetPhis().IsEmpty()
      && !block.GetInstructions().IsEmpty()
      && block.GetFirstInstruction() == block.GetLastInstruction();
}

bool HBasicBlock::IsSingleGoto() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsGoto();
}

bool HBasicBlock::IsSingleTryBoundary() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::EndsWithControlFlowInstruction() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsControlFlow();
}

bool HBasicBlock::EndsWithIf() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsIf();
}

bool HBasicBlock::EndsWithTryBoundary() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::HasSinglePhi() const {
  return !GetPhis().IsEmpty() && GetFirstPhi()->GetNext() == nullptr;
}

bool HTryBoundary::HasSameExceptionHandlersAs(const HTryBoundary& other) const {
  if (GetBlock()->GetSuccessors().size() != other.GetBlock()->GetSuccessors().size()) {
    return false;
  }

  // Exception handlers need to be stored in the same order.
  for (HExceptionHandlerIterator it1(*this), it2(other);
       !it1.Done();
       it1.Advance(), it2.Advance()) {
    DCHECK(!it2.Done());
    if (it1.Current() != it2.Current()) {
      return false;
    }
  }
  return true;
}

size_t HInstructionList::CountSize() const {
  size_t size = 0;
  HInstruction* current = first_instruction_;
  for (; current != nullptr; current = current->GetNext()) {
    size++;
  }
  return size;
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
  if (IsEmpty()) {
    first_instruction_ = instruction_list.first_instruction_;
    last_instruction_ = instruction_list.last_instruction_;
  } else {
    AddAfter(last_instruction_, instruction_list);
  }
}

void HBasicBlock::DisconnectAndDelete() {
  // Dominators must be removed after all the blocks they dominate. This way
  // a loop header is removed last, a requirement for correct loop information
  // iteration.
  DCHECK(dominated_blocks_.empty());

  // Remove the block from all loops it is included in.
  for (HLoopInformationOutwardIterator it(*this); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(this);
    if (loop_info->IsBackEdge(*this)) {
      // If this was the last back edge of the loop, we deliberately leave the
      // loop in an inconsistent state and will fail SSAChecker unless the
      // entire loop is removed during the pass.
      loop_info->RemoveBackEdge(this);
    }
  }

  // Disconnect the block from its predecessors and update their control-flow
  // instructions.
  for (HBasicBlock* predecessor : predecessors_) {
    HInstruction* last_instruction = predecessor->GetLastInstruction();
    predecessor->RemoveSuccessor(this);
    uint32_t num_pred_successors = predecessor->GetSuccessors().size();
    if (num_pred_successors == 1u) {
      // If we have one successor after removing one, then we must have
      // had an HIf or HPackedSwitch, as they have more than one successor.
      // Replace those with a HGoto.
      DCHECK(last_instruction->IsIf() || last_instruction->IsPackedSwitch());
      predecessor->RemoveInstruction(last_instruction);
      predecessor->AddInstruction(new (graph_->GetArena()) HGoto(last_instruction->GetDexPc()));
    } else if (num_pred_successors == 0u) {
      // The predecessor has no remaining successors and therefore must be dead.
      // We deliberately leave it without a control-flow instruction so that the
      // SSAChecker fails unless it is not removed during the pass too.
      predecessor->RemoveInstruction(last_instruction);
    } else {
      // There are multiple successors left.  This must come from a HPackedSwitch
      // and we are in the middle of removing the HPackedSwitch. Like above, leave
      // this alone, and the SSAChecker will fail if it is not removed as well.
      DCHECK(last_instruction->IsPackedSwitch());
    }
  }
  predecessors_.clear();

  // Disconnect the block from its successors and update their phis.
  for (HBasicBlock* successor : successors_) {
    // Delete this block from the list of predecessors.
    size_t this_index = successor->GetPredecessorIndexOf(this);
    successor->predecessors_.erase(successor->predecessors_.begin() + this_index);

    // Check that `successor` has other predecessors, otherwise `this` is the
    // dominator of `successor` which violates the order DCHECKed at the top.
    DCHECK(!successor->predecessors_.empty());

    // Remove this block's entries in the successor's phis.
    if (successor->predecessors_.size() == 1u) {
      // The successor has just one predecessor left. Replace phis with the only
      // remaining input.
      for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        HPhi* phi = phi_it.Current()->AsPhi();
        phi->ReplaceWith(phi->InputAt(1 - this_index));
        successor->RemovePhi(phi);
      }
    } else {
      for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        phi_it.Current()->AsPhi()->RemoveInputAt(this_index);
      }
    }
  }
  successors_.clear();

  // Disconnect from the dominator.
  dominator_->RemoveDominatedBlock(this);
  SetDominator(nullptr);

  // Delete from the graph. The function safely deletes remaining instructions
  // and updates the reverse post order.
  graph_->DeleteDeadBlock(this);
  SetGraph(nullptr);
}

void HBasicBlock::MergeWith(HBasicBlock* other) {
  DCHECK_EQ(GetGraph(), other->GetGraph());
  DCHECK(ContainsElement(dominated_blocks_, other));
  DCHECK_EQ(GetSingleSuccessor(), other);
  DCHECK_EQ(other->GetSinglePredecessor(), this);
  DCHECK(other->GetPhis().IsEmpty());

  // Move instructions from `other` to `this`.
  DCHECK(EndsWithControlFlowInstruction());
  RemoveInstruction(GetLastInstruction());
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);
  other->instructions_.Clear();

  // Remove `other` from the loops it is included in.
  for (HLoopInformationOutwardIterator it(*other); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(other);
    if (loop_info->IsBackEdge(*other)) {
      loop_info->ReplaceBackEdge(other, this);
    }
  }

  // Update links to the successors of `other`.
  successors_.clear();
  while (!other->successors_.empty()) {
    HBasicBlock* successor = other->GetSuccessors()[0];
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  RemoveDominatedBlock(other);
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated_blocks_.push_back(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;

  // Clear the list of predecessors of `other` in preparation of deleting it.
  other->predecessors_.clear();

  // Delete `other` from the graph. The function updates reverse post order.
  graph_->DeleteDeadBlock(other);
  other->SetGraph(nullptr);
}

void HBasicBlock::MergeWithInlined(HBasicBlock* other) {
  DCHECK_NE(GetGraph(), other->GetGraph());
  DCHECK(GetDominatedBlocks().empty());
  DCHECK(GetSuccessors().empty());
  DCHECK(!EndsWithControlFlowInstruction());
  DCHECK(other->GetSinglePredecessor()->IsEntryBlock());
  DCHECK(other->GetPhis().IsEmpty());
  DCHECK(!other->IsInLoop());

  // Move instructions from `other` to `this`.
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);

  // Update links to the successors of `other`.
  successors_.clear();
  while (!other->successors_.empty()) {
    HBasicBlock* successor = other->GetSuccessors()[0];
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated_blocks_.push_back(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;
  other->graph_ = nullptr;
}

void HBasicBlock::ReplaceWith(HBasicBlock* other) {
  while (!GetPredecessors().empty()) {
    HBasicBlock* predecessor = GetPredecessors()[0];
    predecessor->ReplaceSuccessor(this, other);
  }
  while (!GetSuccessors().empty()) {
    HBasicBlock* successor = GetSuccessors()[0];
    successor->ReplacePredecessor(this, other);
  }
  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    other->AddDominatedBlock(dominated);
  }
  GetDominator()->ReplaceDominatedBlock(this, other);
  other->SetDominator(GetDominator());
  dominator_ = nullptr;
  graph_ = nullptr;
}

// Create space in `blocks` for adding `number_of_new_blocks` entries
// starting at location `at`. Blocks after `at` are moved accordingly.
static void MakeRoomFor(ArenaVector<HBasicBlock*>* blocks,
                        size_t number_of_new_blocks,
                        size_t after) {
  DCHECK_LT(after, blocks->size());
  size_t old_size = blocks->size();
  size_t new_size = old_size + number_of_new_blocks;
  blocks->resize(new_size);
  std::copy_backward(blocks->begin() + after + 1u, blocks->begin() + old_size, blocks->end());
}

void HGraph::DeleteDeadBlock(HBasicBlock* block) {
  DCHECK_EQ(block->GetGraph(), this);
  DCHECK(block->GetSuccessors().empty());
  DCHECK(block->GetPredecessors().empty());
  DCHECK(block->GetDominatedBlocks().empty());
  DCHECK(block->GetDominator() == nullptr);

  for (HBackwardInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    block->RemoveInstruction(it.Current());
  }
  for (HBackwardInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    block->RemovePhi(it.Current()->AsPhi());
  }

  if (block->IsExitBlock()) {
    exit_block_ = nullptr;
  }

  RemoveElement(reverse_post_order_, block);
  blocks_[block->GetBlockId()] = nullptr;
}

HInstruction* HGraph::InlineInto(HGraph* outer_graph, HInvoke* invoke) {
  DCHECK(HasExitBlock()) << "Unimplemented scenario";
  // Update the environments in this graph to have the invoke's environment
  // as parent.
  {
    HReversePostOrderIterator it(*this);
    it.Advance();  // Skip the entry block, we do not need to update the entry's suspend check.
    for (; !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      for (HInstructionIterator instr_it(block->GetInstructions());
           !instr_it.Done();
           instr_it.Advance()) {
        HInstruction* current = instr_it.Current();
        if (current->NeedsEnvironment()) {
          current->GetEnvironment()->SetAndCopyParentChain(
              outer_graph->GetArena(), invoke->GetEnvironment());
        }
      }
    }
  }
  outer_graph->UpdateMaximumNumberOfOutVRegs(GetMaximumNumberOfOutVRegs());
  if (HasBoundsChecks()) {
    outer_graph->SetHasBoundsChecks(true);
  }

  HInstruction* return_value = nullptr;
  if (GetBlocks().size() == 3) {
    // Simple case of an entry block, a body block, and an exit block.
    // Put the body block's instruction into `invoke`'s block.
    HBasicBlock* body = GetBlocks()[1];
    DCHECK(GetBlocks()[0]->IsEntryBlock());
    DCHECK(GetBlocks()[2]->IsExitBlock());
    DCHECK(!body->IsExitBlock());
    HInstruction* last = body->GetLastInstruction();

    invoke->GetBlock()->instructions_.AddAfter(invoke, body->GetInstructions());
    body->GetInstructions().SetBlockOfInstructions(invoke->GetBlock());

    // Replace the invoke with the return value of the inlined graph.
    if (last->IsReturn()) {
      return_value = last->InputAt(0);
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

    HBasicBlock* first = entry_block_->GetSuccessors()[0];
    DCHECK(!first->IsInLoop());
    at->MergeWithInlined(first);
    exit_block_->ReplaceWith(to);

    // Update all predecessors of the exit block (now the `to` block)
    // to not `HReturn` but `HGoto` instead.
    bool returns_void = to->GetPredecessors()[0]->GetLastInstruction()->IsReturnVoid();
    if (to->GetPredecessors().size() == 1) {
      HBasicBlock* predecessor = to->GetPredecessors()[0];
      HInstruction* last = predecessor->GetLastInstruction();
      if (!returns_void) {
        return_value = last->InputAt(0);
      }
      predecessor->AddInstruction(new (allocator) HGoto(last->GetDexPc()));
      predecessor->RemoveInstruction(last);
    } else {
      if (!returns_void) {
        // There will be multiple returns.
        return_value = new (allocator) HPhi(
            allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke->GetType()), to->GetDexPc());
        to->AddPhi(return_value->AsPhi());
      }
      for (HBasicBlock* predecessor : to->GetPredecessors()) {
        HInstruction* last = predecessor->GetLastInstruction();
        if (!returns_void) {
          return_value->AsPhi()->AddInput(last->InputAt(0));
        }
        predecessor->AddInstruction(new (allocator) HGoto(last->GetDexPc()));
        predecessor->RemoveInstruction(last);
      }
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
    size_t blocks_added = (reverse_post_order_.size() - kNumberOfSkippedBlocksInCallee)
        + kNumberOfNewBlocksInCaller;

    // Find the location of `at` in the outer graph's reverse post order. The new
    // blocks will be added after it.
    size_t index_of_at = IndexOfElement(outer_graph->reverse_post_order_, at);
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
        outer_graph->reverse_post_order_[++index_of_at] = current;
        if (info != nullptr) {
          current->SetLoopInformation(info);
          for (HLoopInformationOutwardIterator loop_it(*at); !loop_it.Done(); loop_it.Advance()) {
            loop_it.Current()->Add(current);
          }
        }
      }
    }

    // Do (1), (2), and (3) to `to`.
    to->SetGraph(outer_graph);
    outer_graph->AddBlock(to);
    outer_graph->reverse_post_order_[++index_of_at] = to;
    if (info != nullptr) {
      to->SetLoopInformation(info);
      for (HLoopInformationOutwardIterator loop_it(*at); !loop_it.Done(); loop_it.Advance()) {
        loop_it.Current()->Add(to);
      }
      if (info->IsBackEdge(*at)) {
        // Only `to` can become a back edge, as the inlined blocks
        // are predecessors of `to`.
        info->ReplaceBackEdge(at, to);
      }
    }
  }

  // Update the next instruction id of the outer graph, so that instructions
  // added later get bigger ids than those in the inner graph.
  outer_graph->SetCurrentInstructionId(GetNextInstructionId());

  // Walk over the entry block and:
  // - Move constants from the entry block to the outer_graph's entry block,
  // - Replace HParameterValue instructions with their real value.
  // - Remove suspend checks, that hold an environment.
  // We must do this after the other blocks have been inlined, otherwise ids of
  // constants could overlap with the inner graph.
  size_t parameter_index = 0;
  for (HInstructionIterator it(entry_block_->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    HInstruction* replacement = nullptr;
    if (current->IsNullConstant()) {
      replacement = outer_graph->GetNullConstant(current->GetDexPc());
    } else if (current->IsIntConstant()) {
      replacement = outer_graph->GetIntConstant(
          current->AsIntConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsLongConstant()) {
      replacement = outer_graph->GetLongConstant(
          current->AsLongConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsFloatConstant()) {
      replacement = outer_graph->GetFloatConstant(
          current->AsFloatConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsDoubleConstant()) {
      replacement = outer_graph->GetDoubleConstant(
          current->AsDoubleConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsParameterValue()) {
      if (kIsDebugBuild
          && invoke->IsInvokeStaticOrDirect()
          && invoke->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck()) {
        // Ensure we do not use the last input of `invoke`, as it
        // contains a clinit check which is not an actual argument.
        size_t last_input_index = invoke->InputCount() - 1;
        DCHECK(parameter_index != last_input_index);
      }
      replacement = invoke->InputAt(parameter_index++);
    } else if (current->IsCurrentMethod()) {
      replacement = outer_graph->GetCurrentMethod();
    } else {
      DCHECK(current->IsGoto() || current->IsSuspendCheck());
      entry_block_->RemoveInstruction(current);
    }
    if (replacement != nullptr) {
      current->ReplaceWith(replacement);
      // If the current is the return value then we need to update the latter.
      if (current == return_value) {
        DCHECK_EQ(entry_block_, return_value->GetBlock());
        return_value = replacement;
      }
    }
  }

  if (return_value != nullptr) {
    invoke->ReplaceWith(return_value);
  }

  // Finally remove the invoke from the caller.
  invoke->GetBlock()->RemoveInstruction(invoke);

  return return_value;
}

/*
 * Loop will be transformed to:
 *       old_pre_header
 *             |
 *          if_block
 *           /    \
 *  dummy_block   deopt_block
 *           \    /
 *       new_pre_header
 *             |
 *           header
 */
void HGraph::TransformLoopHeaderForBCE(HBasicBlock* header) {
  DCHECK(header->IsLoopHeader());
  HBasicBlock* pre_header = header->GetDominator();

  // Need this to avoid critical edge.
  HBasicBlock* if_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  // Need this to avoid critical edge.
  HBasicBlock* dummy_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* deopt_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
  AddBlock(if_block);
  AddBlock(dummy_block);
  AddBlock(deopt_block);
  AddBlock(new_pre_header);

  header->ReplacePredecessor(pre_header, new_pre_header);
  pre_header->successors_.clear();
  pre_header->dominated_blocks_.clear();

  pre_header->AddSuccessor(if_block);
  if_block->AddSuccessor(dummy_block);  // True successor
  if_block->AddSuccessor(deopt_block);  // False successor
  dummy_block->AddSuccessor(new_pre_header);
  deopt_block->AddSuccessor(new_pre_header);

  pre_header->dominated_blocks_.push_back(if_block);
  if_block->SetDominator(pre_header);
  if_block->dominated_blocks_.push_back(dummy_block);
  dummy_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(deopt_block);
  deopt_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(new_pre_header);
  new_pre_header->SetDominator(if_block);
  new_pre_header->dominated_blocks_.push_back(header);
  header->SetDominator(new_pre_header);

  size_t index_of_header = IndexOfElement(reverse_post_order_, header);
  MakeRoomFor(&reverse_post_order_, 4, index_of_header - 1);
  reverse_post_order_[index_of_header++] = if_block;
  reverse_post_order_[index_of_header++] = dummy_block;
  reverse_post_order_[index_of_header++] = deopt_block;
  reverse_post_order_[index_of_header++] = new_pre_header;

  HLoopInformation* info = pre_header->GetLoopInformation();
  if (info != nullptr) {
    if_block->SetLoopInformation(info);
    dummy_block->SetLoopInformation(info);
    deopt_block->SetLoopInformation(info);
    new_pre_header->SetLoopInformation(info);
    for (HLoopInformationOutwardIterator loop_it(*pre_header);
         !loop_it.Done();
         loop_it.Advance()) {
      loop_it.Current()->Add(if_block);
      loop_it.Current()->Add(dummy_block);
      loop_it.Current()->Add(deopt_block);
      loop_it.Current()->Add(new_pre_header);
    }
  }
}

void HInstruction::SetReferenceTypeInfo(ReferenceTypeInfo rti) {
  if (kIsDebugBuild) {
    DCHECK_EQ(GetType(), Primitive::kPrimNot);
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(rti.IsValid()) << "Invalid RTI for " << DebugName();
    if (IsBoundType()) {
      // Having the test here spares us from making the method virtual just for
      // the sake of a DCHECK.
      ReferenceTypeInfo upper_bound_rti = AsBoundType()->GetUpperBound();
      DCHECK(upper_bound_rti.IsSupertypeOf(rti))
          << " upper_bound_rti: " << upper_bound_rti
          << " rti: " << rti;
      DCHECK(!upper_bound_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes() || rti.IsExact());
    }
  }
  reference_type_info_ = rti;
}

ReferenceTypeInfo::ReferenceTypeInfo() : type_handle_(TypeHandle()), is_exact_(false) {}

ReferenceTypeInfo::ReferenceTypeInfo(TypeHandle type_handle, bool is_exact)
    : type_handle_(type_handle), is_exact_(is_exact) {
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(IsValidHandle(type_handle));
  }
}

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs) {
  ScopedObjectAccess soa(Thread::Current());
  os << "["
     << " is_valid=" << rhs.IsValid()
     << " type=" << (!rhs.IsValid() ? "?" : PrettyClass(rhs.GetTypeHandle().Get()))
     << " is_exact=" << rhs.IsExact()
     << " ]";
  return os;
}

bool HInstruction::HasAnyEnvironmentUseBefore(HInstruction* other) {
  // For now, assume that instructions in different blocks may use the
  // environment.
  // TODO: Use the control flow to decide if this is true.
  if (GetBlock() != other->GetBlock()) {
    return true;
  }

  // We know that we are in the same block. Walk from 'this' to 'other',
  // checking to see if there is any instruction with an environment.
  HInstruction* current = this;
  for (; current != other && current != nullptr; current = current->GetNext()) {
    // This is a conservative check, as the instruction result may not be in
    // the referenced environment.
    if (current->HasEnvironment()) {
      return true;
    }
  }

  // We should have been called with 'this' before 'other' in the block.
  // Just confirm this.
  DCHECK(current != nullptr);
  return false;
}

void HInvoke::SetIntrinsic(Intrinsics intrinsic,
                           IntrinsicNeedsEnvironmentOrCache needs_env_or_cache) {
  intrinsic_ = intrinsic;
  IntrinsicOptimizations opt(this);
  if (needs_env_or_cache == kNoEnvironmentOrCache) {
    opt.SetDoesNotNeedDexCache();
    opt.SetDoesNotNeedEnvironment();
  }
}

bool HInvoke::NeedsEnvironment() const {
  if (!IsIntrinsic()) {
    return true;
  }
  IntrinsicOptimizations opt(*this);
  return !opt.GetDoesNotNeedEnvironment();
}

bool HInvokeStaticOrDirect::NeedsDexCacheOfDeclaringClass() const {
  if (GetMethodLoadKind() != MethodLoadKind::kDexCacheViaMethod) {
    return false;
  }
  if (!IsIntrinsic()) {
    return true;
  }
  IntrinsicOptimizations opt(*this);
  return !opt.GetDoesNotNeedDexCache();
}

void HInstruction::RemoveEnvironmentUsers() {
  for (HUseIterator<HEnvironment*> use_it(GetEnvUses()); !use_it.Done(); use_it.Advance()) {
    HUseListNode<HEnvironment*>* user_node = use_it.Current();
    HEnvironment* user = user_node->GetUser();
    user->SetRawEnvAt(user_node->GetIndex(), nullptr);
  }
  env_uses_.Clear();
}

}  // namespace art
