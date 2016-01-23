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

.class public LIrreducibleLoop;

.super Ljava/lang/Object;

# Check that both the irreducible loop and the other loop entry
# move the constant-folded value to where it's expected.

## CHECK-START-X86: int IrreducibleLoop.simpleLoop(int, long) register (after)
## CHECK-DAG:                     ParallelMove {{.*84->.*}} loop:none
## CHECK-DAG:                     ParallelMove {{.*84->.*}} loop:{{B\d+}} irreducible:true
.method public static simpleLoop(IJ)I
   .registers 10
   const/16 v6, 2
   const/16 v4, 1
   const-wide/16 v0, 42
   add-long v2, v0, v0

   if-eqz p0, :loop_entry
   goto :other_loop_pre_entry

   # The then part: beginning of the irreducible loop.
   :loop_entry
   if-eqz p0, :exit
   cmp-long v6, v2, p1
   :other_loop_entry
   sub-int p0, p0, v4
   goto :loop_entry

   # The other block branching to the irreducible loop.
   # In that block, v4 has no live range.
   :other_loop_pre_entry
   goto :other_loop_entry

   :exit
   return v6
.end method
