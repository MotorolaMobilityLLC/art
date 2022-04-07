/*
 * Copyright (C) 2022 The Android Open Source Project
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

package pkg1;

// This definition is used for compiling but the interface used at runtime is in src2/.
// The commented out code below is enabled in src2/.
public interface I1 {
    static void callI1Foo(I1 i1) {
        System.out.println("Calling pkg1.I1.foo on " + i1.getClass().getName());
        // i1.foo();
    };

    // void foo();
}
