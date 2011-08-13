// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "class_linker.h"

#include <vector>
#include <utility>

#include "casts.h"
#include "dex_cache.h"
#include "dex_verifier.h"
#include "heap.h"
#include "logging.h"
#include "monitor.h"
#include "object.h"
#include "dex_file.h"
#include "scoped_ptr.h"
#include "thread.h"
#include "utils.h"

namespace art {

ClassLinker* ClassLinker::Create(const std::vector<DexFile*>& boot_class_path) {
  scoped_ptr<ClassLinker> class_linker(new ClassLinker);
  class_linker->Init(boot_class_path);
  // TODO: check for failure during initialization
  return class_linker.release();
}

void ClassLinker::Init(const std::vector<DexFile*>& boot_class_path) {
  init_done_ = false;

  // java_lang_Class comes first, its needed for AllocClass
  Class* java_lang_Class = down_cast<Class*>(Heap::AllocObject(NULL, sizeof(Class)));
  CHECK(java_lang_Class != NULL);
  java_lang_Class->descriptor_ = "Ljava/lang/Class;";
  java_lang_Class->object_size_ = sizeof(Class);
  java_lang_Class->klass_ = java_lang_Class;
  // AllocClass(Class*) can now be used

  // java_lang_Object comes next so that object_array_class can be created
  Class* java_lang_Object = AllocClass(java_lang_Class);
  CHECK(java_lang_Object != NULL);
  java_lang_Object->descriptor_ = "Ljava/lang/Object;";
  // backfill Object as the super class of Class
  java_lang_Class->super_class_ = java_lang_Object;

  // object_array_class is for root_classes to provide the storage for these classes
  Class* object_array_class = AllocClass(java_lang_Class);
  CHECK(object_array_class != NULL);

  // String and char[] are necessary so that FindClass can assign names to members
  Class* java_lang_String = AllocClass(java_lang_Class);
  CHECK(java_lang_String != NULL);
  java_lang_String->descriptor_ = "Ljava/lang/String;";
  CHECK_LT(java_lang_String->object_size_, sizeof(String));
  java_lang_String->object_size_ = sizeof(String);
  Class* char_array_class = AllocClass(java_lang_Class);
  CHECK(char_array_class != NULL);

  // int[] and long[] are used for static field storage
  Class* int_array_class = AllocClass(java_lang_Class);
  CHECK(int_array_class != NULL);
  Class* long_array_class = AllocClass(java_lang_Class);
  CHECK(long_array_class != NULL);

  // Field and Method are necessary so that FindClass can link members
  Class* java_lang_reflect_Field = AllocClass(java_lang_Class);
  CHECK(java_lang_reflect_Field != NULL);
  java_lang_reflect_Field->descriptor_ = "Ljava/lang/reflect/Field;";
  CHECK_LT(java_lang_reflect_Field->object_size_, sizeof(Field));
  java_lang_reflect_Field->object_size_ = sizeof(Field);
  Class* java_lang_reflect_Method = AllocClass(java_lang_Class);
  java_lang_reflect_Method->descriptor_ = "Ljava/lang/reflect/Method;";
  CHECK(java_lang_reflect_Method != NULL);
  CHECK_LT(java_lang_reflect_Method->object_size_, sizeof(Method));
  java_lang_reflect_Method->object_size_ = sizeof(Method);

  // create storage for root classes, save away our work so far
  class_roots_ = ObjectArray<Class>::Alloc(object_array_class, kClassRootsMax);
  class_roots_->Set(kJavaLangClass, java_lang_Class);
  class_roots_->Set(kJavaLangObject, java_lang_Object);
  class_roots_->Set(kObjectArrayClass, object_array_class);
  class_roots_->Set(kJavaLangString, java_lang_String);
  class_roots_->Set(kCharArrayClass, char_array_class);
  class_roots_->Set(kIntArrayClass, int_array_class);
  class_roots_->Set(kLongArrayClass, long_array_class);
  class_roots_->Set(kJavaLangReflectField, java_lang_reflect_Field);
  class_roots_->Set(kJavaLangReflectMethod, java_lang_reflect_Method);
  // now that these are registered, we can use AllocClass() and AllocObjectArray

  String::InitClasses(java_lang_String, char_array_class);
  // Now AllocString* can be used

  // setup boot_class_path_ now that we can use AllocObjectArray to
  // create DexCache instances
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    AppendToBootClassPath(boot_class_path[i]);
  }
  // now we can use FindSystemClass, at least for non-arrays classes.

  // run Class, Field, and Method through FindSystemClass.
  // this initializes their dex_cache_ fields and register them in classes_.
  // we also override their object_size_ values to accommodate the extra C++ fields.
  Class* Class_class = FindSystemClass(java_lang_Class->GetDescriptor());
  CHECK_EQ(java_lang_Class, Class_class);
  CHECK_LT(java_lang_Class->object_size_, sizeof(Class));
  java_lang_Class->object_size_ = sizeof(Class);
  Class* Field_class = FindSystemClass(java_lang_reflect_Field->GetDescriptor());
  CHECK_EQ(java_lang_reflect_Field, Field_class);
  CHECK_LT(java_lang_reflect_Field->object_size_, sizeof(Field));
  java_lang_reflect_Field->object_size_ = sizeof(Field);
  Class* Method_class = FindSystemClass(java_lang_reflect_Method->GetDescriptor());
  CHECK_EQ(java_lang_reflect_Method, Method_class);
  CHECK_LT(java_lang_reflect_Method->object_size_, sizeof(Method));
  java_lang_reflect_Method->object_size_ = sizeof(Method);

  // Object and String just need more minimal setup, since they do not have extra C++ fields.
  Class* Object_class = FindSystemClass(java_lang_Object->GetDescriptor());
  CHECK_EQ(java_lang_Object, Object_class);
  CHECK_EQ(java_lang_Object->object_size_, sizeof(Object));
  Class* String_class = FindSystemClass(java_lang_String->GetDescriptor());
  CHECK_EQ(java_lang_String, String_class);
  CHECK_EQ(java_lang_String->object_size_, sizeof(String));

  // Setup the ClassLoaders, adjusting the object_size_ as necessary
  Class* java_lang_ClassLoader = FindSystemClass("Ljava/lang/ClassLoader;");
  CHECK(java_lang_ClassLoader != NULL);
  CHECK_LT(java_lang_ClassLoader->object_size_, sizeof(ClassLoader));
  java_lang_ClassLoader->object_size_ = sizeof(ClassLoader);
  class_roots_->Set(kJavaLangClassLoader, java_lang_ClassLoader);
  Class* dalvik_system_BaseDexClassLoader = FindSystemClass("Ldalvik/system/BaseDexClassLoader;");
  CHECK(dalvik_system_BaseDexClassLoader != NULL);
  CHECK_EQ(dalvik_system_BaseDexClassLoader->object_size_, sizeof(BaseDexClassLoader));
  class_roots_->Set(kDalvikSystemBaseDexClassLoader, dalvik_system_BaseDexClassLoader);
  Class* dalvik_system_PathClassLoader = FindSystemClass("Ldalvik/system/PathClassLoader;");
  CHECK(dalvik_system_PathClassLoader != NULL);
  CHECK_EQ(dalvik_system_PathClassLoader->object_size_, sizeof(PathClassLoader));
  class_roots_->Set(kDalvikSystemPathClassLoader, dalvik_system_PathClassLoader);

  // Setup a single, global copy of "interfaces" and "iftable" for
  // reuse across array classes
  Class* java_lang_Cloneable = FindSystemClass("Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != NULL);
  Class* java_io_Serializable = FindSystemClass("Ljava/io/Serializable;");
  CHECK(java_io_Serializable != NULL);

  array_interfaces_ = AllocObjectArray<Class>(2);
  CHECK(array_interfaces_ != NULL);
  array_interfaces_->Set(0, java_lang_Cloneable);
  array_interfaces_->Set(1, java_io_Serializable);

  // We assume that Cloneable/Serializable don't have superinterfaces --
  // normally we'd have to crawl up and explicitly list all of the
  // supers as well.  These interfaces don't have any methods, so we
  // don't have to worry about the ifviPool either.
  array_iftable_ = new InterfaceEntry[2];
  array_iftable_[0].SetClass(array_interfaces_->Get(0));
  array_iftable_[1].SetClass(array_interfaces_->Get(1));
  // now FindClass can be used for non-primitive array classes

  // run Object[] through FindClass to complete initialization
  Class* found_object_array_class = FindSystemClass("[Ljava/lang/Object;");
  CHECK_EQ(object_array_class, found_object_array_class);
  CHECK_EQ(java_lang_Cloneable, object_array_class->GetInterface(0));
  CHECK_EQ(java_io_Serializable, object_array_class->GetInterface(1));

  // Setup the primitive type classes.
  class_roots_->Set(kPrimitiveByte, CreatePrimitiveClass("B"));
  class_roots_->Set(kPrimitiveChar, CreatePrimitiveClass("C"));
  class_roots_->Set(kPrimitiveDouble, CreatePrimitiveClass("D"));
  class_roots_->Set(kPrimitiveFloat, CreatePrimitiveClass("F"));
  class_roots_->Set(kPrimitiveInt, CreatePrimitiveClass("I"));
  class_roots_->Set(kPrimitiveLong, CreatePrimitiveClass("J"));
  class_roots_->Set(kPrimitiveShort, CreatePrimitiveClass("S"));
  class_roots_->Set(kPrimitiveBoolean, CreatePrimitiveClass("Z"));
  class_roots_->Set(kPrimitiveVoid, CreatePrimitiveClass("V"));
  // now we can use FindSystemClass for anything, including for "[C"

  // run char[], int[] and long[] through FindClass to complete initialization
  Class* found_char_array_class = FindSystemClass("[C");
  CHECK_EQ(char_array_class, found_char_array_class);
  Class* found_int_array_class = FindSystemClass("[I");
  CHECK_EQ(int_array_class, found_int_array_class);
  Class* found_long_array_class = FindSystemClass("[J");
  CHECK_EQ(long_array_class, found_long_array_class);

  // ensure all class_roots_ were initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    CHECK(GetClassRoot(static_cast<ClassRoot>(i)));
  }

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;
}

void ClassLinker::VisitRoots(Heap::RootVistor* root_visitor, void* arg) {
  root_visitor(class_roots_, arg);

  for (size_t i = 0; i < dex_caches_.size(); i++) {
      root_visitor(dex_caches_[i], arg);
  }

  {
    MutexLock mu(classes_lock_);
    typedef Table::const_iterator It; // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
        root_visitor(it->second, arg);
    }
  }

  intern_table_.VisitRoots(root_visitor, arg);

  root_visitor(array_interfaces_, arg);
}

DexCache* ClassLinker::AllocDexCache() {
  return down_cast<DexCache*>(AllocObjectArray<Object>(DexCache::kMax));
}

Class* ClassLinker::AllocClass(Class* java_lang_Class) {
  return down_cast<Class*>(java_lang_Class->NewInstance());
}

Class* ClassLinker::AllocClass() {
  return AllocClass(GetClassRoot(kJavaLangClass));
}

Field* ClassLinker::AllocField() {
  return down_cast<Field*>(GetClassRoot(kJavaLangReflectField)->NewInstance());
}

Method* ClassLinker::AllocMethod() {
  return down_cast<Method*>(GetClassRoot(kJavaLangReflectMethod)->NewInstance());
}

// TODO remove once we can use java.lang.Class.getSystemClassLoader
PathClassLoader* ClassLinker::AllocPathClassLoader(std::vector<const DexFile*> dex_files) {
  PathClassLoader* cl = down_cast<PathClassLoader*>(GetClassRoot(kDalvikSystemPathClassLoader)->NewInstance());
  cl->SetClassPath(dex_files);
  return cl;
}

Class* ClassLinker::FindClass(const StringPiece& descriptor,
                              ClassLoader* class_loader) {
  // TODO remove this contrived parent class loader check when we have a real ClassLoader.
  if (class_loader != NULL) {
    Class* klass = FindClass(descriptor, NULL);
    if (klass != NULL) {
      return klass;
    }
  }

  Thread* self = Thread::Current();
  DCHECK(self != NULL);
  CHECK(!self->IsExceptionPending());
  // Find the class in the loaded classes table.
  Class* klass = LookupClass(descriptor, class_loader);
  if (klass == NULL) {
    // Class is not yet loaded.
    if (descriptor[0] == '[') {
      return CreateArrayClass(descriptor, class_loader);
    }
    DexFile::ClassPath& class_path = ((class_loader != NULL) ? class_loader->GetClassPath() : boot_class_path_);
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, class_path);
    if (pair.second == NULL) {
      LG << "Class " << PrintableString(descriptor) << " not found in class loader " << class_loader;  // TODO: NoClassDefFoundError
      return NULL;
    }
    const DexFile& dex_file = *pair.first;
    const DexFile::ClassDef& dex_class_def = *pair.second;
    DexCache* dex_cache = FindDexCache(pair.first);
    // Load the class from the dex file.
    if (!init_done_) {
      // finish up init of hand crafted class_roots_
      if (descriptor == "Ljava/lang/Object;") {
        klass = GetClassRoot(kJavaLangObject);
      } else if (descriptor == "Ljava/lang/Class;") {
        klass = GetClassRoot(kJavaLangClass);
      } else if (descriptor == "Ljava/lang/String;") {
        klass = GetClassRoot(kJavaLangString);
      } else if (descriptor == "Ljava/lang/reflect/Field;") {
        klass = GetClassRoot(kJavaLangReflectField);
      } else if (descriptor == "Ljava/lang/reflect/Method;") {
        klass = GetClassRoot(kJavaLangReflectMethod);
      } else {
        klass = AllocClass();
      }
    } else {
      klass = AllocClass();
    }
    klass->dex_cache_ = dex_cache;
    LoadClass(dex_file, dex_class_def, klass, class_loader);
    // Check for a pending exception during load
    if (self->IsExceptionPending()) {
      // TODO: free native allocations in klass
      return NULL;
    }
    {
      ObjectLock lock(klass);
      klass->clinit_thread_id_ = self->GetId();
      // Add the newly loaded class to the loaded classes table.
      bool success = InsertClass(klass); // TODO just return collision
      if (!success) {
        // We may fail to insert if we raced with another thread.
        klass->clinit_thread_id_ = 0;
        // TODO: free native allocations in klass
        klass = LookupClass(descriptor, class_loader);
        CHECK(klass != NULL);
      } else {
        // Finish loading (if necessary) by finding parents
        if (!klass->IsLoaded() && !LoadSuperAndInterfaces(klass, dex_file)) {
          // Loading failed.
          // TODO: CHECK(self->IsExceptionPending());
          lock.NotifyAll();
          return NULL;
        }
        CHECK(klass->IsLoaded());
        // Link the class (if necessary)
        if (!klass->IsLinked() && !LinkClass(klass, dex_file)) {
          // Linking failed.
          // TODO: CHECK(self->IsExceptionPending());
          lock.NotifyAll();
          return NULL;
        }
        CHECK(klass->IsLinked());
      }
    }
  }
  // Link the class if it has not already been linked.
  if (!klass->IsLinked() && !klass->IsErroneous()) {
    ObjectLock lock(klass);
    // Check for circular dependencies between classes.
    if (!klass->IsLinked() && klass->clinit_thread_id_ == self->GetId()) {
      LG << "Recursive link";  // TODO: ClassCircularityError
      return NULL;
    }
    // Wait for the pending initialization to complete.
    while (!klass->IsLinked() && !klass->IsErroneous()) {
      lock.Wait();
    }
  }
  if (klass->IsErroneous()) {
    LG << "EarlierClassFailure";  // TODO: EarlierClassFailure
    return NULL;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsLinked());
  CHECK(!self->IsExceptionPending());
  return klass;
}

void ClassLinker::LoadClass(const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            Class* klass,
                            ClassLoader* class_loader) {
  CHECK(klass != NULL);
  CHECK(klass->dex_cache_ != NULL);
  CHECK_EQ(Class::kStatusNotReady, klass->status_);
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);

  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != NULL);

  klass->klass_ = GetClassRoot(kJavaLangClass);
  klass->descriptor_.set(descriptor);
  klass->descriptor_alloc_ = NULL;
  klass->access_flags_ = dex_class_def.access_flags_;
  klass->class_loader_ = class_loader;
  klass->primitive_type_ = Class::kPrimNot;
  klass->status_ = Class::kStatusIdx;

  klass->super_class_ = NULL;
  klass->super_class_idx_ = dex_class_def.superclass_idx_;

  size_t num_static_fields = header.static_fields_size_;
  size_t num_instance_fields = header.instance_fields_size_;
  size_t num_direct_methods = header.direct_methods_size_;
  size_t num_virtual_methods = header.virtual_methods_size_;

  klass->source_file_ = dex_file.dexGetSourceFile(dex_class_def);

  // Load class interfaces.
  LoadInterfaces(dex_file, dex_class_def, klass);

  // Load static fields.
  DCHECK(klass->sfields_ == NULL);
  if (num_static_fields != 0) {
    klass->sfields_ = AllocObjectArray<Field>(num_static_fields);
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumStaticFields(); ++i) {
      DexFile::Field dex_field;
      dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
      Field* sfield = AllocField();
      klass->SetStaticField(i, sfield);
      LoadField(dex_file, dex_field, klass, sfield);
    }
  }

  // Load instance fields.
  DCHECK(klass->ifields_ == NULL);
  if (num_instance_fields != 0) {
    // TODO: allocate on the object heap.
    klass->ifields_ = AllocObjectArray<Field>(num_instance_fields);
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumInstanceFields(); ++i) {
      DexFile::Field dex_field;
      dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
      Field* ifield = AllocField();
      klass->SetInstanceField(i, ifield);
      LoadField(dex_file, dex_field, klass, ifield);
    }
  }

  // Load direct methods.
  DCHECK(klass->direct_methods_ == NULL);
  if (num_direct_methods != 0) {
    // TODO: append direct methods to class object
    klass->direct_methods_ = AllocObjectArray<Method>(num_direct_methods);
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); ++i) {
      DexFile::Method dex_method;
      dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
      Method* meth = AllocMethod();
      klass->SetDirectMethod(i, meth);
      LoadMethod(dex_file, dex_method, klass, meth);
      // TODO: register maps
    }
  }

  // Load virtual methods.
  DCHECK(klass->virtual_methods_ == NULL);
  if (num_virtual_methods != 0) {
    // TODO: append virtual methods to class object
    klass->virtual_methods_ = AllocObjectArray<Method>(num_virtual_methods);
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      DexFile::Method dex_method;
      dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
      Method* meth = AllocMethod();
      klass->SetVirtualMethod(i, meth);
      LoadMethod(dex_file, dex_method, klass, meth);
      // TODO: register maps
    }
  }
}

void ClassLinker::LoadInterfaces(const DexFile& dex_file,
                                 const DexFile::ClassDef& dex_class_def,
                                 Class* klass) {
  const DexFile::TypeList* list = dex_file.GetInterfacesList(dex_class_def);
  if (list != NULL) {
    DCHECK(klass->interfaces_ == NULL);
    klass->interfaces_ = AllocObjectArray<Class>(list->Size());
    DCHECK(klass->interfaces_idx_ == NULL);
    klass->interfaces_idx_ = new uint32_t[list->Size()];
    for (size_t i = 0; i < list->Size(); ++i) {
      const DexFile::TypeItem& type_item = list->GetTypeItem(i);
      klass->interfaces_idx_[i] = type_item.type_idx_;
    }
  }
}

void ClassLinker::LoadField(const DexFile& dex_file,
                            const DexFile::Field& src,
                            Class* klass,
                            Field* dst) {
  const DexFile::FieldId& field_id = dex_file.GetFieldId(src.field_idx_);
  dst->declaring_class_ = klass;
  dst->name_ = ResolveString(klass, field_id.name_idx_, dex_file);
  dst->descriptor_.set(dex_file.dexStringByTypeIdx(field_id.type_idx_));
  // TODO: Assign dst->type_.
  dst->access_flags_ = src.access_flags_;
}

void ClassLinker::LoadMethod(const DexFile& dex_file,
                             const DexFile::Method& src,
                             Class* klass,
                             Method* dst) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(src.method_idx_);
  dst->declaring_class_ = klass;
  dst->name_ = ResolveString(klass, method_id.name_idx_, dex_file);
  {
    int32_t utf16_length;
    scoped_array<char> utf8(dex_file.CreateMethodDescriptor(method_id.proto_idx_,
                                                          &utf16_length));
    dst->descriptor_ = String::AllocFromModifiedUtf8(utf16_length, utf8.get());
  }
  dst->proto_idx_ = method_id.proto_idx_;
  dst->code_off_ = src.code_off_;
  dst->shorty_ = dex_file.GetShorty(method_id.proto_idx_);
  dst->access_flags_ = src.access_flags_;

  // TODO: check for finalize method

  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(src);
  if (code_item != NULL) {
    dst->num_registers_ = code_item->registers_size_;
    dst->num_ins_ = code_item->ins_size_;
    dst->num_outs_ = code_item->outs_size_;
  } else {
    uint16_t num_args = dst->NumArgRegisters();
    if (!dst->IsStatic()) {
      ++num_args;
    }
    dst->num_registers_ = dst->num_ins_ + num_args;
    // TODO: native methods
  }
}

void ClassLinker::AppendToBootClassPath(DexFile* dex_file) {
  CHECK(dex_file != NULL);
  boot_class_path_.push_back(dex_file);
  RegisterDexFile(dex_file);
}

void ClassLinker::RegisterDexFile(const DexFile* dex_file) {
  CHECK(dex_file != NULL);
  dex_files_.push_back(dex_file);
  DexCache* dex_cache = AllocDexCache();
  CHECK(dex_cache != NULL);
  dex_cache->Init(AllocObjectArray<String>(dex_file->NumStringIds()),
                  AllocObjectArray<Class>(dex_file->NumTypeIds()),
                  AllocObjectArray<Method>(dex_file->NumMethodIds()),
                  AllocObjectArray<Field>(dex_file->NumFieldIds()));
  dex_caches_.push_back(dex_cache);
}

const DexFile& ClassLinker::FindDexFile(const DexCache* dex_cache) const {
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    if (dex_caches_[i] == dex_cache) {
        return *dex_files_[i];
    }
  }
  CHECK(false) << "Could not find DexFile";
  return *dex_files_[-1];
}

DexCache* ClassLinker::FindDexCache(const DexFile* dex_file) const {
  for (size_t i = 0; i != dex_files_.size(); ++i) {
    if (dex_files_[i] == dex_file) {
        return dex_caches_[i];
    }
  }
  CHECK(false) << "Could not find DexCache";
  return NULL;
}

Class* ClassLinker::CreatePrimitiveClass(const StringPiece& descriptor) {
  Class* klass = AllocClass();
  CHECK(klass != NULL);
  klass->super_class_ = NULL;
  klass->access_flags_ = kAccPublic | kAccFinal | kAccAbstract;
  klass->descriptor_ = descriptor;
  klass->descriptor_alloc_ = NULL;
  klass->status_ = Class::kStatusInitialized;
  bool success = InsertClass(klass);
  CHECK(success) << "CreatePrimitiveClass(" << descriptor << ") failed";
  return klass;
}

// Create an array class (i.e. the class object for the array, not the
// array itself).  "descriptor" looks like "[C" or "[[[[B" or
// "[Ljava/lang/String;".
//
// If "descriptor" refers to an array of primitives, look up the
// primitive type's internally-generated class object.
//
// "loader" is the class loader of the class that's referring to us.  It's
// used to ensure that we're looking for the element type in the right
// context.  It does NOT become the class loader for the array class; that
// always comes from the base element class.
//
// Returns NULL with an exception raised on failure.
Class* ClassLinker::CreateArrayClass(const StringPiece& descriptor,
                                     ClassLoader* class_loader)
{
  CHECK(descriptor[0] == '[');

  // Identify the underlying element class and the array dimension depth.
  Class* component_type_ = NULL;
  int array_rank;
  if (descriptor[1] == '[') {
    // array of arrays; keep descriptor and grab stuff from parent
    Class* outer = FindClass(descriptor.substr(1), class_loader);
    if (outer != NULL) {
      // want the base class, not "outer", in our component_type_
      component_type_ = outer->component_type_;
      array_rank = outer->array_rank_ + 1;
    } else {
      DCHECK(component_type_ == NULL);  // make sure we fail
    }
  } else {
    array_rank = 1;
    if (descriptor[1] == 'L') {
      // array of objects; strip off "[" and look up descriptor.
      const StringPiece subDescriptor = descriptor.substr(1);
      component_type_ = FindClass(subDescriptor, class_loader);
    } else {
      // array of a primitive type
      component_type_ = FindPrimitiveClass(descriptor[1]);
    }
  }

  if (component_type_ == NULL) {
    // failed
    // DCHECK(Thread::Current()->IsExceptionPending());  // TODO
    return NULL;
  }

  // See if the component type is already loaded.  Array classes are
  // always associated with the class loader of their underlying
  // element type -- an array of Strings goes with the loader for
  // java/lang/String -- so we need to look for it there.  (The
  // caller should have checked for the existence of the class
  // before calling here, but they did so with *their* class loader,
  // not the component type's loader.)
  //
  // If we find it, the caller adds "loader" to the class' initiating
  // loader list, which should prevent us from going through this again.
  //
  // This call is unnecessary if "loader" and "component_type_->class_loader_"
  // are the same, because our caller (FindClass) just did the
  // lookup.  (Even if we get this wrong we still have correct behavior,
  // because we effectively do this lookup again when we add the new
  // class to the hash table --- necessary because of possible races with
  // other threads.)
  if (class_loader != component_type_->class_loader_) {
    Class* new_class = LookupClass(descriptor, component_type_->class_loader_);
    if (new_class != NULL) {
      return new_class;
    }
  }

  // Fill out the fields in the Class.
  //
  // It is possible to execute some methods against arrays, because
  // all arrays are subclasses of java_lang_Object_, so we need to set
  // up a vtable.  We can just point at the one in java_lang_Object_.
  //
  // Array classes are simple enough that we don't need to do a full
  // link step.

  Class* new_class = NULL;
  if (!init_done_) {
    if (descriptor == "[Ljava/lang/Object;") {
      new_class = GetClassRoot(kObjectArrayClass);
    } else if (descriptor == "[C") {
      new_class = GetClassRoot(kCharArrayClass);
    } else if (descriptor == "[I") {
      new_class = GetClassRoot(kIntArrayClass);
    } else if (descriptor == "[J") {
      new_class = GetClassRoot(kLongArrayClass);
    }
  }
  if (new_class == NULL) {
    new_class = AllocClass();
    if (new_class == NULL) {
      return NULL;
    }
  }
  new_class->descriptor_alloc_ = new std::string(descriptor.data(),
                                                 descriptor.size());
  new_class->descriptor_.set(new_class->descriptor_alloc_->data(),
                             new_class->descriptor_alloc_->size());
  Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->super_class_ = java_lang_Object;
  new_class->vtable_ = java_lang_Object->vtable_;
  new_class->primitive_type_ = Class::kPrimNot;
  new_class->component_type_ = component_type_;
  new_class->class_loader_ = component_type_->class_loader_;
  new_class->array_rank_ = array_rank;
  new_class->status_ = Class::kStatusInitialized;
  // don't need to set new_class->object_size_


  // All arrays have java/lang/Cloneable and java/io/Serializable as
  // interfaces.  We need to set that up here, so that stuff like
  // "instanceof" works right.
  //
  // Note: The GC could run during the call to FindSystemClass,
  // so we need to make sure the class object is GC-valid while we're in
  // there.  Do this by clearing the interface list so the GC will just
  // think that the entries are null.


  // Use the single, global copies of "interfaces" and "iftable"
  // (remember not to free them for arrays).
  DCHECK(array_interfaces_ != NULL);
  new_class->interfaces_ = array_interfaces_;
  new_class->iftable_count_ = 2;
  DCHECK(array_iftable_ != NULL);
  new_class->iftable_ = array_iftable_;

  // Inherit access flags from the component type.  Arrays can't be
  // used as a superclass or interface, so we want to add "final"
  // and remove "interface".
  //
  // Don't inherit any non-standard flags (e.g., kAccFinal)
  // from component_type_.  We assume that the array class does not
  // override finalize().
  new_class->access_flags_ = ((new_class->component_type_->access_flags_ &
                               ~kAccInterface) | kAccFinal) & kAccJavaFlagsMask;

  if (InsertClass(new_class)) {
    return new_class;
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  // Grab the winning class.
  Class* other_class = LookupClass(descriptor, component_type_->class_loader_);
  DCHECK(other_class != NULL);
  return other_class;
}

Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (type) {
    case 'B':
      return GetClassRoot(kPrimitiveByte);
    case 'C':
      return GetClassRoot(kPrimitiveChar);
    case 'D':
      return GetClassRoot(kPrimitiveDouble);
    case 'F':
      return GetClassRoot(kPrimitiveFloat);
    case 'I':
      return GetClassRoot(kPrimitiveInt);
    case 'J':
      return GetClassRoot(kPrimitiveLong);
    case 'S':
      return GetClassRoot(kPrimitiveShort);
    case 'Z':
      return GetClassRoot(kPrimitiveBoolean);
    case 'V':
      return GetClassRoot(kPrimitiveVoid);
    case 'L':
    case '[':
      LOG(ERROR) << "Not a primitive type " << PrintableChar(type);
    default:
      LOG(ERROR) << "Unknown primitive type " << PrintableChar(type);
  }
  return NULL;  // Not reachable.
}

bool ClassLinker::InsertClass(Class* klass) {
  MutexLock mu(classes_lock_);
  const StringPiece& key = klass->GetDescriptor();
  Table::iterator it = classes_.insert(std::make_pair(key, klass));
  return ((*it).second == klass);
}

Class* ClassLinker::LookupClass(const StringPiece& descriptor, ClassLoader* class_loader) {
  MutexLock mu(classes_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = classes_.find(descriptor), end = classes_.end(); it != end; ++it) {
    Class* klass = it->second;
    if (klass->descriptor_ == descriptor && klass->class_loader_ == class_loader) {
      return klass;
    }
  }
  return NULL;
}

bool ClassLinker::InitializeClass(Class* klass) {
  CHECK(klass->GetStatus() == Class::kStatusResolved ||
        klass->GetStatus() == Class::kStatusError);

  Thread* self = Thread::Current();

  {
    ObjectLock lock(klass);

    if (klass->GetStatus() < Class::kStatusVerified) {
      if (klass->IsErroneous()) {
        LG << "re-initializing failed class";  // TODO: throw
        return false;
      }

      CHECK(klass->GetStatus() == Class::kStatusResolved);

      klass->status_ = Class::kStatusVerifying;
      if (!DexVerify::VerifyClass(klass)) {
        LG << "Verification failed";  // TODO: ThrowVerifyError
        Object* exception = self->GetException();
        klass->SetVerifyErrorClass(exception->GetClass());
        klass->SetStatus(Class::kStatusError);
        return false;
      }

      klass->SetStatus(Class::kStatusVerified);
    }

    if (klass->status_ == Class::kStatusInitialized) {
      return true;
    }

    while (klass->status_ == Class::kStatusInitializing) {
      // we caught somebody else in the act; was it us?
      if (klass->clinit_thread_id_ == self->GetId()) {
        LG << "recursive <clinit>";
        return true;
      }

      CHECK(!self->IsExceptionPending());

      lock.Wait();  // TODO: check for interruption

      // When we wake up, repeat the test for init-in-progress.  If
      // there's an exception pending (only possible if
      // "interruptShouldThrow" was set), bail out.
      if (self->IsExceptionPending()) {
        CHECK(false);
        LG << "Exception in initialization.";  // TODO: ExceptionInInitializerError
        klass->SetStatus(Class::kStatusError);
        return false;
      }
      if (klass->GetStatus() == Class::kStatusInitializing) {
        continue;
      }
      DCHECK(klass->GetStatus() == Class::kStatusInitialized ||
             klass->GetStatus() == Class::kStatusError);
      if (klass->IsErroneous()) {
        // The caller wants an exception, but it was thrown in a
        // different thread.  Synthesize one here.
        LG << "<clinit> failed";  // TODO: throw UnsatisfiedLinkError
        return false;
      }
      return true;  // otherwise, initialized
    }

    // see if we failed previously
    if (klass->IsErroneous()) {
      // might be wise to unlock before throwing; depends on which class
      // it is that we have locked

      // TODO: throwEarlierClassFailure(klass);
      return false;
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(Class::kStatusError);
      return false;
    }

    DCHECK(klass->status_ < Class::kStatusInitializing);

    klass->clinit_thread_id_ = self->GetId();
    klass->status_ = Class::kStatusInitializing;
  }

  if (!InitializeSuperClass(klass)) {
    return false;
  }

  InitializeStaticFields(klass);

  Method* clinit = klass->FindDeclaredDirectMethod("<clinit>", "()V");
  if (clinit != NULL) {
  } else {
    // JValue unused;
    // TODO: dvmCallMethod(self, method, NULL, &unused);
    // UNIMPLEMENTED(FATAL);
  }

  {
    ObjectLock lock(klass);

    if (self->IsExceptionPending()) {
      klass->SetStatus(Class::kStatusError);
    } else {
      klass->SetStatus(Class::kStatusInitialized);
    }
    lock.NotifyAll();
  }

  return true;
}

bool ClassLinker::ValidateSuperClassDescriptors(const Class* klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // begin with the methods local to the superclass
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    const Class* super = klass->GetSuperClass();
    for (int i = super->NumVirtualMethods() - 1; i >= 0; --i) {
      const Method* method = klass->GetVirtualMethod(i);
      if (method != super->GetVirtualMethod(i) &&
          !HasSameMethodDescriptorClasses(method, super, klass)) {
        LG << "Classes resolve differently in superclass";
        return false;
      }
    }
  }
  for (size_t i = 0; i < klass->iftable_count_; ++i) {
    const InterfaceEntry* iftable = &klass->iftable_[i];
    Class* interface = iftable->GetClass();
    if (klass->GetClassLoader() != interface->GetClassLoader()) {
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        uint32_t vtable_index = iftable->method_index_array_[j];
        const Method* method = klass->GetVirtualMethod(vtable_index);
        if (!HasSameMethodDescriptorClasses(method, interface,
                                            method->GetClass())) {
          LG << "Classes resolve differently in interface";  // TODO: LinkageError
          return false;
        }
      }
    }
  }
  return true;
}

bool ClassLinker::HasSameMethodDescriptorClasses(const Method* method,
                                                 const Class* klass1,
                                                 const Class* klass2) {
  const DexFile& dex_file = FindDexFile(method->GetClass()->GetDexCache());
  const DexFile::ProtoId& proto_id = dex_file.GetProtoId(method->proto_idx_);
  DexFile::ParameterIterator *it;
  for (it = dex_file.GetParameterIterator(proto_id); it->HasNext(); it->Next()) {
    const char* descriptor = it->GetDescriptor();
    if (descriptor == NULL) {
      break;
    }
    if (descriptor[0] == 'L' || descriptor[0] == '[') {
      // Found a non-primitive type.
      if (!HasSameDescriptorClasses(descriptor, klass1, klass2)) {
        return false;
      }
    }
  }
  // Check the return type
  const char* descriptor = dex_file.GetReturnTypeDescriptor(proto_id);
  if (descriptor[0] == 'L' || descriptor[0] == '[') {
    if (HasSameDescriptorClasses(descriptor, klass1, klass2)) {
      return false;
    }
  }
  return true;
}

// Returns true if classes referenced by the descriptor are the
// same classes in klass1 as they are in klass2.
bool ClassLinker::HasSameDescriptorClasses(const char* descriptor,
                                           const Class* klass1,
                                           const Class* klass2) {
  CHECK(descriptor != NULL);
  CHECK(klass1 != NULL);
  CHECK(klass2 != NULL);
#if 0
  Class* found1 = FindClass(descriptor, klass1->GetClassLoader());
  // TODO: found1 == NULL
  Class* found2 = FindClass(descriptor, klass2->GetClassLoader());
  // TODO: found2 == NULL
  // TODO: lookup found1 in initiating loader list
  if (found1 == NULL || found2 == NULL) {
    Thread::Current()->ClearException();
    if (found1 == found2) {
      return true;
    } else {
      return false;
    }
  }
#endif
  return true;
}

bool ClassLinker::InitializeSuperClass(Class* klass) {
  CHECK(klass != NULL);
  MutexLock mu(classes_lock_);
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    Class* super_class = klass->GetSuperClass();
    if (super_class->GetStatus() != Class::kStatusInitialized) {
      CHECK(!super_class->IsInterface());
      klass->MonitorExit();
      bool super_initialized = InitializeClass(super_class);
      klass->MonitorEnter();
      // TODO: check for a pending exception
      if (!super_initialized) {
        klass->SetStatus(Class::kStatusError);
        klass->NotifyAll();
        return false;
      }
    }
  }
  return true;
}

void ClassLinker::InitializeStaticFields(Class* klass) {
  size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields == 0) {
    return;
  }
  DexCache* dex_cache = klass->GetDexCache();
  if (dex_cache == NULL) {
    return;
  }
  const StringPiece& descriptor = klass->GetDescriptor();
  const DexFile& dex_file = FindDexFile(dex_cache);
  const DexFile::ClassDef* dex_class_def = dex_file.FindClassDef(descriptor);
  CHECK(dex_class_def != NULL);
  const byte* addr = dex_file.GetEncodedArray(*dex_class_def);
  size_t array_size = DecodeUnsignedLeb128(&addr);
  for (size_t i = 0; i < array_size; ++i) {
    Field* field = klass->GetStaticField(i);
    JValue value;
    DexFile::ValueType type = dex_file.ReadEncodedValue(&addr, &value);
    switch (type) {
      case DexFile::kByte:
        field->SetByte(value.b);
        break;
      case DexFile::kShort:
        field->SetShort(value.s);
        break;
      case DexFile::kChar:
        field->SetChar(value.c);
        break;
      case DexFile::kInt:
        field->SetInt(value.i);
        break;
      case DexFile::kLong:
        field->SetLong(value.j);
        break;
      case DexFile::kFloat:
        field->SetFloat(value.f);
        break;
      case DexFile::kDouble:
        field->SetDouble(value.d);
        break;
      case DexFile::kString: {
        uint32_t string_idx = value.i;
        String* resolved = ResolveString(klass, string_idx, dex_file);
        field->SetObject(resolved);
        break;
      }
      case DexFile::kBoolean:
        field->SetBoolean(value.z);
        break;
      case DexFile::kNull:
        field->SetObject(value.l);
        break;
      default:
        LOG(FATAL) << "Unknown type " << static_cast<int>(type);
    }
  }
}

bool ClassLinker::LinkClass(Class* klass, const DexFile& dex_file) {
  CHECK_EQ(Class::kStatusLoaded, klass->status_);
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass)) {
    return false;
  }
  if (!LinkStaticFields(klass)) {
    return false;
  }
  if (!LinkInstanceFields(klass)) {
    return false;
  }
  CreateReferenceOffsets(klass);
  CHECK_EQ(Class::kStatusLoaded, klass->status_);
  klass->status_ = Class::kStatusResolved;
  return true;
}

bool ClassLinker::LoadSuperAndInterfaces(Class* klass, const DexFile& dex_file) {
  CHECK_EQ(Class::kStatusIdx, klass->status_);
  if (klass->super_class_idx_ != DexFile::kDexNoIndex) {
    Class* super_class = ResolveClass(klass, klass->super_class_idx_, dex_file);
    if (super_class == NULL) {
      LG << "Failed to resolve superclass";
      return false;
    }
    klass->super_class_ = super_class;  // TODO: write barrier
  }
  if (klass->NumInterfaces() > 0) {
    for (size_t i = 0; i < klass->NumInterfaces(); ++i) {
      uint32_t idx = klass->interfaces_idx_[i];
      klass->SetInterface(i, ResolveClass(klass, idx, dex_file));
      if (klass->GetInterface(i) == NULL) {
        LG << "Failed to resolve interface";
        return false;
      }
      // Verify
      if (!klass->CanAccess(klass->GetInterface(i))) {
        LG << "Inaccessible interface";
        return false;
      }
    }
  }
  // Mark the class as loaded.
  klass->status_ = Class::kStatusLoaded;
  return true;
}

bool ClassLinker::LinkSuperClass(Class* klass) {
  CHECK(!klass->IsPrimitive());
  const Class* super = klass->GetSuperClass();
  if (klass->GetDescriptor() == "Ljava/lang/Object;") {
    if (super != NULL) {
      LG << "Superclass must not be defined";  // TODO: ClassFormatError
      return false;
    }
    // TODO: clear finalize attribute
    return true;
  }
  if (super == NULL) {
    LG << "No superclass defined";  // TODO: LinkageError
    return false;
  }
  // Verify
  if (super->IsFinal()) {
    LG << "Superclass " << super->descriptor_ << " is declared final";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (super->IsInterface()) {
    LG << "Superclass " << super->descriptor_ << " is an interface";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (!klass->CanAccess(super)) {
    LG << "Superclass " << super->descriptor_ << " is  inaccessible";  // TODO: IllegalAccessError
    return false;
  }
  return true;
}

// Populate the class vtable and itable.
bool ClassLinker::LinkMethods(Class* klass) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      LG << "Too many methods on interface";  // TODO: VirtualMachineError
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethod(i)->method_index_ = i;
    }
  } else {
    // Link virtual method tables
    LinkVirtualMethods(klass);

    // Link interface method tables
    LinkInterfaceMethods(klass);

    // Insert stubs.
    LinkAbstractMethods(klass);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(Class* klass) {
  if (klass->HasSuperClass()) {
    uint32_t max_count = klass->NumVirtualMethods() + klass->GetSuperClass()->vtable_->GetLength();
    size_t actual_count = klass->GetSuperClass()->vtable_->GetLength();
    CHECK_LE(actual_count, max_count);
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->vtable_ = klass->GetSuperClass()->vtable_->CopyOf(max_count);
    // See if any of our virtual methods override the superclass.
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      Method* local_method = klass->GetVirtualMethod(i);
      size_t j = 0;
      for (; j < actual_count; ++j) {
        Method* super_method = klass->vtable_->Get(j);
        if (local_method->HasSameNameAndDescriptor(super_method)) {
          // Verify
          if (super_method->IsFinal()) {
            LG << "Method overrides final method";  // TODO: VirtualMachineError
            return false;
          }
          klass->vtable_->Set(j, local_method);
          local_method->method_index_ = j;
          break;
        }
      }
      if (j == actual_count) {
        // Not overriding, append.
        klass->vtable_->Set(actual_count, local_method);
        local_method->method_index_ = actual_count;
        actual_count += 1;
      }
    }
    if (!IsUint(16, actual_count)) {
      LG << "Too many methods defined on class";  // TODO: VirtualMachineError
      return false;
    }
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      // TODO: do not assign to the vtable field until it is fully constructed.
      klass->vtable_ = klass->vtable_->CopyOf(actual_count);
    }
  } else {
    CHECK(klass->GetDescriptor() == "Ljava/lang/Object;");
    uint32_t num_virtual_methods = klass->NumVirtualMethods();
    CHECK(klass->GetDescriptor() == "Ljava/lang/Object;");
    if (!IsUint(16, num_virtual_methods)) {
      LG << "Too many methods";  // TODO: VirtualMachineError
      return false;
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->vtable_ = AllocObjectArray<Method>(num_virtual_methods);
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      klass->vtable_->Set(i, klass->GetVirtualMethod(i));
      klass->GetVirtualMethod(i)->method_index_ = i & 0xFFFF;
    }
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(Class* klass) {
  int pool_offset = 0;
  int pool_size = 0;
  int miranda_count = 0;
  int miranda_alloc = 0;
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->iftable_count_;
  } else {
    super_ifcount = 0;
  }
  size_t ifcount = super_ifcount;
  ifcount += klass->NumInterfaces();
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    ifcount += klass->GetInterface(i)->iftable_count_;
  }
  if (ifcount == 0) {
    DCHECK(klass->iftable_count_ == 0);
    DCHECK(klass->iftable_ == NULL);
    return true;
  }
  klass->iftable_ = new InterfaceEntry[ifcount * sizeof(InterfaceEntry)];
  memset(klass->iftable_, 0x00, sizeof(InterfaceEntry) * ifcount);
  if (super_ifcount != 0) {
    memcpy(klass->iftable_, klass->GetSuperClass()->iftable_,
           sizeof(InterfaceEntry) * super_ifcount);
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    Class* interf = klass->GetInterface(i);
    DCHECK(interf != NULL);
    if (!interf->IsInterface()) {
      LG << "Class implements non-interface class";  // TODO: IncompatibleClassChangeError
      return false;
    }
    klass->iftable_[idx++].SetClass(interf);
    for (size_t j = 0; j < interf->iftable_count_; j++) {
      klass->iftable_[idx++].SetClass(interf->iftable_[j].GetClass());
    }
  }
  CHECK_EQ(idx, ifcount);
  klass->iftable_count_ = ifcount;
  if (klass->IsInterface() || super_ifcount == ifcount) {
    return true;
  }
  for (size_t i = super_ifcount; i < ifcount; i++) {
    pool_size += klass->iftable_[i].GetClass()->NumVirtualMethods();
  }
  if (pool_size == 0) {
    return true;
  }
  klass->ifvi_pool_count_ = pool_size;
  klass->ifvi_pool_ = new uint32_t[pool_size];
  std::vector<Method*> miranda_list;
  for (size_t i = super_ifcount; i < ifcount; ++i) {
    klass->iftable_[i].method_index_array_ = klass->ifvi_pool_ + pool_offset;
    Class* interface = klass->iftable_[i].GetClass();
    pool_offset += interface->NumVirtualMethods();    // end here
    for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
      Method* interface_method = interface->GetVirtualMethod(j);
      int k;  // must be signed
      for (k = klass->vtable_->GetLength() - 1; k >= 0; --k) {
        Method* vtable_method = klass->vtable_->Get(k);
        if (interface_method->HasSameNameAndDescriptor(vtable_method)) {
          if (!vtable_method->IsPublic()) {
            LG << "Implementation not public";
            return false;
          }
          klass->iftable_[i].method_index_array_[j] = k;
          break;
        }
      }
      if (k < 0) {
        if (miranda_count == miranda_alloc) {
          miranda_alloc += 8;
          if (miranda_list.empty()) {
            miranda_list.resize(miranda_alloc);
          } else {
            miranda_list.resize(miranda_alloc);
          }
        }
        int mir;
        for (mir = 0; mir < miranda_count; mir++) {
          Method* miranda_method = miranda_list[mir];
          if (miranda_method->HasSameNameAndDescriptor(interface_method)) {
            break;
          }
        }
        // point the interface table at a phantom slot index
        klass->iftable_[i].method_index_array_[j] = klass->vtable_->GetLength() + mir;
        if (mir == miranda_count) {
          miranda_list[miranda_count++] = interface_method;
        }
      }
    }
  }
  if (miranda_count != 0) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_count;
    klass->virtual_methods_ = klass->virtual_methods_->CopyOf(new_method_count);

    CHECK(klass->vtable_ != NULL);
    int old_vtable_count = klass->vtable_->GetLength();
    int new_vtable_count = old_vtable_count + miranda_count;
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->vtable_ = klass->vtable_->CopyOf(new_vtable_count);

    for (int i = 0; i < miranda_count; i++) {
      Method* meth = AllocMethod();
      memcpy(meth, miranda_list[i], sizeof(Method));
      meth->klass_ = klass;
      meth->access_flags_ |= kAccMiranda;
      meth->method_index_ = 0xFFFF & (old_vtable_count + i);
      klass->SetVirtualMethod(old_method_count + i, meth);
      klass->vtable_->Set(old_vtable_count + i, meth);
    }
  }
  return true;
}

void ClassLinker::LinkAbstractMethods(Class* klass) {
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (method->IsAbstract()) {
      LG << "AbstractMethodError";
      method->code_off_ = 0xFFFFFFFF;
      // TODO: throw AbstractMethodError
    }
  }
}

// Each static field will be stored in one of three arrays: static_references_,
// static_32bit_primitives_, or static_64bit_primitives_. This assigns each
// field a slot in its array and create the arrays.
bool ClassLinker::LinkStaticFields(Class* klass) {
  size_t next_reference_slot = 0;
  size_t next_32bit_primitive_slot = 0;
  size_t next_64bit_primitive_slot = 0;

  for (size_t i = 0; i < klass->NumStaticFields(); i++) {
    Field* field = klass->GetStaticField(i);
    char type = field->GetType();
    if (type == '[' || type == 'L') {
      field->offset_ = next_reference_slot++;
    } else if (type == 'J' || type == 'D') {
      field->offset_ = next_64bit_primitive_slot++;
    } else {
      field->offset_ = next_32bit_primitive_slot++;
    }
  }

  if (next_reference_slot > 0) {
    Class* array_class = GetClassRoot(kObjectArrayClass);
    klass->static_references_ = ObjectArray<Object>::Alloc(array_class, next_reference_slot);
  }
  if (next_32bit_primitive_slot > 0) {
    Class* array_class = GetClassRoot(kIntArrayClass);
    klass->static_32bit_primitives_ = IntArray::Alloc(array_class, next_32bit_primitive_slot);
  }
  if (next_64bit_primitive_slot > 0) {
    Class* array_class = GetClassRoot(kLongArrayClass);
    klass->static_64bit_primitives_ = LongArray::Alloc(array_class, next_64bit_primitive_slot);
  }

  return true;
}

bool ClassLinker::LinkInstanceFields(Class* klass) {
  int field_offset;
  if (klass->GetSuperClass() != NULL) {
    field_offset = klass->GetSuperClass()->object_size_;
  } else {
    field_offset = OFFSETOF_MEMBER(DataObject, fields_);
  }
  // Move references to the front.
  klass->num_reference_instance_fields_ = 0;
  size_t i = 0;
  for ( ; i < klass->NumInstanceFields(); i++) {
    Field* pField = klass->GetInstanceField(i);
    char c = pField->GetType();
    if (c != '[' && c != 'L') {
      for (size_t j = klass->NumInstanceFields() - 1; j > i; j--) {
        Field* refField = klass->GetInstanceField(j);
        char rc = refField->GetType();
        if (rc == '[' || rc == 'L') {
          klass->SetInstanceField(i, refField);
          klass->SetInstanceField(j, pField);
          pField = refField;
          c = rc;
          klass->num_reference_instance_fields_++;
          break;
        }
      }
    } else {
      klass->num_reference_instance_fields_++;
    }
    if (c != '[' && c != 'L') {
      break;
    }
    pField->SetOffset(field_offset);
    field_offset += sizeof(uint32_t);
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (i != klass->NumInstanceFields() && (field_offset & 0x04) != 0) {
    Field* pField = klass->GetInstanceField(i);
    char c = pField->GetType();

    if (c != 'J' && c != 'D') {
      // The field that comes next is 32-bit, so just advance past it.
      DCHECK(c != '[');
      DCHECK(c != 'L');
      pField->SetOffset(field_offset);
      field_offset += sizeof(uint32_t);
      i++;
    } else {
      // Next field is 64-bit, so search for a 32-bit field we can
      // swap into it.
      bool found = false;
      for (size_t j = klass->NumInstanceFields() - 1; j > i; j--) {
        Field* singleField = klass->GetInstanceField(j);
        char rc = singleField->GetType();
        if (rc != 'J' && rc != 'D') {
          klass->SetInstanceField(i, singleField);
          klass->SetInstanceField(j, pField);
          pField = singleField;
          pField->SetOffset(field_offset);
          field_offset += sizeof(uint32_t);
          found = true;
          i++;
          break;
        }
      }
      if (!found) {
        field_offset += sizeof(uint32_t);
      }
    }
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  DCHECK(i == klass->NumInstanceFields() || (field_offset & 0x04) == 0);
  for ( ; i < klass->NumInstanceFields(); i++) {
    Field* pField = klass->GetInstanceField(i);
    char c = pField->GetType();
    if (c != 'D' && c != 'J') {
      for (size_t j = klass->NumInstanceFields() - 1; j > i; j--) {
        Field* doubleField = klass->GetInstanceField(j);
        char rc = doubleField->GetType();
        if (rc == 'D' || rc == 'J') {
          klass->SetInstanceField(i, doubleField);
          klass->SetInstanceField(j, pField);
          pField = doubleField;
          c = rc;
          break;
        }
      }
    } else {
      // This is a double-wide field, leave it be.
    }

    pField->SetOffset(field_offset);
    field_offset += sizeof(uint32_t);
    if (c == 'J' || c == 'D')
      field_offset += sizeof(uint32_t);
  }

#ifndef NDEBUG
  // Make sure that all reference fields appear before
  // non-reference fields, and all double-wide fields are aligned.
  bool seen_non_ref = false;
  for (i = 0; i < klass->NumInstanceFields(); i++) {
    Field *pField = klass->GetInstanceField(i);
    char c = pField->GetType();

    if (c == 'D' || c == 'J') {
      DCHECK_EQ(0U, pField->GetOffset() & 0x07);
    }

    if (c != '[' && c != 'L') {
      if (!seen_non_ref) {
        seen_non_ref = true;
        DCHECK_EQ(klass->NumReferenceInstanceFields(), i);
      }
    } else {
      DCHECK(!seen_non_ref);
    }
  }
  if (!seen_non_ref) {
    DCHECK_EQ(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
  }
#endif
  klass->object_size_ = field_offset;
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceOffsets(Class* klass) {
  uint32_t reference_offsets = 0;
  if (klass->HasSuperClass()) {
    reference_offsets = klass->GetSuperClass()->GetReferenceOffsets();
  }
  // If our superclass overflowed, we don't stand a chance.
  if (reference_offsets != CLASS_WALK_SUPER) {
    // All of the fields that contain object references are guaranteed
    // to be at the beginning of the ifields list.
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); ++i) {
      // Note that, per the comment on struct InstField, f->byteOffset
      // is the offset from the beginning of obj, not the offset into
      // obj->instanceData.
      const Field* field = klass->GetInstanceField(i);
      size_t byte_offset = field->GetOffset();
      CHECK_GE(byte_offset, CLASS_SMALLEST_OFFSET);
      CHECK_EQ(byte_offset & (CLASS_OFFSET_ALIGNMENT - 1), 0U);
      if (CLASS_CAN_ENCODE_OFFSET(byte_offset)) {
        uint32_t new_bit = CLASS_BIT_FROM_OFFSET(byte_offset);
        CHECK_NE(new_bit, 0U);
        reference_offsets |= new_bit;
      } else {
        reference_offsets = CLASS_WALK_SUPER;
        break;
      }
    }
  }
  klass->SetReferenceOffsets(reference_offsets);
}

Class* ClassLinker::ResolveClass(const Class* referrer,
                                 uint32_t class_idx,
                                 const DexFile& dex_file) {
  DexCache* dex_cache = referrer->GetDexCache();
  Class* resolved = dex_cache->GetResolvedClass(class_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const char* descriptor = dex_file.dexStringByTypeIdx(class_idx);
  if (descriptor[0] != '\0' && descriptor[1] == '\0') {
    resolved = FindPrimitiveClass(descriptor[0]);
  } else {
    resolved = FindClass(descriptor, referrer->GetClassLoader());
  }
  if (resolved != NULL) {
    Class* check = resolved->IsArray() ? resolved->component_type_ : resolved;
    if (referrer->GetDexCache() != check->GetDexCache()) {
      if (check->GetClassLoader() != NULL) {
        LG << "Class resolved by unexpected DEX";  // TODO: IllegalAccessError
        return NULL;
      }
    }
    dex_cache->SetResolvedClass(class_idx, resolved);
  } else {
    DCHECK(Thread::Current()->IsExceptionPending());
  }
  return resolved;
}

Method* ResolveMethod(const Class* referrer, uint32_t method_idx,
                      /*MethodType*/ int method_type) {
  CHECK(false);
  return NULL;
}

String* ClassLinker::ResolveString(const Class* referring,
                                   uint32_t string_idx,
                                   const DexFile& dex_file) {
  const DexFile::StringId& string_id = dex_file.GetStringId(string_idx);
  int32_t utf16_length = dex_file.GetStringLength(string_id);
  const char* utf8_data = dex_file.GetStringData(string_id);
  String* string = intern_table_.Intern(utf16_length, utf8_data);
  referring->GetDexCache()->SetResolvedString(string_idx, string);
  return string;
}

}  // namespace art
