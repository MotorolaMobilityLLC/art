#!/bin/sh
#
# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# Rebuild for all known targets.  Necessary until the stuff in "out" gets
# generated as part of the build.
#
set -e

# for arch in arm x86 mips arm64 x86_64 mips64; do TARGET_ARCH_EXT=$arch make -f Makefile_mterp; done
for arch in arm x86 mips arm64 x86_64 ; do TARGET_ARCH_EXT=$arch make -f Makefile_mterp; done
