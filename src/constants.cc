/*
 * Copyright 2012 Google Inc. All Rights Reserved.
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

#include "constants.h"

namespace art {

std::ostream& operator<<(std::ostream& os, const InvokeType& rhs) {
  switch (rhs) {
    case kStatic: os << "static"; break;
    case kDirect: os << "direct"; break;
    case kVirtual: os << "virtual"; break;
    case kSuper: os << "super"; break;
    case kInterface: os << "interface"; break;
    default: os << "InvokeType[" << static_cast<int>(rhs) << "]"; break;
  }
  return os;
}

}  // namespace art
