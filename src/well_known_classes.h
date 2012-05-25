/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_WELL_KNOWN_CLASSES_H_
#define ART_SRC_WELL_KNOWN_CLASSES_H_

#include "jni.h"

namespace art {

// Various classes used in JNI. We cache them so we don't have to keep looking
// them up. Similar to libcore's JniConstants (except there's no overlap, so
// we keep them separate).

struct WellKnownClasses {
  static void Init(JNIEnv* env);

  static jclass com_android_dex_Dex;
  static jclass java_lang_ClassLoader;
  static jclass java_lang_ClassNotFoundException;
  static jclass java_lang_Daemons;
  static jclass java_lang_Error;
  static jclass java_lang_ExceptionInInitializerError;
  static jclass java_lang_reflect_InvocationHandler;
  static jclass java_lang_reflect_Method;
  static jclass java_lang_reflect_Proxy;
  static jclass java_lang_reflect_UndeclaredThrowableException;
  static jclass java_lang_Thread;
  static jclass java_nio_ReadWriteDirectByteBuffer;
  static jclass org_apache_harmony_dalvik_ddmc_Chunk;
  static jclass org_apache_harmony_dalvik_ddmc_DdmServer;

  static jmethodID com_android_dex_Dex_create;
  static jmethodID java_lang_ClassNotFoundException_init;
  static jmethodID java_lang_Daemons_requestHeapTrim;
  static jmethodID java_lang_Daemons_start;
  static jmethodID java_lang_reflect_InvocationHandler_invoke;
  static jmethodID java_lang_Thread_init;
  static jmethodID java_nio_ReadWriteDirectByteBuffer_init;
  static jmethodID org_apache_harmony_dalvik_ddmc_DdmServer_broadcast;
  static jmethodID org_apache_harmony_dalvik_ddmc_DdmServer_dispatch;

  static jfieldID java_lang_reflect_Proxy_h;
  static jfieldID java_nio_ReadWriteDirectByteBuffer_capacity;
  static jfieldID java_nio_ReadWriteDirectByteBuffer_effectiveDirectAddress;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_data;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_length;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_offset;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_type;
};

}  // namespace art

#endif  // ART_SRC_WELL_KNOWN_CLASSES_H_
