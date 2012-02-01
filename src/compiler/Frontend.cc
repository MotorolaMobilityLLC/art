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
#include "Dataflow.h"
#include "constants.h"
#include "leb128.h"
#include "object.h"
#include "runtime.h"

namespace art {

/* Default optimizer/debug setting for the compiler. */
uint32_t compilerOptimizerDisableFlags = 0 | // Disable specific optimizations
     //(1 << kLoadStoreElimination) |
     //(1 << kLoadHoisting) |
     //(1 << kSuppressLoads) |
     //(1 << kNullCheckElimination) |
     //(1 << kPromoteRegs) |
     //(1 << kTrackLiveTemps) |
     //(1 << kSkipLargeMethodOptimization) |
     0;

uint32_t compilerDebugFlags = 0 |     // Enable debug/testing modes
     //(1 << kDebugDisplayMissingTargets) |
     //(1 << kDebugVerbose) |
     //(1 << kDebugDumpCFG) |
     //(1 << kDebugSlowFieldPath) |
     //(1 << kDebugSlowInvokePath) |
     //(1 << kDebugSlowStringPath) |
     //(1 << kDebugSlowestFieldPath) |
     //(1 << kDebugSlowestStringPath) |
     //(1 << kDebugExerciseResolveMethod) |
     //(1 << kDebugVerifyDataflow) |
     //(1 << kDebugShowMemoryUsage) |
     0;

std::string compilerMethodMatch;      // Method name match to apply above flags

bool compilerFlipMatch = false;       // Reverses sense of method name match

STATIC inline bool contentIsInsn(const u2* codePtr) {
    u2 instr = *codePtr;
    Opcode opcode = (Opcode)(instr & 0xff);

    /*
     * Since the low 8-bit in metadata may look like OP_NOP, we need to check
     * both the low and whole sub-word to determine whether it is code or data.
     */
    return (opcode != OP_NOP || instr == 0);
}

/*
 * Parse an instruction, return the length of the instruction
 */
STATIC inline int parseInsn(const u2* codePtr, DecodedInstruction* decInsn,
                            bool printMe)
{
    // Don't parse instruction data
    if (!contentIsInsn(codePtr)) {
        return 0;
    }

    u2 instr = *codePtr;
    Opcode opcode = dexOpcodeFromCodeUnit(instr);

    dexDecodeInstruction(codePtr, decInsn);
    if (printMe) {
        char* decodedString = oatGetDalvikDisassembly(decInsn, NULL);
        LOG(INFO) << codePtr << ": 0x" << std::hex << (int)opcode <<
        " " << decodedString;
    }
    return dexGetWidthFromOpcode(opcode);
}

#define UNKNOWN_TARGET 0xffffffff

STATIC inline bool isGoto(MIR* insn)
{
    switch (insn->dalvikInsn.opcode) {
        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            return true;
        default:
            return false;
    }
}

/*
 * Identify unconditional branch instructions
 */
STATIC inline bool isUnconditionalBranch(MIR* insn)
{
    switch (insn->dalvikInsn.opcode) {
        case OP_RETURN_VOID:
        case OP_RETURN:
        case OP_RETURN_WIDE:
        case OP_RETURN_OBJECT:
            return true;
        default:
            return isGoto(insn);
    }
}

/* Split an existing block from the specified code offset into two */
STATIC BasicBlock *splitBlock(CompilationUnit* cUnit,
                              unsigned int codeOffset,
                              BasicBlock* origBlock,
                              BasicBlock** immedPredBlockP)
{
    MIR* insn = origBlock->firstMIRInsn;
    while (insn) {
        if (insn->offset == codeOffset) break;
        insn = insn->next;
    }
    if (insn == NULL) {
        LOG(FATAL) << "Break split failed";
    }
    BasicBlock *bottomBlock = oatNewBB(cUnit, kDalvikByteCode,
                                       cUnit->numBlocks++);
    oatInsertGrowableList(&cUnit->blockList, (intptr_t) bottomBlock);

    bottomBlock->startOffset = codeOffset;
    bottomBlock->firstMIRInsn = insn;
    bottomBlock->lastMIRInsn = origBlock->lastMIRInsn;

    /* Add it to the quick lookup cache */
    cUnit->blockMap.insert(std::make_pair(bottomBlock->startOffset,
                                          bottomBlock));

    /* Handle the taken path */
    bottomBlock->taken = origBlock->taken;
    if (bottomBlock->taken) {
        origBlock->taken = NULL;
        oatDeleteGrowableList(bottomBlock->taken->predecessors,
                              (intptr_t)origBlock);
        oatInsertGrowableList(bottomBlock->taken->predecessors,
                              (intptr_t)bottomBlock);
    }

    /* Handle the fallthrough path */
    bottomBlock->needFallThroughBranch = origBlock->needFallThroughBranch;
    bottomBlock->fallThrough = origBlock->fallThrough;
    origBlock->fallThrough = bottomBlock;
    origBlock->needFallThroughBranch = true;
    oatInsertGrowableList(bottomBlock->predecessors, (intptr_t)origBlock);
    if (bottomBlock->fallThrough) {
        oatDeleteGrowableList(bottomBlock->fallThrough->predecessors,
                              (intptr_t)origBlock);
        oatInsertGrowableList(bottomBlock->fallThrough->predecessors,
                              (intptr_t)bottomBlock);
    }

    /* Handle the successor list */
    if (origBlock->successorBlockList.blockListType != kNotUsed) {
        bottomBlock->successorBlockList = origBlock->successorBlockList;
        origBlock->successorBlockList.blockListType = kNotUsed;
        GrowableListIterator iterator;

        oatGrowableListIteratorInit(&bottomBlock->successorBlockList.blocks,
                                    &iterator);
        while (true) {
            SuccessorBlockInfo *successorBlockInfo =
                (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
            if (successorBlockInfo == NULL) break;
            BasicBlock *bb = successorBlockInfo->block;
            oatDeleteGrowableList(bb->predecessors, (intptr_t)origBlock);
            oatInsertGrowableList(bb->predecessors, (intptr_t)bottomBlock);
        }
    }

    origBlock->lastMIRInsn = insn->prev;

    insn->prev->next = NULL;
    insn->prev = NULL;
    /*
     * Update the immediate predecessor block pointer so that outgoing edges
     * can be applied to the proper block.
     */
    if (immedPredBlockP) {
        DCHECK_EQ(*immedPredBlockP, origBlock);
        *immedPredBlockP = bottomBlock;
    }
    return bottomBlock;
}

/*
 * Given a code offset, find out the block that starts with it. If the offset
 * is in the middle of an existing block, split it into two.  If immedPredBlockP
 * is not non-null and is the block being split, update *immedPredBlockP to
 * point to the bottom block so that outgoing edges can be set up properly
 * (by the caller)
 * Utilizes a map for fast lookup of the typical cases.
 */
STATIC BasicBlock *findBlock(CompilationUnit* cUnit,
                             unsigned int codeOffset,
                             bool split, bool create,
                             BasicBlock** immedPredBlockP)
{
    GrowableList* blockList = &cUnit->blockList;
    BasicBlock* bb;
    unsigned int i;
    std::map<unsigned int, BasicBlock*>::iterator it;

    it = cUnit->blockMap.find(codeOffset);
    if (it != cUnit->blockMap.end()) {
        return it->second;
    } else if (!create) {
        return NULL;
    }

    if (split) {
        for (i = 0; i < blockList->numUsed; i++) {
            bb = (BasicBlock *) blockList->elemList[i];
            if (bb->blockType != kDalvikByteCode) continue;
            /* Check if a branch jumps into the middle of an existing block */
            if ((codeOffset > bb->startOffset) && (bb->lastMIRInsn != NULL) &&
                (codeOffset <= bb->lastMIRInsn->offset)) {
                BasicBlock *newBB = splitBlock(cUnit, codeOffset, bb,
                                               bb == *immedPredBlockP ?
                                               immedPredBlockP : NULL);
                return newBB;
            }
        }
    }

    /* Create a new one */
    bb = oatNewBB(cUnit, kDalvikByteCode, cUnit->numBlocks++);
    oatInsertGrowableList(&cUnit->blockList, (intptr_t) bb);
    bb->startOffset = codeOffset;
    cUnit->blockMap.insert(std::make_pair(bb->startOffset, bb));
    return bb;
}

/* Dump the CFG into a DOT graph */
void oatDumpCFG(CompilationUnit* cUnit, const char* dirPrefix)
{
    FILE* file;
    std::string name(PrettyMethod(cUnit->method_idx, *cUnit->dex_file));
    char startOffset[80];
    sprintf(startOffset, "_%x", cUnit->entryBlock->fallThrough->startOffset);
    char* fileName = (char*) oatNew(
                        strlen(dirPrefix) +
                        name.length() +
                        strlen(".dot") + 1, true, kAllocDebugInfo);
    sprintf(fileName, "%s%s%s.dot", dirPrefix, name.c_str(), startOffset);

    /*
     * Convert the special characters into a filesystem- and shell-friendly
     * format.
     */
    int i;
    for (i = strlen(dirPrefix); fileName[i]; i++) {
        if (fileName[i] == '/') {
            fileName[i] = '_';
        } else if (fileName[i] == ';') {
            fileName[i] = '#';
        } else if (fileName[i] == '$') {
            fileName[i] = '+';
        } else if (fileName[i] == '(' || fileName[i] == ')') {
            fileName[i] = '@';
        } else if (fileName[i] == '<' || fileName[i] == '>') {
            fileName[i] = '=';
        }
    }
    file = fopen(fileName, "w");
    if (file == NULL) {
        return;
    }
    fprintf(file, "digraph G {\n");

    fprintf(file, "  rankdir=TB\n");

    int numReachableBlocks = cUnit->numReachableBlocks;
    int idx;
    const GrowableList *blockList = &cUnit->blockList;

    for (idx = 0; idx < numReachableBlocks; idx++) {
        int blockIdx = cUnit->dfsOrder.elemList[idx];
        BasicBlock *bb = (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                  blockIdx);
        if (bb == NULL) break;
        if (bb->blockType == kEntryBlock) {
            fprintf(file, "  entry [shape=Mdiamond];\n");
        } else if (bb->blockType == kExitBlock) {
            fprintf(file, "  exit [shape=Mdiamond];\n");
        } else if (bb->blockType == kDalvikByteCode) {
            fprintf(file, "  block%04x [shape=record,label = \"{ \\\n",
                    bb->startOffset);
            const MIR *mir;
            fprintf(file, "    {block id %d\\l}%s\\\n", bb->id,
                    bb->firstMIRInsn ? " | " : " ");
            for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
                fprintf(file, "    {%04x %s\\l}%s\\\n", mir->offset,
                        mir->ssaRep ?
                            oatFullDisassembler(cUnit, mir) :
                            dexGetOpcodeName(mir->dalvikInsn.opcode),
                        mir->next ? " | " : " ");
            }
            fprintf(file, "  }\"];\n\n");
        } else if (bb->blockType == kExceptionHandling) {
            char blockName[BLOCK_NAME_LEN];

            oatGetBlockName(bb, blockName);
            fprintf(file, "  %s [shape=invhouse];\n", blockName);
        }

        char blockName1[BLOCK_NAME_LEN], blockName2[BLOCK_NAME_LEN];

        if (bb->taken) {
            oatGetBlockName(bb, blockName1);
            oatGetBlockName(bb->taken, blockName2);
            fprintf(file, "  %s:s -> %s:n [style=dotted]\n",
                    blockName1, blockName2);
        }
        if (bb->fallThrough) {
            oatGetBlockName(bb, blockName1);
            oatGetBlockName(bb->fallThrough, blockName2);
            fprintf(file, "  %s:s -> %s:n\n", blockName1, blockName2);
        }

        if (bb->successorBlockList.blockListType != kNotUsed) {
            fprintf(file, "  succ%04x [shape=%s,label = \"{ \\\n",
                    bb->startOffset,
                    (bb->successorBlockList.blockListType == kCatch) ?
                        "Mrecord" : "record");
            GrowableListIterator iterator;
            oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                        &iterator);
            SuccessorBlockInfo *successorBlockInfo =
                (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);

            int succId = 0;
            while (true) {
                if (successorBlockInfo == NULL) break;

                BasicBlock *destBlock = successorBlockInfo->block;
                SuccessorBlockInfo *nextSuccessorBlockInfo =
                  (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);

                fprintf(file, "    {<f%d> %04x: %04x\\l}%s\\\n",
                        succId++,
                        successorBlockInfo->key,
                        destBlock->startOffset,
                        (nextSuccessorBlockInfo != NULL) ? " | " : " ");

                successorBlockInfo = nextSuccessorBlockInfo;
            }
            fprintf(file, "  }\"];\n\n");

            oatGetBlockName(bb, blockName1);
            fprintf(file, "  %s:s -> succ%04x:n [style=dashed]\n",
                    blockName1, bb->startOffset);

            if (bb->successorBlockList.blockListType == kPackedSwitch ||
                bb->successorBlockList.blockListType == kSparseSwitch) {

                oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                            &iterator);

                succId = 0;
                while (true) {
                    SuccessorBlockInfo *successorBlockInfo =
                        (SuccessorBlockInfo *)
                            oatGrowableListIteratorNext(&iterator);
                    if (successorBlockInfo == NULL) break;

                    BasicBlock *destBlock = successorBlockInfo->block;

                    oatGetBlockName(destBlock, blockName2);
                    fprintf(file, "  succ%04x:f%d:e -> %s:n\n",
                            bb->startOffset, succId++,
                            blockName2);
                }
            }
        }
        fprintf(file, "\n");

        /* Display the dominator tree */
        oatGetBlockName(bb, blockName1);
        fprintf(file, "  cfg%s [label=\"%s\", shape=none];\n",
                blockName1, blockName1);
        if (bb->iDom) {
            oatGetBlockName(bb->iDom, blockName2);
            fprintf(file, "  cfg%s:s -> cfg%s:n\n\n",
                    blockName2, blockName1);
        }
    }
    fprintf(file, "}\n");
    fclose(file);
}

/* Verify if all the successor is connected with all the claimed predecessors */
STATIC bool verifyPredInfo(CompilationUnit* cUnit, BasicBlock* bb)
{
    GrowableListIterator iter;

    oatGrowableListIteratorInit(bb->predecessors, &iter);
    while (true) {
        BasicBlock *predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
        if (!predBB) break;
        bool found = false;
        if (predBB->taken == bb) {
            found = true;
        } else if (predBB->fallThrough == bb) {
            found = true;
        } else if (predBB->successorBlockList.blockListType != kNotUsed) {
            GrowableListIterator iterator;
            oatGrowableListIteratorInit(&predBB->successorBlockList.blocks,
                                        &iterator);
            while (true) {
                SuccessorBlockInfo *successorBlockInfo =
                    (SuccessorBlockInfo *)
                        oatGrowableListIteratorNext(&iterator);
                if (successorBlockInfo == NULL) break;
                BasicBlock *succBB = successorBlockInfo->block;
                if (succBB == bb) {
                    found = true;
                    break;
                }
            }
        }
        if (found == false) {
            char blockName1[BLOCK_NAME_LEN], blockName2[BLOCK_NAME_LEN];
            oatGetBlockName(bb, blockName1);
            oatGetBlockName(predBB, blockName2);
            oatDumpCFG(cUnit, "/sdcard/cfg/");
            LOG(FATAL) << "Successor " << blockName1 << "not found from "
                << blockName2;
        }
    }
    return true;
}

/* Identify code range in try blocks and set up the empty catch blocks */
STATIC void processTryCatchBlocks(CompilationUnit* cUnit)
{
    const DexFile::CodeItem* code_item = cUnit->code_item;
    int triesSize = code_item->tries_size_;
    int offset;

    if (triesSize == 0) {
        return;
    }

    ArenaBitVector* tryBlockAddr = cUnit->tryBlockAddr;

    for (int i = 0; i < triesSize; i++) {
        const DexFile::TryItem* pTry =
            DexFile::GetTryItems(*code_item, i);
        int startOffset = pTry->start_addr_;
        int endOffset = startOffset + pTry->insn_count_;
        for (offset = startOffset; offset < endOffset; offset++) {
            oatSetBit(tryBlockAddr, offset);
        }
    }

    // Iterate over each of the handlers to enqueue the empty Catch blocks
    const byte* handlers_ptr =
        DexFile::GetCatchHandlerData(*code_item, 0);
    uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
    for (uint32_t idx = 0; idx < handlers_size; idx++) {
        CatchHandlerIterator iterator(handlers_ptr);
        for (; iterator.HasNext(); iterator.Next()) {
            uint32_t address = iterator.GetHandlerAddress();
            findBlock(cUnit, address, false /* split */, true /*create*/,
                      /* immedPredBlockP */ NULL);
        }
        handlers_ptr = iterator.EndDataPointer();
    }
}

/* Process instructions with the kInstrCanBranch flag */
STATIC BasicBlock* processCanBranch(CompilationUnit* cUnit,
                                    BasicBlock* curBlock, MIR* insn,
                                    int curOffset, int width, int flags,
                                    const u2* codePtr, const u2* codeEnd)
{
    int target = curOffset;
    switch (insn->dalvikInsn.opcode) {
        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            target += (int) insn->dalvikInsn.vA;
            break;
        case OP_IF_EQ:
        case OP_IF_NE:
        case OP_IF_LT:
        case OP_IF_GE:
        case OP_IF_GT:
        case OP_IF_LE:
            target += (int) insn->dalvikInsn.vC;
            break;
        case OP_IF_EQZ:
        case OP_IF_NEZ:
        case OP_IF_LTZ:
        case OP_IF_GEZ:
        case OP_IF_GTZ:
        case OP_IF_LEZ:
            target += (int) insn->dalvikInsn.vB;
            break;
        default:
            LOG(FATAL) << "Unexpected opcode(" << (int)insn->dalvikInsn.opcode
                << ") with kInstrCanBranch set";
    }
    BasicBlock *takenBlock = findBlock(cUnit, target,
                                       /* split */
                                       true,
                                       /* create */
                                       true,
                                       /* immedPredBlockP */
                                       &curBlock);
    curBlock->taken = takenBlock;
    oatInsertGrowableList(takenBlock->predecessors, (intptr_t)curBlock);

    /* Always terminate the current block for conditional branches */
    if (flags & kInstrCanContinue) {
        BasicBlock *fallthroughBlock = findBlock(cUnit,
                                                 curOffset +  width,
                                                 /*
                                                  * If the method is processed
                                                  * in sequential order from the
                                                  * beginning, we don't need to
                                                  * specify split for continue
                                                  * blocks. However, this
                                                  * routine can be called by
                                                  * compileLoop, which starts
                                                  * parsing the method from an
                                                  * arbitrary address in the
                                                  * method body.
                                                  */
                                                 true,
                                                 /* create */
                                                 true,
                                                 /* immedPredBlockP */
                                                 &curBlock);
        curBlock->fallThrough = fallthroughBlock;
        oatInsertGrowableList(fallthroughBlock->predecessors,
                              (intptr_t)curBlock);
    } else if (codePtr < codeEnd) {
        /* Create a fallthrough block for real instructions (incl. OP_NOP) */
        if (contentIsInsn(codePtr)) {
            findBlock(cUnit, curOffset + width,
                      /* split */
                      false,
                      /* create */
                      true,
                      /* immedPredBlockP */
                      NULL);
        }
    }
    return curBlock;
}

/* Process instructions with the kInstrCanSwitch flag */
STATIC void processCanSwitch(CompilationUnit* cUnit, BasicBlock* curBlock,
                             MIR* insn, int curOffset, int width, int flags)
{
    u2* switchData= (u2 *) (cUnit->insns + curOffset +
                            insn->dalvikInsn.vB);
    int size;
    int* keyTable;
    int* targetTable;
    int i;
    int firstKey;

    /*
     * Packed switch data format:
     *  ushort ident = 0x0100   magic value
     *  ushort size             number of entries in the table
     *  int first_key           first (and lowest) switch case value
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (4+size*2) 16-bit code units.
     */
    if (insn->dalvikInsn.opcode == OP_PACKED_SWITCH) {
        DCHECK_EQ(switchData[0], kPackedSwitchSignature);
        size = switchData[1];
        firstKey = switchData[2] | (switchData[3] << 16);
        targetTable = (int *) &switchData[4];
        keyTable = NULL;        // Make the compiler happy
    /*
     * Sparse switch data format:
     *  ushort ident = 0x0200   magic value
     *  ushort size             number of entries in the table; > 0
     *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (2+size*4) 16-bit code units.
     */
    } else {
        DCHECK_EQ(switchData[0], kSparseSwitchSignature);
        size = switchData[1];
        keyTable = (int *) &switchData[2];
        targetTable = (int *) &switchData[2 + size*2];
        firstKey = 0;   // To make the compiler happy
    }

    if (curBlock->successorBlockList.blockListType != kNotUsed) {
        LOG(FATAL) << "Successor block list already in use: " <<
             (int)curBlock->successorBlockList.blockListType;
    }
    curBlock->successorBlockList.blockListType =
        (insn->dalvikInsn.opcode == OP_PACKED_SWITCH) ?
        kPackedSwitch : kSparseSwitch;
    oatInitGrowableList(&curBlock->successorBlockList.blocks, size,
                        kListSuccessorBlocks);

    for (i = 0; i < size; i++) {
        BasicBlock *caseBlock = findBlock(cUnit, curOffset + targetTable[i],
                                          /* split */
                                          true,
                                          /* create */
                                          true,
                                          /* immedPredBlockP */
                                          &curBlock);
        SuccessorBlockInfo *successorBlockInfo =
            (SuccessorBlockInfo *) oatNew(sizeof(SuccessorBlockInfo),
                                                  false, kAllocSuccessor);
        successorBlockInfo->block = caseBlock;
        successorBlockInfo->key = (insn->dalvikInsn.opcode == OP_PACKED_SWITCH)?
                                  firstKey + i : keyTable[i];
        oatInsertGrowableList(&curBlock->successorBlockList.blocks,
                              (intptr_t) successorBlockInfo);
        oatInsertGrowableList(caseBlock->predecessors, (intptr_t)curBlock);
    }

    /* Fall-through case */
    BasicBlock* fallthroughBlock = findBlock(cUnit,
                                             curOffset +  width,
                                             /* split */
                                             false,
                                             /* create */
                                             true,
                                             /* immedPredBlockP */
                                             NULL);
    curBlock->fallThrough = fallthroughBlock;
    oatInsertGrowableList(fallthroughBlock->predecessors, (intptr_t)curBlock);
}

/* Process instructions with the kInstrCanThrow flag */
STATIC void processCanThrow(CompilationUnit* cUnit, BasicBlock* curBlock,
                            MIR* insn, int curOffset, int width, int flags,
                            ArenaBitVector* tryBlockAddr, const u2* codePtr,
                            const u2* codeEnd)
{
    const DexFile::CodeItem* code_item = cUnit->code_item;

    /* In try block */
    if (oatIsBitSet(tryBlockAddr, curOffset)) {
        CatchHandlerIterator iterator(*code_item, curOffset);

        if (curBlock->successorBlockList.blockListType != kNotUsed) {
            LOG(FATAL) << "Successor block list already in use: " <<
                 (int)curBlock->successorBlockList.blockListType;
        }

        curBlock->successorBlockList.blockListType = kCatch;
        oatInitGrowableList(&curBlock->successorBlockList.blocks, 2,
                            kListSuccessorBlocks);

        for (;iterator.HasNext(); iterator.Next()) {
            BasicBlock *catchBlock = findBlock(cUnit, iterator.GetHandlerAddress(),
                                               false /* split*/,
                                               false /* creat */,
                                               NULL  /* immedPredBlockP */);
            catchBlock->catchEntry = true;
            SuccessorBlockInfo *successorBlockInfo =
                  (SuccessorBlockInfo *) oatNew(sizeof(SuccessorBlockInfo),
                                                false, kAllocSuccessor);
            successorBlockInfo->block = catchBlock;
            successorBlockInfo->key = iterator.GetHandlerTypeIndex();
            oatInsertGrowableList(&curBlock->successorBlockList.blocks,
                                  (intptr_t) successorBlockInfo);
            oatInsertGrowableList(catchBlock->predecessors, (intptr_t)curBlock);
        }
    } else {
        BasicBlock *ehBlock = oatNewBB(cUnit, kExceptionHandling,
                                       cUnit->numBlocks++);
        curBlock->taken = ehBlock;
        oatInsertGrowableList(&cUnit->blockList, (intptr_t) ehBlock);
        ehBlock->startOffset = curOffset;
        oatInsertGrowableList(ehBlock->predecessors, (intptr_t)curBlock);
    }

    /*
     * Force the current block to terminate.
     *
     * Data may be present before codeEnd, so we need to parse it to know
     * whether it is code or data.
     */
    if (codePtr < codeEnd) {
        /* Create a fallthrough block for real instructions (incl. OP_NOP) */
        if (contentIsInsn(codePtr)) {
            BasicBlock *fallthroughBlock = findBlock(cUnit,
                                                     curOffset + width,
                                                     /* split */
                                                     false,
                                                     /* create */
                                                     true,
                                                     /* immedPredBlockP */
                                                     NULL);
            /*
             * OP_THROW is an unconditional branch.  NOTE:
             * OP_THROW_VERIFICATION_ERROR is also an unconditional
             * branch, but we shouldn't treat it as such until we have
             * a dead code elimination pass (which won't be important
             * until inlining w/ constant propogation is implemented.
             */
            if (insn->dalvikInsn.opcode != OP_THROW) {
                curBlock->fallThrough = fallthroughBlock;
                oatInsertGrowableList(fallthroughBlock->predecessors,
                                      (intptr_t)curBlock);
            }
        }
    }
}

/*
 * Compile a method.
 */
CompiledMethod* oatCompileMethod(const Compiler& compiler, const DexFile::CodeItem* code_item,
                                 uint32_t access_flags, uint32_t method_idx,
                                 const ClassLoader* class_loader,
                                 const DexFile& dex_file, InstructionSet insnSet)
{
    VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";
    oatArenaReset();

    const u2* codePtr = code_item->insns_;
    const u2* codeEnd = code_item->insns_ + code_item->insns_size_in_code_units_;
    int numBlocks = 0;
    unsigned int curOffset = 0;

    oatInit(compiler);

    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    UniquePtr<CompilationUnit> cUnit(new CompilationUnit);
    memset(cUnit.get(), 0, sizeof(*cUnit));
    cUnit->compiler = &compiler;
    cUnit->class_linker = class_linker;
    cUnit->dex_file = &dex_file;
    cUnit->dex_cache = class_linker->FindDexCache(dex_file);
    cUnit->method_idx = method_idx;
    cUnit->code_item = code_item;
    cUnit->access_flags = access_flags;
    cUnit->shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
    cUnit->instructionSet = (OatInstructionSetType)insnSet;
    cUnit->insns = code_item->insns_;
    cUnit->insnsSize = code_item->insns_size_in_code_units_;
    cUnit->numIns = code_item->ins_size_;
    cUnit->numRegs = code_item->registers_size_ - cUnit->numIns;
    cUnit->numOuts = code_item->outs_size_;
    /* Adjust this value accordingly once inlining is performed */
    cUnit->numDalvikRegisters = code_item->registers_size_;
    cUnit->blockMap = std::map<unsigned int, BasicBlock*>();
    cUnit->blockMap.clear();
    cUnit->boundaryMap = std::map<unsigned int, LIR*>();
    cUnit->boundaryMap.clear();
    bool useMatch = compilerMethodMatch.length() != 0;
    bool match = useMatch && (compilerFlipMatch ^
        (PrettyMethod(method_idx, dex_file).find(compilerMethodMatch) != std::string::npos));
    if (!useMatch || match) {
        cUnit->disableOpt = compilerOptimizerDisableFlags;
        cUnit->enableDebug = compilerDebugFlags;
        cUnit->printMe = VLOG_IS_ON(compiler) || (cUnit->enableDebug & (1 << kDebugVerbose));
    }

    /* Assume non-throwing leaf */
    cUnit->attrs = (METHOD_IS_LEAF | METHOD_IS_THROW_FREE);

    /* Initialize the block list, estimate size based on insnsSize */
    oatInitGrowableList(&cUnit->blockList, cUnit->insnsSize, kListBlockList);

    /* Initialize the switchTables list */
    oatInitGrowableList(&cUnit->switchTables, 4, kListSwitchTables);

    /* Intialize the fillArrayData list */
    oatInitGrowableList(&cUnit->fillArrayData, 4, kListFillArrayData);

    /* Intialize the throwLaunchpads list, estimate size based on insnsSize */
    oatInitGrowableList(&cUnit->throwLaunchpads, cUnit->insnsSize,
                        kListThrowLaunchPads);

    /* Intialize the suspendLaunchpads list */
    oatInitGrowableList(&cUnit->suspendLaunchpads, 2048,
                        kListSuspendLaunchPads);

    /* Allocate the bit-vector to track the beginning of basic blocks */
    ArenaBitVector *tryBlockAddr = oatAllocBitVector(cUnit->insnsSize,
                                                     true /* expandable */);
    cUnit->tryBlockAddr = tryBlockAddr;

    /* Create the default entry and exit blocks and enter them to the list */
    BasicBlock *entryBlock = oatNewBB(cUnit.get(), kEntryBlock, numBlocks++);
    BasicBlock *exitBlock = oatNewBB(cUnit.get(), kExitBlock, numBlocks++);

    cUnit->entryBlock = entryBlock;
    cUnit->exitBlock = exitBlock;

    oatInsertGrowableList(&cUnit->blockList, (intptr_t) entryBlock);
    oatInsertGrowableList(&cUnit->blockList, (intptr_t) exitBlock);

    /* Current block to record parsed instructions */
    BasicBlock *curBlock = oatNewBB(cUnit.get(), kDalvikByteCode, numBlocks++);
    curBlock->startOffset = 0;
    oatInsertGrowableList(&cUnit->blockList, (intptr_t) curBlock);
    /* Add first block to the fast lookup cache */
    cUnit->blockMap.insert(std::make_pair(curBlock->startOffset, curBlock));
    entryBlock->fallThrough = curBlock;
    oatInsertGrowableList(curBlock->predecessors, (intptr_t)entryBlock);

    /*
     * Store back the number of blocks since new blocks may be created of
     * accessing cUnit.
     */
    cUnit->numBlocks = numBlocks;

    /* Identify code range in try blocks and set up the empty catch blocks */
    processTryCatchBlocks(cUnit.get());

    /* Parse all instructions and put them into containing basic blocks */
    while (codePtr < codeEnd) {
        MIR *insn = (MIR *) oatNew(sizeof(MIR), true, kAllocMIR);
        insn->offset = curOffset;
        int width = parseInsn(codePtr, &insn->dalvikInsn, false);
        insn->width = width;

        /* Terminate when the data section is seen */
        if (width == 0)
            break;

        oatAppendMIR(curBlock, insn);

        codePtr += width;
        int flags = dexGetFlagsFromOpcode(insn->dalvikInsn.opcode);

        int dfFlags = oatDataFlowAttributes[insn->dalvikInsn.opcode];

        if (dfFlags & DF_HAS_DEFS) {
            cUnit->defCount += (dfFlags & DF_DA_WIDE) ? 2 : 1;
        }

        if (flags & kInstrCanBranch) {
            curBlock = processCanBranch(cUnit.get(), curBlock, insn, curOffset,
                                        width, flags, codePtr, codeEnd);
        } else if (flags & kInstrCanReturn) {
            curBlock->fallThrough = exitBlock;
            oatInsertGrowableList(exitBlock->predecessors, (intptr_t)curBlock);
            /*
             * Terminate the current block if there are instructions
             * afterwards.
             */
            if (codePtr < codeEnd) {
                /*
                 * Create a fallthrough block for real instructions
                 * (incl. OP_NOP).
                 */
                if (contentIsInsn(codePtr)) {
                    findBlock(cUnit.get(), curOffset + width,
                              /* split */
                              false,
                              /* create */
                              true,
                              /* immedPredBlockP */
                              NULL);
                }
            }
        } else if (flags & kInstrCanThrow) {
            processCanThrow(cUnit.get(), curBlock, insn, curOffset, width, flags,
                            tryBlockAddr, codePtr, codeEnd);
        } else if (flags & kInstrCanSwitch) {
            processCanSwitch(cUnit.get(), curBlock, insn, curOffset, width, flags);
        }
        curOffset += width;
        BasicBlock *nextBlock = findBlock(cUnit.get(), curOffset,
                                          /* split */
                                          false,
                                          /* create */
                                          false,
                                          /* immedPredBlockP */
                                          NULL);
        if (nextBlock) {
            /*
             * The next instruction could be the target of a previously parsed
             * forward branch so a block is already created. If the current
             * instruction is not an unconditional branch, connect them through
             * the fall-through link.
             */
            DCHECK(curBlock->fallThrough == NULL ||
                   curBlock->fallThrough == nextBlock ||
                   curBlock->fallThrough == exitBlock);

            if ((curBlock->fallThrough == NULL) &&
                (flags & kInstrCanContinue)) {
                curBlock->fallThrough = nextBlock;
                oatInsertGrowableList(nextBlock->predecessors,
                                      (intptr_t)curBlock);
            }
            curBlock = nextBlock;
        }
    }

    if (!(cUnit->disableOpt & (1 << kSkipLargeMethodOptimization))) {
        if ((cUnit->numBlocks > MANY_BLOCKS) ||
              ((cUnit->numBlocks > MANY_BLOCKS_INITIALIZER) &&
               PrettyMethod(method_idx, dex_file).find("init>") !=
               std::string::npos)) {
            cUnit->disableDataflow = true;
            // Disable optimization which require dataflow/ssa
            cUnit->disableOpt |=
                (1 << kNullCheckElimination) |
                (1 << kPromoteRegs);
            if (cUnit->printMe) {
                LOG(INFO) << "Compiler: " << PrettyMethod(method_idx, dex_file)
                   << " too big: " << cUnit->numBlocks;
            }
        }
    }

    if (cUnit->printMe) {
        oatDumpCompilationUnit(cUnit.get());
    }

    if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
        /* Verify if all blocks are connected as claimed */
        oatDataFlowAnalysisDispatcher(cUnit.get(), verifyPredInfo, kAllNodes,
                                      false /* isIterative */);
    }

    /* Perform SSA transformation for the whole method */
    oatMethodSSATransformation(cUnit.get());

    /* Perform null check elimination */
    oatMethodNullCheckElimination(cUnit.get());

    oatInitializeRegAlloc(cUnit.get());  // Needs to happen after SSA naming

    /* Allocate Registers using simple local allocation scheme */
    oatSimpleRegAlloc(cUnit.get());

    /* Convert MIR to LIR, etc. */
    oatMethodMIR2LIR(cUnit.get());

    // Debugging only
    if (cUnit->enableDebug & (1 << kDebugDumpCFG)) {
        oatDumpCFG(cUnit.get(), "/sdcard/cfg/");
    }

    /* Method is not empty */
    if (cUnit->firstLIRInsn) {

        // mark the targets of switch statement case labels
        oatProcessSwitchTables(cUnit.get());

        /* Convert LIR into machine code. */
        oatAssembleLIR(cUnit.get());

        if (cUnit->printMe) {
            oatCodegenDump(cUnit.get());
        }
    }

    // Combine vmap tables - core regs, then fp regs - into vmapTable
    std::vector<uint16_t> vmapTable;
    for (size_t i = 0 ; i < cUnit->coreVmapTable.size(); i++) {
        vmapTable.push_back(cUnit->coreVmapTable[i]);
    }
    // Add a marker to take place of lr
    vmapTable.push_back(INVALID_VREG);
    // Combine vmap tables - core regs, then fp regs
    for (uint32_t i = 0; i < cUnit->fpVmapTable.size(); i++) {
        vmapTable.push_back(cUnit->fpVmapTable[i]);
    }
    DCHECK_EQ(vmapTable.size(),
              static_cast<uint32_t>(__builtin_popcount(cUnit->coreSpillMask)
                                    + __builtin_popcount(cUnit->fpSpillMask)));
    DCHECK_GE(vmapTable.size(), 1U);  // should always at least one INVALID_VREG for lr

    CompiledMethod* result = new CompiledMethod(kThumb2, cUnit->codeBuffer,
                                                cUnit->frameSize, cUnit->coreSpillMask,
                                                cUnit->fpSpillMask, cUnit->mappingTable,
                                                vmapTable);

    VLOG(compiler) << "Compiled " << PrettyMethod(method_idx, dex_file)
       << " (" << (cUnit->codeBuffer.size() * sizeof(cUnit->codeBuffer[0]))
       << " bytes)";

#ifdef WITH_MEMSTATS
    if (cUnit->enableDebug & (1 << kDebugShowMemoryUsage)) {
        oatDumpMemStats(cUnit.get());
    }
#endif

    return result;
}

void oatInit(const Compiler& compiler)
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    VLOG(compiler) << "Initializing compiler";
    if (!oatArchInit()) {
        LOG(FATAL) << "Failed to initialize oat";
    }
    if (!oatHeapInit()) {
        LOG(FATAL) << "Failed to initialize oat heap";
    }
}

}  // namespace art
