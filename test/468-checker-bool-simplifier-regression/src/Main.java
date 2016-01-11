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

import java.lang.reflect.*;

public class Main {

  public static boolean runTest(boolean input) throws Exception {
    Class<?> c = Class.forName("TestCase");
    Method m = c.getMethod("testCase");
    Field f = c.getField("value");
    f.set(null, (Boolean) input);
    return (Boolean) m.invoke(null);
  }

  public static void main(String[] args) throws Exception {
    if (runTest(true) != false) {
      throw new Error("Expected false, got true");
    }

    if (runTest(false) != true) {
      throw new Error("Expected true, got false");
    }
  }
}
