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

#include "class_linker.h"
#include "jni_internal.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"

namespace art {

/*
 * We get here through Constructor.newInstance().  The Constructor object
 * would not be available if the constructor weren't public (per the
 * definition of Class.getConstructor), so we can skip the method access
 * check.  We can also safely assume the constructor isn't associated
 * with an interface, array, or primitive class.
 */
static jobject Constructor_newInstance(JNIEnv* env, jobject javaMethod, jobjectArray javaArgs) {
  ScopedObjectAccess soa(env);
  Method* m = soa.Decode<Object*>(javaMethod)->AsMethod();
  Class* c = m->GetDeclaringClass();
  if (c->IsAbstract()) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
        "Can't instantiate abstract class %s", PrettyDescriptor(c).c_str());
    return NULL;
  }

  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return NULL;
  }

  Object* receiver = c->AllocObject();
  if (receiver == NULL) {
    return NULL;
  }

  jobject javaReceiver = soa.AddLocalReference<jobject>(receiver);
  InvokeMethod(soa, javaMethod, javaReceiver, javaArgs);

  // Constructors are ()V methods, so we shouldn't touch the result of InvokeMethod.
  return javaReceiver;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Constructor, newInstance, "([Ljava/lang/Object;)Ljava/lang/Object;"),
};

void register_java_lang_reflect_Constructor(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Constructor");
}

}  // namespace art
