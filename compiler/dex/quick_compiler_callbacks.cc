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

#include "quick_compiler_callbacks.h"

#include "quick/dex_file_to_method_inliner_map.h"
#include "verifier/method_verifier-inl.h"
#include "verification_results.h"

namespace art {

void QuickCompilerCallbacks::MethodVerified(verifier::MethodVerifier* verifier) {
  verification_results_->ProcessVerifiedMethod(verifier);
  MethodReference ref = verifier->GetMethodReference();
  method_inliner_map_->GetMethodInliner(ref.dex_file)->AnalyseMethodCode(verifier);
}

void QuickCompilerCallbacks::ClassRejected(ClassReference ref) {
  verification_results_->AddRejectedClass(ref);
}

}  // namespace art
