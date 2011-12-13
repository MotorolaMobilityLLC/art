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

#ifndef ART_SRC_LOGGING_H_
#define ART_SRC_LOGGING_H_

#include <cerrno>
#include <cstring>
#include <iostream>  // NOLINT
#include <sstream>
#include "log_severity.h"
#include "macros.h"

#define CHECK(x) \
  if (UNLIKELY(!(x))) \
    ::art::LogMessage(__FILE__, __LINE__, FATAL, -1).stream() \
        << "Check failed: " #x << " "

#define CHECK_OP(LHS, RHS, OP) \
  for (::art::EagerEvaluator<typeof(LHS), typeof(RHS)> _values(LHS, RHS); \
       UNLIKELY(!(_values.lhs OP _values.rhs)); /* empty */) \
    ::art::LogMessage(__FILE__, __LINE__, FATAL, -1).stream() \
        << "Check failed: " << #LHS << " " << #OP << " " << #RHS \
        << " (" #LHS "=" << _values.lhs << ", " #RHS "=" << _values.rhs << ") "

#define CHECK_EQ(x, y) CHECK_OP(x, y, ==)
#define CHECK_NE(x, y) CHECK_OP(x, y, !=)
#define CHECK_LE(x, y) CHECK_OP(x, y, <=)
#define CHECK_LT(x, y) CHECK_OP(x, y, <)
#define CHECK_GE(x, y) CHECK_OP(x, y, >=)
#define CHECK_GT(x, y) CHECK_OP(x, y, >)

#define CHECK_STROP(s1, s2, sense) \
  if (UNLIKELY((strcmp(s1, s2) == 0) != sense)) \
    LOG(FATAL) << "Check failed: " \
               << "\"" << s1 << "\"" \
               << (sense ? " == " : " != ") \
               << "\"" << s2 << "\""

#define CHECK_STREQ(s1, s2) CHECK_STROP(s1, s2, true)
#define CHECK_STRNE(s1, s2) CHECK_STROP(s1, s2, false)

#define CHECK_PTHREAD_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (rc != 0) { \
      errno = rc; \
      PLOG(FATAL) << # call << " failed for " << what; \
    } \
  } while (false)

#ifndef NDEBUG

#define DCHECK(x) CHECK(x)
#define DCHECK_EQ(x, y) CHECK_EQ(x, y)
#define DCHECK_NE(x, y) CHECK_NE(x, y)
#define DCHECK_LE(x, y) CHECK_LE(x, y)
#define DCHECK_LT(x, y) CHECK_LT(x, y)
#define DCHECK_GE(x, y) CHECK_GE(x, y)
#define DCHECK_GT(x, y) CHECK_GT(x, y)
#define DCHECK_STREQ(s1, s2) CHECK_STREQ(s1, s2)
#define DCHECK_STRNE(s1, s2) CHECK_STRNE(s1, s2)

#else  // NDEBUG

#define DCHECK(condition) \
  while (false) \
    CHECK(condition)

#define DCHECK_EQ(val1, val2) \
  while (false) \
    CHECK_EQ(val1, val2)

#define DCHECK_NE(val1, val2) \
  while (false) \
    CHECK_NE(val1, val2)

#define DCHECK_LE(val1, val2) \
  while (false) \
    CHECK_LE(val1, val2)

#define DCHECK_LT(val1, val2) \
  while (false) \
    CHECK_LT(val1, val2)

#define DCHECK_GE(val1, val2) \
  while (false) \
    CHECK_GE(val1, val2)

#define DCHECK_GT(val1, val2) \
  while (false) \
    CHECK_GT(val1, val2)

#define DCHECK_STREQ(str1, str2) \
  while (false) \
    CHECK_STREQ(str1, str2)

#define DCHECK_STRNE(str1, str2) \
  while (false) \
    CHECK_STRNE(str1, str2)

#endif

#define LOG(severity) ::art::LogMessage(__FILE__, __LINE__, severity, -1).stream()
#define PLOG(severity) ::art::LogMessage(__FILE__, __LINE__, severity, errno).stream()

#define LG LOG(INFO)

#define UNIMPLEMENTED(level) LOG(level) << __PRETTY_FUNCTION__ << " unimplemented "

#define VLOG_IS_ON(module) UNLIKELY(::art::gLogVerbosity.module)
#define VLOG(module) if (VLOG_IS_ON(module)) ::art::LogMessage(__FILE__, __LINE__, INFO, -1).stream()

//
// Implementation details beyond this point.
//

namespace art {

template <typename LHS, typename RHS>
struct EagerEvaluator {
  EagerEvaluator(LHS lhs, RHS rhs) : lhs(lhs), rhs(rhs) { }
  LHS lhs;
  RHS rhs;
};

// This indirection greatly reduces the stack impact of having
// lots of checks/logging in a function.
struct LogMessageData {
 public:
  LogMessageData(int line, LogSeverity severity, int error)
      : file(NULL),
        line_number(line),
        severity(severity),
        error(error) {
  }

  std::ostringstream buffer;
  const char* file;
  int line_number;
  LogSeverity severity;
  int error;

 private:
  DISALLOW_COPY_AND_ASSIGN(LogMessageData);
};

class LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity, int error);
  ~LogMessage();
  std::ostream& stream();

 private:
  void LogLine(const char*);

  LogMessageData* data_;

  DISALLOW_COPY_AND_ASSIGN(LogMessage);
};

void HexDump(const void* address, size_t byte_count, bool show_actual_address = false);

// A convenience to allow any class with a "Dump(std::ostream& os)" member function
// but without an operator<< to be used as if it had an operator<<. Use like this:
//
//   os << Dumpable<MyType>(my_type_instance);
//
template<typename T>
class Dumpable {
 public:
  explicit Dumpable(T& value) : value_(value) {
  }

  void Dump(std::ostream& os) const {
    value_.Dump(os);
  }

 private:
  T& value_;
  DISALLOW_COPY_AND_ASSIGN(Dumpable);
};

template<typename T>
std::ostream& operator<<(std::ostream& os, const Dumpable<T>& rhs) {
  rhs.Dump(os);
  return os;
}

// The members of this struct are the valid arguments to VLOG and VLOG_IS_ON in code,
// and the "-verbose:" command line argument.
struct LogVerbosity {
  bool class_linker; // Enabled with "-verbose:class".
  bool compiler;
  bool heap;
  bool gc;
  bool jdwp;
  bool jni;
  bool monitor;
  bool startup;
  bool third_party_jni; // Enabled with "-verbose:third-party-jni".
  bool threads;
};

extern LogVerbosity gLogVerbosity;

}  // namespace art

#endif  // ART_SRC_LOGGING_H_
