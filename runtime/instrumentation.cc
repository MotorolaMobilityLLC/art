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

#include "instrumentation.h"

#include <functional>
#include <optional>
#include <sstream>

#include <android-base/logging.h>

#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/atomic.h"
#include "base/callee_save_type.h"
#include "class_linker.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_types.h"
#include "dex/dex_instruction-inl.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc_root-inl.h"
#include "interpreter/interpreter.h"
#include "interpreter/interpreter_common.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jvalue-inl.h"
#include "jvalue.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nterp_helpers.h"
#include "nth_caller_visitor.h"
#include "oat_file_manager.h"
#include "oat_quick_method_header.h"
#include "runtime-inl.h"
#include "thread.h"
#include "thread_list.h"

namespace art {
extern "C" NO_RETURN void artDeoptimize(Thread* self);
extern "C" NO_RETURN void artDeliverPendingExceptionFromCode(Thread* self);

namespace instrumentation {

constexpr bool kVerboseInstrumentation = false;

void InstrumentationListener::MethodExited(
    Thread* thread,
    ArtMethod* method,
    OptionalFrame frame,
    MutableHandle<mirror::Object>& return_value) {
  DCHECK_EQ(method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetReturnTypePrimitive(),
            Primitive::kPrimNot);
  const void* original_ret = return_value.Get();
  JValue v;
  v.SetL(return_value.Get());
  MethodExited(thread, method, frame, v);
  DCHECK(original_ret == v.GetL()) << "Return value changed";
}

void InstrumentationListener::FieldWritten(Thread* thread,
                                           Handle<mirror::Object> this_object,
                                           ArtMethod* method,
                                           uint32_t dex_pc,
                                           ArtField* field,
                                           Handle<mirror::Object> field_value) {
  DCHECK(!field->IsPrimitiveType());
  JValue v;
  v.SetL(field_value.Get());
  FieldWritten(thread, this_object, method, dex_pc, field, v);
}

// Instrumentation works on non-inlined frames by updating returned PCs
// of compiled frames.
static constexpr StackVisitor::StackWalkKind kInstrumentationStackWalk =
    StackVisitor::StackWalkKind::kSkipInlinedFrames;

class InstallStubsClassVisitor : public ClassVisitor {
 public:
  explicit InstallStubsClassVisitor(Instrumentation* instrumentation)
      : instrumentation_(instrumentation) {}

  bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES(Locks::mutator_lock_) {
    instrumentation_->InstallStubsForClass(klass.Ptr());
    return true;  // we visit all classes.
  }

 private:
  Instrumentation* const instrumentation_;
};

Instrumentation::Instrumentation()
    : current_force_deopt_id_(0),
      instrumentation_stubs_installed_(false),
      instrumentation_level_(InstrumentationLevel::kInstrumentNothing),
      forced_interpret_only_(false),
      have_method_entry_listeners_(false),
      have_method_exit_listeners_(false),
      have_method_unwind_listeners_(false),
      have_dex_pc_listeners_(false),
      have_field_read_listeners_(false),
      have_field_write_listeners_(false),
      have_exception_thrown_listeners_(false),
      have_watched_frame_pop_listeners_(false),
      have_branch_listeners_(false),
      have_exception_handled_listeners_(false),
      quick_alloc_entry_points_instrumentation_counter_(0),
      alloc_entrypoints_instrumented_(false) {
}

bool Instrumentation::ProcessMethodUnwindCallbacks(Thread* self,
                                                   std::queue<ArtMethod*>& methods,
                                                   MutableHandle<mirror::Throwable>& exception) {
  DCHECK(!self->IsExceptionPending());
  if (!HasMethodUnwindListeners()) {
    return true;
  }
  if (kVerboseInstrumentation) {
    LOG(INFO) << "Popping frames for exception " << exception->Dump();
  }
  // The instrumentation events expect the exception to be set.
  self->SetException(exception.Get());
  bool new_exception_thrown = false;

  // Process callbacks for all methods that would be unwound until a new exception is thrown.
  while (!methods.empty()) {
    ArtMethod* method = methods.front();
    methods.pop();
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Popping for unwind " << method->PrettyMethod();
    }

    if (method->IsRuntimeMethod()) {
      continue;
    }

    // Notify listeners of method unwind.
    // TODO: improve the dex_pc information here.
    uint32_t dex_pc = dex::kDexNoIndex;
    MethodUnwindEvent(self, method, dex_pc);
    new_exception_thrown = self->GetException() != exception.Get();
    if (new_exception_thrown) {
      break;
    }
  }

  exception.Assign(self->GetException());
  self->ClearException();
  if (kVerboseInstrumentation && new_exception_thrown) {
    LOG(INFO) << "Did partial pop of frames due to new exception";
  }
  return !new_exception_thrown;
}

void Instrumentation::InstallStubsForClass(ObjPtr<mirror::Class> klass) {
  if (!klass->IsResolved()) {
    // We need the class to be resolved to install/uninstall stubs. Otherwise its methods
    // could not be initialized or linked with regards to class inheritance.
  } else if (klass->IsErroneousResolved()) {
    // We can't execute code in a erroneous class: do nothing.
  } else {
    for (ArtMethod& method : klass->GetMethods(kRuntimePointerSize)) {
      InstallStubsForMethod(&method);
    }
  }
}

static bool CanHandleInitializationCheck(const void* code) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->IsQuickResolutionStub(code) ||
         class_linker->IsQuickToInterpreterBridge(code) ||
         class_linker->IsQuickGenericJniStub(code) ||
         (code == interpreter::GetNterpWithClinitEntryPoint()) ||
         (code == GetQuickInstrumentationEntryPoint());
}

static bool IsProxyInit(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
  // Annoyingly this can be called before we have actually initialized WellKnownClasses so therefore
  // we also need to check this based on the declaring-class descriptor. The check is valid because
  // Proxy only has a single constructor.
  ArtMethod* well_known_proxy_init = jni::DecodeArtMethod(
      WellKnownClasses::java_lang_reflect_Proxy_init);
  if (well_known_proxy_init == method) {
    return true;
  }

  if (well_known_proxy_init != nullptr) {
    return false;
  }

  return method->IsConstructor() && !method->IsStatic() &&
      method->GetDeclaringClass()->DescriptorEquals("Ljava/lang/reflect/Proxy;");
}

// Returns true if we need entry exit stub to call entry hooks. JITed code
// directly call entry / exit hooks and don't need the stub.
static bool CodeNeedsEntryExitStub(const void* entry_point, ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Proxy.init should never have entry/exit stubs.
  if (IsProxyInit(method)) {
    return false;
  }

  // In some tests runtime isn't setup fully and hence the entry points could
  // be nullptr.
  if (entry_point == nullptr) {
    return true;
  }

  // Code running in the interpreter doesn't need entry/exit stubs.
  if (Runtime::Current()->GetClassLinker()->IsQuickToInterpreterBridge(entry_point)) {
    return false;
  }

  // When jiting code for debuggable runtimes / instrumentation is active  we generate the code to
  // call method entry / exit hooks when required. Hence it is not required to update to
  // instrumentation entry point for JITed code in debuggable mode.
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr && jit->GetCodeCache()->ContainsPc(entry_point)) {
    // If JITed code was compiled with instrumentation support we don't need entry / exit stub.
    OatQuickMethodHeader* header = OatQuickMethodHeader::FromEntryPoint(entry_point);
    return !CodeInfo::IsDebuggable(header->GetOptimizedCodeInfoPtr());
  }

  // GenericJni trampoline can handle entry / exit hooks in debuggable runtimes.
  if (Runtime::Current()->GetClassLinker()->IsQuickGenericJniStub(entry_point) &&
      Runtime::Current()->IsJavaDebuggable()) {
    return false;
  }

  return true;
}

static void UpdateEntryPoints(ArtMethod* method, const void* quick_code)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (kIsDebugBuild) {
    if (NeedsClinitCheckBeforeCall(method) &&
        !method->GetDeclaringClass()->IsVisiblyInitialized()) {
      CHECK(CanHandleInitializationCheck(quick_code));
    }
    jit::Jit* jit = Runtime::Current()->GetJit();
    if (jit != nullptr && jit->GetCodeCache()->ContainsPc(quick_code)) {
      // Ensure we always have the thumb entrypoint for JIT on arm32.
      if (kRuntimeISA == InstructionSet::kArm) {
        CHECK_EQ(reinterpret_cast<uintptr_t>(quick_code) & 1, 1u);
      }
    }
    if (IsProxyInit(method)) {
      CHECK_NE(quick_code, GetQuickInstrumentationEntryPoint());
    }
    const Instrumentation* instr = Runtime::Current()->GetInstrumentation();
    if (instr->EntryExitStubsInstalled()) {
      DCHECK(quick_code == GetQuickInstrumentationEntryPoint() ||
             !CodeNeedsEntryExitStub(quick_code, method));
    }
  }
  // If the method is from a boot image, don't dirty it if the entrypoint
  // doesn't change.
  if (method->GetEntryPointFromQuickCompiledCode() != quick_code) {
    method->SetEntryPointFromQuickCompiledCode(quick_code);
  }
}

bool Instrumentation::NeedsDexPcEvents(ArtMethod* method, Thread* thread) {
  return (InterpretOnly(method) || thread->IsForceInterpreter()) && HasDexPcListeners();
}

bool Instrumentation::InterpretOnly(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (method->IsNative()) {
    return false;
  }
  return InterpretOnly() || IsDeoptimized(method);
}

static bool CanUseAotCode(const void* quick_code)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (quick_code == nullptr) {
    return false;
  }
  Runtime* runtime = Runtime::Current();
  // For simplicity, we never use AOT code for debuggable.
  if (runtime->IsJavaDebuggable()) {
    return false;
  }

  if (runtime->IsNativeDebuggable()) {
    DCHECK(runtime->UseJitCompilation() && runtime->GetJit()->JitAtFirstUse());
    // If we are doing native debugging, ignore application's AOT code,
    // since we want to JIT it (at first use) with extra stackmaps for native
    // debugging. We keep however all AOT code from the boot image,
    // since the JIT-at-first-use is blocking and would result in non-negligible
    // startup performance impact.
    return runtime->GetHeap()->IsInBootImageOatFile(quick_code);
  }

  return true;
}

static bool CanUseNterp(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
  return interpreter::CanRuntimeUseNterp() &&
      CanMethodUseNterp(method) &&
      method->GetDeclaringClass()->IsVerified();
}

static const void* GetOptimizedCodeFor(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(!Runtime::Current()->GetInstrumentation()->InterpretOnly(method));
  CHECK(method->IsInvokable()) << method->PrettyMethod();
  if (method->IsProxyMethod()) {
    return GetQuickProxyInvokeHandler();
  }

  // In debuggable mode, we can only use AOT code for native methods.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const void* aot_code = method->GetOatMethodQuickCode(class_linker->GetImagePointerSize());
  if (CanUseAotCode(aot_code)) {
    return aot_code;
  }

  // If the method has been precompiled, there can be a JIT version.
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    const void* code = jit->GetCodeCache()->GetSavedEntryPointOfPreCompiledMethod(method);
    if (code != nullptr) {
      return code;
    }
  }

  // We need to check if the class has been verified for setting up nterp, as
  // the verifier could punt the method to the switch interpreter in case we
  // need to do lock counting.
  if (CanUseNterp(method)) {
    return interpreter::GetNterpEntryPoint();
  }

  return method->IsNative() ? GetQuickGenericJniStub() : GetQuickToInterpreterBridge();
}

void Instrumentation::InitializeMethodsCode(ArtMethod* method, const void* aot_code)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!method->IsInvokable()) {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr ||
           Runtime::Current()->GetClassLinker()->IsQuickToInterpreterBridge(
               method->GetEntryPointFromQuickCompiledCode()));
    UpdateEntryPoints(method, GetQuickToInterpreterBridge());
    return;
  }

  // Use instrumentation entrypoints if instrumentation is installed.
  if (UNLIKELY(EntryExitStubsInstalled()) && !IsProxyInit(method)) {
    if (!method->IsNative() && InterpretOnly(method)) {
      UpdateEntryPoints(method, GetQuickToInterpreterBridge());
    } else {
      UpdateEntryPoints(method, GetQuickInstrumentationEntryPoint());
    }
    return;
  }

  if (UNLIKELY(IsForcedInterpretOnly() || IsDeoptimized(method))) {
    UpdateEntryPoints(
        method, method->IsNative() ? GetQuickGenericJniStub() : GetQuickToInterpreterBridge());
    return;
  }

  // Special case if we need an initialization check.
  if (NeedsClinitCheckBeforeCall(method) && !method->GetDeclaringClass()->IsVisiblyInitialized()) {
    // If we have code but the method needs a class initialization check before calling
    // that code, install the resolution stub that will perform the check.
    // It will be replaced by the proper entry point by ClassLinker::FixupStaticTrampolines
    // after initializing class (see ClassLinker::InitializeClass method).
    // Note: this mimics the logic in image_writer.cc that installs the resolution
    // stub only if we have compiled code or we can execute nterp, and the method needs a class
    // initialization check.
    if (aot_code != nullptr || method->IsNative() || CanUseNterp(method)) {
      if (kIsDebugBuild && CanUseNterp(method)) {
        // Adds some test coverage for the nterp clinit entrypoint.
        UpdateEntryPoints(method, interpreter::GetNterpWithClinitEntryPoint());
      } else {
        UpdateEntryPoints(method, GetQuickResolutionStub());
      }
    } else {
      UpdateEntryPoints(method, GetQuickToInterpreterBridge());
    }
    return;
  }

  // Use the provided AOT code if possible.
  if (CanUseAotCode(aot_code)) {
    UpdateEntryPoints(method, aot_code);
    return;
  }

  // We check if the class is verified as we need the slow interpreter for lock verification.
  // If the class is not verified, This will be updated in
  // ClassLinker::UpdateClassAfterVerification.
  if (CanUseNterp(method)) {
    UpdateEntryPoints(method, interpreter::GetNterpEntryPoint());
    return;
  }

  // Use default entrypoints.
  UpdateEntryPoints(
      method, method->IsNative() ? GetQuickGenericJniStub() : GetQuickToInterpreterBridge());
}

void Instrumentation::InstallStubsForMethod(ArtMethod* method) {
  if (!method->IsInvokable() || method->IsProxyMethod()) {
    // Do not change stubs for these methods.
    return;
  }
  // Don't stub Proxy.<init>. Note that the Proxy class itself is not a proxy class.
  // TODO We should remove the need for this since it means we cannot always correctly detect calls
  // to Proxy.<init>
  if (IsProxyInit(method)) {
    return;
  }

  // If the instrumentation needs to go through the interpreter, just update the
  // entrypoint to interpreter.
  if (InterpretOnly(method)) {
    UpdateEntryPoints(method, GetQuickToInterpreterBridge());
    return;
  }

  if (EntryExitStubsInstalled()) {
    // Install the instrumentation entry point if needed.
    if (CodeNeedsEntryExitStub(method->GetEntryPointFromQuickCompiledCode(), method)) {
      UpdateEntryPoints(method, GetQuickInstrumentationEntryPoint());
    }
    return;
  }

  // We're being asked to restore the entrypoints after instrumentation.
  CHECK_EQ(instrumentation_level_, InstrumentationLevel::kInstrumentNothing);
  // We need to have the resolution stub still if the class is not initialized.
  if (NeedsClinitCheckBeforeCall(method) && !method->GetDeclaringClass()->IsVisiblyInitialized()) {
    UpdateEntryPoints(method, GetQuickResolutionStub());
    return;
  }
  UpdateEntryPoints(method, GetOptimizedCodeFor(method));
}

void Instrumentation::UpdateEntrypointsForDebuggable() {
  Runtime* runtime = Runtime::Current();
  // If we are transitioning from non-debuggable to debuggable, we patch
  // entry points of methods to remove any aot / JITed entry points.
  InstallStubsClassVisitor visitor(this);
  runtime->GetClassLinker()->VisitClasses(&visitor);
}

// Places the instrumentation exit pc as the return PC for every quick frame. This also allows
// deoptimization of quick frames to interpreter frames. When force_deopt is
// true the frames have to be deoptimized. If the frame has a deoptimization
// stack slot (all Jited frames), it is set to true to indicate this. For frames
// that do not have this slot, the force_deopt_id on the InstrumentationStack is
// used to check if the frame needs to be deoptimized. When force_deopt is false
// we just instrument the stack for method entry / exit hooks.
// Since we may already have done this previously, we need to push new instrumentation frame before
// existing instrumentation frames.
void InstrumentationInstallStack(Thread* thread, void* arg, bool deopt_all_frames)
    REQUIRES(Locks::mutator_lock_) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  struct InstallStackVisitor final : public StackVisitor {
    InstallStackVisitor(Thread* thread_in,
                        Context* context,
                        uintptr_t instrumentation_exit_pc,
                        uint64_t force_deopt_id,
                        bool deopt_all_frames)
        : StackVisitor(thread_in, context, kInstrumentationStackWalk),
          instrumentation_stack_(thread_in->GetInstrumentationStack()),
          instrumentation_exit_pc_(instrumentation_exit_pc),
          reached_existing_instrumentation_frames_(false),
          force_deopt_id_(force_deopt_id),
          deopt_all_frames_(deopt_all_frames),
          runtime_methods_need_deopt_check_(false) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      ArtMethod* m = GetMethod();
      if (m == nullptr || m->IsRuntimeMethod()) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Skipping upcall / runtime method. Frame " << GetFrameId();
        }
        return true;  // Ignore upcalls and runtime methods.
      }
      if (GetCurrentQuickFrame() == nullptr) {
        // Since we are updating the instrumentation related information we have to recalculate
        // NeedsDexPcEvents. For example, when a new method or thread is deoptimized / interpreter
        // stubs are installed the NeedsDexPcEvents could change for the shadow frames on the stack.
        // If we don't update it here we would miss reporting dex pc events which is incorrect.
        ShadowFrame* shadow_frame = GetCurrentShadowFrame();
        DCHECK(shadow_frame != nullptr);
        shadow_frame->SetNotifyDexPcMoveEvents(
            Runtime::Current()->GetInstrumentation()->NeedsDexPcEvents(GetMethod(), GetThread()));
        if (kVerboseInstrumentation) {
          LOG(INFO) << "Pushing shadow frame method " << m->PrettyMethod();
        }
        stack_methods_.push_back(m);
        return true;  // Continue.
      }
      uintptr_t return_pc = GetReturnPc();
      if (kVerboseInstrumentation) {
        LOG(INFO) << "  Installing exit stub in " << DescribeLocation();
      }
      if (return_pc == instrumentation_exit_pc_) {
        auto it = instrumentation_stack_->find(GetReturnPcAddr());
        CHECK(it != instrumentation_stack_->end());
        const InstrumentationStackFrame& frame = it->second;

        // We've reached a frame which has already been installed with instrumentation exit stub.
        // We should have already installed instrumentation or be interpreter on previous frames.
        reached_existing_instrumentation_frames_ = true;

        // Trampolines get replaced with their actual method in the stack,
        // so don't do the check below for runtime methods.
        if (!frame.method_->IsRuntimeMethod()) {
          CHECK_EQ(m->GetNonObsoleteMethod(), frame.method_->GetNonObsoleteMethod())
              << "Expected " << ArtMethod::PrettyMethod(m)
              << ", Found " << ArtMethod::PrettyMethod(frame.method_);
        }
        return_pc = frame.return_pc_;
        if (kVerboseInstrumentation) {
          LOG(INFO) << "Ignoring already instrumented " << frame.Dump();
        }
      } else {
        if (!m->IsRuntimeMethod()) {
          // Record the method so we can call method entry callbacks for all non-runtime methods on
          // the stack. Runtime methods don't need method entry callbacks.
          // TODO(232212577): Add tests to check the validity of the tracefiles generated.
          // Currently the tracing tests only check a trace file is generated.
          stack_methods_.push_back(m);
        }

        if (m->IsNative() && Runtime::Current()->IsJavaDebuggable()) {
          // Native methods in debuggable runtimes don't use instrumentation stubs.
          return true;
        }

        // If it is a JITed frame then just set the deopt bit if required
        // otherwise continue
        const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
        if (method_header != nullptr && method_header->HasShouldDeoptimizeFlag()) {
          if (deopt_all_frames_) {
            runtime_methods_need_deopt_check_ = true;
            SetShouldDeoptimizeFlag(DeoptimizeFlagValue::kDebug);
          }
          return true;
        }
        CHECK_NE(return_pc, 0U);
        DCHECK(!m->IsRuntimeMethod());
        if (UNLIKELY(reached_existing_instrumentation_frames_)) {
          // We already saw an existing instrumentation frame so this should be a runtime-method
          // inserted by the interpreter or runtime.
          std::string thread_name;
          GetThread()->GetThreadName(thread_name);
          LOG(FATAL) << "While walking " << thread_name << " found unexpected non-runtime method"
                     << " without instrumentation exit return or interpreter frame."
                     << " method is " << GetMethod()->PrettyMethod()
                     << " return_pc is " << std::hex << return_pc;
          UNREACHABLE();
        }

        InstrumentationStackFrame instrumentation_frame(
            GetThisObject().Ptr(), m, return_pc, false, force_deopt_id_);
        if (kVerboseInstrumentation) {
          LOG(INFO) << "Pushing frame " << instrumentation_frame.Dump();
        }

        instrumentation_stack_->insert({GetReturnPcAddr(), instrumentation_frame});
        SetReturnPc(instrumentation_exit_pc_);
      }
      return true;  // Continue.
    }
    std::map<uintptr_t, InstrumentationStackFrame>* const instrumentation_stack_;
    std::vector<ArtMethod*> stack_methods_;
    const uintptr_t instrumentation_exit_pc_;
    bool reached_existing_instrumentation_frames_;
    uint64_t force_deopt_id_;
    bool deopt_all_frames_;
    bool runtime_methods_need_deopt_check_;
  };
  if (kVerboseInstrumentation) {
    std::string thread_name;
    thread->GetThreadName(thread_name);
    LOG(INFO) << "Installing exit stubs in " << thread_name;
  }

  Instrumentation* instrumentation = reinterpret_cast<Instrumentation*>(arg);
  std::unique_ptr<Context> context(Context::Create());
  uintptr_t instrumentation_exit_pc = reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc());
  InstallStackVisitor visitor(thread,
                              context.get(),
                              instrumentation_exit_pc,
                              instrumentation->current_force_deopt_id_,
                              deopt_all_frames);
  visitor.WalkStack(true);

  if (visitor.runtime_methods_need_deopt_check_) {
    thread->SetDeoptCheckRequired(true);
  }

  if (instrumentation->ShouldNotifyMethodEnterExitEvents()) {
    // Create method enter events for all methods currently on the thread's stack. We only do this
    // if we haven't already processed the method enter events.
    for (auto smi = visitor.stack_methods_.rbegin(); smi != visitor.stack_methods_.rend(); smi++) {
      instrumentation->MethodEnterEvent(thread,  *smi);
    }
  }
  thread->VerifyStack();
}

void UpdateNeedsDexPcEventsOnStack(Thread* thread) REQUIRES(Locks::mutator_lock_) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());

  struct InstallStackVisitor final : public StackVisitor {
    InstallStackVisitor(Thread* thread_in, Context* context)
        : StackVisitor(thread_in, context, kInstrumentationStackWalk) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      ShadowFrame* shadow_frame = GetCurrentShadowFrame();
      if (shadow_frame != nullptr) {
        shadow_frame->SetNotifyDexPcMoveEvents(
            Runtime::Current()->GetInstrumentation()->NeedsDexPcEvents(GetMethod(), GetThread()));
      }
      return true;
    }
  };

  if (kVerboseInstrumentation) {
    std::string thread_name;
    thread->GetThreadName(thread_name);
    LOG(INFO) << "Updating DexPcMoveEvents on shadow frames on stack  " << thread_name;
  }

  std::unique_ptr<Context> context(Context::Create());
  InstallStackVisitor visitor(thread, context.get());
  visitor.WalkStack(true);
}

void Instrumentation::InstrumentThreadStack(Thread* thread, bool force_deopt) {
  instrumentation_stubs_installed_ = true;
  InstrumentationInstallStack(thread, this, force_deopt);
}

// Removes the instrumentation exit pc as the return PC for every quick frame.
static void InstrumentationRestoreStack(Thread* thread, void* arg)
    REQUIRES(Locks::mutator_lock_) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());

  struct RestoreStackVisitor final : public StackVisitor {
    RestoreStackVisitor(Thread* thread_in,
                        uintptr_t instrumentation_exit_pc,
                        Instrumentation* instrumentation)
        : StackVisitor(thread_in, nullptr, kInstrumentationStackWalk),
          thread_(thread_in),
          instrumentation_exit_pc_(instrumentation_exit_pc),
          instrumentation_(instrumentation),
          instrumentation_stack_(thread_in->GetInstrumentationStack()),
          frames_removed_(0),
          runtime_methods_need_deopt_check_(false) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      if (instrumentation_stack_->size() == 0) {
        return false;  // Stop.
      }
      ArtMethod* m = GetMethod();
      if (GetCurrentQuickFrame() == nullptr) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Ignoring a shadow frame. Frame " << GetFrameId()
              << " Method=" << ArtMethod::PrettyMethod(m);
        }
        return true;  // Ignore shadow frames.
      }
      if (m == nullptr) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Skipping upcall. Frame " << GetFrameId();
        }
        return true;  // Ignore upcalls and runtime methods.
      }
      const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
      if (method_header != nullptr && method_header->HasShouldDeoptimizeFlag()) {
        if (IsShouldDeoptimizeFlagForDebugSet()) {
          runtime_methods_need_deopt_check_ = true;
        }
      }
      auto it = instrumentation_stack_->find(GetReturnPcAddr());
      if (it != instrumentation_stack_->end()) {
        const InstrumentationStackFrame& instrumentation_frame = it->second;
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Removing exit stub in " << DescribeLocation();
        }
        if (instrumentation_frame.interpreter_entry_) {
          CHECK(m == Runtime::Current()->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsAndArgs));
        } else {
          CHECK_EQ(m->GetNonObsoleteMethod(),
                   instrumentation_frame.method_->GetNonObsoleteMethod())
              << ArtMethod::PrettyMethod(m)
              << " and " << instrumentation_frame.method_->GetNonObsoleteMethod()->PrettyMethod();
        }
        SetReturnPc(instrumentation_frame.return_pc_);
        if (instrumentation_->ShouldNotifyMethodEnterExitEvents() &&
            !m->IsRuntimeMethod()) {
          // Create the method exit events. As the methods didn't really exit the result is 0.
          // We only do this if no debugger is attached to prevent from posting events twice.
          JValue val;
          instrumentation_->MethodExitEvent(thread_, m, OptionalFrame{}, val);
        }
        frames_removed_++;
      } else {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  No exit stub in " << DescribeLocation();
        }
      }
      return true;  // Continue.
    }
    Thread* const thread_;
    const uintptr_t instrumentation_exit_pc_;
    Instrumentation* const instrumentation_;
    std::map<uintptr_t, instrumentation::InstrumentationStackFrame>* const instrumentation_stack_;
    size_t frames_removed_;
    bool runtime_methods_need_deopt_check_;
  };
  if (kVerboseInstrumentation) {
    std::string thread_name;
    thread->GetThreadName(thread_name);
    LOG(INFO) << "Removing exit stubs in " << thread_name;
  }
  std::map<uintptr_t, instrumentation::InstrumentationStackFrame>* stack =
      thread->GetInstrumentationStack();
  if (stack->size() > 0) {
    Instrumentation* instrumentation = reinterpret_cast<Instrumentation*>(arg);
    uintptr_t instrumentation_exit_pc =
        reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc());
    RestoreStackVisitor visitor(thread, instrumentation_exit_pc, instrumentation);
    visitor.WalkStack(true);
    DCHECK_IMPLIES(visitor.runtime_methods_need_deopt_check_, thread->IsDeoptCheckRequired());
    if (!visitor.runtime_methods_need_deopt_check_) {
      thread->SetDeoptCheckRequired(false);
    }
    CHECK_EQ(visitor.frames_removed_, stack->size());
    stack->clear();
  }
}

void Instrumentation::DeoptimizeAllThreadFrames() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::thread_list_lock_);
  ThreadList* tl = Runtime::Current()->GetThreadList();
  tl->ForEach([&](Thread* t) {
    Locks::mutator_lock_->AssertExclusiveHeld(self);
    InstrumentThreadStack(t, /* deopt_all_frames= */ true);
  });
  current_force_deopt_id_++;
}

static bool HasEvent(Instrumentation::InstrumentationEvent expected, uint32_t events) {
  return (events & expected) != 0;
}

static void PotentiallyAddListenerTo(Instrumentation::InstrumentationEvent event,
                                     uint32_t events,
                                     std::list<InstrumentationListener*>& list,
                                     InstrumentationListener* listener,
                                     bool* has_listener)
    REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  if (!HasEvent(event, events)) {
    return;
  }
  // If there is a free slot in the list, we insert the listener in that slot.
  // Otherwise we add it to the end of the list.
  auto it = std::find(list.begin(), list.end(), nullptr);
  if (it != list.end()) {
    *it = listener;
  } else {
    list.push_back(listener);
  }
  *has_listener = true;
}

void Instrumentation::AddListener(InstrumentationListener* listener, uint32_t events) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  PotentiallyAddListenerTo(kMethodEntered,
                           events,
                           method_entry_listeners_,
                           listener,
                           &have_method_entry_listeners_);
  PotentiallyAddListenerTo(kMethodExited,
                           events,
                           method_exit_listeners_,
                           listener,
                           &have_method_exit_listeners_);
  PotentiallyAddListenerTo(kMethodUnwind,
                           events,
                           method_unwind_listeners_,
                           listener,
                           &have_method_unwind_listeners_);
  PotentiallyAddListenerTo(kBranch,
                           events,
                           branch_listeners_,
                           listener,
                           &have_branch_listeners_);
  PotentiallyAddListenerTo(kDexPcMoved,
                           events,
                           dex_pc_listeners_,
                           listener,
                           &have_dex_pc_listeners_);
  PotentiallyAddListenerTo(kFieldRead,
                           events,
                           field_read_listeners_,
                           listener,
                           &have_field_read_listeners_);
  PotentiallyAddListenerTo(kFieldWritten,
                           events,
                           field_write_listeners_,
                           listener,
                           &have_field_write_listeners_);
  PotentiallyAddListenerTo(kExceptionThrown,
                           events,
                           exception_thrown_listeners_,
                           listener,
                           &have_exception_thrown_listeners_);
  PotentiallyAddListenerTo(kWatchedFramePop,
                           events,
                           watched_frame_pop_listeners_,
                           listener,
                           &have_watched_frame_pop_listeners_);
  PotentiallyAddListenerTo(kExceptionHandled,
                           events,
                           exception_handled_listeners_,
                           listener,
                           &have_exception_handled_listeners_);
  if (HasEvent(kDexPcMoved, events)) {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
      UpdateNeedsDexPcEventsOnStack(thread);
    }
  }
}

static void PotentiallyRemoveListenerFrom(Instrumentation::InstrumentationEvent event,
                                          uint32_t events,
                                          std::list<InstrumentationListener*>& list,
                                          InstrumentationListener* listener,
                                          bool* has_listener)
    REQUIRES(Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::classlinker_classes_lock_) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  if (!HasEvent(event, events)) {
    return;
  }
  auto it = std::find(list.begin(), list.end(), listener);
  if (it != list.end()) {
    // Just update the entry, do not remove from the list. Removing entries in the list
    // is unsafe when mutators are iterating over it.
    *it = nullptr;
  }

  // Check if the list contains any non-null listener, and update 'has_listener'.
  for (InstrumentationListener* l : list) {
    if (l != nullptr) {
      *has_listener = true;
      return;
    }
  }
  *has_listener = false;
}

void Instrumentation::RemoveListener(InstrumentationListener* listener, uint32_t events) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  PotentiallyRemoveListenerFrom(kMethodEntered,
                                events,
                                method_entry_listeners_,
                                listener,
                                &have_method_entry_listeners_);
  PotentiallyRemoveListenerFrom(kMethodExited,
                                events,
                                method_exit_listeners_,
                                listener,
                                &have_method_exit_listeners_);
  PotentiallyRemoveListenerFrom(kMethodUnwind,
                                events,
                                method_unwind_listeners_,
                                listener,
                                &have_method_unwind_listeners_);
  PotentiallyRemoveListenerFrom(kBranch,
                                events,
                                branch_listeners_,
                                listener,
                                &have_branch_listeners_);
  PotentiallyRemoveListenerFrom(kDexPcMoved,
                                events,
                                dex_pc_listeners_,
                                listener,
                                &have_dex_pc_listeners_);
  PotentiallyRemoveListenerFrom(kFieldRead,
                                events,
                                field_read_listeners_,
                                listener,
                                &have_field_read_listeners_);
  PotentiallyRemoveListenerFrom(kFieldWritten,
                                events,
                                field_write_listeners_,
                                listener,
                                &have_field_write_listeners_);
  PotentiallyRemoveListenerFrom(kExceptionThrown,
                                events,
                                exception_thrown_listeners_,
                                listener,
                                &have_exception_thrown_listeners_);
  PotentiallyRemoveListenerFrom(kWatchedFramePop,
                                events,
                                watched_frame_pop_listeners_,
                                listener,
                                &have_watched_frame_pop_listeners_);
  PotentiallyRemoveListenerFrom(kExceptionHandled,
                                events,
                                exception_handled_listeners_,
                                listener,
                                &have_exception_handled_listeners_);
  if (HasEvent(kDexPcMoved, events)) {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
      UpdateNeedsDexPcEventsOnStack(thread);
    }
  }
}

Instrumentation::InstrumentationLevel Instrumentation::GetCurrentInstrumentationLevel() const {
  return instrumentation_level_;
}

bool Instrumentation::RequiresInstrumentationInstallation(InstrumentationLevel new_level) const {
  // We need to reinstall instrumentation if we go to a different level.
  return GetCurrentInstrumentationLevel() != new_level;
}

void Instrumentation::ConfigureStubs(const char* key, InstrumentationLevel desired_level) {
  // Store the instrumentation level for this key or remove it.
  if (desired_level == InstrumentationLevel::kInstrumentNothing) {
    // The client no longer needs instrumentation.
    requested_instrumentation_levels_.erase(key);
  } else {
    // The client needs instrumentation.
    requested_instrumentation_levels_.Overwrite(key, desired_level);
  }

  UpdateStubs();
}

void Instrumentation::UpdateInstrumentationLevel(InstrumentationLevel requested_level) {
  instrumentation_level_ = requested_level;
}

void Instrumentation::MaybeRestoreInstrumentationStack() {
  // Restore stack only if there is no method currently deoptimized.
  if (!IsDeoptimizedMethodsEmpty()) {
    return;
  }

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::thread_list_lock_);
  bool no_remaining_deopts = true;
  // Check that there are no other forced deoptimizations. Do it here so we only need to lock
  // thread_list_lock once.
  // The compiler gets confused on the thread annotations, so use
  // NO_THREAD_SAFETY_ANALYSIS. Note that we hold the mutator lock
  // exclusively at this point.
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  Runtime::Current()->GetThreadList()->ForEach([&](Thread* t) NO_THREAD_SAFETY_ANALYSIS {
    no_remaining_deopts =
        no_remaining_deopts &&
        !t->IsForceInterpreter() &&
        !t->HasDebuggerShadowFrames() &&
        std::all_of(t->GetInstrumentationStack()->cbegin(),
                    t->GetInstrumentationStack()->cend(),
                    [&](const auto& frame) REQUIRES_SHARED(Locks::mutator_lock_) {
                      return frame.second.force_deopt_id_ == current_force_deopt_id_;
                    });
  });
  if (no_remaining_deopts) {
    Runtime::Current()->GetThreadList()->ForEach(InstrumentationRestoreStack, this);
    // Only do this after restoring, as walking the stack when restoring will see
    // the instrumentation exit pc.
    instrumentation_stubs_installed_ = false;
  }
}

void Instrumentation::UpdateStubs() {
  // Look for the highest required instrumentation level.
  InstrumentationLevel requested_level = InstrumentationLevel::kInstrumentNothing;
  for (const auto& v : requested_instrumentation_levels_) {
    requested_level = std::max(requested_level, v.second);
  }

  if (!RequiresInstrumentationInstallation(requested_level)) {
    // We're already set.
    return;
  }
  Thread* const self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  UpdateInstrumentationLevel(requested_level);
  InstallStubsClassVisitor visitor(this);
  runtime->GetClassLinker()->VisitClasses(&visitor);
  if (requested_level > InstrumentationLevel::kInstrumentNothing) {
    instrumentation_stubs_installed_ = true;
    MutexLock mu(self, *Locks::thread_list_lock_);
    for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
      InstrumentThreadStack(thread, /* deopt_all_frames= */ false);
    }
  } else {
    MaybeRestoreInstrumentationStack();
  }
}

static void ResetQuickAllocEntryPointsForThread(Thread* thread, void* arg ATTRIBUTE_UNUSED) {
  thread->ResetQuickAllocEntryPointsForThread();
}

void Instrumentation::SetEntrypointsInstrumented(bool instrumented) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  Locks::instrument_entrypoints_lock_->AssertHeld(self);
  if (runtime->IsStarted()) {
    ScopedSuspendAll ssa(__FUNCTION__);
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsInstrumented(instrumented);
    ResetQuickAllocEntryPoints();
    alloc_entrypoints_instrumented_ = instrumented;
  } else {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsInstrumented(instrumented);

    // Note: ResetQuickAllocEntryPoints only works when the runtime is started. Manually run the
    //       update for just this thread.
    // Note: self may be null. One of those paths is setting instrumentation in the Heap
    //       constructor for gcstress mode.
    if (self != nullptr) {
      ResetQuickAllocEntryPointsForThread(self, nullptr);
    }

    alloc_entrypoints_instrumented_ = instrumented;
  }
}

void Instrumentation::InstrumentQuickAllocEntryPoints() {
  MutexLock mu(Thread::Current(), *Locks::instrument_entrypoints_lock_);
  InstrumentQuickAllocEntryPointsLocked();
}

void Instrumentation::UninstrumentQuickAllocEntryPoints() {
  MutexLock mu(Thread::Current(), *Locks::instrument_entrypoints_lock_);
  UninstrumentQuickAllocEntryPointsLocked();
}

void Instrumentation::InstrumentQuickAllocEntryPointsLocked() {
  Locks::instrument_entrypoints_lock_->AssertHeld(Thread::Current());
  if (quick_alloc_entry_points_instrumentation_counter_ == 0) {
    SetEntrypointsInstrumented(true);
  }
  ++quick_alloc_entry_points_instrumentation_counter_;
}

void Instrumentation::UninstrumentQuickAllocEntryPointsLocked() {
  Locks::instrument_entrypoints_lock_->AssertHeld(Thread::Current());
  CHECK_GT(quick_alloc_entry_points_instrumentation_counter_, 0U);
  --quick_alloc_entry_points_instrumentation_counter_;
  if (quick_alloc_entry_points_instrumentation_counter_ == 0) {
    SetEntrypointsInstrumented(false);
  }
}

void Instrumentation::ResetQuickAllocEntryPoints() {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsStarted()) {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    runtime->GetThreadList()->ForEach(ResetQuickAllocEntryPointsForThread, nullptr);
  }
}

std::string Instrumentation::EntryPointString(const void* code) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (class_linker->IsQuickToInterpreterBridge(code)) {
    return "interpreter";
  } else if (class_linker->IsQuickResolutionStub(code)) {
    return "resolution";
  } else if (code == GetQuickInstrumentationEntryPoint()) {
    return "instrumentation";
  } else if (jit != nullptr && jit->GetCodeCache()->ContainsPc(code)) {
    return "jit";
  } else if (code == GetInvokeObsoleteMethodStub()) {
    return "obsolete";
  } else if (code == interpreter::GetNterpEntryPoint()) {
    return "nterp";
  } else if (code == interpreter::GetNterpWithClinitEntryPoint()) {
    return "nterp with clinit";
  } else if (class_linker->IsQuickGenericJniStub(code)) {
    return "generic jni";
  } else if (Runtime::Current()->GetOatFileManager().ContainsPc(code)) {
    return "oat";
  }
  return "unknown";
}

void Instrumentation::UpdateMethodsCodeImpl(ArtMethod* method, const void* new_code) {
  if (!AreExitStubsInstalled()) {
    // Fast path: no instrumentation.
    DCHECK(!IsDeoptimized(method));
    UpdateEntryPoints(method, new_code);
    return;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->IsQuickToInterpreterBridge(new_code)) {
    // It's always OK to update to the interpreter.
    UpdateEntryPoints(method, new_code);
    return;
  }

  if (IsDeoptimized(method)) {
    DCHECK(class_linker->IsQuickToInterpreterBridge(method->GetEntryPointFromQuickCompiledCode()))
        << EntryPointString(method->GetEntryPointFromQuickCompiledCode());
    // Don't update, stay deoptimized.
    return;
  }

  if (EntryExitStubsInstalled() && CodeNeedsEntryExitStub(new_code, method)) {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() == GetQuickInstrumentationEntryPoint() ||
        class_linker->IsQuickToInterpreterBridge(method->GetEntryPointFromQuickCompiledCode()))
              << EntryPointString(method->GetEntryPointFromQuickCompiledCode())
              << " " << method->PrettyMethod();
    // If the code we want to update the method with still needs entry/exit stub, just skip.
    return;
  }

  // At this point, we can update as asked.
  UpdateEntryPoints(method, new_code);
}

void Instrumentation::UpdateNativeMethodsCodeToJitCode(ArtMethod* method, const void* new_code) {
  // We don't do any read barrier on `method`'s declaring class in this code, as the JIT might
  // enter here on a soon-to-be deleted ArtMethod. Updating the entrypoint is OK though, as
  // the ArtMethod is still in memory.
  if (EntryExitStubsInstalled() && CodeNeedsEntryExitStub(new_code, method)) {
    // If stubs are installed don't update.
    return;
  }
  UpdateEntryPoints(method, new_code);
}

void Instrumentation::UpdateMethodsCode(ArtMethod* method, const void* new_code) {
  DCHECK(method->GetDeclaringClass()->IsResolved());
  UpdateMethodsCodeImpl(method, new_code);
}

bool Instrumentation::AddDeoptimizedMethod(ArtMethod* method) {
  if (IsDeoptimizedMethod(method)) {
    // Already in the map. Return.
    return false;
  }
  // Not found. Add it.
  deoptimized_methods_.insert(method);
  return true;
}

bool Instrumentation::IsDeoptimizedMethod(ArtMethod* method) {
  return deoptimized_methods_.find(method) != deoptimized_methods_.end();
}

bool Instrumentation::RemoveDeoptimizedMethod(ArtMethod* method) {
  auto it = deoptimized_methods_.find(method);
  if (it == deoptimized_methods_.end()) {
    return false;
  }
  deoptimized_methods_.erase(it);
  return true;
}

void Instrumentation::Deoptimize(ArtMethod* method) {
  CHECK(!method->IsNative());
  CHECK(!method->IsProxyMethod());
  CHECK(method->IsInvokable());

  Thread* self = Thread::Current();
  {
    Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
    bool has_not_been_deoptimized = AddDeoptimizedMethod(method);
    CHECK(has_not_been_deoptimized) << "Method " << ArtMethod::PrettyMethod(method)
        << " is already deoptimized";
  }
  if (!InterpreterStubsInstalled()) {
    UpdateEntryPoints(method, GetQuickToInterpreterBridge());

    // Install instrumentation exit stub and instrumentation frames. We may already have installed
    // these previously so it will only cover the newly created frames.
    instrumentation_stubs_installed_ = true;
    MutexLock mu(self, *Locks::thread_list_lock_);
    for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
      // This isn't a strong deopt. We deopt this method if it is still in the
      // deopt methods list. If by the time we hit this frame we no longer need
      // a deopt it is safe to continue. So we don't mark the frame.
      InstrumentThreadStack(thread, /* deopt_all_frames= */ false);
    }
  }
}

void Instrumentation::Undeoptimize(ArtMethod* method) {
  CHECK(!method->IsNative());
  CHECK(!method->IsProxyMethod());
  CHECK(method->IsInvokable());

  {
    Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
    bool found_and_erased = RemoveDeoptimizedMethod(method);
    CHECK(found_and_erased) << "Method " << ArtMethod::PrettyMethod(method)
        << " is not deoptimized";
  }

  // If interpreter stubs are still needed nothing to do.
  if (InterpreterStubsInstalled()) {
    return;
  }

  if (method->IsObsolete()) {
    // Don't update entry points for obsolete methods. The entrypoint should
    // have been set to InvokeObsoleteMethoStub.
    DCHECK_EQ(method->GetEntryPointFromQuickCompiledCodePtrSize(kRuntimePointerSize),
              GetInvokeObsoleteMethodStub());
    return;
  }

  // We are not using interpreter stubs for deoptimization. Restore the code of the method.
  // We still retain interpreter bridge if we need it for other reasons.
  if (InterpretOnly(method)) {
    UpdateEntryPoints(method, GetQuickToInterpreterBridge());
  } else if (NeedsClinitCheckBeforeCall(method) &&
             !method->GetDeclaringClass()->IsVisiblyInitialized()) {
    if (EntryExitStubsInstalled()) {
      UpdateEntryPoints(method, GetQuickInstrumentationEntryPoint());
    } else {
      UpdateEntryPoints(method, GetQuickResolutionStub());
    }
  } else {
    UpdateEntryPoints(method, GetMaybeInstrumentedCodeForInvoke(method));
  }

  // If there is no deoptimized method left, we can restore the stack of each thread.
  if (!EntryExitStubsInstalled()) {
    MaybeRestoreInstrumentationStack();
  }
}

bool Instrumentation::IsDeoptimizedMethodsEmpty() const {
  return deoptimized_methods_.empty();
}

bool Instrumentation::IsDeoptimized(ArtMethod* method) {
  DCHECK(method != nullptr);
  return IsDeoptimizedMethod(method);
}

void Instrumentation::DisableDeoptimization(const char* key) {
  // Remove any instrumentation support added for deoptimization.
  ConfigureStubs(key, InstrumentationLevel::kInstrumentNothing);
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  // Undeoptimized selected methods.
  while (true) {
    ArtMethod* method;
    {
      if (deoptimized_methods_.empty()) {
        break;
      }
      method = *deoptimized_methods_.begin();
      CHECK(method != nullptr);
    }
    Undeoptimize(method);
  }
}

// Indicates if instrumentation should notify method enter/exit events to the listeners.
bool Instrumentation::ShouldNotifyMethodEnterExitEvents() const {
  if (!HasMethodEntryListeners() && !HasMethodExitListeners()) {
    return false;
  }
  return !InterpreterStubsInstalled();
}

void Instrumentation::DeoptimizeEverything(const char* key) {
  ConfigureStubs(key, InstrumentationLevel::kInstrumentWithInterpreter);
}

void Instrumentation::UndeoptimizeEverything(const char* key) {
  CHECK(InterpreterStubsInstalled());
  ConfigureStubs(key, InstrumentationLevel::kInstrumentNothing);
}

void Instrumentation::EnableMethodTracing(const char* key, bool needs_interpreter) {
  InstrumentationLevel level;
  if (needs_interpreter) {
    level = InstrumentationLevel::kInstrumentWithInterpreter;
  } else {
    level = InstrumentationLevel::kInstrumentWithInstrumentationStubs;
  }
  ConfigureStubs(key, level);
}

void Instrumentation::DisableMethodTracing(const char* key) {
  ConfigureStubs(key, InstrumentationLevel::kInstrumentNothing);
}

const void* Instrumentation::GetCodeForInvoke(ArtMethod* method) {
  // This is called by instrumentation and resolution trampolines
  // and that should never be getting proxy methods.
  DCHECK(!method->IsProxyMethod()) << method->PrettyMethod();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const void* code = method->GetEntryPointFromQuickCompiledCodePtrSize(kRuntimePointerSize);
  // If we don't have the instrumentation, the resolution stub, or the
  // interpreter, just return the current entrypoint,
  // assuming it's the most optimized.
  if (code != GetQuickInstrumentationEntryPoint() &&
      !class_linker->IsQuickResolutionStub(code) &&
      !class_linker->IsQuickToInterpreterBridge(code)) {
    return code;
  }

  if (InterpretOnly(method)) {
    // If we're forced into interpreter just use it.
    return GetQuickToInterpreterBridge();
  }

  return GetOptimizedCodeFor(method);
}

const void* Instrumentation::GetMaybeInstrumentedCodeForInvoke(ArtMethod* method) {
  // This is called by resolution trampolines and that should never be getting proxy methods.
  DCHECK(!method->IsProxyMethod()) << method->PrettyMethod();
  const void* code = GetCodeForInvoke(method);
  if (EntryExitStubsInstalled() && CodeNeedsEntryExitStub(code, method)) {
    return GetQuickInstrumentationEntryPoint();
  }
  return code;
}

void Instrumentation::MethodEnterEventImpl(Thread* thread, ArtMethod* method) const {
  DCHECK(!method->IsRuntimeMethod());
  if (HasMethodEntryListeners()) {
    for (InstrumentationListener* listener : method_entry_listeners_) {
      if (listener != nullptr) {
        listener->MethodEntered(thread, method);
      }
    }
  }
}

template <>
void Instrumentation::MethodExitEventImpl(Thread* thread,
                                          ArtMethod* method,
                                          OptionalFrame frame,
                                          MutableHandle<mirror::Object>& return_value) const {
  if (HasMethodExitListeners()) {
    for (InstrumentationListener* listener : method_exit_listeners_) {
      if (listener != nullptr) {
        listener->MethodExited(thread, method, frame, return_value);
      }
    }
  }
}

template<> void Instrumentation::MethodExitEventImpl(Thread* thread,
                                                     ArtMethod* method,
                                                     OptionalFrame frame,
                                                     JValue& return_value) const {
  if (HasMethodExitListeners()) {
    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    if (method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetReturnTypePrimitive() !=
        Primitive::kPrimNot) {
      for (InstrumentationListener* listener : method_exit_listeners_) {
        if (listener != nullptr) {
          listener->MethodExited(thread, method, frame, return_value);
        }
      }
    } else {
      MutableHandle<mirror::Object> ret(hs.NewHandle(return_value.GetL()));
      MethodExitEventImpl(thread, method, frame, ret);
      return_value.SetL(ret.Get());
    }
  }
}

void Instrumentation::MethodUnwindEvent(Thread* thread,
                                        ArtMethod* method,
                                        uint32_t dex_pc) const {
  if (HasMethodUnwindListeners()) {
    for (InstrumentationListener* listener : method_unwind_listeners_) {
      if (listener != nullptr) {
        listener->MethodUnwind(thread, method, dex_pc);
      }
    }
  }
}

void Instrumentation::DexPcMovedEventImpl(Thread* thread,
                                          ObjPtr<mirror::Object> this_object,
                                          ArtMethod* method,
                                          uint32_t dex_pc) const {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> thiz(hs.NewHandle(this_object));
  for (InstrumentationListener* listener : dex_pc_listeners_) {
    if (listener != nullptr) {
      listener->DexPcMoved(thread, thiz, method, dex_pc);
    }
  }
}

void Instrumentation::BranchImpl(Thread* thread,
                                 ArtMethod* method,
                                 uint32_t dex_pc,
                                 int32_t offset) const {
  for (InstrumentationListener* listener : branch_listeners_) {
    if (listener != nullptr) {
      listener->Branch(thread, method, dex_pc, offset);
    }
  }
}

void Instrumentation::WatchedFramePopImpl(Thread* thread, const ShadowFrame& frame) const {
  for (InstrumentationListener* listener : watched_frame_pop_listeners_) {
    if (listener != nullptr) {
      listener->WatchedFramePop(thread, frame);
    }
  }
}

void Instrumentation::FieldReadEventImpl(Thread* thread,
                                         ObjPtr<mirror::Object> this_object,
                                         ArtMethod* method,
                                         uint32_t dex_pc,
                                         ArtField* field) const {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> thiz(hs.NewHandle(this_object));
  for (InstrumentationListener* listener : field_read_listeners_) {
    if (listener != nullptr) {
      listener->FieldRead(thread, thiz, method, dex_pc, field);
    }
  }
}

void Instrumentation::FieldWriteEventImpl(Thread* thread,
                                          ObjPtr<mirror::Object> this_object,
                                          ArtMethod* method,
                                          uint32_t dex_pc,
                                          ArtField* field,
                                          const JValue& field_value) const {
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  Handle<mirror::Object> thiz(hs.NewHandle(this_object));
  if (field->IsPrimitiveType()) {
    for (InstrumentationListener* listener : field_write_listeners_) {
      if (listener != nullptr) {
        listener->FieldWritten(thread, thiz, method, dex_pc, field, field_value);
      }
    }
  } else {
    Handle<mirror::Object> val(hs.NewHandle(field_value.GetL()));
    for (InstrumentationListener* listener : field_write_listeners_) {
      if (listener != nullptr) {
        listener->FieldWritten(thread, thiz, method, dex_pc, field, val);
      }
    }
  }
}

void Instrumentation::ExceptionThrownEvent(Thread* thread,
                                           ObjPtr<mirror::Throwable> exception_object) const {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> h_exception(hs.NewHandle(exception_object));
  if (HasExceptionThrownListeners()) {
    DCHECK_EQ(thread->GetException(), h_exception.Get());
    thread->ClearException();
    for (InstrumentationListener* listener : exception_thrown_listeners_) {
      if (listener != nullptr) {
        listener->ExceptionThrown(thread, h_exception);
      }
    }
    // See b/65049545 for discussion about this behavior.
    thread->AssertNoPendingException();
    thread->SetException(h_exception.Get());
  }
}

void Instrumentation::ExceptionHandledEvent(Thread* thread,
                                            ObjPtr<mirror::Throwable> exception_object) const {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> h_exception(hs.NewHandle(exception_object));
  if (HasExceptionHandledListeners()) {
    // We should have cleared the exception so that callers can detect a new one.
    DCHECK(thread->GetException() == nullptr);
    for (InstrumentationListener* listener : exception_handled_listeners_) {
      if (listener != nullptr) {
        listener->ExceptionHandled(thread, h_exception);
      }
    }
  }
}

void Instrumentation::PushInstrumentationStackFrame(Thread* self,
                                                    ObjPtr<mirror::Object> this_object,
                                                    ArtMethod* method,
                                                    uintptr_t stack_ptr,
                                                    uintptr_t lr,
                                                    bool interpreter_entry) {
  DCHECK(!self->IsExceptionPending());
  std::map<uintptr_t, instrumentation::InstrumentationStackFrame>* stack =
      self->GetInstrumentationStack();
  if (kVerboseInstrumentation) {
    LOG(INFO) << "Entering " << ArtMethod::PrettyMethod(method) << " from PC "
              << reinterpret_cast<void*>(lr);
  }

  // We send the enter event before pushing the instrumentation frame to make cleanup easier. If the
  // event causes an exception we can simply send the unwind event and return.
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_this(hs.NewHandle(this_object));
  if (!interpreter_entry) {
    MethodEnterEvent(self, method);
    if (self->IsExceptionPending()) {
      MethodUnwindEvent(self, method, 0);
      return;
    }
  }

  // We have a callee-save frame meaning this value is guaranteed to never be 0.
  DCHECK(!self->IsExceptionPending());

  instrumentation::InstrumentationStackFrame instrumentation_frame(
      h_this.Get(), method, lr, interpreter_entry, current_force_deopt_id_);
  stack->insert({stack_ptr, instrumentation_frame});
}

DeoptimizationMethodType Instrumentation::GetDeoptimizationMethodType(ArtMethod* method) {
  if (method->IsRuntimeMethod()) {
    // Certain methods have strict requirement on whether the dex instruction
    // should be re-executed upon deoptimization.
    if (method == Runtime::Current()->GetCalleeSaveMethod(
        CalleeSaveType::kSaveEverythingForClinit)) {
      return DeoptimizationMethodType::kKeepDexPc;
    }
    if (method == Runtime::Current()->GetCalleeSaveMethod(
        CalleeSaveType::kSaveEverythingForSuspendCheck)) {
      return DeoptimizationMethodType::kKeepDexPc;
    }
  }
  return DeoptimizationMethodType::kDefault;
}

JValue Instrumentation::GetReturnValue(ArtMethod* method,
                                       bool* is_ref,
                                       uint64_t* gpr_result,
                                       uint64_t* fpr_result) {
  uint32_t length;
  const PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();

  // Runtime method does not call into MethodExitEvent() so there should not be
  // suspension point below.
  ScopedAssertNoThreadSuspension ants(__FUNCTION__, method->IsRuntimeMethod());
  DCHECK(!method->IsRuntimeMethod());
  char return_shorty = method->GetInterfaceMethodIfProxy(pointer_size)->GetShorty(&length)[0];

  *is_ref = return_shorty == '[' || return_shorty == 'L';
  JValue return_value;
  if (return_shorty == 'V') {
    return_value.SetJ(0);
  } else if (return_shorty == 'F' || return_shorty == 'D') {
    return_value.SetJ(*fpr_result);
  } else {
    return_value.SetJ(*gpr_result);
  }
  return return_value;
}

bool Instrumentation::PushDeoptContextIfNeeded(Thread* self,
                                               DeoptimizationMethodType deopt_type,
                                               bool is_ref,
                                               const JValue& return_value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (self->IsExceptionPending()) {
    return false;
  }

  ArtMethod** sp = self->GetManagedStack()->GetTopQuickFrame();
  DCHECK(sp != nullptr && (*sp)->IsRuntimeMethod());
  if (!ShouldDeoptimizeCaller(self, sp)) {
    return false;
  }

  // TODO(mythria): The current deopt behaviour is we just re-execute the
  // alloc instruction so we don't need the return value. For instrumentation
  // related deopts, we actually don't need to and can use the result we got
  // here. Since this is a debug only feature it is not very important but
  // consider reusing the result in future.
  self->PushDeoptimizationContext(
      return_value, is_ref, nullptr, /* from_code= */ false, deopt_type);
  self->SetException(Thread::GetDeoptimizationException());
  return true;
}

void Instrumentation::DeoptimizeIfNeeded(Thread* self,
                                         ArtMethod** sp,
                                         DeoptimizationMethodType type,
                                         JValue return_value,
                                         bool is_reference) {
  if (self->IsAsyncExceptionPending() || ShouldDeoptimizeCaller(self, sp)) {
    self->PushDeoptimizationContext(return_value,
                                    is_reference,
                                    nullptr,
                                    /* from_code= */ false,
                                    type);
    artDeoptimize(self);
  }
}

bool Instrumentation::NeedsSlowInterpreterForMethod(Thread* self, ArtMethod* method) {
  return (method != nullptr) &&
         (InterpreterStubsInstalled() ||
          IsDeoptimized(method) ||
          self->IsForceInterpreter() ||
          // NB Since structurally obsolete compiled methods might have the offsets of
          // methods/fields compiled in we need to go back to interpreter whenever we hit
          // them.
          method->GetDeclaringClass()->IsObsoleteObject() ||
          Dbg::IsForcedInterpreterNeededForUpcall(self, method));
}

bool Instrumentation::ShouldDeoptimizeCaller(Thread* self, ArtMethod** sp) {
  // When exit stubs aren't installed we don't need to check for any instrumentation related
  // deoptimizations.
  // TODO(mythria): Once we remove instrumentation stubs rename AreExitStubsInstalled. This is
  // used to check if any instrumentation related work needs to be done. For ex: calling method
  // entry / exit hooks, checking for instrumentation related deopts in suspend points
  if (!AreExitStubsInstalled()) {
    return false;
  }

  ArtMethod* runtime_method = *sp;
  DCHECK(runtime_method->IsRuntimeMethod());
  QuickMethodFrameInfo frame_info = Runtime::Current()->GetRuntimeMethodFrameInfo(runtime_method);
  return ShouldDeoptimizeCaller(self, sp, frame_info.FrameSizeInBytes());
}

bool Instrumentation::ShouldDeoptimizeCaller(Thread* self, ArtMethod** sp, size_t frame_size) {
  uintptr_t caller_sp = reinterpret_cast<uintptr_t>(sp) + frame_size;
  ArtMethod* caller = *(reinterpret_cast<ArtMethod**>(caller_sp));
  uintptr_t caller_pc_addr = reinterpret_cast<uintptr_t>(sp) + (frame_size - sizeof(void*));
  uintptr_t caller_pc = *reinterpret_cast<uintptr_t*>(caller_pc_addr);
  return ShouldDeoptimizeCaller(self, caller, caller_pc, caller_sp);
}


bool Instrumentation::ShouldDeoptimizeCaller(Thread* self, const NthCallerVisitor& visitor) {
  uintptr_t caller_sp = reinterpret_cast<uintptr_t>(visitor.GetCurrentQuickFrame());
  // When the caller isn't executing quick code there is no need to deoptimize.
  if (visitor.GetCurrentOatQuickMethodHeader() == nullptr) {
    return false;
  }
  return ShouldDeoptimizeCaller(self, visitor.GetOuterMethod(), visitor.caller_pc, caller_sp);
}

bool Instrumentation::ShouldDeoptimizeCaller(Thread* self,
                                             ArtMethod* caller,
                                             uintptr_t caller_pc,
                                             uintptr_t caller_sp) {
  if (caller == nullptr ||
      caller->IsNative() ||
      caller->IsRuntimeMethod() ||
      caller_pc == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc())) {
    // If caller_pc is QuickInstrumentationExit then deoptimization will be handled by the
    // instrumentation exit trampoline so we don't need to handle deoptimizations here.
    // We need to check for a deoptimization here because when a redefinition happens it is
    // not safe to use any compiled code because the field offsets might change. For native
    // methods, we don't embed any field offsets so no need to check for a deoptimization.
    // If the caller is null we don't need to do anything. This can happen when the caller
    // is being interpreted by the switch interpreter (when called from
    // artQuickToInterpreterBridge) / during shutdown / early startup.
    return false;
  }

  bool needs_deopt = NeedsSlowInterpreterForMethod(self, caller);

  // Non java debuggable apps don't support redefinition and hence it isn't required to check if
  // frame needs to be deoptimized. Even in debuggable apps, we only need this check when a
  // redefinition has actually happened. This is indicated by IsDeoptCheckRequired flag. We also
  // want to avoid getting method header when we need a deopt anyway.
  if (Runtime::Current()->IsJavaDebuggable() && !needs_deopt && self->IsDeoptCheckRequired()) {
    const OatQuickMethodHeader* header = caller->GetOatQuickMethodHeader(caller_pc);
    if (header != nullptr && header->HasShouldDeoptimizeFlag()) {
      DCHECK(header->IsOptimized());
      uint8_t* should_deopt_flag_addr =
          reinterpret_cast<uint8_t*>(caller_sp) + header->GetShouldDeoptimizeFlagOffset();
      if ((*should_deopt_flag_addr & static_cast<uint8_t>(DeoptimizeFlagValue::kDebug)) != 0) {
        needs_deopt = true;
      }
    }
  }

  if (needs_deopt) {
    if (!Runtime::Current()->IsAsyncDeoptimizeable(caller, caller_pc)) {
      LOG(WARNING) << "Got a deoptimization request on un-deoptimizable method "
                   << caller->PrettyMethod();
      return false;
    }
    return true;
  }

  return false;
}

TwoWordReturn Instrumentation::PopInstrumentationStackFrame(Thread* self,
                                                            uintptr_t* return_pc_addr,
                                                            uint64_t* gpr_result,
                                                            uint64_t* fpr_result) {
  DCHECK(gpr_result != nullptr);
  DCHECK(fpr_result != nullptr);
  // Do the pop.
  std::map<uintptr_t, instrumentation::InstrumentationStackFrame>* stack =
      self->GetInstrumentationStack();
  CHECK_GT(stack->size(), 0U);
  auto it = stack->find(reinterpret_cast<uintptr_t>(return_pc_addr));
  CHECK(it != stack->end());
  InstrumentationStackFrame instrumentation_frame = it->second;
  stack->erase(it);

  // Set return PC and check the consistency of the stack.
  // We don't cache the return pc value in a local as it may change after
  // sending a method exit event.
  *return_pc_addr = instrumentation_frame.return_pc_;
  self->VerifyStack();

  ArtMethod* method = instrumentation_frame.method_;
  DCHECK(!method->IsRuntimeMethod());

  bool is_ref;
  JValue return_value = GetReturnValue(method, &is_ref, gpr_result, fpr_result);
  StackHandleScope<1> hs(self);
  MutableHandle<mirror::Object> res(hs.NewHandle<mirror::Object>(nullptr));
  if (is_ref) {
    // Take a handle to the return value so we won't lose it if we suspend.
    DCHECK_ALIGNED(return_value.GetL(), kObjectAlignment);
    res.Assign(return_value.GetL());
  }
  if (!instrumentation_frame.interpreter_entry_) {
    DCHECK(!method->IsRuntimeMethod());
    // Note that sending the event may change the contents of *return_pc_addr.
    MethodExitEvent(self, instrumentation_frame.method_, OptionalFrame{}, return_value);
  }

  // Deoptimize if the caller needs to continue execution in the interpreter. Do nothing if we get
  // back to an upcall.
  NthCallerVisitor visitor(self, 1, true);
  visitor.WalkStack(true);
  // Check if we forced all threads to deoptimize in the time between this frame being created and
  // now.
  bool should_deoptimize_frame = instrumentation_frame.force_deopt_id_ != current_force_deopt_id_;
  bool deoptimize = ShouldDeoptimizeCaller(self, visitor) || should_deoptimize_frame;

  if (is_ref) {
    // Restore the return value if it's a reference since it might have moved.
    *reinterpret_cast<mirror::Object**>(gpr_result) = res.Get();
  }

  if (deoptimize) {
    // NthCallerVisitor also takes inlined frames into consideration, so visitor.caller points to
    // the inlined function. We need the actual method corresponding to the return_pc_addr to check
    // if the method is deoptimizeable. So fetch the outer method.
    if (Runtime::Current()->IsAsyncDeoptimizeable(visitor.GetOuterMethod(), *return_pc_addr)) {
      if (kVerboseInstrumentation) {
        LOG(INFO) << "Deoptimizing "
                  << visitor.caller->PrettyMethod()
                  << " by returning from "
                  << method->PrettyMethod()
                  << " with result "
                  << std::hex << return_value.GetJ() << std::dec
                  << " in "
                  << *self;
      }
      DeoptimizationMethodType deopt_method_type = GetDeoptimizationMethodType(method);
      self->PushDeoptimizationContext(return_value,
                                      is_ref,
                                      /* exception= */ nullptr,
                                      /* from_code= */ false,
                                      deopt_method_type);
      return GetTwoWordSuccessValue(
          *return_pc_addr, reinterpret_cast<uintptr_t>(GetQuickDeoptimizationEntryPoint()));
    } else {
      VLOG(deopt) << "Got a deoptimization request on un-deoptimizable "
                  << visitor.caller->PrettyMethod() << " at PC "
                  << reinterpret_cast<void*>(*return_pc_addr);
    }
  }

  if (kVerboseInstrumentation) {
    LOG(INFO) << "Returning from " << method->PrettyMethod() << " to PC "
              << reinterpret_cast<void*>(*return_pc_addr);
  }
  return GetTwoWordSuccessValue(0, *return_pc_addr);
}

uintptr_t Instrumentation::PopInstrumentationStackUntil(Thread* self, uintptr_t pop_until) const {
  std::map<uintptr_t, instrumentation::InstrumentationStackFrame>* stack =
      self->GetInstrumentationStack();
  // Pop all instrumentation frames below `pop_until`.
  uintptr_t return_pc = 0u;
  for (auto i = stack->begin(); i != stack->end() && i->first <= pop_until;) {
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Popping for deoptimization " << i->second.method_->PrettyMethod();
    }
    return_pc = i->second.return_pc_;
    i = stack->erase(i);
  }
  return return_pc;
}

std::string InstrumentationStackFrame::Dump() const {
  std::ostringstream os;
  os << ArtMethod::PrettyMethod(method_) << ":"
      << reinterpret_cast<void*>(return_pc_) << " this=" << reinterpret_cast<void*>(this_object_)
      << " force_deopt_id=" << force_deopt_id_;
  return os.str();
}

}  // namespace instrumentation
}  // namespace art
