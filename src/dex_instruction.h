// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_H_
#define ART_SRC_DEX_INSTRUCTION_H_

#include "globals.h"
#include "logging.h"
#include "macros.h"

namespace art {

class Instruction {
 public:
  // NOP-encoded switch-statement signatures.
  enum {
    kPackedSwitchSignature = 0x0100,
    kSparseSwitchSignature = 0x0200,
    kArrayDataSignature = 0x0300
  };

  enum Code {
#define INSTRUCTION_ENUM(opcode, cname, p, f, r, i, a, v) cname = opcode,
#include "dex_instruction_list.h"
    DEX_INSTRUCTION_LIST(INSTRUCTION_ENUM)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_ENUM
  };

  enum InstructionFormat {
    k10x,  // op
    k12x,  // op vA, vB
    k11n,  // op vA, #+B
    k11x,  // op vAA
    k10t,  // op +AA
    k20t,  // op +AAAA
    k22x,  // op vAA, vBBBB
    k21t,  // op vAA, +BBBB
    k21s,  // op vAA, #+BBBB
    k21h,  // op vAA, #+BBBB00000[00000000]
    k21c,  // op vAA, thing@BBBB
    k23x,  // op vAA, vBB, vCC
    k22b,  // op vAA, vBB, #+CC
    k22t,  // op vA, vB, +CCCC
    k22s,  // op vA, vB, #+CCCC
    k22c,  // op vA, vB, thing@CCCC
    k32x,  // op vAAAA, vBBBB
    k30t,  // op +AAAAAAAA
    k31t,  // op vAA, +BBBBBBBB
    k31i,  // op vAA, #+BBBBBBBB
    k31c,  // op vAA, thing@BBBBBBBB
    k35c,  // op {vC, vD, vE, vF, vG}, thing@BBBB (B: count, A: vG)
    k3rc,  // op {vCCCC .. v(CCCC+AA-1)}, meth@BBBB
    k51l,  // op vAA, #+BBBBBBBBBBBBBBBB
  };

  enum Flags {
    kBranch   = 0x01,  // conditional or unconditional branch
    kContinue = 0x02,  // flow can continue to next statement
    kSwitch   = 0x04,  // switch statement
    kThrow    = 0x08,  // could cause an exception to be thrown
    kReturn   = 0x10,  // returns, no additional statements
    kInvoke   = 0x20,  // a flavor of invoke
    // TODO: kUnconditional
  };

  enum VerifyFlag {
    kVerifyNone            = 0x00000,
    kVerifyRegA            = 0x00001,
    kVerifyRegAWide        = 0x00002,
    kVerifyRegB            = 0x00004,
    kVerifyRegBField       = 0x00008,
    kVerifyRegBMethod      = 0x00010,
    kVerifyRegBNewInstance = 0x00020,
    kVerifyRegBString      = 0x00040,
    kVerifyRegBType        = 0x00080,
    kVerifyRegBWide        = 0x00100,
    kVerifyRegC            = 0x00200,
    kVerifyRegCField       = 0x00400,
    kVerifyRegCNewArray    = 0x00800,
    kVerifyRegCType        = 0x01000,
    kVerifyRegCWide        = 0x02000,
    kVerifyArrayData       = 0x04000,
    kVerifyBranchTarget    = 0x08000,
    kVerifySwitchTargets   = 0x10000,
    kVerifyVarArg          = 0x20000,
    kVerifyVarArgRange     = 0x40000,
    kVerifyError           = 0x80000,
  };

  /*
   * Holds the contents of a decoded instruction.
   */
  struct DecodedInstruction {
    uint32_t vA_;
    uint32_t vB_;
    uint64_t vB_wide_;        /* for kFmt51l */
    uint32_t vC_;
    uint32_t arg_[5];         /* vC/D/E/F/G in invoke or filled-new-array */
    Code     opcode_;

    DecodedInstruction(const Instruction* inst) {
      inst->Decode(vA_, vB_, vB_wide_, vC_, arg_);
      opcode_ = inst->Opcode();
    }
  };

  // Decodes this instruction, populating its arguments.
  void Decode(uint32_t &vA, uint32_t &vB, uint64_t &vB_wide, uint32_t &vC, uint32_t arg[]) const;

  // Returns the size in bytes of this instruction.
  size_t Size() const;

  // Returns a pointer to the next instruction in the stream.
  const Instruction* Next() const;

  // Name of the instruction.
  const char* Name() const {
    return kInstructionNames[Opcode()];
  }

  // Returns the opcode field of the instruction.
  Code Opcode() const;

  // Reads an instruction out of the stream at the specified address.
  static const Instruction* At(const byte* code) {
    CHECK(code != NULL);
    return reinterpret_cast<const Instruction*>(code);
  }

  // Returns the format of the current instruction.
  InstructionFormat Format() const {
    return kInstructionFormats[Opcode()];
  }

  // Returns the flags for the current instruction.
  int Flag() const {
    return kInstructionFlags[Opcode()];
  }

  // Returns true if this instruction is a branch.
  bool IsBranch() const {
    return (kInstructionFlags[Opcode()] & kBranch) != 0;
  }

  // Returns true if this instruction is a switch.
  bool IsSwitch() const {
    return (kInstructionFlags[Opcode()] & kSwitch) != 0;
  }

  // Returns true if this instruction can throw.
  bool IsThrow() const {
    return (kInstructionFlags[Opcode()] & kThrow) != 0;
  }

  // Determine if the instruction is any of 'return' instructions.
  bool IsReturn() const {
    return (kInstructionFlags[Opcode()] & kReturn) != 0;
  }

  // Determine if this instruction ends execution of its basic block.
  bool IsBasicBlockEnd() const {
    return IsBranch() || IsReturn() || Opcode() == THROW;
  }

  // Determine if this instruction is an invoke.
  bool IsInvoke() const {
    return (kInstructionFlags[Opcode()] & kInvoke) != 0;
  }

  int GetVerifyTypeArgumentA() const {
    return (kInstructionVerifyFlags[Opcode()] & (kVerifyRegA | kVerifyRegAWide));
  }

  int GetVerifyTypeArgumentB() const {
    return (kInstructionVerifyFlags[Opcode()] & (kVerifyRegB | kVerifyRegBField | kVerifyRegBMethod |
             kVerifyRegBNewInstance | kVerifyRegBString | kVerifyRegBType | kVerifyRegBWide));
  }

  int GetVerifyTypeArgumentC() const {
    return (kInstructionVerifyFlags[Opcode()] & (kVerifyRegC | kVerifyRegCField |
             kVerifyRegCNewArray | kVerifyRegBType | kVerifyRegBWide));
  }

  int GetVerifyExtraFlags() const {
    return (kInstructionVerifyFlags[Opcode()] & (kVerifyArrayData | kVerifyBranchTarget |
             kVerifySwitchTargets | kVerifyVarArg | kVerifyVarArgRange | kVerifyError));
  }

 private:
  static const char* const kInstructionNames[];
  static InstructionFormat const kInstructionFormats[];
  static int const kInstructionFlags[];
  static int const kInstructionVerifyFlags[];
  DISALLOW_IMPLICIT_CONSTRUCTORS(Instruction);
};

}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_H_
