/*
 * Copyright (C) 2015 The Android Open Source Project
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

class Circle {
  Circle(double radius) {
    this.radius = radius;
  }
  public double getArea() {
    return radius * radius * Math.PI;
  }
  private double radius;
};

class TestClass {
  TestClass() {
  }
  TestClass(int i, int j) {
    this.i = i;
    this.j = j;
  }
  int i;
  int j;
  volatile int k;
  TestClass next;
  static int si;
};

class SubTestClass extends TestClass {
  int k;
};

class TestClass2 {
  int i;
  int j;
};

public class Main {

  /// CHECK-START: double Main.calcCircleArea(double) load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: double Main.calcCircleArea(double) load_store_elimination (after)
  /// CHECK-NOT: NewInstance
  /// CHECK-NOT: InstanceFieldSet
  /// CHECK-NOT: InstanceFieldGet

  static double calcCircleArea(double radius) {
    return new Circle(radius).getArea();
  }

  /// CHECK-START: int Main.test1(TestClass, TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test1(TestClass, TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Different fields shouldn't alias.
  static int test1(TestClass obj1, TestClass obj2) {
    obj1.i = 1;
    obj2.j = 2;
    return obj1.i + obj2.j;
  }

  /// CHECK-START: int Main.test2(TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test2(TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldSet
  /// CHECK-NOT: InstanceFieldGet

  // Redundant store of the same value.
  static int test2(TestClass obj) {
    obj.j = 1;
    obj.j = 1;
    return obj.j;
  }

  /// CHECK-START: int Main.test3(TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldSet
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test3(TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NewInstance
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldSet
  /// CHECK-NOT: InstanceFieldGet

  // A new allocation shouldn't alias with pre-existing values.
  static int test3(TestClass obj) {
    obj.i = 1;
    obj.next.j = 2;
    TestClass obj2 = new TestClass();
    obj2.i = 3;
    obj2.j = 4;
    return obj.i + obj.next.j + obj2.i + obj2.j;
  }

  /// CHECK-START: int Main.test4(TestClass, boolean) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: Return
  /// CHECK: InstanceFieldSet

  /// CHECK-START: int Main.test4(TestClass, boolean) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet
  /// CHECK: Return
  /// CHECK: InstanceFieldSet

  // Set and merge the same value in two branches.
  static int test4(TestClass obj, boolean b) {
    if (b) {
      obj.i = 1;
    } else {
      obj.i = 1;
    }
    return obj.i;
  }

  /// CHECK-START: int Main.test5(TestClass, boolean) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: Return
  /// CHECK: InstanceFieldSet

  /// CHECK-START: int Main.test5(TestClass, boolean) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: Return
  /// CHECK: InstanceFieldSet

  // Set and merge different values in two branches.
  static int test5(TestClass obj, boolean b) {
    if (b) {
      obj.i = 1;
    } else {
      obj.i = 2;
    }
    return obj.i;
  }

  /// CHECK-START: int Main.test6(TestClass, TestClass, boolean) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test6(TestClass, TestClass, boolean) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Setting the same value doesn't clear the value for aliased locations.
  static int test6(TestClass obj1, TestClass obj2, boolean b) {
    obj1.i = 1;
    obj1.j = 2;
    if (b) {
      obj2.j = 2;
    }
    return obj1.j + obj2.j;
  }

  /// CHECK-START: int Main.test7(TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test7(TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  // Invocation should kill values in non-singleton heap locations.
  static int test7(TestClass obj) {
    obj.i = 1;
    System.out.print("");
    return obj.i;
  }

  /// CHECK-START: int Main.test8() load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InvokeVirtual
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test8() load_store_elimination (after)
  /// CHECK-NOT: NewInstance
  /// CHECK-NOT: InstanceFieldSet
  /// CHECK: InvokeVirtual
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Invocation should not kill values in singleton heap locations.
  static int test8() {
    TestClass obj = new TestClass();
    obj.i = 1;
    System.out.print("");
    return obj.i;
  }

  /// CHECK-START: int Main.test9(TestClass) load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test9(TestClass) load_store_elimination (after)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  // Invocation should kill values in non-singleton heap locations.
  static int test9(TestClass obj) {
    TestClass obj2 = new TestClass();
    obj2.i = 1;
    obj.next = obj2;
    System.out.print("");
    return obj2.i;
  }

  /// CHECK-START: int Main.test10(TestClass) load_store_elimination (before)
  /// CHECK: StaticFieldGet
  /// CHECK: InstanceFieldGet
  /// CHECK: StaticFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test10(TestClass) load_store_elimination (after)
  /// CHECK: StaticFieldGet
  /// CHECK: InstanceFieldGet
  /// CHECK: StaticFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Static fields shouldn't alias with instance fields.
  static int test10(TestClass obj) {
    TestClass.si += obj.i;
    return obj.i;
  }

  /// CHECK-START: int Main.test11(TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test11(TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Loop without heap writes.
  // obj.i is actually hoisted to the loop pre-header by licm already.
  static int test11(TestClass obj) {
    obj.i = 1;
    int sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += obj.i;
    }
    return sum;
  }

  /// CHECK-START: int Main.test12(TestClass, TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldSet

  /// CHECK-START: int Main.test12(TestClass, TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldSet

  // Loop with heap writes.
  static int test12(TestClass obj1, TestClass obj2) {
    obj1.i = 1;
    int sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += obj1.i;
      obj2.i = sum;
    }
    return sum;
  }

  /// CHECK-START: int Main.test13(TestClass, TestClass2) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test13(TestClass, TestClass2) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK-NOT: NullCheck
  /// CHECK-NOT: InstanceFieldGet

  // Different classes shouldn't alias.
  static int test13(TestClass obj1, TestClass2 obj2) {
    obj1.i = 1;
    obj2.i = 2;
    return obj1.i + obj2.i;
  }

  /// CHECK-START: int Main.test14(TestClass, SubTestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test14(TestClass, SubTestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  // Subclass may alias with super class.
  static int test14(TestClass obj1, SubTestClass obj2) {
    obj1.i = 1;
    obj2.i = 2;
    return obj1.i;
  }

  /// CHECK-START: int Main.test15() load_store_elimination (before)
  /// CHECK: StaticFieldSet
  /// CHECK: StaticFieldSet
  /// CHECK: StaticFieldGet

  /// CHECK-START: int Main.test15() load_store_elimination (after)
  /// CHECK: <<Const2:i\d+>> IntConstant 2
  /// CHECK: StaticFieldSet
  /// CHECK: StaticFieldSet
  /// CHECK-NOT: StaticFieldGet
  /// CHECK: Return [<<Const2>>]

  // Static field access from subclass's name.
  static int test15() {
    TestClass.si = 1;
    SubTestClass.si = 2;
    return TestClass.si;
  }

  /// CHECK-START: int Main.test16() load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test16() load_store_elimination (after)
  /// CHECK-NOT: NewInstance
  /// CHECK-NOT: StaticFieldSet
  /// CHECK-NOT: StaticFieldGet

  // Test inlined constructor.
  static int test16() {
    TestClass obj = new TestClass(1, 2);
    return obj.i + obj.j;
  }

  /// CHECK-START: int Main.test17() load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test17() load_store_elimination (after)
  /// CHECK: <<Const0:i\d+>> IntConstant 0
  /// CHECK-NOT: NewInstance
  /// CHECK-NOT: StaticFieldSet
  /// CHECK-NOT: StaticFieldGet
  /// CHECK: Return [<<Const0>>]

  // Test getting default value.
  static int test17() {
    TestClass obj = new TestClass();
    obj.j = 1;
    return obj.i;
  }

  /// CHECK-START: int Main.test18(TestClass) load_store_elimination (before)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  /// CHECK-START: int Main.test18(TestClass) load_store_elimination (after)
  /// CHECK: InstanceFieldSet
  /// CHECK: InstanceFieldGet

  // Volatile field load/store shouldn't be eliminated.
  static int test18(TestClass obj) {
    obj.k = 1;
    return obj.k;
  }

  /// CHECK-START: float Main.test19(float[], float[]) load_store_elimination (before)
  /// CHECK: <<IntTypeValue:i\d+>> ArrayGet
  /// CHECK: ArraySet
  /// CHECK: <<FloatTypeValue:f\d+>> ArrayGet

  /// CHECK-START: float Main.test19(float[], float[]) load_store_elimination (after)
  /// CHECK: <<IntTypeValue:i\d+>> ArrayGet
  /// CHECK: ArraySet
  /// CHECK: <<FloatTypeValue:f\d+>> ArrayGet

  // I/F, J/D aliasing should keep the load/store.
  static float test19(float[] fa1, float[] fa2) {
    fa1[0] = fa2[0];
    return fa1[0];
  }

  /// CHECK-START: TestClass Main.test20() load_store_elimination (before)
  /// CHECK: NewInstance
  /// CHECK: InstanceFieldSet

  /// CHECK-START: TestClass Main.test20() load_store_elimination (after)
  /// CHECK: NewInstance
  /// CHECK-NOT: InstanceFieldSet

  // Storing default heap value is redundant if the heap location has the
  // default heap value.
  static TestClass test20() {
    TestClass obj = new TestClass();
    obj.i = 0;
    return obj;
  }

  public static void assertIntEquals(int expected, int result) {
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

  public static void main(String[] args) {
    assertDoubleEquals(Math.PI * Math.PI * Math.PI, calcCircleArea(Math.PI));
    assertIntEquals(test1(new TestClass(), new TestClass()), 3);
    assertIntEquals(test2(new TestClass()), 1);
    TestClass obj1 = new TestClass();
    TestClass obj2 = new TestClass();
    obj1.next = obj2;
    assertIntEquals(test3(obj1), 10);
    assertIntEquals(test4(new TestClass(), true), 1);
    assertIntEquals(test4(new TestClass(), false), 1);
    assertIntEquals(test5(new TestClass(), true), 1);
    assertIntEquals(test5(new TestClass(), false), 2);
    assertIntEquals(test6(new TestClass(), new TestClass(), true), 4);
    assertIntEquals(test6(new TestClass(), new TestClass(), false), 2);
    assertIntEquals(test7(new TestClass()), 1);
    assertIntEquals(test8(), 1);
    obj1 = new TestClass();
    obj2 = new TestClass();
    obj1.next = obj2;
    assertIntEquals(test9(new TestClass()), 1);
    assertIntEquals(test10(new TestClass(3, 4)), 3);
    assertIntEquals(TestClass.si, 3);
    assertIntEquals(test11(new TestClass()), 10);
    assertIntEquals(test12(new TestClass(), new TestClass()), 10);
    assertIntEquals(test13(new TestClass(), new TestClass2()), 3);
    SubTestClass obj3 = new SubTestClass();
    assertIntEquals(test14(obj3, obj3), 2);
    assertIntEquals(test15(), 2);
    assertIntEquals(test16(), 3);
    assertIntEquals(test17(), 0);
    assertIntEquals(test18(new TestClass()), 1);
    float[] fa1 = { 0.8f };
    float[] fa2 = { 1.8f };
    assertFloatEquals(test19(fa1, fa2), 1.8f);
    assertFloatEquals(test20().i, 0);
  }
}
