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

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;

public class Main {
  public static void main(String[] args) throws Exception {
    System.loadLibrary(args[1]);

    doTest();
    doFollowReferencesTest();
  }

  public static void doTest() throws Exception {
    setupGcCallback();

    enableGcTracking(true);
    run();
    enableGcTracking(false);
  }

  private static void run() {
    clearStats();
    forceGarbageCollection();
    printStats();
  }

  private static void clearStats() {
    getGcStarts();
    getGcFinishes();
  }

  private static void printStats() {
    System.out.println("---");
    int s = getGcStarts();
    int f = getGcFinishes();
    System.out.println((s > 0) + " " + (f > 0));
  }

  public static void doFollowReferencesTest() throws Exception {
    // Force GCs to clean up dirt.
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();

    setTag(Thread.currentThread(), 3000);

    {
      ArrayList<Object> tmpStorage = new ArrayList<>();
      doFollowReferencesTestNonRoot(tmpStorage);
      tmpStorage = null;
    }

    // Force GCs to clean up dirt.
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();

    doFollowReferencesTestRoot();

    // Force GCs to clean up dirt.
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
  }

  private static void doFollowReferencesTestNonRoot(ArrayList<Object> tmpStorage) {
    Verifier v = new Verifier();
    tagClasses(v);
    A a = createTree(v);
    tmpStorage.add(a);
    v.add("0@0", "1@1000");  // tmpStorage[0] --(array-element)--> a.

    doFollowReferencesTestImpl(null, Integer.MAX_VALUE, -1, null, v, null);
    doFollowReferencesTestImpl(a.foo, Integer.MAX_VALUE, -1, null, v, "2@1000");

    tmpStorage.clear();
  }

  private static void doFollowReferencesTestRoot() {
    Verifier v = new Verifier();
    tagClasses(v);
    A a = createTree(v);

    doFollowReferencesTestImpl(null, Integer.MAX_VALUE, -1, a, v, null);
    doFollowReferencesTestImpl(a.foo, Integer.MAX_VALUE, -1, a, v, "2@1000");
  }

  private static void doFollowReferencesTestImpl(A root, int stopAfter, int followSet,
      Object asRoot, Verifier v, String additionalEnabled) {
    String[] lines =
        followReferences(0, null, root, stopAfter, followSet, asRoot);

    v.process(lines, additionalEnabled);

    // TODO: Test filters.
  }

  private static void tagClasses(Verifier v) {
    setTag(A.class, 1000);

    setTag(B.class, 1001);
    v.add("1001@0", "1000@0");  // B.class --(superclass)--> A.class.

    setTag(C.class, 1002);
    v.add("1002@0", "1001@0");  // C.class --(superclass)--> B.class.
    v.add("1002@0", "2001@0");  // C.class --(interface)--> I2.class.

    setTag(I1.class, 2000);

    setTag(I2.class, 2001);
    v.add("2001@0", "2000@0");  // I2.class --(interface)--> I1.class.
  }

  private static A createTree(Verifier v) {
    A aInst = new A();
    setTag(aInst, 1);
    String aInstStr = "1@1000";
    String aClassStr = "1000@0";
    v.add(aInstStr, aClassStr);  // A -->(class) --> A.class.

    A a2Inst = new A();
    setTag(a2Inst, 2);
    aInst.foo = a2Inst;
    String a2InstStr = "2@1000";
    v.add(a2InstStr, aClassStr);  // A2 -->(class) --> A.class.
    v.add(aInstStr, a2InstStr);   // A -->(field) --> A2.

    B bInst = new B();
    setTag(bInst, 3);
    aInst.foo2 = bInst;
    String bInstStr = "3@1001";
    String bClassStr = "1001@0";
    v.add(bInstStr, bClassStr);  // B -->(class) --> B.class.
    v.add(aInstStr, bInstStr);   // A -->(field) --> B.

    A a3Inst = new A();
    setTag(a3Inst, 4);
    bInst.bar = a3Inst;
    String a3InstStr = "4@1000";
    v.add(a3InstStr, aClassStr);  // A3 -->(class) --> A.class.
    v.add(bInstStr, a3InstStr);   // B -->(field) --> A3.

    C cInst = new C();
    setTag(cInst, 5);
    bInst.bar2 = cInst;
    String cInstStr = "5@1000";
    String cClassStr = "1002@0";
    v.add(cInstStr, cClassStr);  // C -->(class) --> C.class.
    v.add(bInstStr, cInstStr);   // B -->(field) --> C.

    A a4Inst = new A();
    setTag(a4Inst, 6);
    cInst.baz = a4Inst;
    String a4InstStr = "6@1000";
    v.add(a4InstStr, aClassStr);  // A4 -->(class) --> A.class.
    v.add(cInstStr, a4InstStr);   // C -->(field) --> A4.

    cInst.baz2 = aInst;
    v.add(cInstStr, aInstStr);  // C -->(field) --> A.

    return aInst;
  }

  public static class A {
    public A foo;
    public A foo2;

    public A() {}
    public A(A a, A b) {
      foo = a;
      foo2 = b;
    }
  }

  public static class B extends A {
    public A bar;
    public A bar2;

    public B() {}
    public B(A a, A b) {
      bar = a;
      bar2 = b;
    }
  }

  public static interface I1 {
    public final static int i1Field = 1;
  }

  public static interface I2 extends I1 {
    public final static int i2Field = 2;
  }

  public static class C extends B implements I2 {
    public A baz;
    public A baz2;

    public C() {}
    public C(A a, A b) {
      baz = a;
      baz2 = b;
    }
  }

  public static class Verifier {
    public static class Node {
      public String referrer;

      public HashSet<String> referrees = new HashSet<>();

      public Node(String r) {
        referrer = r;
      }

      public boolean isRoot() {
        return referrer.startsWith("root@");
      }
    }

    HashMap<String, Node> nodes = new HashMap<>();

    public Verifier() {
    }

    public void add(String referrer, String referree) {
      if (!nodes.containsKey(referrer)) {
        nodes.put(referrer, new Node(referrer));
      }
      if (referree != null) {
        nodes.get(referrer).referrees.add(referree);
      }
    }

    public void process(String[] lines, String additionalEnabledReferrer) {
      // This method isn't optimal. The loops could be merged. However, it's more readable if
      // the different parts are separated.

      ArrayList<String> rootLines = new ArrayList<>();
      ArrayList<String> nonRootLines = new ArrayList<>();

      // Check for consecutive chunks of referrers. Also ensure roots come first.
      {
        String currentHead = null;
        boolean rootsDone = false;
        HashSet<String> completedReferrers = new HashSet<>();
        for (String l : lines) {
          String referrer = getReferrer(l);

          if (isRoot(referrer)) {
            if (rootsDone) {
              System.out.println("ERROR: Late root " + l);
              print(lines);
              return;
            }
            rootLines.add(l);
            continue;
          }

          rootsDone = true;

          if (currentHead == null) {
            currentHead = referrer;
          } else {
            if (!currentHead.equals(referrer)) {
              completedReferrers.add(currentHead);
              currentHead = referrer;
              if (completedReferrers.contains(referrer)) {
                System.out.println("Non-contiguous referrer " + l);
                print(lines);
                return;
              }
            }
          }
          nonRootLines.add(l);
        }
      }

      // Sort (root order is not specified) and print the roots.
      // TODO: What about extra roots? JNI and the interpreter seem to introduce those (though it
      //       isn't clear why a debuggable-AoT test doesn't have the same, at least for locals).
      //       For now, swallow duplicates, and resolve once we have the metadata for the roots.
      {
        Collections.sort(rootLines);
        String lastRoot = null;
        for (String l : rootLines) {
          if (lastRoot != null && lastRoot.equals(l)) {
            continue;
          }
          lastRoot = l;
          System.out.println(l);
        }
      }

      // Iterate through the lines, keeping track of which referrers are visited, to ensure the
      // order is acceptable.
      HashSet<String> enabled = new HashSet<>();
      if (additionalEnabledReferrer != null) {
        enabled.add(additionalEnabledReferrer);
      }
      // Always add "0@0".
      enabled.add("0@0");

      for (String l : lines) {
        String referrer = getReferrer(l);
        String referree = getReferree(l);
        if (isRoot(referrer)) {
          // For a root src, just enable the referree.
          enabled.add(referree);
        } else {
          // Check that the referrer is enabled (may be visited).
          if (!enabled.contains(referrer)) {
            System.out.println("Referrer " + referrer + " not enabled: " + l);
            print(lines);
            return;
          }
          enabled.add(referree);
        }
      }

      // Now just sort the non-root lines and output them
      Collections.sort(nonRootLines);
      for (String l : nonRootLines) {
        System.out.println(l);
      }

      System.out.println("---");
    }

    public static boolean isRoot(String ref) {
      return ref.startsWith("root@");
    }

    private static String getReferrer(String line) {
      int i = line.indexOf(" --");
      if (i <= 0) {
        throw new IllegalArgumentException(line);
      }
      int j = line.indexOf(' ');
      if (i != j) {
        throw new IllegalArgumentException(line);
      }
      return line.substring(0, i);
    }

    private static String getReferree(String line) {
      int i = line.indexOf("--> ");
      if (i <= 0) {
        throw new IllegalArgumentException(line);
      }
      int j = line.indexOf(' ', i + 4);
      if (j < 0) {
        throw new IllegalArgumentException(line);
      }
      return line.substring(i + 4, j);
    }

    private static void print(String[] lines) {
      for (String l : lines) {
        System.out.println(l);
      }
    }
  }

  private static native void setupGcCallback();
  private static native void enableGcTracking(boolean enable);
  private static native int getGcStarts();
  private static native int getGcFinishes();
  private static native void forceGarbageCollection();

  private static native void setTag(Object o, long tag);
  private static native long getTag(Object o);

  private static native String[] followReferences(int heapFilter, Class<?> klassFilter,
      Object initialObject, int stopAfter, int followSet, Object jniRef);
}
