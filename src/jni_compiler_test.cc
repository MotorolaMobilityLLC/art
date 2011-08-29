// Copyright 2011 Google Inc. All Rights Reserved.

#include <sys/mman.h>

#include "assembler.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "indirect_reference_table.h"
#include "jni_compiler.h"
#include "jni_internal.h"
#include "mem_map.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "thread.h"
#include "gtest/gtest.h"

namespace art {

class JniCompilerTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();
    dex_.reset(OpenTestDexFile("MyClassNatives"));
    class_loader_ = AllocPathClassLoader(dex_.get());
    Thread::Current()->SetClassLoaderOverride(class_loader_);
  }

  void SetupForTest(bool direct, const char* method_name,
                    const char* method_sig, void* native_fnptr) {
    env_ = Thread::Current()->GetJniEnv();

    jklass_ = env_->FindClass("MyClass");
    ASSERT_TRUE(jklass_ != NULL);

    Class* c = class_linker_->FindClass("LMyClass;", class_loader_);
    Method* method;
    if (direct) {
      method = c->FindDirectMethod(method_name, method_sig);
    } else {
      method = c->FindVirtualMethod(method_name, method_sig);
    }
    ASSERT_TRUE(method != NULL);

    // Compile the native method
    jni_compiler.Compile(&jni_asm, method);
    ASSERT_TRUE(method->HasCode());

    if (direct) {
      jmethod_ = env_->GetStaticMethodID(jklass_, method_name, method_sig);
    } else {
      jmethod_ = env_->GetMethodID(jklass_, method_name, method_sig);
    }
    ASSERT_TRUE(jmethod_ != NULL);

    JNINativeMethod methods[] = {{method_name, method_sig, native_fnptr}};
    ASSERT_EQ(JNI_OK, env_->RegisterNatives(jklass_, methods, 1));

    jmethodID constructor = env_->GetMethodID(jklass_, "<init>", "()V");
    jobj_ = env_->NewObject(jklass_, constructor);
    ASSERT_TRUE(jobj_ != NULL);
  }

 public:
  static jclass jklass_;
  static jobject jobj_;
 protected:
  scoped_ptr<const DexFile> dex_;
  PathClassLoader* class_loader_;
  Assembler jni_asm;
  JniCompiler jni_compiler;
  JNIEnv* env_;
  jmethodID jmethod_;
};

jclass JniCompilerTest::jklass_;
jobject JniCompilerTest::jobj_;

int gJava_MyClass_foo_calls = 0;
void Java_MyClass_foo(JNIEnv* env, jobject thisObj) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_foo_calls++;
}

TEST_F(JniCompilerTest, CompileAndRunNoArgMethod) {
  SetupForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClass_foo));

  EXPECT_EQ(0, gJava_MyClass_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
}

int gJava_MyClass_fooI_calls = 0;
jint Java_MyClass_fooI(JNIEnv* env, jobject thisObj, jint x) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooI_calls++;
  return x;
}

TEST_F(JniCompilerTest, CompileAndRunIntMethod) {
  SetupForTest(false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClass_fooI));

  EXPECT_EQ(0, gJava_MyClass_fooI_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
}

int gJava_MyClass_fooII_calls = 0;
jint Java_MyClass_fooII(JNIEnv* env, jobject thisObj, jint x, jint y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooII_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunIntIntMethod) {
  SetupForTest(false, "fooII", "(II)I",
               reinterpret_cast<void*>(&Java_MyClass_fooII));

  EXPECT_EQ(0, gJava_MyClass_fooII_calls);
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 99, 10);
  EXPECT_EQ(99 - 10, result);
  EXPECT_EQ(1, gJava_MyClass_fooII_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 0xCAFEBABE,
                                         0xCAFED00D);
  EXPECT_EQ(static_cast<jint>(0xCAFEBABE - 0xCAFED00D), result);
  EXPECT_EQ(2, gJava_MyClass_fooII_calls);
}

int gJava_MyClass_fooDD_calls = 0;
jdouble Java_MyClass_fooDD(JNIEnv* env, jobject thisObj, jdouble x, jdouble y) {
  EXPECT_EQ(1u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooDD_calls++;
  return x - y;  // non-commutative operator
}

TEST_F(JniCompilerTest, CompileAndRunDoubleDoubleMethod) {
  SetupForTest(false, "fooDD", "(DD)D",
               reinterpret_cast<void*>(&Java_MyClass_fooDD));

  EXPECT_EQ(0, gJava_MyClass_fooDD_calls);
  jdouble result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_,
                                                    99.0, 10.0);
  EXPECT_EQ(99.0 - 10.0, result);
  EXPECT_EQ(1, gJava_MyClass_fooDD_calls);
  jdouble a = 3.14159265358979323846;
  jdouble b = 0.69314718055994530942;
  result = env_->CallNonvirtualDoubleMethod(jobj_, jklass_, jmethod_, a, b);
  EXPECT_EQ(a - b, result);
  EXPECT_EQ(2, gJava_MyClass_fooDD_calls);
}

int gJava_MyClass_fooIOO_calls = 0;
jobject Java_MyClass_fooIOO(JNIEnv* env, jobject thisObj, jint x, jobject y,
                            jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(thisObj != NULL);
  EXPECT_TRUE(env->IsInstanceOf(thisObj, JniCompilerTest::jklass_));
  gJava_MyClass_fooIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return thisObj;
  }
}

TEST_F(JniCompilerTest, CompileAndRunIntObjectObjectMethod) {
  SetupForTest(false, "fooIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooIOO));

  EXPECT_EQ(0, gJava_MyClass_fooIOO_calls);
  jobject result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(1, gJava_MyClass_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(2, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, NULL, jklass_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(4, gJava_MyClass_fooIOO_calls);

  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 0, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(5, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 1, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(6, gJava_MyClass_fooIOO_calls);
  result = env_->CallNonvirtualObjectMethod(jobj_, jklass_, jmethod_, 2, jklass_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooIOO_calls);
}

int gJava_MyClass_fooSIOO_calls = 0;
jobject Java_MyClass_fooSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}


TEST_F(JniCompilerTest, CompileAndRunStaticIntObjectObjectMethod) {
  SetupForTest(true, "fooSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooSIOO));

  EXPECT_EQ(0, gJava_MyClass_fooSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClass_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClass_fooSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClass_fooSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooSIOO_calls);
}

int gJava_MyClass_fooSSIOO_calls = 0;
jobject Java_MyClass_fooSSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumSirtReferences());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_TRUE(klass != NULL);
  EXPECT_TRUE(env->IsInstanceOf(JniCompilerTest::jobj_, klass));
  gJava_MyClass_fooSSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}

TEST_F(JniCompilerTest, CompileAndRunStaticSynchronizedIntObjectObjectMethod) {
  SetupForTest(true, "fooSSIOO",
               "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               reinterpret_cast<void*>(&Java_MyClass_fooSSIOO));

  EXPECT_EQ(0, gJava_MyClass_fooSSIOO_calls);
  jobject result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(1, gJava_MyClass_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(2, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(3, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, NULL, jobj_);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(4, gJava_MyClass_fooSSIOO_calls);

  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 0, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jklass_, result));
  EXPECT_EQ(5, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 1, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(jobj_, result));
  EXPECT_EQ(6, gJava_MyClass_fooSSIOO_calls);
  result = env_->CallStaticObjectMethod(jklass_, jmethod_, 2, jobj_, NULL);
  EXPECT_TRUE(env_->IsSameObject(NULL, result));
  EXPECT_EQ(7, gJava_MyClass_fooSSIOO_calls);
}

int gSuspendCounterHandler_calls;
void SuspendCountHandler(Method** frame) {
  EXPECT_TRUE((*frame)->GetName()->Equals("fooI"));
  gSuspendCounterHandler_calls++;
  Thread::Current()->DecrementSuspendCount();
}

TEST_F(JniCompilerTest, SuspendCountAcknowledgement) {
  SetupForTest(false, "fooI", "(I)I",
               reinterpret_cast<void*>(&Java_MyClass_fooI));
  Thread::Current()->RegisterSuspendCountEntryPoint(&SuspendCountHandler);

  gJava_MyClass_fooI_calls = 0;
  jint result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  EXPECT_EQ(0, gSuspendCounterHandler_calls);
  Thread::Current()->IncrementSuspendCount();
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
  EXPECT_EQ(1, gSuspendCounterHandler_calls);
  result = env_->CallNonvirtualIntMethod(jobj_, jklass_, jmethod_, 42);
  EXPECT_EQ(42, result);
  EXPECT_EQ(3, gJava_MyClass_fooI_calls);
  EXPECT_EQ(1, gSuspendCounterHandler_calls);
}

int gExceptionHandler_calls;
void ExceptionHandler(Method** frame) {
  EXPECT_TRUE((*frame)->GetName()->Equals("foo"));
  gExceptionHandler_calls++;
  Thread::Current()->ClearException();
}

TEST_F(JniCompilerTest, ExceptionHandling) {
  SetupForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClass_foo));
  Thread::Current()->RegisterExceptionEntryPoint(&ExceptionHandler);

  gJava_MyClass_foo_calls = 0;
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  EXPECT_EQ(0, gExceptionHandler_calls);
  // TODO: create a real exception here
  Thread::Current()->SetException(reinterpret_cast<Throwable*>(jobj_));
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
  EXPECT_EQ(1, gExceptionHandler_calls);
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  EXPECT_EQ(3, gJava_MyClass_foo_calls);
  EXPECT_EQ(1, gExceptionHandler_calls);
}

}  // namespace art
