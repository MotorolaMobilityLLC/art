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

#ifndef ART_RUNTIME_PRIMITIVE_H_
#define ART_RUNTIME_PRIMITIVE_H_

#include <sys/types.h>

#include "base/logging.h"
#include "base/macros.h"

namespace art {

static constexpr size_t kObjectReferenceSize = 4;

constexpr size_t ComponentSizeShiftWidth(size_t component_size) {
  return component_size == 1u ? 0u :
      component_size == 2u ? 1u :
          component_size == 4u ? 2u :
              component_size == 8u ? 3u : 0u;
}

class Primitive {
 public:
  enum Type {
    kPrimNot = 0,
    kPrimBoolean,
    kPrimByte,
    kPrimChar,
    kPrimShort,
    kPrimInt,
    kPrimLong,
    kPrimFloat,
    kPrimDouble,
    kPrimVoid,
    kPrimLast = kPrimVoid
  };

  static Type GetType(char type) {
    switch (type) {
      case 'B':
        return kPrimByte;
      case 'C':
        return kPrimChar;
      case 'D':
        return kPrimDouble;
      case 'F':
        return kPrimFloat;
      case 'I':
        return kPrimInt;
      case 'J':
        return kPrimLong;
      case 'S':
        return kPrimShort;
      case 'Z':
        return kPrimBoolean;
      case 'V':
        return kPrimVoid;
      default:
        return kPrimNot;
    }
  }

  static size_t ComponentSizeShift(Type type) {
    switch (type) {
      case kPrimVoid:
      case kPrimBoolean:
      case kPrimByte:    return 0;
      case kPrimChar:
      case kPrimShort:   return 1;
      case kPrimInt:
      case kPrimFloat:   return 2;
      case kPrimLong:
      case kPrimDouble:  return 3;
      case kPrimNot:     return ComponentSizeShiftWidth(kObjectReferenceSize);
      default:
        LOG(FATAL) << "Invalid type " << static_cast<int>(type);
        return 0;
    }
  }

  static size_t ComponentSize(Type type) {
    switch (type) {
      case kPrimVoid:    return 0;
      case kPrimBoolean:
      case kPrimByte:    return 1;
      case kPrimChar:
      case kPrimShort:   return 2;
      case kPrimInt:
      case kPrimFloat:   return 4;
      case kPrimLong:
      case kPrimDouble:  return 8;
      case kPrimNot:     return kObjectReferenceSize;
      default:
        LOG(FATAL) << "Invalid type " << static_cast<int>(type);
        return 0;
    }
  }

  static const char* Descriptor(Type type) {
    switch (type) {
      case kPrimBoolean:
        return "Z";
      case kPrimByte:
        return "B";
      case kPrimChar:
        return "C";
      case kPrimShort:
        return "S";
      case kPrimInt:
        return "I";
      case kPrimFloat:
        return "F";
      case kPrimLong:
        return "J";
      case kPrimDouble:
        return "D";
      case kPrimVoid:
        return "V";
      default:
        LOG(FATAL) << "Primitive char conversion on invalid type " << static_cast<int>(type);
        return nullptr;
    }
  }

  static const char* PrettyDescriptor(Type type);

  // Returns the descriptor corresponding to the boxed type of |type|.
  static const char* BoxedDescriptor(Type type);

  static bool IsFloatingPointType(Type type) {
    return type == kPrimFloat || type == kPrimDouble;
  }

  static bool IsIntegralType(Type type) {
    // The Java language does not allow treating boolean as an integral type but
    // our bit representation makes it safe.
    switch (type) {
      case kPrimBoolean:
      case kPrimByte:
      case kPrimChar:
      case kPrimShort:
      case kPrimInt:
      case kPrimLong:
        return true;
      default:
        return false;
    }
  }

  // Return true if |type| is an numeric type.
  static bool IsNumericType(Type type) {
    switch (type) {
      case Primitive::Type::kPrimNot: return false;
      case Primitive::Type::kPrimBoolean: return false;
      case Primitive::Type::kPrimByte: return true;
      case Primitive::Type::kPrimChar: return false;
      case Primitive::Type::kPrimShort: return true;
      case Primitive::Type::kPrimInt: return true;
      case Primitive::Type::kPrimLong: return true;
      case Primitive::Type::kPrimFloat: return true;
      case Primitive::Type::kPrimDouble: return true;
      case Primitive::Type::kPrimVoid: return false;
    }
  }

  // Returns true if |from| and |to| are the same or a widening conversion exists between them.
  static bool IsWidenable(Type from, Type to) {
    static_assert(Primitive::Type::kPrimByte < Primitive::Type::kPrimShort, "Bad ordering");
    static_assert(Primitive::Type::kPrimShort < Primitive::Type::kPrimInt, "Bad ordering");
    static_assert(Primitive::Type::kPrimInt < Primitive::Type::kPrimLong, "Bad ordering");
    static_assert(Primitive::Type::kPrimLong < Primitive::Type::kPrimFloat, "Bad ordering");
    static_assert(Primitive::Type::kPrimFloat < Primitive::Type::kPrimDouble, "Bad ordering");
    return IsNumericType(from) && IsNumericType(to) && from <= to;
  }

  static bool IsIntOrLongType(Type type) {
    return type == kPrimInt || type == kPrimLong;
  }

  static bool Is64BitType(Type type) {
    return type == kPrimLong || type == kPrimDouble;
  }

  // Return the general kind of `type`, fusing integer-like types as kPrimInt.
  static Type PrimitiveKind(Type type) {
    switch (type) {
      case kPrimBoolean:
      case kPrimByte:
      case kPrimShort:
      case kPrimChar:
      case kPrimInt:
        return kPrimInt;
      default:
        return type;
    }
  }

  static int64_t MinValueOfIntegralType(Type type) {
    switch (type) {
      case kPrimBoolean:
        return std::numeric_limits<bool>::min();
      case kPrimByte:
        return std::numeric_limits<int8_t>::min();
      case kPrimChar:
        return std::numeric_limits<uint16_t>::min();
      case kPrimShort:
        return std::numeric_limits<int16_t>::min();
      case kPrimInt:
        return std::numeric_limits<int32_t>::min();
      case kPrimLong:
        return std::numeric_limits<int64_t>::min();
      default:
        LOG(FATAL) << "non integral type";
    }
    return 0;
  }

  static int64_t MaxValueOfIntegralType(Type type) {
    switch (type) {
      case kPrimBoolean:
        return std::numeric_limits<bool>::max();
      case kPrimByte:
        return std::numeric_limits<int8_t>::max();
      case kPrimChar:
        return std::numeric_limits<uint16_t>::max();
      case kPrimShort:
        return std::numeric_limits<int16_t>::max();
      case kPrimInt:
        return std::numeric_limits<int32_t>::max();
      case kPrimLong:
        return std::numeric_limits<int64_t>::max();
      default:
        LOG(FATAL) << "non integral type";
    }
    return 0;
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Primitive);
};

std::ostream& operator<<(std::ostream& os, const Primitive::Type& state);

}  // namespace art

#endif  // ART_RUNTIME_PRIMITIVE_H_
