// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "casts.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_verifier.h"
#include "heap.h"
#include "intern_table.h"
#include "leb128.h"
#include "logging.h"
#include "monitor.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support.h"
#include "ScopedLocalRef.h"
#include "space.h"
#include "stack_indirect_reference_table.h"
#include "stl_util.h"
#include "thread.h"
#include "UniquePtr.h"
#include "utils.h"

namespace art {

namespace {

void ThrowNoClassDefFoundError(const char* fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}

void ThrowClassFormatError(const char* fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void ThrowClassFormatError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/ClassFormatError;", fmt, args);
  va_end(args);
}

void ThrowLinkageError(const char* fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void ThrowLinkageError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/LinkageError;", fmt, args);
  va_end(args);
}

void ThrowNoSuchMethodError(const char* kind,
    Class* c, const StringPiece& name, const StringPiece& signature) {
  DexCache* dex_cache = c->GetDexCache();
  std::ostringstream msg;
  msg << "no " << kind << " method " << name << "." << signature
      << " in class " << c->GetDescriptor()->ToModifiedUtf8()
      << " or its superclasses";
  if (dex_cache) {
    msg << " (defined in " << dex_cache->GetLocation()->ToModifiedUtf8() << ")";
  }
  Thread::Current()->ThrowNewException("Ljava/lang/NoSuchMethodError;", msg.str().c_str());
}

void ThrowEarlierClassFailure(Class* c) {
  /*
   * The class failed to initialize on a previous attempt, so we want to throw
   * a NoClassDefFoundError (v2 2.17.5).  The exception to this rule is if we
   * failed in verification, in which case v2 5.4.1 says we need to re-throw
   * the previous error.
   */
  LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);

  if (c->GetVerifyErrorClass() != NULL) {
    // TODO: change the verifier to store an _instance_, with a useful detail message?
    std::string error_descriptor(c->GetVerifyErrorClass()->GetDescriptor()->ToModifiedUtf8());
    Thread::Current()->ThrowNewException(error_descriptor.c_str(),
        PrettyDescriptor(c->GetDescriptor()).c_str());
  } else {
    ThrowNoClassDefFoundError("%s", PrettyDescriptor(c->GetDescriptor()).c_str());
  }
}

void WrapExceptionInInitializer() {
  JNIEnv* env = Thread::Current()->GetJniEnv();

  ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
  CHECK(cause.get() != NULL);

  env->ExceptionClear();

  // TODO: add java.lang.Error to JniConstants?
  ScopedLocalRef<jclass> error_class(env, env->FindClass("java/lang/Error"));
  CHECK(error_class.get() != NULL);
  if (env->IsInstanceOf(cause.get(), error_class.get())) {
    // We only wrap non-Error exceptions; an Error can just be used as-is.
    env->Throw(cause.get());
    return;
  }

  // TODO: add java.lang.ExceptionInInitializerError to JniConstants?
  ScopedLocalRef<jclass> eiie_class(env, env->FindClass("java/lang/ExceptionInInitializerError"));
  CHECK(eiie_class.get() != NULL);

  jmethodID mid = env->GetMethodID(eiie_class.get(), "<init>" , "(Ljava/lang/Throwable;)V");
  CHECK(mid != NULL);

  ScopedLocalRef<jthrowable> eiie(env,
      reinterpret_cast<jthrowable>(env->NewObject(eiie_class.get(), mid, cause.get())));
  env->Throw(eiie.get());
}

}  // namespace

const char* ClassLinker::class_roots_descriptors_[] = {
  "Ljava/lang/Class;",
  "Ljava/lang/Object;",
  "[Ljava/lang/Class;",
  "[Ljava/lang/Object;",
  "Ljava/lang/String;",
  "Ljava/lang/ref/Reference;",
  "Ljava/lang/reflect/Constructor;",
  "Ljava/lang/reflect/Field;",
  "Ljava/lang/reflect/Method;",
  "Ljava/lang/reflect/Proxy;",
  "Ljava/lang/ClassLoader;",
  "Ldalvik/system/BaseDexClassLoader;",
  "Ldalvik/system/PathClassLoader;",
  "Ljava/lang/StackTraceElement;",
  "Z",
  "B",
  "C",
  "D",
  "F",
  "I",
  "J",
  "S",
  "V",
  "[Z",
  "[B",
  "[C",
  "[D",
  "[F",
  "[I",
  "[J",
  "[S",
  "[Ljava/lang/StackTraceElement;",
};

class ObjectLock {
 public:
  explicit ObjectLock(Object* object) : self_(Thread::Current()), obj_(object) {
    CHECK(object != NULL);
    obj_->MonitorEnter(self_);
  }

  ~ObjectLock() {
    obj_->MonitorExit(self_);
  }

  void Wait() {
    return Monitor::Wait(self_, obj_, 0, 0, false);
  }

  void Notify() {
    obj_->Notify();
  }

  void NotifyAll() {
    obj_->NotifyAll();
  }

 private:
  Thread* self_;
  Object* obj_;
  DISALLOW_COPY_AND_ASSIGN(ObjectLock);
};

ClassLinker* ClassLinker::Create(const std::string& boot_class_path,
                                 InternTable* intern_table) {
  CHECK_NE(boot_class_path.size(), 0U);
  UniquePtr<ClassLinker> class_linker(new ClassLinker(intern_table));
  class_linker->Init(boot_class_path);
  return class_linker.release();
}

ClassLinker* ClassLinker::Create(InternTable* intern_table) {
  UniquePtr<ClassLinker> class_linker(new ClassLinker(intern_table));
  class_linker->InitFromImage();
  return class_linker.release();
}

ClassLinker::ClassLinker(InternTable* intern_table)
    : dex_lock_("ClassLinker dex lock"),
      classes_lock_("ClassLinker classes lock"),
      class_roots_(NULL),
      array_interfaces_(NULL),
      array_iftable_(NULL),
      init_done_(false),
      intern_table_(intern_table) {
  CHECK_EQ(arraysize(class_roots_descriptors_), size_t(kClassRootsMax));
}

void CreateClassPath(const std::string& class_path,
                     std::vector<const DexFile*>& class_path_vector) {
  std::vector<std::string> parsed;
  Split(class_path, ':', parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    const DexFile* dex_file = DexFile::Open(parsed[i], Runtime::Current()->GetHostPrefix());
    if (dex_file == NULL) {
      LOG(WARNING) << "Failed to open dex file " << parsed[i];
    } else {
      class_path_vector.push_back(dex_file);
    }
  }
}

void ClassLinker::Init(const std::string& boot_class_path) {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::InitFrom entering boot_class_path=" << boot_class_path;
  }

  CHECK(!init_done_);

  // java_lang_Class comes first, it's needed for AllocClass
  SirtRef<Class> java_lang_Class(down_cast<Class*>(Heap::AllocObject(NULL, sizeof(ClassClass))));
  CHECK(java_lang_Class.get() != NULL);
  java_lang_Class->SetClass(java_lang_Class.get());
  java_lang_Class->SetClassSize(sizeof(ClassClass));
  // AllocClass(Class*) can now be used

  // Class[] is used for reflection support.
  SirtRef<Class> class_array_class(AllocClass(java_lang_Class.get(), sizeof(Class)));
  class_array_class->SetComponentType(java_lang_Class.get());

  // java_lang_Object comes next so that object_array_class can be created
  SirtRef<Class> java_lang_Object(AllocClass(java_lang_Class.get(), sizeof(Class)));
  CHECK(java_lang_Object.get() != NULL);
  // backfill Object as the super class of Class
  java_lang_Class->SetSuperClass(java_lang_Object.get());
  java_lang_Object->SetStatus(Class::kStatusLoaded);

  // Object[] next to hold class roots
  SirtRef<Class> object_array_class(AllocClass(java_lang_Class.get(), sizeof(Class)));
  object_array_class->SetComponentType(java_lang_Object.get());

  // Setup the char class to be used for char[]
  SirtRef<Class> char_class(AllocClass(java_lang_Class.get(), sizeof(Class)));

  // Setup the char[] class to be used for String
  SirtRef<Class> char_array_class(AllocClass(java_lang_Class.get(), sizeof(Class)));
  char_array_class->SetComponentType(char_class.get());
  CharArray::SetArrayClass(char_array_class.get());

  // Setup String
  SirtRef<Class> java_lang_String(AllocClass(java_lang_Class.get(), sizeof(StringClass)));
  String::SetClass(java_lang_String.get());
  java_lang_String->SetObjectSize(sizeof(String));
  java_lang_String->SetStatus(Class::kStatusResolved);

  // Backfill Class descriptors missing until this point
  java_lang_Class->SetDescriptor(intern_table_->InternStrong("Ljava/lang/Class;"));
  java_lang_Object->SetDescriptor(intern_table_->InternStrong("Ljava/lang/Object;"));
  class_array_class->SetDescriptor(intern_table_->InternStrong("[Ljava/lang/Class;"));
  object_array_class->SetDescriptor(intern_table_->InternStrong("[Ljava/lang/Object;"));
  java_lang_String->SetDescriptor(intern_table_->InternStrong("Ljava/lang/String;"));
  char_array_class->SetDescriptor(intern_table_->InternStrong("[C"));

  // Create storage for root classes, save away our work so far (requires
  // descriptors)
  class_roots_ = ObjectArray<Class>::Alloc(object_array_class.get(), kClassRootsMax);
  CHECK(class_roots_ != NULL);
  SetClassRoot(kJavaLangClass, java_lang_Class.get());
  SetClassRoot(kJavaLangObject, java_lang_Object.get());
  SetClassRoot(kClassArrayClass, class_array_class.get());
  SetClassRoot(kObjectArrayClass, object_array_class.get());
  SetClassRoot(kCharArrayClass, char_array_class.get());
  SetClassRoot(kJavaLangString, java_lang_String.get());

  // Setup the primitive type classes.
  SetClassRoot(kPrimitiveBoolean, CreatePrimitiveClass("Z", Primitive::kPrimBoolean));
  SetClassRoot(kPrimitiveByte, CreatePrimitiveClass("B", Primitive::kPrimByte));
  SetClassRoot(kPrimitiveShort, CreatePrimitiveClass("S", Primitive::kPrimShort));
  SetClassRoot(kPrimitiveInt, CreatePrimitiveClass("I", Primitive::kPrimInt));
  SetClassRoot(kPrimitiveLong, CreatePrimitiveClass("J", Primitive::kPrimLong));
  SetClassRoot(kPrimitiveFloat, CreatePrimitiveClass("F", Primitive::kPrimFloat));
  SetClassRoot(kPrimitiveDouble, CreatePrimitiveClass("D", Primitive::kPrimDouble));
  SetClassRoot(kPrimitiveVoid, CreatePrimitiveClass("V", Primitive::kPrimVoid));

  // Create array interface entries to populate once we can load system classes
  array_interfaces_ = AllocClassArray(2);
  array_iftable_ = AllocObjectArray<InterfaceEntry>(2);

  // Create int array type for AllocDexCache (done in AppendToBootClassPath)
  SirtRef<Class> int_array_class(AllocClass(java_lang_Class.get(), sizeof(Class)));
  int_array_class->SetDescriptor(intern_table_->InternStrong("[I"));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  IntArray::SetArrayClass(int_array_class.get());
  SetClassRoot(kIntArrayClass, int_array_class.get());

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // setup boot_class_path_ and register class_path now that we can
  // use AllocObjectArray to create DexCache instances
  std::vector<const DexFile*> boot_class_path_vector;
  CreateClassPath(boot_class_path, boot_class_path_vector);
  CHECK_NE(0U, boot_class_path_vector.size());
  for (size_t i = 0; i != boot_class_path_vector.size(); ++i) {
    const DexFile* dex_file = boot_class_path_vector[i];
    CHECK(dex_file != NULL);
    AppendToBootClassPath(*dex_file);
  }

  // Constructor, Field, and Method are necessary so that FindClass can link members
  SirtRef<Class> java_lang_reflect_Constructor(AllocClass(java_lang_Class.get(), sizeof(MethodClass)));
  java_lang_reflect_Constructor->SetDescriptor(intern_table_->InternStrong("Ljava/lang/reflect/Constructor;"));
  CHECK(java_lang_reflect_Constructor.get() != NULL);
  java_lang_reflect_Constructor->SetObjectSize(sizeof(Method));
  SetClassRoot(kJavaLangReflectConstructor, java_lang_reflect_Constructor.get());
  java_lang_reflect_Constructor->SetStatus(Class::kStatusResolved);

  SirtRef<Class> java_lang_reflect_Field(AllocClass(java_lang_Class.get(), sizeof(FieldClass)));
  CHECK(java_lang_reflect_Field.get() != NULL);
  java_lang_reflect_Field->SetDescriptor(intern_table_->InternStrong("Ljava/lang/reflect/Field;"));
  java_lang_reflect_Field->SetObjectSize(sizeof(Field));
  SetClassRoot(kJavaLangReflectField, java_lang_reflect_Field.get());
  java_lang_reflect_Field->SetStatus(Class::kStatusResolved);
  Field::SetClass(java_lang_reflect_Field.get());

  SirtRef<Class> java_lang_reflect_Method(AllocClass(java_lang_Class.get(), sizeof(MethodClass)));
  java_lang_reflect_Method->SetDescriptor(intern_table_->InternStrong("Ljava/lang/reflect/Method;"));
  CHECK(java_lang_reflect_Method.get() != NULL);
  java_lang_reflect_Method->SetObjectSize(sizeof(Method));
  SetClassRoot(kJavaLangReflectMethod, java_lang_reflect_Method.get());
  java_lang_reflect_Method->SetStatus(Class::kStatusResolved);
  Method::SetClasses(java_lang_reflect_Constructor.get(), java_lang_reflect_Method.get());

  // now we can use FindSystemClass

  // run char class through InitializePrimitiveClass to finish init
  InitializePrimitiveClass(char_class.get(), "C", Primitive::kPrimChar);
  SetClassRoot(kPrimitiveChar, char_class.get());  // needs descriptor

  // Object and String need to be rerun through FindSystemClass to finish init
  java_lang_Object->SetStatus(Class::kStatusNotReady);
  Class* Object_class = FindSystemClass("Ljava/lang/Object;");
  CHECK_EQ(java_lang_Object.get(), Object_class);
  CHECK_EQ(java_lang_Object->GetObjectSize(), sizeof(Object));
  java_lang_String->SetStatus(Class::kStatusNotReady);
  Class* String_class = FindSystemClass("Ljava/lang/String;");
  CHECK_EQ(java_lang_String.get(), String_class);
  CHECK_EQ(java_lang_String->GetObjectSize(), sizeof(String));

  // Setup the primitive array type classes - can't be done until Object has a vtable
  SetClassRoot(kBooleanArrayClass, FindSystemClass("[Z"));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass("[B"));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  Class* found_char_array_class = FindSystemClass("[C");
  CHECK_EQ(char_array_class.get(), found_char_array_class);

  SetClassRoot(kShortArrayClass, FindSystemClass("[S"));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  Class* found_int_array_class = FindSystemClass("[I");
  CHECK_EQ(int_array_class.get(), found_int_array_class);

  SetClassRoot(kLongArrayClass, FindSystemClass("[J"));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));

  SetClassRoot(kFloatArrayClass, FindSystemClass("[F"));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass("[D"));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  Class* found_class_array_class = FindSystemClass("[Ljava/lang/Class;");
  CHECK_EQ(class_array_class.get(), found_class_array_class);

  Class* found_object_array_class = FindSystemClass("[Ljava/lang/Object;");
  CHECK_EQ(object_array_class.get(), found_object_array_class);

  // Setup the single, global copies of "interfaces" and "iftable"
  Class* java_lang_Cloneable = FindSystemClass("Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != NULL);
  Class* java_io_Serializable = FindSystemClass("Ljava/io/Serializable;");
  CHECK(java_io_Serializable != NULL);
  CHECK(array_interfaces_ != NULL);
  array_interfaces_->Set(0, java_lang_Cloneable);
  array_interfaces_->Set(1, java_io_Serializable);
  // We assume that Cloneable/Serializable don't have superinterfaces --
  // normally we'd have to crawl up and explicitly list all of the
  // supers as well.
  array_iftable_->Set(0, AllocInterfaceEntry(array_interfaces_->Get(0)));
  array_iftable_->Set(1, AllocInterfaceEntry(array_interfaces_->Get(1)));

  // Sanity check Class[] and Object[]'s interfaces
  CHECK_EQ(java_lang_Cloneable, class_array_class->GetInterface(0));
  CHECK_EQ(java_io_Serializable, class_array_class->GetInterface(1));
  CHECK_EQ(java_lang_Cloneable, object_array_class->GetInterface(0));
  CHECK_EQ(java_io_Serializable, object_array_class->GetInterface(1));

  // run Class, Constructor, Field, and Method through FindSystemClass.
  // this initializes their dex_cache_ fields and register them in classes_.
  Class* Class_class = FindSystemClass("Ljava/lang/Class;");
  CHECK_EQ(java_lang_Class.get(), Class_class);

  java_lang_reflect_Constructor->SetStatus(Class::kStatusNotReady);
  Class* Constructor_class = FindSystemClass("Ljava/lang/reflect/Constructor;");
  CHECK_EQ(java_lang_reflect_Constructor.get(), Constructor_class);

  java_lang_reflect_Field->SetStatus(Class::kStatusNotReady);
  Class* Field_class = FindSystemClass("Ljava/lang/reflect/Field;");
  CHECK_EQ(java_lang_reflect_Field.get(), Field_class);

  java_lang_reflect_Method->SetStatus(Class::kStatusNotReady);
  Class* Method_class = FindSystemClass("Ljava/lang/reflect/Method;");
  CHECK_EQ(java_lang_reflect_Method.get(), Method_class);

  // End of special init trickery, subsequent classes may be loaded via FindSystemClass

  // Create java.lang.reflect.Proxy root
  Class* java_lang_reflect_Proxy = FindSystemClass("Ljava/lang/reflect/Proxy;");
  SetClassRoot(kJavaLangReflectProxy, java_lang_reflect_Proxy);

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
  Class* java_lang_ref_Reference = FindSystemClass("Ljava/lang/ref/Reference;");
  SetClassRoot(kJavaLangRefReference, java_lang_ref_Reference);
  Class* java_lang_ref_FinalizerReference = FindSystemClass("Ljava/lang/ref/FinalizerReference;");
  java_lang_ref_FinalizerReference->SetAccessFlags(
      java_lang_ref_FinalizerReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsFinalizerReference);
  Class* java_lang_ref_PhantomReference = FindSystemClass("Ljava/lang/ref/PhantomReference;");
  java_lang_ref_PhantomReference->SetAccessFlags(
      java_lang_ref_PhantomReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsPhantomReference);
  Class* java_lang_ref_SoftReference = FindSystemClass("Ljava/lang/ref/SoftReference;");
  java_lang_ref_SoftReference->SetAccessFlags(
      java_lang_ref_SoftReference->GetAccessFlags() | kAccClassIsReference);
  Class* java_lang_ref_WeakReference = FindSystemClass("Ljava/lang/ref/WeakReference;");
  java_lang_ref_WeakReference->SetAccessFlags(
      java_lang_ref_WeakReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsWeakReference);

  // Setup the ClassLoaders, verifying the object_size_
  Class* java_lang_ClassLoader = FindSystemClass("Ljava/lang/ClassLoader;");
  CHECK_EQ(java_lang_ClassLoader->GetObjectSize(), sizeof(ClassLoader));
  SetClassRoot(kJavaLangClassLoader, java_lang_ClassLoader);

  Class* dalvik_system_BaseDexClassLoader = FindSystemClass("Ldalvik/system/BaseDexClassLoader;");
  CHECK_EQ(dalvik_system_BaseDexClassLoader->GetObjectSize(), sizeof(BaseDexClassLoader));
  SetClassRoot(kDalvikSystemBaseDexClassLoader, dalvik_system_BaseDexClassLoader);

  Class* dalvik_system_PathClassLoader = FindSystemClass("Ldalvik/system/PathClassLoader;");
  CHECK_EQ(dalvik_system_PathClassLoader->GetObjectSize(), sizeof(PathClassLoader));
  SetClassRoot(kDalvikSystemPathClassLoader, dalvik_system_PathClassLoader);
  PathClassLoader::SetClass(dalvik_system_PathClassLoader);

  // Set up java.lang.StackTraceElement as a convenience
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass("Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass, FindSystemClass("[Ljava/lang/StackTraceElement;"));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();

  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::InitFrom exiting";
  }
}

void ClassLinker::FinishInit() {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::FinishInit entering";
  }

  // Let the heap know some key offsets into java.lang.ref instances
  // Note: we hard code the field indexes here rather than using FindInstanceField
  // as the types of the field can't be resolved prior to the runtime being
  // fully initialized
  Class* java_lang_ref_Reference = GetClassRoot(kJavaLangRefReference);
  Class* java_lang_ref_ReferenceQueue = FindSystemClass("Ljava/lang/ref/ReferenceQueue;");
  Class* java_lang_ref_FinalizerReference = FindSystemClass("Ljava/lang/ref/FinalizerReference;");

  Heap::SetWellKnownClasses(java_lang_ref_FinalizerReference, java_lang_ref_ReferenceQueue);

  Field* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  CHECK(pendingNext->GetName()->Equals("pendingNext"));
  CHECK_EQ(ResolveType(pendingNext->GetTypeIdx(), pendingNext), java_lang_ref_Reference);

  Field* queue = java_lang_ref_Reference->GetInstanceField(1);
  CHECK(queue->GetName()->Equals("queue"));
  CHECK_EQ(ResolveType(queue->GetTypeIdx(), queue), java_lang_ref_ReferenceQueue);

  Field* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  CHECK(queueNext->GetName()->Equals("queueNext"));
  CHECK_EQ(ResolveType(queueNext->GetTypeIdx(), queueNext), java_lang_ref_Reference);

  Field* referent = java_lang_ref_Reference->GetInstanceField(3);
  CHECK(referent->GetName()->Equals("referent"));
  CHECK_EQ(ResolveType(referent->GetTypeIdx(), referent), GetClassRoot(kJavaLangObject));

  Field* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  CHECK(zombie->GetName()->Equals("zombie"));
  CHECK_EQ(ResolveType(zombie->GetTypeIdx(), zombie), GetClassRoot(kJavaLangObject));

  Heap::SetReferenceOffsets(referent->GetOffset(),
                            queue->GetOffset(),
                            queueNext->GetOffset(),
                            pendingNext->GetOffset(),
                            zombie->GetOffset());

  // ensure all class_roots_ are initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    Class* klass = GetClassRoot(class_root);
    CHECK(klass != NULL);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != NULL);
    // note SetClassRoot does additional validation.
    // if possible add new checks there to catch errors early
  }

  CHECK(array_iftable_ != NULL);
  CHECK(array_interfaces_ != NULL);

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;

  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::FinishInit exiting";
  }
}

void ClassLinker::RunRootClinits() {
  Thread* self = Thread::Current();
  for (size_t i = 0; i < ClassLinker::kClassRootsMax; ++i) {
    Class* c = GetClassRoot(ClassRoot(i));
    if (!c->IsArrayClass() && !c->IsPrimitive()) {
      EnsureInitialized(GetClassRoot(ClassRoot(i)), true);
      CHECK(!self->IsExceptionPending()) << PrettyTypeOf(self->GetException());
    }
  }
}

const OatFile* ClassLinker::GenerateOatFile(const std::string& filename) {
  std::string oat_filename(GetArtCacheFilenameOrDie(OatFile::DexFilenameToOatFilename(filename)));

  // fork and exec dex2oat
  pid_t pid = fork();
  if (pid == 0) {
    std::string boot_image_option("--boot-image=");
    boot_image_option += Heap::GetSpaces()[0]->GetImageFilename();

    std::string dex_file_option("--dex-file=");
    dex_file_option += filename;

    std::string oat_file_option("--oat=");
    oat_file_option += oat_filename;

    std::string dex2oat("/system/bin/dex2oat");
#ifndef NDEBUG
    dex2oat += 'd';
#endif

    execl(dex2oat.c_str(), dex2oat.c_str(),
          "--runtime-arg", "-Xms64m",
          "--runtime-arg", "-Xmx64m",
          "--runtime-arg", "-classpath",
          "--runtime-arg", Runtime::Current()->GetClassPath().c_str(),
          boot_image_option.c_str(),
          dex_file_option.c_str(),
          oat_file_option.c_str(),
          NULL);

    PLOG(FATAL) << "execl(dex2oatd) failed";
    return NULL;
  } else {
    // wait for dex2oat to finish
    int status;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      return NULL;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << "dex2oatd failed with dex-file=" << filename;
      return NULL;
    }
  }
  return OatFile::Open(oat_filename, "", NULL);
}

OatFile* ClassLinker::OpenOat(const Space* space) {
  MutexLock mu(dex_lock_);
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::OpenOat entering";
  }
  const ImageHeader& image_header = space->GetImageHeader();
  String* oat_location = image_header.GetImageRoot(ImageHeader::kOatLocation)->AsString();
  std::string oat_filename;
  oat_filename += runtime->GetHostPrefix();
  oat_filename += oat_location->ToModifiedUtf8();
  OatFile* oat_file = OatFile::Open(oat_filename, "", image_header.GetOatBaseAddr());
  if (oat_file == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename << " referenced from image.";
    return NULL;
  }
  uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
  uint32_t image_oat_checksum = image_header.GetOatChecksum();
  if (oat_checksum != image_oat_checksum) {
    LOG(ERROR) << "Failed to match oat filechecksum " << std::hex << oat_checksum
               << " to expected oat checksum " << std::hex << oat_checksum
               << " in image";
    return NULL;
  }
  oat_files_.push_back(oat_file);
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::OpenOat exiting";
  }
  return oat_file;
}

const OatFile* ClassLinker::FindOatFile(const DexFile& dex_file) {
  MutexLock mu(dex_lock_);
  const OatFile* oat_file = FindOatFile(OatFile::DexFilenameToOatFilename(dex_file.GetLocation()));
  if (oat_file != NULL) {
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
    if (dex_file.GetHeader().checksum_ == oat_dex_file->GetDexFileChecksum()) {
      return oat_file;
    }
    LOG(WARNING) << ".oat file " << oat_file->GetLocation()
                 << " is older than " << dex_file.GetLocation() << " --- regenerating";
    if (TEMP_FAILURE_RETRY(unlink(oat_file->GetLocation().c_str())) != 0) {
      PLOG(FATAL) << "Couldn't remove obsolete .oat file " << oat_file->GetLocation();
    }
    // Fall through...
  }
  // Generate oat file if it wasn't found or was obsolete.
  oat_file = GenerateOatFile(dex_file.GetLocation());
  if (oat_file == NULL) {
    LOG(ERROR) << "Failed to generate oat file from dex file " << dex_file.GetLocation();
    return NULL;
  }
  oat_files_.push_back(oat_file);
  return oat_file;
}

const OatFile* ClassLinker::FindOpenedOatFile(const std::string& location) {
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != NULL);
    if (oat_file->GetLocation() == location) {
      return oat_file;
    }
  }
  return NULL;
}

const OatFile* ClassLinker::FindOatFile(const std::string& location) {
  const OatFile* oat_file = FindOpenedOatFile(location);
  if (oat_file != NULL) {
    return oat_file;
  }

  oat_file = OatFile::Open(location, "", NULL);
  if (oat_file == NULL) {
    if (location.empty() || location[0] != '/') {
      LOG(ERROR) << "Failed to open oat file from " << location;
      return NULL;
    }

    // not found in /foo/bar/baz.oat? try /data/art-cache/foo@bar@baz.oat
    std::string cache_location = GetArtCacheFilenameOrDie(location);
    oat_file = FindOpenedOatFile(cache_location);
    if (oat_file != NULL) {
      return oat_file;
    }
    oat_file = OatFile::Open(cache_location, "", NULL);
    if (oat_file  == NULL) {
      LOG(INFO) << "Failed to open oat file from " << location << " or " << cache_location << ".";
      return NULL;
    }
  }

  CHECK(oat_file != NULL) << location;
  oat_files_.push_back(oat_file);
  return oat_file;
}

void ClassLinker::InitFromImage() {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::InitFromImage entering";
  }
  CHECK(!init_done_);

  const std::vector<Space*>& spaces = Heap::GetSpaces();
  for (size_t i = 0; i < spaces.size(); i++) {
    Space* space = spaces[i] ;
    if (space->IsImageSpace()) {
      OatFile* oat_file = OpenOat(space);
      CHECK(oat_file != NULL) << "Failed to open oat file for image";
      Object* dex_caches_object = space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
      ObjectArray<DexCache>* dex_caches = dex_caches_object->AsObjectArray<DexCache>();

      CHECK_EQ(oat_file->GetOatHeader().GetDexFileCount(),
               static_cast<uint32_t>(dex_caches->GetLength()));
      for (int i = 0; i < dex_caches->GetLength(); i++) {
        SirtRef<DexCache> dex_cache(dex_caches->Get(i));
        const std::string& dex_file_location = dex_cache->GetLocation()->ToModifiedUtf8();

        std::string dex_filename;
        dex_filename += runtime->GetHostPrefix();
        dex_filename += dex_file_location;
        const DexFile* dex_file = DexFile::Open(dex_filename, runtime->GetHostPrefix());
        if (dex_file == NULL) {
          LOG(FATAL) << "Failed to open dex file " << dex_filename
                     << " referenced from oat file as " << dex_file_location;
        }

        const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file_location);
        CHECK_EQ(dex_file->GetHeader().checksum_, oat_dex_file->GetDexFileChecksum());

        AppendToBootClassPath(*dex_file, dex_cache);
      }
    }
  }

  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);

  // reinit clases_ table
  heap_bitmap->Walk(InitFromImageCallback, this);

  // reinit class_roots_
  Object* class_roots_object = spaces[0]->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots);
  class_roots_ = class_roots_object->AsObjectArray<Class>();

  // reinit array_interfaces_ and array_iftable_ from any array class instance, they should all be ==
  array_interfaces_ = GetClassRoot(kObjectArrayClass)->GetInterfaces();
  DCHECK(array_interfaces_ == GetClassRoot(kBooleanArrayClass)->GetInterfaces());
  array_iftable_ = GetClassRoot(kObjectArrayClass)->GetIfTable();
  DCHECK(array_iftable_ == GetClassRoot(kBooleanArrayClass)->GetIfTable());

  String::SetClass(GetClassRoot(kJavaLangString));
  Field::SetClass(GetClassRoot(kJavaLangReflectField));
  Method::SetClasses(GetClassRoot(kJavaLangReflectConstructor), GetClassRoot(kJavaLangReflectMethod));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  PathClassLoader::SetClass(GetClassRoot(kDalvikSystemPathClassLoader));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();

  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "ClassLinker::InitFromImage exiting";
  }
}

void ClassLinker::InitFromImageCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ClassLinker* class_linker = reinterpret_cast<ClassLinker*>(arg);

  if (obj->IsString()) {
    class_linker->intern_table_->RegisterStrong(obj->AsString());
    return;
  }
  if (obj->IsClass()) {
    // restore class to ClassLinker::classes_ table
    Class* klass = obj->AsClass();
    std::string descriptor = klass->GetDescriptor()->ToModifiedUtf8();
    bool success = class_linker->InsertClass(descriptor, klass, true);
    DCHECK(success);
    return;
  }
}

// Keep in sync with InitCallback. Anything we visit, we need to
// reinit references to when reinitializing a ClassLinker from a
// mapped image.
void ClassLinker::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  visitor(class_roots_, arg);

  for (size_t i = 0; i < dex_caches_.size(); i++) {
    visitor(dex_caches_[i], arg);
  }

  {
    MutexLock mu(classes_lock_);
    typedef Table::const_iterator It;  // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
      visitor(it->second, arg);
    }
    // Note. we deliberately ignore the class roots in the image (held in image_classes_)
  }

  visitor(array_interfaces_, arg);
  visitor(array_iftable_, arg);
}

ClassLinker::~ClassLinker() {
  String::ResetClass();
  Field::ResetClass();
  Method::ResetClasses();
  BooleanArray::ResetArrayClass();
  ByteArray::ResetArrayClass();
  CharArray::ResetArrayClass();
  DoubleArray::ResetArrayClass();
  FloatArray::ResetArrayClass();
  IntArray::ResetArrayClass();
  LongArray::ResetArrayClass();
  ShortArray::ResetArrayClass();
  PathClassLoader::ResetClass();
  StackTraceElement::ResetClass();
  STLDeleteElements(&boot_class_path_);
  STLDeleteElements(&oat_files_);
}

DexCache* ClassLinker::AllocDexCache(const DexFile& dex_file) {
  SirtRef<DexCache> dex_cache(down_cast<DexCache*>(AllocObjectArray<Object>(DexCache::LengthAsArray())));
  if (dex_cache.get() == NULL) {
    return NULL;
  }
  SirtRef<String> location(intern_table_->InternStrong(dex_file.GetLocation().c_str()));
  if (location.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<String> > strings(AllocObjectArray<String>(dex_file.NumStringIds()));
  if (strings.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<Class> > types(AllocClassArray(dex_file.NumTypeIds()));
  if (types.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<Method> > methods(AllocObjectArray<Method>(dex_file.NumMethodIds()));
  if (methods.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<Field> > fields(AllocObjectArray<Field>(dex_file.NumFieldIds()));
  if (fields.get() == NULL) {
    return NULL;
  }
  SirtRef<CodeAndDirectMethods> code_and_direct_methods(AllocCodeAndDirectMethods(dex_file.NumMethodIds()));
  if (code_and_direct_methods.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<StaticStorageBase> > initialized_static_storage(AllocObjectArray<StaticStorageBase>(dex_file.NumTypeIds()));
  if (initialized_static_storage.get() == NULL) {
    return NULL;
  }

  dex_cache->Init(location.get(),
                  strings.get(),
                  types.get(),
                  methods.get(),
                  fields.get(),
                  code_and_direct_methods.get(),
                  initialized_static_storage.get());
  return dex_cache.get();
}

CodeAndDirectMethods* ClassLinker::AllocCodeAndDirectMethods(size_t length) {
  return down_cast<CodeAndDirectMethods*>(IntArray::Alloc(CodeAndDirectMethods::LengthAsArray(length)));
}

InterfaceEntry* ClassLinker::AllocInterfaceEntry(Class* interface) {
  DCHECK(interface->IsInterface());
  SirtRef<ObjectArray<Object> > array(AllocObjectArray<Object>(InterfaceEntry::LengthAsArray()));
  SirtRef<InterfaceEntry> interface_entry(down_cast<InterfaceEntry*>(array.get()));
  interface_entry->SetInterface(interface);
  return interface_entry.get();
}

Class* ClassLinker::AllocClass(Class* java_lang_Class, size_t class_size) {
  DCHECK_GE(class_size, sizeof(Class));
  SirtRef<Class> klass(Heap::AllocObject(java_lang_Class, class_size)->AsClass());
  klass->SetPrimitiveType(Primitive::kPrimNot);  // default to not being primitive
  klass->SetClassSize(class_size);
  return klass.get();
}

Class* ClassLinker::AllocClass(size_t class_size) {
  return AllocClass(GetClassRoot(kJavaLangClass), class_size);
}

Field* ClassLinker::AllocField() {
  return down_cast<Field*>(GetClassRoot(kJavaLangReflectField)->AllocObject());
}

Method* ClassLinker::AllocMethod() {
  return down_cast<Method*>(GetClassRoot(kJavaLangReflectMethod)->AllocObject());
}

ObjectArray<StackTraceElement>* ClassLinker::AllocStackTraceElementArray(size_t length) {
  return ObjectArray<StackTraceElement>::Alloc(
      GetClassRoot(kJavaLangStackTraceElementArrayClass),
      length);
}

Class* EnsureResolved(Class* klass) {
  DCHECK(klass != NULL);
  // Wait for the class if it has not already been linked.
  Thread* self = Thread::Current();
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    ObjectLock lock(klass);
    // Check for circular dependencies between classes.
    if (!klass->IsResolved() && klass->GetClinitThreadId() == self->GetTid()) {
      self->ThrowNewException("Ljava/lang/ClassCircularityError;",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
      return NULL;
    }
    // Wait for the pending initialization to complete.
    while (!klass->IsResolved() && !klass->IsErroneous()) {
      lock.Wait();
    }
  }
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return NULL;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  CHECK(!self->IsExceptionPending())
      << PrettyClass(klass) << " " << PrettyTypeOf(self->GetException());
  return klass;
}

Class* ClassLinker::FindClass(const std::string& descriptor,
                              const ClassLoader* class_loader) {
  CHECK_NE(descriptor.size(), 0U);
  Thread* self = Thread::Current();
  DCHECK(self != NULL);
  CHECK(!self->IsExceptionPending()) << PrettyTypeOf(self->GetException());
  // Find the class in the loaded classes table.
  Class* klass = LookupClass(descriptor, class_loader);
  if (klass != NULL) {
    return EnsureResolved(klass);
  }
  if (descriptor.size() == 1) {
    // only the descriptors of primitive types should be 1 character long
    return FindPrimitiveClass(descriptor[0]);
  }
  // Class is not yet loaded.
  if (descriptor[0] == '[') {
    return CreateArrayClass(descriptor, class_loader);
  }
  if (class_loader == NULL) {
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, boot_class_path_);
    if (pair.second == NULL) {
      std::string name(PrintableString(descriptor));
      ThrowNoClassDefFoundError("Class %s not found in boot class loader", name.c_str());
      return NULL;
    }
    return DefineClass(descriptor, NULL, *pair.first, *pair.second);
  }

  if (ClassLoader::UseCompileTimeClassPath()) {
    const std::vector<const DexFile*>& class_path
        = ClassLoader::GetCompileTimeClassPath(class_loader);
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, class_path);
    if (pair.second == NULL) {
      return FindSystemClass(descriptor);
    }
    return DefineClass(descriptor, class_loader, *pair.first, *pair.second);
  }

  std::string class_name_string = DescriptorToDot(descriptor);
  ScopedThreadStateChange(self, Thread::kNative);
  JNIEnv* env = self->GetJniEnv();
  ScopedLocalRef<jclass> c(env, AddLocalReference<jclass>(env, GetClassRoot(kJavaLangClassLoader)));
  CHECK(c.get() != NULL);
  // TODO: cache method?
  jmethodID mid = env->GetMethodID(c.get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  CHECK(mid != NULL);
  ScopedLocalRef<jobject> class_name_object(env, env->NewStringUTF(class_name_string.c_str()));
  if (class_name_object.get() == NULL) {
    return NULL;
  }
  ScopedLocalRef<jobject> class_loader_object(env, AddLocalReference<jobject>(env, class_loader));
  ScopedLocalRef<jobject> result(env, env->CallObjectMethod(class_loader_object.get(), mid, class_name_object.get()));
  return Decode<Class*>(env, result.get());
}

Class* ClassLinker::DefineClass(const std::string& descriptor,
                                const ClassLoader* class_loader,
                                const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  SirtRef<Class> klass(NULL);
  // Load the class from the dex file.
  if (!init_done_) {
    // finish up init of hand crafted class_roots_
    if (descriptor == "Ljava/lang/Object;") {
      klass.reset(GetClassRoot(kJavaLangObject));
    } else if (descriptor == "Ljava/lang/Class;") {
      klass.reset(GetClassRoot(kJavaLangClass));
    } else if (descriptor == "Ljava/lang/String;") {
      klass.reset(GetClassRoot(kJavaLangString));
    } else if (descriptor == "Ljava/lang/reflect/Constructor;") {
      klass.reset(GetClassRoot(kJavaLangReflectConstructor));
    } else if (descriptor == "Ljava/lang/reflect/Field;") {
      klass.reset(GetClassRoot(kJavaLangReflectField));
    } else if (descriptor == "Ljava/lang/reflect/Method;") {
      klass.reset(GetClassRoot(kJavaLangReflectMethod));
    } else {
      klass.reset(AllocClass(SizeOfClass(dex_file, dex_class_def)));
    }
  } else {
    klass.reset(AllocClass(SizeOfClass(dex_file, dex_class_def)));
  }
  klass->SetDexCache(FindDexCache(dex_file));
  LoadClass(dex_file, dex_class_def, klass, class_loader);
  // Check for a pending exception during load
  Thread* self = Thread::Current();
  if (self->IsExceptionPending()) {
    return NULL;
  }
  ObjectLock lock(klass.get());
  klass->SetClinitThreadId(self->GetTid());
  // Add the newly loaded class to the loaded classes table.
  bool success = InsertClass(descriptor, klass.get(), false);  // TODO: just return collision
  if (!success) {
    // We may fail to insert if we raced with another thread.
    klass->SetClinitThreadId(0);
    klass.reset(LookupClass(descriptor, class_loader));
    CHECK(klass.get() != NULL);
    return klass.get();
  }
  // Finish loading (if necessary) by finding parents
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    // Loading failed.
    CHECK(self->IsExceptionPending());
    klass->SetStatus(Class::kStatusError);
    lock.NotifyAll();
    return NULL;
  }
  CHECK(klass->IsLoaded());
  // Link the class (if necessary)
  CHECK(!klass->IsResolved());
  if (!LinkClass(klass)) {
    // Linking failed.
    CHECK(self->IsExceptionPending());
    klass->SetStatus(Class::kStatusError);
    lock.NotifyAll();
    return NULL;
  }
  CHECK(klass->IsResolved());
  return klass.get();
}

// Precomputes size that will be needed for Class, matching LinkStaticFields
size_t ClassLinker::SizeOfClass(const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  size_t num_ref = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (class_data != NULL) {
    for (ClassDataItemIterator it(dex_file, class_data); it.HasNextStaticField(); it.Next()) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
      const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
      char c = descriptor[0];
      if (c == 'L' || c == '[') {
        num_ref++;
      } else if (c == 'J' || c == 'D') {
        num_64++;
      } else {
        num_32++;
      }
    }
  }
  // start with generic class data
  size_t size = sizeof(Class);
  // follow with reference fields which must be contiguous at start
  size += (num_ref * sizeof(uint32_t));
  // if there are 64-bit fields to add, make sure they are aligned
  if (num_64 != 0 && size != RoundUp(size, 8)) { // for 64-bit alignment
    if (num_32 != 0) {
      // use an available 32-bit field for padding
      num_32--;
    }
    size += sizeof(uint32_t);  // either way, we are adding a word
    DCHECK_EQ(size, RoundUp(size, 8));
  }
  // tack on any 64-bit fields now that alignment is assured
  size += (num_64 * sizeof(uint64_t));
  // tack on any remaining 32-bit fields
  size += (num_32 * sizeof(uint32_t));
  return size;
}

void LinkCode(SirtRef<Method>& method, const OatFile::OatClass* oat_class, uint32_t method_index) {
  // Every kind of method should at least get an invoke stub from the oat_method.
  // non-abstract methods also get their code pointers.
  const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
  oat_method.LinkMethod(method.get());

  if (method->IsAbstract()) {
    method->SetCode(Runtime::Current()->GetAbstractMethodErrorStubArray()->GetData());
    return;
  }
  if (method->IsNative()) {
    // unregistering restores the dlsym lookup stub
    method->UnregisterNative();
    return;
  }
}

void ClassLinker::LoadClass(const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            SirtRef<Class>& klass,
                            const ClassLoader* class_loader) {
  CHECK(klass.get() != NULL);
  CHECK(klass->GetDexCache() != NULL);
  CHECK_EQ(Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != NULL);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  if (klass->GetDescriptor() != NULL) {
    DCHECK(klass->GetDescriptor()->Equals(descriptor));
  } else {
    klass->SetDescriptor(intern_table_->InternStrong(descriptor));
    if (klass->GetDescriptor() == NULL) {
      return;
    }
  }
  uint32_t access_flags = dex_class_def.access_flags_;
  // Make sure that none of our runtime-only flags are set.
  CHECK_EQ(access_flags & ~kAccJavaFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK(klass->GetPrimitiveType() == Primitive::kPrimNot);
  klass->SetStatus(Class::kStatusIdx);

  klass->SetTypeIdx(dex_class_def.class_idx_);
  klass->SetSuperClassTypeIdx(dex_class_def.superclass_idx_);
  klass->SetAnnotationsOffset(dex_class_def.annotations_off_);

  const char* source_file = dex_file.GetSourceFile(dex_class_def);
  if (source_file != NULL) {
    String* source_file_string = intern_table_->InternStrong(source_file);
    if (source_file_string == NULL) {
      return;
    }
    klass->SetSourceFile(source_file_string);
  }

  // Load class interfaces.
  LoadInterfaces(dex_file, dex_class_def, klass);

  // Load fields fields.
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == NULL) {
    return;  // no fields or methods - for example a marker interface
  }
  ClassDataItemIterator it(dex_file, class_data);
  if (it.NumStaticFields() != 0) {
    klass->SetSFields(AllocObjectArray<Field>(it.NumStaticFields()));
  }
  if (it.NumInstanceFields() != 0) {
    klass->SetIFields(AllocObjectArray<Field>(it.NumInstanceFields()));
  }
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    SirtRef<Field> sfield(AllocField());
    klass->SetStaticField(i, sfield.get());
    LoadField(dex_file, it, klass, sfield);
  }
  for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
    SirtRef<Field> ifield(AllocField());
    klass->SetInstanceField(i, ifield.get());
    LoadField(dex_file, it, klass, ifield);
  }

  UniquePtr<const OatFile::OatClass> oat_class;
  if (Runtime::Current()->IsStarted() && !ClassLoader::UseCompileTimeClassPath()) {
    const OatFile* oat_file = FindOatFile(dex_file);
    if (oat_file != NULL) {
      const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
      if (oat_dex_file != NULL) {
        uint32_t class_def_index;
        bool found = dex_file.FindClassDefIndex(descriptor, class_def_index);
        CHECK(found) << descriptor;
        oat_class.reset(oat_dex_file->GetOatClass(class_def_index));
        CHECK(oat_class.get() != NULL) << descriptor;
      }
    }
  }
  // Load methods.
  if (it.NumDirectMethods() != 0) {
    // TODO: append direct methods to class object
    klass->SetDirectMethods(AllocObjectArray<Method>(it.NumDirectMethods()));
  }
  if (it.NumVirtualMethods() != 0) {
    // TODO: append direct methods to class object
    klass->SetVirtualMethods(AllocObjectArray<Method>(it.NumVirtualMethods()));
  }
  size_t method_index = 0;
  for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
    SirtRef<Method> method(AllocMethod());
    klass->SetDirectMethod(i, method.get());
    LoadMethod(dex_file, it, klass, method);
    if (oat_class.get() != NULL) {
      LinkCode(method, oat_class.get(), method_index);
    }
    method_index++;
  }
  for (size_t i = 0; it.HasNextVirtualMethod(); i++, it.Next()) {
    SirtRef<Method> method(AllocMethod());
    klass->SetVirtualMethod(i, method.get());
    LoadMethod(dex_file, it, klass, method);
    if (oat_class.get() != NULL) {
      LinkCode(method, oat_class.get(), method_index);
    }
    method_index++;
  }
  DCHECK(!it.HasNext());
}

void ClassLinker::LoadInterfaces(const DexFile& dex_file,
                                 const DexFile::ClassDef& dex_class_def,
                                 SirtRef<Class>& klass) {
  const DexFile::TypeList* list = dex_file.GetInterfacesList(dex_class_def);
  if (list != NULL) {
    klass->SetInterfaces(AllocClassArray(list->Size()));
    IntArray* interfaces_idx = IntArray::Alloc(list->Size());
    klass->SetInterfacesTypeIdx(interfaces_idx);
    for (size_t i = 0; i < list->Size(); ++i) {
      const DexFile::TypeItem& type_item = list->GetTypeItem(i);
      interfaces_idx->Set(i, type_item.type_idx_);
    }
  }
}

void ClassLinker::LoadField(const DexFile& dex_file, const ClassDataItemIterator& it,
                            SirtRef<Class>& klass, SirtRef<Field>& dst) {
  const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
  dst->SetDeclaringClass(klass.get());
  dst->SetName(ResolveString(dex_file, field_id.name_idx_, klass->GetDexCache()));
  dst->SetTypeIdx(field_id.type_idx_);
  dst->SetAccessFlags(it.GetMemberAccessFlags());

  // In order to access primitive types using GetTypeDuringLinking we need to
  // ensure they are resolved into the dex cache
  const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
  if (descriptor[1] == '\0') {
    // only the descriptors of primitive types should be 1 character long
    Class* resolved = ResolveType(dex_file, field_id.type_idx_, klass.get());
    DCHECK(resolved->IsPrimitive());
  }
}

void ClassLinker::LoadMethod(const DexFile& dex_file, const ClassDataItemIterator& it,
                             SirtRef<Class>& klass, SirtRef<Method>& dst) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(it.GetMemberIndex());
  dst->SetDeclaringClass(klass.get());

  String* method_name = ResolveString(dex_file, method_id.name_idx_, klass->GetDexCache());
  if (method_name == NULL) {
    return;
  }
  dst->SetName(method_name);
  if (method_name->Equals("<init>")) {
    dst->SetClass(GetClassRoot(kJavaLangReflectConstructor));
  }

  int32_t utf16_length;
  std::string signature(dex_file.CreateMethodSignature(method_id.proto_idx_, &utf16_length));
  String* signature_string = intern_table_->InternStrong(utf16_length, signature.c_str());
  if (signature_string == NULL) {
    return;
  }
  dst->SetSignature(signature_string);

  if (method_name->Equals("finalize") && signature == "()V") {
    /*
     * The Enum class declares a "final" finalize() method to prevent subclasses from introducing
     * a finalizer. We don't want to set the finalizable flag for Enum or its subclasses, so we
     * exclude it here.
     *
     * We also want to avoid setting the flag on Object, where we know that finalize() is empty.
     */
    if (klass->GetClassLoader() != NULL ||
        (!klass->GetDescriptor()->Equals("Ljava/lang/Object;") &&
            !klass->GetDescriptor()->Equals("Ljava/lang/Enum;"))) {
      klass->SetFinalizable();
    }
  }

  dst->SetProtoIdx(method_id.proto_idx_);
  dst->SetCodeItemOffset(it.GetMethodCodeItemOffset());
  const char* shorty = dex_file.GetShorty(method_id.proto_idx_);
  String* shorty_string = intern_table_->InternStrong(shorty);
  dst->SetShorty(shorty_string);
  if (shorty_string == NULL) {
    return;
  }
  dst->SetAccessFlags(it.GetMemberAccessFlags());
  uint32_t return_type_idx = dex_file.GetProtoId(method_id.proto_idx_).return_type_idx_;
  DCHECK_LT(return_type_idx, dex_file.NumTypeIds());
  dst->SetReturnTypeIdx(return_type_idx);

  dst->SetDexCacheStrings(klass->GetDexCache()->GetStrings());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedFields(klass->GetDexCache()->GetResolvedFields());
  dst->SetDexCacheCodeAndDirectMethods(klass->GetDexCache()->GetCodeAndDirectMethods());
  dst->SetDexCacheInitializedStaticStorage(klass->GetDexCache()->GetInitializedStaticStorage());

  // TODO: check for finalize method

  const DexFile::CodeItem* code_item = it.GetMethodCodeItem();
  if (code_item != NULL) {
    dst->SetNumRegisters(code_item->registers_size_);
    dst->SetNumIns(code_item->ins_size_);
    dst->SetNumOuts(code_item->outs_size_);
  } else {
    uint16_t num_args = Method::NumArgRegisters(shorty);
    if ((it.GetMemberAccessFlags() & kAccStatic) == 0) {
      ++num_args;
    }
    dst->SetNumRegisters(num_args);
    // TODO: native methods
  }
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file) {
  SirtRef<DexCache> dex_cache(AllocDexCache(dex_file));
  AppendToBootClassPath(dex_file, dex_cache);
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}

bool ClassLinker::IsDexFileRegisteredLocked(const DexFile& dex_file) const {
  dex_lock_.AssertHeld();
  for (size_t i = 0; i != dex_files_.size(); ++i) {
    if (dex_files_[i] == &dex_file) {
        return true;
    }
  }
  return false;
}

bool ClassLinker::IsDexFileRegistered(const DexFile& dex_file) const {
  MutexLock mu(dex_lock_);
  return IsDexFileRegisteredLocked(dex_file);
}

void ClassLinker::RegisterDexFileLocked(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  dex_lock_.AssertHeld();
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  CHECK(dex_cache->GetLocation()->Equals(dex_file.GetLocation()));
  dex_files_.push_back(&dex_file);
  dex_caches_.push_back(dex_cache.get());
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  {
    MutexLock mu(dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
  }
  // Don't alloc while holding the lock, since allocation may need to
  // suspend all threads and another thread may need the dex_lock_ to
  // get to a suspend point.
  SirtRef<DexCache> dex_cache(AllocDexCache(dex_file));
  {
    MutexLock mu(dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
    RegisterDexFileLocked(dex_file, dex_cache);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  MutexLock mu(dex_lock_);
  RegisterDexFileLocked(dex_file, dex_cache);
}

const DexFile& ClassLinker::FindDexFile(const DexCache* dex_cache) const {
  CHECK(dex_cache != NULL);
  MutexLock mu(dex_lock_);
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    if (dex_caches_[i] == dex_cache) {
        return *dex_files_[i];
    }
  }
  CHECK(false) << "Failed to find DexFile for DexCache " << dex_cache->GetLocation()->ToModifiedUtf8();
  return *dex_files_[-1];
}

DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) const {
  MutexLock mu(dex_lock_);
  for (size_t i = 0; i != dex_files_.size(); ++i) {
    if (dex_files_[i] == &dex_file) {
        return dex_caches_[i];
    }
  }
  CHECK(false) << "Failed to find DexCache for DexFile " << dex_file.GetLocation();
  return NULL;
}

Class* ClassLinker::InitializePrimitiveClass(Class* primitive_class,
                                             const char* descriptor,
                                             Primitive::Type type) {
  // TODO: deduce one argument from the other
  CHECK(primitive_class != NULL);
  primitive_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  primitive_class->SetDescriptor(intern_table_->InternStrong(descriptor));
  CHECK(primitive_class->GetDescriptor() != NULL);
  primitive_class->SetPrimitiveType(type);
  primitive_class->SetStatus(Class::kStatusInitialized);
  bool success = InsertClass(descriptor, primitive_class, false);
  CHECK(success) << "InitPrimitiveClass(" << descriptor << ") failed";
  return primitive_class;
}

// Create an array class (i.e. the class object for the array, not the
// array itself).  "descriptor" looks like "[C" or "[[[[B" or
// "[Ljava/lang/String;".
//
// If "descriptor" refers to an array of primitives, look up the
// primitive type's internally-generated class object.
//
// "class_loader" is the class loader of the class that's referring to
// us.  It's used to ensure that we're looking for the element type in
// the right context.  It does NOT become the class loader for the
// array class; that always comes from the base element class.
//
// Returns NULL with an exception raised on failure.
Class* ClassLinker::CreateArrayClass(const std::string& descriptor,
                                     const ClassLoader* class_loader) {
  CHECK_EQ('[', descriptor[0]);

  // Identify the underlying component type
  Class* component_type = FindClass(descriptor.substr(1), class_loader);
  if (component_type == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
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
  // This call is unnecessary if "loader" and "component_type->GetClassLoader()"
  // are the same, because our caller (FindClass) just did the
  // lookup.  (Even if we get this wrong we still have correct behavior,
  // because we effectively do this lookup again when we add the new
  // class to the hash table --- necessary because of possible races with
  // other threads.)
  if (class_loader != component_type->GetClassLoader()) {
    Class* new_class = LookupClass(descriptor, component_type->GetClassLoader());
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

  SirtRef<Class> new_class(NULL);
  if (!init_done_) {
    // Classes that were hand created, ie not by FindSystemClass
    if (descriptor == "[Ljava/lang/Class;") {
      new_class.reset(GetClassRoot(kClassArrayClass));
    } else if (descriptor == "[Ljava/lang/Object;") {
      new_class.reset(GetClassRoot(kObjectArrayClass));
    } else if (descriptor == "[C") {
      new_class.reset(GetClassRoot(kCharArrayClass));
    } else if (descriptor == "[I") {
      new_class.reset(GetClassRoot(kIntArrayClass));
    }
  }
  if (new_class.get() == NULL) {
    new_class.reset(AllocClass(sizeof(Class)));
    if (new_class.get() == NULL) {
      return NULL;
    }
    new_class->SetComponentType(component_type);
  }
  DCHECK(new_class->GetComponentType() != NULL);
  if (new_class->GetDescriptor() != NULL) {
    DCHECK(new_class->GetDescriptor()->Equals(descriptor));
  } else {
    new_class->SetDescriptor(intern_table_->InternStrong(descriptor.c_str()));
    if (new_class->GetDescriptor() == NULL) {
      return NULL;
    }
  }
  Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Primitive::kPrimNot);
  new_class->SetClassLoader(component_type->GetClassLoader());
  new_class->SetStatus(Class::kStatusInitialized);
  // don't need to set new_class->SetObjectSize(..)
  // because Object::SizeOf delegates to Array::SizeOf


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
  CHECK(array_interfaces_ != NULL);
  CHECK(array_iftable_ != NULL);
  new_class->SetInterfaces(array_interfaces_);
  new_class->SetIfTable(array_iftable_);

  // Inherit access flags from the component type.  Arrays can't be
  // used as a superclass or interface, so we want to add "final"
  // and remove "interface".
  //
  // Don't inherit any non-standard flags (e.g., kAccFinal)
  // from component_type.  We assume that the array class does not
  // override finalize().
  new_class->SetAccessFlags(((new_class->GetComponentType()->GetAccessFlags() &
                             ~kAccInterface) | kAccFinal) & kAccJavaFlagsMask);

  if (InsertClass(descriptor, new_class.get(), false)) {
    return new_class.get();
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  // Grab the winning class.
  Class* other_class = LookupClass(descriptor, component_type->GetClassLoader());
  DCHECK(other_class != NULL);
  return other_class;
}

Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (Primitive::GetType(type)) {
    case Primitive::kPrimByte:
      return GetClassRoot(kPrimitiveByte);
    case Primitive::kPrimChar:
      return GetClassRoot(kPrimitiveChar);
    case Primitive::kPrimDouble:
      return GetClassRoot(kPrimitiveDouble);
    case Primitive::kPrimFloat:
      return GetClassRoot(kPrimitiveFloat);
    case Primitive::kPrimInt:
      return GetClassRoot(kPrimitiveInt);
    case Primitive::kPrimLong:
      return GetClassRoot(kPrimitiveLong);
    case Primitive::kPrimShort:
      return GetClassRoot(kPrimitiveShort);
    case Primitive::kPrimBoolean:
      return GetClassRoot(kPrimitiveBoolean);
    case Primitive::kPrimVoid:
      return GetClassRoot(kPrimitiveVoid);
    case Primitive::kPrimNot:
      break;
  }
  std::string printable_type(PrintableChar(type));
  ThrowNoClassDefFoundError("Not a primitive type: %s", printable_type.c_str());
  return NULL;
}

bool ClassLinker::InsertClass(const std::string& descriptor, Class* klass, bool image_class) {
  size_t hash = StringPieceHash()(descriptor);
  MutexLock mu(classes_lock_);
  Table::iterator it;
  if (image_class) {
    // TODO: sanity check there's no match in classes_
    it = image_classes_.insert(std::make_pair(hash, klass));
  } else {
    // TODO: sanity check there's no match in image_classes_
    it = classes_.insert(std::make_pair(hash, klass));
  }
  return ((*it).second == klass);
}

Class* ClassLinker::LookupClass(const std::string& descriptor, const ClassLoader* class_loader) {
  size_t hash = StringPieceHash()(descriptor);
  MutexLock mu(classes_lock_);
  typedef Table::const_iterator It;  // TODO: C++0x auto
  // TODO: determine if its better to search classes_ or image_classes_ first
  for (It it = classes_.find(hash), end = classes_.end(); it != end; ++it) {
    Class* klass = it->second;
    if (klass->GetDescriptor()->Equals(descriptor) && klass->GetClassLoader() == class_loader) {
      return klass;
    }
  }
  for (It it = image_classes_.find(hash), end = image_classes_.end(); it != end; ++it) {
    Class* klass = it->second;
    if (klass->GetDescriptor()->Equals(descriptor) && klass->GetClassLoader() == class_loader) {
      return klass;
    }
  }
  return NULL;
}

void ClassLinker::VerifyClass(Class* klass) {
  if (klass->IsVerified()) {
    return;
  }

  CHECK_EQ(klass->GetStatus(), Class::kStatusResolved);
  klass->SetStatus(Class::kStatusVerifying);

  if (verifier::DexVerifier::VerifyClass(klass)) {
    klass->SetStatus(Class::kStatusVerified);
  } else {
    LOG(ERROR) << "Verification failed on class " << PrettyClass(klass);
    Thread* self = Thread::Current();
    CHECK(!self->IsExceptionPending()) << PrettyTypeOf(self->GetException());
    self->ThrowNewExceptionF("Ljava/lang/VerifyError;", "Verification of %s failed",
        PrettyDescriptor(klass->GetDescriptor()).c_str());
    CHECK_EQ(klass->GetStatus(), Class::kStatusVerifying);
    klass->SetStatus(Class::kStatusError);
  }
}

Class* ClassLinker::CreateProxyClass(String* name, ObjectArray<Class>* interfaces,
    ClassLoader* loader, ObjectArray<Method>* methods, ObjectArray<ObjectArray<Class> >* throws) {
  SirtRef<Class> klass(AllocClass(GetClassRoot(kJavaLangClass), sizeof(ProxyClass)));
  CHECK(klass.get() != NULL);
  klass->SetObjectSize(sizeof(Proxy));
  const char* descriptor = DotToDescriptor(name->ToModifiedUtf8().c_str()).c_str();;
  klass->SetDescriptor(intern_table_->InternStrong(descriptor));
  klass->SetAccessFlags(kAccPublic | kAccFinal);
  klass->SetClassLoader(loader);
  klass->SetStatus(Class::kStatusInitialized);  // no loading or initializing necessary
  Class* proxy_class = GetClassRoot(kJavaLangReflectProxy);
  klass->SetSuperClass(proxy_class);  // The super class is java.lang.reflect.Proxy
  klass->SetInterfaces(interfaces);  // The interfaces are the array of interfaces specified

  // Proxies have 1 direct method, the constructor
  klass->SetDirectMethods(AllocObjectArray<Method>(1));
  klass->SetDirectMethod(0, CreateProxyConstructor(klass));

  // Create virtual method using specified prototypes
  size_t num_virtual_methods = methods->GetLength();
  klass->SetVirtualMethods(AllocObjectArray<Method>(num_virtual_methods));
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    SirtRef<Method> prototype(methods->Get(i));
    klass->SetVirtualMethod(i, CreateProxyMethod(klass, prototype, throws->Get(i)));
  }
  // Link the virtual methods, creating vtable and iftables
  if (!LinkMethods(klass)) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  return klass.get();
}

Method* ClassLinker::CreateProxyConstructor(SirtRef<Class>& klass) {
  // Create constructor for Proxy that must initialize h
  Class* proxy_class = GetClassRoot(kJavaLangReflectProxy);
  ObjectArray<Method>* proxy_direct_methods = proxy_class->GetDirectMethods();
  CHECK_EQ(proxy_direct_methods->GetLength(), 15);
  Method* proxy_constructor = proxy_direct_methods->Get(2);
  // Clone the existing constructor of Proxy (our constructor would just invoke it so steal its
  // code_ too)
  Method* constructor = down_cast<Method*>(proxy_constructor->Clone());
  // Make this constructor public and fix the class to be our Proxy version
  constructor->SetAccessFlags((constructor->GetAccessFlags() & ~kAccProtected) | kAccPublic);
  constructor->SetDeclaringClass(klass.get());
  // Sanity checks
  CHECK(constructor->IsConstructor());
  CHECK(constructor->GetName()->Equals("<init>"));
  CHECK(constructor->GetSignature()->Equals("(Ljava/lang/reflect/InvocationHandler;)V"));
  DCHECK(constructor->IsPublic());
  return constructor;
}

Method* ClassLinker::CreateProxyMethod(SirtRef<Class>& klass, SirtRef<Method>& prototype,
                                       ObjectArray<Class>* throws) {
  // We steal everything from the prototype (such as DexCache, invoke stub, etc.) then specialise
  // as necessary
  Method* method = down_cast<Method*>(prototype->Clone());

  // Set class to be the concrete proxy class and clear the abstract flag, modify exceptions to
  // the intersection of throw exceptions as defined in Proxy
  method->SetDeclaringClass(klass.get());
  method->SetAccessFlags((method->GetAccessFlags() & ~kAccAbstract) | kAccFinal);
  method->SetExceptionTypes(throws);

  // At runtime the method looks like a reference and argument saving method, clone the code
  // related parameters from this method.
  Method* refs_and_args = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
  method->SetCoreSpillMask(refs_and_args->GetCoreSpillMask());
  method->SetFpSpillMask(refs_and_args->GetFpSpillMask());
  method->SetFrameSizeInBytes(refs_and_args->GetFrameSizeInBytes());
  method->SetCode(reinterpret_cast<void*>(art_proxy_invoke_handler));

  // Basic sanity
  DCHECK(method->GetName()->Equals(prototype->GetName()));
  DCHECK(method->GetSignature()->Equals(prototype->GetSignature()));
  DCHECK(method->GetShorty()->Equals(prototype->GetShorty()));

  // More complex sanity - via dex cache
  CHECK_EQ(method->GetReturnType(), prototype->GetReturnType());

  return method;
}

bool ClassLinker::InitializeClass(Class* klass, bool can_run_clinit) {
  CHECK(klass->IsResolved() || klass->IsErroneous())
      << PrettyClass(klass) << " is " << klass->GetStatus();

  Thread* self = Thread::Current();

  Method* clinit = NULL;
  {
    // see JLS 3rd edition, 12.4.2 "Detailed Initialization Procedure" for the locking protocol
    ObjectLock lock(klass);

    if (klass->GetStatus() == Class::kStatusInitialized) {
      return true;
    }

    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass);
      return false;
    }

    if (klass->GetStatus() == Class::kStatusResolved) {
      VerifyClass(klass);
      if (klass->GetStatus() != Class::kStatusVerified) {
        return false;
      }
    }

    clinit = klass->FindDeclaredDirectMethod("<clinit>", "()V");
    if (clinit != NULL && !can_run_clinit) {
      // if the class has a <clinit> but we can't run it during compilation,
      // don't bother going to kStatusInitializing
      return false;
    }

    // If the class is kStatusInitializing, either this thread is
    // initializing higher up the stack or another thread has beat us
    // to initializing and we need to wait. Either way, this
    // invocation of InitializeClass will not be responsible for
    // running <clinit> and will return.
    if (klass->GetStatus() == Class::kStatusInitializing) {
      // We caught somebody else in the act; was it us?
      if (klass->GetClinitThreadId() == self->GetTid()) {
        // Yes. That's fine. Return so we can continue initializing.
        return true;
      }
      // No. That's fine. Wait for another thread to finish initializing.
      return WaitForInitializeClass(klass, self, lock);
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(Class::kStatusError);
      return false;
    }

    DCHECK_EQ(klass->GetStatus(), Class::kStatusVerified);

    klass->SetClinitThreadId(self->GetTid());
    klass->SetStatus(Class::kStatusInitializing);
  }

  uint64_t t0 = NanoTime();

  if (!InitializeSuperClass(klass, can_run_clinit)) {
    return false;
  }

  InitializeStaticFields(klass);

  if (clinit != NULL) {
    clinit->Invoke(self, NULL, NULL, NULL);
  }

  uint64_t t1 = NanoTime();

  {
    ObjectLock lock(klass);

    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(Class::kStatusError);
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      klass->SetStatus(Class::kStatusInitialized);
    }
    lock.NotifyAll();
  }

  return true;
}

bool ClassLinker::WaitForInitializeClass(Class* klass, Thread* self, ObjectLock& lock) {
  while (true) {
    CHECK(!self->IsExceptionPending()) << PrettyTypeOf(self->GetException());
    lock.Wait();

    // When we wake up, repeat the test for init-in-progress.  If
    // there's an exception pending (only possible if
    // "interruptShouldThrow" was set), bail out.
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(Class::kStatusError);
      return false;
    }
    // Spurious wakeup? Go back to waiting.
    if (klass->GetStatus() == Class::kStatusInitializing) {
      continue;
    }
    if (klass->IsErroneous()) {
      // The caller wants an exception, but it was thrown in a
      // different thread.  Synthesize one here.
      ThrowNoClassDefFoundError("<clinit> failed for class %s; see exception in other thread",
                                PrettyDescriptor(klass->GetDescriptor()).c_str());
      return false;
    }
    if (klass->IsInitialized()) {
      return true;
    }
    LOG(FATAL) << "Unexpected class status. " << PrettyClass(klass) << " is " << klass->GetStatus();
  }
  LOG(FATAL) << "Not Reached" << PrettyClass(klass);
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
      const Method* method = super->GetVirtualMethod(i);
      if (method != super->GetVirtualMethod(i) &&
          !HasSameMethodDescriptorClasses(method, super, klass)) {
        klass->DumpClass(std::cerr, Class::kDumpClassFullDetail);

        ThrowLinkageError("Class %s method %s resolves differently in superclass %s", PrettyDescriptor(klass->GetDescriptor()).c_str(), PrettyMethod(method).c_str(), PrettyDescriptor(super->GetDescriptor()).c_str());
        return false;
      }
    }
  }
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    InterfaceEntry* interface_entry = klass->GetIfTable()->Get(i);
    Class* interface = interface_entry->GetInterface();
    if (klass->GetClassLoader() != interface->GetClassLoader()) {
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        const Method* method = interface_entry->GetMethodArray()->Get(j);
        if (!HasSameMethodDescriptorClasses(method, interface,
                                            method->GetDeclaringClass())) {
          klass->DumpClass(std::cerr, Class::kDumpClassFullDetail);

          ThrowLinkageError("Class %s method %s resolves differently in interface %s", PrettyDescriptor(method->GetDeclaringClass()->GetDescriptor()).c_str(), PrettyMethod(method).c_str(), PrettyDescriptor(interface->GetDescriptor()).c_str());
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
  if (klass1 == klass2) {
    return true;
  }
  const DexFile& dex_file = FindDexFile(method->GetDeclaringClass()->GetDexCache());
  const DexFile::ProtoId& proto_id = dex_file.GetProtoId(method->GetProtoIdx());
  for (DexFileParameterIterator it(dex_file, proto_id); it.HasNext(); it.Next()) {
    const char* descriptor = it.GetDescriptor();
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
    if (!HasSameDescriptorClasses(descriptor, klass1, klass2)) {
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
  if (klass1 == klass2) {
    return true;
  }
  Class* found1 = FindClass(descriptor, klass1->GetClassLoader());
  // TODO: found1 == NULL
  Class* found2 = FindClass(descriptor, klass2->GetClassLoader());
  // TODO: found2 == NULL
  // TODO: lookup found1 in initiating loader list
  if (found1 == NULL || found2 == NULL) {
    Thread::Current()->ClearException();
    return found1 == found2;
  } else {
    return true;
  }
}

bool ClassLinker::InitializeSuperClass(Class* klass, bool can_run_clinit) {
  CHECK(klass != NULL);
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    Class* super_class = klass->GetSuperClass();
    if (super_class->GetStatus() != Class::kStatusInitialized) {
      CHECK(!super_class->IsInterface());
      Thread* self = Thread::Current();
      klass->MonitorEnter(self);
      bool super_initialized = InitializeClass(super_class, can_run_clinit);
      klass->MonitorExit(self);
      // TODO: check for a pending exception
      if (!super_initialized) {
        if (!can_run_clinit) {
         // Don't set status to error when we can't run <clinit>.
         CHECK_EQ(klass->GetStatus(), Class::kStatusInitializing);
         klass->SetStatus(Class::kStatusVerified);
         return false;
        }
        klass->SetStatus(Class::kStatusError);
        klass->NotifyAll();
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::EnsureInitialized(Class* c, bool can_run_clinit) {
  CHECK(c != NULL);
  if (c->IsInitialized()) {
    return true;
  }

  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  InitializeClass(c, can_run_clinit);
  return !self->IsExceptionPending();
}

void ClassLinker::ConstructFieldMap(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                                    Class* c, std::map<uint32_t, Field*>& field_map) {
  const ClassLoader* cl = c->GetClassLoader();
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  ClassDataItemIterator it(dex_file, class_data);
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    field_map[i] = ResolveField(dex_file, it.GetMemberIndex(), c->GetDexCache(), cl, true);
  }
}

void ClassLinker::InitializeStaticFields(Class* klass) {
  size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields == 0) {
    return;
  }
  DexCache* dex_cache = klass->GetDexCache();
  // TODO: this seems like the wrong check. do we really want !IsPrimitive && !IsArray?
  if (dex_cache == NULL) {
    return;
  }
  const DexFile& dex_file = FindDexFile(dex_cache);
  const std::string descriptor(klass->GetDescriptor()->ToModifiedUtf8());
  const DexFile::ClassDef* dex_class_def = dex_file.FindClassDef(descriptor);
  CHECK(dex_class_def != NULL);
  EncodedStaticFieldValueIterator it(dex_file, dex_cache, this, *dex_class_def);

  if (it.HasNext()) {
    // We reordered the fields, so we need to be able to map the field indexes to the right fields.
    std::map<uint32_t, Field*> field_map;
    ConstructFieldMap(dex_file, *dex_class_def, klass, field_map);
    for (size_t i = 0; it.HasNext(); i++, it.Next()) {
      it.ReadValueToField(field_map[i]);
    }
  }
}

bool ClassLinker::LinkClass(SirtRef<Class>& klass) {
  CHECK_EQ(Class::kStatusLoaded, klass->GetStatus());
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass)) {
    return false;
  }
  if (!LinkInstanceFields(klass)) {
    return false;
  }
  if (!LinkStaticFields(klass)) {
    return false;
  }
  CreateReferenceInstanceOffsets(klass);
  CreateReferenceStaticOffsets(klass);
  CHECK_EQ(Class::kStatusLoaded, klass->GetStatus());
  klass->SetStatus(Class::kStatusResolved);
  return true;
}

bool ClassLinker::LoadSuperAndInterfaces(SirtRef<Class>& klass, const DexFile& dex_file) {
  CHECK_EQ(Class::kStatusIdx, klass->GetStatus());
  if (klass->GetSuperClassTypeIdx() != DexFile::kDexNoIndex16) {
    Class* super_class = ResolveType(dex_file, klass->GetSuperClassTypeIdx(), klass.get());
    if (super_class == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    klass->SetSuperClass(super_class);
  }
  for (size_t i = 0; i < klass->NumInterfaces(); ++i) {
    uint32_t idx = klass->GetInterfacesTypeIdx()->Get(i);
    Class* interface = ResolveType(dex_file, idx, klass.get());
    klass->SetInterface(i, interface);
    if (interface == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    // Verify
    if (!klass->CanAccess(interface)) {
      // TODO: the RI seemed to ignore this in my testing.
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
          "Interface %s implemented by class %s is inaccessible",
          PrettyDescriptor(interface->GetDescriptor()).c_str(),
          PrettyDescriptor(klass->GetDescriptor()).c_str());
      return false;
    }
  }
  // Mark the class as loaded.
  klass->SetStatus(Class::kStatusLoaded);
  return true;
}

bool ClassLinker::LinkSuperClass(SirtRef<Class>& klass) {
  CHECK(!klass->IsPrimitive());
  Class* super = klass->GetSuperClass();
  if (klass->GetDescriptor()->Equals("Ljava/lang/Object;")) {
    if (super != NULL) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassFormatError;",
          "java.lang.Object must not have a superclass");
      return false;
    }
    return true;
  }
  if (super == NULL) {
    ThrowLinkageError("No superclass defined for class %s",
        PrettyDescriptor(klass->GetDescriptor()).c_str());
    return false;
  }
  // Verify
  if (super->IsFinal() || super->IsInterface()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
        "Superclass %s of %s is %s",
        PrettyDescriptor(super->GetDescriptor()).c_str(),
        PrettyDescriptor(klass->GetDescriptor()).c_str(),
        super->IsFinal() ? "declared final" : "an interface");
    return false;
  }
  if (!klass->CanAccess(super)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
        "Superclass %s is inaccessible by %s",
        PrettyDescriptor(super->GetDescriptor()).c_str(),
        PrettyDescriptor(klass->GetDescriptor()).c_str());
    return false;
  }

  // Inherit kAccClassIsFinalizable from the superclass in case this class doesn't override finalize.
  if (super->IsFinalizable()) {
    klass->SetFinalizable();
  }

  // Inherit reference flags (if any) from the superclass.
  int reference_flags = (super->GetAccessFlags() & kAccReferenceFlagsMask);
  if (reference_flags != 0) {
    klass->SetAccessFlags(klass->GetAccessFlags() | reference_flags);
  }
  // Disallow custom direct subclasses of java.lang.ref.Reference.
  if (init_done_ && super == GetClassRoot(kJavaLangRefReference)) {
    ThrowLinkageError("Class %s attempts to subclass java.lang.ref.Reference, which is not allowed",
        PrettyDescriptor(klass->GetDescriptor()).c_str());
    return false;
  }

#ifndef NDEBUG
  // Ensure super classes are fully resolved prior to resolving fields..
  while (super != NULL) {
    CHECK(super->IsResolved());
    super = super->GetSuperClass();
  }
#endif
  return true;
}

// Populate the class vtable and itable. Compute return type indices.
bool ClassLinker::LinkMethods(SirtRef<Class>& klass) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      ThrowClassFormatError("Too many methods on interface: %d", count);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i)->SetMethodIndex(i);
    }
    // Link interface method tables
    return LinkInterfaceMethods(klass);
  } else {
    // Link virtual and interface method tables
    return LinkVirtualMethods(klass) && LinkInterfaceMethods(klass);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(SirtRef<Class>& klass) {
  if (klass->HasSuperClass()) {
    uint32_t max_count = klass->NumVirtualMethods() + klass->GetSuperClass()->GetVTable()->GetLength();
    size_t actual_count = klass->GetSuperClass()->GetVTable()->GetLength();
    CHECK_LE(actual_count, max_count);
    // TODO: do not assign to the vtable field until it is fully constructed.
    ObjectArray<Method>* vtable = klass->GetSuperClass()->GetVTable()->CopyOf(max_count);
    // See if any of our virtual methods override the superclass.
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      Method* local_method = klass->GetVirtualMethodDuringLinking(i);
      size_t j = 0;
      for (; j < actual_count; ++j) {
        Method* super_method = vtable->Get(j);
        if (local_method->HasSameNameAndSignature(super_method)) {
          // Verify
          if (super_method->IsFinal()) {
            ThrowLinkageError("Method %s.%s overrides final method in class %s",
                PrettyDescriptor(klass->GetDescriptor()).c_str(),
                local_method->GetName()->ToModifiedUtf8().c_str(),
                PrettyDescriptor(super_method->GetDeclaringClass()->GetDescriptor()).c_str());
            return false;
          }
          vtable->Set(j, local_method);
          local_method->SetMethodIndex(j);
          break;
        }
      }
      if (j == actual_count) {
        // Not overriding, append.
        vtable->Set(actual_count, local_method);
        local_method->SetMethodIndex(actual_count);
        actual_count += 1;
      }
    }
    if (!IsUint(16, actual_count)) {
      ThrowClassFormatError("Too many methods defined on class: %d", actual_count);
      return false;
    }
    // Shrink vtable if possible
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable = vtable->CopyOf(actual_count);
    }
    klass->SetVTable(vtable);
  } else {
    CHECK(klass->GetDescriptor()->Equals("Ljava/lang/Object;"));
    uint32_t num_virtual_methods = klass->NumVirtualMethods();
    if (!IsUint(16, num_virtual_methods)) {
      ThrowClassFormatError("Too many methods: %d", num_virtual_methods);
      return false;
    }
    SirtRef<ObjectArray<Method> > vtable(AllocObjectArray<Method>(num_virtual_methods));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      Method* virtual_method = klass->GetVirtualMethodDuringLinking(i);
      vtable->Set(i, virtual_method);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable.get());
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(SirtRef<Class>& klass) {
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->GetIfTableCount();
  } else {
    super_ifcount = 0;
  }
  size_t ifcount = super_ifcount;
  ifcount += klass->NumInterfaces();
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    ifcount += klass->GetInterface(i)->GetIfTableCount();
  }
  if (ifcount == 0) {
    // TODO: enable these asserts with klass status validation
    // DCHECK_EQ(klass->GetIfTableCount(), 0);
    // DCHECK(klass->GetIfTable() == NULL);
    return true;
  }
  SirtRef<ObjectArray<InterfaceEntry> > iftable(AllocObjectArray<InterfaceEntry>(ifcount));
  if (super_ifcount != 0) {
    ObjectArray<InterfaceEntry>* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; i++) {
      iftable->Set(i, AllocInterfaceEntry(super_iftable->Get(i)->GetInterface()));
    }
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    Class* interface = klass->GetInterface(i);
    DCHECK(interface != NULL);
    if (!interface->IsInterface()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
          "Class %s implements non-interface class %s",
          PrettyDescriptor(klass->GetDescriptor()).c_str(),
          PrettyDescriptor(interface->GetDescriptor()).c_str());
      return false;
    }
    // Add this interface.
    iftable->Set(idx++, AllocInterfaceEntry(interface));
    // Add this interface's superinterfaces.
    for (int32_t j = 0; j < interface->GetIfTableCount(); j++) {
      iftable->Set(idx++, AllocInterfaceEntry(interface->GetIfTable()->Get(j)->GetInterface()));
    }
  }
  klass->SetIfTable(iftable.get());
  CHECK_EQ(idx, ifcount);

  // If we're an interface, we don't need the vtable pointers, so we're done.
  if (klass->IsInterface() /*|| super_ifcount == ifcount*/) {
    return true;
  }
  std::vector<Method*> miranda_list;
  for (size_t i = 0; i < ifcount; ++i) {
    InterfaceEntry* interface_entry = iftable->Get(i);
    Class* interface = interface_entry->GetInterface();
    ObjectArray<Method>* method_array = AllocObjectArray<Method>(interface->NumVirtualMethods());
    interface_entry->SetMethodArray(method_array);
    ObjectArray<Method>* vtable = klass->GetVTableDuringLinking();
    for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
      Method* interface_method = interface->GetVirtualMethod(j);
      int32_t k;
      // For each method listed in the interface's method list, find the
      // matching method in our class's method list.  We want to favor the
      // subclass over the superclass, which just requires walking
      // back from the end of the vtable.  (This only matters if the
      // superclass defines a private method and this class redefines
      // it -- otherwise it would use the same vtable slot.  In .dex files
      // those don't end up in the virtual method table, so it shouldn't
      // matter which direction we go.  We walk it backward anyway.)
      for (k = vtable->GetLength() - 1; k >= 0; --k) {
        Method* vtable_method = vtable->Get(k);
        if (interface_method->HasSameNameAndSignature(vtable_method)) {
          if (!vtable_method->IsPublic()) {
            Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                "Implementation not public: %s", PrettyMethod(vtable_method).c_str());
            return false;
          }
          method_array->Set(j, vtable_method);
          break;
        }
      }
      if (k < 0) {
        SirtRef<Method> miranda_method(NULL);
        for (size_t mir = 0; mir < miranda_list.size(); mir++) {
          if (miranda_list[mir]->HasSameNameAndSignature(interface_method)) {
            miranda_method.reset(miranda_list[mir]);
            break;
          }
        }
        if (miranda_method.get() == NULL) {
          // point the interface table at a phantom slot
          miranda_method.reset(AllocMethod());
          memcpy(miranda_method.get(), interface_method, sizeof(Method));
          miranda_list.push_back(miranda_method.get());
        }
        method_array->Set(j, miranda_method.get());
      }
    }
  }
  if (!miranda_list.empty()) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_list.size();
    klass->SetVirtualMethods((old_method_count == 0)
                             ? AllocObjectArray<Method>(new_method_count)
                             : klass->GetVirtualMethods()->CopyOf(new_method_count));

    ObjectArray<Method>* vtable = klass->GetVTableDuringLinking();
    CHECK(vtable != NULL);
    int old_vtable_count = vtable->GetLength();
    int new_vtable_count = old_vtable_count + miranda_list.size();
    vtable = vtable->CopyOf(new_vtable_count);
    for (size_t i = 0; i < miranda_list.size(); ++i) {
      Method* method = miranda_list[i];
      // Leave the declaring class alone as type indices are relative to it
      method->SetAccessFlags(method->GetAccessFlags() | kAccMiranda);
      method->SetMethodIndex(0xFFFF & (old_vtable_count + i));
      klass->SetVirtualMethod(old_method_count + i, method);
      vtable->Set(old_vtable_count + i, method);
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->SetVTable(vtable);
  }

  ObjectArray<Method>* vtable = klass->GetVTableDuringLinking();
  for (int i = 0; i < vtable->GetLength(); ++i) {
    CHECK(vtable->Get(i) != NULL);
  }

//  klass->DumpClass(std::cerr, Class::kDumpClassFullDetail);

  return true;
}

bool ClassLinker::LinkInstanceFields(SirtRef<Class>& klass) {
  CHECK(klass.get() != NULL);
  return LinkFields(klass, false);
}

bool ClassLinker::LinkStaticFields(SirtRef<Class>& klass) {
  CHECK(klass.get() != NULL);
  size_t allocated_class_size = klass->GetClassSize();
  bool success = LinkFields(klass, true);
  CHECK_EQ(allocated_class_size, klass->GetClassSize());
  return success;
}

struct LinkFieldsComparator {
  bool operator()(const Field* field1, const Field* field2) {
    // First come reference fields, then 64-bit, and finally 32-bit
    Primitive::Type type1 = field1->GetPrimitiveType();
    Primitive::Type type2 = field2->GetPrimitiveType();
    bool isPrimitive1 = type1 != Primitive::kPrimNot;
    bool isPrimitive2 = type2 != Primitive::kPrimNot;
    bool is64bit1 = isPrimitive1 && (type1 == Primitive::kPrimLong || type1 == Primitive::kPrimDouble);
    bool is64bit2 = isPrimitive2 && (type2 == Primitive::kPrimLong || type2 == Primitive::kPrimDouble);
    int order1 = (!isPrimitive1 ? 0 : (is64bit1 ? 1 : 2));
    int order2 = (!isPrimitive2 ? 0 : (is64bit2 ? 1 : 2));
    if (order1 != order2) {
      return order1 < order2;
    }

    // same basic group? then sort by string.
    std::string name1 = field1->GetName()->ToModifiedUtf8();
    std::string name2 = field2->GetName()->ToModifiedUtf8();
    return name1 < name2;
  }
};

bool ClassLinker::LinkFields(SirtRef<Class>& klass, bool is_static) {
  size_t num_fields =
      is_static ? klass->NumStaticFields() : klass->NumInstanceFields();

  ObjectArray<Field>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();

  // Initialize size and field_offset
  size_t size;
  MemberOffset field_offset(0);
  if (is_static) {
    size = klass->GetClassSize();
    field_offset = Class::FieldsOffset();
  } else {
    Class* super_class = klass->GetSuperClass();
    if (super_class != NULL) {
      CHECK(super_class->IsResolved());
      field_offset = MemberOffset(super_class->GetObjectSize());
    }
    size = field_offset.Uint32Value();
  }

  CHECK_EQ(num_fields == 0, fields == NULL);

  // we want a relatively stable order so that adding new fields
  // minimizes disruption of C++ version such as Class and Method.
  std::deque<Field*> grouped_and_sorted_fields;
  for (size_t i = 0; i < num_fields; i++) {
    grouped_and_sorted_fields.push_back(fields->Get(i));
  }
  std::sort(grouped_and_sorted_fields.begin(),
            grouped_and_sorted_fields.end(),
            LinkFieldsComparator());

  // References should be at the front.
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  for (; current_field < num_fields; current_field++) {
    Field* field = grouped_and_sorted_fields.front();
    Primitive::Type type = field->GetPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break; // past last reference, move on to the next phase
    }
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    fields->Set(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (current_field != num_fields && !IsAligned<8>(field_offset.Uint32Value())) {
    for (size_t i = 0; i < grouped_and_sorted_fields.size(); i++) {
      Field* field = grouped_and_sorted_fields[i];
      Primitive::Type type = field->GetPrimitiveType();
      CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
      if (type == Primitive::kPrimLong || type == Primitive::kPrimDouble) {
        continue;
      }
      fields->Set(current_field++, field);
      field->SetOffset(field_offset);
      // drop the consumed field
      grouped_and_sorted_fields.erase(grouped_and_sorted_fields.begin() + i);
      break;
    }
    // whether we found a 32-bit field for padding or not, we advance
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  DCHECK(current_field == num_fields || IsAligned<8>(field_offset.Uint32Value()));
  while (!grouped_and_sorted_fields.empty()) {
    Field* field = grouped_and_sorted_fields.front();
    grouped_and_sorted_fields.pop_front();
    Primitive::Type type = field->GetPrimitiveType();
    CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
    fields->Set(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                ((type == Primitive::kPrimLong || type == Primitive::kPrimDouble)
                                 ? sizeof(uint64_t)
                                 : sizeof(uint32_t)));
    current_field++;
  }

  // We lie to the GC about the java.lang.ref.Reference.referent field, so it doesn't scan it.
  if (!is_static && klass->GetDescriptor()->Equals("Ljava/lang/ref/Reference;")) {
    // We know there are no non-reference fields in the Reference classes, and we know
    // that 'referent' is alphabetically last, so this is easy...
    CHECK_EQ(num_reference_fields, num_fields);
    CHECK(fields->Get(num_fields - 1)->GetName()->Equals("referent"));
    --num_reference_fields;
  }

#ifndef NDEBUG
  // Make sure that all reference fields appear before
  // non-reference fields, and all double-wide fields are aligned.
  bool seen_non_ref = false;
  for (size_t i = 0; i < num_fields; i++) {
    Field* field = fields->Get(i);
    if (false) {  // enable to debug field layout
      LOG(INFO) << "LinkFields: " << (is_static ? "static" : "instance")
                << " class=" << PrettyClass(klass.get())
                << " field=" << PrettyField(field)
                << " offset=" << field->GetField32(MemberOffset(Field::OffsetOffset()), false);
    }
    Primitive::Type type = field->GetPrimitiveType();
    bool is_primitive = type != Primitive::kPrimNot;
    if (klass->GetDescriptor()->Equals("Ljava/lang/ref/Reference;") && field->GetName()->Equals("referent")) {
      is_primitive = true; // We lied above, so we have to expect a lie here.
    }
    if (is_primitive) {
      if (!seen_non_ref) {
        seen_non_ref = true;
        DCHECK_EQ(num_reference_fields, i);
      }
    } else {
      DCHECK(!seen_non_ref);
    }
  }
  if (!seen_non_ref) {
    DCHECK_EQ(num_fields, num_reference_fields);
  }
#endif
  size = field_offset.Uint32Value();
  // Update klass
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    klass->SetClassSize(size);
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      klass->SetObjectSize(size);
    }
  }
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceInstanceOffsets(SirtRef<Class>& klass) {
  uint32_t reference_offsets = 0;
  Class* super_class = klass->GetSuperClass();
  if (super_class != NULL) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    // If our superclass overflowed, we don't stand a chance.
    if (reference_offsets == CLASS_WALK_SUPER) {
      klass->SetReferenceInstanceOffsets(reference_offsets);
      return;
    }
  }
  CreateReferenceOffsets(klass, false, reference_offsets);
}

void ClassLinker::CreateReferenceStaticOffsets(SirtRef<Class>& klass) {
  CreateReferenceOffsets(klass, true, 0);
}

void ClassLinker::CreateReferenceOffsets(SirtRef<Class>& klass, bool is_static,
                                         uint32_t reference_offsets) {
  size_t num_reference_fields =
      is_static ? klass->NumReferenceStaticFieldsDuringLinking()
                : klass->NumReferenceInstanceFieldsDuringLinking();
  const ObjectArray<Field>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();
  // All of the fields that contain object references are guaranteed
  // to be at the beginning of the fields list.
  for (size_t i = 0; i < num_reference_fields; ++i) {
    // Note that byte_offset is the offset from the beginning of
    // object, not the offset into instance data
    const Field* field = fields->Get(i);
    MemberOffset byte_offset = field->GetOffsetDuringLinking();
    CHECK_EQ(byte_offset.Uint32Value() & (CLASS_OFFSET_ALIGNMENT - 1), 0U);
    if (CLASS_CAN_ENCODE_OFFSET(byte_offset.Uint32Value())) {
      uint32_t new_bit = CLASS_BIT_FROM_OFFSET(byte_offset.Uint32Value());
      CHECK_NE(new_bit, 0U);
      reference_offsets |= new_bit;
    } else {
      reference_offsets = CLASS_WALK_SUPER;
      break;
    }
  }
  // Update fields in klass
  if (is_static) {
    klass->SetReferenceStaticOffsets(reference_offsets);
  } else {
    klass->SetReferenceInstanceOffsets(reference_offsets);
  }
}

String* ClassLinker::ResolveString(const DexFile& dex_file,
    uint32_t string_idx, DexCache* dex_cache) {
  String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::StringId& string_id = dex_file.GetStringId(string_idx);
  int32_t utf16_length = dex_file.GetStringLength(string_id);
  const char* utf8_data = dex_file.GetStringData(string_id);
  String* string = intern_table_->InternStrong(utf16_length, utf8_data);
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}

Class* ClassLinker::ResolveType(const DexFile& dex_file,
                                uint32_t type_idx,
                                DexCache* dex_cache,
                                const ClassLoader* class_loader) {
  Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == NULL) {
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(descriptor, class_loader);
    if (resolved != NULL) {
      // TODO: we used to throw here if resolved's class loader was not the
      //       boot class loader. This was to permit different classes with the
      //       same name to be loaded simultaneously by different loaders
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      DCHECK(Thread::Current()->IsExceptionPending());
    }
  }
  return resolved;
}

Method* ClassLinker::ResolveMethod(const DexFile& dex_file,
                                   uint32_t method_idx,
                                   DexCache* dex_cache,
                                   const ClassLoader* class_loader,
                                   bool is_direct) {
  Method* resolved = dex_cache->GetResolvedMethod(method_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
  std::string signature(dex_file.CreateMethodSignature(method_id.proto_idx_, NULL));
  if (is_direct) {
    resolved = klass->FindDirectMethod(name, signature);
  } else if (klass->IsInterface()) {
    resolved = klass->FindInterfaceMethod(name, signature);
  } else {
    resolved = klass->FindVirtualMethod(name, signature);
  }
  if (resolved != NULL) {
    dex_cache->SetResolvedMethod(method_idx, resolved);
  } else {
    ThrowNoSuchMethodError(is_direct ? "direct" : "virtual", klass, name, signature);
  }
  return resolved;
}

Field* ClassLinker::ResolveField(const DexFile& dex_file,
                                 uint32_t field_idx,
                                 DexCache* dex_cache,
                                 const ClassLoader* class_loader,
                                 bool is_static) {
  Field* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    return NULL;
  }

  const char* name = dex_file.GetFieldName(field_id);
  const char* type = dex_file.GetFieldTypeDescriptor(field_id);
  if (is_static) {
    resolved = klass->FindStaticField(name, type);
  } else {
    resolved = klass->FindInstanceField(name, type);
  }
  if (resolved != NULL) {
    dex_cache->SetResolvedField(field_idx, resolved);
  } else {
    DCHECK(Thread::Current()->IsExceptionPending())
        << PrettyClass(klass) << " " << name << " " << type << " " << is_static;
  }
  return resolved;
}

const char* ClassLinker::MethodShorty(uint32_t method_idx, Method* referrer) {
  Class* declaring_class = referrer->GetDeclaringClass();
  DexCache* dex_cache = declaring_class->GetDexCache();
  const DexFile& dex_file = FindDexFile(dex_cache);
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  return dex_file.GetShorty(method_id.proto_idx_);
}

void ClassLinker::DumpAllClasses(int flags) const {
  // TODO: at the time this was written, it wasn't safe to call PrettyField with the ClassLinker
  // lock held, because it might need to resolve a field's type, which would try to take the lock.
  std::vector<Class*> all_classes;
  {
    MutexLock mu(classes_lock_);
    typedef Table::const_iterator It;  // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
      all_classes.push_back(it->second);
    }
    for (It it = image_classes_.begin(), end = image_classes_.end(); it != end; ++it) {
      all_classes.push_back(it->second);
    }
  }

  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}

void ClassLinker::DumpForSigQuit(std::ostream& os) const {
  MutexLock mu(classes_lock_);
  os << "Loaded classes: " << image_classes_.size() << " image classes; "
     << classes_.size() << " allocated classes\n";
}

size_t ClassLinker::NumLoadedClasses() const {
  MutexLock mu(classes_lock_);
  return classes_.size() + image_classes_.size();
}

pid_t ClassLinker::GetClassesLockOwner() {
  return classes_lock_.GetOwner();
}

pid_t ClassLinker::GetDexLockOwner() {
  return dex_lock_.GetOwner();
}

}  // namespace art
