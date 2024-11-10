/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_VERIFIER_REG_TYPE_H_
#define ART_RUNTIME_VERIFIER_REG_TYPE_H_

#include <stdint.h>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include "base/arena_object.h"
#include "base/bit_vector.h"
#include "base/locks.h"
#include "base/macros.h"
#include "dex/primitive.h"
#include "gc_root.h"
#include "handle.h"
#include "handle_scope.h"
#include "obj_ptr.h"

namespace art HIDDEN {
namespace mirror {
class Class;
class ClassLoader;
}  // namespace mirror

class ArenaAllocator;
class ArenaBitVector;

namespace verifier {

class MethodVerifier;
class RegTypeCache;

#define FOR_EACH_CONCRETE_REG_TYPE(V)     \
  V(Undefined)                            \
  V(Conflict)                             \
  V(Boolean)                              \
  V(Byte)                                 \
  V(Char)                                 \
  V(Short)                                \
  V(Integer)                              \
  V(LongLo)                               \
  V(LongHi)                               \
  V(Float)                                \
  V(DoubleLo)                             \
  V(DoubleHi)                             \
  V(UnresolvedReference)                  \
  V(UninitializedReference)               \
  V(UninitializedThisReference)           \
  V(UnresolvedUninitializedReference)     \
  V(UnresolvedUninitializedThisReference) \
  V(UnresolvedMergedReference)            \
  V(UnresolvedSuperClass)                 \
  V(Reference)                            \
  V(PreciseConstant)                      \
  V(PreciseConstantLo)                    \
  V(PreciseConstantHi)                    \
  V(ImpreciseConstantLo)                  \
  V(ImpreciseConstantHi)                  \
  V(ImpreciseConstant)                    \
  V(Null)                                 \

#define FORWARD_DECLARE_REG_TYPE(name) class name##Type;
FOR_EACH_CONCRETE_REG_TYPE(FORWARD_DECLARE_REG_TYPE)
#undef FORWARD_DECLARE_REG_TYPE

/*
 * RegType holds information about the "type" of data held in a register.
 */
class RegType {
 public:
  enum Kind : uint8_t {
#define DEFINE_REG_TYPE_ENUMERATOR(name) \
    k##name,
    FOR_EACH_CONCRETE_REG_TYPE(DEFINE_REG_TYPE_ENUMERATOR)
#undef DEFINE_REG_TYPE_ENUMERATOR
  };

  Kind GetKind() const { return kind_; }

#define DEFINE_IS_CONCRETE_REG_TYPE(name) \
  bool Is##name() const { return GetKind() == Kind::k##name; }
  FOR_EACH_CONCRETE_REG_TYPE(DEFINE_IS_CONCRETE_REG_TYPE)
#undef DEFINE_IS_CONCRETE_REG_TYPE

  virtual bool IsConstantTypes() const { return false; }
  bool IsConstant() const {
    return IsImpreciseConstant() || IsPreciseConstant();
  }
  bool IsConstantLo() const {
    return IsImpreciseConstantLo() || IsPreciseConstantLo();
  }
  bool IsPrecise() const {
    return IsPreciseConstantLo() || IsPreciseConstant() ||
           IsPreciseConstantHi();
  }
  bool IsLongConstant() const { return IsConstantLo(); }
  bool IsConstantHi() const {
    return (IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  bool IsLongConstantHigh() const { return IsConstantHi(); }
  virtual bool IsUninitializedTypes() const { return false; }
  virtual bool IsUnresolvedTypes() const { return false; }

  bool IsLowHalf() const {
    return (IsLongLo() || IsDoubleLo() || IsPreciseConstantLo() || IsImpreciseConstantLo());
  }
  bool IsHighHalf() const {
    return (IsLongHi() || IsDoubleHi() || IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  bool IsLongOrDoubleTypes() const { return IsLowHalf(); }
  // Check this is the low half, and that type_h is its matching high-half.
  inline bool CheckWidePair(const RegType& type_h) const {
    if (IsLowHalf()) {
      return ((IsImpreciseConstantLo() && type_h.IsPreciseConstantHi()) ||
              (IsImpreciseConstantLo() && type_h.IsImpreciseConstantHi()) ||
              (IsPreciseConstantLo() && type_h.IsPreciseConstantHi()) ||
              (IsPreciseConstantLo() && type_h.IsImpreciseConstantHi()) ||
              (IsDoubleLo() && type_h.IsDoubleHi()) ||
              (IsLongLo() && type_h.IsLongHi()));
    }
    return false;
  }
  // The high half that corresponds to this low half
  const RegType& HighHalf(RegTypeCache* cache) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsConstantBoolean() const;
  virtual bool IsConstantChar() const { return false; }
  virtual bool IsConstantByte() const { return false; }
  virtual bool IsConstantShort() const { return false; }
  virtual bool IsOne() const { return false; }
  virtual bool IsZero() const { return false; }
  bool IsReferenceTypes() const {
    return IsNonZeroReferenceTypes() || IsZero() || IsNull();
  }
  bool IsZeroOrNull() const {
    return IsZero() || IsNull();
  }
  virtual bool IsNonZeroReferenceTypes() const { return false; }
  bool IsCategory1Types() const {
    return IsChar() || IsInteger() || IsFloat() || IsConstant() || IsByte() ||
           IsShort() || IsBoolean();
  }
  bool IsCategory2Types() const {
    return IsLowHalf();  // Don't expect explicit testing of high halves
  }
  bool IsBooleanTypes() const { return IsBoolean() || IsConstantBoolean(); }
  bool IsByteTypes() const {
    return IsConstantByte() || IsByte() || IsBoolean();
  }
  bool IsShortTypes() const {
    return IsShort() || IsByte() || IsBoolean() || IsConstantShort();
  }
  bool IsCharTypes() const {
    return IsChar() || IsBooleanTypes() || IsConstantChar();
  }
  bool IsIntegralTypes() const {
    return IsInteger() || IsConstant() || IsByte() || IsShort() || IsChar() ||
           IsBoolean();
  }
  // Give the constant value encoded, but this shouldn't be called in the
  // general case.
  bool IsArrayIndexTypes() const { return IsIntegralTypes(); }
  // Float type may be derived from any constant type
  bool IsFloatTypes() const { return IsFloat() || IsConstant(); }
  bool IsLongTypes() const { return IsLongLo() || IsLongConstant(); }
  bool IsLongHighTypes() const {
    return (IsLongHi() || IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  bool IsDoubleTypes() const { return IsDoubleLo() || IsLongConstant(); }
  bool IsDoubleHighTypes() const {
    return (IsDoubleHi() || IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  bool HasClass() const {
    bool result = !klass_.IsNull();
    DCHECK_EQ(result, HasClassVirtual());
    return result;
  }
  virtual bool HasClassVirtual() const { return false; }
  bool IsJavaLangObject() const REQUIRES_SHARED(Locks::mutator_lock_);
  virtual bool IsArrayTypes() const REQUIRES_SHARED(Locks::mutator_lock_);
  virtual bool IsObjectArrayTypes() const REQUIRES_SHARED(Locks::mutator_lock_);
  Primitive::Type GetPrimitiveType() const;
  bool IsJavaLangObjectArray() const
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsInstantiableTypes() const REQUIRES_SHARED(Locks::mutator_lock_);
  const std::string_view& GetDescriptor() const {
    DCHECK(HasClass() ||
           (IsUnresolvedTypes() && !IsUnresolvedMergedReference() &&
            !IsUnresolvedSuperClass()));
    return descriptor_;
  }
  ObjPtr<mirror::Class> GetClass() const REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsUnresolvedReference());
    DCHECK(!klass_.IsNull());
    DCHECK(HasClass());
    return klass_.Get();
  }
  Handle<mirror::Class> GetClassHandle() const REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsUnresolvedReference());
    DCHECK(!klass_.IsNull()) << Dump();
    DCHECK(HasClass()) << Dump();
    return klass_;
  }
  uint16_t GetId() const { return cache_id_; }
  const RegType& GetSuperClass(RegTypeCache* cache) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  virtual std::string Dump() const
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Can this type access other?
  bool CanAccess(const RegType& other) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can this type access a member with the given properties?
  bool CanAccessMember(ObjPtr<mirror::Class> klass, uint32_t access_flags) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can this type be assigned by src?
  // Note: Object and interface types may always be assigned to one another, see
  // comment on
  // ClassJoin.
  bool IsAssignableFrom(const RegType& src, MethodVerifier* verifier) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Can this type be assigned by src? Variant of IsAssignableFrom that doesn't
  // allow assignment to
  // an interface from an Object.
  bool IsStrictlyAssignableFrom(const RegType& src, MethodVerifier* verifier) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Are these RegTypes the same?
  bool Equals(const RegType& other) const { return GetId() == other.GetId(); }

  // Compute the merge of this register from one edge (path) with incoming_type
  // from another.
  const RegType& Merge(const RegType& incoming_type,
                       RegTypeCache* reg_types,
                       MethodVerifier* verifier) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Same as above, but also handles the case where incoming_type == this.
  const RegType& SafeMerge(const RegType& incoming_type,
                           RegTypeCache* reg_types,
                           MethodVerifier* verifier) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (Equals(incoming_type)) {
      return *this;
    }
    return Merge(incoming_type, reg_types, verifier);
  }

  virtual ~RegType() {}

  static void* operator new(size_t size) noexcept {
    return ::operator new(size);
  }

  static void* operator new(size_t size, ArenaAllocator* allocator);
  static void* operator new(size_t size, ScopedArenaAllocator* allocator) = delete;

  enum class AssignmentType {
    kBoolean,
    kByte,
    kShort,
    kChar,
    kInteger,
    kFloat,
    kLongLo,
    kDoubleLo,
    kConflict,
    kReference,
    kNotAssignable,
  };

  ALWAYS_INLINE
  inline AssignmentType GetAssignmentType() const {
    AssignmentType t = GetAssignmentTypeImpl();
    if (kIsDebugBuild) {
      if (IsBoolean()) {
        CHECK(AssignmentType::kBoolean == t);
      } else if (IsByte()) {
        CHECK(AssignmentType::kByte == t);
      } else if (IsShort()) {
        CHECK(AssignmentType::kShort == t);
      } else if (IsChar()) {
        CHECK(AssignmentType::kChar == t);
      } else if (IsInteger()) {
        CHECK(AssignmentType::kInteger == t);
      } else if (IsFloat()) {
        CHECK(AssignmentType::kFloat == t);
      } else if (IsLongLo()) {
        CHECK(AssignmentType::kLongLo == t);
      } else if (IsDoubleLo()) {
        CHECK(AssignmentType::kDoubleLo == t);
      } else if (IsConflict()) {
        CHECK(AssignmentType::kConflict == t);
      } else if (IsReferenceTypes()) {
        CHECK(AssignmentType::kReference == t);
      } else {
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
      }
    }
    return t;
  }

 protected:
  RegType(Handle<mirror::Class> klass,
          const std::string_view& descriptor,
          uint16_t cache_id,
          Kind kind) REQUIRES_SHARED(Locks::mutator_lock_)
      : descriptor_(descriptor),
        klass_(klass),
        cache_id_(cache_id),
        kind_(kind) {}

  template <typename Class>
  void CheckConstructorInvariants([[maybe_unused]] Class* this_) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  virtual AssignmentType GetAssignmentTypeImpl() const = 0;

  const std::string_view descriptor_;
  const Handle<mirror::Class> klass_;
  const uint16_t cache_id_;
  const Kind kind_;

  friend class RegTypeCache;

 private:
  virtual void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_);


  static bool AssignableFrom(const RegType& lhs,
                             const RegType& rhs,
                             bool strict,
                             MethodVerifier* verifier)
      REQUIRES_SHARED(Locks::mutator_lock_);

  DISALLOW_COPY_AND_ASSIGN(RegType);
};

std::ostream& operator<<(std::ostream& os, RegType::Kind kind);

namespace detail {

template <class ConcreteRegType>
struct RegTypeToKind { /* No `kind` defined in unspecialized template. */ };

#define DEFINE_REG_TYPE_TO_KIND(name) \
  template<> struct RegTypeToKind<name##Type> { \
    static constexpr RegType::Kind kind = RegType::Kind::k##name; \
  };
FOR_EACH_CONCRETE_REG_TYPE(DEFINE_REG_TYPE_TO_KIND);
#undef DEFINE_REG_TYPE_TO_KIND

}  // namespace detail

template <typename Class>
inline void RegType::CheckConstructorInvariants([[maybe_unused]] Class* this_) const {
  static_assert(std::is_final<Class>::value, "Class must be final.");
  if (kIsDebugBuild) {
    CHECK_EQ(GetKind(), detail::RegTypeToKind<Class>::kind);
    CheckInvariants();
  }
}

// Bottom type.
class ConflictType final : public RegType {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kConflict;
  }

  ConflictType(Handle<mirror::Class> klass,
               const std::string_view& descriptor,
               uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id, Kind::kConflict) {
    CheckConstructorInvariants(this);
  }
};

// A variant of the bottom type used to specify an undefined value in the
// incoming registers.
// Merging with UndefinedType yields ConflictType which is the true bottom.
class UndefinedType final : public RegType {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }

  UndefinedType(Handle<mirror::Class> klass,
                const std::string_view& descriptor,
                uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id, Kind::kUndefined) {
    CheckConstructorInvariants(this);
  }
};

class PrimitiveType : public RegType {
 public:
  PrimitiveType(Handle<mirror::Class> klass,
                const std::string_view& descriptor,
                uint16_t cache_id,
                Kind kind) REQUIRES_SHARED(Locks::mutator_lock_);

  bool HasClassVirtual() const override { return true; }
};

class Cat1Type : public PrimitiveType {
 public:
  Cat1Type(Handle<mirror::Class> klass,
           const std::string_view& descriptor,
           uint16_t cache_id,
           Kind kind) REQUIRES_SHARED(Locks::mutator_lock_);
};

class IntegerType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kInteger;
  }

  IntegerType(Handle<mirror::Class> klass,
              const std::string_view& descriptor,
              uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kInteger) {
    CheckConstructorInvariants(this);
  }
};

class BooleanType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kBoolean;
  }

  BooleanType(Handle<mirror::Class> klass,
              const std::string_view& descriptor,
              uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kBoolean) {
    CheckConstructorInvariants(this);
  }
};

class ByteType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kByte;
  }

  ByteType(Handle<mirror::Class> klass,
           const std::string_view& descriptor,
           uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kByte) {
    CheckConstructorInvariants(this);
  }
};

class ShortType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kShort;
  }

  ShortType(Handle<mirror::Class> klass,
            const std::string_view& descriptor,
            uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kShort) {
    CheckConstructorInvariants(this);
  }
};

class CharType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kChar;
  }

  CharType(Handle<mirror::Class> klass,
           const std::string_view& descriptor,
           uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kChar) {
    CheckConstructorInvariants(this);
  }
};

class FloatType final : public Cat1Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kFloat;
  }

  FloatType(Handle<mirror::Class> klass,
            const std::string_view& descriptor,
            uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat1Type(klass, descriptor, cache_id, Kind::kFloat) {
    CheckConstructorInvariants(this);
  }
};

class Cat2Type : public PrimitiveType {
 public:
  Cat2Type(Handle<mirror::Class> klass,
           const std::string_view& descriptor,
           uint16_t cache_id,
           Kind kind) REQUIRES_SHARED(Locks::mutator_lock_);
};

class LongLoType final : public Cat2Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kLongLo;
  }

  LongLoType(Handle<mirror::Class> klass,
             const std::string_view& descriptor,
             uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat2Type(klass, descriptor, cache_id, Kind::kLongLo) {
    CheckConstructorInvariants(this);
  }
};

class LongHiType final : public Cat2Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }

  LongHiType(Handle<mirror::Class> klass,
             const std::string_view& descriptor,
             uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat2Type(klass, descriptor, cache_id, Kind::kLongHi) {
    CheckConstructorInvariants(this);
  }
};

class DoubleLoType final : public Cat2Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kDoubleLo;
  }

  DoubleLoType(Handle<mirror::Class> klass,
               const std::string_view& descriptor,
               uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat2Type(klass, descriptor, cache_id, Kind::kDoubleLo) {
    CheckConstructorInvariants(this);
  }
};

class DoubleHiType final : public Cat2Type {
 public:
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }

  DoubleHiType(Handle<mirror::Class> klass,
               const std::string_view& descriptor,
               uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : Cat2Type(klass, descriptor, cache_id, Kind::kDoubleHi) {
    CheckConstructorInvariants(this);
  }
};

class ConstantType : public RegType {
 public:
  ConstantType(Handle<mirror::Class> klass,
               uint32_t constant,
               uint16_t cache_id,
               Kind kind) REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, "", cache_id, kind), constant_(constant) {
  }


  // If this is a 32-bit constant, what is the value? This value may be
  // imprecise in which case
  // the value represents part of the integer range of values that may be held
  // in the register.
  int32_t ConstantValue() const {
    DCHECK(IsConstantTypes());
    return constant_;
  }

  int32_t ConstantValueLo() const {
    DCHECK(IsConstantLo());
    return constant_;
  }

  int32_t ConstantValueHi() const {
    if (IsConstantHi() || IsPreciseConstantHi() || IsImpreciseConstantHi()) {
      return constant_;
    } else {
      DCHECK(false);
      return 0;
    }
  }

  bool IsZero() const override {
    return IsPreciseConstant() && ConstantValue() == 0;
  }
  bool IsOne() const override {
    return IsPreciseConstant() && ConstantValue() == 1;
  }

  bool IsConstantChar() const override {
    return IsConstant() && ConstantValue() >= 0 &&
           ConstantValue() <= std::numeric_limits<uint16_t>::max();
  }
  bool IsConstantByte() const override {
    return IsConstant() &&
           ConstantValue() >= std::numeric_limits<int8_t>::min() &&
           ConstantValue() <= std::numeric_limits<int8_t>::max();
  }
  bool IsConstantShort() const override {
    return IsConstant() &&
           ConstantValue() >= std::numeric_limits<int16_t>::min() &&
           ConstantValue() <= std::numeric_limits<int16_t>::max();
  }
  bool IsConstantTypes() const override { return true; }

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }

 private:
  const uint32_t constant_;
};

class PreciseConstantType final : public ConstantType {
 public:
  PreciseConstantType(Handle<mirror::Class> cls, uint32_t constant, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : ConstantType(cls, constant, cache_id, Kind::kPreciseConstant) {
    CheckConstructorInvariants(this);
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

class PreciseConstantLoType final : public ConstantType {
 public:
  PreciseConstantLoType(Handle<mirror::Class> cls, uint32_t constant, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : ConstantType(cls, constant, cache_id, Kind::kPreciseConstantLo) {
    CheckConstructorInvariants(this);
  }
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

class PreciseConstantHiType final : public ConstantType {
 public:
  PreciseConstantHiType(Handle<mirror::Class> cls, uint32_t constant, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : ConstantType(cls, constant, cache_id, Kind::kPreciseConstantHi) {
    CheckConstructorInvariants(this);
  }
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

class ImpreciseConstantType final : public ConstantType {
 public:
  ImpreciseConstantType(Handle<mirror::Class> cls, uint32_t constat, uint16_t cache_id)
       REQUIRES_SHARED(Locks::mutator_lock_)
       : ConstantType(cls, constat, cache_id, Kind::kImpreciseConstant) {
    CheckConstructorInvariants(this);
  }
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

class ImpreciseConstantLoType final : public ConstantType {
 public:
  ImpreciseConstantLoType(Handle<mirror::Class> cls, uint32_t constant, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : ConstantType(cls, constant, cache_id, Kind::kImpreciseConstantLo) {
    CheckConstructorInvariants(this);
  }
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

class ImpreciseConstantHiType final : public ConstantType {
 public:
  ImpreciseConstantHiType(Handle<mirror::Class> cls, uint32_t constant, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : ConstantType(cls, constant, cache_id, Kind::kImpreciseConstantHi) {
    CheckConstructorInvariants(this);
  }
  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kNotAssignable;
  }
};

// Special "null" type that captures the semantics of null / bottom.
class NullType final : public RegType {
 public:
  std::string Dump() const override {
    return "null";
  }

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kReference;
  }

  bool IsConstantTypes() const override {
    return true;
  }

  NullType(Handle<mirror::Class> klass, const std::string_view& descriptor, uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id, Kind::kNull) {
    CheckConstructorInvariants(this);
  }
};

// Common parent of all uninitialized types. Uninitialized types are created by
// "new" dex
// instructions and must be passed to a constructor.
class UninitializedType : public RegType {
 public:
  UninitializedType(Handle<mirror::Class> klass,
                    const std::string_view& descriptor,
                    uint16_t cache_id,
                    Kind kind)
      : RegType(klass, descriptor, cache_id, kind) {}

  bool IsUninitializedTypes() const override;
  bool IsNonZeroReferenceTypes() const override;

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kReference;
  }
};

// Similar to ReferenceType but not yet having been passed to a constructor.
class UninitializedReferenceType final : public UninitializedType {
 public:
  UninitializedReferenceType(Handle<mirror::Class> klass,
                             const std::string_view& descriptor,
                             uint16_t cache_id,
                             const ReferenceType* initialized_type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UninitializedType(klass, descriptor, cache_id, Kind::kUninitializedReference),
        initialized_type_(initialized_type) {
    CheckConstructorInvariants(this);
  }

  bool HasClassVirtual() const override { return true; }

  const ReferenceType* GetInitializedType() const {
    return initialized_type_;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  // The corresponding initialized type to transition to after a constructor call.
  const ReferenceType* const initialized_type_;
};

// Similar to UnresolvedReferenceType but not yet having been passed to a
// constructor.
class UnresolvedUninitializedReferenceType final : public UninitializedType {
 public:
  UnresolvedUninitializedReferenceType(Handle<mirror::Class> klass,
                                       const std::string_view& descriptor,
                                       uint16_t cache_id,
                                       const UnresolvedReferenceType* initialized_type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UninitializedType(klass, descriptor, cache_id, Kind::kUnresolvedUninitializedReference),
        initialized_type_(initialized_type) {
    CheckConstructorInvariants(this);
  }

  bool IsUnresolvedTypes() const override { return true; }

  const UnresolvedReferenceType* GetInitializedType() const {
    return initialized_type_;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  // The corresponding initialized type to transition to after a constructor call.
  const UnresolvedReferenceType* const initialized_type_;
};

// Similar to UninitializedReferenceType but special case for the this argument
// of a constructor.
class UninitializedThisReferenceType final : public UninitializedType {
 public:
  UninitializedThisReferenceType(Handle<mirror::Class> klass,
                                 const std::string_view& descriptor,
                                 uint16_t cache_id,
                                 const ReferenceType* initialized_type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UninitializedType(klass, descriptor, cache_id, Kind::kUninitializedThisReference),
        initialized_type_(initialized_type) {
    CheckConstructorInvariants(this);
  }

  bool HasClassVirtual() const override { return true; }

  const ReferenceType* GetInitializedType() const {
    return initialized_type_;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  // The corresponding initialized type to transition to after a constructor call.
  const ReferenceType* initialized_type_;
};

class UnresolvedUninitializedThisReferenceType final : public UninitializedType {
 public:
  UnresolvedUninitializedThisReferenceType(Handle<mirror::Class> klass,
                                           const std::string_view& descriptor,
                                           uint16_t cache_id,
                                           const UnresolvedReferenceType* initialized_type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UninitializedType(klass, descriptor, cache_id, Kind::kUnresolvedUninitializedThisReference),
        initialized_type_(initialized_type) {
    CheckConstructorInvariants(this);
  }

  bool IsUnresolvedTypes() const override { return true; }

  const UnresolvedReferenceType* GetInitializedType() const {
    return initialized_type_;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  // The corresponding initialized type to transition to after a constructor call.
  const UnresolvedReferenceType* initialized_type_;
};

// A type of register holding a reference to an Object of type GetClass or a
// sub-class.
class ReferenceType final : public RegType {
 public:
  ReferenceType(Handle<mirror::Class> klass,
                const std::string_view& descriptor,
                uint16_t cache_id) REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id, Kind::kReference),
        uninitialized_type_(nullptr) {
    CheckConstructorInvariants(this);
  }

  bool IsNonZeroReferenceTypes() const override { return true; }

  bool HasClassVirtual() const override { return true; }

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kReference;
  }

  const UninitializedReferenceType* GetUninitializedType() const {
    return uninitialized_type_;
  }

  void SetUninitializedType(const UninitializedReferenceType* uninitialized_type) const {
    uninitialized_type_ = uninitialized_type;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  // The corresponding uninitialized type created from this type for a `new-instance` instruction.
  // This member is mutable because it's a part of the type cache, not part of the type itself.
  mutable const UninitializedReferenceType* uninitialized_type_;
};

// Common parent of unresolved types.
class UnresolvedType : public RegType {
 public:
  UnresolvedType(Handle<mirror::Class> klass,
                 const std::string_view& descriptor,
                 uint16_t cache_id,
                 Kind kind)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id, kind) {}

  bool IsNonZeroReferenceTypes() const override;

  AssignmentType GetAssignmentTypeImpl() const override {
    return AssignmentType::kReference;
  }
};

// Similar to ReferenceType except the Class couldn't be loaded. Assignability
// and other tests made
// of this type must be conservative.
class UnresolvedReferenceType final : public UnresolvedType {
 public:
  UnresolvedReferenceType(Handle<mirror::Class> cls,
                          const std::string_view& descriptor,
                          uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UnresolvedType(cls, descriptor, cache_id, Kind::kUnresolvedReference),
        uninitialized_type_(nullptr) {
    CheckConstructorInvariants(this);
  }

  bool IsUnresolvedTypes() const override { return true; }

  const UnresolvedUninitializedReferenceType* GetUninitializedType() const {
    return uninitialized_type_;
  }

  void SetUninitializedType(const UnresolvedUninitializedReferenceType* uninitialized_type) const {
    uninitialized_type_ = uninitialized_type;
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  // The corresponding uninitialized type created from this type for a `new-instance` instruction.
  // This member is mutable because it's a part of the type cache, not part of the type itself.
  mutable const UnresolvedUninitializedReferenceType* uninitialized_type_;
};

// Type representing the super-class of an unresolved type.
class UnresolvedSuperClassType final : public UnresolvedType {
 public:
  UnresolvedSuperClassType(Handle<mirror::Class> cls,
                           uint16_t child_id,
                           RegTypeCache* reg_type_cache,
                           uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : UnresolvedType(cls, "", cache_id, Kind::kUnresolvedSuperClass),
        unresolved_child_id_(child_id),
        reg_type_cache_(reg_type_cache) {
    CheckConstructorInvariants(this);
  }

  bool IsUnresolvedTypes() const override { return true; }

  uint16_t GetUnresolvedSuperClassChildId() const {
    DCHECK(IsUnresolvedSuperClass());
    return static_cast<uint16_t>(unresolved_child_id_ & 0xFFFF);
  }

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  const uint16_t unresolved_child_id_;
  const RegTypeCache* const reg_type_cache_;
};

// A merge of unresolved (and resolved) types. If the types were resolved this may be
// Conflict or another known ReferenceType.
class UnresolvedMergedReferenceType final : public UnresolvedType {
 public:
  // Note: the constructor will copy the unresolved BitVector, not use it directly.
  UnresolvedMergedReferenceType(const RegType& resolved,
                                const BitVector& unresolved,
                                const RegTypeCache* reg_type_cache,
                                uint16_t cache_id)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // The resolved part. See description below.
  const RegType& GetResolvedPart() const {
    return resolved_part_;
  }
  // The unresolved part.
  const BitVector& GetUnresolvedTypes() const {
    return unresolved_types_;
  }

  bool IsUnresolvedTypes() const override { return true; }

  bool IsArrayTypes() const override REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsObjectArrayTypes() const override REQUIRES_SHARED(Locks::mutator_lock_);

  std::string Dump() const override REQUIRES_SHARED(Locks::mutator_lock_);

  const RegTypeCache* GetRegTypeCache() const { return reg_type_cache_; }

 private:
  void CheckInvariants() const REQUIRES_SHARED(Locks::mutator_lock_) override;

  const RegTypeCache* const reg_type_cache_;

  // The original implementation of merged types was a binary tree. Collection of the flattened
  // types ("leaves") can be expensive, so we store the expanded list now, as two components:
  // 1) A resolved component. We use Zero when there is no resolved component, as that will be
  //    an identity merge.
  // 2) A bitvector of the unresolved reference types. A bitvector was chosen with the assumption
  //    that there should not be too many types in flight in practice. (We also bias the index
  //    against the index of Zero, which is one of the later default entries in any cache.)
  const RegType& resolved_part_;
  const BitVector unresolved_types_;
};

std::ostream& operator<<(std::ostream& os, const RegType& rhs)
    REQUIRES_SHARED(Locks::mutator_lock_);

}  // namespace verifier
}  // namespace art

#endif  // ART_RUNTIME_VERIFIER_REG_TYPE_H_
