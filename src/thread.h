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

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <pthread.h>

#include <bitset>
#include <deque>
#include <iosfwd>
#include <list>
#include <string>

#include "base/macros.h"
#include "closure.h"
#include "globals.h"
#include "instrumentation.h"
#include "jvalue.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "locks.h"
#include "offsets.h"
#include "runtime_stats.h"
#include "stack.h"
#include "stack_indirect_reference_table.h"
#include "UniquePtr.h"

namespace art {

class AbstractMethod;
class Array;
class BaseMutex;
class Class;
class ClassLinker;
class ClassLoader;
class Context;
struct DebugInvokeReq;
class DexFile;
struct JavaVMExt;
struct JNIEnvExt;
class Monitor;
class Object;
class Runtime;
class ScopedObjectAccess;
class ScopedObjectAccessUnchecked;
class ShadowFrame;
class StackIndirectReferenceTable;
class StackTraceElement;
class StaticStorageBase;
class Thread;
class ThreadList;
class Throwable;

template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
typedef PrimitiveArray<int32_t> IntArray;

// Thread priorities. These must match the Thread.MIN_PRIORITY,
// Thread.NORM_PRIORITY, and Thread.MAX_PRIORITY constants.
enum ThreadPriority {
  kMinThreadPriority = 1,
  kNormThreadPriority = 5,
  kMaxThreadPriority = 10,
};

enum ThreadState {
  //                                  Thread.State   JDWP state
  kTerminated,                     // TERMINATED     TS_ZOMBIE    Thread.run has returned, but Thread* still around
  kRunnable,                       // RUNNABLE       TS_RUNNING   runnable
  kTimedWaiting,                   // TIMED_WAITING  TS_WAIT      in Object.wait() with a timeout
  kSleeping,                       // TIMED_WAITING  TS_SLEEPING  in Thread.sleep()
  kBlocked,                        // BLOCKED        TS_MONITOR   blocked on a monitor
  kWaiting,                        // WAITING        TS_WAIT      in Object.wait()
  kWaitingForGcToComplete,         // WAITING        TS_WAIT      blocked waiting for GC
  kWaitingPerformingGc,            // WAITING        TS_WAIT      performing GC
  kWaitingForDebuggerSend,         // WAITING        TS_WAIT      blocked waiting for events to be sent
  kWaitingForDebuggerToAttach,     // WAITING        TS_WAIT      blocked waiting for debugger to attach
  kWaitingInMainDebuggerLoop,      // WAITING        TS_WAIT      blocking/reading/processing debugger events
  kWaitingForDebuggerSuspension,   // WAITING        TS_WAIT      waiting for debugger suspend all
  kWaitingForJniOnLoad,            // WAITING        TS_WAIT      waiting for execution of dlopen and JNI on load code
  kWaitingForSignalCatcherOutput,  // WAITING        TS_WAIT      waiting for signal catcher IO to complete
  kWaitingInMainSignalCatcherLoop, // WAITING        TS_WAIT      blocking/reading/processing signals
  kStarting,                       // NEW            TS_WAIT      native thread started, not yet ready to run managed code
  kNative,                         // RUNNABLE       TS_RUNNING   running in a JNI native method
  kSuspended,                      // RUNNABLE       TS_RUNNING   suspended by GC or debugger
};

enum ThreadFlag {
  kSuspendRequest   = 1,  // If set implies that suspend_count_ > 0 and the Thread should enter the
                          // safepoint handler.
  kCheckpointRequest = 2, // Request that the thread do some checkpoint work and then continue.
  kExceptionPending = 4,  // If set implies that exception_ != NULL.
  kEnterInterpreter = 8,  // Instruct managed code it should enter the interpreter.
};

class PACKED(4) Thread {
 public:
  // Space to throw a StackOverflowError in.
  static const size_t kStackOverflowReservedBytes = 10 * KB;

  // Creates a new native thread corresponding to the given managed peer.
  // Used to implement Thread.start.
  static void CreateNativeThread(JNIEnv* env, jobject peer, size_t stack_size, bool daemon);

  // Attaches the calling native thread to the runtime, returning the new native peer.
  // Used to implement JNI AttachCurrentThread and AttachCurrentThreadAsDaemon calls.
  static Thread* Attach(const char* thread_name, bool as_daemon, jobject thread_group,
                        bool create_peer);

  // Reset internal state of child thread after fork.
  void InitAfterFork();

  static Thread* Current() __attribute__ ((pure)) {
    // We rely on Thread::Current returning NULL for a detached thread, so it's not obvious
    // that we can replace this with a direct %fs access on x86.
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
  }

  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts, Object* thread_peer)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_list_lock_)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts, jobject thread)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_list_lock_)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Translates 172 to pAllocArrayFromCode and so on.
  static void DumpThreadOffset(std::ostream& os, uint32_t offset, size_t size_of_pointers);

  // Dumps a one-line summary of thread state (used for operator<<).
  void ShortDump(std::ostream& os) const;

  // Dumps the detailed thread state and the thread stack (used for SIGQUIT).
  void Dump(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Dumps the SIGQUIT per-thread header. 'thread' can be NULL for a non-attached thread, in which
  // case we use 'tid' to identify the thread, and we'll include as much information as we can.
  static void DumpState(std::ostream& os, const Thread* thread, pid_t tid)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ThreadState GetState() const {
    return static_cast<ThreadState>(state_and_flags_.as_struct.state);
  }

  ThreadState SetState(ThreadState new_state);

  int GetSuspendCount() const EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    return suspend_count_;
  }

  int GetDebugSuspendCount() const EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    return debug_suspend_count_;
  }

  bool IsSuspended() const {
    union StateAndFlags state_and_flags = state_and_flags_;
    return state_and_flags.as_struct.state != kRunnable &&
        (state_and_flags.as_struct.flags & kSuspendRequest) != 0;
  }

  void ModifySuspendCount(Thread* self, int delta, bool for_debugger)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_);

  bool RequestCheckpoint(Closure* function);

  // Called when thread detected that the thread_suspend_count_ was non-zero. Gives up share of
  // mutator_lock_ and waits until it is resumed and thread_suspend_count_ is zero.
  void FullSuspendCheck()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Transition from non-runnable to runnable state acquiring share on mutator_lock_.
  ThreadState TransitionFromSuspendedToRunnable()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_);

  // Transition from runnable into a state where mutator privileges are denied. Releases share of
  // mutator lock.
  void TransitionFromRunnableToSuspended(ThreadState new_state)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      UNLOCK_FUNCTION(Locks::mutator_lock_);

  // Wait for a debugger suspension on the thread associated with the given peer. Returns the
  // thread on success, else NULL. If the thread should be suspended then request_suspension should
  // be true on entry. If the suspension times out then *timeout is set to true.
  static Thread* SuspendForDebugger(jobject peer,  bool request_suspension, bool* timeout)
      LOCKS_EXCLUDED(Locks::mutator_lock_,
                     Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_);

  // Once called thread suspension will cause an assertion failure.
#ifndef NDEBUG
  const char* StartAssertNoThreadSuspension(const char* cause) {
    CHECK(cause != NULL);
    const char* previous_cause = last_no_thread_suspension_cause_;
    no_thread_suspension_++;
    last_no_thread_suspension_cause_ = cause;
    return previous_cause;
  }
#else
  const char* StartAssertNoThreadSuspension(const char* cause) {
    CHECK(cause != NULL);
    return NULL;
  }
#endif

  // End region where no thread suspension is expected.
#ifndef NDEBUG
  void EndAssertNoThreadSuspension(const char* old_cause) {
    CHECK(old_cause != NULL || no_thread_suspension_ == 1);
    CHECK_GT(no_thread_suspension_, 0U);
    no_thread_suspension_--;
    last_no_thread_suspension_cause_ = old_cause;
  }
#else
  void EndAssertNoThreadSuspension(const char*) {
  }
#endif


#ifndef NDEBUG
  void AssertThreadSuspensionIsAllowable(bool check_locks = true) const;
#else
  void AssertThreadSuspensionIsAllowable(bool check_locks = true) const {
    UNUSED(check_locks);  // Keep GCC happy about unused parameters.
  }
#endif

  bool IsDaemon() const {
    return daemon_;
  }

  bool HoldsLock(Object*);

  /*
   * Changes the priority of this thread to match that of the java.lang.Thread object.
   *
   * We map a priority value from 1-10 to Linux "nice" values, where lower
   * numbers indicate higher priority.
   */
  void SetNativePriority(int newPriority);

  /*
   * Returns the thread priority for the current thread by querying the system.
   * This is useful when attaching a thread through JNI.
   *
   * Returns a value from 1 to 10 (compatible with java.lang.Thread values).
   */
  static int GetNativePriority();

  uint32_t GetThinLockId() const {
    return thin_lock_id_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  // Returns the java.lang.Thread's name, or NULL if this Thread* doesn't have a peer.
  String* GetThreadName(const ScopedObjectAccessUnchecked& ts) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sets 'name' to the java.lang.Thread's name. This requires no transition to managed code,
  // allocation, or locking.
  void GetThreadName(std::string& name) const;

  // Sets the thread's name.
  void SetThreadName(const char* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Object* GetPeer() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(jpeer_ == NULL);
    return opeer_;
  }

  bool HasPeer() const {
    return jpeer_ != NULL || opeer_ != NULL;
  }

  RuntimeStats* GetStats() {
    return &stats_;
  }

  bool IsStillStarting() const;

  bool IsExceptionPending() const {
    bool result = ReadFlag(kExceptionPending);
    DCHECK_EQ(result, exception_ != NULL);
    return result;
  }

  Throwable* GetException() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return exception_;
  }

  void AssertNoPendingException() const;

  void SetException(Throwable* new_exception) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(new_exception != NULL);
    // TODO: DCHECK(!IsExceptionPending());
    exception_ = new_exception;
    AtomicSetFlag(kExceptionPending);
    DCHECK(IsExceptionPending());
  }

  void ClearException() {
    exception_ = NULL;
    AtomicClearFlag(kExceptionPending);
    DCHECK(!IsExceptionPending());
  }

  void DeliverException(Throwable* exception) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (exception == NULL) {
      ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
    } else {
      SetException(exception);
    }
  }

  // Find catch block and perform long jump to appropriate exception handle
  void QuickDeliverException() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Context* GetLongJumpContext();
  void ReleaseLongJumpContext(Context* context) {
    DCHECK(long_jump_context_ == NULL);
    long_jump_context_ = context;
  }

  AbstractMethod* GetCurrentMethod(uint32_t* dex_pc = NULL, size_t* frame_id = NULL) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetTopOfStack(void* stack, uintptr_t pc) {
    AbstractMethod** top_method = reinterpret_cast<AbstractMethod**>(stack);
    managed_stack_.SetTopQuickFrame(top_method);
    managed_stack_.SetTopQuickFramePc(pc);
  }

  bool HasManagedStack() const {
    return managed_stack_.GetTopQuickFrame() != NULL || managed_stack_.GetTopShadowFrame() != NULL;
  }

  // If 'msg' is NULL, no detail message is set.
  void ThrowNewException(const char* exception_class_descriptor, const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // If 'msg' is NULL, no detail message is set. An exception must be pending, and will be
  // used as the new exception's cause.
  void ThrowNewWrappedException(const char* exception_class_descriptor, const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)))
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // OutOfMemoryError is special, because we need to pre-allocate an instance.
  // Only the GC should call this.
  void ThrowOutOfMemoryError(const char* msg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  //QuickFrameIterator FindExceptionHandler(void* throw_pc, void** handler_pc);

  void* FindExceptionHandlerInMethod(const AbstractMethod* method,
                                     void* throw_pc,
                                     const DexFile& dex_file,
                                     ClassLinker* class_linker);

  static void Startup();
  static void FinishStartup();
  static void Shutdown();

  // JNI methods
  JNIEnvExt* GetJniEnv() const {
    return jni_env_;
  }

  // Convert a jobject into a Object*
  Object* DecodeJObject(jobject obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Implements java.lang.Thread.interrupted.
  bool Interrupted();
  // Implements java.lang.Thread.isInterrupted.
  bool IsInterrupted();
  void Interrupt();
  void Notify();

  ClassLoader* GetClassLoaderOverride() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return class_loader_override_;
  }

  void SetClassLoaderOverride(ClassLoader* class_loader_override) {
    class_loader_override_ = class_loader_override;
  }

  // Create the internal representation of a stack trace, that is more time
  // and space efficient to compute than the StackTraceElement[]
  jobject CreateInternalStackTrace(const ScopedObjectAccessUnchecked& soa) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Convert an internal stack trace representation (returned by CreateInternalStackTrace) to a
  // StackTraceElement[]. If output_array is NULL, a new array is created, otherwise as many
  // frames as will fit are written into the given array. If stack_depth is non-NULL, it's updated
  // with the number of valid frames in the returned array.
  static jobjectArray InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
      jobjectArray output_array = NULL, int* stack_depth = NULL);

  void VisitRoots(Heap::RootVisitor* visitor, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void VerifyRoots(Heap::VerifyRootVisitor* visitor, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

#if VERIFY_OBJECT_ENABLED
  void VerifyStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
#else
  void VerifyStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){}
#endif

  //
  // Offsets of various members of native Thread class, used by compiled code.
  //

  static ThreadOffset SelfOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, self_));
  }

  static ThreadOffset ExceptionOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_));
  }

  static ThreadOffset PeerOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, opeer_));
  }

  static ThreadOffset ThinLockIdOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, thin_lock_id_));
  }

  static ThreadOffset CardTableOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, card_table_));
  }

  static ThreadOffset ThreadFlagsOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, state_and_flags_));
  }

  // Size of stack less any space reserved for stack overflow
  size_t GetStackSize() const {
    return stack_size_ - (stack_end_ - stack_begin_);
  }

  byte* GetStackEnd() const {
    return stack_end_;
  }

  // Set the stack end to that to be used during a stack overflow
  void SetStackEndForStackOverflow() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Set the stack end to that to be used during regular execution
  void ResetDefaultStackEnd() {
    // Our stacks grow down, so we want stack_end_ to be near there, but reserving enough room
    // to throw a StackOverflowError.
    stack_end_ = stack_begin_ + kStackOverflowReservedBytes;
  }

  bool IsHandlingStackOverflow() const {
    return stack_end_ == stack_begin_;
  }

  static ThreadOffset StackEndOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, stack_end_));
  }

  static ThreadOffset JniEnvOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, jni_env_));
  }

  static ThreadOffset TopOfManagedStackOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopQuickFrameOffset());
  }

  static ThreadOffset TopOfManagedStackPcOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopQuickFramePcOffset());
  }

  const ManagedStack* GetManagedStack() const {
    return &managed_stack_;
  }

  // Linked list recording fragments of managed stack.
  void PushManagedStackFragment(ManagedStack* fragment) {
    managed_stack_.PushManagedStackFragment(fragment);
  }
  void PopManagedStackFragment(const ManagedStack& fragment) {
    managed_stack_.PopManagedStackFragment(fragment);
  }

  ShadowFrame* PushShadowFrame(ShadowFrame* new_top_frame) {
    return managed_stack_.PushShadowFrame(new_top_frame);
  }

  ShadowFrame* PopShadowFrame() {
    return managed_stack_.PopShadowFrame();
  }

  static ThreadOffset TopShadowFrameOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopShadowFrameOffset());
  }

  // Number of references allocated in JNI ShadowFrames on this thread
  size_t NumJniShadowFrameReferences() const {
    return managed_stack_.NumJniShadowFrameReferences();
  }

  // Number of references in SIRTs on this thread
  size_t NumSirtReferences();

  // Number of references allocated in SIRTs & JNI shadow frames on this thread
  size_t NumStackReferences() {
    return NumSirtReferences() + NumJniShadowFrameReferences();
  };

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj) const;

  void SirtVisitRoots(Heap::RootVisitor* visitor, void* arg);

  void PushSirt(StackIndirectReferenceTable* sirt) {
    sirt->SetLink(top_sirt_);
    top_sirt_ = sirt;
  }

  StackIndirectReferenceTable* PopSirt() {
    StackIndirectReferenceTable* sirt = top_sirt_;
    DCHECK(sirt != NULL);
    top_sirt_ = top_sirt_->GetLink();
    return sirt;
  }

  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  DebugInvokeReq* GetInvokeReq() {
    return debug_invoke_req_;
  }

  void SetDebuggerUpdatesEnabled(bool enabled);

  void SetDeoptimizationShadowFrame(ShadowFrame* sf, const JValue& ret_val);

  ShadowFrame* GetAndClearDeoptimizationShadowFrame(JValue* ret_val);

  const std::deque<InstrumentationStackFrame>* GetInstrumentationStack() const {
    return instrumentation_stack_;
  }

  bool IsInstrumentationStackEmpty() const {
    return instrumentation_stack_->empty();
  }

  void PushInstrumentationStackFrame(const InstrumentationStackFrame& frame) {
    instrumentation_stack_->push_front(frame);
  }

  void PushBackInstrumentationStackFrame(const InstrumentationStackFrame& frame) {
    instrumentation_stack_->push_back(frame);
  }

  InstrumentationStackFrame PopInstrumentationStackFrame() {
    InstrumentationStackFrame frame = instrumentation_stack_->front();
    instrumentation_stack_->pop_front();
    return frame;
  }

  BaseMutex* GetHeldMutex(LockLevel level) const {
    return held_mutexes_[level];
  }

  void SetHeldMutex(LockLevel level, BaseMutex* mutex) {
    held_mutexes_[level] = mutex;
  }

  void RunCheckpointFunction() {
    CHECK(checkpoint_function_ != NULL);
    checkpoint_function_->Run(this);
  }

  bool ReadFlag(ThreadFlag flag) const {
    return (state_and_flags_.as_struct.flags & flag) != 0;
  }

  void AtomicSetFlag(ThreadFlag flag);

  void AtomicClearFlag(ThreadFlag flag);

 private:
  // We have no control over the size of 'bool', but want our boolean fields
  // to be 4-byte quantities.
  typedef uint32_t bool32_t;

  explicit Thread(bool daemon);
  ~Thread() LOCKS_EXCLUDED(Locks::mutator_lock_,
                           Locks::thread_suspend_count_lock_);
  void Destroy();
  friend class ThreadList;  // For ~Thread and Destroy.

  void CreatePeer(const char* name, bool as_daemon, jobject thread_group);
  friend class Runtime; // For CreatePeer.

  // Avoid use, callers should use SetState. Used only by SignalCatcher::HandleSigQuit and ~Thread.
  ThreadState SetStateUnsafe(ThreadState new_state) {
    ThreadState old_state = GetState();
    state_and_flags_.as_struct.state = new_state;
    return old_state;
  }
  friend class SignalCatcher;  // For SetStateUnsafe.

  void DumpState(std::ostream& os) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DumpStack(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Out-of-line conveniences for debugging in gdb.
  static Thread* CurrentFromGdb(); // Like Thread::Current.
  // Like Thread::Dump(std::cerr).
  void DumpFromGdb() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void* CreateCallback(void* arg);

  void HandleUncaughtExceptions(ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RemoveFromThreadGroup(ScopedObjectAccess& soa) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Init(ThreadList*, JavaVMExt*) EXCLUSIVE_LOCKS_REQUIRED(Locks::runtime_shutdown_lock_);
  void InitCardTable();
  void InitCpu();
  void InitFunctionPointers();
  void InitTid();
  void InitPthreadKeySelf();
  void InitStackHwm();

  void NotifyLocked(Thread* self) EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_);

  static void ThreadExitCallback(void* arg);

  // TLS key used to retrieve the Thread*.
  static pthread_key_t pthread_key_self_;

  // Used to notify threads that they should attempt to resume, they will suspend again if
  // their suspend count is > 0.
  static ConditionVariable* resume_cond_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // --- Frequently accessed fields first for short offsets ---

  // 32 bits of atomically changed state and flags. Keeping as 32 bits allows and atomic CAS to
  // change from being Suspended to Runnable without a suspend request occurring.
  union StateAndFlags {
    struct PACKED(4) {
      // Bitfield of flag values. Must be changed atomically so that flag values aren't lost. See
      // ThreadFlags for bit field meanings.
      volatile uint16_t flags;
      // Holds the ThreadState. May be changed non-atomically between Suspended (ie not Runnable)
      // transitions. Changing to Runnable requires that the suspend_request be part of the atomic
      // operation. If a thread is suspended and a suspend_request is present, a thread may not
      // change to Runnable as a GC or other operation is in progress.
      volatile uint16_t state;
    } as_struct;
    volatile int32_t as_int;
  };
  union StateAndFlags state_and_flags_;
  COMPILE_ASSERT(sizeof(union StateAndFlags) == sizeof(int32_t),
                 sizeof_state_and_flags_and_int32_are_different);

  // A non-zero value is used to tell the current thread to enter a safe point
  // at the next poll.
  int suspend_count_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // The biased card table, see CardTable for details
  byte* card_table_;

  // The pending exception or NULL.
  Throwable* exception_;

  // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
  // We leave extra space so there's room for the code that throws StackOverflowError.
  byte* stack_end_;

  // The top of the managed stack often manipulated directly by compiler generated code.
  ManagedStack managed_stack_;

  // Every thread may have an associated JNI environment
  JNIEnvExt* jni_env_;

  // Initialized to "this". On certain architectures (such as x86) reading
  // off of Thread::Current is easy but getting the address of Thread::Current
  // is hard. This field can be read off of Thread::Current to give the address.
  Thread* self_;

  // Our managed peer (an instance of java.lang.Thread). The jobject version is used during thread
  // start up, until the thread is registered and the local opeer_ is used.
  Object* opeer_;
  jobject jpeer_;

  // The "lowest addressable byte" of the stack
  byte* stack_begin_;

  // Size of the stack
  size_t stack_size_;

  // Thin lock thread id. This is a small integer used by the thin lock implementation.
  // This is not to be confused with the native thread's tid, nor is it the value returned
  // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
  // important difference between this id and the ids visible to managed code is that these
  // ones get reused (to ensure that they fit in the number of bits available).
  uint32_t thin_lock_id_;

  // System thread id.
  pid_t tid_;

  // Guards the 'interrupted_' and 'wait_monitor_' members.
  mutable Mutex* wait_mutex_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ConditionVariable* wait_cond_ GUARDED_BY(wait_mutex_);
  // Pointer to the monitor lock we're currently waiting on (or NULL).
  Monitor* wait_monitor_ GUARDED_BY(wait_mutex_);
  // Thread "interrupted" status; stays raised until queried or thrown.
  bool32_t interrupted_ GUARDED_BY(wait_mutex_);
  // The next thread in the wait set this thread is part of.
  Thread* wait_next_;
  // If we're blocked in MonitorEnter, this is the object we're trying to lock.
  Object* monitor_enter_object_;

  friend class Monitor;

  // Top of linked list of stack indirect reference tables or NULL for none
  StackIndirectReferenceTable* top_sirt_;

  Runtime* runtime_;

  RuntimeStats stats_;

  // Needed to get the right ClassLoader in JNI_OnLoad, but also
  // useful for testing.
  ClassLoader* class_loader_override_;

  // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
  Context* long_jump_context_;

  // A boolean telling us whether we're recursively throwing OOME.
  bool32_t throwing_OutOfMemoryError_;

  // How much of 'suspend_count_' is by request of the debugger, used to set things right
  // when the debugger detaches. Must be <= suspend_count_.
  int debug_suspend_count_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // JDWP invoke-during-breakpoint support.
  DebugInvokeReq* debug_invoke_req_;

  // Shadow frame that is used temporarily during the deoptimization of a method.
  ShadowFrame* deoptimization_shadow_frame_;
  JValue deoptimization_return_value_;

  // Additional stack used by method instrumentation to store method and return pc values.
  // Stored as a pointer since std::deque is not PACKED.
  std::deque<InstrumentationStackFrame>* instrumentation_stack_;

  // A cached copy of the java.lang.Thread's name.
  std::string* name_;

  // Is the thread a daemon?
  const bool32_t daemon_;

  // A cached pthread_t for the pthread underlying this Thread*.
  pthread_t pthread_self_;

  // Support for Mutex lock hierarchy bug detection.
  BaseMutex* held_mutexes_[kMaxMutexLevel + 1];

  // A positive value implies we're in a region where thread suspension isn't expected.
  uint32_t no_thread_suspension_;

  // Cause for last suspension.
  const char* last_no_thread_suspension_cause_;

  // Pending checkpoint functions.
  Closure* checkpoint_function_;

 public:
  // Runtime support function pointers
  // TODO: move this near the top, since changing its offset requires all oats to be recompiled!
  EntryPoints entrypoints_;

 private:
  // How many times has our pthread key's destructor been called?
  uint32_t thread_exit_check_count_;

  friend class ScopedThreadStateChange;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const ThreadState& state);

}  // namespace art

#endif  // ART_SRC_THREAD_H_
