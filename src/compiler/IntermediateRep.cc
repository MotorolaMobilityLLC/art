/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "Dalvik.h"
#include "CompilerInternals.h"

namespace art {

/* Allocate a new basic block */
BasicBlock* oatNewBB(CompilationUnit* cUnit, BBType blockType, int blockId)
{
    BasicBlock* bb = (BasicBlock* )oatNew(sizeof(BasicBlock), true, kAllocBB);
    bb->blockType = blockType;
    bb->id = blockId;
    bb->predecessors = (GrowableList*) oatNew(sizeof(GrowableList), false,
                                              kAllocPredecessors);
    oatInitGrowableList(bb->predecessors, (blockType == kExitBlock) ? 2048 : 2,
                        kListPredecessors);
    return bb;
}

/* Insert an MIR instruction to the end of a basic block */
void oatAppendMIR(BasicBlock* bb, MIR* mir)
{
    if (bb->firstMIRInsn == NULL) {
        DCHECK(bb->lastMIRInsn == NULL);
        bb->lastMIRInsn = bb->firstMIRInsn = mir;
        mir->prev = mir->next = NULL;
    } else {
        bb->lastMIRInsn->next = mir;
        mir->prev = bb->lastMIRInsn;
        mir->next = NULL;
        bb->lastMIRInsn = mir;
    }
}

/* Insert an MIR instruction to the head of a basic block */
void oatPrependMIR(BasicBlock* bb, MIR* mir)
{
    if (bb->firstMIRInsn == NULL) {
        DCHECK(bb->lastMIRInsn == NULL);
        bb->lastMIRInsn = bb->firstMIRInsn = mir;
        mir->prev = mir->next = NULL;
    } else {
        bb->firstMIRInsn->prev = mir;
        mir->next = bb->firstMIRInsn;
        mir->prev = NULL;
        bb->firstMIRInsn = mir;
    }
}

/* Insert an MIR instruction after the specified MIR */
void oatInsertMIRAfter(BasicBlock* bb, MIR* currentMIR, MIR* newMIR)
{
    newMIR->prev = currentMIR;
    newMIR->next = currentMIR->next;
    currentMIR->next = newMIR;

    if (newMIR->next) {
        /* Is not the last MIR in the block */
        newMIR->next->prev = newMIR;
    } else {
        /* Is the last MIR in the block */
        bb->lastMIRInsn = newMIR;
    }
}

/*
 * Append an LIR instruction to the LIR list maintained by a compilation
 * unit
 */
void oatAppendLIR(CompilationUnit *cUnit, LIR* lir)
{
    if (cUnit->firstLIRInsn == NULL) {
        DCHECK(cUnit->lastLIRInsn == NULL);
        cUnit->lastLIRInsn = cUnit->firstLIRInsn = lir;
        lir->prev = lir->next = NULL;
    } else {
        cUnit->lastLIRInsn->next = lir;
        lir->prev = cUnit->lastLIRInsn;
        lir->next = NULL;
        cUnit->lastLIRInsn = lir;
    }
}

/*
 * Insert an LIR instruction before the current instruction, which cannot be the
 * first instruction.
 *
 * prevLIR <-> newLIR <-> currentLIR
 */
void oatInsertLIRBefore(LIR* currentLIR, LIR* newLIR)
{
    DCHECK(currentLIR->prev != NULL);
    LIR *prevLIR = currentLIR->prev;

    prevLIR->next = newLIR;
    newLIR->prev = prevLIR;
    newLIR->next = currentLIR;
    currentLIR->prev = newLIR;
}

/*
 * Insert an LIR instruction after the current instruction, which cannot be the
 * first instruction.
 *
 * currentLIR -> newLIR -> oldNext
 */
void oatInsertLIRAfter(LIR* currentLIR, LIR* newLIR)
{
    newLIR->prev = currentLIR;
    newLIR->next = currentLIR->next;
    currentLIR->next = newLIR;
    newLIR->next->prev = newLIR;
}

}  // namespace art
