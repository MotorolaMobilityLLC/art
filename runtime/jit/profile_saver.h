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

#ifndef ART_RUNTIME_JIT_PROFILE_SAVER_H_
#define ART_RUNTIME_JIT_PROFILE_SAVER_H_

#include "base/mutex.h"
#include "jit_code_cache.h"
#include "offline_profiling_info.h"
#include "safe_map.h"

namespace art {

class ProfileSaver {
 public:
  // Starts the profile saver thread if not already started.
  // If the saver is already running it adds (output_filename, code_paths) to its tracked locations.
  static void Start(const std::string& output_filename,
                    jit::JitCodeCache* jit_code_cache,
                    const std::vector<std::string>& code_paths,
                    const std::string& foreign_dex_profile_path,
                    const std::string& app_data_dir)
      REQUIRES(!Locks::profiler_lock_, !wait_lock_);

  // Stops the profile saver thread.
  // NO_THREAD_SAFETY_ANALYSIS for static function calling into member function with excludes lock.
  static void Stop(bool dump_info_)
      REQUIRES(!Locks::profiler_lock_, !wait_lock_)
      NO_THREAD_SAFETY_ANALYSIS;

  // Returns true if the profile saver is started.
  static bool IsStarted() REQUIRES(!Locks::profiler_lock_);

  static void NotifyDexUse(const std::string& dex_location);

  // If the profile saver is running, dumps statistics to the `os`. Otherwise it does nothing.
  static void DumpInstanceInfo(std::ostream& os);

 private:
  ProfileSaver(const std::string& output_filename,
               jit::JitCodeCache* jit_code_cache,
               const std::vector<std::string>& code_paths,
               const std::string& foreign_dex_profile_path,
               const std::string& app_data_dir);

  // NO_THREAD_SAFETY_ANALYSIS for static function calling into member function with excludes lock.
  static void* RunProfileSaverThread(void* arg)
      REQUIRES(!Locks::profiler_lock_, !wait_lock_)
      NO_THREAD_SAFETY_ANALYSIS;

  // The run loop for the saver.
  void Run() REQUIRES(!Locks::profiler_lock_, !wait_lock_);
  // Processes the existing profiling info from the jit code cache and returns
  // true if it needed to be saved to disk.
  bool ProcessProfilingInfo();
  // Returns true if the saver is shutting down (ProfileSaver::Stop() has been called).
  bool ShuttingDown(Thread* self) REQUIRES(!Locks::profiler_lock_);

  void AddTrackedLocations(const std::string& output_filename,
                           const std::vector<std::string>& code_paths)
      REQUIRES(Locks::profiler_lock_);

  // Retrieves the cached profile compilation info for the given profile file.
  // If no entry exists, a new empty one will be created, added to the cache and
  // then returned.
  ProfileCompilationInfo* GetCachedProfiledInfo(const std::string& filename);
  // Fetches the current resolved classes from the ClassLinker and stores them
  // in the profile_cache_ for later save.
  void FetchAndCacheResolvedClasses();

  static bool MaybeRecordDexUseInternal(
      const std::string& dex_location,
      const std::set<std::string>& tracked_locations,
      const std::string& foreign_dex_profile_path,
      const std::string& app_data_dir);

  void DumpInfo(std::ostream& os);

  // The only instance of the saver.
  static ProfileSaver* instance_ GUARDED_BY(Locks::profiler_lock_);
  // Profile saver thread.
  static pthread_t profiler_pthread_ GUARDED_BY(Locks::profiler_lock_);

  jit::JitCodeCache* jit_code_cache_;
  SafeMap<std::string, std::set<std::string>> tracked_dex_base_locations_
      GUARDED_BY(Locks::profiler_lock_);
  std::string foreign_dex_profile_path_;
  std::string app_data_dir_;
  bool shutting_down_ GUARDED_BY(Locks::profiler_lock_);
  uint32_t last_save_number_of_methods_;
  uint32_t last_save_number_of_classes_;

  // A local cache for the profile information. Maps each tracked file to its
  // profile information. The size of this cache is usually very small and tops
  // to just a few hundreds entries in the ProfileCompilationInfo objects.
  // It helps avoiding unnecessary writes to disk.
  SafeMap<std::string, ProfileCompilationInfo> profile_cache_;

  // Save period condition support.
  Mutex wait_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ConditionVariable period_condition_ GUARDED_BY(wait_lock_);

  uint64_t total_bytes_written_;
  uint64_t total_number_of_writes_;
  uint64_t total_number_of_code_cache_queries_;
  uint64_t total_number_of_skipped_writes_;
  uint64_t total_number_of_failed_writes_;
  uint64_t total_ms_of_sleep_;
  uint64_t total_ns_of_work_;
  uint64_t total_number_of_foreign_dex_marks_;
  // TODO(calin): replace with an actual size.
  uint64_t max_number_of_profile_entries_cached_;

  DISALLOW_COPY_AND_ASSIGN(ProfileSaver);
};

}  // namespace art

#endif  // ART_RUNTIME_JIT_PROFILE_SAVER_H_
