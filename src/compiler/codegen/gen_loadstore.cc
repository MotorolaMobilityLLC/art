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

/* This file contains target-independent codegen and support. */

/*
 * Load an immediate value into a fixed or temp register.  Target
 * register is clobbered, and marked inUse.
 */
LIR* loadConstant(CompilationUnit* cUnit, int rDest, int value)
{
  if (oatIsTemp(cUnit, rDest)) {
    oatClobber(cUnit, rDest);
    oatMarkInUse(cUnit, rDest);
  }
  return loadConstantNoClobber(cUnit, rDest, value);
}

/* Load a word at base + displacement.  Displacement must be word multiple */
LIR* loadWordDisp(CompilationUnit* cUnit, int rBase, int displacement,
                  int rDest)
{
  return loadBaseDisp(cUnit, rBase, displacement, rDest, kWord,
                      INVALID_SREG);
}

LIR* storeWordDisp(CompilationUnit* cUnit, int rBase, int displacement,
                   int rSrc)
{
  return storeBaseDisp(cUnit, rBase, displacement, rSrc, kWord);
}

/*
 * Load a Dalvik register into a physical register.  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void loadValueDirect(CompilationUnit* cUnit, RegLocation rlSrc, int rDest)
{
  rlSrc = oatUpdateLoc(cUnit, rlSrc);
  if (rlSrc.location == kLocPhysReg) {
    opRegCopy(cUnit, rDest, rlSrc.lowReg);
  } else {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    loadWordDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, rlSrc.sRegLow), rDest);
  }
}

/*
 * Similar to loadValueDirect, but clobbers and allocates the target
 * register.  Should be used when loading to a fixed register (for example,
 * loading arguments to an out of line call.
 */
void loadValueDirectFixed(CompilationUnit* cUnit, RegLocation rlSrc, int rDest)
{
  oatClobber(cUnit, rDest);
  oatMarkInUse(cUnit, rDest);
  loadValueDirect(cUnit, rlSrc, rDest);
}

/*
 * Load a Dalvik register pair into a physical register[s].  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void loadValueDirectWide(CompilationUnit* cUnit, RegLocation rlSrc, int regLo,
             int regHi)
{
  rlSrc = oatUpdateLocWide(cUnit, rlSrc);
  if (rlSrc.location == kLocPhysReg) {
    opRegCopyWide(cUnit, regLo, regHi, rlSrc.lowReg, rlSrc.highReg);
  } else {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    loadBaseDispWide(cUnit, targetReg(kSp), oatSRegOffset(cUnit, rlSrc.sRegLow),
                     regLo, regHi, INVALID_SREG);
  }
}

/*
 * Similar to loadValueDirect, but clobbers and allocates the target
 * registers.  Should be used when loading to a fixed registers (for example,
 * loading arguments to an out of line call.
 */
void loadValueDirectWideFixed(CompilationUnit* cUnit, RegLocation rlSrc,
                              int regLo, int regHi)
{
  oatClobber(cUnit, regLo);
  oatClobber(cUnit, regHi);
  oatMarkInUse(cUnit, regLo);
  oatMarkInUse(cUnit, regHi);
  loadValueDirectWide(cUnit, rlSrc, regLo, regHi);
}

RegLocation loadValue(CompilationUnit* cUnit, RegLocation rlSrc,
                      RegisterClass opKind)
{
  rlSrc = oatEvalLoc(cUnit, rlSrc, opKind, false);
  if (rlSrc.location != kLocPhysReg) {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    loadValueDirect(cUnit, rlSrc, rlSrc.lowReg);
    rlSrc.location = kLocPhysReg;
    oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
  }
  return rlSrc;
}

void storeValue(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening oatClobberSReg().
   */
  DCHECK((cUnit->liveSReg == INVALID_SREG) ||
         (rlDest.sRegLow != cUnit->liveSReg));
  cUnit->liveSReg = rlDest.sRegLow;
#endif
  LIR* defStart;
  LIR* defEnd;
  DCHECK(!rlDest.wide);
  DCHECK(!rlSrc.wide);
  rlSrc = oatUpdateLoc(cUnit, rlSrc);
  rlDest = oatUpdateLoc(cUnit, rlDest);
  if (rlSrc.location == kLocPhysReg) {
    if (oatIsLive(cUnit, rlSrc.lowReg) ||
      oatIsPromoted(cUnit, rlSrc.lowReg) ||
      (rlDest.location == kLocPhysReg)) {
      // Src is live/promoted or Dest has assigned reg.
      rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
      opRegCopy(cUnit, rlDest.lowReg, rlSrc.lowReg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rlDest.lowReg = rlSrc.lowReg;
      oatClobber(cUnit, rlSrc.lowReg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
    loadValueDirect(cUnit, rlSrc, rlDest.lowReg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  oatMarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
  oatMarkDirty(cUnit, rlDest);


  oatResetDefLoc(cUnit, rlDest);
  if (oatIsDirty(cUnit, rlDest.lowReg) &&
      oatLiveOut(cUnit, rlDest.sRegLow)) {
    defStart = cUnit->lastLIRInsn;
    storeBaseDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, rlDest.sRegLow),
                  rlDest.lowReg, kWord);
    oatMarkClean(cUnit, rlDest);
    defEnd = cUnit->lastLIRInsn;
    oatMarkDef(cUnit, rlDest, defStart, defEnd);
  }
}

RegLocation loadValueWide(CompilationUnit* cUnit, RegLocation rlSrc,
              RegisterClass opKind)
{
  DCHECK(rlSrc.wide);
  rlSrc = oatEvalLoc(cUnit, rlSrc, opKind, false);
  if (rlSrc.location != kLocPhysReg) {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
        (rlSrc.location == kLocCompilerTemp));
    loadValueDirectWide(cUnit, rlSrc, rlSrc.lowReg, rlSrc.highReg);
    rlSrc.location = kLocPhysReg;
    oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
    oatMarkLive(cUnit, rlSrc.highReg,
                oatSRegHi(rlSrc.sRegLow));
  }
  return rlSrc;
}

void storeValueWide(CompilationUnit* cUnit, RegLocation rlDest,
          RegLocation rlSrc)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening oatClobberSReg().
   */
  DCHECK((cUnit->liveSReg == INVALID_SREG) ||
      (rlDest.sRegLow != cUnit->liveSReg));
  cUnit->liveSReg = rlDest.sRegLow;
#endif
  LIR* defStart;
  LIR* defEnd;
  DCHECK_EQ(fpReg(rlSrc.lowReg), fpReg(rlSrc.highReg));
  DCHECK(rlDest.wide);
  DCHECK(rlSrc.wide);
  if (rlSrc.location == kLocPhysReg) {
    if (oatIsLive(cUnit, rlSrc.lowReg) ||
        oatIsLive(cUnit, rlSrc.highReg) ||
        oatIsPromoted(cUnit, rlSrc.lowReg) ||
        oatIsPromoted(cUnit, rlSrc.highReg) ||
        (rlDest.location == kLocPhysReg)) {
      // Src is live or promoted or Dest has assigned reg.
      rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
      opRegCopyWide(cUnit, rlDest.lowReg, rlDest.highReg,
                    rlSrc.lowReg, rlSrc.highReg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rlDest.lowReg = rlSrc.lowReg;
      rlDest.highReg = rlSrc.highReg;
      oatClobber(cUnit, rlSrc.lowReg);
      oatClobber(cUnit, rlSrc.highReg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
    loadValueDirectWide(cUnit, rlSrc, rlDest.lowReg, rlDest.highReg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  oatMarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
  oatMarkLive(cUnit, rlDest.highReg, oatSRegHi(rlDest.sRegLow));
  oatMarkDirty(cUnit, rlDest);
  oatMarkPair(cUnit, rlDest.lowReg, rlDest.highReg);


  oatResetDefLocWide(cUnit, rlDest);
  if ((oatIsDirty(cUnit, rlDest.lowReg) ||
      oatIsDirty(cUnit, rlDest.highReg)) &&
      (oatLiveOut(cUnit, rlDest.sRegLow) ||
      oatLiveOut(cUnit, oatSRegHi(rlDest.sRegLow)))) {
    defStart = cUnit->lastLIRInsn;
    DCHECK_EQ((SRegToVReg(cUnit, rlDest.sRegLow)+1),
              SRegToVReg(cUnit, oatSRegHi(rlDest.sRegLow)));
    storeBaseDispWide(cUnit, targetReg(kSp), oatSRegOffset(cUnit, rlDest.sRegLow),
                      rlDest.lowReg, rlDest.highReg);
    oatMarkClean(cUnit, rlDest);
    defEnd = cUnit->lastLIRInsn;
    oatMarkDefWide(cUnit, rlDest, defStart, defEnd);
  }
}

/* Utilities to load the current Method* */
void loadCurrMethodDirect(CompilationUnit *cUnit, int rTgt)
{
  loadValueDirectFixed(cUnit, cUnit->methodLoc, rTgt);
}

RegLocation loadCurrMethod(CompilationUnit *cUnit)
{
  return loadValue(cUnit, cUnit->methodLoc, kCoreReg);
}

bool methodStarInReg(CompilationUnit* cUnit)
{
   return (cUnit->methodLoc.location == kLocPhysReg);
}


}  // namespace art
