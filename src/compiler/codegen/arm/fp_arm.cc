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

#include "arm_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

bool GenArithOpFloat(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
                     RegLocation rl_src1, RegLocation rl_src2)
{
  int op = kThumbBkpt;
  RegLocation rl_result;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      op = kThumb2Vadds;
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      op = kThumb2Vsubs;
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      op = kThumb2Vdivs;
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      op = kThumb2Vmuls;
      break;
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
    case Instruction::NEG_FLOAT: {
      return GenArithOpFloatPortable(cu, opcode, rl_dest, rl_src1, rl_src2);
    }
    default:
      return true;
  }
  rl_src1 = LoadValue(cu, rl_src1, kFPReg);
  rl_src2 = LoadValue(cu, rl_src2, kFPReg);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  NewLIR3(cu, op, rl_result.low_reg, rl_src1.low_reg, rl_src2.low_reg);
  StoreValue(cu, rl_dest, rl_result);
  return false;
}

bool GenArithOpDouble(CompilationUnit* cu, Instruction::Code opcode,
                      RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)
{
  int op = kThumbBkpt;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      op = kThumb2Vaddd;
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      op = kThumb2Vsubd;
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      op = kThumb2Vdivd;
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      op = kThumb2Vmuld;
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
    case Instruction::NEG_DOUBLE: {
      return GenArithOpDoublePortable(cu, opcode, rl_dest, rl_src1, rl_src2);
    }
    default:
      return true;
  }

  rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
  DCHECK(rl_src1.wide);
  rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
  DCHECK(rl_src2.wide);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  DCHECK(rl_dest.wide);
  DCHECK(rl_result.wide);
  NewLIR3(cu, op, S2d(rl_result.low_reg, rl_result.high_reg), S2d(rl_src1.low_reg, rl_src1.high_reg),
          S2d(rl_src2.low_reg, rl_src2.high_reg));
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool GenConversion(CompilationUnit* cu, Instruction::Code opcode,
                   RegLocation rl_dest, RegLocation rl_src)
{
  int op = kThumbBkpt;
  int src_reg;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      op = kThumb2VcvtIF;
      break;
    case Instruction::FLOAT_TO_INT:
      op = kThumb2VcvtFI;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      op = kThumb2VcvtDF;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      op = kThumb2VcvtFd;
      break;
    case Instruction::INT_TO_DOUBLE:
      op = kThumb2VcvtID;
      break;
    case Instruction::DOUBLE_TO_INT:
      op = kThumb2VcvtDI;
      break;
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionPortable(cu, opcode, rl_dest, rl_src);
    default:
      return true;
  }
  if (rl_src.wide) {
    rl_src = LoadValueWide(cu, rl_src, kFPReg);
    src_reg = S2d(rl_src.low_reg, rl_src.high_reg);
  } else {
    rl_src = LoadValue(cu, rl_src, kFPReg);
    src_reg = rl_src.low_reg;
  }
  if (rl_dest.wide) {
    rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
    NewLIR2(cu, op, S2d(rl_result.low_reg, rl_result.high_reg), src_reg);
    StoreValueWide(cu, rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
    NewLIR2(cu, op, rl_result.low_reg, src_reg);
    StoreValue(cu, rl_dest, rl_result);
  }
  return false;
}

void GenFusedFPCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                         bool gt_bias, bool is_double)
{
  LIR* label_list = cu->block_label_list;
  LIR* target = &label_list[bb->taken->id];
  RegLocation rl_src1;
  RegLocation rl_src2;
  if (is_double) {
    rl_src1 = GetSrcWide(cu, mir, 0);
    rl_src2 = GetSrcWide(cu, mir, 2);
    rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
    rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
    NewLIR2(cu, kThumb2Vcmpd, S2d(rl_src1.low_reg, rl_src2.high_reg),
            S2d(rl_src2.low_reg, rl_src2.high_reg));
  } else {
    rl_src1 = GetSrc(cu, mir, 0);
    rl_src2 = GetSrc(cu, mir, 1);
    rl_src1 = LoadValue(cu, rl_src1, kFPReg);
    rl_src2 = LoadValue(cu, rl_src2, kFPReg);
    NewLIR2(cu, kThumb2Vcmps, rl_src1.low_reg, rl_src2.low_reg);
  }
  NewLIR0(cu, kThumb2Fmstat);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  switch(ccode) {
    case kCondEq:
    case kCondNe:
      break;
    case kCondLt:
      if (gt_bias) {
        ccode = kCondMi;
      }
      break;
    case kCondLe:
      if (gt_bias) {
        ccode = kCondLs;
      }
      break;
    case kCondGt:
      if (gt_bias) {
        ccode = kCondHi;
      }
      break;
    case kCondGe:
      if (gt_bias) {
        ccode = kCondCs;
      }
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(cu, ccode, target);
}


bool GenCmpFP(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest,
        RegLocation rl_src1, RegLocation rl_src2)
{
  bool is_double;
  int default_result;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::CMPL_FLOAT:
      is_double = false;
      default_result = -1;
      break;
    case Instruction::CMPG_FLOAT:
      is_double = false;
      default_result = 1;
      break;
    case Instruction::CMPL_DOUBLE:
      is_double = true;
      default_result = -1;
      break;
    case Instruction::CMPG_DOUBLE:
      is_double = true;
      default_result = 1;
      break;
    default:
      return true;
  }
  if (is_double) {
    rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
    rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
    ClobberSReg(cu, rl_dest.s_reg_low);
    rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
    LoadConstant(cu, rl_result.low_reg, default_result);
    NewLIR2(cu, kThumb2Vcmpd, S2d(rl_src1.low_reg, rl_src2.high_reg),
            S2d(rl_src2.low_reg, rl_src2.high_reg));
  } else {
    rl_src1 = LoadValue(cu, rl_src1, kFPReg);
    rl_src2 = LoadValue(cu, rl_src2, kFPReg);
    ClobberSReg(cu, rl_dest.s_reg_low);
    rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
    LoadConstant(cu, rl_result.low_reg, default_result);
    NewLIR2(cu, kThumb2Vcmps, rl_src1.low_reg, rl_src2.low_reg);
  }
  DCHECK(!ARM_FPREG(rl_result.low_reg));
  NewLIR0(cu, kThumb2Fmstat);

  OpIT(cu, (default_result == -1) ? kArmCondGt : kArmCondMi, "");
  NewLIR2(cu, kThumb2MovImmShift, rl_result.low_reg,
          ModifiedImmediate(-default_result)); // Must not alter ccodes
  GenBarrier(cu);

  OpIT(cu, kArmCondEq, "");
  LoadConstant(cu, rl_result.low_reg, 0);
  GenBarrier(cu);

  StoreValue(cu, rl_dest, rl_result);
  return false;
}

void GenNegFloat(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValue(cu, rl_src, kFPReg);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  NewLIR2(cu, kThumb2Vnegs, rl_result.low_reg, rl_src.low_reg);
  StoreValue(cu, rl_dest, rl_result);
}

void GenNegDouble(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValueWide(cu, rl_src, kFPReg);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  NewLIR2(cu, kThumb2Vnegd, S2d(rl_result.low_reg, rl_result.high_reg),
          S2d(rl_src.low_reg, rl_src.high_reg));
  StoreValueWide(cu, rl_dest, rl_result);
}

bool GenInlinedSqrt(CompilationUnit* cu, CallInfo* info) {
  DCHECK_EQ(cu->instruction_set, kThumb2);
  LIR *branch;
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(cu, info);  // double place for result
  rl_src = LoadValueWide(cu, rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  NewLIR2(cu, kThumb2Vsqrtd, S2d(rl_result.low_reg, rl_result.high_reg),
          S2d(rl_src.low_reg, rl_src.high_reg));
  NewLIR2(cu, kThumb2Vcmpd, S2d(rl_result.low_reg, rl_result.high_reg),
          S2d(rl_result.low_reg, rl_result.high_reg));
  NewLIR0(cu, kThumb2Fmstat);
  branch = NewLIR2(cu, kThumbBCond, 0, kArmCondEq);
  ClobberCalleeSave(cu);
  LockCallTemps(cu);  // Using fixed registers
  int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pSqrt));
  NewLIR3(cu, kThumb2Fmrrd, r0, r1, S2d(rl_src.low_reg, rl_src.high_reg));
  NewLIR1(cu, kThumbBlxR, r_tgt);
  NewLIR3(cu, kThumb2Fmdrr, S2d(rl_result.low_reg, rl_result.high_reg), r0, r1);
  branch->target = NewLIR0(cu, kPseudoTargetLabel);
  StoreValueWide(cu, rl_dest, rl_result);
  return true;
}


}  // namespace art
