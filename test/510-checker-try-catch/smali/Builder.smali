# Copyright (C) 2015 The Android Open Source Project
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

.class public LBuilder;

.super Ljava/lang/Object;

# Basic test case with two try blocks and three catch handlers, one of which
# is shared by the two tries.

## CHECK-START: int Builder.testMultipleTryCatch(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnterTry1:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1
## CHECK:  <<Minus2:i\d+>>  IntConstant -2
## CHECK:  <<Minus3:i\d+>>  IntConstant -3

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry1>>"
## CHECK:  successors       "<<BExitTry1:B\d+>>"
## CHECK:  DivZeroCheck

## CHECK:  name             "<<BAdd:B\d+>>"
## CHECK:  predecessors     "<<BExitTry1>>"
## CHECK:  successors       "<<BEnterTry2:B\d+>>"
## CHECK:  Add

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry2>>"
## CHECK:  successors       "<<BExitTry2:B\d+>>"
## CHECK:  DivZeroCheck

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry2>>" "<<BCatch1:B\d+>>" "<<BCatch2:B\d+>>" "<<BCatch3:B\d+>>"
## CHECK:  Return

## CHECK:  name             "<<BCatch1>>"
## CHECK:  predecessors     "<<BEnterTry1>>" "<<BExitTry1>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BCatch2>>"
## CHECK:  predecessors     "<<BEnterTry2>>" "<<BExitTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus2>>]

## CHECK:  name             "<<BCatch3>>"
## CHECK:  predecessors     "<<BEnterTry1>>" "<<BExitTry1>>" "<<BEnterTry2>>" "<<BExitTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus3>>]

## CHECK:  name             "<<BEnterTry1>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatch1>>" "<<BCatch3>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry1>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BAdd>>"
## CHECK:  xhandlers        "<<BCatch1>>" "<<BCatch3>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

## CHECK:  name             "<<BEnterTry2>>"
## CHECK:  predecessors     "<<BAdd>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatch2>>" "<<BCatch3>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry2>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch2>>" "<<BCatch3>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testMultipleTryCatch(III)I
    .registers 3

    :try_start_1
    div-int/2addr p0, p1
    :try_end_1
    .catch Ljava/lang/ArithmeticException; {:try_start_1 .. :try_end_1} :catch_arith
    .catchall {:try_start_1 .. :try_end_1} :catch_other

    add-int/2addr p0, p0

    :try_start_2
    div-int/2addr p0, p2
    :try_end_2
    .catch Ljava/lang/OutOfMemoryError; {:try_start_2 .. :try_end_2} :catch_mem
    .catchall {:try_start_2 .. :try_end_2} :catch_other

    :return
    return p0

    :catch_arith
    const/4 p0, -0x1
    goto :return

    :catch_mem
    const/4 p0, -0x2
    goto :return

    :catch_other
    const/4 p0, -0x3
    goto :return
.end method

# Test that multiple try-entry blocks are generated if there are multiple entry
# points into the try block.

## CHECK-START: int Builder.testMultipleEntries(int, int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BIf:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1

## CHECK:  name             "<<BIf>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BEnterTry2:B\d+>>" "<<BThen:B\d+>>"
## CHECK:  If

## CHECK:  name             "<<BThen>>"
## CHECK:  predecessors     "<<BIf>>"
## CHECK:  successors       "<<BEnterTry1:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry1>>"
## CHECK:  successors       "<<BTry2:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry2>>"
## CHECK:  predecessors     "<<BEnterTry2>>" "<<BTry1>>"
## CHECK:  successors       "<<BExitTry:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry>>" "<<BCatch:B\d+>>"
## CHECK:  Return

## CHECK:  name             "<<BCatch>>"
## CHECK:  predecessors     "<<BEnterTry1>>" "<<BEnterTry2>>" "<<BExitTry>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BEnterTry1>>"
## CHECK:  predecessors     "<<BThen>>"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BEnterTry2>>"
## CHECK:  predecessors     "<<BIf>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testMultipleEntries(IIII)I
    .registers 4

    if-eqz p2, :else

    div-int/2addr p0, p1

    :try_start
    div-int/2addr p0, p2

    :else
    div-int/2addr p0, p3
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return

.end method

# Test that multiple try-exit blocks are generated if (normal) control flow can
# jump out of the try block at multiple points.

## CHECK-START: int Builder.testMultipleExits(int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnterTry:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1
## CHECK:  <<Minus2:i\d+>>  IntConstant -2

## CHECK:  name             "<<BTry:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry>>"
## CHECK:  successors       "<<BExitTry1:B\d+>>" "<<BExitTry2:B\d+>>"
## CHECK:  Div
## CHECK:  If

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry2>>" "<<BThen:B\d+>>" "<<BCatch:B\d+>>"
## CHECK:  Return

## CHECK:  name             "<<BThen>>"
## CHECK:  predecessors     "<<BExitTry1>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BCatch>>"
## CHECK:  predecessors     "<<BEnterTry>>" "<<BExitTry1>>" "<<BExitTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus2>>]

## CHECK:  name             "<<BEnterTry>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry1>>"
## CHECK:  predecessors     "<<BTry>>"
## CHECK:  successors       "<<BThen>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

## CHECK:  name             "<<BExitTry2>>"
## CHECK:  predecessors     "<<BTry>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testMultipleExits(II)I
    .registers 2

    :try_start
    div-int/2addr p0, p1
    if-eqz p0, :then
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0

    :then
    const/4 p0, -0x1
    goto :return

    :catch_all
    const/4 p0, -0x2
    goto :return
.end method

# Test that only one TryBoundary is inserted when an edge connects two different
# try ranges.

## CHECK-START: int Builder.testSharedBoundary(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnter1:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1
## CHECK:  <<Minus2:i\d+>>  IntConstant -2

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnter1>>"
## CHECK:  successors       "<<BExit1Enter2:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BExit1Enter2>>"
## CHECK:  successors       "<<BExit2:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExit2>>" "<<BCatch1:B\d+>>" "<<BCatch2:B\d+>>"
## CHECK:  Return

## CHECK:  name             "<<BCatch1>>"
## CHECK:  predecessors     "<<BEnter1>>" "<<BExit1Enter2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BCatch2>>"
## CHECK:  predecessors     "<<BExit1Enter2>>" "<<BExit2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus2>>]

## CHECK:  name             "<<BEnter1>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatch1>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExit1Enter2>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatch1>>" "<<BCatch2>>"
## CHECK:  TryBoundary      is_entry:true is_exit:true

## CHECK:  name             "<<BExit2>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch2>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testSharedBoundary(III)I
    .registers 3

    :try_start_1
    div-int/2addr p0, p1
    :try_end_1
    .catchall {:try_start_1 .. :try_end_1} :catch_all_1

    :try_start_2
    div-int/2addr p0, p2
    :try_end_2
    .catchall {:try_start_2 .. :try_end_2} :catch_all_2

    :return
    return p0

    :catch_all_1
    const/4 p0, -0x1
    goto :return

    :catch_all_2
    const/4 p0, -0x2
    goto :return
.end method

# Same as previous test, only the blocks are processed in the opposite order.

## CHECK-START: int Builder.testSharedBoundary_Reverse(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BGoto:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1
## CHECK:  <<Minus2:i\d+>>  IntConstant -2

## CHECK:  name             "<<BGoto>>"
## CHECK:  successors       "<<BEnter2:B\d+>>"
## CHECK:  Goto

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BExit2Enter1:B\d+>>"
## CHECK:  successors       "<<BExit1:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BEnter2>>"
## CHECK:  successors       "<<BExit2Enter1>>"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExit1>>" "<<BCatch1:B\d+>>" "<<BCatch2:B\d+>>"
## CHECK:  Return

## CHECK:  name             "<<BCatch1>>"
## CHECK:  predecessors     "<<BExit2Enter1>>" "<<BExit1>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BCatch2>>"
## CHECK:  predecessors     "<<BEnter2>>" "<<BExit2Enter1>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus2>>]

## CHECK:  name             "<<BExit2Enter1>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatch1>>" "<<BCatch2>>"
## CHECK:  TryBoundary      is_entry:true is_exit:true

## CHECK:  name             "<<BExit1>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch1>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

## CHECK:  name             "<<BEnter2>>"
## CHECK:  predecessors     "<<BGoto>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatch2>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

.method public static testSharedBoundary_Reverse(III)I
    .registers 3

    goto :try_start_2

    :try_start_1
    div-int/2addr p0, p1
    goto :return
    :try_end_1
    .catchall {:try_start_1 .. :try_end_1} :catch_all_1

    :try_start_2
    div-int/2addr p0, p2
    goto :try_start_1
    :try_end_2
    .catchall {:try_start_2 .. :try_end_2} :catch_all_2

    :return
    return p0

    :catch_all_1
    const/4 p0, -0x1
    goto :return

    :catch_all_2
    const/4 p0, -0x2
    goto :return
.end method

# Test that nested tries are split into non-overlapping blocks and TryBoundary
# blocks are correctly created between them.

## CHECK-START: int Builder.testNestedTry(int, int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1
## CHECK:  <<Minus2:i\d+>>  IntConstant -2

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnter1:B\d+>>"
## CHECK:  successors       "<<BExit1Enter2:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BExit1Enter2>>"
## CHECK:  successors       "<<BExit2Enter3:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry3:B\d+>>"
## CHECK:  predecessors     "<<BExit2Enter3>>"
## CHECK:  successors       "<<BExit3:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExit3>>" "<<BCatchArith:B\d+>>" "<<BCatchAll:B\d+>>"

## CHECK:  name             "<<BCatchArith>>"
## CHECK:  predecessors     "<<BExit1Enter2>>" "<<BExit2Enter3>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BCatchAll>>"
## CHECK:  predecessors     "<<BEnter1>>" "<<BExit1Enter2>>" "<<BExit2Enter3>>" "<<BExit3>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus2>>]

## CHECK:  name             "<<BEnter1>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatchAll>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExit1Enter2>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatchAll>>" "<<BCatchArith>>"
## CHECK:  TryBoundary      is_entry:true is_exit:true

## CHECK:  name             "<<BExit2Enter3>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BTry3>>"
## CHECK:  xhandlers        "<<BCatchArith>>" "<<BCatchAll>>"
## CHECK:  TryBoundary      is_entry:true is_exit:true

## CHECK:  name             "<<BExit3>>"
## CHECK:  predecessors     "<<BTry3>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatchAll>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testNestedTry(IIII)I
    .registers 4

    :try_start_1
    div-int/2addr p0, p1

    :try_start_2
    div-int/2addr p0, p2
    :try_end_2
    .catch Ljava/lang/ArithmeticException; {:try_start_2 .. :try_end_2} :catch_arith

    div-int/2addr p0, p3
    :try_end_1
    .catchall {:try_start_1 .. :try_end_1} :catch_all

    :return
    return p0

    :catch_arith
    const/4 p0, -0x1
    goto :return

    :catch_all
    const/4 p0, -0x2
    goto :return
.end method

# Test control flow that enters a try block, leaves it and returns again.

## CHECK-START: int Builder.testIncontinuousTry(int, int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry1:B\d+>>"
## CHECK:  successors       "<<BExitTry1:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry2:B\d+>>"
## CHECK:  successors       "<<BExitTry2:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry2>>" "<<BCatch:B\d+>>"

## CHECK:  name             "<<BOutside:B\d+>>"
## CHECK:  predecessors     "<<BExitTry1>>"
## CHECK:  successors       "<<BEnterTry2>>"
## CHECK:  Div

## CHECK:  name             "<<BCatch>>"
## CHECK:  predecessors     "<<BEnterTry1>>" "<<BExitTry1>>" "<<BEnterTry2>>" "<<BExitTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BEnterTry1>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry1>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BOutside>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

## CHECK:  name             "<<BEnterTry2>>"
## CHECK:  predecessors     "<<BOutside>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry2>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testIncontinuousTry(IIII)I
    .registers 4

    :try_start
    div-int/2addr p0, p1
    goto :outside

    :inside
    div-int/2addr p0, p3
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0

    :outside
    div-int/2addr p0, p2
    goto :inside

    :catch_all
    const/4 p0, -0x1
    goto :return
.end method

# Test that a TryBoundary is inserted between a Throw instruction and the exit
# block when covered by a try range.

## CHECK-START: int Builder.testThrow(java.lang.Exception) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnterTry:B\d+>>"
## CHECK:  <<Minus1:i\d+>>  IntConstant -1

## CHECK:  name             "<<BTry:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry>>"
## CHECK:  successors       "<<BExitTry:B\d+>>"
## CHECK:  Throw

## CHECK:  name             "<<BCatch:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry>>" "<<BExitTry>>"
## CHECK:  successors       "<<BExit:B\d+>>"
## CHECK:  flags            "catch_block"
## CHECK:  StoreLocal       [v0,<<Minus1>>]

## CHECK:  name             "<<BEnterTry>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry>>"
## CHECK:  predecessors     "<<BTry>>"
## CHECK:  successors       "<<BExit>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

## CHECK:  name             "<<BExit>>"
## CHECK:  predecessors     "<<BExitTry>>" "<<BCatch>>"
## CHECK:  Exit

.method public static testThrow(Ljava/lang/Exception;)I
    .registers 2

    :try_start
    throw p0
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :catch_all
    const/4 v0, -0x1
    return v0
.end method

# Test graph with a throw/catch loop.

## CHECK-START: int Builder.testCatchLoop(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnterTry:B\d+>>"

## CHECK:  name             "<<BTry:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry>>" "<<BEnterTry>>" "<<BExitTry:B\d+>>"
## CHECK:  successors       "<<BExitTry>>"
## CHECK:  flags            "catch_block"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry>>"

## CHECK:  name             "<<BEnterTry>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry>>"
## CHECK:  xhandlers        "<<BTry>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry>>"
## CHECK:  predecessors     "<<BTry>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BTry>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testCatchLoop(III)I
    .registers 4

    :try_start
    :catch_all
    div-int/2addr p0, p2
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0
.end method

# Test that handler edges are not split. In this scenario, the catch block is
# only the handler of the try block.

## CHECK-START: int Builder.testHandlerEdge1(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnterTry:B\d+>>"

## CHECK:  name             "<<BTry:B\d+>>"
## CHECK:  predecessors     "<<BEnterTry>>"
## CHECK:  successors       "<<BCatch:B\d+>>"
## CHECK:  Div

## CHECK:  name             "<<BCatch>>"
## CHECK:  predecessors     "<<BTry>>" "<<BEnterTry>>" "<<BExitTry:B\d+>>"
## CHECK:  successors       "<<BExitTry>>"
## CHECK:  flags            "catch_block"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExitTry>>"

## CHECK:  name             "<<BEnterTry>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExitTry>>"
## CHECK:  predecessors     "<<BCatch>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BCatch>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testHandlerEdge1(III)I
    .registers 4

    :try_start
    div-int/2addr p0, p1

    :catch_all
    div-int/2addr p0, p2
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    return p0
.end method

# Test that handler edges are not split. In this scenario, the catch block is
# the handler and also the successor of the try block.

## CHECK-START: int Builder.testHandlerEdge2(int, int, int) builder (after)

## CHECK:  name             "B0"
## CHECK:  successors       "<<BEnter1:B\d+>>"

## CHECK:  name             "<<BTry1:B\d+>>"
## CHECK:  predecessors     "<<BEnter1>>" "<<BExit1Enter2:B\d+>>" "<<BExit2:B\d+>>"
## CHECK:  successors       "<<BExit1Enter2>>"
## CHECK:  flags            "catch_block"
## CHECK:  Div

## CHECK:  name             "<<BTry2:B\d+>>"
## CHECK:  predecessors     "<<BExit1Enter2>>" "<<BEnter1>>" "<<BExit1Enter2>>"
## CHECK:  successors       "<<BExit2>>"
## CHECK:  flags            "catch_block"
## CHECK:  Div

## CHECK:  name             "<<BReturn:B\d+>>"
## CHECK:  predecessors     "<<BExit2>>"
## CHECK:  Return

## CHECK:  name             "<<BEnter1>>"
## CHECK:  predecessors     "B0"
## CHECK:  successors       "<<BTry1>>"
## CHECK:  xhandlers        "<<BTry2>>"
## CHECK:  TryBoundary      is_entry:true is_exit:false

## CHECK:  name             "<<BExit1Enter2>>"
## CHECK:  predecessors     "<<BTry1>>"
## CHECK:  successors       "<<BTry2>>"
## CHECK:  xhandlers        "<<BTry2>>" "<<BTry1>>"
## CHECK:  TryBoundary      is_entry:true is_exit:true

## CHECK:  name             "<<BExit2>>"
## CHECK:  predecessors     "<<BTry2>>"
## CHECK:  successors       "<<BReturn>>"
## CHECK:  xhandlers        "<<BTry1>>"
## CHECK:  TryBoundary      is_entry:false is_exit:true

.method public static testHandlerEdge2(III)I
    .registers 4

    :try_start_1
    :catch_all_1
    div-int/2addr p0, p1
    :try_end_1
    .catchall {:try_start_1 .. :try_end_1} :catch_all_2

    :try_start_2
    :catch_all_2
    div-int/2addr p0, p2
    :try_end_2
    .catchall {:try_start_2 .. :try_end_2} :catch_all_1

    return p0
.end method

# Test that a MOVE_RESULT instruction is placed into the same block as the
# INVOKE it follows, even if there is a try boundary between them.

## CHECK-START: int Builder.testMoveResult_Invoke(int, int, int) builder (after)

## CHECK:       <<Res:i\d+>> InvokeStaticOrDirect
## CHECK-NEXT:  StoreLocal   [v0,<<Res>>]

.method public static testMoveResult_Invoke(III)I
    .registers 3

    :try_start
    invoke-static {p0, p1, p2}, LBuilder;->testCatchLoop(III)I
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    move-result p0

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return
.end method

# Test that a MOVE_RESULT instruction is placed into the same block as the
# FILLED_NEW_ARRAY it follows, even if there is a try boundary between them.

## CHECK-START: int[] Builder.testMoveResult_FilledNewArray(int, int, int) builder (after)

## CHECK:      <<Res:l\d+>>     NewArray
## CHECK-NEXT:                  Temporary
## CHECK-NEXT: <<Local1:i\d+>>  LoadLocal  [v0]
## CHECK-NEXT:                  ArraySet   [<<Res>>,{{i\d+}},<<Local1>>]
## CHECK-NEXT: <<Local2:i\d+>>  LoadLocal  [v1]
## CHECK-NEXT:                  ArraySet   [<<Res>>,{{i\d+}},<<Local2>>]
## CHECK-NEXT: <<Local3:i\d+>>  LoadLocal  [v2]
## CHECK-NEXT:                  ArraySet   [<<Res>>,{{i\d+}},<<Local3>>]
## CHECK-NEXT:                  StoreLocal [v0,<<Res>>]

.method public static testMoveResult_FilledNewArray(III)[I
    .registers 3

    :try_start
    filled-new-array {p0, p1, p2}, [I
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    move-result-object p0

    :return
    return-object p0

    :catch_all
    const/4 p0, 0x0
    goto :return
.end method

# Test case for ReturnVoid inside a try block. Builder needs to move it outside
# the try block so as to not split the ReturnVoid-Exit edge.
# This invariant is enforced by GraphChecker.

.method public static testReturnVoidInTry(II)V
    .registers 2

    :catch_all
    :try_start
    return-void
    :try_end
    .catchall {:try_start .. :try_end} :catch_all
.end method

# Test case for Return inside a try block. Builder needs to move it outside the
# try block so as to not split the Return-Exit edge.
# This invariant is enforced by GraphChecker.

.method public static testReturnInTry(II)I
    .registers 2

    :try_start
    div-int/2addr p0, p1
    return p0
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :catch_all
    const/4 v0, 0x0
    return v0
.end method

# Test a (dead) try block which flows out of the method. The block will be
# removed by DCE but needs to pass post-builder GraphChecker.

## CHECK-START: int Builder.testDeadEndTry(int) builder (after)
## CHECK-NOT:     TryBoundary is_exit:true

.method public static testDeadEndTry(I)I
    .registers 1

    return p0

    :catch_all
    nop

    :try_start
    nop
    :try_end
    .catchall {:try_start .. :try_end} :catch_all
.end method
