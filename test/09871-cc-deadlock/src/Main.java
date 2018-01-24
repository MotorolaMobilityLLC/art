/*
 * Copyright (C) 2011 The Android Open Source Project
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

import java.lang.ref.WeakReference;
import java.util.Arrays;
import java.util.LinkedList;
import java.lang.Thread;
import java.lang.reflect.*;
import java.util.concurrent.locks.*;
import java.util.concurrent.TimeUnit;
import java.util.Random;
import java.util.ArrayList;
import java.io.File;
import java.io.BufferedReader;
import java.io.FileReader;

/**
 * This thread is used to monitor whther art deadlock.
 */
class WatchdogTest extends Thread {
    public static native void watchdogNative();
    public void run() {
        watchdogNative();
    }
}

class TestThread extends Thread {
    private int contents;
    private boolean isNoDeadlock = true;
    private Lock aLock = new ReentrantLock();
    private Condition condVar = aLock.newCondition();

    TestThread() {
        Thread.currentThread().setName(this.getClass().getName());
    }

    public void runImpl() {
    }

    public void run() {
        while (!Main.quit) {
            runImpl();
        }
    }
}

/**
 * Make this thread enter ClassLinker and add some class loader of weak reference for Signal Catcher can enter
 * DecodeWeakGlobalLocked.
 */
class ThreadClassLinkTest extends TestThread {
    private static final String CLASS_PATH = System.getenv("DEX_LOCATION") + "/09871-cc-deadlock-ex.jar";
    private static final String ODEX_DIR = System.getenv("DEX_LOCATION");
    private static final String ODEX_ALT = "/tmp";
    private static final String LIB_DIR = "/nowhere/nothing/";

    private static final String getOdexDir() {
        return new File(ODEX_DIR).isDirectory() ? ODEX_DIR : ODEX_ALT;
    }

    private WeakReference<ClassLoader> testLoadDex() {
        try {
            ClassLoader classLoader = Main.class.getClassLoader();
            Class<?> DexClassLoader = classLoader.loadClass("dalvik.system.DexClassLoader");
            Constructor<?> DexClassLoader_init = DexClassLoader.getConstructor(String.class,
                    String.class,
                    String.class,
                    ClassLoader.class);

            ClassLoader dexClassLoader = (ClassLoader) DexClassLoader_init.newInstance(CLASS_PATH,
                    getOdexDir(), LIB_DIR, classLoader);
            Class<?> ccDeadlock = dexClassLoader.loadClass("CCDeadlockTest");
            ICCDeadlockTest ccDeadlockObj = (ICCDeadlockTest) ccDeadlock.newInstance();
            double ccResult1 = ccDeadlockObj.testLoad();
            double ccResult2 = 0;

            if (ccResult1 % 100 == 0) {
                Class<?> ccDeadlock2 = dexClassLoader.loadClass("CCDeadlockTest2");
                ICCDeadlockTest ccDeadlockObj2 = (ICCDeadlockTest) ccDeadlock2.newInstance();
                ccResult2 = ccDeadlockObj2.testLoad2();
            }
            return new WeakReference(dexClassLoader);
        } catch (Exception e) {
            System.out.println(e);
            e.printStackTrace();
        }
        return null;
    }

    public void runImpl() {
        try {
            for (int i = 0; i < 100; i++) {
                WeakReference<ClassLoader> weakLoader = testLoadDex();
            }
        } catch (Exception e) {
            System.out.println(e);
            e.printStackTrace();
        }
    }
}

/**
 * Trigger Signal Catcher execution.
 */
class ThreadSignalQuitTest extends TestThread {
    public static native void signalQuitTest();

    public void runImpl() {
        signalQuitTest();
        Main.sleep(Main.SLEEP_TIME * 10);
    }
}

/**
 * Trigger HeapTaskDaemon execute GC.
 */
class ThreadGCTest extends TestThread {
    public static native void weakGlobalRefTest();

    private boolean isNoBlock = false;
    WeakReference<ArrayList<String>> watcher;
    int[] tooBig;

    ThreadGCTest() {
        // leverage other gc test code to enhance gc triggered chance
        Robin robin = new Robin();
        Deep deep = new Deep();
        Large large = new Large();

        /* start all threads */
        robin.start();
        deep.start();
        large.start();
        for (int i = 0; i < 10000; i++) {
            weakGlobalRefTest();
        }
    }

    void keepMemory(ArrayList<String> t) {
        String s = t.get(0);
        s += "a";
    }

    String getSaltString() {
        String SALTCHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
        StringBuilder salt = new StringBuilder();
        Random rnd = new Random();
        while (salt.length() < 18) { // length of the random string.
            int index = (int) (rnd.nextFloat() * SALTCHARS.length());
            salt.append(SALTCHARS.charAt(index));
        }
        String saltStr = salt.toString();
        return saltStr;
    }

    ArrayList<String> getRandomString() {
        ArrayList<String> a = new ArrayList<>();
        Random rand = new Random();
        int n = rand.nextInt(100) + 1000;
        for (int i = 0; i < n; i++) {
            a.add(getSaltString());
        }
        return a;
    }

    public void runImpl() {
        Main.sleep(Main.SLEEP_TIME / 4);
        watcher = new WeakReference<ArrayList<String>>(getRandomString());
        keepMemory(watcher.get());
    }
}

/**
 * Simulate system high loading to increase reproduce rate.
 */
class HighCPUUsageThread extends Thread{
    static int nameSerial = 0;
    HighCPUUsageThread() {
        Thread.currentThread().setName(this.getClass().getName() + HighCPUUsageThread.nameSerial++);
    }

    public void run() {
        int i = 0;
        while (!Main.quit) {
            i++;
        }
    }
}

public class Main {
    static final int SLEEP_TIME = 100;
    private static final int TEST_TIME = 30;
    private static final int MINUTES_OF_MILLISECONDS = 60 * 1000;
    public static volatile boolean quit = false;
    public static final boolean DEBUG = false;

    static boolean isAllThreadFinished(TestThread[] t) {
        for (int i = 0; i < t.length; i++) {
            if (t[i].isAlive()) {
                return false;
            }
        }
        return true;
    }

    /**
     * Sleeps for the "ms" milliseconds.
     */
    public static void sleep(int ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ie) {
            System.out.println("sleep was interrupted");
        }
    }

    /**
     * Sleeps briefly, allowing other threads some CPU time to get started.
     */
    public static void startupDelay() {
        sleep(500);
    }

    public static void main(String[] args) {
        System.loadLibrary(args[0]);
        TestThread[] allTestThreads = new TestThread[]{
                new ThreadClassLinkTest(),
                new ThreadSignalQuitTest(),
                new ThreadGCTest()
        };

        for (int i = 0; i < allTestThreads.length; i++) {
            allTestThreads[i].start();
        }

        int cores = Runtime.getRuntime().availableProcessors();
        for (int i = 0;i < cores - allTestThreads.length; i++){
            (new HighCPUUsageThread()).start();
        }

        WatchdogTest wt = new WatchdogTest();
        wt.start();

        try {
            for (int i = 1; i <= TEST_TIME; i++) {
                Thread.sleep(MINUTES_OF_MILLISECONDS);
                System.out.println("Elapse time: " + i + "m");
            }
        }catch (Exception e){
            e.printStackTrace();
        }
        Main.quit = true;
        System.out.println("Test PASS!");
    }
}

/**
 * Allocates useless objects and holds on to several of them.
 * <p>
 * Uses a single large array of references, replaced repeatedly in round-robin
 * order.
 */
class Robin extends Thread {
    private static final int ARRAY_SIZE = 40960;
    int sleepCount = 0;

    public void run() {
        Main.startupDelay();

        String strings[] = new String[ARRAY_SIZE];
        int idx = 0;

        while (!Main.quit) {
            strings[idx] = makeString(idx);

            if (idx % (ARRAY_SIZE / 4) == 0) {
                Main.sleep(400);
                sleepCount++;
            }

            idx = (idx + 1) % ARRAY_SIZE;
        }

        if (Main.DEBUG)
            System.out.println("Robin: sleepCount=" + sleepCount);
    }

    private String makeString(int val) {
        try {
            return new String("Robin" + val);
        } catch (OutOfMemoryError e) {
            return null;
        }
    }
}

/**
 * Allocates useless objects in recursive calls.
 */
class Deep extends Thread {
    private static final int MAX_DEPTH = 61;

    private static String strong[] = new String[MAX_DEPTH];
    private static WeakReference weak[] = new WeakReference[MAX_DEPTH];

    public void run() {
        int iter = 0;
        boolean once = false;

        Main.startupDelay();

        while (!Main.quit) {
            dive(0, iter);
            once = true;
            iter += MAX_DEPTH;
        }

        if (!once) {
            System.out.println("not even once?");
            return;
        }

        checkStringReferences();

        /*
         * Wipe "strong", do a GC, see if "weak" got collected.
         */
        for (int i = 0; i < MAX_DEPTH; i++)
            strong[i] = null;

        Runtime.getRuntime().gc();

        for (int i = 0; i < MAX_DEPTH; i++) {
            if (weak[i].get() != null) {
                System.out.println("Deep: weak still has " + i);
            }
        }

        if (Main.DEBUG)
            System.out.println("Deep: iters=" + iter / MAX_DEPTH);
    }


    /**
     * Check the results of the last trip through.  Everything in
     * "weak" should be matched in "strong", and the two should be
     * equivalent (object-wise, not just string-equality-wise).
     * <p>
     * We do that check in a separate method to avoid retaining these
     * String references in local DEX registers. In interpreter mode,
     * they would retain these references until the end of the method
     * or until they are updated to another value.
     */
    private static void checkStringReferences() {
        for (int i = 0; i < MAX_DEPTH; i++) {
            if (strong[i] != weak[i].get()) {
                System.out.println("Deep: " + i + " strong=" + strong[i] +
                        ", weak=" + weak[i].get());
            }
        }
    }

    /**
     * Recursively dive down, setting one or more local variables.
     * <p>
     * We pad the stack out with locals, attempting to create a mix of
     * valid and invalid references on the stack.
     */
    private String dive(int depth, int iteration) {
        try {
            String str0;
            String str1;
            String str2;
            String str3;
            String str4;
            String str5;
            String str6;
            String str7;
            String funStr = "";
            switch (iteration % 8) {
                case 0:
                    funStr = str0 = makeString(iteration);
                    break;
                case 1:
                    funStr = str1 = makeString(iteration);
                    break;
                case 2:
                    funStr = str2 = makeString(iteration);
                    break;
                case 3:
                    funStr = str3 = makeString(iteration);
                    break;
                case 4:
                    funStr = str4 = makeString(iteration);
                    break;
                case 5:
                    funStr = str5 = makeString(iteration);
                    break;
                case 6:
                    funStr = str6 = makeString(iteration);
                    break;
                case 7:
                    funStr = str7 = makeString(iteration);
                    break;
            }

            weak[depth] = new WeakReference(funStr);
            strong[depth] = funStr;
            if (depth + 1 < MAX_DEPTH)
                dive(depth + 1, iteration + 1);
            else
                Main.sleep(100);
            return funStr;
        } catch (OutOfMemoryError e) {
            // Silently ignore OOME since gc stress mode causes them to occur but shouldn't be a
            // test failure.
        }
        return "";
    }

    private String makeString(int val) {
        try {
            return new String("Deep" + val);
        } catch (OutOfMemoryError e) {
            return null;
        }
    }
}


/**
 * Allocates large useless objects.
 */
class Large extends Thread {
    public void run() {
        byte[] chunk;
        int count = 0;
        int sleepCount = 0;

        Main.startupDelay();

        while (!Main.quit) {
            try {
                chunk = new byte[100000];
                pretendToUse(chunk);

                count++;
                if ((count % 500) == 0) {
                    Main.sleep(400);
                    sleepCount++;
                }
            } catch (OutOfMemoryError e) {
            }
        }

        if (Main.DEBUG)
            System.out.println("Large: sleepCount=" + sleepCount);
    }

    public void pretendToUse(byte[] chunk) {
    }
}