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

interface Interface {
  void $noinline$f();
}

class Super implements Interface {
  public void $noinline$f() {
    throw new RuntimeException();
  }
}

class SubclassA extends Super {
  public void $noinline$f() {
    throw new RuntimeException();
  }

  public String $noinline$h() {
    throw new RuntimeException();
  }

  void $noinline$g() {
    throw new RuntimeException();
  }
}

class SubclassC extends SubclassA {
}

class SubclassB extends Super {
  public void $noinline$f() {
    throw new RuntimeException();
  }

  void $noinline$g() {
    throw new RuntimeException();
  }
}

class Generic<A> {
  private A a = null;
  public A get() {
    return a;
  }
}

final class Final {}

public class Main {

  /// CHECK-START: void Main.testSimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testSimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testSimpleRemove() {
    Super s = new SubclassA();
    ((SubclassA)s).$noinline$g();
  }

  /// CHECK-START: void Main.testSimpleKeep(Super) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testSimpleKeep(Super) instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  public void testSimpleKeep(Super s) {
    ((SubclassA)s).$noinline$f();
  }

  /// CHECK-START: java.lang.String Main.testClassRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: java.lang.String Main.testClassRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public String testClassRemove() {
    Object s = SubclassA.class;
    return ((Class)s).getName();
  }

  /// CHECK-START: java.lang.String Main.testClassKeep() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: java.lang.String Main.testClassKeep() instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  public String testClassKeep() {
    Object s = SubclassA.class;
    return ((SubclassA)s).$noinline$h();
  }

  /// CHECK-START: void Main.testIfRemove(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testIfRemove(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testIfRemove(int x) {
    Super s;
    if (x % 2 == 0) {
      s = new SubclassA();
    } else {
      s = new SubclassC();
    }
    ((SubclassA)s).$noinline$g();
  }

  /// CHECK-START: void Main.testIfKeep(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testIfKeep(int) instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  public void testIfKeep(int x) {
    Super s;
    if (x % 2 == 0) {
      s = new SubclassA();
    } else {
      s = new SubclassB();
    }
    ((SubclassA)s).$noinline$g();
  }

  /// CHECK-START: void Main.testForRemove(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testForRemove(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testForRemove(int x) {
    Super s = new SubclassA();
    for (int i = 0 ; i < x; i++) {
      if (x % 2 == 0) {
        s = new SubclassC();
      }
    }
    ((SubclassA)s).$noinline$g();
  }

  /// CHECK-START: void Main.testForKeep(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testForKeep(int) instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  public void testForKeep(int x) {
    Super s = new SubclassA();
    for (int i = 0 ; i < x; i++) {
      if (x % 2 == 0) {
        s = new SubclassC();
      }
    }
    ((SubclassC)s).$noinline$g();
  }

  /// CHECK-START: void Main.testPhiFromCall(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testPhiFromCall(int) instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  public void testPhiFromCall(int i) {
    Object x;
    if (i % 2 == 0) {
      x = new SubclassC();
    } else {
      x = newObject();  // this one will have an unknown type.
    }
    ((SubclassC)x).$noinline$g();
  }

  /// CHECK-START: void Main.testInstanceOf(java.lang.Object) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOf(java.lang.Object) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOf(Object o) {
    if (o instanceof SubclassC) {
      ((SubclassC)o).$noinline$g();
    }
    if (o instanceof SubclassB) {
      ((SubclassB)o).$noinline$g();
    }
  }

  /// CHECK-START: void Main.testInstanceOfKeep(java.lang.Object) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfKeep(java.lang.Object) instruction_simplifier_after_types (after)
  /// CHECK:         CheckCast
  /// CHECK:         CheckCast
  public void testInstanceOfKeep(Object o) {
    if (o instanceof SubclassC) {
      ((SubclassB)o).$noinline$g();
    }
    if (o instanceof SubclassB) {
      ((SubclassA)o).$noinline$g();
    }
  }

  /// CHECK-START: void Main.testInstanceOfNested(java.lang.Object) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfNested(java.lang.Object) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfNested(Object o) {
    if (o instanceof SubclassC) {
      if (o instanceof SubclassB) {
        ((SubclassB)o).$noinline$g();
      } else {
        ((SubclassC)o).$noinline$g();
      }
    }
  }

  /// CHECK-START: void Main.testInstanceOfWithPhi(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfWithPhi(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfWithPhi(int i) {
    Object o;
    if (i == 0) {
      o = new SubclassA();
    } else {
      o = new SubclassB();
    }

    if (o instanceof SubclassB) {
      ((SubclassB)o).$noinline$g();
    }
  }

  /// CHECK-START: void Main.testInstanceOfInFor(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfInFor(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfInFor(int n) {
    Object o = new SubclassA();
    for (int i = 0; i < n; i++) {
      if (i / 2 == 0) {
        o = new SubclassB();
      }
      if (o instanceof SubclassB) {
        ((SubclassB)o).$noinline$g();
      }
    }
  }

  /// CHECK-START: void Main.testInstanceOfSubclass() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfSubclass() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfSubclass() {
    Object o = new SubclassA();
    if (o instanceof Super) {
      ((SubclassA)o).$noinline$g();
    }
  }

  /// CHECK-START: void Main.testInstanceOfWithPhiSubclass(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfWithPhiSubclass(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfWithPhiSubclass(int i) {
    Object o;
    if (i == 0) {
      o = new SubclassA();
    } else {
      o = new SubclassC();
    }

    if (o instanceof Super) {
      ((SubclassA)o).$noinline$g();
    }
  }

  /// CHECK-START: void Main.testInstanceOfWithPhiTop(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfWithPhiTop(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfWithPhiTop(int i) {
    Object o;
    if (i == 0) {
      o = new Object();
    } else {
      o = new SubclassC();
    }

    if (o instanceof Super) {
      ((Super)o).$noinline$f();
    }
  }

  /// CHECK-START: void Main.testInstanceOfSubclassInFor(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfSubclassInFor(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfSubclassInFor(int n) {
    Object o = new SubclassA();
    for (int i = 0; i < n; i++) {
      if (o instanceof Super) {
        ((SubclassA)o).$noinline$g();
      }
      if (i / 2 == 0) {
        o = new SubclassC();
      }
    }
  }

  /// CHECK-START: void Main.testInstanceOfTopInFor(int) instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceOfTopInFor(int) instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceOfTopInFor(int n) {
    Object o = new SubclassA();
    for (int i = 0; i < n; i++) {
      if (o instanceof Super) {
        ((Super)o).$noinline$f();
      }
      if (i / 2 == 0) {
        o = new Object();
      }
    }
  }

  public Object newObject() {
    try {
      return Object.class.newInstance();
    } catch (Exception e) {
      return null;
    }
  }

  public SubclassA a = new SubclassA();
  public static SubclassA b = new SubclassA();

  /// CHECK-START: void Main.testInstanceFieldGetSimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInstanceFieldGetSimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInstanceFieldGetSimpleRemove() {
    Main m = new Main();
    Super a = m.a;
    ((SubclassA)a).$noinline$g();
  }

  /// CHECK-START: void Main.testStaticFieldGetSimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testStaticFieldGetSimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testStaticFieldGetSimpleRemove() {
    Super b = Main.b;
    ((SubclassA)b).$noinline$g();
  }

  public SubclassA $noinline$getSubclass() { throw new RuntimeException(); }

  /// CHECK-START: void Main.testArraySimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testArraySimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testArraySimpleRemove() {
    Super[] b = new SubclassA[10];
    SubclassA[] c = (SubclassA[])b;
  }

  /// CHECK-START: void Main.testInvokeSimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testInvokeSimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testInvokeSimpleRemove() {
    Super b = $noinline$getSubclass();
    ((SubclassA)b).$noinline$g();
  }
  /// CHECK-START: void Main.testArrayGetSimpleRemove() instruction_simplifier_after_types (before)
  /// CHECK:         CheckCast

  /// CHECK-START: void Main.testArrayGetSimpleRemove() instruction_simplifier_after_types (after)
  /// CHECK-NOT:     CheckCast
  public void testArrayGetSimpleRemove() {
    Super[] a = new SubclassA[10];
    ((SubclassA)a[0]).$noinline$g();
  }

  private Generic<SubclassC> genericC = new Generic<SubclassC>();
  private Generic<Final> genericFinal = new Generic<Final>();

  private SubclassC get() {
    return genericC.get();
  }

  private Final getFinal() {
    return genericFinal.get();
  }

  /// CHECK-START: SubclassC Main.inlineGenerics() reference_type_propagation (after)
  /// CHECK:      <<Invoke:l\d+>>    InvokeStaticOrDirect klass:SubclassC exact:false
  /// CHECK-NEXT:                    Return [<<Invoke>>]

  /// CHECK-START: SubclassC Main.inlineGenerics() reference_type_propagation_after_inlining (after)
  /// CHECK:      <<BoundType:l\d+>> BoundType klass:SubclassC exact:false
  /// CHECK:                         Return [<<BoundType>>]
  private SubclassC inlineGenerics() {
    SubclassC c = get();
    return c;
  }

  /// CHECK-START: Final Main.inlineGenericsFinal() reference_type_propagation (after)
  /// CHECK:      <<Invoke:l\d+>>    InvokeStaticOrDirect klass:Final exact:true
  /// CHECK-NEXT:                    Return [<<Invoke>>]

  /// CHECK-START: Final Main.inlineGenericsFinal() reference_type_propagation_after_inlining (after)
  /// CHECK:      <<BoundType:l\d+>> BoundType klass:Final exact:true
  /// CHECK:                         Return [<<BoundType>>]
  private Final inlineGenericsFinal() {
    Final f = getFinal();
    return f;
  }

  /// CHECK-START: void Main.boundOnlyOnceIfNotNull(java.lang.Object) reference_type_propagation_after_inlining (after)
  /// CHECK:      BoundType
  /// CHECK-NOT:  BoundType
  private void boundOnlyOnceIfNotNull(Object o) {
    if (o != null) {
      o.toString();
    }
  }

  /// CHECK-START: void Main.boundOnlyOnceIfInstanceOf(java.lang.Object) reference_type_propagation_after_inlining (after)
  /// CHECK:      BoundType
  /// CHECK-NOT:  BoundType
  private void boundOnlyOnceIfInstanceOf(Object o) {
    if (o instanceof Main) {
      o.toString();
    }
  }

  /// CHECK-START: Final Main.boundOnlyOnceCheckCast(Generic) reference_type_propagation_after_inlining (after)
  /// CHECK:      BoundType
  /// CHECK-NOT:  BoundType
  private Final boundOnlyOnceCheckCast(Generic<Final> o) {
    Final f = o.get();
    return f;
  }

  private Super getSuper() {
    return new SubclassA();
  }

  /// CHECK-START: void Main.updateNodesInTheSameBlockAsPhi(boolean) reference_type_propagation (after)
  /// CHECK:      <<Phi:l\d+>> Phi klass:Super
  /// CHECK:                   NullCheck [<<Phi>>] klass:Super

  /// CHECK-START: void Main.updateNodesInTheSameBlockAsPhi(boolean) reference_type_propagation_after_inlining (after)
  /// CHECK:      <<Phi:l\d+>> Phi klass:SubclassA
  /// CHECK:                   NullCheck [<<Phi>>] klass:SubclassA
  private void updateNodesInTheSameBlockAsPhi(boolean cond) {
    Super s = getSuper();
    if (cond) {
      s = new SubclassA();
    }
    s.$noinline$f();
  }

  public static void main(String[] args) {
  }
}
