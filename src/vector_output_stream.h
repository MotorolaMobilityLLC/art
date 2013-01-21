/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_SRC_VECTOR_OUTPUT_STREAM_H_
#define ART_SRC_VECTOR_OUTPUT_STREAM_H_

#include "output_stream.h"

#include <string>
#include <vector>

namespace art {

class VectorOutputStream : public OutputStream {

 public:
  VectorOutputStream(const std::string& location, std::vector<uint8_t>& vector);

  virtual ~VectorOutputStream() {};

  virtual bool WriteFully(const void* buffer, int64_t byte_count);

  virtual off_t lseek(off_t offset, int whence);

 private:
  void EnsureCapacity(off_t new_offset);

  off_t offset_;
  std::vector<uint8_t>& vector_;

  DISALLOW_COPY_AND_ASSIGN(VectorOutputStream);
};

}  // namespace art

#endif  // ART_SRC_VECTOR_OUTPUT_STREAM_H_
