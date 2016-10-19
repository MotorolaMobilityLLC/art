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

#ifndef ART_RUNTIME_MIRROR_STRING_H_
#define ART_RUNTIME_MIRROR_STRING_H_

#include "gc_root.h"
#include "gc/allocator_type.h"
#include "object.h"
#include "object_callbacks.h"

namespace art {

template<class T> class Handle;
struct StringOffsets;
class StringPiece;
class StubTest_ReadBarrierForRoot_Test;

namespace mirror {

// String Compression
static constexpr bool kUseStringCompression = false;

// C++ mirror of java.lang.String
class MANAGED String FINAL : public Object {
 public:
  // Size of java.lang.String.class.
  static uint32_t ClassSize(PointerSize pointer_size);

  // Size of an instance of java.lang.String not including its value array.
  static constexpr uint32_t InstanceSize() {
    return sizeof(String);
  }

  static MemberOffset CountOffset() {
    return OFFSET_OF_OBJECT_MEMBER(String, count_);
  }

  static MemberOffset ValueOffset() {
    return OFFSET_OF_OBJECT_MEMBER(String, value_);
  }

  uint16_t* GetValue() REQUIRES_SHARED(Locks::mutator_lock_) {
    return &value_[0];
  }

  uint8_t* GetValueCompressed() REQUIRES_SHARED(Locks::mutator_lock_) {
    return &value_compressed_[0];
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  size_t SizeOf() REQUIRES_SHARED(Locks::mutator_lock_);

  // Taking out the first/uppermost bit because it is not part of actual length value
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  int32_t GetLength() REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetLengthFromCount(GetCount<kVerifyFlags>());
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  int32_t GetCount() REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetField32<kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(String, count_));
  }

  void SetCount(int32_t new_count) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Count is invariant so use non-transactional mode. Also disable check as we may run inside
    // a transaction.
    DCHECK_LE(0, (new_count & INT32_MAX));
    SetField32<false, false>(OFFSET_OF_OBJECT_MEMBER(String, count_), new_count);
  }

  int32_t GetHashCode() REQUIRES_SHARED(Locks::mutator_lock_);

  // Computes, stores, and returns the hash code.
  int32_t ComputeHashCode() REQUIRES_SHARED(Locks::mutator_lock_);

  int32_t GetUtfLength() REQUIRES_SHARED(Locks::mutator_lock_);

  uint16_t CharAt(int32_t index) REQUIRES_SHARED(Locks::mutator_lock_);

  void SetCharAt(int32_t index, uint16_t c) REQUIRES_SHARED(Locks::mutator_lock_);

  String* Intern() REQUIRES_SHARED(Locks::mutator_lock_);

  template <bool kIsInstrumented>
  ALWAYS_INLINE static String* AllocFromByteArray(Thread* self, int32_t byte_length,
                                                  Handle<ByteArray> array, int32_t offset,
                                                  int32_t high_byte,
                                                  gc::AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  template <bool kIsInstrumented>
  ALWAYS_INLINE static String* AllocFromCharArray(Thread* self, int32_t count,
                                                  Handle<CharArray> array, int32_t offset,
                                                  gc::AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  template <bool kIsInstrumented>
  ALWAYS_INLINE static String* AllocFromString(Thread* self, int32_t string_length,
                                               Handle<String> string, int32_t offset,
                                               gc::AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  template <bool kIsInstrumented>
  ALWAYS_INLINE static String* AllocEmptyString(Thread* self,
                                                gc::AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static String* AllocFromStrings(Thread* self, Handle<String> string, Handle<String> string2)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static String* AllocFromUtf16(Thread* self, int32_t utf16_length, const uint16_t* utf16_data_in)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static String* AllocFromModifiedUtf8(Thread* self, const char* utf)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static String* AllocFromModifiedUtf8(Thread* self, int32_t utf16_length,
                                       const char* utf8_data_in, int32_t utf8_length)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static String* AllocFromModifiedUtf8(Thread* self, int32_t utf16_length, const char* utf8_data_in)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  // TODO: This is only used in the interpreter to compare against
  // entries from a dex files constant pool (ArtField names). Should
  // we unify this with Equals(const StringPiece&); ?
  bool Equals(const char* modified_utf8) REQUIRES_SHARED(Locks::mutator_lock_);

  // TODO: This is only used to compare DexCache.location with
  // a dex_file's location (which is an std::string). Do we really
  // need this in mirror::String just for that one usage ?
  bool Equals(const StringPiece& modified_utf8)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool Equals(ObjPtr<String> that) REQUIRES_SHARED(Locks::mutator_lock_);

  // Compare UTF-16 code point values not in a locale-sensitive manner
  int Compare(int32_t utf16_length, const char* utf8_data_in);

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const uint16_t* that_chars, int32_t that_offset,
              int32_t that_length)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Create a modified UTF-8 encoded std::string from a java/lang/String object.
  std::string ToModifiedUtf8() REQUIRES_SHARED(Locks::mutator_lock_);

  int32_t FastIndexOf(int32_t ch, int32_t start) REQUIRES_SHARED(Locks::mutator_lock_);

  template <typename MemoryType>
  int32_t FastIndexOf(MemoryType* chars, int32_t ch, int32_t start)
      REQUIRES_SHARED(Locks::mutator_lock_);

  int32_t CompareTo(ObjPtr<String> other) REQUIRES_SHARED(Locks::mutator_lock_);

  CharArray* ToCharArray(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  void GetChars(int32_t start, int32_t end, Handle<CharArray> array, int32_t index)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsCompressed() REQUIRES_SHARED(Locks::mutator_lock_) {
    return kUseStringCompression && GetCompressionFlagFromCount(GetCount());
  }

  bool IsValueNull() REQUIRES_SHARED(Locks::mutator_lock_);

  template<typename MemoryType>
  static bool AllASCII(const MemoryType* const chars, const int length);

  ALWAYS_INLINE static bool GetCompressionFlagFromCount(const int32_t count) {
    return kUseStringCompression && ((count & (1u << 31)) != 0);
  }

  ALWAYS_INLINE static int32_t GetLengthFromCount(const int32_t count) {
    return kUseStringCompression ? (count & INT32_MAX) : count;
  }

  ALWAYS_INLINE static int32_t GetFlaggedCount(const int32_t count) {
    return kUseStringCompression ? (count | (1u << 31)) : count;
  }

  static Class* GetJavaLangString() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!java_lang_String_.IsNull());
    return java_lang_String_.Read();
  }

  static void SetClass(ObjPtr<Class> java_lang_String) REQUIRES_SHARED(Locks::mutator_lock_);
  static void ResetClass() REQUIRES_SHARED(Locks::mutator_lock_);
  static void VisitRoots(RootVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns a human-readable equivalent of 'descriptor'. So "I" would be "int",
  // "[[I" would be "int[][]", "[Ljava/lang/String;" would be
  // "java.lang.String[]", and so forth.
  static std::string PrettyStringDescriptor(ObjPtr<mirror::String> descriptor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string PrettyStringDescriptor()
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  void SetHashCode(int32_t new_hash_code) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Hash code is invariant so use non-transactional mode. Also disable check as we may run inside
    // a transaction.
    DCHECK_EQ(0, GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_)));
    SetField32<false, false>(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), new_hash_code);
  }

  template <bool kIsInstrumented, typename PreFenceVisitor>
  ALWAYS_INLINE static String* Alloc(Thread* self, int32_t utf16_length_with_flag,
                                     gc::AllocatorType allocator_type,
                                     const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // First bit (uppermost/leftmost) is taken out for Compressed/Uncompressed flag
  // [0] Uncompressed: string uses 16-bit memory | [1] Compressed: 8-bit memory
  int32_t count_;

  uint32_t hash_code_;

  // Compression of all-ASCII into 8-bit memory leads to usage one of these fields
  union {
    uint16_t value_[0];
    uint8_t value_compressed_[0];
  };

  static GcRoot<Class> java_lang_String_;

  friend struct art::StringOffsets;  // for verifying offset information
  ART_FRIEND_TEST(art::StubTest, ReadBarrierForRoot);  // For java_lang_String_.

  DISALLOW_IMPLICIT_CONSTRUCTORS(String);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_STRING_H_
