// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include <sys/mman.h>
#include "src/assembler.h"
#include "src/class_linker.h"
#include "src/common_test.h"
#include "src/dex_file.h"
#include "src/jni_compiler.h"
#include "src/runtime.h"
#include "src/thread.h"
#include "gtest/gtest.h"

namespace art {

class JniCompilerTest : public testing::Test {
 protected:
  virtual void SetUp() {
    // Create runtime and attach thread
    runtime_ = Runtime::Create();
    CHECK(runtime_->AttachCurrentThread());
    // Create thunk code that performs the native to managed transition
    thunk_code_size_ = 4096;
    thunk_ = mmap(NULL, thunk_code_size_, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    CHECK_NE(MAP_FAILED, thunk_);
    Assembler thk_asm;
    // TODO: shouldn't have machine specific code in a general purpose file
#if defined(__i386__)
    thk_asm.pushl(EDI);                   // preserve EDI
    thk_asm.movl(EAX, Address(ESP, 8));   // EAX = method->GetCode()
    thk_asm.movl(EDI, Address(ESP, 12));  // EDI = method
    thk_asm.pushl(Immediate(0));          // push pad
    thk_asm.pushl(Immediate(0));          // push pad
    thk_asm.pushl(Address(ESP, 40));      // push pad  or jlong high
    thk_asm.pushl(Address(ESP, 40));      // push jint or jlong low
    thk_asm.pushl(Address(ESP, 40));      // push jint or jlong high
    thk_asm.pushl(Address(ESP, 40));      // push jint or jlong low
    thk_asm.pushl(Address(ESP, 40));      // push jobject
    thk_asm.call(EAX);                    // Continue in method->GetCode()
    thk_asm.addl(ESP, Immediate(28));     // pop arguments
    thk_asm.popl(EDI);                    // restore EDI
    thk_asm.ret();
#else
    LOG(FATAL) << "Unimplemented";
#endif
    size_t cs = thk_asm.CodeSize();
    MemoryRegion code(thunk_, cs);
    thk_asm.FinalizeInstructions(code);
    thunk_entry1_ = reinterpret_cast<jint (*)(const void*, art::Method*,
                                              jobject, jint, jint, jint)
                                    >(code.pointer());
    thunk_entry2_ = reinterpret_cast<jdouble (*)(const void*, art::Method*,
                                                 jobject, jdouble, jdouble)
                                    >(code.pointer());
  }

  virtual void TearDown() {
    // Release thunk code
    CHECK(runtime_->DetachCurrentThread());
    CHECK_EQ(0, munmap(thunk_, thunk_code_size_));
  }

  // Run generated code associated with method passing and returning int size
  // arguments
  jvalue RunMethod(Method* method, jvalue a, jvalue b, jvalue c, jvalue d) {
    jvalue result;
    // sanity checks
    EXPECT_NE(static_cast<void*>(NULL), method->GetCode());
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    // perform call
    result.i = (*thunk_entry1_)(method->GetCode(), method, a.l, b.i, c.i, d.i);
    // sanity check post-call
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    return result;
  }

  // Run generated code associated with method passing and returning double size
  // arguments
  jvalue RunMethodD(Method* method, jvalue a, jvalue b, jvalue c) {
    jvalue result;
    // sanity checks
    EXPECT_NE(static_cast<void*>(NULL), method->GetCode());
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    // perform call
    result.d = (*thunk_entry2_)(method->GetCode(), method, a.l, b.d, c.d);
    // sanity check post-call
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    return result;
  }

  Runtime* runtime_;
  void* thunk_;
  size_t thunk_code_size_;
  jint (*thunk_entry1_)(const void*, Method*, jobject, jint, jint, jint);
  jdouble (*thunk_entry2_)(const void*, Method*, jobject, jdouble, jdouble);
};

int gJava_MyClass_foo_calls = 0;
void Java_MyClass_foo(JNIEnv*, jobject) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_foo_calls++;
}

int gJava_MyClass_fooI_calls = 0;
jint Java_MyClass_fooI(JNIEnv*, jobject, jint x) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooI_calls++;
  return x;
}

int gJava_MyClass_fooII_calls = 0;
jint Java_MyClass_fooII(JNIEnv*, jobject, jint x, jint y) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooII_calls++;
  return x - y;  // non-commutative operator
}

int gJava_MyClass_fooDD_calls = 0;
jdouble Java_MyClass_fooDD(JNIEnv*, jobject, jdouble x, jdouble y) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooDD_calls++;
  return x - y;  // non-commutative operator
}

int gJava_MyClass_fooIOO_calls = 0;
jobject Java_MyClass_fooIOO(JNIEnv*, jobject thisObject, jint x, jobject y,
                            jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return thisObject;
  }
}

int gJava_MyClass_fooSIOO_calls = 0;
jobject Java_MyClass_fooSIOO(JNIEnv*, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
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

TEST_F(JniCompilerTest, CompileAndRunNoArgMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindVirtualMethod("foo");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  // JNIEnv* env = Thread::Current()->GetJniEnv();
  // JNINativeMethod methods[] = {{"foo", "()V", (void*)&Java_MyClass_foo}};
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_foo));

  jvalue a;
  a.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_foo_calls);
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindVirtualMethod("fooI");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooI));

  jvalue a, b, c;
  a.l = (jobject)NULL;
  b.i = 42;
  EXPECT_EQ(0, gJava_MyClass_fooI_calls);
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ(42, c.i);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  b.i = 0xCAFED00D;
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ((jint)0xCAFED00D, c.i);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindVirtualMethod("fooII");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooII));

  jvalue a, b, c, d;
  a.l = (jobject)NULL;
  b.i = 99;
  c.i = 10;
  EXPECT_EQ(0, gJava_MyClass_fooII_calls);
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ(99 - 10, d.i);
  EXPECT_EQ(1, gJava_MyClass_fooII_calls);
  b.i = 0xCAFEBABE;
  c.i = 0xCAFED00D;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jint)(0xCAFEBABE - 0xCAFED00D), d.i);
  EXPECT_EQ(2, gJava_MyClass_fooII_calls);
}


TEST_F(JniCompilerTest, CompileAndRunDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindVirtualMethod("fooDD");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooDD));

  jvalue a, b, c, d;
  a.l = (jobject)NULL;
  b.d = 99;
  c.d = 10;
  EXPECT_EQ(0, gJava_MyClass_fooDD_calls);
  d = RunMethodD(method, a, b, c);
  ASSERT_EQ(b.d - c.d, d.d);
  EXPECT_EQ(1, gJava_MyClass_fooDD_calls);
  b.d = 3.14159265358979323846;
  c.d = 0.69314718055994530942;
  d = RunMethodD(method, a, b, c);
  ASSERT_EQ(b.d - c.d, d.d);
  EXPECT_EQ(2, gJava_MyClass_fooDD_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntObjectObjectMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindVirtualMethod("fooIOO");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooIOO));

  jvalue a, b, c, d, e;
  a.l = (jobject)NULL;
  b.i = 0;
  c.l = (jobject)NULL;
  d.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_fooIOO_calls);
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(1, gJava_MyClass_fooIOO_calls);
  a.l = (jobject)8;
  b.i = 0;
  c.l = (jobject)NULL;
  d.l = (jobject)16;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)8, e.l);
  EXPECT_EQ(2, gJava_MyClass_fooIOO_calls);
  b.i = 1;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(3, gJava_MyClass_fooIOO_calls);
  b.i = 2;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)16, e.l);
  EXPECT_EQ(4, gJava_MyClass_fooIOO_calls);
  a.l = (jobject)8;
  b.i = 0;
  c.l = (jobject)16;
  d.l = (jobject)NULL;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)8, e.l);
  EXPECT_EQ(5, gJava_MyClass_fooIOO_calls);
  b.i = 1;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)16, e.l);
  EXPECT_EQ(6, gJava_MyClass_fooIOO_calls);
  b.i = 2;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(7, gJava_MyClass_fooIOO_calls);
}

TEST_F(JniCompilerTest, CompileAndRunStaticIntObjectObjectMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());
  Class* klass = linker->FindClass("LMyClass;", NULL);
  Method* method = klass->FindDirectMethod("fooSIOO");

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooSIOO));

  jvalue a, b, c, d;
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_fooSIOO_calls);
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(1, gJava_MyClass_fooSIOO_calls);
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)16;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(2, gJava_MyClass_fooSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(3, gJava_MyClass_fooSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(4, gJava_MyClass_fooSIOO_calls);
  a.i = 0;
  b.l = (jobject)16;
  c.l = (jobject)NULL;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(5, gJava_MyClass_fooSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(6, gJava_MyClass_fooSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(7, gJava_MyClass_fooSIOO_calls);
}

}  // namespace art
