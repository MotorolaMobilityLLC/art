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

/*
 * This file contains arm-specific codegen factory support.
 * It is included by
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

static ArmLIR* genUnconditionalBranch(CompilationUnit*, ArmLIR*);
static ArmLIR* genConditionalBranch(CompilationUnit*, ArmConditionCode,
                                    ArmLIR*);

/*
 * Utiltiy to load the current Method*.  Broken out
 * to allow easy change between placing the current Method* in a
 * dedicated register or its home location in the frame.
 */
static void loadCurrMethodDirect(CompilationUnit *cUnit, int rTgt)
{
#if defined(METHOD_IN_REG)
    genRegCopy(cUnit, rTgt, rMETHOD);
#else
    loadWordDisp(cUnit, rSP, 0, rTgt);
#endif
}

static int loadCurrMethod(CompilationUnit *cUnit)
{
#if defined(METHOD_IN_REG)
    return rMETHOD;
#else
    int mReg = oatAllocTemp(cUnit);
    loadCurrMethodDirect(cUnit, mReg);
    return mReg;
#endif
}

static ArmLIR* genImmedCheck(CompilationUnit* cUnit, ArmConditionCode cCode,
                             int reg, int immVal, MIR* mir, ArmThrowKind kind)
{
    ArmLIR* tgt = (ArmLIR*)oatNew(sizeof(ArmLIR), true);
    tgt->opcode = kArmPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir->offset;
    ArmLIR* branch;
    if (cCode == kArmCondAl) {
        branch = genUnconditionalBranch(cUnit, tgt);
    } else {
        branch = genCmpImmBranch(cUnit, kArmCondEq, reg, 0);
        branch->generic.target = (LIR*)tgt;
    }
    // Remember branch target - will process later
    oatInsertGrowableList(&cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

/*
 * Perform null-check on a register. sReg is the ssa register being checked,
 * and mReg is the machine register holding the actual value. If internal state
 * indicates that sReg has been checked before the check request is ignored.
 */
static ArmLIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg,
                             MIR* mir)
{
    if (oatIsBitSet(cUnit->regPool->nullCheckedRegs, sReg)) {
        /* This particular Dalvik register has been null-checked */
        return NULL;
    }
    oatSetBit(cUnit->regPool->nullCheckedRegs, sReg);
    return genImmedCheck(cUnit, kArmCondEq, mReg, 0, mir, kArmThrowNullPointer);
}

/* Perform bound check on two registers */
static TGT_LIR* genBoundsCheck(CompilationUnit* cUnit, int rIndex,
                               int rBound, MIR* mir, ArmThrowKind kind)
{
    ArmLIR* tgt = (ArmLIR*)oatNew(sizeof(ArmLIR), true);
    tgt->opcode = kArmPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir->offset;
    tgt->operands[2] = rIndex;
    tgt->operands[3] = rBound;
    opRegReg(cUnit, kOpCmp, rIndex, rBound);
    ArmLIR* branch = genConditionalBranch(cUnit, kArmCondCs, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(&cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}
