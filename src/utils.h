// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_UTILS_H_
#define ART_SRC_UTILS_H_

#include "globals.h"
#include "logging.h"
#include "primitive.h"
#include "stringpiece.h"
#include "stringprintf.h"

#include <pthread.h>
#include <string>
#include <vector>

namespace art {

class Class;
class DexFile;
class Field;
class Method;
class Object;
class String;

template<typename T>
static inline bool IsPowerOfTwo(T x) {
  return (x & (x - 1)) == 0;
}

template<int n, typename T>
static inline bool IsAligned(T x) {
  COMPILE_ASSERT((n & (n - 1)) == 0, n_not_power_of_two);
  return (x & (n - 1)) == 0;
}

template<int n, typename T>
static inline bool IsAligned(T* x) {
  return IsAligned<n>(reinterpret_cast<const uintptr_t>(x));
}

#define CHECK_ALIGNED(value, alignment) \
  CHECK(::art::IsAligned<alignment>(value)) << reinterpret_cast<const void*>(value)

#define DCHECK_ALIGNED(value, alignment) \
  DCHECK(::art::IsAligned<alignment>(value)) << reinterpret_cast<const void*>(value)

// Check whether an N-bit two's-complement representation can hold value.
static inline bool IsInt(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << (N - 1);
  return (-limit <= value) && (value < limit);
}

static inline bool IsUint(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  word limit = static_cast<word>(1) << N;
  return (0 <= value) && (value < limit);
}

static inline bool IsAbsoluteUint(int N, word value) {
  CHECK_LT(0, N);
  CHECK_LT(N, kBitsPerWord);
  if (value < 0) value = -value;
  return IsUint(N, value);
}

static inline int32_t Low16Bits(int32_t value) {
  return static_cast<int32_t>(value & 0xffff);
}

static inline int32_t High16Bits(int32_t value) {
  return static_cast<int32_t>(value >> 16);
}

static inline int32_t Low32Bits(int64_t value) {
  return static_cast<int32_t>(value);
}

static inline int32_t High32Bits(int64_t value) {
  return static_cast<int32_t>(value >> 32);
}

template<typename T>
static inline T RoundDown(T x, int n) {
  CHECK(IsPowerOfTwo(n));
  return (x & -n);
}

template<typename T>
static inline T RoundUp(T x, int n) {
  return RoundDown(x + n - 1, n);
}

// Implementation is from "Hacker's Delight" by Henry S. Warren, Jr.,
// figure 3-3, page 48, where the function is called clp2.
static inline uint32_t RoundUpToPowerOfTwo(uint32_t x) {
  x = x - 1;
  x = x | (x >> 1);
  x = x | (x >> 2);
  x = x | (x >> 4);
  x = x | (x >> 8);
  x = x | (x >> 16);
  return x + 1;
}

// Implementation is from "Hacker's Delight" by Henry S. Warren, Jr.,
// figure 5-2, page 66, where the function is called pop.
static inline int CountOneBits(uint32_t x) {
  x = x - ((x >> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  x = (x + (x >> 4)) & 0x0F0F0F0F;
  x = x + (x >> 8);
  x = x + (x >> 16);
  return static_cast<int>(x & 0x0000003F);
}

#define CLZ(x) __builtin_clz(x)

static inline bool NeedsEscaping(uint16_t ch) {
  return (ch < ' ' || ch > '~');
}

static inline std::string PrintableChar(uint16_t ch) {
  std::string result;
  result += '\'';
  if (NeedsEscaping(ch)) {
    StringAppendF(&result, "\\u%04x", ch);
  } else {
    result += ch;
  }
  result += '\'';
  return result;
}

// TODO: assume the content is UTF-8, and show code point escapes?
template<typename StringT>
static inline std::string PrintableString(const StringT& s) {
  std::string result;
  result += '"';
  for (typename StringT::const_iterator it = s.begin(); it != s.end(); ++it) {
    char ch = *it;
    if (NeedsEscaping(ch)) {
      StringAppendF(&result, "\\x%02x", ch & 0xff);
    } else {
      result += ch;
    }
  }
  result += '"';
  return result;
}

// Used to implement PrettyClass, PrettyField, PrettyMethod, and PrettyTypeOf,
// one of which is probably more useful to you.
// Returns a human-readable equivalent of 'descriptor'. So "I" would be "int",
// "[[I" would be "int[][]", "[Ljava/lang/String;" would be
// "java.lang.String[]", and so forth.
std::string PrettyDescriptor(const String* descriptor);
std::string PrettyDescriptor(const std::string& descriptor);
std::string PrettyDescriptor(Primitive::Type type);
std::string PrettyDescriptor(const Class* klass);

// Returns a human-readable signature for 'f'. Something like "a.b.C.f" or
// "int a.b.C.f" (depending on the value of 'with_type').
std::string PrettyField(const Field* f, bool with_type = true);

// Returns a human-readable signature for 'm'. Something like "a.b.C.m" or
// "a.b.C.m(II)V" (depending on the value of 'with_signature').
std::string PrettyMethod(const Method* m, bool with_signature = true);
std::string PrettyMethod(uint32_t method_idx, const DexFile& dex_file, bool with_signature = true);

// Returns a human-readable form of the name of the *class* of the given object.
// So given an instance of java.lang.String, the output would
// be "java.lang.String". Given an array of int, the output would be "int[]".
// Given String.class, the output would be "java.lang.Class<java.lang.String>".
std::string PrettyTypeOf(const Object* obj);

// Returns a human-readable form of the name of the given class.
// Given String.class, the output would be "java.lang.Class<java.lang.String>".
std::string PrettyClass(const Class* c);

// Returns a human-readable form of the name of the given class with its class loader.
std::string PrettyClassAndClassLoader(const Class* c);

// Performs JNI name mangling as described in section 11.3 "Linking Native Methods"
// of the JNI spec.
std::string MangleForJni(const std::string& s);

// Turn "java.lang.String" into "Ljava/lang/String;".
std::string DotToDescriptor(const char* class_name);

// Turn "Ljava/lang/String;" into "java.lang.String".
std::string DescriptorToDot(const StringPiece& descriptor);

// Tests for whether 's' is a valid class name in the three common forms:
bool IsValidBinaryClassName(const char* s);  // "java.lang.String"
bool IsValidJniClassName(const char* s);     // "java/lang/String"
bool IsValidDescriptor(const char* s);       // "Ljava/lang/String;"

// Returns whether the given string is a valid field or method name,
// additionally allowing names that begin with '<' and end with '>'.
bool IsValidMemberName(const char* s);

// Returns the JNI native function name for the non-overloaded method 'm'.
std::string JniShortName(const Method* m);
// Returns the JNI native function name for the overloaded method 'm'.
std::string JniLongName(const Method* m);

bool ReadFileToString(const std::string& file_name, std::string* result);

// Returns the current date in ISO yyyy-mm-dd hh:mm:ss format.
std::string GetIsoDate();

// Returns the current time in milliseconds (using the POSIX CLOCK_MONOTONIC).
uint64_t MilliTime();

// Returns the current time in microseconds (using the POSIX CLOCK_MONOTONIC).
uint64_t MicroTime();

// Returns the current time in nanoseconds (using the POSIX CLOCK_MONOTONIC).
uint64_t NanoTime();

// Returns the current time in microseconds (using the POSIX CLOCK_THREAD_CPUTIME_ID).
uint64_t ThreadCpuMicroTime();

// Converts the given number of nanoseconds to milliseconds.
uint64_t NsToMs(uint64_t ns);

// Splits a string using the given delimiter character into a vector of
// strings. Empty strings will be omitted.
void Split(const std::string& s, char delim, std::vector<std::string>& result);

// Returns the calling thread's tid. (The C libraries don't expose this.)
pid_t GetTid();

// Reads data from "/proc/self/task/${tid}/stat".
void GetTaskStats(pid_t tid, int& utime, int& stime, int& task_cpu);

// Sets the name of the current thread. The name may be truncated to an
// implementation-defined limit.
void SetThreadName(const char* name);

// Returns the art-cache location, or dies trying.
std::string GetArtCacheOrDie();

// Returns the art-cache location for a DexFile or OatFile, or dies trying.
std::string GetArtCacheFilenameOrDie(const std::string& location);

// Check whether the given filename has a valid zip or dex extension
bool IsValidZipFilename(const std::string& filename);
bool IsValidDexFilename(const std::string& filename);

}  // namespace art

#endif  // ART_SRC_UTILS_H_
