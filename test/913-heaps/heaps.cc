/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "heaps.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "base/logging.h"
#include "base/macros.h"
#include "base/stringprintf.h"
#include "jit/jit.h"
#include "jni.h"
#include "native_stack_dump.h"
#include "openjdkjvmti/jvmti.h"
#include "runtime.h"
#include "thread-inl.h"
#include "thread_list.h"

#include "ti-agent/common_helper.h"
#include "ti-agent/common_load.h"

namespace art {
namespace Test913Heaps {

extern "C" JNIEXPORT void JNICALL Java_Main_forceGarbageCollection(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                   jclass klass ATTRIBUTE_UNUSED) {
  jvmtiError ret = jvmti_env->ForceGarbageCollection();
  if (ret != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(ret, &err);
    printf("Error forcing a garbage collection: %s\n", err);
  }
}

class IterationConfig {
 public:
  IterationConfig() {}
  virtual ~IterationConfig() {}

  virtual jint Handle(jvmtiHeapReferenceKind reference_kind,
                      const jvmtiHeapReferenceInfo* reference_info,
                      jlong class_tag,
                      jlong referrer_class_tag,
                      jlong size,
                      jlong* tag_ptr,
                      jlong* referrer_tag_ptr,
                      jint length,
                      void* user_data) = 0;
};

static jint JNICALL HeapReferenceCallback(jvmtiHeapReferenceKind reference_kind,
                                          const jvmtiHeapReferenceInfo* reference_info,
                                          jlong class_tag,
                                          jlong referrer_class_tag,
                                          jlong size,
                                          jlong* tag_ptr,
                                          jlong* referrer_tag_ptr,
                                          jint length,
                                          void* user_data) {
  IterationConfig* config = reinterpret_cast<IterationConfig*>(user_data);
  return config->Handle(reference_kind,
                        reference_info,
                        class_tag,
                        referrer_class_tag,
                        size,
                        tag_ptr,
                        referrer_tag_ptr,
                        length,
                        user_data);
}

static bool Run(jint heap_filter,
                jclass klass_filter,
                jobject initial_object,
                IterationConfig* config) {
  jvmtiHeapCallbacks callbacks;
  memset(&callbacks, 0, sizeof(jvmtiHeapCallbacks));
  callbacks.heap_reference_callback = HeapReferenceCallback;

  jvmtiError ret = jvmti_env->FollowReferences(heap_filter,
                                               klass_filter,
                                               initial_object,
                                               &callbacks,
                                               config);
  if (ret != JVMTI_ERROR_NONE) {
    char* err;
    jvmti_env->GetErrorName(ret, &err);
    printf("Failure running FollowReferences: %s\n", err);
    return false;
  }
  return true;
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_Main_followReferences(JNIEnv* env,
                                                                     jclass klass ATTRIBUTE_UNUSED,
                                                                     jint heap_filter,
                                                                     jclass klass_filter,
                                                                     jobject initial_object,
                                                                     jint stop_after,
                                                                     jint follow_set,
                                                                     jobject jniRef) {
  class PrintIterationConfig FINAL : public IterationConfig {
   public:
    PrintIterationConfig(jint _stop_after, jint _follow_set)
        : counter_(0),
          stop_after_(_stop_after),
          follow_set_(_follow_set) {
    }

    jint Handle(jvmtiHeapReferenceKind reference_kind,
                const jvmtiHeapReferenceInfo* reference_info,
                jlong class_tag,
                jlong referrer_class_tag,
                jlong size,
                jlong* tag_ptr,
                jlong* referrer_tag_ptr,
                jint length,
                void* user_data ATTRIBUTE_UNUSED) OVERRIDE {
      jlong tag = *tag_ptr;
      // Only check tagged objects.
      if (tag == 0) {
        return JVMTI_VISIT_OBJECTS;
      }

      Print(reference_kind,
            reference_info,
            class_tag,
            referrer_class_tag,
            size,
            tag_ptr,
            referrer_tag_ptr,
            length);

      counter_++;
      if (counter_ == stop_after_) {
        return JVMTI_VISIT_ABORT;
      }

      if (tag > 0 && tag < 32) {
        bool should_visit_references = (follow_set_ & (1 << static_cast<int32_t>(tag))) != 0;
        return should_visit_references ? JVMTI_VISIT_OBJECTS : 0;
      }

      return JVMTI_VISIT_OBJECTS;
    }

    void Print(jvmtiHeapReferenceKind reference_kind,
               const jvmtiHeapReferenceInfo* reference_info,
               jlong class_tag,
               jlong referrer_class_tag,
               jlong size,
               jlong* tag_ptr,
               jlong* referrer_tag_ptr,
               jint length) {
      std::string referrer_str;
      if (referrer_tag_ptr == nullptr) {
        referrer_str = "root@root";
      } else {
        referrer_str = StringPrintf("%" PRId64 "@%" PRId64, *referrer_tag_ptr, referrer_class_tag);
      }

      jlong adapted_size = size;
      if (*tag_ptr >= 1000) {
        // This is a class or interface, the size of which will be dependent on the architecture.
        // Do not print the size, but detect known values and "normalize" for the golden file.
        if ((sizeof(void*) == 4 && size == 180) || (sizeof(void*) == 8 && size == 232)) {
          adapted_size = 123;
        }
      }

      std::string referree_str = StringPrintf("%" PRId64 "@%" PRId64, *tag_ptr, class_tag);

      lines_.push_back(CreateElem(referrer_str,
                                  referree_str,
                                  reference_kind,
                                  reference_info,
                                  adapted_size,
                                  length));

      if (reference_kind == JVMTI_HEAP_REFERENCE_THREAD && *tag_ptr == 1000) {
        DumpStacks();
      }
    }

    std::vector<std::string> GetLines() const {
      std::vector<std::string> ret;
      for (const std::unique_ptr<Elem>& e : lines_) {
        ret.push_back(e->Print());
      }
      return ret;
    }

   private:
    // We need to postpone some printing, as required functions are not callback-safe.
    class Elem {
     public:
      Elem(const std::string& referrer, const std::string& referree, jlong size, jint length)
          : referrer_(referrer), referree_(referree), size_(size), length_(length) {}
      virtual ~Elem() {}

      std::string Print() const {
        return StringPrintf("%s --(%s)--> %s [size=%" PRId64 ", length=%d]",
                            referrer_.c_str(),
                            PrintArrowType().c_str(),
                            referree_.c_str(),
                            size_,
                            length_);
      }

     protected:
      virtual std::string PrintArrowType() const = 0;

     private:
      std::string referrer_;
      std::string referree_;
      jlong size_;
      jint length_;
    };

    class JNILocalElement : public Elem {
     public:
      JNILocalElement(const std::string& referrer,
                      const std::string& referree,
                      jlong size,
                      jint length,
                      const jvmtiHeapReferenceInfo* reference_info)
          : Elem(referrer, referree, size, length) {
        memcpy(&info_, reference_info, sizeof(jvmtiHeapReferenceInfo));
      }

     protected:
      std::string PrintArrowType() const OVERRIDE {
        char* name = nullptr;
        if (info_.jni_local.method != nullptr) {
          jvmti_env->GetMethodName(info_.jni_local.method, &name, nullptr, nullptr);
        }
        std::string ret = StringPrintf("jni-local[id=%" PRId64 ",tag=%" PRId64 ",depth=%d,"
                                       "method=%s]",
                                       info_.jni_local.thread_id,
                                       info_.jni_local.thread_tag,
                                       info_.jni_local.depth,
                                       name == nullptr ? "<null>" : name);
        if (name != nullptr) {
          jvmti_env->Deallocate(reinterpret_cast<unsigned char*>(name));
        }

        return ret;
      }

     private:
      const std::string string_;
      jvmtiHeapReferenceInfo info_;
    };

    // For simple or unimplemented cases.
    class StringElement : public Elem {
     public:
      StringElement(const std::string& referrer,
                   const std::string& referree,
                   jlong size,
                   jint length,
                   const std::string& string)
          : Elem(referrer, referree, size, length), string_(string) {}

     protected:
      std::string PrintArrowType() const OVERRIDE {
        return string_;
      }

     private:
      const std::string string_;
    };

    static std::unique_ptr<Elem> CreateElem(const std::string& referrer,
                                            const std::string& referree,
                                            jvmtiHeapReferenceKind reference_kind,
                                            const jvmtiHeapReferenceInfo* reference_info,
                                            jlong size,
                                            jint length) {
      switch (reference_kind) {
        case JVMTI_HEAP_REFERENCE_CLASS:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "class"));
        case JVMTI_HEAP_REFERENCE_FIELD: {
          std::string tmp = StringPrintf("field@%d", reference_info->field.index);
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                        referree,
                                                        size,
                                                        length,
                                                        tmp));
        }
        case JVMTI_HEAP_REFERENCE_ARRAY_ELEMENT: {
          std::string tmp = StringPrintf("array-element@%d", reference_info->array.index);
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         tmp));
        }
        case JVMTI_HEAP_REFERENCE_CLASS_LOADER:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "classloader"));
        case JVMTI_HEAP_REFERENCE_SIGNERS:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "signers"));
        case JVMTI_HEAP_REFERENCE_PROTECTION_DOMAIN:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "protection-domain"));
        case JVMTI_HEAP_REFERENCE_INTERFACE:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "interface"));
        case JVMTI_HEAP_REFERENCE_STATIC_FIELD: {
          std::string tmp = StringPrintf("array-element@%d", reference_info->array.index);
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         tmp));;
        }
        case JVMTI_HEAP_REFERENCE_CONSTANT_POOL:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "constant-pool"));
        case JVMTI_HEAP_REFERENCE_SUPERCLASS:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "superclass"));
        case JVMTI_HEAP_REFERENCE_JNI_GLOBAL:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "jni-global"));
        case JVMTI_HEAP_REFERENCE_SYSTEM_CLASS:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "system-class"));
        case JVMTI_HEAP_REFERENCE_MONITOR:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "monitor"));
        case JVMTI_HEAP_REFERENCE_STACK_LOCAL:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "stack-local"));
        case JVMTI_HEAP_REFERENCE_JNI_LOCAL:
          return std::unique_ptr<Elem>(new JNILocalElement(referrer,
                                                           referree,
                                                           size,
                                                           length,
                                                           reference_info));
        case JVMTI_HEAP_REFERENCE_THREAD:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "thread"));
        case JVMTI_HEAP_REFERENCE_OTHER:
          return std::unique_ptr<Elem>(new StringElement(referrer,
                                                         referree,
                                                         size,
                                                         length,
                                                         "other"));
      }
      LOG(FATAL) << "Unknown kind";
      UNREACHABLE();
    }

    static void DumpStacks() NO_THREAD_SAFETY_ANALYSIS {
      auto dump_function = [](art::Thread* t, void* data ATTRIBUTE_UNUSED) {
        std::string name;
        t->GetThreadName(name);
        LOG(ERROR) << name;
        art::DumpNativeStack(LOG_STREAM(ERROR), t->GetTid());
      };
      art::Runtime::Current()->GetThreadList()->ForEach(dump_function, nullptr);
    }

    jint counter_;
    const jint stop_after_;
    const jint follow_set_;

    std::vector<std::unique_ptr<Elem>> lines_;
  };

  jit::ScopedJitSuspend sjs;  // Wait to avoid JIT influence (e.g., JNI globals).

  // If jniRef isn't null, add a local and a global ref.
  ScopedLocalRef<jobject> jni_local_ref(env, nullptr);
  jobject jni_global_ref = nullptr;
  if (jniRef != nullptr) {
    jni_local_ref.reset(env->NewLocalRef(jniRef));
    jni_global_ref = env->NewGlobalRef(jniRef);
  }

  PrintIterationConfig config(stop_after, follow_set);
  Run(heap_filter, klass_filter, initial_object, &config);

  std::vector<std::string> lines = config.GetLines();
  jobjectArray ret = CreateObjectArray(env,
                                       static_cast<jint>(lines.size()),
                                       "java/lang/String",
                                       [&](jint i) {
                                         return env->NewStringUTF(lines[i].c_str());
                                       });

  if (jni_global_ref != nullptr) {
    env->DeleteGlobalRef(jni_global_ref);
  }

  return ret;
}

// Don't do anything
jint OnLoad(JavaVM* vm,
            char* options ATTRIBUTE_UNUSED,
            void* reserved ATTRIBUTE_UNUSED) {
  if (vm->GetEnv(reinterpret_cast<void**>(&jvmti_env), JVMTI_VERSION_1_0)) {
    printf("Unable to get jvmti env!\n");
    return 1;
  }
  SetAllCapabilities(jvmti_env);
  return 0;
}

}  // namespace Test913Heaps
}  // namespace art
