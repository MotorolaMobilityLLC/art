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

public class Main {

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static boolean $inline$constantTrue() {
    return true;
  }

  public static boolean $inline$constantFalse() {
    return false;
  }

  /// CHECK-START: int Main.testTrueBranch(int, int) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:     <<ArgX:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<ArgY:i\d+>>    ParameterValue
  /// CHECK-DAG:                      If
  /// CHECK-DAG:     <<Add:i\d+>>     Add [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:     <<Sub:i\d+>>     Sub [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:     <<Phi:i\d+>>     Phi [<<Add>>,<<Sub>>]
  /// CHECK-DAG:                      Return [<<Phi>>]

  /// CHECK-START: int Main.testTrueBranch(int, int) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<ArgX:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<ArgY:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<Add:i\d+>>     Add [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:                      Return [<<Add>>]

  /// CHECK-START: int Main.testTrueBranch(int, int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      If
  /// CHECK-NOT:                      Sub
  /// CHECK-NOT:                      Phi

  public static int testTrueBranch(int x, int y) {
    int z;
    if ($inline$constantTrue()) {
      z = x + y;
    } else {
      z = x - y;
      // Prevent HSelect simplification by having a branch with multiple instructions.
      System.nanoTime();
    }
    return z;
  }

  /// CHECK-START: int Main.testFalseBranch(int, int) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:     <<ArgX:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<ArgY:i\d+>>    ParameterValue
  /// CHECK-DAG:                      If
  /// CHECK-DAG:     <<Add:i\d+>>     Add [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:     <<Sub:i\d+>>     Sub [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:     <<Phi:i\d+>>     Phi [<<Add>>,<<Sub>>]
  /// CHECK-DAG:                      Return [<<Phi>>]

  /// CHECK-START: int Main.testFalseBranch(int, int) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<ArgX:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<ArgY:i\d+>>    ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>     Sub [<<ArgX>>,<<ArgY>>]
  /// CHECK-DAG:                      Return [<<Sub>>]

  /// CHECK-START: int Main.testFalseBranch(int, int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      If
  /// CHECK-NOT:                      Add
  /// CHECK-NOT:                      Phi

  public static int testFalseBranch(int x, int y) {
    int z;
    if ($inline$constantFalse()) {
      z = x + y;
    } else {
      z = x - y;
      // Prevent HSelect simplification by having a branch with multiple instructions.
      System.nanoTime();
    }
    return z;
  }

  /// CHECK-START: int Main.testRemoveLoop(int) dead_code_elimination$after_inlining (before)
  /// CHECK:                          Mul

  /// CHECK-START: int Main.testRemoveLoop(int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      Mul

  public static int testRemoveLoop(int x) {
    if ($inline$constantFalse()) {
      for (int i = 0; i < x; ++i) {
        x *= x;
      }
    }
    return x;
  }

  /// CHECK-START: int Main.testInfiniteLoop(int) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:                      Return
  /// CHECK-DAG:                      Exit

  /// CHECK-START: int Main.testInfiniteLoop(int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      Return
  /// CHECK-NOT:                      Exit

  public static int testInfiniteLoop(int x) {
    while ($inline$constantTrue()) {
      x++;
    }
    return x;
  }

  /// CHECK-START: int Main.testDeadLoop(int) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:                      If
  /// CHECK-DAG:                      Add

  /// CHECK-START: int Main.testDeadLoop(int) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: int Main.testDeadLoop(int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      If
  /// CHECK-NOT:                      Add

  public static int testDeadLoop(int x) {
    while ($inline$constantFalse()) {
      x++;
    }
    return x;
  }

  /// CHECK-START: int Main.testUpdateLoopInformation(int) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:                      If
  /// CHECK-DAG:                      If
  /// CHECK-DAG:                      Add

  /// CHECK-START: int Main.testUpdateLoopInformation(int) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: int Main.testUpdateLoopInformation(int) dead_code_elimination$after_inlining (after)
  /// CHECK-NOT:                      If
  /// CHECK-NOT:                      Add

  public static int testUpdateLoopInformation(int x) {
    // Use of Or in the condition generates a dead loop where not all of its
    // blocks are removed. This forces DCE to update their loop information.
    while ($inline$constantFalse() || !$inline$constantTrue()) {
      x++;
    }
    return x;
  }

  /// CHECK-START: int Main.testRemoveSuspendCheck(int, int) dead_code_elimination$after_inlining (before)
  /// CHECK:                          SuspendCheck
  /// CHECK:                          SuspendCheck
  /// CHECK:                          SuspendCheck
  /// CHECK-NOT:                      SuspendCheck

  /// CHECK-START: int Main.testRemoveSuspendCheck(int, int) dead_code_elimination$after_inlining (after)
  /// CHECK:                          SuspendCheck
  /// CHECK:                          SuspendCheck
  /// CHECK-NOT:                      SuspendCheck

  public static int testRemoveSuspendCheck(int x, int y) {
    // Inner loop will leave behind the header with its SuspendCheck. DCE must
    // remove it, otherwise the outer loop would end up with two.
    while (y > 0) {
      while ($inline$constantFalse() || !$inline$constantTrue()) {
        x++;
      }
      y--;
    }
    return x;
  }

  public static void main(String[] args) {
    assertIntEquals(7, testTrueBranch(4, 3));
    assertIntEquals(1, testFalseBranch(4, 3));
    assertIntEquals(42, testRemoveLoop(42));
    assertIntEquals(23, testUpdateLoopInformation(23));
    assertIntEquals(12, testRemoveSuspendCheck(12, 5));
  }
}
