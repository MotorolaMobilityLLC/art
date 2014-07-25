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

import java.util.ArrayList;
import java.util.List;

public class Main implements Runnable {
    public static void main(String[] args) throws Exception {
        Thread[] threads = new Thread[16];
        for (int i = 0; i < threads.length; i++) {
            threads[i] = new Thread(new Main(i));
        }
        for (Thread thread : threads) {
            thread.start();
        }
        for (Thread thread : threads) {
            thread.join();
        }
    }

    private final int id;

    private Main(int id) {
        this.id = id;
        // Allocate this early so it's independent of how the threads are scheduled on startup.
        this.l = new ArrayList<Object>();
    }

    public void run() {
        for (int i = 0; i < 1000; i++) {
            try {
                l.add(new ArrayList<Object>(i));
            } catch (OutOfMemoryError oom) {
                // Ignore.
            }
        }
    }

    private List<Object> l;
}
