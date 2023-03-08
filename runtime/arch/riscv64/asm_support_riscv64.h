/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef ART_RUNTIME_ARCH_RISCV64_ASM_SUPPORT_RISCV64_H_
#define ART_RUNTIME_ARCH_RISCV64_ASM_SUPPORT_RISCV64_H_

#include "asm_support.h"
#include "entrypoints/entrypoint_asm_constants.h"

// FS0 - FS11, S0, S3 - S11, RA and ArtMethod*, total 8*(12 + 10 + 1 + 1) = 192
#define FRAME_SIZE_SAVE_ALL_CALLEE_SAVES 192
// FA0 - FA7, A1 - A7, S0, S3 - S11, RA, ArtMethod*, padding, total 8*(8 + 7 + 10 + 1 + 1 + 1) = 224
// Excluded GPRs are: A0 (ArtMethod*), S1/TR (ART thread register), S2 (shadow stack).
#define FRAME_SIZE_SAVE_REFS_AND_ARGS    224
// All 32 FPRs, 26 GPRs, ArtMethod*, padding, total 8*(32 + 26 + 1 + 1) = 480
// Excluded GPRs are: SP, Zero, TP, GP, S1/TR (ART thread register), S2 (shadow stack).
#define FRAME_SIZE_SAVE_EVERYTHING       480

#endif  // ART_RUNTIME_ARCH_RISCV64_ASM_SUPPORT_RISCV64_H_
