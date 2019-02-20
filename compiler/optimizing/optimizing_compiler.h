/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_H_

#include "base/globals.h"
#include "base/mutex.h"

namespace art {

class ArtMethod;
class Compiler;
class CompiledMethodStorage;
class CompilerOptions;
class DexFile;

Compiler* CreateOptimizingCompiler(const CompilerOptions& compiler_options,
                                   CompiledMethodStorage* storage);

bool EncodeArtMethodInInlineInfo(ArtMethod* method);

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_H_
