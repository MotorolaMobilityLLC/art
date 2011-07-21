// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)
#include "src/jni_compiler.h"
#include <sys/mman.h>
#include "src/assembler.h"
#include "src/calling_convention.h"
#include "src/jni_internal.h"
#include "src/macros.h"
#include "src/managed_register.h"
#include "src/logging.h"
#include "src/thread.h"

namespace art {

// Generate the JNI bridge for the given method, general contract:
// - Arguments are in the managed runtime format, either on stack or in
//   registers, a reference to the method object is supplied as part of this
//   convention.
//
void JniCompiler::Compile(Assembler* jni_asm, Method* native_method) {
  CHECK(native_method->IsNative());
  JniCallingConvention jni_conv(native_method);
  ManagedRuntimeCallingConvention mr_conv(native_method);
  const bool is_static = native_method->IsStatic();

  // 1. Build the frame
  const size_t frame_size(jni_conv.FrameSize());
  jni_asm->BuildFrame(frame_size, mr_conv.MethodRegister());

  // 2. Save callee save registers that aren't callee save in the native code
  // TODO: implement computing the difference of the callee saves
  // and saving

  // 3. Set up the StackHandleBlock
  mr_conv.ResetIterator(FrameOffset(frame_size));
  jni_conv.ResetIterator(FrameOffset(0));
  jni_asm->StoreImmediateToFrame(jni_conv.ShbNumRefsOffset(),
                                 jni_conv.HandleCount(),
                                 mr_conv.InterproceduralScratchRegister());
  jni_asm->CopyRawPtrFromThread(jni_conv.ShbLinkOffset(),
                                Thread::TopShbOffset(),
                                mr_conv.InterproceduralScratchRegister());
  jni_asm->StoreStackOffsetToThread(Thread::TopShbOffset(),
                                    jni_conv.ShbOffset(),
                                    mr_conv.InterproceduralScratchRegister());

  // 4. Place incoming reference arguments into handle block
  jni_conv.Next();  // Skip JNIEnv*
  // 4.5. Create Class argument for static methods out of passed method
  if (is_static) {
    FrameOffset handle_offset = jni_conv.CurrentParamHandleOffset();
    // Check handle offset is within frame
    CHECK_LT(handle_offset.Uint32Value(), frame_size);
    jni_asm->LoadRef(jni_conv.InterproceduralScratchRegister(),
                     mr_conv.MethodRegister(), Method::ClassOffset());
    jni_asm->ValidateRef(jni_conv.InterproceduralScratchRegister(), false);
    jni_asm->StoreRef(handle_offset, jni_conv.InterproceduralScratchRegister());
    jni_conv.Next();  // handlerized so move to next argument
  }
  while (mr_conv.HasNext()) {
    CHECK(jni_conv.HasNext());
    bool ref_param = jni_conv.IsCurrentParamAReference();
    CHECK(!ref_param || mr_conv.IsCurrentParamAReference());
    // References need handlerization and the handle address passing
    if (ref_param) {
      // Compute handle offset, note null is handlerized but its boxed value
      // must be NULL
      FrameOffset handle_offset = jni_conv.CurrentParamHandleOffset();
      // Check handle offset is within frame
      CHECK_LT(handle_offset.Uint32Value(), frame_size);
      bool input_in_reg = mr_conv.IsCurrentParamInRegister();
      CHECK(input_in_reg || mr_conv.IsCurrentParamOnStack());
      if (input_in_reg) {
        LOG(FATAL) << "UNTESTED";
        ManagedRegister in_reg  =  mr_conv.CurrentParamRegister();
        jni_asm->ValidateRef(in_reg, mr_conv.IsCurrentParamPossiblyNull());
        jni_asm->StoreRef(handle_offset, in_reg);
      } else {
        FrameOffset in_off  = mr_conv.CurrentParamStackOffset();
        jni_asm->ValidateRef(in_off, mr_conv.IsCurrentParamPossiblyNull());
        jni_asm->CopyRef(handle_offset, in_off,
                         mr_conv.InterproceduralScratchRegister());
      }
    }
    mr_conv.Next();
    jni_conv.Next();
  }

  // 5. Transition from being in managed to native code
  // TODO: write out anchor, ensure the transition to native follow a store
  //       fence.
  jni_asm->StoreImmediateToThread(Thread::StateOffset(), Thread::kNative,
                                  mr_conv.InterproceduralScratchRegister());

  // 6. Move frame down to allow space for out going args. Do for as short a
  //    time as possible to aid profiling..
  const size_t out_arg_size = jni_conv.OutArgSize();
  jni_asm->IncreaseFrameSize(out_arg_size);

  // 7. Acquire lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    // Get stack handle for 1st argument
    if (is_static) {
      FrameOffset handle_offset = jni_conv.CurrentParamHandleOffset();
      if (jni_conv.IsCurrentParamOnStack()) {
        FrameOffset out_off = jni_conv.CurrentParamStackOffset();
        jni_asm->CreateStackHandle(out_off, handle_offset,
                                   mr_conv.InterproceduralScratchRegister(),
                                   false);
      } else {
        ManagedRegister out_reg = jni_conv.CurrentParamRegister();
        jni_asm->CreateStackHandle(out_reg, handle_offset,
                                   ManagedRegister::NoRegister(), false);
      }
    } else {
      CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
    }
    // Generate JNIEnv* in place and leave a copy in jni_env_register
    ManagedRegister jni_env_register =
        jni_conv.InterproceduralScratchRegister();
    if (jni_conv.IsCurrentParamInRegister()) {
      jni_env_register = jni_conv.CurrentParamRegister();
    }
    jni_asm->LoadRawPtrFromThread(jni_env_register, Thread::JniEnvOffset());
    if (!jni_conv.IsCurrentParamInRegister()) {
      FrameOffset out_off = jni_conv.CurrentParamStackOffset();
      jni_asm->StoreRawPtr(out_off, jni_env_register);
    }
    // Call JNIEnv*->MonitorEnter(JNIEnv*, object)
    jni_asm->Call(jni_env_register, JniEnvironment::MonitorEnterOffset(),
                  jni_conv.InterproceduralScratchRegister());
  }

  // 8. Iterate over arguments placing values from managed calling convention in
  //    to the convention required for a native call (shuffling). For references
  //    place an index/pointer to the reference after checking whether it is
  //    NULL (which must be encoded as NULL).
  //    NB. we do this prior to materializing the JNIEnv* and static's jclass to
  //    give as many free registers for the shuffle as possible
  mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
  jni_conv.ResetIterator(FrameOffset(out_arg_size));
  jni_conv.Next();  // Skip JNIEnv*
  if (is_static) {
    FrameOffset handle_offset = jni_conv.CurrentParamHandleOffset();
    if (jni_conv.IsCurrentParamOnStack()) {
      FrameOffset out_off = jni_conv.CurrentParamStackOffset();
      jni_asm->CreateStackHandle(out_off, handle_offset,
                                 mr_conv.InterproceduralScratchRegister(),
                                 false);
    } else {
      ManagedRegister out_reg = jni_conv.CurrentParamRegister();
      jni_asm->CreateStackHandle(out_reg, handle_offset,
                                 ManagedRegister::NoRegister(), false);
    }
    jni_conv.Next();
  }
  while (mr_conv.HasNext()) {
    CHECK(jni_conv.HasNext());
    CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
    mr_conv.Next();
    jni_conv.Next();
  }
  // 9. Create 1st argument, the JNI environment ptr
  jni_conv.ResetIterator(FrameOffset(out_arg_size));
  if (jni_conv.IsCurrentParamInRegister()) {
    jni_asm->LoadRawPtrFromThread(jni_conv.CurrentParamRegister(),
                                  Thread::JniEnvOffset());
  } else {
    jni_asm->CopyRawPtrFromThread(jni_conv.CurrentParamStackOffset(),
                                  Thread::JniEnvOffset(),
                                  jni_conv.InterproceduralScratchRegister());
  }

  // 10. Plant call to native code associated with method
  jni_asm->Call(mr_conv.MethodRegister(), Method::NativeMethodOffset(),
                mr_conv.InterproceduralScratchRegister());

  // 11. Release lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    // Save return value
    FrameOffset return_save_location = jni_conv.ReturnValueSaveLocation();
    CHECK_LT(return_save_location.Uint32Value(), frame_size+out_arg_size);
    jni_asm->Store(return_save_location, jni_conv.ReturnRegister(),
                   jni_conv.SizeOfReturnValue());
    // Get stack handle for 1st argument
    if (is_static) {
      FrameOffset handle_offset = jni_conv.CurrentParamHandleOffset();
      if (jni_conv.IsCurrentParamOnStack()) {
        FrameOffset out_off = jni_conv.CurrentParamStackOffset();
        jni_asm->CreateStackHandle(out_off, handle_offset,
                                   mr_conv.InterproceduralScratchRegister(),
                                   false);
      } else {
        ManagedRegister out_reg = jni_conv.CurrentParamRegister();
        jni_asm->CreateStackHandle(out_reg, handle_offset,
                                   ManagedRegister::NoRegister(), false);
      }
    } else {
      CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
    }
    // Generate JNIEnv* in place and leave a copy in jni_env_register
    ManagedRegister jni_env_register =
        jni_conv.InterproceduralScratchRegister();
    if (jni_conv.IsCurrentParamInRegister()) {
      jni_env_register = jni_conv.CurrentParamRegister();
    }
    jni_asm->LoadRawPtrFromThread(jni_env_register, Thread::JniEnvOffset());
    if (!jni_conv.IsCurrentParamInRegister()) {
      FrameOffset out_off = jni_conv.CurrentParamStackOffset();
      jni_asm->StoreRawPtr(out_off, jni_env_register);
    }
    // Call JNIEnv*->MonitorExit(JNIEnv*, object)
    jni_asm->Call(jni_env_register, JniEnvironment::MonitorExitOffset(),
                  jni_conv.InterproceduralScratchRegister());
    // Reload return value
    jni_asm->Load(jni_conv.ReturnRegister(), return_save_location,
                  jni_conv.SizeOfReturnValue());
  }

  // 11. Release outgoing argument area
  jni_asm->DecreaseFrameSize(out_arg_size);
  mr_conv.ResetIterator(FrameOffset(frame_size));
  jni_conv.ResetIterator(FrameOffset(0));

  // 12. Transition from being in native to managed code, possibly entering a
  //     safepoint
  jni_asm->StoreImmediateToThread(Thread::StateOffset(), Thread::kRunnable,
                                  mr_conv.InterproceduralScratchRegister());
  // TODO: check for safepoint transition

  // 15. Place result in correct register possibly dehandlerizing
  if (jni_conv.IsReturnAReference()) {
    jni_asm->LoadReferenceFromStackHandle(mr_conv.ReturnRegister(),
                                          jni_conv.ReturnRegister());
  } else {
    jni_asm->Move(mr_conv.ReturnRegister(), jni_conv.ReturnRegister());
  }

  // 16. Remove stack handle block from thread
  jni_asm->CopyRawPtrToThread(Thread::TopShbOffset(), jni_conv.ShbLinkOffset(),
                              jni_conv.InterproceduralScratchRegister());

  // 17. Remove activation
  jni_asm->RemoveFrame(frame_size);

  // 18. Finalize code generation
  size_t cs = jni_asm->CodeSize();
  MemoryRegion code(AllocateCode(cs), cs);
  jni_asm->FinalizeInstructions(code);
  native_method->SetCode(code.pointer());
}

// Copy a single parameter from the managed to the JNI calling convention
void JniCompiler::CopyParameter(Assembler* jni_asm,
                                ManagedRuntimeCallingConvention* mr_conv,
                                JniCallingConvention* jni_conv,
                                size_t frame_size, size_t out_arg_size) {
  bool input_in_reg = mr_conv->IsCurrentParamInRegister();
  bool output_in_reg = jni_conv->IsCurrentParamInRegister();
  FrameOffset handle_offset(0);
  bool null_allowed = false;
  bool ref_param = jni_conv->IsCurrentParamAReference();
  CHECK(!ref_param || mr_conv->IsCurrentParamAReference());
  CHECK(input_in_reg || mr_conv->IsCurrentParamOnStack());
  CHECK(output_in_reg || jni_conv->IsCurrentParamOnStack());
  // References need handlerization and the handle address passing
  if (ref_param) {
    null_allowed = mr_conv->IsCurrentParamPossiblyNull();
    // Compute handle offset. Note null is placed in the SHB but the jobject
    // passed to the native code must be null (not a pointer into the SHB
    // as with regular references).
    handle_offset = jni_conv->CurrentParamHandleOffset();
    // Check handle offset is within frame.
    CHECK_LT(handle_offset.Uint32Value(), (frame_size+out_arg_size));
  }
  if (input_in_reg && output_in_reg) {
    LOG(FATAL) << "UNTESTED";
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    if (ref_param) {
      jni_asm->CreateStackHandle(out_reg, handle_offset, in_reg,
                                 null_allowed);
    } else {
      jni_asm->Move(out_reg, in_reg);
    }
  } else if (!input_in_reg && !output_in_reg) {
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    if (ref_param) {
      jni_asm->CreateStackHandle(out_off, handle_offset,
                                 mr_conv->InterproceduralScratchRegister(),
                                 null_allowed);
    } else {
      FrameOffset in_off = mr_conv->CurrentParamStackOffset();
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Copy(out_off, in_off, mr_conv->InterproceduralScratchRegister(),
                    param_size);
    }
  } else if (!input_in_reg && output_in_reg) {
    LOG(FATAL) << "UNTESTED";
    FrameOffset in_off = mr_conv->CurrentParamStackOffset();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    // Check that incoming stack arguments are above the current stack frame.
    CHECK_GT(in_off.Uint32Value(), frame_size);
    if (ref_param) {
      jni_asm->CreateStackHandle(out_reg, handle_offset,
                                 ManagedRegister::NoRegister(), null_allowed);
    } else {
      unsigned int param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Load(out_reg, in_off, param_size);
    }
  } else {
    LOG(FATAL) << "UNTESTED";
    CHECK(input_in_reg && !output_in_reg);
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    // Check outgoing argument is within frame
    CHECK_LT(out_off.Uint32Value(), frame_size);
    if (ref_param) {
      // TODO: recycle value in in_reg rather than reload from handle
      jni_asm->CreateStackHandle(out_off, handle_offset,
                                 mr_conv->InterproceduralScratchRegister(),
                                 null_allowed);
    } else {
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Store(out_off, in_reg, param_size);
    }
  }
}

void* JniCompiler::AllocateCode(size_t size) {
  CHECK_LT(((jni_code_top_ - jni_code_) + size), jni_code_size_);
  void *result = jni_code_top_;
  jni_code_top_ += size;
  return result;
}

JniCompiler::JniCompiler() {
  // TODO: this shouldn't be managed by the JniCompiler, we should have a
  // code cache.
  jni_code_size_ = 4096;
  jni_code_ = static_cast<byte*>(mmap(NULL, jni_code_size_,
                                 PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  CHECK_NE(MAP_FAILED, jni_code_);
  jni_code_top_ = jni_code_;
}

JniCompiler::~JniCompiler() {
  // TODO: this shouldn't be managed by the JniCompiler, we should have a
  // code cache.
  CHECK_EQ(0, munmap(jni_code_, jni_code_size_));
}

}  // namespace art
