// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONSTANTS_X86_H_
#define ART_SRC_CONSTANTS_X86_H_

#include "src/globals.h"
#include "src/logging.h"
#include "src/macros.h"

namespace art {

enum Register {
  EAX = 0,
  ECX = 1,
  EDX = 2,
  EBX = 3,
  ESP = 4,
  EBP = 5,
  ESI = 6,
  EDI = 7,
  kNumberOfCpuRegisters = 8,
  kFirstByteUnsafeRegister = 4,
  kNoRegister = -1  // Signals an illegal register.
};


enum ByteRegister {
  AL = 0,
  CL = 1,
  DL = 2,
  BL = 3,
  AH = 4,
  CH = 5,
  DH = 6,
  BH = 7,
  kNoByteRegister = -1  // Signals an illegal register.
};


enum XmmRegister {
  XMM0 = 0,
  XMM1 = 1,
  XMM2 = 2,
  XMM3 = 3,
  XMM4 = 4,
  XMM5 = 5,
  XMM6 = 6,
  XMM7 = 7,
  kNumberOfXmmRegisters = 8,
  kNoXmmRegister = -1  // Signals an illegal register.
};


enum ScaleFactor {
  TIMES_1 = 0,
  TIMES_2 = 1,
  TIMES_4 = 2,
  TIMES_8 = 3
};


enum Condition {
  OVERFLOW      =  0,
  NO_OVERFLOW   =  1,
  BELOW         =  2,
  ABOVE_EQUAL   =  3,
  EQUAL         =  4,
  NOT_EQUAL     =  5,
  BELOW_EQUAL   =  6,
  ABOVE         =  7,
  SIGN          =  8,
  NOT_SIGN      =  9,
  PARITY_EVEN   = 10,
  PARITY_ODD    = 11,
  LESS          = 12,
  GREATER_EQUAL = 13,
  LESS_EQUAL    = 14,
  GREATER       = 15,

  ZERO          = EQUAL,
  NOT_ZERO      = NOT_EQUAL,
  NEGATIVE      = SIGN,
  POSITIVE      = NOT_SIGN
};


class Instr {
 public:
  static const uint8_t kHltInstruction = 0xF4;
  // We prefer not to use the int3 instruction since it conflicts with gdb.
  static const uint8_t kBreakPointInstruction = kHltInstruction;
  static const int kBreakPointInstructionSize = 1;

  bool IsBreakPoint() {
    CHECK_EQ(kBreakPointInstructionSize, 1);
    return (*reinterpret_cast<const uint8_t*>(this)) == kBreakPointInstruction;
  }

  // Instructions are read out of a code stream. The only way to get a
  // reference to an instruction is to convert a pointer. There is no way
  // to allocate or create instances of class Instr.
  // Use the At(pc) function to create references to Instr.
  static Instr* At(uintptr_t pc) { return reinterpret_cast<Instr*>(pc); }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Instr);
};

}  // namespace art

#endif  // ART_SRC_CONSTANTS_X86_H_
