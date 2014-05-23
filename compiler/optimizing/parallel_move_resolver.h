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

#ifndef ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_
#define ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_

#include "utils/allocation.h"
#include "utils/growable_array.h"

namespace art {

class HParallelMove;
class MoveOperands;

/**
 * Helper class to resolve a set of parallel moves. Architecture dependent code
 * generator must have their own subclass that implements the `EmitMove` and `EmitSwap`
 * operations.
 */
class ParallelMoveResolver : public ValueObject {
 public:
  explicit ParallelMoveResolver(ArenaAllocator* allocator) : moves_(allocator, 32) {}

  // Resolve a set of parallel moves, emitting assembler instructions.
  void EmitNativeCode(HParallelMove* parallel_move);

 protected:
  // Emit a move.
  virtual void EmitMove(size_t index) = 0;

  // Execute a move by emitting a swap of two operands.
  virtual void EmitSwap(size_t index) = 0;

  // List of moves not yet resolved.
  GrowableArray<MoveOperands*> moves_;

 private:
  // Build the initial list of moves.
  void BuildInitialMoveList(HParallelMove* parallel_move);

  // Perform the move at the moves_ index in question (possibly requiring
  // other moves to satisfy dependencies).
  void PerformMove(size_t index);

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolver);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_
