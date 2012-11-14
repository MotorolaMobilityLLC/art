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
#include "X86LIR.h"
#include "../Ralloc.h"

#include <string>

namespace art {

RegLocation locCReturn()
{
  RegLocation res = X86_LOC_C_RETURN;
  return res;
}

RegLocation locCReturnWide()
{
  RegLocation res = X86_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation locCReturnFloat()
{
  RegLocation res = X86_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation locCReturnDouble()
{
  RegLocation res = X86_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int targetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rX86_SELF; break;
    case kSuspend: res =  rX86_SUSPEND; break;
    case kLr: res =  rX86_LR; break;
    case kPc: res =  rX86_PC; break;
    case kSp: res =  rX86_SP; break;
    case kArg0: res = rX86_ARG0; break;
    case kArg1: res = rX86_ARG1; break;
    case kArg2: res = rX86_ARG2; break;
    case kArg3: res = rX86_ARG3; break;
    case kFArg0: res = rX86_FARG0; break;
    case kFArg1: res = rX86_FARG1; break;
    case kFArg2: res = rX86_FARG2; break;
    case kFArg3: res = rX86_FARG3; break;
    case kRet0: res = rX86_RET0; break;
    case kRet1: res = rX86_RET1; break;
    case kInvokeTgt: res = rX86_INVOKE_TGT; break;
    case kCount: res = rX86_COUNT; break;
  }
  return res;
}

// Create a double from a pair of singles.
int s2d(int lowReg, int highReg)
{
  return X86_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool fpReg(int reg)
{
  return X86_FPREG(reg);
}

// Is reg a single?
bool singleReg(int reg)
{
  return X86_SINGLEREG(reg);
}

// Is reg a double?
bool doubleReg(int reg)
{
  return X86_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t fpRegMask()
{
  return X86_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool sameRegType(int reg1, int reg2)
{
  return (X86_REGTYPE(reg1) == X86_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
u8 getRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  u8 seed;
  int shift;
  int regId;

  regId = reg & 0xf;
  /* Double registers in x86 are just a single FP register */
  seed = 1;
  /* FP register starts at bit position 16 */
  shift = X86_FPREG(reg) ? kX86FPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += regId;
  return (seed << shift);
}

uint64_t getPCUseDefEncoding()
{
  /*
   * FIXME: might make sense to use a virtual resource encoding bit for pc.  Might be
   * able to clean up some of the x86/Arm_Mips differences
   */
  LOG(FATAL) << "Unexpected call to getPCUseDefEncoding for x86";
  return 0ULL;
}

void setupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  DCHECK_EQ(cUnit->instructionSet, kX86);

  // X86-specific resource map setup here.
  uint64_t flags = EncodingMap[lir->opcode].flags;

  if (flags & REG_USE_SP) {
    lir->useMask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEF_SP) {
    lir->defMask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEFA) {
    oatSetupRegMask(cUnit, &lir->defMask, rAX);
  }

  if (flags & REG_DEFD) {
    oatSetupRegMask(cUnit, &lir->defMask, rDX);
  }
  if (flags & REG_USEA) {
    oatSetupRegMask(cUnit, &lir->useMask, rAX);
  }

  if (flags & REG_USEC) {
    oatSetupRegMask(cUnit, &lir->useMask, rCX);
  }

  if (flags & REG_USED) {
    oatSetupRegMask(cUnit, &lir->useMask, rDX);
  }
}

/* For dumping instructions */
static const char* x86RegName[] = {
  "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* x86CondName[] = {
  "O",
  "NO",
  "B/NAE/C",
  "NB/AE/NC",
  "Z/EQ",
  "NZ/NE",
  "BE/NA",
  "NBE/A",
  "S",
  "NS",
  "P/PE",
  "NP/PO",
  "L/NGE",
  "NL/GE",
  "LE/NG",
  "NLE/G"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.cc.
 */
std::string buildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr) {
  std::string buf;
  size_t i = 0;
  size_t fmt_len = strlen(fmt);
  while (i < fmt_len) {
    if (fmt[i] != '!') {
      buf += fmt[i];
      i++;
    } else {
      i++;
      DCHECK_LT(i, fmt_len);
      char operand_number_ch = fmt[i];
      i++;
      if (operand_number_ch == '!') {
        buf += "!";
      } else {
        int operand_number = operand_number_ch - '0';
        DCHECK_LT(operand_number, 6);  // Expect upto 6 LIR operands.
        DCHECK_LT(i, fmt_len);
        int operand = lir->operands[operand_number];
        switch (fmt[i]) {
          case 'c':
            DCHECK_LT(static_cast<size_t>(operand), sizeof(x86CondName));
            buf += x86CondName[operand];
            break;
          case 'd':
            buf += StringPrintf("%d", operand);
            break;
          case 'p': {
            SwitchTable *tabRec = reinterpret_cast<SwitchTable*>(operand);
            buf += StringPrintf("0x%08x", tabRec->offset);
            break;
          }
          case 'r':
            if (X86_FPREG(operand) || X86_DOUBLEREG(operand)) {
              int fp_reg = operand & X86_FP_REG_MASK;
              buf += StringPrintf("xmm%d", fp_reg);
            } else {
              DCHECK_LT(static_cast<size_t>(operand), sizeof(x86RegName));
              buf += x86RegName[operand];
            }
            break;
          case 't':
            buf += StringPrintf("0x%08x (L%p)",
                                reinterpret_cast<uint32_t>(baseAddr)
                                + lir->offset + operand, lir->target);
            break;
          default:
            buf += StringPrintf("DecodeError '%c'", fmt[i]);
            break;
        }
        i++;
      }
    }
  }
  return buf;
}

void oatDumpResourceMask(LIR *lir, u8 mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;
  LIR *x86LIR = (LIR *) lir;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kX86RegEnd; i++) {
      if (mask & (1ULL << i)) {
        sprintf(num, "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    /* Memory bits */
    if (x86LIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", x86LIR->aliasInfo & 0xffff,
              (x86LIR->aliasInfo & 0x80000000) ? "(+1)" : "");
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
