/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "class_loader.h"
#include "class_linker.h"
#include "dex_file.h"
#include "logging.h"
#include "runtime.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

// A smart pointer that provides read-only access to a Java string's UTF chars.
// Unlike libcore's NullableScopedUtfChars, this will *not* throw NullPointerException if
// passed a null jstring. The correct idiom is:
//
//   NullableScopedUtfChars name(env, javaName);
//   if (env->ExceptionOccurred()) {
//       return NULL;
//   }
//   // ... use name.c_str()
//
// TODO: rewrite to get rid of this, or change ScopedUtfChars to offer this option.
class NullableScopedUtfChars {
public:
    NullableScopedUtfChars(JNIEnv* env, jstring s)
    : mEnv(env), mString(s)
    {
        mUtfChars = (s != NULL) ? env->GetStringUTFChars(s, NULL) : NULL;
    }

    ~NullableScopedUtfChars() {
        if (mUtfChars) {
            mEnv->ReleaseStringUTFChars(mString, mUtfChars);
        }
    }

    const char* c_str() const {
        return mUtfChars;
    }

    size_t size() const {
        return strlen(mUtfChars);
    }

    // Element access.
    const char& operator[](size_t n) const {
        return mUtfChars[n];
    }

private:
    JNIEnv* mEnv;
    jstring mString;
    const char* mUtfChars;

    // Disallow copy and assignment.
    NullableScopedUtfChars(const NullableScopedUtfChars&);
    void operator=(const NullableScopedUtfChars&);
};

static jint DexFile_openDexFile(JNIEnv* env, jclass, jstring javaSourceName, jstring javaOutputName) {
  ScopedUtfChars sourceName(env, javaSourceName);
  if (sourceName.c_str() == NULL) {
    return 0;
  }
  NullableScopedUtfChars outputName(env, javaOutputName);
  if (env->ExceptionOccurred()) {
    return 0;
  }
  const DexFile* dex_file = DexFile::Open(sourceName.c_str(), "");
  if (dex_file == NULL) {
    jniThrowExceptionFmt(env, "java/io/IOException", "unable to open DEX file: %s",
                         sourceName.c_str());
    return NULL;
  }
  return static_cast<jint>(reinterpret_cast<uintptr_t>(dex_file));
}

static const DexFile* toDexFile(JNIEnv* env, int dex_file_address) {
  const DexFile* dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(dex_file_address));
  if ((dex_file == NULL)) {
    jniThrowNullPointerException(env, "dex_file == null");
  }
  return dex_file;
}

void DexFile_closeDexFile(JNIEnv* env, jclass, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return;
  }
  if (Runtime::Current()->GetClassLinker()->IsDexFileRegistered(*dex_file)) {
    return;
  }
  delete dex_file;
}

jclass DexFile_defineClass(JNIEnv* env, jclass, jstring javaName, jobject javaLoader, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return NULL;
  }
  String* name = Decode<String*>(env, javaName);
  const char* class_name = name->ToModifiedUtf8().c_str();
  const std::string descriptor = DotToDescriptor(class_name);
  const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
  if (dex_class_def == NULL) {
    jniThrowExceptionFmt(env, "Ljava/lang/NoClassDefFoundError;", "Class %s not found", class_name);
    return NULL;
  }

  Object* class_loader_object = Decode<Object*>(env, javaLoader);
  ClassLoader* class_loader = down_cast<ClassLoader*>(class_loader_object);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->RegisterDexFile(*dex_file);
  Class* result = class_linker->DefineClass(descriptor, class_loader, *dex_file, *dex_class_def);
  return AddLocalReference<jclass>(env, result);
}

jobjectArray DexFile_getClassNameList(JNIEnv* env, jclass, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return NULL;
  }
  UNIMPLEMENTED(ERROR);
  return NULL;
}

jboolean DexFile_isDexOptNeeded(JNIEnv* env, jclass, jstring javaFilename) {
  // TODO: run dex2oat?
  UNIMPLEMENTED(WARNING);
  return JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexFile, closeDexFile, "(I)V"),
  NATIVE_METHOD(DexFile, defineClass, "(Ljava/lang/String;Ljava/lang/ClassLoader;I)Ljava/lang/Class;"),
  NATIVE_METHOD(DexFile, getClassNameList, "(I)[Ljava/lang/String;"),
  NATIVE_METHOD(DexFile, isDexOptNeeded, "(Ljava/lang/String;)Z"),
  NATIVE_METHOD(DexFile, openDexFile, "(Ljava/lang/String;Ljava/lang/String;I)I"),
};

}  // namespace

void register_dalvik_system_DexFile(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/DexFile", gMethods, NELEM(gMethods));
}

}  // namespace art
