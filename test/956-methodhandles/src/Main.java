/*
 * Copyright (C) 2016 The Android Open Source Project
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

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodHandles.Lookup;
import java.lang.invoke.MethodType;
import java.lang.invoke.WrongMethodTypeException;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class Main {

  public static class A {
    public void foo() {
      System.out.println("foo_A");
    }

    public static final Lookup lookup = MethodHandles.lookup();
  }

  public static class B extends A {
    public void foo() {
      System.out.println("foo_B");
    }

    public static final Lookup lookup = MethodHandles.lookup();
  }

  public static class C extends B {
    public static final Lookup lookup = MethodHandles.lookup();
  }

  public static class D {
    private final void privateRyan() {
      System.out.println("privateRyan_D");
    }

    public static final Lookup lookup = MethodHandles.lookup();
  }

  public static class E extends D {
    public static final Lookup lookup = MethodHandles.lookup();
  }

  public static void main(String[] args) throws Throwable {
    testfindSpecial_invokeSuperBehaviour();
    testfindSpecial_invokeDirectBehaviour();
    testExceptionDetailMessages();
    testfindVirtual();
    testUnreflects();
    testAsType();
    testConstructors();
    testStringConstructors();
    testReturnValueConversions();
  }

  public static void testfindSpecial_invokeSuperBehaviour() throws Throwable {
    // This is equivalent to an invoke-super instruction where the referrer
    // is B.class.
    MethodHandle mh1 = B.lookup.findSpecial(A.class /* refC */, "foo",
        MethodType.methodType(void.class), B.class /* specialCaller */);

    A aInstance = new A();
    B bInstance = new B();
    C cInstance = new C();

    // This should be as if an invoke-super was called from one of B's methods.
    mh1.invokeExact(bInstance);
    mh1.invoke(bInstance);

    // This should not work. The receiver type in the handle will be suitably
    // restricted to B and subclasses.
    try {
      mh1.invoke(aInstance);
      System.out.println("mh1.invoke(aInstance) should not succeeed");
    } catch (ClassCastException expected) {
    }

    try {
      mh1.invokeExact(aInstance);
      System.out.println("mh1.invoke(aInstance) should not succeeed");
    } catch (WrongMethodTypeException expected) {
    }

    // This should *still* be as if an invoke-super was called from one of C's
    // methods, despite the fact that we're operating on a C.
    mh1.invoke(cInstance);

    // Now that C is the special caller, the next invoke will call B.foo.
    MethodHandle mh2 = C.lookup.findSpecial(A.class /* refC */, "foo",
        MethodType.methodType(void.class), C.class /* specialCaller */);
    mh2.invokeExact(cInstance);

    // Shouldn't allow invoke-super semantics from an unrelated special caller.
    try {
      C.lookup.findSpecial(A.class, "foo",
        MethodType.methodType(void.class), D.class /* specialCaller */);
      System.out.println("findSpecial(A.class, foo, .. D.class) unexpectedly succeeded.");
    } catch (IllegalAccessException expected) {
    }
  }

  public static void testfindSpecial_invokeDirectBehaviour() throws Throwable {
    D dInstance = new D();

    MethodHandle mh3 = D.lookup.findSpecial(D.class, "privateRyan",
        MethodType.methodType(void.class), D.class /* specialCaller */);
    mh3.invoke(dInstance);

    // The private method shouldn't be accessible from any special caller except
    // itself...
    try {
      D.lookup.findSpecial(D.class, "privateRyan", MethodType.methodType(void.class), C.class);
      System.out.println("findSpecial(privateRyan, C.class) unexpectedly succeeded");
    } catch (IllegalAccessException expected) {
    }

    // ... or from any lookup context except its own.
    try {
      E.lookup.findSpecial(D.class, "privateRyan", MethodType.methodType(void.class), E.class);
      System.out.println("findSpecial(privateRyan, E.class) unexpectedly succeeded");
    } catch (IllegalAccessException expected) {
    }
  }

  public static void testExceptionDetailMessages() throws Throwable {
    MethodHandle handle = MethodHandles.lookup().findVirtual(String.class, "concat",
        MethodType.methodType(String.class, String.class));

    try {
      handle.invokeExact("a", new Object());
      System.out.println("invokeExact(\"a\", new Object()) unexpectedly succeeded.");
    } catch (WrongMethodTypeException ex) {
      System.out.println("Received exception: " + ex.getMessage());
    }
  }

  public interface Foo {
    public String foo();
  }

  public interface Bar extends Foo {
    public String bar();
  }

  public static class BarSuper {
    public String superPublicMethod() {
      return "superPublicMethod";
    }

    public String superProtectedMethod() {
      return "superProtectedMethod";
    }

    String superPackageMethod() {
      return "superPackageMethod";
    }
  }

  public static class BarImpl extends BarSuper implements Bar {
    public BarImpl() {
    }

    @Override
    public String foo() {
      return "foo";
    }

    @Override
    public String bar() {
      return "bar";
    }

    private String privateMethod() { return "privateMethod"; }

    public static String staticMethod() { return null; }

    static final MethodHandles.Lookup lookup = MethodHandles.lookup();
  }

  public static void testfindVirtual() throws Throwable {
    // Virtual lookups on static methods should not succeed.
    try {
        MethodHandles.lookup().findVirtual(
            BarImpl.class,  "staticMethod", MethodType.methodType(String.class));
        System.out.println("findVirtual(staticMethod) unexpectedly succeeded");
    } catch (IllegalAccessException expected) {
    }

    // Virtual lookups on private methods should not succeed, unless the Lookup
    // context had sufficient privileges.
    try {
        MethodHandles.lookup().findVirtual(
            BarImpl.class,  "privateMethod", MethodType.methodType(String.class));
        System.out.println("findVirtual(privateMethod) unexpectedly succeeded");
    } catch (IllegalAccessException expected) {
    }

    // Virtual lookup on a private method with a context that *does* have sufficient
    // privileges.
    MethodHandle mh = BarImpl.lookup.findVirtual(
            BarImpl.class,  "privateMethod", MethodType.methodType(String.class));
    String str = (String) mh.invoke(new BarImpl());
    if (!"privateMethod".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#privateMethod: " + str);
    }

    // Find virtual must find interface methods defined by interfaces implemented
    // by the class.
    mh = MethodHandles.lookup().findVirtual(BarImpl.class, "foo",
        MethodType.methodType(String.class));
    str = (String) mh.invoke(new BarImpl());
    if (!"foo".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#foo: " + str);
    }

    // .. and their super-interfaces.
    mh = MethodHandles.lookup().findVirtual(BarImpl.class, "bar",
        MethodType.methodType(String.class));
    str = (String) mh.invoke(new BarImpl());
    if (!"bar".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#bar: " + str);
    }

    // TODO(narayan): Fix this case, we're using the wrong ArtMethod for the
    // invoke resulting in a failing check in the interpreter.
    //
    // mh = MethodHandles.lookup().findVirtual(Bar.class, "bar",
    //    MethodType.methodType(String.class));
    // str = (String) mh.invoke(new BarImpl());
    // if (!"bar".equals(str)) {
    //   System.out.println("Unexpected return value for BarImpl#bar: " + str);
    // }

    // We should also be able to lookup public / protected / package methods in
    // the super class, given sufficient access privileges.
    mh = MethodHandles.lookup().findVirtual(BarImpl.class, "superPublicMethod",
        MethodType.methodType(String.class));
    str = (String) mh.invoke(new BarImpl());
    if (!"superPublicMethod".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#superPublicMethod: " + str);
    }

    mh = MethodHandles.lookup().findVirtual(BarImpl.class, "superProtectedMethod",
        MethodType.methodType(String.class));
    str = (String) mh.invoke(new BarImpl());
    if (!"superProtectedMethod".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#superProtectedMethod: " + str);
    }

    mh = MethodHandles.lookup().findVirtual(BarImpl.class, "superPackageMethod",
        MethodType.methodType(String.class));
    str = (String) mh.invoke(new BarImpl());
    if (!"superPackageMethod".equals(str)) {
      System.out.println("Unexpected return value for BarImpl#superPackageMethod: " + str);
    }
  }

  static class UnreflectTester {
    public String publicField;
    private String privateField;

    public static String publicStaticField = "publicStaticValue";
    private static String privateStaticField = "privateStaticValue";

    private UnreflectTester(String val) {
      publicField = val;
      privateField = val;
    }

    // NOTE: The boolean constructor argument only exists to give this a
    // different signature.
    public UnreflectTester(String val, boolean unused) {
      this(val);
    }

    private static String privateStaticMethod() {
      return "privateStaticMethod";
    }

    private String privateMethod() {
      return "privateMethod";
    }

    public static String publicStaticMethod() {
      return "publicStaticMethod";
    }

    public String publicMethod() {
      return "publicMethod";
    }
  }

  public static void testUnreflects() throws Throwable {
    UnreflectTester instance = new UnreflectTester("unused");
    Method publicMethod = UnreflectTester.class.getMethod("publicMethod");

    MethodHandle mh = MethodHandles.lookup().unreflect(publicMethod);
    assertEquals("publicMethod", (String) mh.invoke(instance));
    assertEquals("publicMethod", (String) mh.invokeExact(instance));

    Method publicStaticMethod = UnreflectTester.class.getMethod("publicStaticMethod");
    mh = MethodHandles.lookup().unreflect(publicStaticMethod);
    assertEquals("publicStaticMethod", (String) mh.invoke());
    assertEquals("publicStaticMethod", (String) mh.invokeExact());

    Method privateMethod = UnreflectTester.class.getDeclaredMethod("privateMethod");
    try {
      mh = MethodHandles.lookup().unreflect(privateMethod);
      fail();
    } catch (IllegalAccessException expected) {}

    privateMethod.setAccessible(true);
    mh = MethodHandles.lookup().unreflect(privateMethod);
    assertEquals("privateMethod", (String) mh.invoke(instance));
    assertEquals("privateMethod", (String) mh.invokeExact(instance));

    Method privateStaticMethod = UnreflectTester.class.getDeclaredMethod("privateStaticMethod");
    try {
      mh = MethodHandles.lookup().unreflect(privateStaticMethod);
      fail();
    } catch (IllegalAccessException expected) {}

    privateStaticMethod.setAccessible(true);
    mh = MethodHandles.lookup().unreflect(privateStaticMethod);
    assertEquals("privateStaticMethod", (String) mh.invoke());
    assertEquals("privateStaticMethod", (String) mh.invokeExact());

    Constructor privateConstructor = UnreflectTester.class.getDeclaredConstructor(String.class);
    try {
      mh = MethodHandles.lookup().unreflectConstructor(privateConstructor);
      fail();
    } catch (IllegalAccessException expected) {}

    privateConstructor.setAccessible(true);
    mh = MethodHandles.lookup().unreflectConstructor(privateConstructor);
    instance = (UnreflectTester) mh.invokeExact("abc");
    assertEquals("abc", instance.publicField);
    instance = (UnreflectTester) mh.invoke("def");
    assertEquals("def", instance.publicField);
    Constructor publicConstructor = UnreflectTester.class.getConstructor(String.class,
        boolean.class);
    mh = MethodHandles.lookup().unreflectConstructor(publicConstructor);
    instance = (UnreflectTester) mh.invokeExact("abc", false);
    assertEquals("abc", instance.publicField);
    instance = (UnreflectTester) mh.invoke("def", true);
    assertEquals("def", instance.publicField);

    // TODO(narayan): Non exact invokes for field sets/gets are not implemented yet.
    //
    // assertEquals("instanceValue", (String) mh.invoke(new UnreflectTester("instanceValue")));
    Field publicField = UnreflectTester.class.getField("publicField");
    mh = MethodHandles.lookup().unreflectGetter(publicField);
    instance = new UnreflectTester("instanceValue");
    assertEquals("instanceValue", (String) mh.invokeExact(instance));

    mh = MethodHandles.lookup().unreflectSetter(publicField);
    instance = new UnreflectTester("instanceValue");
    mh.invokeExact(instance, "updatedInstanceValue");
    assertEquals("updatedInstanceValue", instance.publicField);

    Field publicStaticField = UnreflectTester.class.getField("publicStaticField");
    mh = MethodHandles.lookup().unreflectGetter(publicStaticField);
    UnreflectTester.publicStaticField = "updatedStaticValue";
    assertEquals("updatedStaticValue", (String) mh.invokeExact());

    mh = MethodHandles.lookup().unreflectSetter(publicStaticField);
    UnreflectTester.publicStaticField = "updatedStaticValue";
    mh.invokeExact("updatedStaticValue2");
    assertEquals("updatedStaticValue2", UnreflectTester.publicStaticField);

    Field privateField = UnreflectTester.class.getDeclaredField("privateField");
    try {
      mh = MethodHandles.lookup().unreflectGetter(privateField);
      fail();
    } catch (IllegalAccessException expected) {
    }
    try {
      mh = MethodHandles.lookup().unreflectSetter(privateField);
      fail();
    } catch (IllegalAccessException expected) {
    }

    privateField.setAccessible(true);

    mh = MethodHandles.lookup().unreflectGetter(privateField);
    instance = new UnreflectTester("instanceValue");
    assertEquals("instanceValue", (String) mh.invokeExact(instance));

    mh = MethodHandles.lookup().unreflectSetter(privateField);
    instance = new UnreflectTester("instanceValue");
    mh.invokeExact(instance, "updatedInstanceValue");
    assertEquals("updatedInstanceValue", instance.privateField);

    Field privateStaticField = UnreflectTester.class.getDeclaredField("privateStaticField");
    try {
      mh = MethodHandles.lookup().unreflectGetter(privateStaticField);
      fail();
    } catch (IllegalAccessException expected) {
    }
    try {
      mh = MethodHandles.lookup().unreflectSetter(privateStaticField);
      fail();
    } catch (IllegalAccessException expected) {
    }

    privateStaticField.setAccessible(true);
    mh = MethodHandles.lookup().unreflectGetter(privateStaticField);
    privateStaticField.set(null, "updatedStaticValue");
    assertEquals("updatedStaticValue", (String) mh.invokeExact());

    mh = MethodHandles.lookup().unreflectSetter(privateStaticField);
    privateStaticField.set(null, "updatedStaticValue");
    mh.invokeExact("updatedStaticValue2");
    assertEquals("updatedStaticValue2", (String) privateStaticField.get(null));
  }

  // This method only exists to fool Jack's handling of types. See b/32536744.
  public static CharSequence getSequence() {
    return "foo";
  }

  public static void testAsType() throws Throwable {
    // The type of this handle is (String, String)String.
    MethodHandle mh = MethodHandles.lookup().findVirtual(String.class,
        "concat", MethodType.methodType(String.class, String.class));

    // Change it to (CharSequence, String)Object.
    MethodHandle asType = mh.asType(
        MethodType.methodType(Object.class, CharSequence.class, String.class));

    Object obj = asType.invokeExact((CharSequence) getSequence(), "bar");
    assertEquals("foobar", (String) obj);

    // Should fail due to a wrong return type.
    try {
      String str = (String) asType.invokeExact((CharSequence) getSequence(), "bar");
      fail();
    } catch (WrongMethodTypeException expected) {
    }

    // Should fail due to a wrong argument type (String instead of Charsequence).
    try {
      String str = (String) asType.invokeExact("baz", "bar");
      fail();
    } catch (WrongMethodTypeException expected) {
    }

    // Calls to asType should fail if the types are not convertible.
    //
    // Bad return type conversion.
    try {
      mh.asType(MethodType.methodType(int.class, String.class, String.class));
      fail();
    } catch (WrongMethodTypeException expected) {
    }

    // Bad argument conversion.
    try {
      mh.asType(MethodType.methodType(String.class, int.class, String.class));
      fail();
    } catch (WrongMethodTypeException expected) {
    }
  }

  public static void assertEquals(String s1, String s2) {
    if (s1 == s2) {
      return;
    }

    if (s1 != null && s2 != null && s1.equals(s2)) {
      return;
    }

    throw new AssertionError("assertEquals s1: " + s1 + ", s2: " + s2);
  }

  public static void fail() {
    System.out.println("fail");
    Thread.dumpStack();
  }

  public static void fail(String message) {
    System.out.println("fail: " + message);
    Thread.dumpStack();
  }

  public static void testConstructors() throws Throwable {
    MethodHandle mh =
        MethodHandles.lookup().findConstructor(Float.class,
                                               MethodType.methodType(void.class,
                                                                     float.class));
    Float value = (Float) mh.invokeExact(0.33f);
    if (value.floatValue() != 0.33f) {
      fail("Unexpected float value from invokeExact " + value.floatValue());
    }

    value = (Float) mh.invoke(3.34f);
    if (value.floatValue() != 3.34f) {
      fail("Unexpected float value from invoke " + value.floatValue());
    }

    mh = MethodHandles.lookup().findConstructor(Double.class,
                                                MethodType.methodType(void.class, String.class));
    Double d = (Double) mh.invoke("8.45e3");
    if (d.doubleValue() != 8.45e3) {
      fail("Unexpected double value from Double(String) " + value.doubleValue());
    }

    mh = MethodHandles.lookup().findConstructor(Double.class,
                                                MethodType.methodType(void.class, double.class));
    d = (Double) mh.invoke(8.45e3);
    if (d.doubleValue() != 8.45e3) {
      fail("Unexpected double value from Double(double) " + value.doubleValue());
    }

    // Primitive type
    try {
      mh = MethodHandles.lookup().findConstructor(int.class, MethodType.methodType(void.class));
      fail("Unexpected lookup success for primitive constructor");
    } catch (NoSuchMethodException e) {}

    // Interface
    try {
      mh = MethodHandles.lookup().findConstructor(Readable.class,
                                                  MethodType.methodType(void.class));
      fail("Unexpected lookup success for interface constructor");
    } catch (NoSuchMethodException e) {}

    // Abstract
    mh = MethodHandles.lookup().findConstructor(Process.class, MethodType.methodType(void.class));
    try {
      mh.invoke();
      fail("Unexpected ability to instantiate an abstract class");
    } catch (InstantiationException e) {}

    // Non-existent
    try {
        MethodHandle bad = MethodHandles.lookup().findConstructor(
            String.class, MethodType.methodType(String.class, Float.class));
        fail("Unexpected success for non-existent constructor");
    } catch (NoSuchMethodException e) {}

    // Non-void constructor search. (I)I instead of (I)V.
    try {
        MethodHandle foo = MethodHandles.lookup().findConstructor(
            Integer.class, MethodType.methodType(Integer.class, Integer.class));
        fail("Unexpected success for non-void type for findConstructor");
    } catch (NoSuchMethodException e) {}
  }

  public static void testStringConstructors() throws Throwable {
    final String testPattern = "The system as we know it is broken";

    // String()
    MethodHandle mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class));
    String s = (String) mh.invokeExact();
    if (!s.equals("")) {
      fail("Unexpected empty string constructor result: '" + s + "'");
    }

    // String(String)
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, String.class));
    s = (String) mh.invokeExact(testPattern);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(char[])
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, char[].class));
    s = (String) mh.invokeExact(testPattern.toCharArray());
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(char[], int, int)
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, char[].class, int.class, int.class));
    s = (String) mh.invokeExact(new char [] { 'a', 'b', 'c', 'd', 'e'}, 2, 3);
    if (!s.equals("cde")) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(int[] codePoints, int offset, int count)
    StringBuffer sb = new StringBuffer(testPattern);
    int[] codePoints = new int[sb.codePointCount(0, sb.length())];
    for (int i = 0; i < sb.length(); ++i) {
      codePoints[i] = sb.codePointAt(i);
    }
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, int[].class, int.class, int.class));
    s = (String) mh.invokeExact(codePoints, 0, codePoints.length);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte ascii[], int hibyte, int offset, int count)
    byte [] ascii = testPattern.getBytes(StandardCharsets.US_ASCII);
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, byte[].class, int.class, int.class));
    s = (String) mh.invokeExact(ascii, 0, ascii.length);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[], int offset, int length, String charsetName)
    mh = MethodHandles.lookup().findConstructor(
        String.class,
        MethodType.methodType(void.class, byte[].class, int.class, int.class, String.class));
    s = (String) mh.invokeExact(ascii, 0, 5, StandardCharsets.US_ASCII.name());
    if (!s.equals(testPattern.substring(0, 5))) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[], int offset, int length, Charset charset)
    mh = MethodHandles.lookup().findConstructor(
        String.class,
        MethodType.methodType(void.class, byte[].class, int.class, int.class, Charset.class));
    s = (String) mh.invokeExact(ascii, 0, 5, StandardCharsets.US_ASCII);
    if (!s.equals(testPattern.substring(0, 5))) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[], String charsetName)
    mh = MethodHandles.lookup().findConstructor(
        String.class,
        MethodType.methodType(void.class, byte[].class, String.class));
    s = (String) mh.invokeExact(ascii, StandardCharsets.US_ASCII.name());
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[], Charset charset)
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, byte[].class, Charset.class));
    s = (String) mh.invokeExact(ascii, StandardCharsets.US_ASCII);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[], int offset, int length)
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, byte[].class, int.class, int.class));
    s = (String) mh.invokeExact(ascii, 1, ascii.length - 2);
    s = testPattern.charAt(0) + s + testPattern.charAt(testPattern.length() - 1);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(byte bytes[])
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, byte[].class));
    s = (String) mh.invokeExact(ascii);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    // String(StringBuffer buffer)
    mh = MethodHandles.lookup().findConstructor(
        String.class, MethodType.methodType(void.class, StringBuffer.class));
    s = (String) mh.invokeExact(sb);
    if (!s.equals(testPattern)) {
      fail("Unexpected string constructor result: '" + s + "'");
    }

    System.out.println("String constructors done.");
  }

  private static void testReferenceReturnValueConversions() throws Throwable {
    MethodHandle mh = MethodHandles.lookup().findStatic(
        Float.class, "valueOf", MethodType.methodType(Float.class, String.class));

    // No conversion
    Float f = (Float) mh.invokeExact("1.375");
    if (f.floatValue() != 1.375) {
      fail();
    }
    f = (Float) mh.invoke("1.875");
    if (f.floatValue() != 1.875) {
      fail();
    }

    // Bad conversion
    try {
      int i = (int) mh.invokeExact("7.77");
      fail();
    } catch (WrongMethodTypeException e) {}

    try {
      int i = (int) mh.invoke("7.77");
      fail();
    } catch (WrongMethodTypeException e) {}

    // Assignment to super-class.
    Number n = (Number) mh.invoke("1.11");
    try {
      Number o = (Number) mh.invokeExact("1.11");
      fail();
    } catch (WrongMethodTypeException e) {}

    // Assignment to widened boxed primitive class.
    try {
      Double u = (Double) mh.invoke("1.11");
      fail();
    } catch (ClassCastException e) {}

    try {
      Double v = (Double) mh.invokeExact("1.11");
      fail();
    } catch (WrongMethodTypeException e) {}

    // Unboxed
    float p = (float) mh.invoke("1.11");
    if (p != 1.11f) {
      fail();
    }

    // Unboxed and widened
    double d = (double) mh.invoke("2.5");
    if (d != 2.5) {
      fail();
    }

    // Interface
    Comparable<Float> c = (Comparable<Float>) mh.invoke("2.125");
    if (c.compareTo(new Float(2.125f)) != 0) {
      fail();
    }

    System.out.println("testReferenceReturnValueConversions done.");
  }

  private static void testPrimitiveReturnValueConversions() throws Throwable {
    MethodHandle mh = MethodHandles.lookup().findStatic(
        Math.class, "min", MethodType.methodType(int.class, int.class, int.class));

    final int SMALL = -8972;
    final int LARGE = 7932529;

    // No conversion
    if ((int) mh.invokeExact(LARGE, SMALL) != SMALL) {
      fail();
    } else if ((int) mh.invoke(LARGE, SMALL) != SMALL) {
      fail();
    } else if ((int) mh.invokeExact(SMALL, LARGE) != SMALL) {
      fail();
    } else if ((int) mh.invoke(SMALL, LARGE) != SMALL) {
      fail();
    }

    // int -> long
    try {
      if ((long) mh.invokeExact(LARGE, SMALL) != (long) SMALL) {}
        fail();
    } catch (WrongMethodTypeException e) {}

    if ((long) mh.invoke(LARGE, SMALL) != (long) SMALL) {
      fail();
    }

    // int -> short
    try {
      if ((short) mh.invokeExact(LARGE, SMALL) != (short) SMALL) {}
      fail();
    } catch (WrongMethodTypeException e) {}

    try {
      if ((short) mh.invoke(LARGE, SMALL) != (short) SMALL) {
        fail();
      }
    } catch (WrongMethodTypeException e) {}

    // int -> Integer
    try {
      if (!((Integer) mh.invokeExact(LARGE, SMALL)).equals(new Integer(SMALL))) {}
      fail();
    } catch (WrongMethodTypeException e) {}

    if (!((Integer) mh.invoke(LARGE, SMALL)).equals(new Integer(SMALL))) {
      fail();
    }

    // int -> Long
    try {
      Long l = (Long) mh.invokeExact(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    try {
      Long l = (Long) mh.invoke(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    // int -> Short
    try {
      Short s = (Short) mh.invokeExact(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    try {
      Short s = (Short) mh.invoke(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    // int -> Process
    try {
      Process p = (Process) mh.invokeExact(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    try {
      Process p = (Process) mh.invoke(LARGE, SMALL);
      fail();
    } catch (WrongMethodTypeException e) {}

    // void -> Object
    mh = MethodHandles.lookup().findStatic(System.class, "gc", MethodType.methodType(void.class));
    Object o = (Object) mh.invoke();
    if (o != null) fail();

    // void -> long
    long l = (long) mh.invoke();
    if (l != 0) fail();

    System.out.println("testPrimitiveReturnValueConversions done.");
  }

  public static void testReturnValueConversions() throws Throwable {
    testReferenceReturnValueConversions();
    testPrimitiveReturnValueConversions();
  }
}
