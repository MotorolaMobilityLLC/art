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

  // CHECK-START: int Main.sieve(int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: int Main.sieve(int) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static int sieve(int size) {
    int primeCount = 0;
    boolean[] flags = new boolean[size + 1];
    for (int i = 1; i < size; i++) flags[i] = true; // Can eliminate.
    for (int i = 2; i < size; i++) {
      if (flags[i]) { // Can eliminate.
        primeCount++;
        for (int k = i + 1; k <= size; k += i)
          flags[k - 1] = false; // Can't eliminate yet due to (k+i) may overflow.
      }
    }
    return primeCount;
  }


  // CHECK-START: void Main.narrow(int[], int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.narrow(int[], int) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void narrow(int[] array, int offset) {
    if (offset < 0) {
      return;
    }
    if (offset < array.length) {
      // offset is in range [0, array.length-1].
      // Bounds check can be eliminated.
      array[offset] = 1;

      int biased_offset1 = offset + 1;
      // biased_offset1 is in range [1, array.length].
      if (biased_offset1 < array.length) {
        // biased_offset1 is in range [1, array.length-1].
        // Bounds check can be eliminated.
        array[biased_offset1] = 1;
      }

      int biased_offset2 = offset + 0x70000000;
      // biased_offset2 is in range [0x70000000, array.length-1+0x70000000].
      // It may overflow and be negative.
      if (biased_offset2 < array.length) {
        // Even with this test, biased_offset2 can be negative so we can't
        // eliminate this bounds check.
        array[biased_offset2] = 1;
      }

      // offset_sub1 won't underflow since offset is no less than 0.
      int offset_sub1 = offset - Integer.MAX_VALUE;
      if (offset_sub1 >= 0) {
        array[offset_sub1] = 1;  // Bounds check can be eliminated.
      }

      // offset_sub2 can underflow.
      int offset_sub2 = offset_sub1 - Integer.MAX_VALUE;
      if (offset_sub2 >= 0) {
        array[offset_sub2] = 1;  // Bounds check can't be eliminated.
      }
    }
  }


  // CHECK-START: void Main.constantIndexing(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantIndexing(int[]) BCE (after)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void constantIndexing(int[] array) {
    array[5] = 1;
    array[4] = 1;
    array[6] = 1;
  }


  // CHECK-START: void Main.loopPattern1(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern1(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern1(int[] array) {
    for (int i = 0; i < array.length; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 1; i < array.length; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 1; i < array.length - 1; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = -1; i < array.length; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = 0; i <= array.length; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = 0; i < array.length; i += 2) {
      // We don't have any assumption on max array length yet.
      // Bounds check can't be eliminated due to overflow concern.
      array[i] = 1;
    }

    for (int i = 1; i < array.length; i += 2) {
      // Bounds check can be eliminated since i is odd so the last
      // i that's less than array.length is at most (Integer.MAX_VALUE - 2).
      array[i] = 1;
    }
  }


  // CHECK-START: void Main.loopPattern2(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern2(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern2(int[] array) {
    for (int i = array.length - 1; i >= 0; i--) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length; i > 0; i--) {
      array[i - 1] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length - 1; i > 0; i--) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length; i >= 0; i--) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = array.length; i >= 0; i--) {
      array[i - 1] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = array.length; i > 0; i -= 20) {
      // For i >= 0, (i - 20 - 1) is guaranteed not to underflow.
      array[i - 1] = 1;  // Bounds check can be eliminated.
    }
  }


  // CHECK-START: void Main.loopPattern3(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern3(int[]) BCE (after)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern3(int[] array) {
    java.util.Random random = new java.util.Random();
    for (int i = 0; ; i++) {
      if (random.nextInt() % 1000 == 0 && i < array.length) {
        // Can't eliminate the bound check since not every i++ is
        // matched with a array length check, so there is some chance that i
        // overflows and is negative.
        array[i] = 1;
      }
    }
  }


  // CHECK-START: void Main.constantNewArray() BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantNewArray() BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void constantNewArray() {
    int[] array = new int[10];
    for (int i = 0; i < 10; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 0; i <= 10; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    array[0] = 1;  // Bounds check can be eliminated.
    array[9] = 1;  // Bounds check can be eliminated.
    array[10] = 1; // Bounds check can't be eliminated.
  }


  static byte readData() {
    return 1;
  }

  // CHECK-START: void Main.circularBufferProducer() BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.circularBufferProducer() BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void circularBufferProducer() {
    byte[] array = new byte[4096];
    int i = 0;
    while (true) {
      array[i & (array.length - 1)] = readData();
      i++;
    }
  }


  // CHECK-START: void Main.pyramid1(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid1(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid1(int[] array) {
    for (int i = 0; i < (array.length + 1) / 2; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: void Main.pyramid2(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid2(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid2(int[] array) {
    for (int i = 0; i < (array.length + 1) >> 1; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: void Main.pyramid3(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid3(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid3(int[] array) {
    for (int i = 0; i < (array.length + 1) >>> 1; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: boolean Main.isPyramid(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: boolean Main.isPyramid(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  static boolean isPyramid(int[] array) {
    int i = 0;
    int j = array.length - 1;
    while (i <= j) {
      if (array[i] != i) {
        return false;
      }
      if (array[j] != i) {
        return false;
      }
      i++; j--;
    }
    return true;
  }


  // CHECK-START: void Main.bubbleSort(int[]) GVN (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.bubbleSort(int[]) GVN (after)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.bubbleSort(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void bubbleSort(int[] array) {
    for (int i = 0; i < array.length - 1; i++) {
      for (int j = 0; j < array.length - i - 1; j++) {
        if (array[j] > array[j + 1]) {
          int temp = array[j + 1];
          array[j + 1] = array[j];
          array[j] = temp;
        }
      }
    }
  }


  public static void main(String[] args) {
    sieve(20);

    int[] array = {5, 2, 3, 7, 0, 1, 6, 4};
    bubbleSort(array);
    for (int i = 0; i < 8; i++) {
      if (array[i] != i) {
        System.out.println("bubble sort failed!");
      }
    }

    array = new int[7];
    pyramid1(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid1 failed!");
    }

    array = new int[8];
    pyramid2(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid2 failed!");
    }

    java.util.Arrays.fill(array, -1);
    pyramid3(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid3 failed!");
    }
  }
}
