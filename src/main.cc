// Copyright 2011 Google Inc. All Rights Reserved.

#include <cstring>
#include <cstdio>
#include <signal.h>
#include <string>

#include "jni.h"
#include "logging.h"
#include "scoped_ptr.h"
#include "ScopedLocalRef.h"

// TODO: move this into the runtime.
static void BlockSigpipe() {
  sigset_t sigset;
  if (sigemptyset(&sigset) == -1) {
    PLOG(ERROR) << "sigemptyset failed";
    return;
  }
  if (sigaddset(&sigset, SIGPIPE) == -1) {
    PLOG(ERROR) << "sigaddset failed";
    return;
  }
  if (sigprocmask(SIG_BLOCK, &sigset, NULL) == -1) {
    PLOG(ERROR) << "sigprocmask failed";
  }
}

// TODO: this code should be shared with other parts of the system
// that create string arrays.
//Create a String[] and populate it with the contents of argv.
static jobjectArray CreateStringArray(JNIEnv* env, char** argv, int argc) {
  // Find the String class.
  ScopedLocalRef<jclass> klass(env, env->FindClass("java/lang/String"));
  if (env->ExceptionCheck()) {
    fprintf(stderr, "Got exception while finding class String\n");
    return NULL;
  }
  DCHECK(klass.get() != NULL);

  // Create an array of String elements.
  jobjectArray args = env->NewObjectArray(argc, klass.get(), NULL);
  if (env->ExceptionCheck()) {
    fprintf(stderr, "Got exception while creating String array\n");
    return NULL;
  }
  DCHECK(args != NULL);

  // Allocate a string object for each argv element.
  for (int i = 0; i < argc; ++i) {
    ScopedLocalRef<jstring> elt(env, env->NewStringUTF(argv[i]));
    if (env->ExceptionCheck()) {
      fprintf(stderr, "Got exception while allocating Strings\n");
      return NULL;
    }
    DCHECK(elt.get() != NULL);
    env->SetObjectArrayElement(args, i, elt.get());
  }

  return args;
}

// Determine whether or not the specified method is public.
//
// Returns JNI_TRUE on success, JNI_FALSE on failure.
static bool IsMethodPublic(JNIEnv* env, jclass clazz, jmethodID method_id) {
  ScopedLocalRef<jobject> reflected(env, env->ToReflectedMethod(clazz,
      method_id, JNI_FALSE));
  if (reflected.get() == NULL) {
    fprintf(stderr, "Unable to get reflected method\n");
    return false;
  }
  // We now have a Method instance.  We need to call its
  // getModifiers() method.
  ScopedLocalRef<jclass> method(env,
      env->FindClass("java/lang/reflect/Method"));
  if (method.get() == NULL) {
    fprintf(stderr, "Unable to find class Method\n");
    return false;
  }
  jmethodID get_modifiers = env->GetMethodID(method.get(),
                                             "getModifiers",
                                             "()I");
  if (get_modifiers == NULL) {
    fprintf(stderr, "Unable to find reflect.Method.getModifiers\n");
    return false;
  }
  static const int PUBLIC = 0x0001;   // java.lang.reflect.Modifiers.PUBLIC
  int modifiers = env->CallIntMethod(method.get(), get_modifiers);
  if ((modifiers & PUBLIC) == 0) {
    return false;
  }
  return true;
}

static bool InvokeMain(JavaVM* vm, JNIEnv* env, int argc, char** argv) {
  // We want to call main() with a String array with our arguments in
  // it.  Create an array and populate it.  Note argv[0] is not
  // included.
  ScopedLocalRef<jobjectArray> args(env,
      CreateStringArray(env, argv + 1, argc - 1));
  if (args.get() == NULL) {
    return false;
  }

  // Find [class].main(String[]).

  // Convert "com.android.Blah" to "com/android/Blah".
  std::string class_name = argv[0];
  std::replace(class_name.begin(), class_name.end(), '.', '/');

  ScopedLocalRef<jclass> klass(env, env->FindClass(class_name.c_str()));
  if (klass.get() == NULL) {
    fprintf(stderr, "Unable to locate class '%s'\n", class_name.c_str());
    return false;
  }

  jmethodID method = env->GetStaticMethodID(klass.get(),
                                            "main",
                                            "([Ljava/lang/String;)V");
  if (method == NULL) {
    fprintf(stderr, "Unable to find static main(String[]) in '%s'\n",
            class_name.c_str());
    return false;
  }

  // Make sure the method is public.  JNI doesn't prevent us from
  // calling a private method, so we have to check it explicitly.
  if (!IsMethodPublic(env, klass.get(), method)) {
    fprintf(stderr, "Sorry, main() is not public\n");
    return false;
  }

  // Invoke main().

  env->CallStaticVoidMethod(klass.get(), method, args.get());
  if (env->ExceptionCheck()) {
    return false;
  } else {
    return true;
  }
}

// Parse arguments.  Most of it just gets passed through to the VM.
// The JNI spec defines a handful of standard arguments.
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  // Skip over argv[0].
  argv++;
  argc--;

  // If we're adding any additional stuff, e.g. function hook specifiers,
  // add them to the count here.
  //
  // We're over-allocating, because this includes the options to the VM
  // plus the options to the program.
  int option_count = argc;
  scoped_array<JavaVMOption> options(new JavaVMOption[option_count]());

  // Copy options over.  Everything up to the name of the class starts
  // with a '-' (the function hook stuff is strictly internal).
  //
  // [Do we need to catch & handle "-jar" here?]
  bool need_extra = false;
  int curr_opt, arg_idx;
  for (curr_opt = arg_idx = 0; arg_idx < argc; arg_idx++) {
    if (argv[arg_idx][0] != '-' && !need_extra) {
      break;
    }
    options[curr_opt++].optionString = argv[arg_idx];

    // Some options require an additional argument.
    need_extra = false;
    if (strcmp(argv[arg_idx], "-classpath") == 0 ||
        strcmp(argv[arg_idx], "-cp") == 0) {
      // others?
      need_extra = true;
    }
  }

  if (need_extra) {
    fprintf(stderr, "VM requires value after last option flag\n");
    return EXIT_FAILURE;
  }

  // Make sure they provided a class name.  We do this after VM init
  // so that things like "-Xrunjdwp:help" have the opportunity to emit
  // a usage statement.
  if (arg_idx == argc) {
    fprintf(stderr, "Class name required\n");
    return EXIT_FAILURE;
  }

  // insert additional internal options here

  DCHECK_LE(curr_opt, option_count);

  JavaVMInitArgs init_args;
  init_args.version = JNI_VERSION_1_4;
  init_args.options = options.get();
  init_args.nOptions = curr_opt;
  init_args.ignoreUnrecognized = JNI_FALSE;

  BlockSigpipe();

  // Start VM.  The current thread becomes the main thread of the VM.
  JavaVM* vm = NULL;
  JNIEnv* env = NULL;
  if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
    fprintf(stderr, "VM init failed (check log file)\n");
    return EXIT_FAILURE;
  }

  bool success = InvokeMain(vm, env, argc - arg_idx, &argv[arg_idx]);

  if (vm != NULL && vm->DetachCurrentThread() != JNI_OK) {
    fprintf(stderr, "Warning: unable to detach main thread\n");
    success = false;
  }

  if (vm != NULL && vm->DestroyJavaVM() != 0) {
    fprintf(stderr, "Warning: VM did not shut down cleanly\n");
    success = false;
  }

  int retval = success ? EXIT_SUCCESS : EXIT_FAILURE;
  return retval;
}
