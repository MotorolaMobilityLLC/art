#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "jni.h"
#include "art_method-inl.h"

static jweak weakly_stored_class_[40000];
static int jweak_cur = 0;

extern "C" JNIEXPORT void JNICALL Java_ThreadGCTest_weakGlobalRefTest(JNIEnv* env, jclass) {
  jclass c = env->FindClass("java/lang/String");
  jmethodID mid2 = env->GetMethodID(c, "<init>", "([B)V");
  const char* test_array = "Test";
  int byte_array_length = strlen(test_array);
  jbyteArray byte_array = env->NewByteArray(byte_array_length);
  env->SetByteArrayRegion(byte_array, 0, byte_array_length, reinterpret_cast<const jbyte*>(test_array));

  jstring s6 = reinterpret_cast<jstring>(env->AllocObject(c));
  weakly_stored_class_[jweak_cur++] = env->NewWeakGlobalRef(s6);
  s6 = reinterpret_cast<jstring>(weakly_stored_class_[jweak_cur-1]);
  env->CallNonvirtualVoidMethod(s6, c, mid2, byte_array);
  CHECK_EQ(env->GetStringLength(s6), byte_array_length);

  const char* chars6 = env->GetStringUTFChars(s6, nullptr);
  CHECK_EQ(strcmp(test_array, chars6), 0);
  env->ReleaseStringUTFChars(s6, chars6);
}
