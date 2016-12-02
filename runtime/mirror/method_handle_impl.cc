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

#include "method_handle_impl.h"

#include "class-inl.h"
#include "gc_root-inl.h"

namespace art {
namespace mirror {

mirror::Class* MethodHandle::StaticClass() {
  mirror::Class* klass = MethodHandleImpl::StaticClass()->GetSuperClass();
  DCHECK(klass->DescriptorEquals("Ljava/lang/invoke/MethodHandle;"));
  return klass;
}

GcRoot<mirror::Class> MethodHandleImpl::static_class_;

void MethodHandleImpl::SetClass(Class* klass) {
  CHECK(static_class_.IsNull()) << static_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  static_class_ = GcRoot<Class>(klass);
}

void MethodHandleImpl::ResetClass() {
  CHECK(!static_class_.IsNull());
  static_class_ = GcRoot<Class>(nullptr);
}

void MethodHandleImpl::VisitRoots(RootVisitor* visitor) {
  static_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

}  // namespace mirror
}  // namespace art
