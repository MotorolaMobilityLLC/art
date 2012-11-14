/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "../../CompilerInternals.h"
#include "MipsLIR.h"
#include "../Ralloc.h"

#include <string>

namespace art {

RegLocation locCReturn()
{
  RegLocation res = MIPS_LOC_C_RETURN;
  return res;
}

RegLocation locCReturnWide()
{
  RegLocation res = MIPS_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation locCReturnFloat()
{
  RegLocation res = MIPS_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation locCReturnDouble()
{
  RegLocation res = MIPS_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int targetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rMIPS_SELF; break;
    case kSuspend: res =  rMIPS_SUSPEND; break;
    case kLr: res =  rMIPS_LR; break;
    case kPc: res =  rMIPS_PC; break;
    case kSp: res =  rMIPS_SP; break;
    case kArg0: res = rMIPS_ARG0; break;
    case kArg1: res = rMIPS_ARG1; break;
    case kArg2: res = rMIPS_ARG2; break;
    case kArg3: res = rMIPS_ARG3; break;
    case kFArg0: res = rMIPS_FARG0; break;
    case kFArg1: res = rMIPS_FARG1; break;
    case kFArg2: res = rMIPS_FARG2; break;
    case kFArg3: res = rMIPS_FARG3; break;
    case kRet0: res = rMIPS_RET0; break;
    case kRet1: res = rMIPS_RET1; break;
    case kInvokeTgt: res = rMIPS_INVOKE_TGT; break;
    case kCount: res = rMIPS_COUNT; break;
  }
  return res;
}

// Create a double from a pair of singles.
int s2d(int lowReg, int highReg)
{
  return MIPS_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool fpReg(int reg)
{
  return MIPS_FPREG(reg);
}

// Is reg a single?
bool singleReg(int reg)
{
  return MIPS_SINGLEREG(reg);
}

// Is reg a double?
bool doubleReg(int reg)
{
  return MIPS_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t fpRegMask()
{
  return MIPS_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool sameRegType(int reg1, int reg2)
{
  return (MIPS_REGTYPE(reg1) == MIPS_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
u8 getRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  u8 seed;
  int shift;
  int regId;


  regId = reg & 0x1f;
  /* Each double register is equal to a pair of single-precision FP registers */
  seed = MIPS_DOUBLEREG(reg) ? 3 : 1;
  /* FP register starts at bit position 16 */
  shift = MIPS_FPREG(reg) ? kMipsFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += regId;
  return (seed << shift);
}

uint64_t getPCUseDefEncoding()
{
  return ENCODE_MIPS_REG_PC;
}


void setupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  DCHECK_EQ(cUnit->instructionSet, kMips);

  // Mips-specific resource map setup here.
  uint64_t flags = EncodingMap[lir->opcode].flags;

  if (flags & REG_DEF_SP) {
    lir->defMask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_USE_SP) {
    lir->useMask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_DEF_LR) {
    lir->defMask |= ENCODE_MIPS_REG_LR;
  }
}

/* For dumping instructions */
#define MIPS_REG_COUNT 32
static const char *mipsRegName[MIPS_REG_COUNT] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string buildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr)
{
  std::string buf;
  int i;
  const char *fmtEnd = &fmt[strlen(fmt)];
  char tbuf[256];
  char nc;
  while (fmt < fmtEnd) {
    int operand;
    if (*fmt == '!') {
      fmt++;
      DCHECK_LT(fmt, fmtEnd);
      nc = *fmt++;
      if (nc=='!') {
        strcpy(tbuf, "!");
      } else {
         DCHECK_LT(fmt, fmtEnd);
         DCHECK_LT((unsigned)(nc-'0'), 4u);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'b':
             strcpy(tbuf,"0000");
             for (i=3; i>= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 's':
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'S':
             DCHECK_EQ(((operand & MIPS_FP_REG_MASK) & 1), 0);
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'h':
             sprintf(tbuf,"%04x", operand);
             break;
           case 'M':
           case 'd':
             sprintf(tbuf,"%d", operand);
             break;
           case 'D':
             sprintf(tbuf,"%d", operand+1);
             break;
           case 'E':
             sprintf(tbuf,"%d", operand*4);
             break;
           case 'F':
             sprintf(tbuf,"%d", operand*2);
             break;
           case 't':
             sprintf(tbuf,"0x%08x (L%p)", (int) baseAddr + lir->offset + 4 +
                     (operand << 2), lir->target);
             break;
           case 'T':
             sprintf(tbuf,"0x%08x", (int) (operand << 2));
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             intptr_t target =
                 ((((intptr_t) baseAddr + lir->offset + 4) & ~3) +
                 (offset_1 << 21 >> 9) + (offset_2 << 1)) & 0xfffffffc;
             sprintf(tbuf, "%p", (void *) target);
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'r':
             DCHECK(operand >= 0 && operand < MIPS_REG_COUNT);
             strcpy(tbuf, mipsRegName[operand]);
             break;
           case 'N':
             // Placeholder for delay slot handling
             strcpy(tbuf, ";  nop");
             break;
           default:
             strcpy(tbuf,"DecodeError");
             break;
         }
         buf += tbuf;
      }
    } else {
       buf += *fmt++;
    }
  }
  return buf;
}

// FIXME: need to redo resource maps for MIPS - fix this at that time
void oatDumpResourceMask(LIR *lir, u8 mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;
  LIR *mipsLIR = (LIR *) lir;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kMipsRegEnd; i++) {
      if (mask & (1ULL << i)) {
        sprintf(num, "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    if (mask & ENCODE_FP_STATUS) {
      strcat(buf, "fpcc ");
    }
    /* Memory bits */
    if (mipsLIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", mipsLIR->aliasInfo & 0xffff,
              (mipsLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
    }
    if (mask & ENCODE_LITERAL) {
      strcat(buf, "lit ");
    }

    if (mask & ENCODE_HEAP_REF) {
      strcat(buf, "heap ");
    }
    if (mask & ENCODE_MUST_NOT_ALIAS) {
      strcat(buf, "noalias ");
    }
  }
  if (buf[0]) {
    LOG(INFO) << prefix << ": " <<  buf;
  }
}

} // namespace art
