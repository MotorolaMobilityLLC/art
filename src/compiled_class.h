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

#ifndef ART_SRC_COMPILED_CLASS_H_
#define ART_SRC_COMPILED_CLASS_H_

#include "object.h"

namespace art {

class CompiledClass {
 public:
  explicit CompiledClass(Class::Status status) : status_(status) {}
  ~CompiledClass() {}
  Class::Status GetStatus() const {
    return status_;
  }
 private:
  const Class::Status status_;
};

}  // namespace art

#endif  // ART_SRC_COMPILED_CLASS_H_
