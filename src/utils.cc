// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "file.h"
#include "object.h"
#include "os.h"
#include "utils.h"

namespace art {

std::string ReadFileToString(const char* file_name) {
  scoped_ptr<File> file(OS::OpenFile(file_name, false));
  CHECK(file != NULL);

  std::string contents;
  char buf[8 * KB];
  while (true) {
    int64_t n = file->Read(buf, sizeof(buf));
    CHECK_NE(-1, n);
    if (n == 0) {
        break;
    }
    contents.append(buf, n);
  }
  return contents;
}

std::string PrettyDescriptor(const String* java_descriptor) {
  std::string descriptor(java_descriptor->ToModifiedUtf8());

  // Count the number of '['s to get the dimensionality.
  const char* c = descriptor.c_str();
  size_t dim = 0;
  while (*c == '[') {
    dim++;
    c++;
  }

  // Reference or primitive?
  if (*c == 'L') {
    // "[[La/b/C;" -> "a.b.C[][]".
    c++; // Skip the 'L'.
  } else {
    // "[[B" -> "byte[][]".
    // To make life easier, we make primitives look like unqualified
    // reference types.
    switch (*c) {
    case 'B': c = "byte;"; break;
    case 'C': c = "char;"; break;
    case 'D': c = "double;"; break;
    case 'F': c = "float;"; break;
    case 'I': c = "int;"; break;
    case 'J': c = "long;"; break;
    case 'S': c = "short;"; break;
    case 'Z': c = "boolean;"; break;
    default: return descriptor;
    }
  }

  // At this point, 'c' is a string of the form "fully/qualified/Type;"
  // or "primitive;". Rewrite the type with '.' instead of '/':
  std::string result;
  const char* p = c;
  while (*p != ';') {
    char ch = *p++;
    if (ch == '/') {
      ch = '.';
    }
    result.push_back(ch);
  }
  // ...and replace the semicolon with 'dim' "[]" pairs:
  while (dim--) {
    result += "[]";
  }
  return result;
}

std::string PrettyMethod(const Method* m, bool with_signature) {
  if (m == NULL) {
    return "null";
  }
  Class* c = m->GetDeclaringClass();
  std::string result(PrettyDescriptor(c->GetDescriptor()));
  result += '.';
  result += m->GetName()->ToModifiedUtf8();
  if (with_signature) {
    // TODO: iterate over the signature's elements and pass them all to
    // PrettyDescriptor? We'd need to pull out the return type specially, too.
    result += m->GetSignature()->ToModifiedUtf8();
  }
  return result;
}

std::string PrettyType(const Object* obj) {
  if (obj == NULL) {
    return "null";
  }
  if (obj->GetClass() == NULL) {
    return "(raw)";
  }
  std::string result(PrettyDescriptor(obj->GetClass()->GetDescriptor()));
  if (obj->IsClass()) {
    result += "<" + PrettyDescriptor(obj->AsClass()->GetDescriptor()) + ">";
  }
  return result;
}

}  // namespace art
