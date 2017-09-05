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

package art;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class Exceptions {
  public static native void setupExceptionTracing(
      Class<?> methodClass,
      Class<?> exceptionClass,
      Method exceptionEventMethod,
      Method exceptionCaughtEventMethod);

  public static native void enableExceptionCatchEvent(Thread thr);
  public static native void enableExceptionEvent(Thread thr);
  public static native void disableExceptionCatchEvent(Thread thr);
  public static native void disableExceptionEvent(Thread thr);
}
