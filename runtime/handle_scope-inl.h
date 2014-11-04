/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_HANDLE_SCOPE_INL_H_
#define ART_RUNTIME_HANDLE_SCOPE_INL_H_

#include "handle_scope.h"

#include "handle.h"
#include "thread.h"

namespace art {

template<size_t kNumReferences>
inline StackHandleScope<kNumReferences>::StackHandleScope(Thread* self, mirror::Object* fill_value)
    : HandleScope(self->GetTopHandleScope(), kNumReferences), self_(self), pos_(0) {
  static_assert(kNumReferences >= 1, "StackHandleScope must contain at least 1 reference");
  // TODO: Figure out how to use a compile assert.
  CHECK_EQ(&storage_[0], GetReferences());
  for (size_t i = 0; i < kNumReferences; ++i) {
    SetReference(i, fill_value);
  }
  self_->PushHandleScope(this);
}

template<size_t kNumReferences>
inline StackHandleScope<kNumReferences>::~StackHandleScope() {
  HandleScope* top_handle_scope = self_->PopHandleScope();
  DCHECK_EQ(top_handle_scope, this);
}

}  // namespace art

#endif  // ART_RUNTIME_HANDLE_SCOPE_INL_H_
