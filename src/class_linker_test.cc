// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"

#include <string>

#include "UniquePtr.h"
#include "common_test.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"

namespace art {

class ClassLinkerTest : public CommonTest {
 protected:
  void AssertNonExistentClass(const StringPiece& descriptor) {
    EXPECT_TRUE(class_linker_->FindSystemClass(descriptor) == NULL);
    Thread* self = Thread::Current();
    EXPECT_TRUE(self->IsExceptionPending());
    Object* exception = self->GetException();
    self->ClearException();
    Class* exception_class = class_linker_->FindSystemClass("Ljava/lang/NoClassDefFoundError;");
    EXPECT_TRUE(exception->InstanceOf(exception_class));
  }

  void AssertPrimitiveClass(const StringPiece& descriptor) {
    AssertPrimitiveClass(descriptor, class_linker_->FindSystemClass(descriptor));
  }

  void AssertPrimitiveClass(const StringPiece& descriptor, const Class* primitive) {
    ASSERT_TRUE(primitive != NULL);
    ASSERT_TRUE(primitive->GetClass() != NULL);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != NULL);
    ASSERT_TRUE(primitive->GetDescriptor()->Equals(descriptor));
    EXPECT_TRUE(primitive->GetSuperClass() == NULL);
    EXPECT_FALSE(primitive->HasSuperClass());
    EXPECT_TRUE(primitive->GetClassLoader() == NULL);
    EXPECT_TRUE(primitive->GetStatus() == Class::kStatusInitialized);
    EXPECT_FALSE(primitive->IsErroneous());
    EXPECT_TRUE(primitive->IsVerified());
    EXPECT_TRUE(primitive->IsLinked());
    EXPECT_FALSE(primitive->IsArrayInstance());
    EXPECT_FALSE(primitive->IsArrayClass());
    EXPECT_EQ(0, primitive->GetArrayRank());
    EXPECT_FALSE(primitive->IsInterface());
    EXPECT_TRUE(primitive->IsPublic());
    EXPECT_TRUE(primitive->IsFinal());
    EXPECT_TRUE(primitive->IsPrimitive());
    EXPECT_FALSE(primitive->IsSynthetic());
    EXPECT_EQ(0U, primitive->NumDirectMethods());
    EXPECT_EQ(0U, primitive->NumVirtualMethods());
    EXPECT_EQ(0U, primitive->NumInstanceFields());
    EXPECT_EQ(0U, primitive->NumStaticFields());
    EXPECT_EQ(0U, primitive->NumInterfaces());
  }

  void AssertArrayClass(const StringPiece& array_descriptor,
                        int32_t array_rank,
                        const StringPiece& component_type,
                        const ClassLoader* class_loader) {
    Class* array = class_linker_->FindClass(array_descriptor, class_loader);
    EXPECT_EQ(array_rank, array->GetArrayRank());
    EXPECT_TRUE(array->GetComponentType()->GetDescriptor()->Equals(component_type));
    EXPECT_EQ(class_loader, array->GetClassLoader());
    AssertArrayClass(array_descriptor, array);
  }

  void AssertArrayClass(const StringPiece& array_descriptor, Class* array) {
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_TRUE(array->GetDescriptor()->Equals(array_descriptor));
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != NULL);
    ASSERT_TRUE(array->GetComponentType()->GetDescriptor() != NULL);
    EXPECT_TRUE(array->GetStatus() == Class::kStatusInitialized);
    EXPECT_FALSE(array->IsErroneous());
    EXPECT_TRUE(array->IsVerified());
    EXPECT_TRUE(array->IsLinked());
    EXPECT_FALSE(array->IsArrayInstance());
    EXPECT_TRUE(array->IsArrayClass());
    EXPECT_LE(1, array->GetArrayRank());
    EXPECT_FALSE(array->IsInterface());
    EXPECT_EQ(array->GetComponentType()->IsPublic(), array->IsPublic());
    EXPECT_TRUE(array->IsFinal());
    EXPECT_FALSE(array->IsPrimitive());
    EXPECT_FALSE(array->IsSynthetic());
    EXPECT_EQ(0U, array->NumDirectMethods());
    EXPECT_EQ(0U, array->NumVirtualMethods());
    EXPECT_EQ(0U, array->NumInstanceFields());
    EXPECT_EQ(0U, array->NumStaticFields());
    EXPECT_EQ(2U, array->NumInterfaces());
  }

  void AssertMethod(Class* klass, Method* method) {
    EXPECT_TRUE(method != NULL);
    EXPECT_TRUE(method->GetName() != NULL);
    EXPECT_TRUE(method->GetSignature() != NULL);

    EXPECT_TRUE(method->GetDexCacheStrings() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedTypes() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedMethods() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedFields() != NULL);
    EXPECT_TRUE(method->GetDexCacheCodeAndDirectMethods() != NULL);
    EXPECT_TRUE(method->GetDexCacheInitializedStaticStorage() != NULL);
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetStrings(),
              method->GetDexCacheStrings());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedTypes(),
              method->GetDexCacheResolvedTypes());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedMethods(),
              method->GetDexCacheResolvedMethods());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedFields(),
              method->GetDexCacheResolvedFields());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetCodeAndDirectMethods(),
              method->GetDexCacheCodeAndDirectMethods());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetInitializedStaticStorage(),
              method->GetDexCacheInitializedStaticStorage());
  }

  void AssertField(Class* klass, Field* field) {
    EXPECT_TRUE(field != NULL);
    EXPECT_EQ(klass, field->GetDeclaringClass());
    EXPECT_TRUE(field->GetName() != NULL);
    EXPECT_TRUE(field->GetType() != NULL);
  }

  void AssertClass(const StringPiece& descriptor, Class* klass) {
    EXPECT_TRUE(klass->GetDescriptor()->Equals(descriptor));
    if (klass->GetDescriptor()->Equals(String::AllocFromModifiedUtf8("Ljava/lang/Object;"))) {
      EXPECT_FALSE(klass->HasSuperClass());
    } else {
      EXPECT_TRUE(klass->HasSuperClass());
      EXPECT_TRUE(klass->GetSuperClass() != NULL);
    }
    EXPECT_TRUE(klass->GetDexCache() != NULL);
    EXPECT_EQ(Class::kStatusResolved, klass->GetStatus());
    EXPECT_FALSE(klass->IsErroneous());
    EXPECT_FALSE(klass->IsVerified());
    EXPECT_TRUE(klass->IsLinked());
    EXPECT_TRUE(klass->IsLoaded());
    EXPECT_FALSE(klass->IsArrayClass());
    EXPECT_EQ(0, klass->GetArrayRank());
    EXPECT_TRUE(klass->IsInSamePackage(klass));
    EXPECT_TRUE(Class::IsInSamePackage(klass->GetDescriptor(), klass->GetDescriptor()));
    if (klass->IsInterface()) {
      EXPECT_TRUE(klass->IsAbstract());
      if (klass->NumDirectMethods() == 1) {
        EXPECT_TRUE(klass->GetDirectMethod(0)->GetName()->Equals("<clinit>"));
        EXPECT_TRUE(klass->GetDirectMethod(0)->IsDirect());
      } else {
        EXPECT_EQ(0U, klass->NumDirectMethods());
      }
    } else {
      if (!klass->IsSynthetic()) {
        EXPECT_NE(0U, klass->NumDirectMethods());
      }
    }
    if (klass->IsAbstract()) {
      EXPECT_FALSE(klass->IsFinal());
    } else {
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsFinal()) {
      EXPECT_FALSE(klass->IsAbstract());
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsAnnotation()) {
      EXPECT_FALSE(klass->IsFinal());
      EXPECT_TRUE(klass->IsAbstract());
    }

    EXPECT_FALSE(klass->IsPrimitive());
    EXPECT_TRUE(klass->CanAccess(klass));

    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      Method* method = klass->GetDirectMethod(i);
      AssertMethod(klass, method);
      EXPECT_TRUE(method->IsDirect());
      EXPECT_EQ(klass, method->GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      Method* method = klass->GetVirtualMethod(i);
      AssertMethod(klass, method);
      EXPECT_FALSE(method->IsDirect());
      EXPECT_TRUE(method->GetDeclaringClass()->IsAssignableFrom(klass));
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      AssertField(klass, field);
      EXPECT_FALSE(field->IsStatic());
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
      Field* field = klass->GetStaticField(i);
      AssertField(klass, field);
      EXPECT_TRUE(field->IsStatic());
   }

    // Confirm that all instances fields are packed together at the start
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      Class* field_type = field->GetType();
      ASSERT_TRUE(field_type != NULL);
      ASSERT_TRUE(!field_type->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      Class* field_type = field->GetType();
      ASSERT_TRUE(field_type != NULL);
      EXPECT_TRUE(field_type->IsPrimitive());
    }

    size_t total_num_reference_instance_fields = 0;
    Class* k = klass;
    while (k != NULL) {
      total_num_reference_instance_fields += k->NumReferenceInstanceFields();
      k = k->GetSuperClass();
    }
    EXPECT_EQ(klass->GetReferenceInstanceOffsets() == 0,
              total_num_reference_instance_fields == 0);
  }

  void AssertDexFileClass(ClassLoader* class_loader, const StringPiece& descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    ASSERT_TRUE(klass != NULL);
    EXPECT_TRUE(klass->GetDescriptor()->Equals(descriptor));
    EXPECT_EQ(class_loader, klass->GetClassLoader());
    if (klass->IsPrimitive()) {
      AssertPrimitiveClass(descriptor, klass);
    } else if (klass->IsArrayClass()) {
      AssertArrayClass(descriptor, klass);
    } else {
      AssertClass(descriptor, klass);
    }
  }

  void AssertDexFile(const DexFile* dex, ClassLoader* class_loader) {
    ASSERT_TRUE(dex != NULL);

    // Verify all the classes defined in this file
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      AssertDexFileClass(class_loader, descriptor);
    }
    // Verify all the types referenced by this file
    for (size_t i = 0; i < dex->NumTypeIds(); i++) {
      const DexFile::TypeId& type_id = dex->GetTypeId(i);
      const char* descriptor = dex->GetTypeDescriptor(type_id);
      AssertDexFileClass(class_loader, descriptor);
    }
    class_linker_->VisitRoots(TestRootVisitor, NULL);
  }

  static void TestRootVisitor(const Object* root, void* arg) {
    EXPECT_TRUE(root != NULL);
  }
};

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  AssertNonExistentClass("NoSuchClass;");
  AssertNonExistentClass("LNoSuchClass;");
}

TEST_F(ClassLinkerTest, FindClassNested) {
  const ClassLoader* class_loader = LoadDex("Nested");

  Class* outer = class_linker_->FindClass("LNested;", class_loader);
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_->FindClass("LNested$Inner;", class_loader);
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass_Primitives) {
  StringPiece expected = "BCDFIJSZV";
  for (int ch = 0; ch < 255; ch++) {
    char* s = reinterpret_cast<char*>(&ch);
    StringPiece descriptor(s, 1);
    if (expected.find(ch) == StringPiece::npos) {
      AssertNonExistentClass(descriptor);
    } else {
      AssertPrimitiveClass(descriptor);
    }
  }
}

TEST_F(ClassLinkerTest, FindClass) {
  Class* JavaLangObject = class_linker_->FindSystemClass("Ljava/lang/Object;");
  ASSERT_TRUE(JavaLangObject != NULL);
  ASSERT_TRUE(JavaLangObject->GetClass() != NULL);
  ASSERT_EQ(JavaLangObject->GetClass(), JavaLangObject->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
  ASSERT_TRUE(JavaLangObject->GetDescriptor()->Equals("Ljava/lang/Object;"));
  EXPECT_TRUE(JavaLangObject->GetSuperClass() == NULL);
  EXPECT_FALSE(JavaLangObject->HasSuperClass());
  EXPECT_TRUE(JavaLangObject->GetClassLoader() == NULL);
  EXPECT_FALSE(JavaLangObject->IsErroneous());
  EXPECT_FALSE(JavaLangObject->IsVerified());
  EXPECT_TRUE(JavaLangObject->IsLinked());
  EXPECT_FALSE(JavaLangObject->IsArrayInstance());
  EXPECT_FALSE(JavaLangObject->IsArrayClass());
  EXPECT_EQ(0, JavaLangObject->GetArrayRank());
  EXPECT_FALSE(JavaLangObject->IsInterface());
  EXPECT_TRUE(JavaLangObject->IsPublic());
  EXPECT_FALSE(JavaLangObject->IsFinal());
  EXPECT_FALSE(JavaLangObject->IsPrimitive());
  EXPECT_FALSE(JavaLangObject->IsSynthetic());
  EXPECT_EQ(2U, JavaLangObject->NumDirectMethods());
  EXPECT_EQ(11U, JavaLangObject->NumVirtualMethods());
  EXPECT_EQ(0U, JavaLangObject->NumInstanceFields());
  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, JavaLangObject->NumInterfaces());

  const ClassLoader* class_loader = LoadDex("MyClass");
  AssertNonExistentClass("LMyClass;");
  Class* MyClass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_TRUE(MyClass->GetDescriptor()->Equals("LMyClass;"));
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader, MyClass->GetClassLoader());
  EXPECT_TRUE(MyClass->GetStatus() == Class::kStatusResolved);
  EXPECT_FALSE(MyClass->IsErroneous());
  EXPECT_FALSE(MyClass->IsVerified());
  EXPECT_TRUE(MyClass->IsLinked());
  EXPECT_FALSE(MyClass->IsArrayInstance());
  EXPECT_FALSE(MyClass->IsArrayClass());
  EXPECT_EQ(0, JavaLangObject->GetArrayRank());
  EXPECT_FALSE(MyClass->IsInterface());
  EXPECT_FALSE(MyClass->IsPublic());
  EXPECT_FALSE(MyClass->IsFinal());
  EXPECT_FALSE(MyClass->IsPrimitive());
  EXPECT_FALSE(MyClass->IsSynthetic());
  EXPECT_EQ(1U, MyClass->NumDirectMethods());
  EXPECT_EQ(0U, MyClass->NumVirtualMethods());
  EXPECT_EQ(0U, MyClass->NumInstanceFields());
  EXPECT_EQ(0U, MyClass->NumStaticFields());
  EXPECT_EQ(0U, MyClass->NumInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", 1, "C", NULL);
  AssertArrayClass("[Ljava/lang/Object;", 1, "Ljava/lang/Object;", NULL);
  // synthesized on the fly
  AssertArrayClass("[[C", 2, "C", NULL);
  AssertArrayClass("[[[LMyClass;", 3, "LMyClass;", class_loader);
  // or not available at all
  AssertNonExistentClass("[[[[LNonExistentClass;");
}

TEST_F(ClassLinkerTest, LibCore) {
  AssertDexFile(java_lang_dex_file_.get(), NULL);
}

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkInstanceFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  Class* string = class_linker_->FindSystemClass( "Ljava/lang/String;");
  ASSERT_EQ(4U, string->NumInstanceFields());
  EXPECT_TRUE(string->GetInstanceField(0)->GetName()->Equals("value"));
  EXPECT_TRUE(string->GetInstanceField(1)->GetName()->Equals("hashCode"));
  EXPECT_TRUE(string->GetInstanceField(2)->GetName()->Equals("offset"));
  EXPECT_TRUE(string->GetInstanceField(3)->GetName()->Equals("count"));

  Class* throwable = class_linker_->FindSystemClass( "Ljava/lang/Throwable;");
  ASSERT_EQ(5U, throwable->NumInstanceFields());
  EXPECT_TRUE(throwable->GetInstanceField(0)->GetName()->Equals("cause"));
  EXPECT_TRUE(throwable->GetInstanceField(1)->GetName()->Equals("detailMessage"));
  EXPECT_TRUE(throwable->GetInstanceField(2)->GetName()->Equals("stackState"));
  EXPECT_TRUE(throwable->GetInstanceField(3)->GetName()->Equals("stackTrace"));
  EXPECT_TRUE(throwable->GetInstanceField(4)->GetName()->Equals("suppressedExceptions"));

  Class* stack_trace_element = class_linker_->FindSystemClass( "Ljava/lang/StackTraceElement;");
  ASSERT_EQ(4U, stack_trace_element->NumInstanceFields());
  EXPECT_TRUE(stack_trace_element->GetInstanceField(0)->GetName()->Equals("declaringClass"));
  EXPECT_TRUE(stack_trace_element->GetInstanceField(1)->GetName()->Equals("fileName"));
  EXPECT_TRUE(stack_trace_element->GetInstanceField(2)->GetName()->Equals("methodName"));
  EXPECT_TRUE(stack_trace_element->GetInstanceField(3)->GetName()->Equals("lineNumber"));

  Class* accessible_object = class_linker_->FindSystemClass("Ljava/lang/reflect/AccessibleObject;");
  ASSERT_EQ(1U, accessible_object->NumInstanceFields());
  EXPECT_TRUE(accessible_object->GetInstanceField(0)->GetName()->Equals("flag"));

  Class* field = class_linker_->FindSystemClass("Ljava/lang/reflect/Field;");
  ASSERT_EQ(6U, field->NumInstanceFields());
  EXPECT_TRUE(field->GetInstanceField(0)->GetName()->Equals("declaringClass"));
  EXPECT_TRUE(field->GetInstanceField(1)->GetName()->Equals("genericType"));
  EXPECT_TRUE(field->GetInstanceField(2)->GetName()->Equals("type"));
  EXPECT_TRUE(field->GetInstanceField(3)->GetName()->Equals("name"));
  EXPECT_TRUE(field->GetInstanceField(4)->GetName()->Equals("slot"));
  EXPECT_TRUE(field->GetInstanceField(5)->GetName()->Equals("genericTypesAreInitialized"));

  Class* method = class_linker_->FindSystemClass("Ljava/lang/reflect/Method;");
  ASSERT_EQ(11U, method->NumInstanceFields());
  EXPECT_TRUE(method->GetInstanceField( 0)->GetName()->Equals("declaringClass"));
  EXPECT_TRUE(method->GetInstanceField( 1)->GetName()->Equals("exceptionTypes"));
  EXPECT_TRUE(method->GetInstanceField( 2)->GetName()->Equals("formalTypeParameters"));
  EXPECT_TRUE(method->GetInstanceField( 3)->GetName()->Equals("genericExceptionTypes"));
  EXPECT_TRUE(method->GetInstanceField( 4)->GetName()->Equals("genericParameterTypes"));
  EXPECT_TRUE(method->GetInstanceField( 5)->GetName()->Equals("genericReturnType"));
  EXPECT_TRUE(method->GetInstanceField( 6)->GetName()->Equals("returnType"));
  EXPECT_TRUE(method->GetInstanceField( 7)->GetName()->Equals("name"));
  EXPECT_TRUE(method->GetInstanceField( 8)->GetName()->Equals("parameterTypes"));
  EXPECT_TRUE(method->GetInstanceField( 9)->GetName()->Equals("genericTypesAreInitialized"));
  EXPECT_TRUE(method->GetInstanceField(10)->GetName()->Equals("slot"));

  Class* class_loader = class_linker_->FindSystemClass("Ljava/lang/ClassLoader;");
  ASSERT_EQ(2U, class_loader->NumInstanceFields());
  EXPECT_TRUE(class_loader->GetInstanceField(0)->GetName()->Equals("packages"));
  EXPECT_TRUE(class_loader->GetInstanceField(1)->GetName()->Equals("parent"));

  Class* dex_base_class_loader = class_linker_->FindSystemClass("Ldalvik/system/BaseDexClassLoader;");
  ASSERT_EQ(2U, dex_base_class_loader->NumInstanceFields());
  EXPECT_TRUE(dex_base_class_loader->GetInstanceField(0)->GetName()->Equals("originalPath"));
  EXPECT_TRUE(dex_base_class_loader->GetInstanceField(1)->GetName()->Equals("pathList"));
}

// The first reference array element must be a multiple of 8 bytes from the
// start of the object
TEST_F(ClassLinkerTest, ValidateObjectArrayElementsOffset) {
  Class* array_class = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ObjectArray<String>* array = ObjectArray<String>::Alloc(array_class, 0);
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset =
      array_offset + ObjectArray<String>::DataOffset().Uint32Value();
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, ValidatePrimitiveArrayElementsOffset) {
  LongArray* array = LongArray::Alloc(0);
  EXPECT_EQ(class_linker_->FindSystemClass("[J"), array->GetClass());
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset = reinterpret_cast<uint32_t>(array->GetData());
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  const ClassLoader* class_loader_1 = LoadDex("MyClass");
  const ClassLoader* class_loader_2 = LoadDex("MyClass");
  Class* MyClass_1 = class_linker_->FindClass("LMyClass;", class_loader_1);
  Class* MyClass_2 = class_linker_->FindClass("LMyClass;", class_loader_2);
  EXPECT_TRUE(MyClass_1 != NULL);
  EXPECT_TRUE(MyClass_2 != NULL);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  // TODO: uncomment expectations of initial values when InitializeClass works
  const ClassLoader* class_loader = LoadDex("Statics");
  Class* statics = class_linker_->FindClass("LStatics;", class_loader);
  class_linker_->EnsureInitialized(statics);

  EXPECT_EQ(10U, statics->NumStaticFields());

  Field* s0 = statics->FindStaticField("s0", class_linker_->FindClass("Z", class_loader));
  EXPECT_TRUE(s0->GetClass()->GetDescriptor()->Equals("Ljava/lang/reflect/Field;"));
  EXPECT_TRUE(s0->GetType()->IsPrimitiveBoolean());
  // EXPECT_EQ(true, s0->GetBoolean(NULL)); // TODO: needs clinit to be run?
  s0->SetBoolean(NULL, false);

  Field* s1 = statics->FindStaticField("s1", class_linker_->FindClass("B", class_loader));
  EXPECT_TRUE(s1->GetType()->IsPrimitiveByte());
  // EXPECT_EQ(5, s1->GetByte(NULL));  // TODO: needs clinit to be run?
  s1->SetByte(NULL, 6);

  Field* s2 = statics->FindStaticField("s2", class_linker_->FindClass("C", class_loader));
  EXPECT_TRUE(s2->GetType()->IsPrimitiveChar());
  // EXPECT_EQ('a', s2->GetChar(NULL));  // TODO: needs clinit to be run?
  s2->SetChar(NULL, 'b');

  Field* s3 = statics->FindStaticField("s3", class_linker_->FindClass("S", class_loader));
  EXPECT_TRUE(s3->GetType()->IsPrimitiveShort());
  // EXPECT_EQ(65000, s3->GetShort(NULL));  // TODO: needs clinit to be run?
  s3->SetShort(NULL, 65001);

  Field* s4 = statics->FindStaticField("s4", class_linker_->FindClass("I", class_loader));
  EXPECT_TRUE(s4->GetType()->IsPrimitiveInt());
  // EXPECT_EQ(2000000000, s4->GetInt(NULL));  // TODO: needs clinit to be run?
  s4->SetInt(NULL, 2000000001);

  Field* s5 = statics->FindStaticField("s5", class_linker_->FindClass("J", class_loader));
  EXPECT_TRUE(s5->GetType()->IsPrimitiveLong());
  // EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong(NULL));  // TODO: needs clinit to be run?
  s5->SetLong(NULL, 0x34567890abcdef12LL);

  Field* s6 = statics->FindStaticField("s6", class_linker_->FindClass("F", class_loader));
  EXPECT_TRUE(s6->GetType()->IsPrimitiveFloat());
  // EXPECT_EQ(0.5, s6->GetFloat(NULL));  // TODO: needs clinit to be run?
  s6->SetFloat(NULL, 0.75);

  Field* s7 = statics->FindStaticField("s7", class_linker_->FindClass("D", class_loader));
  EXPECT_TRUE(s7->GetType()->IsPrimitiveDouble());
  // EXPECT_EQ(16777217, s7->GetDouble(NULL));  // TODO: needs clinit to be run?
  s7->SetDouble(NULL, 16777219);

  Field* s8 = statics->FindStaticField("s8", class_linker_->FindClass("Ljava/lang/Object;", class_loader));
  EXPECT_FALSE(s8->GetType()->IsPrimitive());
  // EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("android"));  // TODO: needs clinit to be run?
  s8->SetObject(NULL, String::AllocFromModifiedUtf8("robot"));

  Field* s9 = statics->FindStaticField("s9", class_linker_->FindClass("[Ljava/lang/Object;", class_loader));
  EXPECT_TRUE(s9->GetType()->IsArrayClass());
  // EXPECT_EQ(NULL, s9->GetObject(NULL));  // TODO: needs clinit to be run?
  s9->SetObject(NULL, NULL);

  EXPECT_EQ(false,                s0->GetBoolean(NULL));
  EXPECT_EQ(6,                    s1->GetByte(NULL));
  EXPECT_EQ('b',                  s2->GetChar(NULL));
  EXPECT_EQ(65001,                s3->GetShort(NULL));
  EXPECT_EQ(2000000001,           s4->GetInt(NULL));
  EXPECT_EQ(0x34567890abcdef12LL, s5->GetLong(NULL));
  EXPECT_EQ(0.75,                 s6->GetFloat(NULL));
  EXPECT_EQ(16777219,             s7->GetDouble(NULL));
  EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("robot"));
}

TEST_F(ClassLinkerTest, Interfaces) {
  const ClassLoader* class_loader = LoadDex("Interfaces");
  Class* I = class_linker_->FindClass("LInterfaces$I;", class_loader);
  Class* J = class_linker_->FindClass("LInterfaces$J;", class_loader);
  Class* K = class_linker_->FindClass("LInterfaces$K;", class_loader);
  Class* A = class_linker_->FindClass("LInterfaces$A;", class_loader);
  Class* B = class_linker_->FindClass("LInterfaces$B;", class_loader);
  EXPECT_TRUE(I->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(K));
  EXPECT_TRUE(K->IsAssignableFrom(B));
  EXPECT_TRUE(J->IsAssignableFrom(B));

  Method* Ii = I->FindVirtualMethod("i", "()V");
  Method* Jj1 = J->FindVirtualMethod("j1", "()V");
  Method* Jj2 = J->FindVirtualMethod("j2", "()V");
  Method* Kj1 = K->FindInterfaceMethod("j1", "()V");
  Method* Kj2 = K->FindInterfaceMethod("j2", "()V");
  Method* Kk = K->FindInterfaceMethod("k", "()V");
  Method* Ai = A->FindVirtualMethod("i", "()V");
  Method* Aj1 = A->FindVirtualMethod("j1", "()V");
  Method* Aj2 = A->FindVirtualMethod("j2", "()V");
  ASSERT_TRUE(Ii != NULL);
  ASSERT_TRUE(Jj1 != NULL);
  ASSERT_TRUE(Jj2 != NULL);
  ASSERT_TRUE(Kj1 != NULL);
  ASSERT_TRUE(Kj2 != NULL);
  ASSERT_TRUE(Kk != NULL);
  ASSERT_TRUE(Ai != NULL);
  ASSERT_TRUE(Aj1 != NULL);
  ASSERT_TRUE(Aj2 != NULL);
  EXPECT_NE(Ii, Ai);
  EXPECT_NE(Jj1, Aj1);
  EXPECT_NE(Jj2, Aj2);
  EXPECT_EQ(Kj1, Jj1);
  EXPECT_EQ(Kj2, Jj2);
  EXPECT_EQ(Ai, A->FindVirtualMethodForInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForInterface(Jj2));
  EXPECT_EQ(Ai, A->FindVirtualMethodForVirtualOrInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForVirtualOrInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForVirtualOrInterface(Jj2));
}

TEST_F(ClassLinkerTest, InitializeStaticStorageFromCode) {
  // pretend we are trying to get the static storage for the Statics class.

  // case 1, get the uninitialized storage from Statics.<clinit>
  // case 2, get the initialized storage from Statics.getS8

  const ClassLoader* class_loader = LoadDex("Statics");
  const DexFile* dex_file = ClassLoader::GetClassPath(class_loader)[0];
  CHECK(dex_file != NULL);

  Class* Statics = class_linker_->FindClass("LStatics;", class_loader);
  Method* clinit = Statics->FindDirectMethod("<clinit>", "()V");
  Method* getS8 = Statics->FindDirectMethod("getS8", "()Ljava/lang/Object;");
  uint32_t type_idx = FindTypeIdxByDescriptor(*dex_file, "LStatics;");

  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* uninit = class_linker_->InitializeStaticStorageFromCode(type_idx, clinit);
  EXPECT_TRUE(uninit != NULL);
  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* init = class_linker_->InitializeStaticStorageFromCode(type_idx, getS8);
  EXPECT_TRUE(init != NULL);
  EXPECT_EQ(init, clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx));
}

}  // namespace art
