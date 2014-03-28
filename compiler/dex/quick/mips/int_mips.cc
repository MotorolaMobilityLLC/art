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

/* This file contains codegen for the Mips ISA */

#include "codegen_mips.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mips_lir.h"
#include "mirror/array.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 *    slt   t0,  x.hi, y.hi;        # (x.hi < y.hi) ? 1:0
 *    sgt   t1,  x.hi, y.hi;        # (y.hi > x.hi) ? 1:0
 *    subu  res, t0, t1             # res = -1:1:0 for [ < > = ]
 *    bnez  res, finish
 *    sltu  t0, x.lo, y.lo
 *    sgtu  r1, x.lo, y.lo
 *    subu  res, t0, t1
 * finish:
 *
 */
void MipsMir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  int t0 = AllocTemp().GetReg();
  int t1 = AllocTemp().GetReg();
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR3(kMipsSlt, t0, rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
  NewLIR3(kMipsSlt, t1, rl_src2.reg.GetHighReg(), rl_src1.reg.GetHighReg());
  NewLIR3(kMipsSubu, rl_result.reg.GetReg(), t1, t0);
  LIR* branch = OpCmpImmBranch(kCondNe, rl_result.reg, 0, NULL);
  NewLIR3(kMipsSltu, t0, rl_src1.reg.GetLowReg(), rl_src2.reg.GetLowReg());
  NewLIR3(kMipsSltu, t1, rl_src2.reg.GetLowReg(), rl_src1.reg.GetLowReg());
  NewLIR3(kMipsSubu, rl_result.reg.GetReg(), t1, t0);
  FreeTemp(t0);
  FreeTemp(t1);
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch->target = target;
  StoreValue(rl_dest, rl_result);
}

LIR* MipsMir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  LIR* branch;
  MipsOpCode slt_op;
  MipsOpCode br_op;
  bool cmp_zero = false;
  bool swapped = false;
  switch (cond) {
    case kCondEq:
      br_op = kMipsBeq;
      cmp_zero = true;
      break;
    case kCondNe:
      br_op = kMipsBne;
      cmp_zero = true;
      break;
    case kCondUlt:
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      break;
    case kCondUge:
      slt_op = kMipsSltu;
      br_op = kMipsBeqz;
      break;
    case kCondGe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      break;
    case kCondGt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      swapped = true;
      break;
    case kCondLe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      swapped = true;
      break;
    case kCondLt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      break;
    case kCondHi:  // Gtu
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      swapped = true;
      break;
    default:
      LOG(FATAL) << "No support for ConditionCode: " << cond;
      return NULL;
  }
  if (cmp_zero) {
    branch = NewLIR2(br_op, src1.GetReg(), src2.GetReg());
  } else {
    int t_reg = AllocTemp().GetReg();
    if (swapped) {
      NewLIR3(slt_op, t_reg, src2.GetReg(), src1.GetReg());
    } else {
      NewLIR3(slt_op, t_reg, src1.GetReg(), src2.GetReg());
    }
    branch = NewLIR1(br_op, t_reg);
    FreeTemp(t_reg);
  }
  branch->target = target;
  return branch;
}

LIR* MipsMir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target) {
  LIR* branch;
  if (check_value != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti
    RegStorage t_reg = AllocTemp();
    LoadConstant(t_reg, check_value);
    branch = OpCmpBranch(cond, reg, t_reg, target);
    FreeTemp(t_reg);
    return branch;
  }
  MipsOpCode opc;
  switch (cond) {
    case kCondEq: opc = kMipsBeqz; break;
    case kCondGe: opc = kMipsBgez; break;
    case kCondGt: opc = kMipsBgtz; break;
    case kCondLe: opc = kMipsBlez; break;
    // case KCondMi:
    case kCondLt: opc = kMipsBltz; break;
    case kCondNe: opc = kMipsBnez; break;
    default:
      // Tuning: use slti when applicable
      RegStorage t_reg = AllocTemp();
      LoadConstant(t_reg, check_value);
      branch = OpCmpBranch(cond, reg, t_reg, target);
      FreeTemp(t_reg);
      return branch;
  }
  branch = NewLIR1(opc, reg.GetReg());
  branch->target = target;
  return branch;
}

LIR* MipsMir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  // If src or dest is a pair, we'll be using low reg.
  if (r_dest.IsPair()) {
    r_dest = r_dest.GetLow();
  }
  if (r_src.IsPair()) {
    r_src = r_src.GetLow();
  }
  if (MIPS_FPREG(r_dest.GetReg()) || MIPS_FPREG(r_src.GetReg()))
    return OpFpRegCopy(r_dest, r_src);
  LIR* res = RawLIR(current_dalvik_offset_, kMipsMove,
            r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* MipsMir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  LIR *res = OpRegCopyNoInsert(r_dest, r_src);
  AppendLIR(res);
  return res;
}

void MipsMir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  bool dest_fp = MIPS_FPREG(r_dest.GetLowReg());
  bool src_fp = MIPS_FPREG(r_src.GetLowReg());
  if (dest_fp) {
    if (src_fp) {
      // FIXME: handle this here - reserve OpRegCopy for 32-bit copies.
      OpRegCopy(RegStorage::Solo64(S2d(r_dest.GetLowReg(), r_dest.GetHighReg())),
                RegStorage::Solo64(S2d(r_src.GetLowReg(), r_src.GetHighReg())));
    } else {
       /* note the operands are swapped for the mtc1 instr */
      NewLIR2(kMipsMtc1, r_src.GetLowReg(), r_dest.GetLowReg());
      NewLIR2(kMipsMtc1, r_src.GetHighReg(), r_dest.GetHighReg());
    }
  } else {
    if (src_fp) {
      NewLIR2(kMipsMfc1, r_dest.GetLowReg(), r_src.GetLowReg());
      NewLIR2(kMipsMfc1, r_dest.GetHighReg(), r_src.GetHighReg());
    } else {
      // Handle overlap
      if (r_src.GetHighReg() == r_dest.GetLowReg()) {
        OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
        OpRegCopy(r_dest.GetLow(), r_src.GetLow());
      } else {
        OpRegCopy(r_dest.GetLow(), r_src.GetLow());
        OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
      }
    }
  }
}

void MipsMir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  UNIMPLEMENTED(FATAL) << "Need codegen for select";
}

void MipsMir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

LIR* MipsMir2Lir::GenRegMemCheck(ConditionCode c_code, RegStorage reg1, RegStorage base,
                                 int offset, ThrowKind kind) {
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation MipsMir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg1, RegStorage reg2,
                                    bool is_div) {
  NewLIR2(kMipsDiv, reg1.GetReg(), reg2.GetReg());
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    NewLIR1(kMipsMflo, rl_result.reg.GetReg());
  } else {
    NewLIR1(kMipsMfhi, rl_result.reg.GetReg());
  }
  return rl_result;
}

RegLocation MipsMir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg1, int lit,
                                       bool is_div) {
  int t_reg = AllocTemp().GetReg();
  NewLIR3(kMipsAddiu, t_reg, rZERO, lit);
  NewLIR2(kMipsDiv, reg1.GetReg(), t_reg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    NewLIR1(kMipsMflo, rl_result.reg.GetReg());
  } else {
    NewLIR1(kMipsMfhi, rl_result.reg.GetReg());
  }
  FreeTemp(t_reg);
  return rl_result;
}

RegLocation MipsMir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation rl_src2, bool is_div, bool check_zero) {
  LOG(FATAL) << "Unexpected use of GenDivRem for Mips";
  return rl_dest;
}

RegLocation MipsMir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Mips";
  return rl_dest;
}

void MipsMir2Lir::OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale,
                        int offset) {
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void MipsMir2Lir::OpTlsCmp(ThreadOffset offset, int val) {
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

bool MipsMir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  DCHECK_NE(cu_->instruction_set, kThumb2);
  return false;
}

bool MipsMir2Lir::GenInlinedSqrt(CallInfo* info) {
  DCHECK_NE(cu_->instruction_set, kThumb2);
  return false;
}

bool MipsMir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[1]
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  DCHECK(size == kSignedByte);
  LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size, INVALID_SREG);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool MipsMir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[1]
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  DCHECK(size == kSignedByte);
  RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
  StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size);
  return true;
}

LIR* MipsMir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for Mips";
  return NULL;
}

LIR* MipsMir2Lir::OpVldm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVldm for Mips";
  return NULL;
}

LIR* MipsMir2Lir::OpVstm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVstm for Mips";
  return NULL;
}

void MipsMir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                                RegLocation rl_result, int lit,
                                                int first_bit, int second_bit) {
  RegStorage t_reg = AllocTemp();
  OpRegRegImm(kOpLsl, t_reg, rl_src.reg, second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void MipsMir2Lir::GenDivZeroCheck(RegStorage reg) {
  DCHECK(reg.IsPair());   // TODO: support k64BitSolo.
  RegStorage t_reg = AllocTemp();
  OpRegRegReg(kOpOr, t_reg, reg.GetLow(), reg.GetHigh());
  GenImmedCheck(kCondEq, t_reg, 0, kThrowDivZero);
  FreeTemp(t_reg);
}

// Test suspend flag, return target of taken suspend branch
LIR* MipsMir2Lir::OpTestSuspend(LIR* target) {
  OpRegImm(kOpSub, rs_rMIPS_SUSPEND, 1);
  return OpCmpImmBranch((target == NULL) ? kCondEq : kCondNe, rs_rMIPS_SUSPEND, 0, target);
}

// Decrement register and branch on condition
LIR* MipsMir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  OpRegImm(kOpSub, reg, 1);
  return OpCmpImmBranch(c_code, reg, 0, target);
}

bool MipsMir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                     RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips";
  return false;
}

LIR* MipsMir2Lir::OpIT(ConditionCode cond, const char* guide) {
  LOG(FATAL) << "Unexpected use of OpIT in Mips";
  return NULL;
}

void MipsMir2Lir::GenMulLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenMulLong for Mips";
}

void MipsMir2Lir::GenAddLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] + [a3 a2];
   *  addu v0,a2,a0
   *  addu t1,a3,a1
   *  sltu v1,v0,a2
   *  addu v1,v1,t1
   */

  OpRegRegReg(kOpAdd, rl_result.reg.GetLow(), rl_src2.reg.GetLow(), rl_src1.reg.GetLow());
  RegStorage t_reg = AllocTemp();
  OpRegRegReg(kOpAdd, t_reg, rl_src2.reg.GetHigh(), rl_src1.reg.GetHigh());
  NewLIR3(kMipsSltu, rl_result.reg.GetHighReg(), rl_result.reg.GetLowReg(), rl_src2.reg.GetLowReg());
  OpRegRegReg(kOpAdd, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
  FreeTemp(t_reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenSubLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] - [a3 a2];
   *  sltu  t1,a0,a2
   *  subu  v0,a0,a2
   *  subu  v1,a1,a3
   *  subu  v1,v1,t1
   */

  RegStorage t_reg = AllocTemp();
  NewLIR3(kMipsSltu, t_reg.GetReg(), rl_src1.reg.GetLowReg(), rl_src2.reg.GetLowReg());
  OpRegRegReg(kOpSub, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
  OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
  FreeTemp(t_reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  -[a1 a0]
   *  negu  v0,a0
   *  negu  v1,a1
   *  sltu  t1,r_zero
   *  subu  v1,v1,t1
   */

  OpRegReg(kOpNeg, rl_result.reg.GetLow(), rl_src.reg.GetLow());
  OpRegReg(kOpNeg, rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
  RegStorage t_reg = AllocTemp();
  NewLIR3(kMipsSltu, t_reg.GetReg(), rZERO, rl_result.reg.GetLowReg());
  OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
  FreeTemp(t_reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenAndLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1,
                             RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenAndLong for Mips";
}

void MipsMir2Lir::GenOrLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenOrLong for Mips";
}

void MipsMir2Lir::GenXorLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenXorLong for Mips";
}

/*
 * Generate array load
 */
void MipsMir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                          RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  rl_array = LoadValue(rl_array, kCoreReg);
  rl_index = LoadValue(rl_index, kCoreReg);

  if (size == kLong || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  RegStorage reg_ptr = AllocTemp();
  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    /* Get len */
    LoadWordDisp(rl_array.reg, len_offset, reg_len);
  }
  /* reg_ptr -> array data */
  OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
  FreeTemp(rl_array.reg.GetReg());
  if ((size == kLong) || (size == kDouble)) {
    if (scale) {
      RegStorage r_new_index = AllocTemp();
      OpRegRegImm(kOpLsl, r_new_index, rl_index.reg, scale);
      OpRegReg(kOpAdd, reg_ptr, r_new_index);
      FreeTemp(r_new_index);
    } else {
      OpRegReg(kOpAdd, reg_ptr, rl_index.reg);
    }
    FreeTemp(rl_index.reg);
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg, reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }
    LoadBaseDispWide(reg_ptr, 0, rl_result.reg, INVALID_SREG);

    FreeTemp(reg_ptr);
    StoreValueWide(rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg, reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }
    LoadBaseIndexed(reg_ptr, rl_index.reg, rl_result.reg, scale, size);

    FreeTemp(reg_ptr);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void MipsMir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                          RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == kLong || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(rl_array, kCoreReg);
  rl_index = LoadValue(rl_index, kCoreReg);
  RegStorage reg_ptr;
  bool allocated_reg_ptr_temp = false;
  if (IsTemp(rl_array.reg.GetReg()) && !card_mark) {
    Clobber(rl_array.reg.GetReg());
    reg_ptr = rl_array.reg;
  } else {
    reg_ptr = AllocTemp();
    OpRegCopy(reg_ptr, rl_array.reg);
    allocated_reg_ptr_temp = true;
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // NOTE: max live temps(4) here.
    /* Get len */
    LoadWordDisp(rl_array.reg, len_offset, reg_len);
  }
  /* reg_ptr -> array data */
  OpRegImm(kOpAdd, reg_ptr, data_offset);
  /* at this point, reg_ptr points to array, 2 live temps */
  if ((size == kLong) || (size == kDouble)) {
    // TUNING: specific wide routine that can handle fp regs
    if (scale) {
      RegStorage r_new_index = AllocTemp();
      OpRegRegImm(kOpLsl, r_new_index, rl_index.reg, scale);
      OpRegReg(kOpAdd, reg_ptr, r_new_index);
      FreeTemp(r_new_index);
    } else {
      OpRegReg(kOpAdd, reg_ptr, rl_index.reg);
    }
    rl_src = LoadValueWide(rl_src, reg_class);

    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg, reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }

    StoreBaseDispWide(reg_ptr, 0, rl_src.reg);
  } else {
    rl_src = LoadValue(rl_src, reg_class);
    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg, reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }
    StoreBaseIndexed(reg_ptr, rl_index.reg, rl_src.reg, scale, size);
  }
  if (allocated_reg_ptr_temp) {
    FreeTemp(reg_ptr);
  }
  if (card_mark) {
    MarkGCCard(rl_src.reg, rl_array.reg);
  }
}

void MipsMir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                    RegLocation rl_src1, RegLocation rl_shift) {
  // Default implementation is just to ignore the constant case.
  GenShiftOpLong(opcode, rl_dest, rl_src1, rl_shift);
}

void MipsMir2Lir::GenArithImmOpLong(Instruction::Code opcode,
                                    RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  // Default - bail to non-const handler.
  GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
}

}  // namespace art
