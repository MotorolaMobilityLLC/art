/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_COMPILER_BACKEND_H_
#define ART_COMPILER_COMPILER_BACKEND_H_

#include "dex_file.h"
#include "os.h"

namespace art {

class Backend;
class CompilationUnit;
class CompilerDriver;
class CompiledMethod;
class MIRGraph;
class OatWriter;

namespace mirror {
  class ArtMethod;
}

class CompilerBackend {
 public:
  enum Kind {
    kQuick,
    kPortable
  };

  explicit CompilerBackend(int warning)
      : maximum_compilation_time_before_warning_(warning) {}

  static CompilerBackend* Create(Kind kind);

  virtual void Init(CompilerDriver& driver) const = 0;

  virtual void UnInit(CompilerDriver& driver) const = 0;

  virtual CompiledMethod* Compile(CompilerDriver& compiler,
                                  const DexFile::CodeItem* code_item,
                                  uint32_t access_flags,
                                  InvokeType invoke_type,
                                  uint16_t class_def_idx,
                                  uint32_t method_idx,
                                  jobject class_loader,
                                  const DexFile& dex_file) const = 0;

  virtual CompiledMethod* JniCompile(CompilerDriver& driver,
                                     uint32_t access_flags,
                                     uint32_t method_idx,
                                     const DexFile& dex_file) const = 0;

  virtual uintptr_t GetEntryPointOf(mirror::ArtMethod* method) const = 0;

  virtual bool WriteElf(art::File* file,
                        OatWriter& oat_writer,
                        const std::vector<const art::DexFile*>& dex_files,
                        const std::string& android_root,
                        bool is_host, const CompilerDriver& driver) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) = 0;

  virtual Backend* GetCodeGenerator(CompilationUnit* cu,
                                    void* compilation_unit) const = 0;

  uint64_t GetMaximumCompilationTimeBeforeWarning() const {
    return maximum_compilation_time_before_warning_;
  }

  virtual bool IsPortable() const { return false; }
  void SetBitcodeFileName(std::string const& filename) {
    UNUSED(filename);
  }

  virtual void InitCompilationUnit(CompilationUnit& cu) const = 0;

  virtual ~CompilerBackend() {}

 private:
  uint64_t maximum_compilation_time_before_warning_;

  DISALLOW_COPY_AND_ASSIGN(CompilerBackend);
};

}  // namespace art

#endif  // ART_COMPILER_COMPILER_BACKEND_H_
