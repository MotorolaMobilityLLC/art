/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "method_handle_impl-inl.h"

#include "class-inl.h"
#include "class_root.h"

namespace art {
namespace mirror {

const char* MethodHandle::GetReturnTypeDescriptor(const char* invoke_method_name) {
  if (strcmp(invoke_method_name, "invoke") == 0 || strcmp(invoke_method_name, "invokeExact") == 0) {
    return "Ljava/lang/Object;";
  } else {
    return nullptr;
  }
}

void MethodHandle::Initialize(uintptr_t art_field_or_method,
                              Kind kind,
                              Handle<MethodType> method_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CHECK(!Runtime::Current()->IsActiveTransaction());
  SetFieldObject<false>(CachedSpreadInvokerOffset(), nullptr);
  SetFieldObject<false>(NominalTypeOffset(), nullptr);
  SetFieldObject<false>(MethodTypeOffset(), method_type.Get());
  SetField32<false>(HandleKindOffset(), static_cast<uint32_t>(kind));
  SetField64<false>(ArtFieldOrMethodOffset(), art_field_or_method);
}

mirror::MethodHandleImpl* MethodHandleImpl::Create(Thread* const self,
                                                   uintptr_t art_field_or_method,
                                                   MethodHandle::Kind kind,
                                                   Handle<MethodType> method_type)
    REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::MethodHandleImpl> mh(hs.NewHandle(ObjPtr<MethodHandleImpl>::DownCast(
      GetClassRoot<MethodHandleImpl>()->AllocObject(self))));
  mh->Initialize(art_field_or_method, kind, method_type);
  return mh.Get();
}

}  // namespace mirror
}  // namespace art
