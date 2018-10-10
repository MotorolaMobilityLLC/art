/*
 * Copyright (C) 2017 The Android Open Source Project
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

import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

import java.time.Duration;

import java.util.concurrent.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;
import java.util.Random;
import java.util.Stack;
import java.util.Vector;

import java.util.function.Supplier;

import art.*;

public class Main extends Test1953 {
  public Main(boolean run_class_load_tests) {
    super(run_class_load_tests, (testObj) -> {
      try {
        // Make sure everything is jitted in the method. We do this before calling setup since the
        // suspend setup might make it impossible to jit the methods (by setting breakpoints or
        // something).
        for (Method m : testObj.getClass().getMethods()) {
          if ((m.getModifiers() & Modifier.NATIVE) == 0 &&
              !m.getName().startsWith("$noprecompile$")) {
            ensureMethodJitCompiled(m);
          }
        }
      } catch (Exception e) {}
    });
  }

  public static void main(String[] args) throws Exception {
    new Main(!Arrays.asList(args).contains("DISABLE_CLASS_LOAD_TESTS")).runTests();
  }

  public static native void ensureMethodJitCompiled(Method meth);
}
