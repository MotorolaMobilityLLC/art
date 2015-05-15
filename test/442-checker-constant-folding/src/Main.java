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

public class Main {

  public static void assertFalse(boolean condition) {
    if (condition) {
      throw new Error();
    }
  }

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertFloatEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertDoubleEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /**
   * Tiny three-register program exercising int constant folding
   * on negation.
   */

  // CHECK-START: int Main.IntNegation() constant_folding (before)
  // CHECK-DAG:     <<Const42:i\d+>>  IntConstant 42
  // CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Const42>>]
  // CHECK-DAG:                       Return [<<Neg>>]

  // CHECK-START: int Main.IntNegation() constant_folding (after)
  // CHECK-DAG:     <<ConstN42:i\d+>> IntConstant -42
  // CHECK-DAG:                       Return [<<ConstN42>>]

  public static int IntNegation() {
    int x, y;
    x = 42;
    y = -x;
    return y;
  }

  /**
   * Tiny three-register program exercising int constant folding
   * on addition.
   */

  // CHECK-START: int Main.IntAddition1() constant_folding (before)
  // CHECK-DAG:     <<Const1:i\d+>>  IntConstant 1
  // CHECK-DAG:     <<Const2:i\d+>>  IntConstant 2
  // CHECK-DAG:     <<Add:i\d+>>     Add [<<Const1>>,<<Const2>>]
  // CHECK-DAG:                      Return [<<Add>>]

  // CHECK-START: int Main.IntAddition1() constant_folding (after)
  // CHECK-DAG:     <<Const3:i\d+>>  IntConstant 3
  // CHECK-DAG:                      Return [<<Const3>>]

  public static int IntAddition1() {
    int a, b, c;
    a = 1;
    b = 2;
    c = a + b;
    return c;
  }

 /**
  * Small three-register program exercising int constant folding
  * on addition.
  */

  // CHECK-START: int Main.IntAddition2() constant_folding (before)
  // CHECK-DAG:     <<Const1:i\d+>>  IntConstant 1
  // CHECK-DAG:     <<Const2:i\d+>>  IntConstant 2
  // CHECK-DAG:     <<Const5:i\d+>>  IntConstant 5
  // CHECK-DAG:     <<Const6:i\d+>>  IntConstant 6
  // CHECK-DAG:     <<Add1:i\d+>>    Add [<<Const1>>,<<Const2>>]
  // CHECK-DAG:     <<Add2:i\d+>>    Add [<<Const5>>,<<Const6>>]
  // CHECK-DAG:     <<Add3:i\d+>>    Add [<<Add1>>,<<Add2>>]
  // CHECK-DAG:                      Return [<<Add3>>]

  // CHECK-START: int Main.IntAddition2() constant_folding (after)
  // CHECK-DAG:     <<Const14:i\d+>> IntConstant 14
  // CHECK-DAG:                      Return [<<Const14>>]

  public static int IntAddition2() {
    int a, b, c;
    a = 1;
    b = 2;
    a += b;
    b = 5;
    c = 6;
    b += c;
    c = a + b;
    return c;
  }

  /**
   * Tiny three-register program exercising int constant folding
   * on subtraction.
   */

  // CHECK-START: int Main.IntSubtraction() constant_folding (before)
  // CHECK-DAG:     <<Const6:i\d+>>  IntConstant 6
  // CHECK-DAG:     <<Const2:i\d+>>  IntConstant 2
  // CHECK-DAG:     <<Sub:i\d+>>     Sub [<<Const6>>,<<Const2>>]
  // CHECK-DAG:                      Return [<<Sub>>]

  // CHECK-START: int Main.IntSubtraction() constant_folding (after)
  // CHECK-DAG:     <<Const4:i\d+>>  IntConstant 4
  // CHECK-DAG:                      Return [<<Const4>>]

  public static int IntSubtraction() {
    int a, b, c;
    a = 6;
    b = 2;
    c = a - b;
    return c;
  }

  /**
   * Tiny three-register program exercising long constant folding
   * on addition.
   */

  // CHECK-START: long Main.LongAddition() constant_folding (before)
  // CHECK-DAG:     <<Const1:j\d+>>  LongConstant 1
  // CHECK-DAG:     <<Const2:j\d+>>  LongConstant 2
  // CHECK-DAG:     <<Add:j\d+>>     Add [<<Const1>>,<<Const2>>]
  // CHECK-DAG:                      Return [<<Add>>]

  // CHECK-START: long Main.LongAddition() constant_folding (after)
  // CHECK-DAG:     <<Const3:j\d+>>  LongConstant 3
  // CHECK-DAG:                      Return [<<Const3>>]

  public static long LongAddition() {
    long a, b, c;
    a = 1L;
    b = 2L;
    c = a + b;
    return c;
  }

  /**
   * Tiny three-register program exercising long constant folding
   * on subtraction.
   */

  // CHECK-START: long Main.LongSubtraction() constant_folding (before)
  // CHECK-DAG:     <<Const6:j\d+>>  LongConstant 6
  // CHECK-DAG:     <<Const2:j\d+>>  LongConstant 2
  // CHECK-DAG:     <<Sub:j\d+>>     Sub [<<Const6>>,<<Const2>>]
  // CHECK-DAG:                      Return [<<Sub>>]

  // CHECK-START: long Main.LongSubtraction() constant_folding (after)
  // CHECK-DAG:     <<Const4:j\d+>>  LongConstant 4
  // CHECK-DAG:                      Return [<<Const4>>]

  public static long LongSubtraction() {
    long a, b, c;
    a = 6L;
    b = 2L;
    c = a - b;
    return c;
  }

  /**
   * Three-register program with a constant (static) condition.
   */

  // CHECK-START: int Main.StaticCondition() constant_folding (before)
  // CHECK-DAG:     <<Const7:i\d+>>  IntConstant 7
  // CHECK-DAG:     <<Const2:i\d+>>  IntConstant 2
  // CHECK-DAG:     <<Cond:z\d+>>    GreaterThanOrEqual [<<Const7>>,<<Const2>>]
  // CHECK-DAG:                      If [<<Cond>>]

  // CHECK-START: int Main.StaticCondition() constant_folding (after)
  // CHECK-DAG:     <<Const1:i\d+>>  IntConstant 1
  // CHECK-DAG:                      If [<<Const1>>]

  public static int StaticCondition() {
    int a, b, c;
    a = 7;
    b = 2;
    if (a < b)
      c = a + b;
    else
      c = a - b;
    return c;
  }

  /**
   * Four-variable program with jumps leading to the creation of many
   * blocks.
   *
   * The intent of this test is to ensure that all constant expressions
   * are actually evaluated at compile-time, thanks to the reverse
   * (forward) post-order traversal of the the dominator tree.
   */

  // CHECK-START: int Main.JumpsAndConditionals(boolean) constant_folding (before)
  // CHECK-DAG:     <<Const2:i\d+>>  IntConstant 2
  // CHECK-DAG:     <<Const5:i\d+>>  IntConstant 5
  // CHECK-DAG:     <<Add:i\d+>>     Add [<<Const5>>,<<Const2>>]
  // CHECK-DAG:     <<Sub:i\d+>>     Sub [<<Const5>>,<<Const2>>]
  // CHECK-DAG:     <<Phi:i\d+>>     Phi [<<Add>>,<<Sub>>]
  // CHECK-DAG:                      Return [<<Phi>>]

  // CHECK-START: int Main.JumpsAndConditionals(boolean) constant_folding (after)
  // CHECK-DAG:     <<Const3:i\d+>>  IntConstant 3
  // CHECK-DAG:     <<Const7:i\d+>>  IntConstant 7
  // CHECK-DAG:     <<Phi:i\d+>>     Phi [<<Const7>>,<<Const3>>]
  // CHECK-DAG:                      Return [<<Phi>>]

  public static int JumpsAndConditionals(boolean cond) {
    int a, b, c;
    a = 5;
    b = 2;
    if (cond)
      c = a + b;
    else
      c = a - b;
    return c;
  }

  /**
   * Test optimizations of arithmetic identities yielding a constant result.
   */

  // CHECK-START: int Main.And0(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:     <<And:i\d+>>      And [<<Arg>>,<<Const0>>]
  // CHECK-DAG:                       Return [<<And>>]

  // CHECK-START: int Main.And0(int) constant_folding (after)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-NOT:                       And
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int And0(int arg) {
    return arg & 0;
  }

  // CHECK-START: long Main.Mul0(long) constant_folding (before)
  // CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-DAG:     <<Mul:j\d+>>      Mul [<<Arg>>,<<Const0>>]
  // CHECK-DAG:                       Return [<<Mul>>]

  // CHECK-START: long Main.Mul0(long) constant_folding (after)
  // CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-NOT:                       Mul
  // CHECK-DAG:                       Return [<<Const0>>]

  public static long Mul0(long arg) {
    return arg * 0;
  }

  // CHECK-START: int Main.OrAllOnes(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<ConstF:i\d+>>   IntConstant -1
  // CHECK-DAG:     <<Or:i\d+>>       Or [<<Arg>>,<<ConstF>>]
  // CHECK-DAG:                       Return [<<Or>>]

  // CHECK-START: int Main.OrAllOnes(int) constant_folding (after)
  // CHECK-DAG:     <<ConstF:i\d+>>   IntConstant -1
  // CHECK-NOT:                       Or
  // CHECK-DAG:                       Return [<<ConstF>>]

  public static int OrAllOnes(int arg) {
    return arg | -1;
  }

  // CHECK-START: long Main.Rem0(long) constant_folding (before)
  // CHECK-DAG:     <<Arg:j\d+>>           ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>        LongConstant 0
  // CHECK-DAG:     <<DivZeroCheck:j\d+>>  DivZeroCheck [<<Arg>>]
  // CHECK-DAG:     <<Rem:j\d+>>           Rem [<<Const0>>,<<DivZeroCheck>>]
  // CHECK-DAG:                            Return [<<Rem>>]

  // CHECK-START: long Main.Rem0(long) constant_folding (after)
  // CHECK-DAG:     <<Const0:j\d+>>        LongConstant 0
  // CHECK-NOT:                            Rem
  // CHECK-DAG:                            Return [<<Const0>>]

  public static long Rem0(long arg) {
    return 0 % arg;
  }

  // CHECK-START: int Main.Rem1(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  // CHECK-DAG:     <<Rem:i\d+>>      Rem [<<Arg>>,<<Const1>>]
  // CHECK-DAG:                       Return [<<Rem>>]

  // CHECK-START: int Main.Rem1(int) constant_folding (after)
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-NOT:                       Rem
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int Rem1(int arg) {
    return arg % 1;
  }

  // CHECK-START: long Main.RemN1(long) constant_folding (before)
  // CHECK-DAG:     <<Arg:j\d+>>           ParameterValue
  // CHECK-DAG:     <<ConstN1:j\d+>>       LongConstant -1
  // CHECK-DAG:     <<DivZeroCheck:j\d+>>  DivZeroCheck [<<ConstN1>>]
  // CHECK-DAG:     <<Rem:j\d+>>           Rem [<<Arg>>,<<DivZeroCheck>>]
  // CHECK-DAG:                            Return [<<Rem>>]

  // CHECK-START: long Main.RemN1(long) constant_folding (after)
  // CHECK-DAG:     <<Const0:j\d+>>        LongConstant 0
  // CHECK-NOT:                            Rem
  // CHECK-DAG:                            Return [<<Const0>>]

  public static long RemN1(long arg) {
    return arg % -1;
  }

  // CHECK-START: int Main.Shl0(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:     <<Shl:i\d+>>      Shl [<<Const0>>,<<Arg>>]
  // CHECK-DAG:                       Return [<<Shl>>]

  // CHECK-START: int Main.Shl0(int) constant_folding (after)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-NOT:                       Shl
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int Shl0(int arg) {
    return 0 << arg;
  }

  // CHECK-START: long Main.Shr0(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Const0>>,<<Arg>>]
  // CHECK-DAG:                       Return [<<Shr>>]

  // CHECK-START: long Main.Shr0(int) constant_folding (after)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-NOT:                       Shr
  // CHECK-DAG:                       Return [<<Const0>>]

  public static long Shr0(int arg) {
    return (long)0 >> arg;
  }

  // CHECK-START: long Main.SubSameLong(long) constant_folding (before)
  // CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  // CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Arg>>,<<Arg>>]
  // CHECK-DAG:                       Return [<<Sub>>]

  // CHECK-START: long Main.SubSameLong(long) constant_folding (after)
  // CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-NOT:                       Sub
  // CHECK-DAG:                       Return [<<Const0>>]

  public static long SubSameLong(long arg) {
    return arg - arg;
  }

  // CHECK-START: int Main.UShr0(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Const0>>,<<Arg>>]
  // CHECK-DAG:                       Return [<<UShr>>]

  // CHECK-START: int Main.UShr0(int) constant_folding (after)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-NOT:                       UShr
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int UShr0(int arg) {
    return 0 >>> arg;
  }

  // CHECK-START: int Main.XorSameInt(int) constant_folding (before)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Xor:i\d+>>      Xor [<<Arg>>,<<Arg>>]
  // CHECK-DAG:                       Return [<<Xor>>]

  // CHECK-START: int Main.XorSameInt(int) constant_folding (after)
  // CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-NOT:                       Xor
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int XorSameInt(int arg) {
    return arg ^ arg;
  }

  // CHECK-START: boolean Main.CmpFloatGreaterThanNaN(float) constant_folding (before)
  // CHECK-DAG:     <<Arg:f\d+>>      ParameterValue
  // CHECK-DAG:     <<ConstNan:f\d+>> FloatConstant nan
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:                       IntConstant 1
  // CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Arg>>,<<ConstNan>>]
  // CHECK-DAG:     <<Le:z\d+>>       LessThanOrEqual [<<Cmp>>,<<Const0>>]
  // CHECK-DAG:                       If [<<Le>>]

  // CHECK-START: boolean Main.CmpFloatGreaterThanNaN(float) constant_folding (after)
  // CHECK-DAG:                       ParameterValue
  // CHECK-DAG:                       FloatConstant nan
  // CHECK-DAG:                       IntConstant 0
  // CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  // CHECK-DAG:                       If [<<Const1>>]

  // CHECK-START: boolean Main.CmpFloatGreaterThanNaN(float) constant_folding (after)
  // CHECK-NOT:                       Compare
  // CHECK-NOT:                       LessThanOrEqual

  public static boolean CmpFloatGreaterThanNaN(float arg) {
    return arg > Float.NaN;
  }

  // CHECK-START: boolean Main.CmpDoubleLessThanNaN(double) constant_folding (before)
  // CHECK-DAG:     <<Arg:d\d+>>      ParameterValue
  // CHECK-DAG:     <<ConstNan:d\d+>> DoubleConstant nan
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:                       IntConstant 1
  // CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Arg>>,<<ConstNan>>]
  // CHECK-DAG:     <<Ge:z\d+>>       GreaterThanOrEqual [<<Cmp>>,<<Const0>>]
  // CHECK-DAG:                       If [<<Ge>>]

  // CHECK-START: boolean Main.CmpDoubleLessThanNaN(double) constant_folding (after)
  // CHECK-DAG:                       ParameterValue
  // CHECK-DAG:                       DoubleConstant nan
  // CHECK-DAG:                       IntConstant 0
  // CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  // CHECK-DAG:                       If [<<Const1>>]

  // CHECK-START: boolean Main.CmpDoubleLessThanNaN(double) constant_folding (after)
  // CHECK-NOT:                       Compare
  // CHECK-NOT:                       GreaterThanOrEqual

  public static boolean CmpDoubleLessThanNaN(double arg) {
    return arg < Double.NaN;
  }

  // CHECK-START: int Main.ReturnInt33() constant_folding (before)
  // CHECK-DAG:     <<Const33:j\d+>>  LongConstant 33
  // CHECK-DAG:     <<Convert:i\d+>>  TypeConversion [<<Const33>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: int Main.ReturnInt33() constant_folding (after)
  // CHECK-DAG:     <<Const33:i\d+>>  IntConstant 33
  // CHECK-DAG:                       Return [<<Const33>>]

  public static int ReturnInt33() {
    long imm = 33L;
    return (int) imm;
  }

  // CHECK-START: int Main.ReturnIntMax() constant_folding (before)
  // CHECK-DAG:     <<ConstMax:f\d+>> FloatConstant 1e+34
  // CHECK-DAG:     <<Convert:i\d+>>  TypeConversion [<<ConstMax>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: int Main.ReturnIntMax() constant_folding (after)
  // CHECK-DAG:     <<ConstMax:i\d+>> IntConstant 2147483647
  // CHECK-DAG:                       Return [<<ConstMax>>]

  public static int ReturnIntMax() {
    float imm = 1.0e34f;
    return (int) imm;
  }

  // CHECK-START: int Main.ReturnInt0() constant_folding (before)
  // CHECK-DAG:     <<ConstNaN:d\d+>> DoubleConstant nan
  // CHECK-DAG:     <<Convert:i\d+>>  TypeConversion [<<ConstNaN>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: int Main.ReturnInt0() constant_folding (after)
  // CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  // CHECK-DAG:                       Return [<<Const0>>]

  public static int ReturnInt0() {
    double imm = Double.NaN;
    return (int) imm;
  }

  // CHECK-START: long Main.ReturnLong33() constant_folding (before)
  // CHECK-DAG:     <<Const33:i\d+>>  IntConstant 33
  // CHECK-DAG:     <<Convert:j\d+>>  TypeConversion [<<Const33>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: long Main.ReturnLong33() constant_folding (after)
  // CHECK-DAG:     <<Const33:j\d+>>  LongConstant 33
  // CHECK-DAG:                       Return [<<Const33>>]

  public static long ReturnLong33() {
    int imm = 33;
    return (long) imm;
  }

  // CHECK-START: long Main.ReturnLong34() constant_folding (before)
  // CHECK-DAG:     <<Const34:f\d+>>  FloatConstant 34
  // CHECK-DAG:     <<Convert:j\d+>>  TypeConversion [<<Const34>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: long Main.ReturnLong34() constant_folding (after)
  // CHECK-DAG:     <<Const34:j\d+>>  LongConstant 34
  // CHECK-DAG:                       Return [<<Const34>>]

  public static long ReturnLong34() {
    float imm = 34.0f;
    return (long) imm;
  }

  // CHECK-START: long Main.ReturnLong0() constant_folding (before)
  // CHECK-DAG:     <<ConstNaN:d\d+>> DoubleConstant nan
  // CHECK-DAG:     <<Convert:j\d+>>  TypeConversion [<<ConstNaN>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: long Main.ReturnLong0() constant_folding (after)
  // CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  // CHECK-DAG:                       Return [<<Const0>>]

  public static long ReturnLong0() {
    double imm = -Double.NaN;
    return (long) imm;
  }

  // CHECK-START: float Main.ReturnFloat33() constant_folding (before)
  // CHECK-DAG:     <<Const33:i\d+>>  IntConstant 33
  // CHECK-DAG:     <<Convert:f\d+>>  TypeConversion [<<Const33>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: float Main.ReturnFloat33() constant_folding (after)
  // CHECK-DAG:     <<Const33:f\d+>>  FloatConstant 33
  // CHECK-DAG:                       Return [<<Const33>>]

  public static float ReturnFloat33() {
    int imm = 33;
    return (float) imm;
  }

  // CHECK-START: float Main.ReturnFloat34() constant_folding (before)
  // CHECK-DAG:     <<Const34:j\d+>>  LongConstant 34
  // CHECK-DAG:     <<Convert:f\d+>>  TypeConversion [<<Const34>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: float Main.ReturnFloat34() constant_folding (after)
  // CHECK-DAG:     <<Const34:f\d+>>  FloatConstant 34
  // CHECK-DAG:                       Return [<<Const34>>]

  public static float ReturnFloat34() {
    long imm = 34L;
    return (float) imm;
  }

  // CHECK-START: float Main.ReturnFloat99P25() constant_folding (before)
  // CHECK-DAG:     <<Const:d\d+>>    DoubleConstant 99.25
  // CHECK-DAG:     <<Convert:f\d+>>  TypeConversion [<<Const>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: float Main.ReturnFloat99P25() constant_folding (after)
  // CHECK-DAG:     <<Const:f\d+>>    FloatConstant 99.25
  // CHECK-DAG:                       Return [<<Const>>]

  public static float ReturnFloat99P25() {
    double imm = 99.25;
    return (float) imm;
  }

  // CHECK-START: double Main.ReturnDouble33() constant_folding (before)
  // CHECK-DAG:     <<Const33:i\d+>>  IntConstant 33
  // CHECK-DAG:     <<Convert:d\d+>>  TypeConversion [<<Const33>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: double Main.ReturnDouble33() constant_folding (after)
  // CHECK-DAG:     <<Const33:d\d+>>  DoubleConstant 33
  // CHECK-DAG:                       Return [<<Const33>>]

  public static double ReturnDouble33() {
    int imm = 33;
    return (double) imm;
  }

  // CHECK-START: double Main.ReturnDouble34() constant_folding (before)
  // CHECK-DAG:     <<Const34:j\d+>>  LongConstant 34
  // CHECK-DAG:     <<Convert:d\d+>>  TypeConversion [<<Const34>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: double Main.ReturnDouble34() constant_folding (after)
  // CHECK-DAG:     <<Const34:d\d+>>  DoubleConstant 34
  // CHECK-DAG:                       Return [<<Const34>>]

  public static double ReturnDouble34() {
    long imm = 34L;
    return (double) imm;
  }

  // CHECK-START: double Main.ReturnDouble99P25() constant_folding (before)
  // CHECK-DAG:     <<Const:f\d+>>    FloatConstant 99.25
  // CHECK-DAG:     <<Convert:d\d+>>  TypeConversion [<<Const>>]
  // CHECK-DAG:                       Return [<<Convert>>]

  // CHECK-START: double Main.ReturnDouble99P25() constant_folding (after)
  // CHECK-DAG:     <<Const:d\d+>>    DoubleConstant 99.25
  // CHECK-DAG:                       Return [<<Const>>]

  public static double ReturnDouble99P25() {
    float imm = 99.25f;
    return (double) imm;
  }

  public static void main(String[] args) {
    assertIntEquals(IntNegation(), -42);
    assertIntEquals(IntAddition1(), 3);
    assertIntEquals(IntAddition2(), 14);
    assertIntEquals(IntSubtraction(), 4);
    assertLongEquals(LongAddition(), 3L);
    assertLongEquals(LongSubtraction(), 4L);
    assertIntEquals(StaticCondition(), 5);
    assertIntEquals(JumpsAndConditionals(true), 7);
    assertIntEquals(JumpsAndConditionals(false), 3);
    int arbitrary = 123456;  // Value chosen arbitrarily.
    assertIntEquals(And0(arbitrary), 0);
    assertLongEquals(Mul0(arbitrary), 0);
    assertIntEquals(OrAllOnes(arbitrary), -1);
    assertLongEquals(Rem0(arbitrary), 0);
    assertIntEquals(Rem1(arbitrary), 0);
    assertLongEquals(RemN1(arbitrary), 0);
    assertIntEquals(Shl0(arbitrary), 0);
    assertLongEquals(Shr0(arbitrary), 0);
    assertLongEquals(SubSameLong(arbitrary), 0);
    assertIntEquals(UShr0(arbitrary), 0);
    assertIntEquals(XorSameInt(arbitrary), 0);
    assertFalse(CmpFloatGreaterThanNaN(arbitrary));
    assertFalse(CmpDoubleLessThanNaN(arbitrary));
    assertIntEquals(ReturnInt33(), 33);
    assertIntEquals(ReturnIntMax(), 2147483647);
    assertIntEquals(ReturnInt0(), 0);
    assertLongEquals(ReturnLong33(), 33);
    assertLongEquals(ReturnLong34(), 34);
    assertLongEquals(ReturnLong0(), 0);
    assertFloatEquals(ReturnFloat33(), 33);
    assertFloatEquals(ReturnFloat34(), 34);
    assertFloatEquals(ReturnFloat99P25(), 99.25f);
    assertDoubleEquals(ReturnDouble33(), 33);
    assertDoubleEquals(ReturnDouble34(), 34);
    assertDoubleEquals(ReturnDouble99P25(), 99.25);
  }
}
