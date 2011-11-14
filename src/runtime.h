// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <stdio.h>

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

#include "constants.h"
#include "heap.h"
#include "globals.h"
#include "macros.h"
#include "runtime_stats.h"
#include "stringpiece.h"
#include "unordered_set.h"

namespace art {

template<class T> class PrimitiveArray;
typedef PrimitiveArray<int8_t> ByteArray;
class ClassLinker;
class ClassLoader;
class DexFile;
class Heap;
class InternTable;
class JavaVMExt;
class Method;
class MonitorList;
class SignalCatcher;
class String;
class ThreadList;

class Runtime {
 public:

  typedef std::vector<std::pair<StringPiece, const void*> > Options;

  class ParsedOptions {
   public:
    // returns null if problem parsing and ignore_unrecognized is false
    static ParsedOptions* Create(const Options& options, bool ignore_unrecognized);

    std::string boot_class_path_;
    std::string class_path_;
    std::string host_prefix_;
    std::vector<std::string> images_;
    bool check_jni_;
    std::string jni_trace_;
    bool is_zygote_;
    size_t heap_initial_size_;
    size_t heap_maximum_size_;
    size_t heap_growth_limit_;
    size_t stack_size_;
    size_t jni_globals_max_;
    size_t lock_profiling_threshold_;
    std::string stack_trace_file_;
    bool (*hook_is_sensitive_thread_)();
    jint (*hook_vfprintf_)(FILE* stream, const char* format, va_list ap);
    void (*hook_exit_)(jint status);
    void (*hook_abort_)();
    std::tr1::unordered_set<std::string> verbose_;
    std::vector<std::string> properties_;

    bool IsVerbose(const std::string& key) const {
      return verbose_.find(key) != verbose_.end();
    }

   private:
    ParsedOptions() {}
  };

  // Creates and initializes a new runtime.
  static Runtime* Create(const Options& options, bool ignore_unrecognized);

  bool IsVerboseStartup() const {
    return verbose_startup_;
  }

  bool IsZygote() const {
    return is_zygote_;
  }

  const std::string& GetHostPrefix() const {
    DCHECK(!IsStarted());
    return host_prefix_;
  }

  // Starts a runtime, which may cause threads to be started and code to run.
  void Start();

  bool IsStarted() const;

  static Runtime* Current() {
    return instance_;
  }

  // Compiles a dex file.
  static void Compile(const StringPiece& filename);

  // Aborts semi-cleanly. Used in the implementation of LOG(FATAL), which most
  // callers should prefer.
  // This isn't marked ((noreturn)) because then gcc will merge multiple calls
  // in a single function together. This reduces code size slightly, but means
  // that the native stack trace we get may point at the wrong call site.
  static void Abort(const char* file, int line);

  // Attaches the current native thread to the runtime.
  void AttachCurrentThread(const char* name, bool as_daemon);

  void CallExitHook(jint status);

  // Detaches the current native thread from the runtime.
  void DetachCurrentThread();

  void Dump(std::ostream& os);

  ~Runtime();

  const std::string& GetBootClassPath() const {
    return boot_class_path_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  const std::string& GetClassPath() const {
    return class_path_;
  }

  size_t GetDefaultStackSize() const {
    return default_stack_size_;
  }

  InternTable* GetInternTable() const {
    return intern_table_;
  }

  JavaVMExt* GetJavaVM() const {
    return java_vm_;
  }

  const std::vector<std::string>& GetProperties() const {
    return properties_;
  }

  MonitorList* GetMonitorList() const {
    return monitor_list_;
  }

  ThreadList* GetThreadList() const {
    return thread_list_;
  }

  const char* GetVersion() const {
    return "2.0.0";
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  bool HasJniDlsymLookupStub() const;
  ByteArray* GetJniDlsymLookupStub() const;
  void SetJniDlsymLookupStub(ByteArray* jni_stub_array);

  bool HasAbstractMethodErrorStubArray() const;
  ByteArray* GetAbstractMethodErrorStubArray() const;
  void SetAbstractMethodErrorStubArray(ByteArray* abstract_method_error_stub_array);

  enum TrampolineType {
    kInstanceMethod,
    kStaticMethod,
    kUnknownMethod,
    kLastTrampolineMethodType  // Value used for iteration
  };
  static TrampolineType GetTrampolineType(Method* method);
  bool HasResolutionStubArray(TrampolineType type) const;
  ByteArray* GetResolutionStubArray(TrampolineType type) const;
  void SetResolutionStubArray(ByteArray* resolution_stub_array, TrampolineType type);

  // Returns a special method that describes all callee saves being spilled to the stack.
  enum CalleeSaveType {
    kSaveAll,
    kRefsOnly,
    kRefsAndArgs,
    kLastCalleeSaveType  // Value used for iteration
  };
  Method* CreateCalleeSaveMethod(InstructionSet insns, CalleeSaveType type);
  bool HasCalleeSaveMethod(CalleeSaveType type) const;
  Method* GetCalleeSaveMethod(CalleeSaveType type) const;
  void SetCalleeSaveMethod(Method* method, CalleeSaveType type);

  Method* CreateRefOnlyCalleeSaveMethod(InstructionSet insns);
  Method* CreateRefAndArgsCalleeSaveMethod(InstructionSet insns);

  int32_t GetStat(int kind);

  RuntimeStats* GetStats();

  bool HasStatsEnabled() const {
    return stats_enabled_;
  }

  void ResetStats(int kinds);

  void SetStatsEnabled(bool new_state);

  void DidForkFromZygote();

 private:
  static void PlatformAbort(const char*, int);

  Runtime();

  void BlockSignals();

  bool Init(const Options& options, bool ignore_unrecognized);
  void InitNativeMethods();
  void RegisterRuntimeNativeMethods(JNIEnv*);

  void StartDaemonThreads();
  void StartSignalCatcher();

  bool verbose_startup_;
  bool is_zygote_;

  // The host prefix is used during cross compilation. It is removed
  // from the start of host paths such as:
  //    $ANDROID_PRODUCT_OUT/data/art-cache/boot.oat
  // to produce target paths such as
  //    /system/framework/boot.oat
  // Similarly it is prepended to target paths to arrive back at a
  // host past. In both cases this is necessary because image and oat
  // files embedded expect paths of dependent files (an image points
  // to an oat file and an oat files to one or more dex files). These
  // files contain the expected target path.
  std::string host_prefix_;

  std::string boot_class_path_;
  std::string class_path_;
  std::vector<std::string> properties_;

  // The default stack size for managed threads created by the runtime.
  size_t default_stack_size_;

  MonitorList* monitor_list_;

  ThreadList* thread_list_;

  InternTable* intern_table_;

  ClassLinker* class_linker_;

  SignalCatcher* signal_catcher_;
  std::string stack_trace_file_;

  JavaVMExt* java_vm_;

  ByteArray* jni_stub_array_;

  ByteArray* abstract_method_error_stub_array_;

  ByteArray* resolution_stub_array_[kLastTrampolineMethodType];

  Method* callee_save_method_[kLastCalleeSaveType];

  // As returned by ClassLoader.getSystemClassLoader()
  ClassLoader* system_class_loader_;

  bool started_;

  // Hooks supported by JNI_CreateJavaVM
  jint (*vfprintf_)(FILE* stream, const char* format, va_list ap);
  void (*exit_)(jint status);
  void (*abort_)();

  bool stats_enabled_;
  RuntimeStats stats_;

  // A pointer to the active runtime or NULL.
  static Runtime* instance_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
