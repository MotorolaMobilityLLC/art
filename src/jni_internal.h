// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"
#include "reference_table.h"

namespace art {

class Runtime;
class Thread;

struct JavaVMExt {
  JavaVMExt(Runtime* runtime);

  // Must be first to correspond with JNIEnv.
  const struct JNIInvokeInterface* fns;

  Runtime* runtime;

  // Used to hold references to pinned primitive arrays.
  ReferenceTable pin_table;
};

struct JNIEnvExt {
  JNIEnvExt(Thread* self);

  // Must be first to correspond with JavaVM.
  const struct JNINativeInterface* fns;

  Thread* self;

  // Are we in a "critical" JNI call?
  bool critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable  monitor_table;

  // Used to help call synchronized native methods.
  // TODO: make jni_compiler.cc do the indirection itself.
  void (*MonitorEnterHelper)(JNIEnv*, jobject);
  void (*MonitorExitHelper)(JNIEnv*, jobject);
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
