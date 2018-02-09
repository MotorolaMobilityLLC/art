/*
 * Copyright (C) 2009 The Android Open Source Project
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

import other.Mutant;
import other.InaccessibleClass;
import other.InaccessibleMethod;

/**
 * Test some problematic situations that the verifier detects.
 */
public class Main {
    public static final boolean VERBOSE = false;

    public static void main(String[] args) {
        testClassNewInstance();
        testMissingStuff();
        testBadAccess();
        testBadInterfaceMethod();
     }
    /**
     * Try to create and invoke a non-existent interface method.
     */
    static void testBadInterfaceMethod() {
        BadInterface badiface = new BadIfaceImpl();
        try {
            badiface.internalClone();
        } catch (IncompatibleClassChangeError icce) {
            // TODO b/64274113 This should really be an NSME
            System.out.println("Got expected IncompatibleClassChangeError (interface)");
            if (VERBOSE) System.out.println("--- " + icce);
        }
    }

    /**
     * Try to create a new instance of an abstract class.
     */
    static void testClassNewInstance() {
        try {
            MaybeAbstract ma = new MaybeAbstract();
            System.out.println("ERROR: MaybeAbstract succeeded unexpectedly");
        } catch (InstantiationError ie) {
            System.out.println("Got expected InstantationError");
            if (VERBOSE) System.out.println("--- " + ie);
        } catch (Exception ex) {
            System.out.println("Got unexpected MaybeAbstract failure");
        }
    }

    /**
     * Test stuff that disappears.
     */
    static void testMissingStuff() {
        Mutant mutant = new Mutant();

        try {
            int x = mutant.disappearingField;
        } catch (NoSuchFieldError nsfe) {
            System.out.println("Got expected NoSuchFieldError");
            if (VERBOSE) System.out.println("--- " + nsfe);
        }

        try {
            int y = Mutant.disappearingStaticField;
        } catch (NoSuchFieldError nsfe) {
            System.out.println("Got expected NoSuchFieldError");
            if (VERBOSE) System.out.println("--- " + nsfe);
        }

        try {
            mutant.disappearingMethod();
        } catch (NoSuchMethodError nsme) {
            System.out.println("Got expected NoSuchMethodError");
            if (VERBOSE) System.out.println("--- " + nsme);
        }

        try {
            Mutant.disappearingStaticMethod();
        } catch (NoSuchMethodError nsme) {
            System.out.println("Got expected NoSuchMethodError");
            if (VERBOSE) System.out.println("--- " + nsme);
        }
    }

    /**
     * Test stuff that becomes inaccessible.
     */
    static void testBadAccess() {
        Mutant mutant = new Mutant();

        try {
            int x = mutant.inaccessibleField;
            System.out.println("ERROR: bad access succeeded (ifield)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (ifield)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            int y = Mutant.inaccessibleStaticField;
            System.out.println("ERROR: bad access succeeded (sfield)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (sfield)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            mutant.inaccessibleMethod();
            System.out.println("ERROR: bad access succeeded (method)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (method)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            Mutant.inaccessibleStaticMethod();
            System.out.println("ERROR: bad access succeeded (smethod)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (smethod)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            /* accessible static method in an inaccessible class */
            InaccessibleClass.test();
            System.out.println("ERROR: bad meth-class access succeeded (meth-class)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (meth-class)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            /* accessible static field in an inaccessible class */
            int blah = InaccessibleClass.blah;
            System.out.println("ERROR: bad field-class access succeeded (field-class)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (field-class)");
            if (VERBOSE) System.out.println("--- " + iae);
        }

        try {
            /* inaccessible static method in an accessible class */
            InaccessibleMethod.test();
            System.out.println("ERROR: bad access succeeded (meth-meth)");
        } catch (IllegalAccessError iae) {
            System.out.println("Got expected IllegalAccessError (meth-meth)");
            if (VERBOSE) System.out.println("--- " + iae);
        }
    }
}
