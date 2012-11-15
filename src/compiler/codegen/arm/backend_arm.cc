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
#define _CODEGEN_C
#define _ARMV7_A

#include "../../dalvik.h"
#include "../../compiler_internals.h"
#include "arm_lir.h"
#include "../ralloc.h"
#include "codegen.h"

/* Common codegen utility code */
#include "../codegen_util.cc"

#include "utility_arm.cc"
#include "../codegen_factory.cc"
#include "../gen_common.cc"
#include "../gen_invoke.cc"
#include "call_arm.cc"
#include "fp_arm.cc"
#include "int_arm.cc"

/* Bitcode conversion */
#include "../method_bitcode.cc"

/* MIR2LIR dispatcher and architectural independent codegen routines */
#include "../method_codegen_driver.cc"

/* Target-independent local optimizations */
#include "../local_optimizations.cc"
