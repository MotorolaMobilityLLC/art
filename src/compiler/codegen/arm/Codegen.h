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
 * This file contains register alloction support and is intended to be
 * included by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#include "../../CompilerIR.h"

#if defined(_CODEGEN_C)
/*
 * loadConstant() sometimes needs to add a small imm to a pre-existing constant
 */
static ArmLIR *opRegImm(CompilationUnit* cUnit, OpKind op, int rDestSrc1,
                        int value);
static ArmLIR *opRegReg(CompilationUnit* cUnit, OpKind op, int rDestSrc1,
                        int rSrc2);

/* Forward decalraton the portable versions due to circular dependency */
static bool genArithOpFloatPortable(CompilationUnit* cUnit, MIR* mir,
                                    RegLocation rlDest, RegLocation rlSrc1,
                                    RegLocation rlSrc2);

static bool genArithOpDoublePortable(CompilationUnit* cUnit, MIR* mir,
                                     RegLocation rlDest, RegLocation rlSrc1,
                                     RegLocation rlSrc2);

static bool genConversionPortable(CompilationUnit* cUnit, MIR* mir);

#endif

extern void oatSetupResourceMasks(ArmLIR* lir);

extern ArmLIR* oatRegCopyNoInsert(CompilationUnit* cUnit, int rDest,
                                          int rSrc);
