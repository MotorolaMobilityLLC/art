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

.class public LSmaliTests;
.super Ljava/lang/Object;

## CHECK-START: int SmaliTests.EqualTrueRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Arg>>,<<Const1>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualTrueRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualTrueRhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x5
  if-eq p0, v0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualTrueLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Const1>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualTrueLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualTrueLhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x5
  if-eq v0, p0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualFalseRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Arg>>,<<Const0>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualFalseRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualFalseRhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x3
  if-eq p0, v0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualFalseLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Const0>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualFalseLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualFalseLhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x3
  if-eq v0, p0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualTrueRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Arg>>,<<Const1>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualTrueRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualTrueRhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x3
  if-ne p0, v0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualTrueLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Const1>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualTrueLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualTrueLhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x3
  if-ne v0, p0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualFalseRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Arg>>,<<Const0>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualFalseRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualFalseRhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x5
  if-ne p0, v0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualFalseLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Const0>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualFalseLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualFalseLhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x5
  if-ne v0, p0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.AddSubConst(int) instruction_simplifier (before)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const7:i\d+>>    IntConstant 7
## CHECK-DAG:     <<Const8:i\d+>>    IntConstant 8
## CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<Const7>>]
## CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Add>>,<<Const8>>]
## CHECK-DAG:                        Return [<<Sub>>]

## CHECK-START: int SmaliTests.AddSubConst(int) instruction_simplifier (after)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<ConstM1:i\d+>>   IntConstant -1
## CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<ConstM1>>]
## CHECK-DAG:                        Return [<<Add>>]

.method public static AddSubConst(I)I
    .registers 3

    .prologue
    add-int/lit8 v0, p0, 7

    const/16 v1, 8

    sub-int v0, v0, v1

    return v0
.end method

## CHECK-START: int SmaliTests.SubAddConst(int) instruction_simplifier (before)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const3:i\d+>>    IntConstant 3
## CHECK-DAG:     <<Const4:i\d+>>    IntConstant 4
## CHECK-DAG:     <<Sub:i\d+>>       Sub [<<ArgValue>>,<<Const3>>]
## CHECK-DAG:     <<Add:i\d+>>       Add [<<Sub>>,<<Const4>>]
## CHECK-DAG:                        Return [<<Add>>]

## CHECK-START: int SmaliTests.SubAddConst(int) instruction_simplifier (after)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>    IntConstant 1
## CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<Const1>>]
## CHECK-DAG:                        Return [<<Add>>]

.method public static SubAddConst(I)I
    .registers 2

    .prologue
    const/4 v0, 3

    sub-int v0, p0, v0

    add-int/lit8 v0, v0, 4

    return v0
.end method

## CHECK-START: int SmaliTests.SubSubConst1(int) instruction_simplifier (before)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const9:i\d+>>    IntConstant 9
## CHECK-DAG:     <<Const10:i\d+>>   IntConstant 10
## CHECK-DAG:     <<Sub1:i\d+>>      Sub [<<ArgValue>>,<<Const9>>]
## CHECK-DAG:     <<Sub2:i\d+>>      Sub [<<Sub1>>,<<Const10>>]
## CHECK-DAG:                        Return [<<Sub2>>]

## CHECK-START: int SmaliTests.SubSubConst1(int) instruction_simplifier (after)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<ConstM19:i\d+>>  IntConstant -19
## CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<ConstM19>>]
## CHECK-DAG:                        Return [<<Add>>]

.method public static SubSubConst1(I)I
    .registers 3

    .prologue
    const/16 v1, 9

    sub-int v0, p0, v1

    const/16 v1, 10

    sub-int v0, v0, v1

    return v0
.end method

## CHECK-START: int SmaliTests.SubSubConst2(int) instruction_simplifier (before)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const11:i\d+>>   IntConstant 11
## CHECK-DAG:     <<Const12:i\d+>>   IntConstant 12
## CHECK-DAG:     <<Sub1:i\d+>>      Sub [<<Const11>>,<<ArgValue>>]
## CHECK-DAG:     <<Sub2:i\d+>>      Sub [<<Sub1>>,<<Const12>>]
## CHECK-DAG:                        Return [<<Sub2>>]

## CHECK-START: int SmaliTests.SubSubConst2(int) instruction_simplifier (after)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<ConstM1:i\d+>>   IntConstant -1
## CHECK-DAG:     <<Sub:i\d+>>       Sub [<<ConstM1>>,<<ArgValue>>]
## CHECK-DAG:                        Return [<<Sub>>]

.method public static SubSubConst2(I)I
    .registers 3

    .prologue
    rsub-int/lit8 v0, p0, 11

    const/16 v1, 12

    sub-int v0, v0, v1

    return v0
.end method

## CHECK-START: int SmaliTests.SubSubConst3(int) instruction_simplifier (before)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const15:i\d+>>   IntConstant 15
## CHECK-DAG:     <<Const16:i\d+>>   IntConstant 16
## CHECK-DAG:     <<Sub1:i\d+>>      Sub [<<ArgValue>>,<<Const16>>]
## CHECK-DAG:     <<Sub2:i\d+>>      Sub [<<Const15>>,<<Sub1>>]
## CHECK-DAG:                        Return [<<Sub2>>]

## CHECK-START: int SmaliTests.SubSubConst3(int) instruction_simplifier (after)
## CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
## CHECK-DAG:     <<Const31:i\d+>>   IntConstant 31
## CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Const31>>,<<ArgValue>>]
## CHECK-DAG:                        Return [<<Sub>>]

.method public static SubSubConst3(I)I
    .registers 2

    .prologue
    const/16 v0, 16

    sub-int v0, p0, v0

    rsub-int/lit8 v0, v0, 15

    return v0
.end method
