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

#include "reference.h"

#include "art_method.h"
#include "gc_root-inl.h"

namespace art {
namespace mirror {

GcRoot<Class> Reference::java_lang_ref_Reference_;

void Reference::SetClass(Class* java_lang_ref_Reference) {
  CHECK(java_lang_ref_Reference_.IsNull());
  CHECK(java_lang_ref_Reference != nullptr);
  java_lang_ref_Reference_ = GcRoot<Class>(java_lang_ref_Reference);
}

void Reference::ResetClass() {
  CHECK(!java_lang_ref_Reference_.IsNull());
  java_lang_ref_Reference_ = GcRoot<Class>(nullptr);
}

void Reference::VisitRoots(RootVisitor* visitor) {
  java_lang_ref_Reference_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

}  // namespace mirror
}  // namespace art
