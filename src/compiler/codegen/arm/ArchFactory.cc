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

/* This file contains arm-specific codegen factory support. */

#include "oat/runtime/oat_support_entrypoints.h"

namespace art {

bool genNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  int zReg = oatAllocTemp(cUnit);
  loadConstantNoClobber(cUnit, zReg, 0);
  // Check for destructive overlap
  if (rlResult.lowReg == rlSrc.highReg) {
    int tReg = oatAllocTemp(cUnit);
    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    opRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, tReg);
    oatFreeTemp(cUnit, tReg);
  } else {
    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    opRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, rlSrc.highReg);
  }
  oatFreeTemp(cUnit, zReg);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

int loadHelper(CompilationUnit* cUnit, int offset)
{
  loadWordDisp(cUnit, rARM_SELF, offset, rARM_LR);
  return rARM_LR;
}

void genEntrySequence(CompilationUnit* cUnit, RegLocation* argLocs,
                      RegLocation rlMethod)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  oatLockTemp(cUnit, r0);
  oatLockTemp(cUnit, r1);
  oatLockTemp(cUnit, r2);
  oatLockTemp(cUnit, r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                            ((size_t)cUnit->frameSize <
                            Thread::kStackOverflowReservedBytes));
  newLIR0(cUnit, kPseudoMethodEntry);
  if (!skipOverflowCheck) {
    /* Load stack limit */
    loadWordDisp(cUnit, rARM_SELF, Thread::StackEndOffset().Int32Value(), r12);
  }
  /* Spill core callee saves */
  newLIR1(cUnit, kThumb2Push, cUnit->coreSpillMask);
  /* Need to spill any FP regs? */
  if (cUnit->numFPSpills) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    newLIR1(cUnit, kThumb2VPushCS, cUnit->numFPSpills);
  }
  if (!skipOverflowCheck) {
    opRegRegImm(cUnit, kOpSub, rARM_LR, rARM_SP, cUnit->frameSize - (spillCount * 4));
    genRegRegCheck(cUnit, kCondCc, rARM_LR, r12, kThrowStackOverflow);
    opRegCopy(cUnit, rARM_SP, rARM_LR);     // Establish stack
  } else {
    opRegImm(cUnit, kOpSub, rARM_SP, cUnit->frameSize - (spillCount * 4));
  }

  flushIns(cUnit, argLocs, rlMethod);

  oatFreeTemp(cUnit, r0);
  oatFreeTemp(cUnit, r1);
  oatFreeTemp(cUnit, r2);
  oatFreeTemp(cUnit, r3);
}

void genExitSequence(CompilationUnit* cUnit)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  oatLockTemp(cUnit, r0);
  oatLockTemp(cUnit, r1);

  newLIR0(cUnit, kPseudoMethodExit);
  opRegImm(cUnit, kOpAdd, rARM_SP, cUnit->frameSize - (spillCount * 4));
  /* Need to restore any FP callee saves? */
  if (cUnit->numFPSpills) {
    newLIR1(cUnit, kThumb2VPopCS, cUnit->numFPSpills);
  }
  if (cUnit->coreSpillMask & (1 << rARM_LR)) {
    /* Unspill rARM_LR to rARM_PC */
    cUnit->coreSpillMask &= ~(1 << rARM_LR);
    cUnit->coreSpillMask |= (1 << rARM_PC);
  }
  newLIR1(cUnit, kThumb2Pop, cUnit->coreSpillMask);
  if (!(cUnit->coreSpillMask & (1 << rARM_PC))) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    newLIR1(cUnit, kThumbBx, rARM_LR);
  }
}

/*
 * Nop any unconditional branches that go to the next instruction.
 * Note: new redundant branches may be inserted later, and we'll
 * use a check in final instruction assembly to nop those out.
 */
void removeRedundantBranches(CompilationUnit* cUnit)
{
  LIR* thisLIR;

  for (thisLIR = (LIR*) cUnit->firstLIRInsn;
     thisLIR != (LIR*) cUnit->lastLIRInsn;
     thisLIR = NEXT_LIR(thisLIR)) {

    /* Branch to the next instruction */
    if ((thisLIR->opcode == kThumbBUncond) ||
      (thisLIR->opcode == kThumb2BUncond)) {
      LIR* nextLIR = thisLIR;

      while (true) {
        nextLIR = NEXT_LIR(nextLIR);

        /*
         * Is the branch target the next instruction?
         */
        if (nextLIR == (LIR*) thisLIR->target) {
          thisLIR->flags.isNop = true;
          break;
        }

        /*
         * Found real useful stuff between the branch and the target.
         * Need to explicitly check the lastLIRInsn here because it
         * might be the last real instruction.
         */
        if (!isPseudoOpcode(nextLIR->opcode) ||
          (nextLIR = (LIR*) cUnit->lastLIRInsn))
          break;
      }
    }
  }
}


/* Common initialization routine for an architecture family */
bool oatArchInit()
{
  int i;

  for (i = 0; i < kArmLast; i++) {
    if (EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << (int)EncodingMap[i].opcode;
    }
  }

  return oatArchVariantInit();
}

bool genAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genAddLong for Arm";
  return false;
}

bool genSubLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genSubLong for Arm";
  return false;
}

bool genAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genAndLong for Arm";
  return false;
}

bool genOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genOrLong for Arm";
  return false;
}

bool genXorLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genXoLong for Arm";
  return false;
}

}  // namespace art
