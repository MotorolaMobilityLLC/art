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

import dalvik.system.VMDebug;
import java.io.IOException;
import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;
import libcore.util.NativeAllocationRegistry;
import org.apache.harmony.dalvik.ddmc.DdmVmInternal;

/**
 * Program used to create a heap dump for test purposes.
 */
public class Main {
  // Keep a reference to the DumpedStuff instance so that it is not garbage
  // collected before we take the heap dump.
  public static DumpedStuff stuff;

  public static class ObjectTree {
    public ObjectTree left;
    public ObjectTree right;

    public ObjectTree(ObjectTree left, ObjectTree right) {
      this.left = left;
      this.right = right;
    }
  }

  public static class AddedObject {
  }

  public static class RemovedObject {
  }

  public static class UnchangedObject {
  }

  public static class ModifiedObject {
    public int value;
    public String modifiedRefField;
    public String unmodifiedRefField;
  }

  public static class StackSmasher {
    public StackSmasher child;
  }

  public static class Reference {
    public Object referent;

    public Reference(Object referent) {
      this.referent = referent;
    }
  }

  // We will take a heap dump that includes a single instance of this
  // DumpedStuff class. Objects stored as fields in this class can be easily
  // found in the hprof dump by searching for the instance of the DumpedStuff
  // class and reading the desired field.
  public static class DumpedStuff {
    public String basicString = "hello, world";
    public String nonAscii = "Sigma (Ʃ) is not ASCII";
    public String embeddedZero = "embedded\0...";  // Non-ASCII for string compression purposes.
    public char[] charArray = "char thing".toCharArray();
    public String nullString = null;
    public Object anObject = new Object();
    public Reference aReference = new Reference(anObject);
    public ReferenceQueue<Object> referenceQueue = new ReferenceQueue<Object>();
    public PhantomReference aPhantomReference = new PhantomReference(anObject, referenceQueue);
    public WeakReference aWeakReference = new WeakReference(anObject, referenceQueue);
    public WeakReference aNullReferentReference = new WeakReference(null, referenceQueue);
    public SoftReference aSoftReference = new SoftReference(new Object());
    public byte[] bigArray;
    public ObjectTree[] gcPathArray = new ObjectTree[]{null, null,
      new ObjectTree(
          new ObjectTree(null, new ObjectTree(null, null)),
          new ObjectTree(null, null)),
      null};
    public Reference aLongStrongPathToSamplePathObject;
    public WeakReference aShortWeakPathToSamplePathObject;
    public Object[] basicStringRef;
    public AddedObject addedObject;
    public UnchangedObject unchangedObject = new UnchangedObject();
    public RemovedObject removedObject;
    public ModifiedObject modifiedObject;
    public StackSmasher stackSmasher;
    public StackSmasher stackSmasherAdded;
    public static String modifiedStaticField;
    public int[] modifiedArray;
    public Object objectAllocatedAtKnownSite1;
    public Object objectAllocatedAtKnownSite2;

    private void allocateObjectAtKnownSite1() {
      objectAllocatedAtKnownSite1 = new Object();
      allocateObjectAtKnownSite2();
    }

    private void allocateObjectAtKnownSite2() {
      objectAllocatedAtKnownSite2 = new Object();
    }

    DumpedStuff(boolean baseline) {
      int n = baseline ? 400000 : 1000000;
      bigArray = new byte[n];
      for (int i = 0; i < n; i++) {
        bigArray[i] = (byte)((i * i) & 0xFF);
      }

      // 0x12345, 50000, and 0xABCDABCD are arbitrary values.
      NativeAllocationRegistry registry = new NativeAllocationRegistry(
          Main.class.getClassLoader(), 0x12345, 50000);
      registry.registerNativeAllocation(anObject, 0xABCDABCD);

      aLongStrongPathToSamplePathObject = new Reference(new Reference(new Object()));
      aShortWeakPathToSamplePathObject = new WeakReference(
          ((Reference)aLongStrongPathToSamplePathObject.referent).referent,
          referenceQueue);

      addedObject = baseline ? null : new AddedObject();
      removedObject = baseline ? new RemovedObject() : null;
      modifiedObject = new ModifiedObject();
      modifiedObject.value = baseline ? 5 : 8;
      modifiedObject.modifiedRefField = baseline ? "A1" : "A2";
      modifiedObject.unmodifiedRefField = "B";
      modifiedStaticField = baseline ? "C1" : "C2";
      modifiedArray = baseline ? new int[]{0, 1, 2, 3} : new int[]{3, 1, 2, 0};

      allocateObjectAtKnownSite1();

      // Deep matching dominator trees shouldn't smash the stack when we try
      // to diff them. Make some deep dominator trees to help test it.
      for (int i = 0; i < 10000; i++) {
        StackSmasher smasher = new StackSmasher();
        smasher.child = stackSmasher;
        stackSmasher = smasher;

        if (!baseline) {
          smasher = new StackSmasher();
          smasher.child = stackSmasherAdded;
          stackSmasherAdded = smasher;
        }
      }

      gcPathArray[2].right.left = gcPathArray[2].left.right;
    }
  }

  public static void main(String[] args) throws IOException {
    if (args.length < 1) {
      System.err.println("no output file specified");
      return;
    }
    String file = args[0];

    // If a --base argument is provided, it means we should generate a
    // baseline hprof file suitable for using in testing diff.
    boolean baseline = args.length > 1 && args[1].equals("--base");

    // Enable allocation tracking so we get stack traces in the heap dump.
    DdmVmInternal.enableRecentAllocations(true);

    // Allocate the instance of DumpedStuff.
    stuff = new DumpedStuff(baseline);

    // Create a bunch of unreachable objects pointing to basicString for the
    // reverseReferencesAreNotUnreachable test
    for (int i = 0; i < 100; i++) {
      stuff.basicStringRef = new Object[]{stuff.basicString};
    }

    // Take a heap dump that will include that instance of DumpedStuff.
    System.err.println("Dumping hprof data to " + file);
    VMDebug.dumpHprofData(file);
  }
}
