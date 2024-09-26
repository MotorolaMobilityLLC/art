/*
 * Copyright (C) 2024 The Android Open Source Project
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

import dalvik.system.VMRuntime;

public class Main {
  private static final boolean SHOULD_PRINT = false;  // True causes failure.

  static VMRuntime runtime = VMRuntime.getRuntime();

  static boolean objects_can_move = false;  // Usually set to true in main.

  public static void main(String[] args) throws Exception {
    Object[] x = new Object[12];
    int[] int_array = new int[10];
    int[] large_int_array = new int[100_000];

    try {
      checkNonMovable(int_array);
    } catch (RuntimeException e) {
      if (!e.getMessage().contains(" movable ")) {
        throw e;
      }
      objects_can_move = true;
    }
    // Objects_can_move will be true for all collectors we care about.
    for (int i = 0; i < 10; ++i) {
      if (i != 0) {
        for (Object y : x) {
          checkNonMovable(y);
        }
      }
      checkMovable(int_array);
      checkMovable(large_int_array);
      // For completeness, we test various primitive types.
      // `addressOf` doesn't work for reference types, so we skip those here.
      // We check both large-object-space and regular objects.
      x[0] = runtime.newNonMovableArray(int.class, 100);
      x[1] = runtime.newNonMovableArray(int.class, 100_000);
      x[2] = runtime.newNonMovableArray(byte.class, 100);
      x[3] = runtime.newNonMovableArray(byte.class, 100_000);
      x[4] = runtime.newNonMovableArray(long.class, 100);
      x[5] = runtime.newNonMovableArray(long.class, 100_000);
      x[6] = runtime.newNonMovableArray(char.class, 100);
      x[7] = runtime.newNonMovableArray(char.class, 100_000);
      x[8] = runtime.newNonMovableArray(float.class, 100);
      x[9] = runtime.newNonMovableArray(float.class, 100_000);
      x[10] = runtime.newNonMovableArray(double.class, 100);
      x[11] = runtime.newNonMovableArray(double.class, 100_000);
      for (Object y : x) {
        checkNonMovable(y);
      }
      if (i % 2 == 1) {
        // Doesn't compile without casts because Object.clone() is protected.
        x[0] = ((int[])x[0]).clone();
        x[1] = ((int[])x[1]).clone();
        x[2] = ((byte[])x[2]).clone();
        x[3] = ((byte[])x[3]).clone();
        x[4] = ((long[])x[4]).clone();
        x[5] = ((long[])x[5]).clone();
        int_array = int_array.clone();
      } else {
        x[6] = ((char[])x[6]).clone();
        x[7] = ((char[])x[7]).clone();
        x[8] = ((float[])x[8]).clone();
        x[9] = ((float[])x[9]).clone();
        x[10] = ((double[])x[10]).clone();
        x[11] = ((double[])x[11]).clone();
        large_int_array = large_int_array.clone();
      }
      System.gc();
      System.runFinalization();
    }
    System.out.println("Done");
  }

  public static void checkNonMovable(Object x) {
    // If things go wrong, this will throw instead. A zero return would be very strange indeed.
    if (runtime.addressOf(x) == 0) {
      System.out.println("Unexpectedly got 0 address");
    }
  }

  public static void checkMovable(Object x) {
    if (!objects_can_move) {
      return;
    }
    try {
      if (runtime.addressOf(x) != 0) {
        System.out.println("addressOf succeeded on movable object");
      }
      System.out.println("Unexpectedly got 0 address in checkMovable");
    } catch (RuntimeException e) { /* expected */ }
  }
}
