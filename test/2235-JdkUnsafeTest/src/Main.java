/*
 * Copyright (C) 2021 The Android Open Source Project
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

import java.lang.reflect.Field;
import jdk.internal.misc.Unsafe;

public class Main {
  private static void check(int actual, int expected, String msg) {
    if (actual != expected) {
      System.out.println(msg + " : " + actual + " != " + expected);
      System.exit(1);
    }
  }

  private static void check(long actual, long expected, String msg) {
    if (actual != expected) {
      System.out.println(msg + " : " + actual + " != " + expected);
      System.exit(1);
    }
  }

  private static void check(float actual, float expected, String msg) {
    if (actual != expected) {
      System.out.println(msg + " : " + actual + " != " + expected);
      System.exit(1);
    }
  }

  private static void check(double actual, double expected, String msg) {
    if (actual != expected) {
      System.out.println(msg + " : " + actual + " != " + expected);
      System.exit(1);
    }
  }

  private static void check(Object actual, Object expected, String msg) {
    if (actual != expected) {
      System.out.println(msg + " : " + actual + " != " + expected);
      System.exit(1);
    }
  }

  private static Unsafe getUnsafe() throws NoSuchFieldException, IllegalAccessException {
    Class<?> unsafeClass = Unsafe.class;
    Field f = unsafeClass.getDeclaredField("theUnsafe");
    f.setAccessible(true);
    return (Unsafe) f.get(null);
  }

  public static void main(String[] args) throws NoSuchFieldException, IllegalAccessException {
    System.loadLibrary(args[0]);
    Unsafe unsafe = getUnsafe();

    testArrayBaseOffset(unsafe);
    testArrayIndexScale(unsafe);
    testGetAndPutAndCAS(unsafe);
    testCompareAndSet(unsafe);
    testGetAndPutVolatile(unsafe);
    testGetAcquireAndPutRelease(unsafe);
    testCopyMemoryPrimitiveArrays(unsafe);
  }

  private static void testArrayBaseOffset(Unsafe unsafe) {
    check(unsafe.arrayBaseOffset(boolean[].class), vmJdkArrayBaseOffset(boolean[].class),
        "Unsafe.arrayBaseOffset(boolean[])");
    check(unsafe.arrayBaseOffset(byte[].class), vmJdkArrayBaseOffset(byte[].class),
        "Unsafe.arrayBaseOffset(byte[])");
    check(unsafe.arrayBaseOffset(char[].class), vmJdkArrayBaseOffset(char[].class),
        "Unsafe.arrayBaseOffset(char[])");
    check(unsafe.arrayBaseOffset(double[].class), vmJdkArrayBaseOffset(double[].class),
        "Unsafe.arrayBaseOffset(double[])");
    check(unsafe.arrayBaseOffset(float[].class), vmJdkArrayBaseOffset(float[].class),
        "Unsafe.arrayBaseOffset(float[])");
    check(unsafe.arrayBaseOffset(int[].class), vmJdkArrayBaseOffset(int[].class),
        "Unsafe.arrayBaseOffset(int[])");
    check(unsafe.arrayBaseOffset(long[].class), vmJdkArrayBaseOffset(long[].class),
        "Unsafe.arrayBaseOffset(long[])");
    check(unsafe.arrayBaseOffset(Object[].class), vmJdkArrayBaseOffset(Object[].class),
        "Unsafe.arrayBaseOffset(Object[])");
  }

  private static void testArrayIndexScale(Unsafe unsafe) {
    check(unsafe.arrayIndexScale(boolean[].class), vmJdkArrayIndexScale(boolean[].class),
        "Unsafe.arrayIndexScale(boolean[])");
    check(unsafe.arrayIndexScale(byte[].class), vmJdkArrayIndexScale(byte[].class),
        "Unsafe.arrayIndexScale(byte[])");
    check(unsafe.arrayIndexScale(char[].class), vmJdkArrayIndexScale(char[].class),
        "Unsafe.arrayIndexScale(char[])");
    check(unsafe.arrayIndexScale(double[].class), vmJdkArrayIndexScale(double[].class),
        "Unsafe.arrayIndexScale(double[])");
    check(unsafe.arrayIndexScale(float[].class), vmJdkArrayIndexScale(float[].class),
        "Unsafe.arrayIndexScale(float[])");
    check(unsafe.arrayIndexScale(int[].class), vmJdkArrayIndexScale(int[].class),
        "Unsafe.arrayIndexScale(int[])");
    check(unsafe.arrayIndexScale(long[].class), vmJdkArrayIndexScale(long[].class),
        "Unsafe.arrayIndexScale(long[])");
    check(unsafe.arrayIndexScale(Object[].class), vmJdkArrayIndexScale(Object[].class),
        "Unsafe.arrayIndexScale(Object[])");
  }

  private static void testGetAndPutAndCAS(Unsafe unsafe) throws NoSuchFieldException {
    TestClass t = new TestClass();

    int intValue = 12345678;
    Field intField = TestClass.class.getDeclaredField("intVar");
    long intOffset = unsafe.objectFieldOffset(intField);
    check(unsafe.getInt(t, intOffset), 0, "Unsafe.getInt(Object, long) - initial");
    unsafe.putInt(t, intOffset, intValue);
    check(t.intVar, intValue, "Unsafe.putInt(Object, long, int)");
    check(unsafe.getInt(t, intOffset), intValue, "Unsafe.getInt(Object, long)");

    long longValue = 1234567887654321L;
    Field longField = TestClass.class.getDeclaredField("longVar");
    long longOffset = unsafe.objectFieldOffset(longField);
    check(unsafe.getLong(t, longOffset), 0, "Unsafe.getLong(Object, long) - initial");
    unsafe.putLong(t, longOffset, longValue);
    check(t.longVar, longValue, "Unsafe.putLong(Object, long, long)");
    check(unsafe.getLong(t, longOffset), longValue, "Unsafe.getLong(Object, long)");

    Object objectValue = new Object();
    Field objectField = TestClass.class.getDeclaredField("objectVar");
    long objectOffset = unsafe.objectFieldOffset(objectField);
    check(unsafe.getObject(t, objectOffset), null, "Unsafe.getObject(Object, long) - initial");
    unsafe.putObject(t, objectOffset, objectValue);
    check(t.objectVar, objectValue, "Unsafe.putObject(Object, long, Object)");
    check(unsafe.getObject(t, objectOffset), objectValue, "Unsafe.getObject(Object, long)");

    if (unsafe.compareAndSwapInt(t, intOffset, 0, 1)) {
      System.out.println("Unexpectedly succeeding compareAndSwapInt(t, intOffset, 0, 1)");
    }
    if (!unsafe.compareAndSwapInt(t, intOffset, intValue, 0)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSwapInt(t, intOffset, intValue, 0)");
    }
    if (!unsafe.compareAndSwapInt(t, intOffset, 0, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSwapInt(t, intOffset, 0, 1)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapInt using the same
    // integer (1) for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSwapInt(t, intOffset, 1, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSwapInt(t, intOffset, 1, 1)");
    }

    if (unsafe.compareAndSwapLong(t, longOffset, 0, 1)) {
      System.out.println("Unexpectedly succeeding compareAndSwapLong(t, longOffset, 0, 1)");
    }
    if (!unsafe.compareAndSwapLong(t, longOffset, longValue, 0)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSwapLong(t, longOffset, longValue, 0)");
    }
    if (!unsafe.compareAndSwapLong(t, longOffset, 0, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSwapLong(t, longOffset, 0, 1)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapLong using the same
    // integer (1) for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSwapLong(t, longOffset, 1, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSwapLong(t, longOffset, 1, 1)");
    }

    // We do not use `null` as argument to jdk.internal.misc.Unsafe.compareAndSwapObject
    // in those tests, as this value is not affected by heap poisoning
    // (which uses address negation to poison and unpoison heap object
    // references).  This way, when heap poisoning is enabled, we can
    // better exercise its implementation within that method.
    if (unsafe.compareAndSwapObject(t, objectOffset, new Object(), new Object())) {
      System.out.println("Unexpectedly succeeding " +
          "compareAndSwapObject(t, objectOffset, new Object(), new Object())");
    }
    Object objectValue2 = new Object();
    if (!unsafe.compareAndSwapObject(t, objectOffset, objectValue, objectValue2)) {
      System.out.println("Unexpectedly not succeeding " +
          "compareAndSwapObject(t, objectOffset, objectValue, objectValue2)");
    }
    Object objectValue3 = new Object();
    if (!unsafe.compareAndSwapObject(t, objectOffset, objectValue2, objectValue3)) {
      System.out.println("Unexpectedly not succeeding " +
          "compareAndSwapObject(t, objectOffset, objectValue2, objectValue3)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapObject using the same
    // object (`objectValue3`) for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSwapObject(t, objectOffset, objectValue3, objectValue3)) {
      System.out.println("Unexpectedly not succeeding " +
          "compareAndSwapObject(t, objectOffset, objectValue3, objectValue3)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapObject using the same
    // object (`t`) for the `obj` and `newValue` arguments.
    if (!unsafe.compareAndSwapObject(t, objectOffset, objectValue3, t)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSwapObject(t, objectOffset, objectValue3, t)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapObject using the same
    // object (`t`) for the `obj`, `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSwapObject(t, objectOffset, t, t)) {
      System.out.println("Unexpectedly not succeeding compareAndSwapObject(t, objectOffset, t, t)");
    }
    // Exercise jdk.internal.misc.Unsafe.compareAndSwapObject using the same
    // object (`t`) for the `obj` and `expectedValue` arguments.
    if (!unsafe.compareAndSwapObject(t, objectOffset, t, new Object())) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSwapObject(t, objectOffset, t, new Object())");
    }
  }

  private static void testCompareAndSet(Unsafe unsafe) throws NoSuchFieldException {
    TestClass t = new TestClass();

    int intValue = 12345678;
    Field intField = TestClass.class.getDeclaredField("intVar");
    long intOffset = unsafe.objectFieldOffset(intField);
    unsafe.putInt(t, intOffset, intValue);

    long longValue = 1234567887654321L;
    Field longField = TestClass.class.getDeclaredField("longVar");
    long longOffset = unsafe.objectFieldOffset(longField);
    unsafe.putLong(t, longOffset, longValue);

    Object objectValue = new Object();
    Field objectField = TestClass.class.getDeclaredField("objectVar");
    long objectOffset = unsafe.objectFieldOffset(objectField);
    unsafe.putObject(t, objectOffset, objectValue);

    if (unsafe.compareAndSetInt(t, intOffset, 0, 1)) {
      System.out.println("Unexpectedly succeeding compareAndSetInt(t, intOffset, 0, 1)");
    }
    check(t.intVar, intValue, "Unsafe.compareAndSetInt(Object, long, int, int) - not set");
    if (!unsafe.compareAndSetInt(t, intOffset, intValue, 0)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSetInt(t, intOffset, intValue, 0)");
    }
    check(t.intVar, 0, "Unsafe.compareAndSetInt(Object, long, int, int) - gets set");
    if (!unsafe.compareAndSetInt(t, intOffset, 0, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSetInt(t, intOffset, 0, 1)");
    }
    check(t.intVar, 1, "Unsafe.compareAndSetInt(Object, long, int, int) - gets re-set");
    // Exercise jdk.internal.misc.Unsafe.compareAndSetInt using the same
    // integer (1) for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSetInt(t, intOffset, 1, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSetInt(t, intOffset, 1, 1)");
    }
    check(t.intVar, 1, "Unsafe.compareAndSetInt(Object, long, int, int) - gets set to same");

    if (unsafe.compareAndSetLong(t, longOffset, 0, 1)) {
      System.out.println("Unexpectedly succeeding compareAndSetLong(t, longOffset, 0, 1)");
    }
    check(t.longVar, longValue, "Unsafe.compareAndSetLong(Object, long, long, long) - not set");
    if (!unsafe.compareAndSetLong(t, longOffset, longValue, 0)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSetLong(t, longOffset, longValue, 0)");
    }
    check(t.longVar, 0, "Unsafe.compareAndSetLong(Object, long, long, long) - gets set");
    if (!unsafe.compareAndSetLong(t, longOffset, 0, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSetLong(t, longOffset, 0, 1)");
    }
    check(t.longVar, 1, "Unsafe.compareAndSetLong(Object, long, long, long) - gets re-set");
    // Exercise jdk.internal.misc.Unsafe.compareAndSetLong using the same
    // integer (1) for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSetLong(t, longOffset, 1, 1)) {
      System.out.println("Unexpectedly not succeeding compareAndSetLong(t, longOffset, 1, 1)");
    }
    check(t.longVar, 1, "Unsafe.compareAndSetLong(Object, long, long, long) - gets set to same");

    // We do not use `null` as argument to jdk.internal.misc.Unsafe.compareAndSwapObject
    // in those tests, as this value is not affected by heap poisoning
    // (which uses address negation to poison and unpoison heap object
    // references).  This way, when heap poisoning is enabled, we can
    // better exercise its implementation within that method.
    if (unsafe.compareAndSetObject(t, objectOffset, new Object(), new Object())) {
      System.out.println("Unexpectedly succeeding compareAndSetObject(t, objectOffset, 0, 1)");
    }
    check(t.objectVar, objectValue, "Unsafe.compareAndSetObject(Object, long, Object, Object) - not set");
    Object objectValue2 = new Object();
    if (!unsafe.compareAndSetObject(t, objectOffset, objectValue, objectValue2)) {
      System.out.println(
          "Unexpectedly not succeeding compareAndSetObject(t, objectOffset, objectValue, 0)");
    }
    check(t.objectVar, objectValue2, "Unsafe.compareAndSetObject(Object, long, Object, Object) - gets set");
    Object objectValue3 = new Object();
    if (!unsafe.compareAndSetObject(t, objectOffset, objectValue2, objectValue3)) {
      System.out.println("Unexpectedly not succeeding compareAndSetObject(t, objectOffset, 0, 1)");
    }
    check(t.objectVar, objectValue3, "Unsafe.compareAndSetObject(Object, long, Object, Object) - gets re-set");
    // Exercise jdk.internal.misc.Unsafe.compareAndSetObject using the same
    // object for the `expectedValue` and `newValue` arguments.
    if (!unsafe.compareAndSetObject(t, objectOffset, objectValue3, objectValue3)) {
      System.out.println("Unexpectedly not succeeding compareAndSetObject(t, objectOffset, 1, 1)");
    }
    check(t.objectVar, objectValue3, "Unsafe.compareAndSetObject(Object, long, Object, Object) - gets set to same");
 }

  private static void testGetAndPutVolatile(Unsafe unsafe) throws NoSuchFieldException {
    TestVolatileClass tv = new TestVolatileClass();

    int intValue = 12345678;
    Field volatileIntField = TestVolatileClass.class.getDeclaredField("volatileIntVar");
    long volatileIntOffset = unsafe.objectFieldOffset(volatileIntField);
    check(unsafe.getIntVolatile(tv, volatileIntOffset),
          0,
          "Unsafe.getIntVolatile(Object, long) - initial");
    unsafe.putIntVolatile(tv, volatileIntOffset, intValue);
    check(tv.volatileIntVar, intValue, "Unsafe.putIntVolatile(Object, long, int)");
    check(unsafe.getIntVolatile(tv, volatileIntOffset),
          intValue,
          "Unsafe.getIntVolatile(Object, long)");

    long longValue = 1234567887654321L;
    Field volatileLongField = TestVolatileClass.class.getDeclaredField("volatileLongVar");
    long volatileLongOffset = unsafe.objectFieldOffset(volatileLongField);
    check(unsafe.getLongVolatile(tv, volatileLongOffset),
          0,
          "Unsafe.getLongVolatile(Object, long) - initial");
    unsafe.putLongVolatile(tv, volatileLongOffset, longValue);
    check(tv.volatileLongVar, longValue, "Unsafe.putLongVolatile(Object, long, long)");
    check(unsafe.getLongVolatile(tv, volatileLongOffset),
          longValue,
          "Unsafe.getLongVolatile(Object, long)");

    Object objectValue = new Object();
    Field volatileObjectField = TestVolatileClass.class.getDeclaredField("volatileObjectVar");
    long volatileObjectOffset = unsafe.objectFieldOffset(volatileObjectField);
    check(unsafe.getObjectVolatile(tv, volatileObjectOffset),
          null,
          "Unsafe.getObjectVolatile(Object, long) - initial");
    unsafe.putObjectVolatile(tv, volatileObjectOffset, objectValue);
    check(tv.volatileObjectVar, objectValue, "Unsafe.putObjectVolatile(Object, long, Object)");
    check(unsafe.getObjectVolatile(tv, volatileObjectOffset),
          objectValue,
          "Unsafe.getObjectVolatile(Object, long)");
  }

  private static void testGetAcquireAndPutRelease(Unsafe unsafe) throws NoSuchFieldException {
    TestVolatileClass tv = new TestVolatileClass();

    int intValue = 12345678;
    Field volatileIntField = TestVolatileClass.class.getDeclaredField("volatileIntVar");
    long volatileIntOffset = unsafe.objectFieldOffset(volatileIntField);
    check(unsafe.getIntAcquire(tv, volatileIntOffset),
          0,
          "Unsafe.getIntAcquire(Object, long) - initial");
    unsafe.putIntRelease(tv, volatileIntOffset, intValue);
    check(tv.volatileIntVar, intValue, "Unsafe.putIntRelease(Object, long, int)");
    check(unsafe.getIntAcquire(tv, volatileIntOffset),
          intValue,
          "Unsafe.getIntAcquire(Object, long)");

    long longValue = 1234567887654321L;
    Field volatileLongField = TestVolatileClass.class.getDeclaredField("volatileLongVar");
    long volatileLongOffset = unsafe.objectFieldOffset(volatileLongField);
    check(unsafe.getLongAcquire(tv, volatileLongOffset),
          0,
          "Unsafe.getLongAcquire(Object, long) - initial");
    unsafe.putLongRelease(tv, volatileLongOffset, longValue);
    check(tv.volatileLongVar, longValue, "Unsafe.putLongRelease(Object, long, long)");
    check(unsafe.getLongAcquire(tv, volatileLongOffset),
          longValue,
          "Unsafe.getLongAcquire(Object, long)");

    Object objectValue = new Object();
    Field volatileObjectField = TestVolatileClass.class.getDeclaredField("volatileObjectVar");
    long volatileObjectOffset = unsafe.objectFieldOffset(volatileObjectField);
    check(unsafe.getObjectAcquire(tv, volatileObjectOffset),
          null,
          "Unsafe.getObjectAcquire(Object, long) - initial");
    unsafe.putObjectRelease(tv, volatileObjectOffset, objectValue);
    check(tv.volatileObjectVar, objectValue, "Unsafe.putObjectRelease(Object, long, Object)");
    check(unsafe.getObjectAcquire(tv, volatileObjectOffset),
          objectValue,
          "Unsafe.getObjectAcquire(Object, long)");
  }

  // Regression test for "copyMemory" operations hitting a DCHECK() for float/double arrays.
  private static void testCopyMemoryPrimitiveArrays(Unsafe unsafe) {
    int size = 4 * 1024;
    long memory = jdkUnsafeTestMalloc(size);

    int floatSize = 4;
    float[] inputFloats = new float[size / floatSize];
    for (int i = 0; i != inputFloats.length; ++i) {
      inputFloats[i] = ((float)i) + 0.5f;
    }
    float[] outputFloats = new float[size / floatSize];
    unsafe.copyMemoryFromPrimitiveArray(inputFloats, 0, memory, size);
    unsafe.copyMemoryToPrimitiveArray(memory, outputFloats, 0, size);
    for (int i = 0; i != inputFloats.length; ++i) {
      check(inputFloats[i], outputFloats[i], "unsafe.copyMemory/float");
    }

    int doubleSize = 8;
    double[] inputDoubles = new double[size / doubleSize];
    for (int i = 0; i != inputDoubles.length; ++i) {
      inputDoubles[i] = ((double)i) + 0.5;
    }
    double[] outputDoubles = new double[size / doubleSize];
    unsafe.copyMemoryFromPrimitiveArray(inputDoubles, 0, memory, size);
    unsafe.copyMemoryToPrimitiveArray(memory, outputDoubles, 0, size);
    for (int i = 0; i != inputDoubles.length; ++i) {
      check(inputDoubles[i], outputDoubles[i], "unsafe.copyMemory/double");
    }

    jdkUnsafeTestFree(memory);
  }

  private static class TestClass {
    public int intVar = 0;
    public long longVar = 0;
    public Object objectVar = null;
  }

  private static class TestVolatileClass {
    public volatile int volatileIntVar = 0;
    public volatile long volatileLongVar = 0;
    public volatile Object volatileObjectVar = null;
  }

  private static native int vmJdkArrayBaseOffset(Class<?> clazz);
  private static native int vmJdkArrayIndexScale(Class<?> clazz);
  private static native long jdkUnsafeTestMalloc(long size);
  private static native void jdkUnsafeTestFree(long memory);
}
