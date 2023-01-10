/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_INSTRUMENTATION_H_
#define ART_RUNTIME_INSTRUMENTATION_H_

#include <stdint.h>

#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_set>

#include "arch/instruction_set.h"
#include "base/enums.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/safe_map.h"
#include "gc_root.h"
#include "jvalue.h"
#include "offsets.h"

namespace art {
namespace mirror {
class Class;
class Object;
class Throwable;
}  // namespace mirror
class ArtField;
class ArtMethod;
template <typename T> class Handle;
template <typename T> class MutableHandle;
struct NthCallerVisitor;
union JValue;
class OatQuickMethodHeader;
class SHARED_LOCKABLE ReaderWriterMutex;
class ShadowFrame;
class Thread;
enum class DeoptimizationMethodType;

namespace instrumentation {


// Do we want to deoptimize for method entry and exit listeners or just try to intercept
// invocations? Deoptimization forces all code to run in the interpreter and considerably hurts the
// application's performance.
static constexpr bool kDeoptimizeForAccurateMethodEntryExitListeners = true;

// an optional frame is either Some(const ShadowFrame& current_frame) or None depending on if the
// method being exited has a shadow-frame associed with the current stack frame. In cases where
// there is no shadow-frame associated with this stack frame this will be None.
using OptionalFrame = std::optional<std::reference_wrapper<const ShadowFrame>>;

// Instrumentation event listener API. Registered listeners will get the appropriate call back for
// the events they are listening for. The call backs supply the thread, method and dex_pc the event
// occurred upon. The thread may or may not be Thread::Current().
struct InstrumentationListener {
  InstrumentationListener() {}
  virtual ~InstrumentationListener() {}

  // Call-back for when a method is entered.
  virtual void MethodEntered(Thread* thread, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  virtual void MethodExited(Thread* thread,
                            ArtMethod* method,
                            OptionalFrame frame,
                            MutableHandle<mirror::Object>& return_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call-back for when a method is exited. The implementor should either handler-ize the return
  // value (if appropriate) or use the alternate MethodExited callback instead if they need to
  // go through a suspend point.
  virtual void MethodExited(Thread* thread,
                            ArtMethod* method,
                            OptionalFrame frame,
                            JValue& return_value)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when a method is popped due to an exception throw. A method will either cause a
  // MethodExited call-back or a MethodUnwind call-back when its activation is removed.
  virtual void MethodUnwind(Thread* thread,
                            ArtMethod* method,
                            uint32_t dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when the dex pc moves in a method.
  virtual void DexPcMoved(Thread* thread,
                          Handle<mirror::Object> this_object,
                          ArtMethod* method,
                          uint32_t new_dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when we read from a field.
  virtual void FieldRead(Thread* thread,
                         Handle<mirror::Object> this_object,
                         ArtMethod* method,
                         uint32_t dex_pc,
                         ArtField* field) = 0;

  virtual void FieldWritten(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            ArtField* field,
                            Handle<mirror::Object> field_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call-back for when we write into a field.
  virtual void FieldWritten(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            ArtField* field,
                            const JValue& field_value)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when an exception is thrown.
  virtual void ExceptionThrown(Thread* thread,
                               Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when an exception is caught/handled by java code.
  virtual void ExceptionHandled(Thread* thread, Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back for when we execute a branch.
  virtual void Branch(Thread* thread,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      int32_t dex_pc_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Call-back when a shadow_frame with the needs_notify_pop_ boolean set is popped off the stack by
  // either return or exceptions. Normally instrumentation listeners should ensure that there are
  // shadow-frames by deoptimizing stacks.
  virtual void WatchedFramePop(Thread* thread ATTRIBUTE_UNUSED,
                               const ShadowFrame& frame ATTRIBUTE_UNUSED)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
};

class Instrumentation;
// A helper to send instrumentation events while popping the stack in a safe way.
class InstrumentationStackPopper {
 public:
  explicit InstrumentationStackPopper(Thread* self);
  ~InstrumentationStackPopper() REQUIRES_SHARED(Locks::mutator_lock_);

  // Increase the number of frames being popped up to `stack_pointer`. Return true if the
  // frames were popped without any exceptions, false otherwise. The exception that caused
  // the pop is 'exception'.
  bool PopFramesTo(uintptr_t stack_pointer, /*in-out*/MutableHandle<mirror::Throwable>& exception)
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  Thread* self_;
  Instrumentation* instrumentation_;
  // The stack pointer limit for frames to pop.
  uintptr_t pop_until_;
};

// Instrumentation is a catch-all for when extra information is required from the runtime. The
// typical use for instrumentation is for profiling and debugging. Instrumentation may add stubs
// to method entry and exit, it may also force execution to be switched to the interpreter and
// trigger deoptimization.
class Instrumentation {
 public:
  enum InstrumentationEvent {
    kMethodEntered = 0x1,
    kMethodExited = 0x2,
    kMethodUnwind = 0x4,
    kDexPcMoved = 0x8,
    kFieldRead = 0x10,
    kFieldWritten = 0x20,
    kExceptionThrown = 0x40,
    kBranch = 0x80,
    kWatchedFramePop = 0x200,
    kExceptionHandled = 0x400,
  };

  enum class InstrumentationLevel {
    kInstrumentNothing,                   // execute without instrumentation
    kInstrumentWithInstrumentationStubs,  // execute with instrumentation entry/exit stubs
    kInstrumentWithInterpreter            // execute with interpreter
  };

  Instrumentation();

  static constexpr MemberOffset NeedsExitHooksOffset() {
    // Assert that instrumentation_stubs_installed_ is 8bits wide. If the size changes
    // update the compare instructions in the code generator when generating checks for
    // MethodEntryExitHooks.
    static_assert(sizeof(instrumentation_stubs_installed_) == 1,
                  "instrumentation_stubs_installed_ isn't expected size");
    return MemberOffset(OFFSETOF_MEMBER(Instrumentation, instrumentation_stubs_installed_));
  }

  static constexpr MemberOffset HaveMethodEntryListenersOffset() {
    // Assert that have_method_entry_listeners_ is 8bits wide. If the size changes
    // update the compare instructions in the code generator when generating checks for
    // MethodEntryExitHooks.
    static_assert(sizeof(have_method_entry_listeners_) == 1,
                  "have_method_entry_listeners_ isn't expected size");
    return MemberOffset(OFFSETOF_MEMBER(Instrumentation, have_method_entry_listeners_));
  }

  static constexpr MemberOffset HaveMethodExitListenersOffset() {
    // Assert that have_method_exit_listeners_ is 8bits wide. If the size changes
    // update the compare instructions in the code generator when generating checks for
    // MethodEntryExitHooks.
    static_assert(sizeof(have_method_exit_listeners_) == 1,
                  "have_method_exit_listeners_ isn't expected size");
    return MemberOffset(OFFSETOF_MEMBER(Instrumentation, have_method_exit_listeners_));
  }

  // Add a listener to be notified of the masked together sent of instrumentation events. This
  // suspend the runtime to install stubs. You are expected to hold the mutator lock as a proxy
  // for saying you should have suspended all threads (installing stubs while threads are running
  // will break).
  void AddListener(InstrumentationListener* listener, uint32_t events)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_);

  // Removes a listener possibly removing instrumentation stubs.
  void RemoveListener(InstrumentationListener* listener, uint32_t events)
      REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_);

  // Calls UndeoptimizeEverything which may visit class linker classes through ConfigureStubs.
  void DisableDeoptimization(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_);

  // Enables entry exit hooks support. This is called in preparation for debug requests that require
  // calling method entry / exit hooks.
  void EnableEntryExitHooks(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_);

  bool AreAllMethodsDeoptimized() const {
    return InterpreterStubsInstalled();
  }
  bool ShouldNotifyMethodEnterExitEvents() const REQUIRES_SHARED(Locks::mutator_lock_);

  // Executes everything with interpreter.
  void DeoptimizeEverything(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);

  // Executes everything with compiled code (or interpreter if there is no code). May visit class
  // linker classes through ConfigureStubs.
  void UndeoptimizeEverything(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);

  // Deoptimize a method by forcing its execution with the interpreter. Nevertheless, a static
  // method (except a class initializer) set to the resolution trampoline will be deoptimized only
  // once its declaring class is initialized.
  void Deoptimize(ArtMethod* method) REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_);

  // Undeoptimze the method by restoring its entrypoints. Nevertheless, a static method
  // (except a class initializer) set to the resolution trampoline will be updated only once its
  // declaring class is initialized.
  void Undeoptimize(ArtMethod* method) REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_);

  // Indicates whether the method has been deoptimized so it is executed with the interpreter.
  bool IsDeoptimized(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  // Indicates if any method needs to be deoptimized. This is used to avoid walking the stack to
  // determine if a deoptimization is required.
  bool IsDeoptimizedMethodsEmpty() const REQUIRES_SHARED(Locks::mutator_lock_);

  // Enable method tracing by installing instrumentation entry/exit stubs or interpreter.
  void EnableMethodTracing(const char* key,
                           bool needs_interpreter = kDeoptimizeForAccurateMethodEntryExitListeners)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);

  // Disable method tracing by uninstalling instrumentation entry/exit stubs or interpreter.
  void DisableMethodTracing(const char* key)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);


  void InstrumentQuickAllocEntryPoints() REQUIRES(!Locks::instrument_entrypoints_lock_);
  void UninstrumentQuickAllocEntryPoints() REQUIRES(!Locks::instrument_entrypoints_lock_);
  void InstrumentQuickAllocEntryPointsLocked()
      REQUIRES(Locks::instrument_entrypoints_lock_, !Locks::thread_list_lock_,
               !Locks::runtime_shutdown_lock_);
  void UninstrumentQuickAllocEntryPointsLocked()
      REQUIRES(Locks::instrument_entrypoints_lock_, !Locks::thread_list_lock_,
               !Locks::runtime_shutdown_lock_);
  void ResetQuickAllocEntryPoints() REQUIRES(Locks::runtime_shutdown_lock_);

  // Returns a string representation of the given entry point.
  static std::string EntryPointString(const void* code);

  // Initialize the entrypoint of the method .`aot_code` is the AOT code.
  void InitializeMethodsCode(ArtMethod* method, const void* aot_code)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Update the code of a method respecting any installed stubs.
  void UpdateMethodsCode(ArtMethod* method, const void* new_code)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Update the code of a native method to a JITed stub.
  void UpdateNativeMethodsCodeToJitCode(ArtMethod* method, const void* new_code)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Return the code that we can execute for an invoke including from the JIT.
  const void* GetCodeForInvoke(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  // Return the code that we can execute considering the current instrumentation level.
  // If interpreter stubs are installed return interpreter bridge. If the entry exit stubs
  // are installed return an instrumentation entry point. Otherwise, return the code that
  // can be executed including from the JIT.
  const void* GetMaybeInstrumentedCodeForInvoke(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void ForceInterpretOnly() {
    forced_interpret_only_ = true;
  }

  bool EntryExitStubsInstalled() const {
    return instrumentation_level_ == InstrumentationLevel::kInstrumentWithInstrumentationStubs ||
           instrumentation_level_ == InstrumentationLevel::kInstrumentWithInterpreter;
  }

  bool InterpreterStubsInstalled() const {
    return instrumentation_level_ == InstrumentationLevel::kInstrumentWithInterpreter;
  }

  // Called by ArtMethod::Invoke to determine dispatch mechanism.
  bool InterpretOnly() const {
    return forced_interpret_only_ || InterpreterStubsInstalled();
  }
  bool InterpretOnly(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsForcedInterpretOnly() const {
    return forced_interpret_only_;
  }

  bool AreExitStubsInstalled() const {
    return instrumentation_stubs_installed_;
  }

  bool HasMethodEntryListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_entry_listeners_;
  }

  bool HasMethodExitListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_exit_listeners_;
  }

  bool HasMethodUnwindListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_method_unwind_listeners_;
  }

  bool HasDexPcListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_dex_pc_listeners_;
  }

  bool HasFieldReadListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_field_read_listeners_;
  }

  bool HasFieldWriteListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_field_write_listeners_;
  }

  bool HasExceptionThrownListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_exception_thrown_listeners_;
  }

  bool HasBranchListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_branch_listeners_;
  }

  bool HasWatchedFramePopListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_watched_frame_pop_listeners_;
  }

  bool HasExceptionHandledListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_exception_handled_listeners_;
  }

  // Returns if dex pc events need to be reported for the specified method.
  // These events are reported when DexPCListeners are installed and at least one of the
  // following conditions hold:
  // 1. The method is deoptimized. This is done when there is a breakpoint on method.
  // 2. When the thread is deoptimized. This is used when single stepping a single thread.
  // 3. When interpreter stubs are installed. In this case no additional information is maintained
  //    about which methods need dex pc move events. This is usually used for features which need
  //    them for several methods across threads or need expensive processing. So it is OK to not
  //    further optimize this case.
  // DexPCListeners are installed when there is a breakpoint on any method / single stepping
  // on any of thread. These are removed when the last breakpoint was removed. See AddListener and
  // RemoveListener for more details.
  bool NeedsDexPcEvents(ArtMethod* method, Thread* thread) REQUIRES_SHARED(Locks::mutator_lock_);

  bool NeedsSlowInterpreterForListeners() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return have_field_read_listeners_ ||
           have_field_write_listeners_ ||
           have_watched_frame_pop_listeners_ ||
           have_exception_handled_listeners_;
  }

  // Inform listeners that a method has been entered. A dex PC is provided as we may install
  // listeners into executing code and get method enter events for methods already on the stack.
  void MethodEnterEvent(Thread* thread, ArtMethod* method) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasMethodEntryListeners())) {
      MethodEnterEventImpl(thread, method);
    }
  }

  // Inform listeners that a method has been exited.
  template<typename T>
  void MethodExitEvent(Thread* thread,
                       ArtMethod* method,
                       OptionalFrame frame,
                       T& return_value) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasMethodExitListeners())) {
      MethodExitEventImpl(thread, method, frame, return_value);
    }
  }

  // Inform listeners that a method has been exited due to an exception.
  void MethodUnwindEvent(Thread* thread,
                         ArtMethod* method,
                         uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Inform listeners that the dex pc has moved (only supported by the interpreter).
  void DexPcMovedEvent(Thread* thread,
                       ObjPtr<mirror::Object> this_object,
                       ArtMethod* method,
                       uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasDexPcListeners())) {
      DexPcMovedEventImpl(thread, this_object, method, dex_pc);
    }
  }

  // Inform listeners that a branch has been taken (only supported by the interpreter).
  void Branch(Thread* thread, ArtMethod* method, uint32_t dex_pc, int32_t offset) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasBranchListeners())) {
      BranchImpl(thread, method, dex_pc, offset);
    }
  }

  // Inform listeners that we read a field (only supported by the interpreter).
  void FieldReadEvent(Thread* thread,
                      ObjPtr<mirror::Object> this_object,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      ArtField* field) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasFieldReadListeners())) {
      FieldReadEventImpl(thread, this_object, method, dex_pc, field);
    }
  }

  // Inform listeners that we write a field (only supported by the interpreter).
  void FieldWriteEvent(Thread* thread,
                       ObjPtr<mirror::Object> this_object,
                       ArtMethod* method,
                       uint32_t dex_pc,
                       ArtField* field,
                       const JValue& field_value) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasFieldWriteListeners())) {
      FieldWriteEventImpl(thread, this_object, method, dex_pc, field, field_value);
    }
  }

  // Inform listeners that a branch has been taken (only supported by the interpreter).
  void WatchedFramePopped(Thread* thread, const ShadowFrame& frame) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(HasWatchedFramePopListeners())) {
      WatchedFramePopImpl(thread, frame);
    }
  }

  // Inform listeners that an exception was thrown.
  void ExceptionThrownEvent(Thread* thread, ObjPtr<mirror::Throwable> exception_object) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Inform listeners that an exception has been handled. This is not sent for native code or for
  // exceptions which reach the end of the thread's stack.
  void ExceptionHandledEvent(Thread* thread, ObjPtr<mirror::Throwable> exception_object) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  JValue GetReturnValue(ArtMethod* method, bool* is_ref, uint64_t* gpr_result, uint64_t* fpr_result)
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool PushDeoptContextIfNeeded(Thread* self,
                                DeoptimizationMethodType deopt_type,
                                bool is_ref,
                                const JValue& result) REQUIRES_SHARED(Locks::mutator_lock_);
  void DeoptimizeIfNeeded(Thread* self,
                          ArtMethod** sp,
                          DeoptimizationMethodType type,
                          JValue result,
                          bool is_ref) REQUIRES_SHARED(Locks::mutator_lock_);
  // TODO(mythria): Update uses of ShouldDeoptimizeCaller that takes a visitor by a method that
  // doesn't need to walk the stack. This is used on method exits to check if the caller needs a
  // deoptimization.
  bool ShouldDeoptimizeCaller(Thread* self, const NthCallerVisitor& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // This returns if the caller of runtime method requires a deoptimization. This checks both if the
  // method requires a deopt or if this particular frame needs a deopt because of a class
  // redefinition.
  bool ShouldDeoptimizeCaller(Thread* self, ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_);
  bool ShouldDeoptimizeCaller(Thread* self, ArtMethod** sp, size_t frame_size)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // This is a helper function used by the two variants of ShouldDeoptimizeCaller.
  // Remove this once ShouldDeoptimizeCaller is updated not to use NthCallerVisitor.
  bool ShouldDeoptimizeCaller(Thread* self,
                              ArtMethod* caller,
                              uintptr_t caller_pc,
                              uintptr_t caller_sp) REQUIRES_SHARED(Locks::mutator_lock_);
  // This returns if the specified method requires a deoptimization. This doesn't account if a stack
  // frame involving this method requires a deoptimization.
  bool NeedsSlowInterpreterForMethod(Thread* self, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Called when an instrumented method is entered. The intended link register (lr) is saved so
  // that returning causes a branch to the method exit stub. Generates method enter events.
  void PushInstrumentationStackFrame(Thread* self,
                                     ObjPtr<mirror::Object> this_object,
                                     ArtMethod* method,
                                     uintptr_t stack_pointer,
                                     uintptr_t lr,
                                     bool interpreter_entry)
      REQUIRES_SHARED(Locks::mutator_lock_);

  DeoptimizationMethodType GetDeoptimizationMethodType(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Called when an instrumented method is exited. Removes the pushed instrumentation frame
  // returning the intended link register. Generates method exit events. The gpr_result and
  // fpr_result pointers are pointers to the locations where the integer/pointer and floating point
  // result values of the function are stored. Both pointers must always be valid but the values
  // held there will only be meaningful if interpreted as the appropriate type given the function
  // being returned from.
  TwoWordReturn PopInstrumentationStackFrame(Thread* self,
                                             uintptr_t* return_pc_addr,
                                             uint64_t* gpr_result,
                                             uint64_t* fpr_result)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Pops instrumentation frames until the specified stack_pointer from the current thread. Returns
  // the return pc for the last instrumentation frame that's popped.
  uintptr_t PopInstrumentationStackUntil(Thread* self, uintptr_t stack_pointer) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Call back for configure stubs.
  void InstallStubsForClass(ObjPtr<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_);

  void InstallStubsForMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  void UpdateEntrypointsForDebuggable() REQUIRES(art::Locks::mutator_lock_);

  // Install instrumentation exit stub on every method of the stack of the given thread.
  // This is used by:
  //  - the debugger to cause a deoptimization of the all frames in thread's stack (for
  //    example, after updating local variables)
  //  - to call method entry / exit hooks for tracing. For this we instrument
  //    the stack frame to run entry / exit hooks but we don't need to deoptimize.
  // deopt_all_frames indicates whether the frames need to deoptimize or not.
  void InstrumentThreadStack(Thread* thread, bool deopt_all_frames) REQUIRES(Locks::mutator_lock_);

  // Force all currently running frames to be deoptimized back to interpreter. This should only be
  // used in cases where basically all compiled code has been invalidated.
  void DeoptimizeAllThreadFrames() REQUIRES(art::Locks::mutator_lock_);

  static size_t ComputeFrameId(Thread* self,
                               size_t frame_depth,
                               size_t inlined_frames_before_frame)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Does not hold lock, used to check if someone changed from not instrumented to instrumented
  // during a GC suspend point.
  bool AllocEntrypointsInstrumented() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return alloc_entrypoints_instrumented_;
  }

  bool ProcessMethodUnwindCallbacks(Thread* self,
                                    std::queue<ArtMethod*>& methods,
                                    MutableHandle<mirror::Throwable>& exception)
      REQUIRES_SHARED(Locks::mutator_lock_);

  InstrumentationLevel GetCurrentInstrumentationLevel() const;

  bool MethodSupportsExitEvents(ArtMethod* method, const OatQuickMethodHeader* header)
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  // Returns true if moving to the given instrumentation level requires the installation of stubs.
  // False otherwise.
  bool RequiresInstrumentationInstallation(InstrumentationLevel new_level) const;

  // Update the current instrumentation_level_.
  void UpdateInstrumentationLevel(InstrumentationLevel level);

  // Does the job of installing or removing instrumentation code within methods.
  // In order to support multiple clients using instrumentation at the same time,
  // the caller must pass a unique key (a string) identifying it so we remind which
  // instrumentation level it needs. Therefore the current instrumentation level
  // becomes the highest instrumentation level required by a client.
  void ConfigureStubs(const char* key, InstrumentationLevel desired_instrumentation_level)
      REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);
  void UpdateStubs() REQUIRES(Locks::mutator_lock_, Roles::uninterruptible_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::classlinker_classes_lock_);

  // If there are no pending deoptimizations restores the stack to the normal state by updating the
  // return pcs to actual return addresses from the instrumentation stack and clears the
  // instrumentation stack.
  void MaybeRestoreInstrumentationStack() REQUIRES(Locks::mutator_lock_);

  // No thread safety analysis to get around SetQuickAllocEntryPointsInstrumented requiring
  // exclusive access to mutator lock which you can't get if the runtime isn't started.
  void SetEntrypointsInstrumented(bool instrumented) NO_THREAD_SAFETY_ANALYSIS;

  void MethodEnterEventImpl(Thread* thread, ArtMethod* method) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  template <typename T>
  void MethodExitEventImpl(Thread* thread,
                           ArtMethod* method,
                           OptionalFrame frame,
                           T& return_value) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void DexPcMovedEventImpl(Thread* thread,
                           ObjPtr<mirror::Object> this_object,
                           ArtMethod* method,
                           uint32_t dex_pc) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void BranchImpl(Thread* thread, ArtMethod* method, uint32_t dex_pc, int32_t offset) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void WatchedFramePopImpl(Thread* thread, const ShadowFrame& frame) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void FieldReadEventImpl(Thread* thread,
                          ObjPtr<mirror::Object> this_object,
                          ArtMethod* method,
                          uint32_t dex_pc,
                          ArtField* field) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void FieldWriteEventImpl(Thread* thread,
                           ObjPtr<mirror::Object> this_object,
                           ArtMethod* method,
                           uint32_t dex_pc,
                           ArtField* field,
                           const JValue& field_value) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Read barrier-aware utility functions for accessing deoptimized_methods_
  bool AddDeoptimizedMethod(ArtMethod* method) REQUIRES(Locks::mutator_lock_);
  bool IsDeoptimizedMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  bool RemoveDeoptimizedMethod(ArtMethod* method) REQUIRES(Locks::mutator_lock_);
  void UpdateMethodsCodeImpl(ArtMethod* method, const void* new_code)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // A counter that's incremented every time a DeoptimizeAllFrames. We check each
  // InstrumentationStackFrames creation id against this number and if they differ we deopt even if
  // we could otherwise continue running.
  uint64_t current_force_deopt_id_ GUARDED_BY(Locks::mutator_lock_);

  // Have we hijacked ArtMethod::code_ so that it calls instrumentation/interpreter code?
  bool instrumentation_stubs_installed_;

  // The required level of instrumentation. This could be one of the following values:
  // kInstrumentNothing: no instrumentation support is needed
  // kInstrumentWithInstrumentationStubs: needs support to call method entry/exit stubs.
  // kInstrumentWithInterpreter: only execute with interpreter
  Instrumentation::InstrumentationLevel instrumentation_level_;

  // Did the runtime request we only run in the interpreter? ie -Xint mode.
  bool forced_interpret_only_;

  // Do we have any listeners for method entry events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_entry_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for method exit events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_exit_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for method unwind events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_method_unwind_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for dex move events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_dex_pc_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for field read events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_field_read_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any listeners for field write events? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_field_write_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any exception thrown listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_exception_thrown_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any frame pop listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_watched_frame_pop_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any branch listeners? Short-cut to avoid taking the instrumentation_lock_.
  bool have_branch_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Do we have any exception handled listeners? Short-cut to avoid taking the
  // instrumentation_lock_.
  bool have_exception_handled_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // Contains the instrumentation level required by each client of the instrumentation identified
  // by a string key.
  using InstrumentationLevelTable = SafeMap<const char*, InstrumentationLevel>;
  InstrumentationLevelTable requested_instrumentation_levels_ GUARDED_BY(Locks::mutator_lock_);

  // The event listeners, written to with the mutator_lock_ exclusively held.
  // Mutators must be able to iterate over these lists concurrently, that is, with listeners being
  // added or removed while iterating. The modifying thread holds exclusive lock,
  // so other threads cannot iterate (i.e. read the data of the list) at the same time but they
  // do keep iterators that need to remain valid. This is the reason these listeners are std::list
  // and not for example std::vector: the existing storage for a std::list does not move.
  // Note that mutators cannot make a copy of these lists before iterating, as the instrumentation
  // listeners can also be deleted concurrently.
  // As a result, these lists are never trimmed. That's acceptable given the low number of
  // listeners we have.
  std::list<InstrumentationListener*> method_entry_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> method_exit_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> method_unwind_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> branch_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> dex_pc_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> field_read_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> field_write_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> exception_thrown_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> watched_frame_pop_listeners_ GUARDED_BY(Locks::mutator_lock_);
  std::list<InstrumentationListener*> exception_handled_listeners_ GUARDED_BY(Locks::mutator_lock_);

  // The set of methods being deoptimized (by the debugger) which must be executed with interpreter
  // only.
  std::unordered_set<ArtMethod*> deoptimized_methods_ GUARDED_BY(Locks::mutator_lock_);

  // Current interpreter handler table. This is updated each time the thread state flags are
  // modified.

  // Greater than 0 if quick alloc entry points instrumented.
  size_t quick_alloc_entry_points_instrumentation_counter_;

  // alloc_entrypoints_instrumented_ is only updated with all the threads suspended, this is done
  // to prevent races with the GC where the GC relies on thread suspension only see
  // alloc_entrypoints_instrumented_ change during suspend points.
  bool alloc_entrypoints_instrumented_;

  friend class InstrumentationTest;  // For GetCurrentInstrumentationLevel and ConfigureStubs.
  friend class InstrumentationStackPopper;  // For popping instrumentation frames.
  friend void InstrumentationInstallStack(Thread*, void*, bool);

  DISALLOW_COPY_AND_ASSIGN(Instrumentation);
};
std::ostream& operator<<(std::ostream& os, Instrumentation::InstrumentationEvent rhs);
std::ostream& operator<<(std::ostream& os, Instrumentation::InstrumentationLevel rhs);

// An element in the instrumentation side stack maintained in art::Thread.
struct InstrumentationStackFrame {
  InstrumentationStackFrame(mirror::Object* this_object,
                            ArtMethod* method,
                            uintptr_t return_pc,
                            bool interpreter_entry,
                            uint64_t force_deopt_id)
      : this_object_(this_object),
        method_(method),
        return_pc_(return_pc),
        interpreter_entry_(interpreter_entry),
        force_deopt_id_(force_deopt_id) {
  }

  std::string Dump() const REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::Object* this_object_;
  ArtMethod* method_;
  uintptr_t return_pc_;
  bool interpreter_entry_;
  uint64_t force_deopt_id_;
};

}  // namespace instrumentation
}  // namespace art

#endif  // ART_RUNTIME_INSTRUMENTATION_H_
