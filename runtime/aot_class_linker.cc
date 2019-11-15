/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "aot_class_linker.h"

#include "class_status.h"
#include "compiler_callbacks.h"
#include "dex/class_reference.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "runtime.h"
#include "verifier/verifier_enums.h"

namespace art {

AotClassLinker::AotClassLinker(InternTable* intern_table)
    : ClassLinker(intern_table, /*fast_class_not_found_exceptions=*/ false) {}

AotClassLinker::~AotClassLinker() {}

bool AotClassLinker::CanAllocClass() {
  // AllocClass doesn't work under transaction, so we abort.
  if (Runtime::Current()->IsActiveTransaction()) {
    Runtime::Current()->AbortTransactionAndThrowAbortError(
        Thread::Current(), "Can't resolve type within transaction.");
    return false;
  }
  return ClassLinker::CanAllocClass();
}

// Wrap the original InitializeClass with creation of transaction when in strict mode.
bool AotClassLinker::InitializeClass(Thread* self,
                                     Handle<mirror::Class> klass,
                                     bool can_init_statics,
                                     bool can_init_parents) {
  Runtime* const runtime = Runtime::Current();
  bool strict_mode = runtime->IsActiveStrictTransactionMode();

  DCHECK(klass != nullptr);
  if (klass->IsInitialized() || klass->IsInitializing()) {
    return ClassLinker::InitializeClass(self, klass, can_init_statics, can_init_parents);
  }

  // When compiling a boot image extension, do not initialize a class defined
  // in a dex file belonging to the boot image we're compiling against.
  // However, we must allow the initialization of TransactionAbortError,
  // VerifyError, etc. outside of a transaction.
  if (!strict_mode && runtime->GetHeap()->ObjectIsInBootImageSpace(klass->GetDexCache())) {
    if (runtime->IsActiveTransaction()) {
      runtime->AbortTransactionAndThrowAbortError(self, "Can't initialize " + klass->PrettyTypeOf()
           + " because it is defined in a boot image dex file.");
      return false;
    }
    CHECK(klass->IsThrowableClass()) << klass->PrettyDescriptor();
  }

  // When in strict_mode, don't initialize a class if it belongs to boot but not initialized.
  if (strict_mode && klass->IsBootStrapClassLoaded()) {
    runtime->AbortTransactionAndThrowAbortError(self, "Can't resolve "
        + klass->PrettyTypeOf() + " because it is an uninitialized boot class.");
    return false;
  }

  // Don't initialize klass if it's superclass is not initialized, because superclass might abort
  // the transaction and rolled back after klass's change is commited.
  if (strict_mode && !klass->IsInterface() && klass->HasSuperClass()) {
    if (klass->GetSuperClass()->GetStatus() == ClassStatus::kInitializing) {
      runtime->AbortTransactionAndThrowAbortError(self, "Can't resolve "
          + klass->PrettyTypeOf() + " because it's superclass is not initialized.");
      return false;
    }
  }

  if (strict_mode) {
    runtime->EnterTransactionMode(/*strict=*/ true, klass.Get());
  }
  bool success = ClassLinker::InitializeClass(self, klass, can_init_statics, can_init_parents);

  if (strict_mode) {
    if (success) {
      // Exit Transaction if success.
      runtime->ExitTransactionMode();
    } else {
      // If not successfully initialized, don't rollback immediately, leave the cleanup to compiler
      // driver which needs abort message and exception.
      DCHECK(self->IsExceptionPending());
    }
  }
  return success;
}

verifier::FailureKind AotClassLinker::PerformClassVerification(Thread* self,
                                                               Handle<mirror::Class> klass,
                                                               verifier::HardFailLogMode log_level,
                                                               std::string* error_msg) {
  Runtime* const runtime = Runtime::Current();
  CompilerCallbacks* callbacks = runtime->GetCompilerCallbacks();
  ClassStatus old_status = callbacks->GetPreviousClassState(
      ClassReference(&klass->GetDexFile(), klass->GetDexClassDefIndex()));
  // Was it verified? Report no failure.
  if (old_status >= ClassStatus::kVerified) {
    return verifier::FailureKind::kNoFailure;
  }
  // Does it need to be verified at runtime? Report soft failure.
  if (old_status >= ClassStatus::kRetryVerificationAtRuntime) {
    // Error messages from here are only reported through -verbose:class. It is not worth it to
    // create a message.
    return verifier::FailureKind::kSoftFailure;
  }
  // Do the actual work.
  return ClassLinker::PerformClassVerification(self, klass, log_level, error_msg);
}

}  // namespace art
