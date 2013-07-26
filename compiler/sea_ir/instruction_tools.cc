/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "instruction_tools.h"

namespace sea_ir {

bool InstructionTools::IsDefinition(const art::Instruction* const instruction) {
  if (0 != (InstructionTools::instruction_attributes_[instruction->Opcode()] & (1 << kDA))) {
    return true;
  }
  return false;
}

const int InstructionTools::instruction_attributes_[] = {
  // 00 NOP
  DF_NOP,

  // 01 MOVE vA, vB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 02 MOVE_FROM16 vAA, vBBBB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 03 MOVE_16 vAAAA, vBBBB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 04 MOVE_WIDE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 05 MOVE_WIDE_FROM16 vAA, vBBBB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 06 MOVE_WIDE_16 vAAAA, vBBBB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 07 MOVE_OBJECT vA, vB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 08 MOVE_OBJECT_FROM16 vAA, vBBBB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 09 MOVE_OBJECT_16 vAAAA, vBBBB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 0A MOVE_RESULT vAA
  DF_DA,

  // 0B MOVE_RESULT_WIDE vAA
  DF_DA | DF_A_WIDE,

  // 0C MOVE_RESULT_OBJECT vAA
  DF_DA | DF_REF_A,

  // 0D MOVE_EXCEPTION vAA
  DF_DA | DF_REF_A | DF_NON_NULL_DST,

  // 0E RETURN_VOID
  DF_NOP,

  // 0F RETURN vAA
  DF_UA,

  // 10 RETURN_WIDE vAA
  DF_UA | DF_A_WIDE,

  // 11 RETURN_OBJECT vAA
  DF_UA | DF_REF_A,

  // 12 CONST_4 vA, #+B
  DF_DA | DF_SETS_CONST,

  // 13 CONST_16 vAA, #+BBBB
  DF_DA | DF_SETS_CONST,

  // 14 CONST vAA, #+BBBBBBBB
  DF_DA | DF_SETS_CONST,

  // 15 CONST_HIGH16 VAA, #+BBBB0000
  DF_DA | DF_SETS_CONST,

  // 16 CONST_WIDE_16 vAA, #+BBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 17 CONST_WIDE_32 vAA, #+BBBBBBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 18 CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 19 CONST_WIDE_HIGH16 vAA, #+BBBB000000000000
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 1A CONST_STRING vAA, string@BBBB
  DF_DA | DF_REF_A | DF_NON_NULL_DST,

  // 1B CONST_STRING_JUMBO vAA, string@BBBBBBBB
  DF_DA | DF_REF_A | DF_NON_NULL_DST,

  // 1C CONST_CLASS vAA, type@BBBB
  DF_DA | DF_REF_A | DF_NON_NULL_DST,

  // 1D MONITOR_ENTER vAA
  DF_UA | DF_NULL_CHK_0 | DF_REF_A,

  // 1E MONITOR_EXIT vAA
  DF_UA | DF_NULL_CHK_0 | DF_REF_A,

  // 1F CHK_CAST vAA, type@BBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 20 INSTANCE_OF vA, vB, type@CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_REF_B | DF_UMS,

  // 21 ARRAY_LENGTH vA, vB
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_A | DF_REF_B,

  // 22 NEW_INSTANCE vAA, type@BBBB
  DF_DA | DF_NON_NULL_DST | DF_REF_A | DF_UMS,

  // 23 NEW_ARRAY vA, vB, type@CCCC
  DF_DA | DF_UB | DF_NON_NULL_DST | DF_REF_A | DF_CORE_B | DF_UMS,

  // 24 FILLED_NEW_ARRAY {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NON_NULL_RET | DF_UMS,

  // 25 FILLED_NEW_ARRAY_RANGE {vCCCC .. vNNNN}, type@BBBB
  DF_FORMAT_3RC | DF_NON_NULL_RET | DF_UMS,

  // 26 FILL_ARRAY_DATA vAA, +BBBBBBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 27 THROW vAA
  DF_UA | DF_REF_A | DF_UMS,

  // 28 GOTO
  DF_NOP,

  // 29 GOTO_16
  DF_NOP,

  // 2A GOTO_32
  DF_NOP,

  // 2B PACKED_SWITCH vAA, +BBBBBBBB
  DF_UA,

  // 2C SPARSE_SWITCH vAA, +BBBBBBBB
  DF_UA,

  // 2D CMPL_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 2E CMPG_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 2F CMPL_DOUBLE vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 30 CMPG_DOUBLE vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 31 CMP_LONG vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 32 IF_EQ vA, vB, +CCCC
  DF_UA | DF_UB,

  // 33 IF_NE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 34 IF_LT vA, vB, +CCCC
  DF_UA | DF_UB,

  // 35 IF_GE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 36 IF_GT vA, vB, +CCCC
  DF_UA | DF_UB,

  // 37 IF_LE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 38 IF_EQZ vAA, +BBBB
  DF_UA,

  // 39 IF_NEZ vAA, +BBBB
  DF_UA,

  // 3A IF_LTZ vAA, +BBBB
  DF_UA,

  // 3B IF_GEZ vAA, +BBBB
  DF_UA,

  // 3C IF_GTZ vAA, +BBBB
  DF_UA,

  // 3D IF_LEZ vAA, +BBBB
  DF_UA,

  // 3E UNUSED_3E
  DF_NOP,

  // 3F UNUSED_3F
  DF_NOP,

  // 40 UNUSED_40
  DF_NOP,

  // 41 UNUSED_41
  DF_NOP,

  // 42 UNUSED_42
  DF_NOP,

  // 43 UNUSED_43
  DF_NOP,

  // 44 AGET vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 45 AGET_WIDE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 46 AGET_OBJECT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_A | DF_REF_B | DF_CORE_C,

  // 47 AGET_BOOLEAN vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 48 AGET_BYTE vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 49 AGET_CHAR vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 4A AGET_SHORT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 4B APUT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 4C APUT_WIDE vAA, vBB, vCC
  DF_UA | DF_A_WIDE | DF_UB | DF_UC | DF_NULL_CHK_2 | DF_RANGE_CHK_3 | DF_REF_B | DF_CORE_C,

  // 4D APUT_OBJECT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_A | DF_REF_B | DF_CORE_C,

  // 4E APUT_BOOLEAN vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 4F APUT_BYTE vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 50 APUT_CHAR vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 51 APUT_SHORT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 52 IGET vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 53 IGET_WIDE vA, vB, field@CCCC
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 54 IGET_OBJECT vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_A | DF_REF_B,

  // 55 IGET_BOOLEAN vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 56 IGET_BYTE vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 57 IGET_CHAR vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 58 IGET_SHORT vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 59 IPUT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5A IPUT_WIDE vA, vB, field@CCCC
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2 | DF_REF_B,

  // 5B IPUT_OBJECT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_A | DF_REF_B,

  // 5C IPUT_BOOLEAN vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5D IPUT_BYTE vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5E IPUT_CHAR vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5F IPUT_SHORT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 60 SGET vAA, field@BBBB
  DF_DA | DF_UMS,

  // 61 SGET_WIDE vAA, field@BBBB
  DF_DA | DF_A_WIDE | DF_UMS,

  // 62 SGET_OBJECT vAA, field@BBBB
  DF_DA | DF_REF_A | DF_UMS,

  // 63 SGET_BOOLEAN vAA, field@BBBB
  DF_DA | DF_UMS,

  // 64 SGET_BYTE vAA, field@BBBB
  DF_DA | DF_UMS,

  // 65 SGET_CHAR vAA, field@BBBB
  DF_DA | DF_UMS,

  // 66 SGET_SHORT vAA, field@BBBB
  DF_DA | DF_UMS,

  // 67 SPUT vAA, field@BBBB
  DF_UA | DF_UMS,

  // 68 SPUT_WIDE vAA, field@BBBB
  DF_UA | DF_A_WIDE | DF_UMS,

  // 69 SPUT_OBJECT vAA, field@BBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 6A SPUT_BOOLEAN vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6B SPUT_BYTE vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6C SPUT_CHAR vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6D SPUT_SHORT vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6E INVOKE_VIRTUAL {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 6F INVOKE_SUPER {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 70 INVOKE_DIRECT {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 71 INVOKE_STATIC {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_UMS,

  // 72 INVOKE_INTERFACE {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 73 UNUSED_73
  DF_NOP,

  // 74 INVOKE_VIRTUAL_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 75 INVOKE_SUPER_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 76 INVOKE_DIRECT_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 77 INVOKE_STATIC_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_UMS,

  // 78 INVOKE_INTERFACE_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 79 UNUSED_79
  DF_NOP,

  // 7A UNUSED_7A
  DF_NOP,

  // 7B NEG_INT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 7C NOT_INT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 7D NEG_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 7E NOT_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 7F NEG_FLOAT vA, vB
  DF_DA | DF_UB | DF_FP_A | DF_FP_B,

  // 80 NEG_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 81 INT_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

  // 82 INT_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_FP_A | DF_CORE_B,

  // 83 INT_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_A | DF_CORE_B,

  // 84 LONG_TO_INT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 85 LONG_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_A | DF_CORE_B,

  // 86 LONG_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_CORE_B,

  // 87 FLOAT_TO_INT vA, vB
  DF_DA | DF_UB | DF_FP_B | DF_CORE_A,

  // 88 FLOAT_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_B | DF_CORE_A,

  // 89 FLOAT_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_A | DF_FP_B,

  // 8A DOUBLE_TO_INT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_B | DF_CORE_A,

  // 8B DOUBLE_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_B | DF_CORE_A,

  // 8C DOUBLE_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 8D INT_TO_BYTE vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 8E INT_TO_CHAR vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 8F INT_TO_SHORT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 90 ADD_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 91 SUB_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 92 MUL_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 93 DIV_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 94 REM_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 95 AND_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 96 OR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 97 XOR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 98 SHL_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 99 SHR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9A USHR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9B ADD_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9C SUB_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9D MUL_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9E DIV_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9F REM_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A0 AND_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A1 OR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A2 XOR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A3 SHL_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A4 SHR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A5 USHR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A6 ADD_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A7 SUB_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A8 MUL_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A9 DIV_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // AA REM_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // AB ADD_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AC SUB_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AD MUL_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AE DIV_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AF REM_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // B0 ADD_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B1 SUB_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B2 MUL_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B3 DIV_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B4 REM_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B5 AND_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B6 OR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B7 XOR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B8 SHL_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B9 SHR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // BA USHR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // BB ADD_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BC SUB_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BD MUL_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BE DIV_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BF REM_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C0 AND_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C1 OR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C2 XOR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C3 SHL_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C4 SHR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C5 USHR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C6 ADD_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C7 SUB_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C8 MUL_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C9 DIV_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // CA REM_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // CB ADD_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CC SUB_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CD MUL_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CE DIV_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CF REM_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // D0 ADD_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D1 RSUB_INT vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D2 MUL_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D3 DIV_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D4 REM_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D5 AND_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D6 OR_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D7 XOR_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D8 ADD_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D9 RSUB_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DA MUL_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DB DIV_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DC REM_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DD AND_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DE OR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DF XOR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E0 SHL_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E1 SHR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E2 USHR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E3 IGET_VOLATILE
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // E4 IPUT_VOLATILE
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // E5 SGET_VOLATILE
  DF_DA | DF_UMS,

  // E6 SPUT_VOLATILE
  DF_UA | DF_UMS,

  // E7 IGET_OBJECT_VOLATILE
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_A | DF_REF_B,

  // E8 IGET_WIDE_VOLATILE
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // E9 IPUT_WIDE_VOLATILE
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2 | DF_REF_B,

  // EA SGET_WIDE_VOLATILE
  DF_DA | DF_A_WIDE | DF_UMS,

  // EB SPUT_WIDE_VOLATILE
  DF_UA | DF_A_WIDE | DF_UMS,

  // EC BREAKPOINT
  DF_NOP,

  // ED THROW_VERIFICATION_ERROR
  DF_NOP | DF_UMS,

  // EE EXECUTE_INLINE
  DF_FORMAT_35C,

  // EF EXECUTE_INLINE_RANGE
  DF_FORMAT_3RC,

  // F0 INVOKE_OBJECT_INIT_RANGE
  DF_NOP | DF_NULL_CHK_0,

  // F1 RETURN_VOID_BARRIER
  DF_NOP,

  // F2 IGET_QUICK
  DF_DA | DF_UB | DF_NULL_CHK_0,

  // F3 IGET_WIDE_QUICK
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0,

  // F4 IGET_OBJECT_QUICK
  DF_DA | DF_UB | DF_NULL_CHK_0,

  // F5 IPUT_QUICK
  DF_UA | DF_UB | DF_NULL_CHK_1,

  // F6 IPUT_WIDE_QUICK
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2,

  // F7 IPUT_OBJECT_QUICK
  DF_UA | DF_UB | DF_NULL_CHK_1,

  // F8 INVOKE_VIRTUAL_QUICK
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // F9 INVOKE_VIRTUAL_QUICK_RANGE
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // FA INVOKE_SUPER_QUICK
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // FB INVOKE_SUPER_QUICK_RANGE
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // FC IPUT_OBJECT_VOLATILE
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_A | DF_REF_B,

  // FD SGET_OBJECT_VOLATILE
  DF_DA | DF_REF_A | DF_UMS,

  // FE SPUT_OBJECT_VOLATILE
  DF_UA | DF_REF_A | DF_UMS,

  // FF UNUSED_FF
  DF_NOP
};
}  // namespace sea_ir
