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

namespace art {

#define DEBUG_OPT(X)

/* Check RAW, WAR, and WAR dependency on the register operands */
#define CHECK_REG_DEP(use, def, check) ((def & check->useMask) || \
                                        ((use | def) & check->defMask))

/* Scheduler heuristics */
#define MAX_HOIST_DISTANCE 20
#define LDLD_DISTANCE 4
#define LD_LATENCY 2

inline bool isDalvikRegisterClobbered(LIR* lir1, LIR* lir2)
{
    int reg1Lo = DECODE_ALIAS_INFO_REG(lir1->aliasInfo);
    int reg1Hi = reg1Lo + DECODE_ALIAS_INFO_WIDE(lir1->aliasInfo);
    int reg2Lo = DECODE_ALIAS_INFO_REG(lir2->aliasInfo);
    int reg2Hi = reg2Lo + DECODE_ALIAS_INFO_WIDE(lir2->aliasInfo);

    return (reg1Lo == reg2Lo) || (reg1Lo == reg2Hi) || (reg1Hi == reg2Lo);
}

/* Convert a more expensive instruction (ie load) into a move */
void convertMemOpIntoMove(CompilationUnit* cUnit, LIR* origLIR, int dest,
                          int src)
{
    /* Insert a move to replace the load */
    LIR* moveLIR;
    moveLIR = oatRegCopyNoInsert( cUnit, dest, src);
    /*
     * Insert the converted instruction after the original since the
     * optimization is scannng in the top-down order and the new instruction
     * will need to be re-checked (eg the new dest clobbers the src used in
     * thisLIR).
     */
    oatInsertLIRAfter((LIR*) origLIR, (LIR*) moveLIR);
}

/*
 * Perform a pass of top-down walk, from the second-last instruction in the
 * superblock, to eliminate redundant loads and stores.
 *
 * An earlier load can eliminate a later load iff
 *   1) They are must-aliases
 *   2) The native register is not clobbered in between
 *   3) The memory location is not written to in between
 *
 * An earlier store can eliminate a later load iff
 *   1) They are must-aliases
 *   2) The native register is not clobbered in between
 *   3) The memory location is not written to in between
 *
 * A later store can be eliminated by an earlier store iff
 *   1) They are must-aliases
 *   2) The memory location is not written to in between
 */
void applyLoadStoreElimination(CompilationUnit* cUnit, LIR* headLIR,
                               LIR* tailLIR)
{
    LIR* thisLIR;

    if (headLIR == tailLIR) return;

    for (thisLIR = PREV_LIR(tailLIR);
         thisLIR != headLIR;
         thisLIR = PREV_LIR(thisLIR)) {
        int sinkDistance = 0;

        /* Skip non-interesting instructions */
        if ((thisLIR->flags.isNop == true) ||
            isPseudoOpcode(thisLIR->opcode) ||
            !(EncodingMap[thisLIR->opcode].flags & (IS_LOAD | IS_STORE))) {
            continue;
        }

        int nativeRegId = thisLIR->operands[0];
        bool isThisLIRLoad = EncodingMap[thisLIR->opcode].flags & IS_LOAD;
        LIR* checkLIR;
        /* Use the mem mask to determine the rough memory location */
        u8 thisMemMask = (thisLIR->useMask | thisLIR->defMask) & ENCODE_MEM;

        /*
         * Currently only eliminate redundant ld/st for constant and Dalvik
         * register accesses.
         */
        if (!(thisMemMask & (ENCODE_LITERAL | ENCODE_DALVIK_REG))) continue;

// FIXME: make sure we have a branch barrier for x86
#if defined(TARGET_X86)
        u8 stopUseRegMask = (thisLIR->useMask) & ~ENCODE_MEM;
#else
        /*
         * Add r15 (pc) to the resource mask to prevent this instruction
         * from sinking past branch instructions. Also take out the memory
         * region bits since stopMask is used to check data/control
         * dependencies.
         */
        u8 stopUseRegMask = (ENCODE_REG_PC | thisLIR->useMask) &
                            ~ENCODE_MEM;
#endif
        u8 stopDefRegMask = thisLIR->defMask & ~ENCODE_MEM;

        for (checkLIR = NEXT_LIR(thisLIR);
             checkLIR != tailLIR;
             checkLIR = NEXT_LIR(checkLIR)) {

            /*
             * Skip already dead instructions (whose dataflow information is
             * outdated and misleading).
             */
            if (checkLIR->flags.isNop) continue;

            u8 checkMemMask = (checkLIR->useMask | checkLIR->defMask) &
                              ENCODE_MEM;
            u8 aliasCondition = thisMemMask & checkMemMask;
            bool stopHere = false;

            /*
             * Potential aliases seen - check the alias relations
             */
            if (checkMemMask != ENCODE_MEM && aliasCondition != 0) {
                bool isCheckLIRLoad = EncodingMap[checkLIR->opcode].flags &
                                      IS_LOAD;
                if  (aliasCondition == ENCODE_LITERAL) {
                    /*
                     * Should only see literal loads in the instruction
                     * stream.
                     */
                    DCHECK(!(EncodingMap[checkLIR->opcode].flags &
                             IS_STORE));
                    /* Same value && same register type */
                    if (checkLIR->aliasInfo == thisLIR->aliasInfo &&
                        REGTYPE(checkLIR->operands[0]) == REGTYPE(nativeRegId)) {
                        /*
                         * Different destination register - insert
                         * a move
                         */
                        if (checkLIR->operands[0] != nativeRegId) {
                            convertMemOpIntoMove(cUnit, checkLIR,
                                                 checkLIR->operands[0],
                                                 nativeRegId);
                        }
                        checkLIR->flags.isNop = true;
                    }
                } else if (aliasCondition == ENCODE_DALVIK_REG) {
                    /* Must alias */
                    if (checkLIR->aliasInfo == thisLIR->aliasInfo) {
                        /* Only optimize compatible registers */
                        bool regCompatible =
                            REGTYPE(checkLIR->operands[0]) ==
                            REGTYPE(nativeRegId);
                        if ((isThisLIRLoad && isCheckLIRLoad) ||
                            (!isThisLIRLoad && isCheckLIRLoad)) {
                            /* RAR or RAW */
                            if (regCompatible) {
                                /*
                                 * Different destination register -
                                 * insert a move
                                 */
                                if (checkLIR->operands[0] !=
                                    nativeRegId) {
                                    convertMemOpIntoMove(cUnit,
                                                 checkLIR,
                                                 checkLIR->operands[0],
                                                 nativeRegId);
                                }
                                checkLIR->flags.isNop = true;
                            } else {
                                /*
                                 * Destinaions are of different types -
                                 * something complicated going on so
                                 * stop looking now.
                                 */
                                stopHere = true;
                            }
                        } else if (isThisLIRLoad && !isCheckLIRLoad) {
                            /* WAR - register value is killed */
                            stopHere = true;
                        } else if (!isThisLIRLoad && !isCheckLIRLoad) {
                            /* WAW - nuke the earlier store */
                            thisLIR->flags.isNop = true;
                            stopHere = true;
                        }
                    /* Partial overlap */
                    } else if (isDalvikRegisterClobbered(thisLIR, checkLIR)) {
                        /*
                         * It is actually ok to continue if checkLIR
                         * is a read. But it is hard to make a test
                         * case for this so we just stop here to be
                         * conservative.
                         */
                        stopHere = true;
                    }
                }
                /* Memory content may be updated. Stop looking now. */
                if (stopHere) {
                    break;
                /* The checkLIR has been transformed - check the next one */
                } else if (checkLIR->flags.isNop) {
                    continue;
                }
            }


            /*
             * this and check LIRs have no memory dependency. Now check if
             * their register operands have any RAW, WAR, and WAW
             * dependencies. If so, stop looking.
             */
            if (stopHere == false) {
                stopHere = CHECK_REG_DEP(stopUseRegMask, stopDefRegMask,
                                         checkLIR);
            }

            if (stopHere == true) {
                DEBUG_OPT(dumpDependentInsnPair(thisLIR, checkLIR,
                                                "REG CLOBBERED"));
                /* Only sink store instructions */
                if (sinkDistance && !isThisLIRLoad) {
                    LIR* newStoreLIR =
                        (LIR* ) oatNew(cUnit, sizeof(LIR), true,
                                       kAllocLIR);
                    *newStoreLIR = *thisLIR;
                    /*
                     * Stop point found - insert *before* the checkLIR
                     * since the instruction list is scanned in the
                     * top-down order.
                     */
                    oatInsertLIRBefore((LIR*) checkLIR,
                                               (LIR*) newStoreLIR);
                    thisLIR->flags.isNop = true;
                }
                break;
            } else if (!checkLIR->flags.isNop) {
                sinkDistance++;
            }
        }
    }
}

/*
 * Perform a pass of bottom-up walk, from the second instruction in the
 * superblock, to try to hoist loads to earlier slots.
 */
void applyLoadHoisting(CompilationUnit* cUnit, LIR* headLIR, LIR* tailLIR)
{
    LIR* thisLIR, *checkLIR;
    /*
     * Store the list of independent instructions that can be hoisted past.
     * Will decide the best place to insert later.
     */
    LIR* prevInstList[MAX_HOIST_DISTANCE];

    /* Empty block */
    if (headLIR == tailLIR) return;

    /* Start from the second instruction */
    for (thisLIR = NEXT_LIR(headLIR);
         thisLIR != tailLIR;
         thisLIR = NEXT_LIR(thisLIR)) {

        /* Skip non-interesting instructions */
        if ((thisLIR->flags.isNop == true) ||
            isPseudoOpcode(thisLIR->opcode) ||
            !(EncodingMap[thisLIR->opcode].flags & IS_LOAD)) {
            continue;
        }

        u8 stopUseAllMask = thisLIR->useMask;

#if !defined(TARGET_X86)
        /*
         * Branches for null/range checks are marked with the true resource
         * bits, and loads to Dalvik registers, constant pools, and non-alias
         * locations are safe to be hoisted. So only mark the heap references
         * conservatively here.
         */
        if (stopUseAllMask & ENCODE_HEAP_REF) {
            stopUseAllMask |= ENCODE_REG_PC;
        }
#endif

        /* Similar as above, but just check for pure register dependency */
        u8 stopUseRegMask = stopUseAllMask & ~ENCODE_MEM;
        u8 stopDefRegMask = thisLIR->defMask & ~ENCODE_MEM;

        int nextSlot = 0;
        bool stopHere = false;

        /* Try to hoist the load to a good spot */
        for (checkLIR = PREV_LIR(thisLIR);
             checkLIR != headLIR;
             checkLIR = PREV_LIR(checkLIR)) {

            /*
             * Skip already dead instructions (whose dataflow information is
             * outdated and misleading).
             */
            if (checkLIR->flags.isNop) continue;

            u8 checkMemMask = checkLIR->defMask & ENCODE_MEM;
            u8 aliasCondition = stopUseAllMask & checkMemMask;
            stopHere = false;

            /* Potential WAR alias seen - check the exact relation */
            if (checkMemMask != ENCODE_MEM && aliasCondition != 0) {
                /* We can fully disambiguate Dalvik references */
                if (aliasCondition == ENCODE_DALVIK_REG) {
                    /* Must alias or partually overlap */
                    if ((checkLIR->aliasInfo == thisLIR->aliasInfo) ||
                        isDalvikRegisterClobbered(thisLIR, checkLIR)) {
                        stopHere = true;
                    }
                /* Conservatively treat all heap refs as may-alias */
                } else {
                    DCHECK_EQ(aliasCondition, ENCODE_HEAP_REF);
                    stopHere = true;
                }
                /* Memory content may be updated. Stop looking now. */
                if (stopHere) {
                    prevInstList[nextSlot++] = checkLIR;
                    break;
                }
            }

            if (stopHere == false) {
                stopHere = CHECK_REG_DEP(stopUseRegMask, stopDefRegMask,
                                         checkLIR);
            }

            /*
             * Store the dependent or non-pseudo/indepedent instruction to the
             * list.
             */
            if (stopHere || !isPseudoOpcode(checkLIR->opcode)) {
                prevInstList[nextSlot++] = checkLIR;
                if (nextSlot == MAX_HOIST_DISTANCE) break;
            }

            /* Found a new place to put the load - move it here */
            if (stopHere == true) {
                DEBUG_OPT(dumpDependentInsnPair(checkLIR, thisLIR
                                                "HOIST STOP"));
                break;
            }
        }

        /*
         * Reached the top - use headLIR as the dependent marker as all labels
         * are barriers.
         */
        if (stopHere == false && nextSlot < MAX_HOIST_DISTANCE) {
            prevInstList[nextSlot++] = headLIR;
        }

        /*
         * At least one independent instruction is found. Scan in the reversed
         * direction to find a beneficial slot.
         */
        if (nextSlot >= 2) {
            int firstSlot = nextSlot - 2;
            int slot;
            LIR* depLIR = prevInstList[nextSlot-1];
            /* If there is ld-ld dependency, wait LDLD_DISTANCE cycles */
            if (!isPseudoOpcode(depLIR->opcode) &&
                (EncodingMap[depLIR->opcode].flags & IS_LOAD)) {
                firstSlot -= LDLD_DISTANCE;
            }
            /*
             * Make sure we check slot >= 0 since firstSlot may be negative
             * when the loop is first entered.
             */
            for (slot = firstSlot; slot >= 0; slot--) {
                LIR* curLIR = prevInstList[slot];
                LIR* prevLIR = prevInstList[slot+1];

                /* Check the highest instruction */
                if (prevLIR->defMask == ENCODE_ALL) {
                    /*
                     * If the first instruction is a load, don't hoist anything
                     * above it since it is unlikely to be beneficial.
                     */
                    if (EncodingMap[curLIR->opcode].flags & IS_LOAD) continue;
                    /*
                     * If the remaining number of slots is less than LD_LATENCY,
                     * insert the hoisted load here.
                     */
                    if (slot < LD_LATENCY) break;
                }

                /*
                 * NOTE: now prevLIR is guaranteed to be a non-pseudo
                 * instruction (ie accessing EncodingMap[prevLIR->opcode] is
                 * safe).
                 *
                 * Try to find two instructions with load/use dependency until
                 * the remaining instructions are less than LD_LATENCY.
                 */
                if (((curLIR->useMask & prevLIR->defMask) &&
                     (EncodingMap[prevLIR->opcode].flags & IS_LOAD)) ||
                    (slot < LD_LATENCY)) {
                    break;
                }
            }

            /* Found a slot to hoist to */
            if (slot >= 0) {
                LIR* curLIR = prevInstList[slot];
                LIR* newLoadLIR = (LIR* ) oatNew(cUnit, sizeof(LIR),
                                                       true, kAllocLIR);
                *newLoadLIR = *thisLIR;
                /*
                 * Insertion is guaranteed to succeed since checkLIR
                 * is never the first LIR on the list
                 */
                oatInsertLIRBefore((LIR*) curLIR, (LIR*) newLoadLIR);
                thisLIR->flags.isNop = true;
            }
        }
    }
}

void oatApplyLocalOptimizations(CompilationUnit* cUnit, LIR* headLIR,
                                        LIR* tailLIR)
{
    if (!(cUnit->disableOpt & (1 << kLoadStoreElimination))) {
        applyLoadStoreElimination(cUnit, (LIR* ) headLIR,
                                  (LIR* ) tailLIR);
    }
    if (!(cUnit->disableOpt & (1 << kLoadHoisting))) {
        applyLoadHoisting(cUnit, (LIR* ) headLIR, (LIR* ) tailLIR);
    }
}

}  // namespace art
