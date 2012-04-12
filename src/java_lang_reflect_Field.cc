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

#include "jni_internal.h"
#include "class_linker.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

static bool GetFieldValue(Object* o, Field* f, JValue& value, bool allow_references) {
  DCHECK_EQ(value.GetJ(), 0LL);
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(f->GetDeclaringClass(), true, true)) {
    return false;
  }
  switch (FieldHelper(f).GetTypeAsPrimitiveType()) {
  case Primitive::kPrimBoolean:
    value.SetZ(f->GetBoolean(o));
    return true;
  case Primitive::kPrimByte:
    value.SetB(f->GetByte(o));
    return true;
  case Primitive::kPrimChar:
    value.SetC(f->GetChar(o));
    return true;
  case Primitive::kPrimDouble:
    value.SetD(f->GetDouble(o));
    return true;
  case Primitive::kPrimFloat:
    value.SetF(f->GetFloat(o));
    return true;
  case Primitive::kPrimInt:
    value.SetI(f->GetInt(o));
    return true;
  case Primitive::kPrimLong:
    value.SetJ(f->GetLong(o));
    return true;
  case Primitive::kPrimShort:
    value.SetS(f->GetShort(o));
    return true;
  case Primitive::kPrimNot:
    if (allow_references) {
      value.SetL(f->GetObject(o));
      return true;
    }
    // Else break to report an error.
    break;
  case Primitive::kPrimVoid:
    // Never okay.
    break;
  }
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
      "Not a primitive field: %s", PrettyField(f).c_str());
  return false;
}

static bool CheckReceiver(JNIEnv* env, jobject javaObj, Field* f, Object*& o) {
  if (f->IsStatic()) {
    o = NULL;
    return true;
  }

  o = Decode<Object*>(env, javaObj);
  Class* declaringClass = f->GetDeclaringClass();
  if (!VerifyObjectInClass(env, o, declaringClass)) {
    return false;
  }
  return true;
}

static jobject Field_get(JNIEnv* env, jobject javaField, jobject javaObj) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return NULL;
  }

  // Get the field's value, boxing if necessary.
  JValue value;
  if (!GetFieldValue(o, f, value, true)) {
    return NULL;
  }
  BoxPrimitive(FieldHelper(f).GetTypeAsPrimitiveType(), value);

  return AddLocalReference<jobject>(env, value.GetL());
}

static JValue GetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, char dst_descriptor) {
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return JValue();
  }

  // Read the value.
  JValue field_value;
  if (!GetFieldValue(o, f, field_value, false)) {
    return JValue();
  }

  // Widen it if necessary (and possible).
  JValue wide_value;
  Class* dst_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(dst_descriptor);
  if (!ConvertPrimitiveValue(FieldHelper(f).GetTypeAsPrimitiveType(), dst_type->GetPrimitiveType(),
                             field_value, wide_value)) {
    return JValue();
  }
  return wide_value;
}

static jboolean Field_getBoolean(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'Z').GetZ();
}

static jbyte Field_getByte(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'B').GetB();
}

static jchar Field_getChar(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'C').GetC();
}

static jdouble Field_getDouble(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'D').GetD();
}

static jfloat Field_getFloat(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'F').GetF();
}

static jint Field_getInt(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'I').GetI();
}

static jlong Field_getLong(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'J').GetJ();
}

static jshort Field_getShort(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField(env, javaField, javaObj, 'S').GetS();
}

static void SetFieldValue(Object* o, Field* f, const JValue& new_value, bool allow_references) {
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(f->GetDeclaringClass(), true, true)) {
    return;
  }
  switch (FieldHelper(f).GetTypeAsPrimitiveType()) {
  case Primitive::kPrimBoolean:
    f->SetBoolean(o, new_value.GetZ());
    break;
  case Primitive::kPrimByte:
    f->SetByte(o, new_value.GetB());
    break;
  case Primitive::kPrimChar:
    f->SetChar(o, new_value.GetC());
    break;
  case Primitive::kPrimDouble:
    f->SetDouble(o, new_value.GetD());
    break;
  case Primitive::kPrimFloat:
    f->SetFloat(o, new_value.GetF());
    break;
  case Primitive::kPrimInt:
    f->SetInt(o, new_value.GetI());
    break;
  case Primitive::kPrimLong:
    f->SetLong(o, new_value.GetJ());
    break;
  case Primitive::kPrimShort:
    f->SetShort(o, new_value.GetS());
    break;
  case Primitive::kPrimNot:
    if (allow_references) {
      f->SetObject(o, new_value.GetL());
      break;
    }
    // Else fall through to report an error.
  case Primitive::kPrimVoid:
    // Never okay.
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "Not a primitive field: %s", PrettyField(f).c_str());
    return;
  }

  // Special handling for final fields on SMP systems.
  // We need a store/store barrier here (JMM requirement).
  if (f->IsFinal()) {
    ANDROID_MEMBAR_STORE();
  }
}

static void Field_set(JNIEnv* env, jobject javaField, jobject javaObj, jobject javaValue) {
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));

  // Unbox the value, if necessary.
  Object* boxed_value = Decode<Object*>(env, javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitive(boxed_value, FieldHelper(f).GetType(), unboxed_value, "field")) {
    return;
  }

  // Check that the receiver is non-null and an instance of the field's declaring class.
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return;
  }

  SetFieldValue(o, f, unboxed_value, true);
}

static void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj, char src_descriptor,
                              const JValue& new_value) {
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Field* f = DecodeField(env->FromReflectedField(javaField));
  Object* o = NULL;
  if (!CheckReceiver(env, javaObj, f, o)) {
    return;
  }
  FieldHelper fh(f);
  if (!fh.IsPrimitiveType()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
        "Not a primitive field: %s", PrettyField(f).c_str());
    return;
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  Class* src_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(src_descriptor);
  if (!ConvertPrimitiveValue(src_type->GetPrimitiveType(), fh.GetTypeAsPrimitiveType(),
                             new_value, wide_value)) {
    return;
  }

  // Write the value.
  SetFieldValue(o, f, wide_value, false);
}

static void Field_setBoolean(JNIEnv* env, jobject javaField, jobject javaObj, jboolean z) {
  JValue value;
  value.SetZ(z);
  SetPrimitiveField(env, javaField, javaObj, 'Z', value);
}

static void Field_setByte(JNIEnv* env, jobject javaField, jobject javaObj, jbyte b) {
  JValue value;
  value.SetB(b);
  SetPrimitiveField(env, javaField, javaObj, 'B', value);
}

static void Field_setChar(JNIEnv* env, jobject javaField, jobject javaObj, jchar c) {
  JValue value;
  value.SetC(c);
  SetPrimitiveField(env, javaField, javaObj, 'C', value);
}

static void Field_setDouble(JNIEnv* env, jobject javaField, jobject javaObj, jdouble d) {
  JValue value;
  value.SetD(d);
  SetPrimitiveField(env, javaField, javaObj, 'D', value);
}

static void Field_setFloat(JNIEnv* env, jobject javaField, jobject javaObj, jfloat f) {
  JValue value;
  value.SetF(f);
  SetPrimitiveField(env, javaField, javaObj, 'F', value);
}

static void Field_setInt(JNIEnv* env, jobject javaField, jobject javaObj, jint i) {
  JValue value;
  value.SetI(i);
  SetPrimitiveField(env, javaField, javaObj, 'I', value);
}

static void Field_setLong(JNIEnv* env, jobject javaField, jobject javaObj, jlong j) {
  JValue value;
  value.SetJ(j);
  SetPrimitiveField(env, javaField, javaObj, 'J', value);
}

static void Field_setShort(JNIEnv* env, jobject javaField, jobject javaObj, jshort s) {
  JValue value;
  value.SetS(s);
  SetPrimitiveField(env, javaField, javaObj, 'S', value);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Field, get,        "(Ljava/lang/Object;)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getBoolean, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Field, getByte,    "(Ljava/lang/Object;)B"),
  NATIVE_METHOD(Field, getChar,    "(Ljava/lang/Object;)C"),
  NATIVE_METHOD(Field, getDouble,  "(Ljava/lang/Object;)D"),
  NATIVE_METHOD(Field, getFloat,   "(Ljava/lang/Object;)F"),
  NATIVE_METHOD(Field, getInt,     "(Ljava/lang/Object;)I"),
  NATIVE_METHOD(Field, getLong,    "(Ljava/lang/Object;)J"),
  NATIVE_METHOD(Field, getShort,   "(Ljava/lang/Object;)S"),
  NATIVE_METHOD(Field, set,        "(Ljava/lang/Object;Ljava/lang/Object;)V"),
  NATIVE_METHOD(Field, setBoolean, "(Ljava/lang/Object;Z)V"),
  NATIVE_METHOD(Field, setByte,    "(Ljava/lang/Object;B)V"),
  NATIVE_METHOD(Field, setChar,    "(Ljava/lang/Object;C)V"),
  NATIVE_METHOD(Field, setDouble,  "(Ljava/lang/Object;D)V"),
  NATIVE_METHOD(Field, setFloat,   "(Ljava/lang/Object;F)V"),
  NATIVE_METHOD(Field, setInt,     "(Ljava/lang/Object;I)V"),
  NATIVE_METHOD(Field, setLong,    "(Ljava/lang/Object;J)V"),
  NATIVE_METHOD(Field, setShort,   "(Ljava/lang/Object;S)V"),
};

void register_java_lang_reflect_Field(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Field", gMethods, NELEM(gMethods));
}

}  // namespace art
