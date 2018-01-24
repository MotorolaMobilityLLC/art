//#include <jni.h>
//#include <iostream>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "jni.h"
#include "art_method-inl.h"

namespace art {
  pthread_t thread;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  static JavaVM* jvm = nullptr;

  static void* PthreadHelper(void*) {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args = { JNI_VERSION_1_6, __FUNCTION__, nullptr };
    int attach_result = jvm->AttachCurrentThread(&env, &args);
    CHECK_EQ(attach_result, 0);
    jclass c = env->FindClass("java/lang/String");
    jmethodID mid1 = env->GetMethodID(c, "<init>", "()V");
    jstring s1 = reinterpret_cast<jstring>(env->AllocObject(c));
    env->CallVoidMethod(s1, mid1);

    int detach_result = jvm->DetachCurrentThread();
    CHECK_EQ(detach_result, 0);
    pthread_mutex_lock(&mutex);
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    return nullptr;
  }

  extern "C" JNIEXPORT void JNICALL Java_WatchdogTest_watchdogNative(JNIEnv* env, jclass) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
    struct timeval now;
    struct timespec outtime;
    env->GetJavaVM(&jvm);

    jclass main_klass = env->FindClass("Main");
    jfieldID fid = env->GetStaticFieldID(main_klass, "quit", "Z");
    while (!env->GetStaticBooleanField(main_klass, fid))
    {
      pthread_mutex_lock(&mutex);
      if (0 != pthread_create(&thread, nullptr, PthreadHelper, nullptr))
      {
        printf("error when create pthread,%d\n", errno);
        continue;
      }
      gettimeofday(&now, NULL);
      outtime.tv_sec = now.tv_sec + 20;
      outtime.tv_nsec = now.tv_usec * 1000;
      int rc = pthread_cond_timedwait(&cond, &mutex, &outtime);
      if (rc == ETIMEDOUT) {
        printf("Wait timed out! Test fail.\n");
        abort();
      }
      pthread_join(thread, nullptr);
      pthread_mutex_unlock(&mutex);
      // sleep will fail by SIGQUIT. We have to ensure watchdog sleep at least 2 seconds.
      int sleep_result = -1;
      while (sleep_result != 0) {
        sleep_result = sleep(2);
      }
    }
  }
}
