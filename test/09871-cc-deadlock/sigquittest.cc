#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "jni.h"

extern "C" JNIEXPORT void JNICALL Java_ThreadSignalQuitTest_signalQuitTest(JNIEnv*, jclass) {
  kill(getpid(), SIGQUIT);
}
