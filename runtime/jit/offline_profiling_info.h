/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_
#define ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_

#include <set>

#include "atomic.h"
#include "dex_file.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

class ArtMethod;

/**
 * Profiling information in a format that can be serialized to disk.
 * It is a serialize-friendly format based on information collected
 * by the interpreter (ProfileInfo).
 * Currently it stores only the hot compiled methods.
 */
class OfflineProfilingInfo {
 public:
  bool NeedsSaving(uint64_t last_update_time_ns) const;
  void SaveProfilingInfo(const std::string& filename,
                         uint64_t last_update_time_ns,
                         const std::set<ArtMethod*>& methods);
  void SetTrackedDexLocations(const std::vector<std::string>& dex_locations);
  const std::set<const std::string>& GetTrackedDexLocations() const;

 private:
  // Map identifying the location of the profiled methods.
  // dex_file_ -> [dex_method_index]+
  using DexFileToMethodsMap = SafeMap<const DexFile*, std::set<uint32_t>>;

  void AddMethodInfo(ArtMethod* method, DexFileToMethodsMap* info)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool Serialize(const std::string& filename, const DexFileToMethodsMap& info) const;

  // TODO(calin): Verify if Atomic is really needed (are we sure to be called from a
  // single thread?)
  Atomic<uint64_t> last_update_time_ns_;

  std::set<const std::string> tracked_dex_base_locations_;
};

/**
 * Profile information in a format suitable to be queried by the compiler and performing
 * profile guided compilation.
 */
class ProfileCompilationInfo {
 public:
  // Constructs a ProfileCompilationInfo backed by the provided file.
  explicit ProfileCompilationInfo(const std::string& filename) : filename_(filename) {}

  // Loads profile information corresponding to the provided dex files.
  // The dex files' multidex suffixes must be unique.
  // This resets the state of the profiling information
  // (i.e. all previously loaded info are cleared).
  bool Load(const std::vector<const DexFile*>& dex_files);

  // Returns true if the method reference is present in the profiling info.
  bool ContainsMethod(const MethodReference& method_ref) const;

  const std::string& GetFilename() const { return filename_; }

  // Dumps all the loaded profile info into a string and returns it.
  // This is intended for testing and debugging.
  std::string DumpInfo(bool print_full_dex_location = true) const;

 private:
  bool ProcessLine(const std::string& line,
                   const std::vector<const DexFile*>& dex_files);

  using ClassToMethodsMap = SafeMap<uint32_t, std::set<uint32_t>>;
  // Map identifying the location of the profiled methods.
  // dex_file -> class_index -> [dex_method_index]+
  using DexFileToProfileInfoMap = SafeMap<const DexFile*, ClassToMethodsMap>;

  const std::string filename_;
  DexFileToProfileInfoMap info_;
};

}  // namespace art

#endif  // ART_RUNTIME_JIT_OFFLINE_PROFILING_INFO_H_
