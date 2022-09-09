/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "artd.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "aidl/com/android/server/art/BnArtd.h"
#include "aidl/com/android/server/art/DexoptTrigger.h"
#include "android-base/errors.h"
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/result.h"
#include "android-base/scopeguard.h"
#include "android-base/strings.h"
#include "android/binder_auto_utils.h"
#include "android/binder_manager.h"
#include "android/binder_process.h"
#include "base/compiler_filter.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/os.h"
#include "exec_utils.h"
#include "file_utils.h"
#include "fmt/format.h"
#include "oat_file_assistant.h"
#include "oat_file_assistant_context.h"
#include "path_utils.h"
#include "profman/profman_result.h"
#include "selinux/android.h"
#include "tools/cmdline_builder.h"
#include "tools/tools.h"

namespace art {
namespace artd {

namespace {

using ::aidl::com::android::server::art::ArtifactsPath;
using ::aidl::com::android::server::art::DexoptOptions;
using ::aidl::com::android::server::art::DexoptResult;
using ::aidl::com::android::server::art::DexoptTrigger;
using ::aidl::com::android::server::art::FileVisibility;
using ::aidl::com::android::server::art::FsPermission;
using ::aidl::com::android::server::art::GetDexoptNeededResult;
using ::aidl::com::android::server::art::GetOptimizationStatusResult;
using ::aidl::com::android::server::art::OutputArtifacts;
using ::aidl::com::android::server::art::OutputProfile;
using ::aidl::com::android::server::art::PriorityClass;
using ::aidl::com::android::server::art::ProfilePath;
using ::aidl::com::android::server::art::VdexPath;
using ::android::base::Dirname;
using ::android::base::Error;
using ::android::base::Join;
using ::android::base::make_scope_guard;
using ::android::base::ReadFileToString;
using ::android::base::Result;
using ::android::base::Split;
using ::android::base::StringReplace;
using ::android::base::WriteStringToFd;
using ::art::tools::CmdlineBuilder;
using ::ndk::ScopedAStatus;

using ::fmt::literals::operator""_format;  // NOLINT

using ArtifactsLocation = GetDexoptNeededResult::ArtifactsLocation;
using TmpRefProfilePath = ProfilePath::TmpRefProfilePath;

constexpr const char* kServiceName = "artd";

// Timeout for short operations, such as merging profiles.
constexpr int kShortTimeoutSec = 60;  // 1 minute.

// Timeout for long operations, such as compilation. We set it to be smaller than the Package
// Manager watchdog (PackageManagerService.WATCHDOG_TIMEOUT, 10 minutes), so that if the operation
// is called from the Package Manager's thread handler, it will be aborted before that watchdog
// would take down the system server.
constexpr int kLongTimeoutSec = 570;  // 9.5 minutes.

// Deletes a file. Returns the size of the deleted file, or 0 if the deleted file is empty or an
// error occurs.
int64_t GetSizeAndDeleteFile(const std::string& path) {
  std::error_code ec;
  int64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    // It is okay if the file does not exist. We don't have to log it.
    if (ec.value() != ENOENT) {
      LOG(ERROR) << "Failed to get the file size of '{}': {}"_format(path, ec.message());
    }
    return 0;
  }

  if (!std::filesystem::remove(path, ec)) {
    LOG(ERROR) << "Failed to remove '{}': {}"_format(path, ec.message());
    return 0;
  }

  return size;
}

std::string EscapeErrorMessage(const std::string& message) {
  return StringReplace(message, std::string("\0", /*n=*/1), "\\0", /*all=*/true);
}

// Indicates an error that should never happen (e.g., illegal arguments passed by service-art
// internally). System server should crash if this kind of error happens.
ScopedAStatus Fatal(const std::string& message) {
  return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_STATE,
                                                     EscapeErrorMessage(message).c_str());
}

// Indicates an error that service-art should handle (e.g., I/O errors, sub-process crashes).
// The scope of the error depends on the function that throws it, so service-art should catch the
// error at every call site and take different actions.
// Ideally, this should be a checked exception or an additional return value that forces service-art
// to handle it, but `ServiceSpecificException` (a separate runtime exception type) is the best
// approximate we have given the limitation of Java and Binder.
ScopedAStatus NonFatal(const std::string& message) {
  constexpr int32_t kArtdNonFatalErrorCode = 1;
  return ScopedAStatus::fromServiceSpecificErrorWithMessage(kArtdNonFatalErrorCode,
                                                            EscapeErrorMessage(message).c_str());
}

Result<CompilerFilter::Filter> ParseCompilerFilter(const std::string& compiler_filter_str) {
  CompilerFilter::Filter compiler_filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter_str.c_str(), &compiler_filter)) {
    return Errorf("Failed to parse compiler filter '{}'", compiler_filter_str);
  }
  return compiler_filter;
}

OatFileAssistant::DexOptTrigger DexOptTriggerFromAidl(int32_t aidl_value) {
  OatFileAssistant::DexOptTrigger trigger{};
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_BETTER)) != 0) {
    trigger.targetFilterIsBetter = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_SAME)) != 0) {
    trigger.targetFilterIsSame = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::COMPILER_FILTER_IS_WORSE)) != 0) {
    trigger.targetFilterIsWorse = true;
  }
  if ((aidl_value & static_cast<int32_t>(DexoptTrigger::PRIMARY_BOOT_IMAGE_BECOMES_USABLE)) != 0) {
    trigger.primaryBootImageBecomesUsable = true;
  }
  return trigger;
}

ArtifactsLocation ArtifactsLocationToAidl(OatFileAssistant::Location location) {
  switch (location) {
    case OatFileAssistant::Location::kLocationNoneOrError:
      return ArtifactsLocation::NONE_OR_ERROR;
    case OatFileAssistant::Location::kLocationOat:
      return ArtifactsLocation::DALVIK_CACHE;
    case OatFileAssistant::Location::kLocationOdex:
      return ArtifactsLocation::NEXT_TO_DEX;
    case OatFileAssistant::Location::kLocationDm:
      return ArtifactsLocation::DM;
      // No default. All cases should be explicitly handled, or the compilation will fail.
  }
  // This should never happen. Just in case we get a non-enumerator value.
  LOG(FATAL) << "Unexpected Location " << location;
}

Result<void> PrepareArtifactsDir(
    const std::string& path,
    const FsPermission& fs_permission,
    const std::optional<OutputArtifacts::PermissionSettings::SeContext>& se_context =
        std::nullopt) {
  std::error_code ec;
  bool created = std::filesystem::create_directory(path, ec);
  if (ec) {
    return Errorf("Failed to create directory '{}': {}", path, ec.message());
  }

  auto cleanup = make_scope_guard([&] {
    if (created) {
      std::filesystem::remove(path, ec);
    }
  });

  if (chmod(path.c_str(), DirFsPermissionToMode(fs_permission)) != 0) {
    return ErrnoErrorf("Failed to chmod directory '{}'", path);
  }
  OR_RETURN(Chown(path, fs_permission));

  if (kIsTargetAndroid) {
    int res = 0;
    if (se_context.has_value()) {
      res = selinux_android_restorecon_pkgdir(path.c_str(),
                                              se_context->seInfo.c_str(),
                                              se_context->packageUid,
                                              SELINUX_ANDROID_RESTORECON_RECURSE);
    } else {
      res = selinux_android_restorecon(path.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE);
    }
    if (res != 0) {
      return ErrnoErrorf("Failed to restorecon directory '{}'", path);
    }
  }

  cleanup.Disable();
  return {};
}

Result<void> PrepareArtifactsDirs(const OutputArtifacts& output_artifacts) {
  if (output_artifacts.artifactsPath.isInDalvikCache) {
    return {};
  }

  std::filesystem::path oat_path(OR_RETURN(BuildOatPath(output_artifacts.artifactsPath)));
  std::filesystem::path isa_dir = oat_path.parent_path();
  std::filesystem::path oat_dir = isa_dir.parent_path();
  DCHECK_EQ(oat_dir.filename(), "oat");

  OR_RETURN(PrepareArtifactsDir(oat_dir,
                                output_artifacts.permissionSettings.dirFsPermission,
                                output_artifacts.permissionSettings.seContext));
  OR_RETURN(PrepareArtifactsDir(isa_dir, output_artifacts.permissionSettings.dirFsPermission));
  return {};
}

Result<FileVisibility> GetFileVisibility(const std::string& file) {
  std::error_code ec;
  std::filesystem::file_status status = std::filesystem::status(file, ec);
  if (!std::filesystem::status_known(status)) {
    return Errorf("Failed to get status of '{}': {}", file, ec.message());
  }
  if (!std::filesystem::exists(status)) {
    return FileVisibility::NOT_FOUND;
  }

  return (status.permissions() & std::filesystem::perms::others_read) !=
                 std::filesystem::perms::none ?
             FileVisibility::OTHER_READABLE :
             FileVisibility::NOT_OTHER_READABLE;
}

class FdLogger {
 public:
  void Add(const NewFile& file) { fd_mapping_.emplace_back(file.Fd(), file.TempPath()); }
  void Add(const File& file) { fd_mapping_.emplace_back(file.Fd(), file.GetPath()); }

 private:
  std::vector<std::pair<int, std::string>> fd_mapping_;

  friend std::ostream& operator<<(std::ostream& os, const FdLogger& fd_logger);
};

std::ostream& operator<<(std::ostream& os, const FdLogger& fd_logger) {
  for (const auto& [fd, path] : fd_logger.fd_mapping_) {
    os << fd << ":" << path << ' ';
  }
  return os;
}

}  // namespace

#define OR_RETURN_ERROR(func, expr)         \
  ({                                        \
    decltype(expr)&& tmp = (expr);          \
    if (!tmp.ok()) {                        \
      return (func)(tmp.error().message()); \
    }                                       \
    std::move(tmp).value();                 \
  })

#define OR_RETURN_FATAL(expr)     OR_RETURN_ERROR(Fatal, expr)
#define OR_RETURN_NON_FATAL(expr) OR_RETURN_ERROR(NonFatal, expr)

ScopedAStatus Artd::isAlive(bool* _aidl_return) {
  *_aidl_return = true;
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::deleteArtifacts(const ArtifactsPath& in_artifactsPath, int64_t* _aidl_return) {
  std::string oat_path = OR_RETURN_FATAL(BuildOatPath(in_artifactsPath));

  *_aidl_return = 0;
  *_aidl_return += GetSizeAndDeleteFile(oat_path);
  *_aidl_return += GetSizeAndDeleteFile(OatPathToVdexPath(oat_path));
  *_aidl_return += GetSizeAndDeleteFile(OatPathToArtPath(oat_path));

  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getOptimizationStatus(const std::string& in_dexFile,
                                          const std::string& in_instructionSet,
                                          const std::string& in_classLoaderContext,
                                          GetOptimizationStatusResult* _aidl_return) {
  Result<OatFileAssistantContext*> ofa_context = GetOatFileAssistantContext();
  if (!ofa_context.ok()) {
    return NonFatal("Failed to get runtime options: " + ofa_context.error().message());
  }

  std::unique_ptr<ClassLoaderContext> context;
  std::string error_msg;
  auto oat_file_assistant = OatFileAssistant::Create(in_dexFile.c_str(),
                                                     in_instructionSet.c_str(),
                                                     in_classLoaderContext.c_str(),
                                                     /*load_executable=*/false,
                                                     /*only_load_trusted_executable=*/true,
                                                     ofa_context.value(),
                                                     &context,
                                                     &error_msg);
  if (oat_file_assistant == nullptr) {
    return NonFatal("Failed to create OatFileAssistant: " + error_msg);
  }

  std::string ignored_odex_status;
  oat_file_assistant->GetOptimizationStatus(&_aidl_return->locationDebugString,
                                            &_aidl_return->compilerFilter,
                                            &_aidl_return->compilationReason,
                                            &ignored_odex_status);

  // We ignore odex_status because it is not meaningful. It can only be either "up-to-date",
  // "apk-more-recent", or "io-error-no-oat", which means it doesn't give us information in addition
  // to what we can learn from compiler_filter because compiler_filter will be the actual compiler
  // filter, "run-from-apk-fallback", and "run-from-apk" in those three cases respectively.
  DCHECK(ignored_odex_status == "up-to-date" || ignored_odex_status == "apk-more-recent" ||
         ignored_odex_status == "io-error-no-oat");

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::isProfileUsable(const ProfilePath& in_profile,
                                         const std::string& in_dexFile,
                                         bool* _aidl_return) {
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));

  CmdlineBuilder args;
  FdLogger fd_logger;
  args.Add(OR_RETURN_FATAL(GetArtExec()))
      .Add("--drop-capabilities")
      .Add("--")
      .Add(OR_RETURN_FATAL(GetProfman()));

  Result<std::unique_ptr<File>> profile = OpenFileForReading(profile_path);
  if (!profile.ok()) {
    if (profile.error().code() == ENOENT) {
      *_aidl_return = false;
      return ScopedAStatus::ok();
    }
    return NonFatal(
        "Failed to open profile '{}': {}"_format(profile_path, profile.error().message()));
  }
  args.Add("--reference-profile-file-fd=%d", profile.value()->Fd());
  fd_logger.Add(*profile.value());

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(in_dexFile));
  args.Add("--apk-fd=%d", dex_file->Fd());
  fd_logger.Add(*dex_file);

  LOG(DEBUG) << "Running profman: " << Join(args.Get(), /*separator=*/" ")
             << "\nOpened FDs: " << fd_logger;

  Result<int> result = ExecAndReturnCode(args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return NonFatal("Failed to run profman: " + result.error().message());
  }

  if (result.value() != ProfmanResult::kSkipCompilationSmallDelta &&
      result.value() != ProfmanResult::kSkipCompilationEmptyProfiles) {
    return NonFatal("profman returned an unexpected code: {}"_format(result.value()));
  }

  *_aidl_return = result.value() == ProfmanResult::kSkipCompilationSmallDelta;
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::copyProfile(const ProfilePath& in_src, OutputProfile* in_dst) {
  std::string src_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_src));
  if (in_src.getTag() == ProfilePath::dexMetadataPath) {
    return Fatal("Does not support DM file, got '{}'"_format(src_path));
  }
  std::string dst_path = OR_RETURN_FATAL(BuildRefProfilePath(in_dst->profilePath.refProfilePath));

  std::string content;
  if (!ReadFileToString(src_path, &content)) {
    return NonFatal("Failed to read file '{}': {}"_format(src_path, strerror(errno)));
  }

  std::unique_ptr<NewFile> dst =
      OR_RETURN_NON_FATAL(NewFile::Create(dst_path, in_dst->fsPermission));
  if (!WriteStringToFd(content, dst->Fd())) {
    return NonFatal("Failed to write file '{}': {}"_format(dst_path, strerror(errno)));
  }

  OR_RETURN_NON_FATAL(dst->Keep());
  in_dst->profilePath.id = dst->TempId();
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::copyAndRewriteProfile(const ProfilePath& in_src,
                                               OutputProfile* in_dst,
                                               const std::string& in_dexFile,
                                               bool* _aidl_return) {
  std::string src_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_src));
  std::string dst_path = OR_RETURN_FATAL(BuildRefProfilePath(in_dst->profilePath.refProfilePath));
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));

  CmdlineBuilder args;
  FdLogger fd_logger;
  args.Add(OR_RETURN_FATAL(GetArtExec()))
      .Add("--drop-capabilities")
      .Add("--")
      .Add(OR_RETURN_FATAL(GetProfman()))
      .Add("--copy-and-update-profile-key");

  Result<std::unique_ptr<File>> src = OpenFileForReading(src_path);
  if (!src.ok()) {
    if (src.error().code() == ENOENT) {
      *_aidl_return = false;
      return ScopedAStatus::ok();
    }
    return NonFatal("Failed to open src profile '{}': {}"_format(src_path, src.error().message()));
  }
  args.Add("--profile-file-fd=%d", src.value()->Fd());
  fd_logger.Add(*src.value());

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(in_dexFile));
  args.Add("--apk-fd=%d", dex_file->Fd());
  fd_logger.Add(*dex_file);

  std::unique_ptr<NewFile> dst =
      OR_RETURN_NON_FATAL(NewFile::Create(dst_path, in_dst->fsPermission));
  args.Add("--reference-profile-file-fd=%d", dst->Fd());
  fd_logger.Add(*dst);

  LOG(DEBUG) << "Running profman: " << Join(args.Get(), /*separator=*/" ")
             << "\nOpened FDs: " << fd_logger;

  Result<int> result = ExecAndReturnCode(args.Get(), kShortTimeoutSec);
  if (!result.ok()) {
    return NonFatal("Failed to run profman: " + result.error().message());
  }

  if (result.value() == ProfmanResult::kCopyAndUpdateNoUpdate) {
    *_aidl_return = false;
    return ScopedAStatus::ok();
  }

  if (result.value() != ProfmanResult::kCopyAndUpdateSuccess) {
    return NonFatal("profman returned an unexpected code: {}"_format(result.value()));
  }

  OR_RETURN_NON_FATAL(dst->Keep());
  *_aidl_return = true;
  in_dst->profilePath.id = dst->TempId();
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::commitTmpProfile(const TmpRefProfilePath& in_profile) {
  std::string tmp_profile_path = OR_RETURN_FATAL(BuildTmpRefProfilePath(in_profile));
  std::string ref_profile_path = OR_RETURN_FATAL(BuildRefProfilePath(in_profile.refProfilePath));

  std::error_code ec;
  std::filesystem::rename(tmp_profile_path, ref_profile_path, ec);
  if (ec) {
    return NonFatal(
        "Failed to move '{}' to '{}': {}"_format(tmp_profile_path, ref_profile_path, ec.message()));
  }

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::deleteProfile(const ProfilePath& in_profile) {
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));

  std::error_code ec;
  if (!std::filesystem::remove(profile_path, ec)) {
    LOG(ERROR) << "Failed to remove '{}': {}"_format(profile_path, ec.message());
  }

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getProfileVisibility(const ProfilePath& in_profile,
                                              FileVisibility* _aidl_return) {
  std::string profile_path = OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile));
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(profile_path));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getArtifactsVisibility(const ArtifactsPath& in_artifactsPath,
                                                FileVisibility* _aidl_return) {
  std::string oat_path = OR_RETURN_FATAL(BuildOatPath(in_artifactsPath));
  *_aidl_return = OR_RETURN_NON_FATAL(GetFileVisibility(oat_path));
  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::getDexoptNeeded(const std::string& in_dexFile,
                                         const std::string& in_instructionSet,
                                         const std::string& in_classLoaderContext,
                                         const std::string& in_compilerFilter,
                                         int32_t in_dexoptTrigger,
                                         GetDexoptNeededResult* _aidl_return) {
  Result<OatFileAssistantContext*> ofa_context = GetOatFileAssistantContext();
  if (!ofa_context.ok()) {
    return NonFatal("Failed to get runtime options: " + ofa_context.error().message());
  }

  std::unique_ptr<ClassLoaderContext> context;
  std::string error_msg;
  auto oat_file_assistant = OatFileAssistant::Create(in_dexFile.c_str(),
                                                     in_instructionSet.c_str(),
                                                     in_classLoaderContext.c_str(),
                                                     /*load_executable=*/false,
                                                     /*only_load_trusted_executable=*/true,
                                                     ofa_context.value(),
                                                     &context,
                                                     &error_msg);
  if (oat_file_assistant == nullptr) {
    return NonFatal("Failed to create OatFileAssistant: " + error_msg);
  }

  OatFileAssistant::DexOptStatus status;
  _aidl_return->isDexoptNeeded =
      oat_file_assistant->GetDexOptNeeded(OR_RETURN_FATAL(ParseCompilerFilter(in_compilerFilter)),
                                          DexOptTriggerFromAidl(in_dexoptTrigger),
                                          &status);
  _aidl_return->isVdexUsable = status.IsVdexUsable();
  _aidl_return->artifactsLocation = ArtifactsLocationToAidl(status.GetLocation());

  return ScopedAStatus::ok();
}

ndk::ScopedAStatus Artd::dexopt(const OutputArtifacts& in_outputArtifacts,
                                const std::string& in_dexFile,
                                const std::string& in_instructionSet,
                                const std::string& in_classLoaderContext,
                                const std::string& in_compilerFilter,
                                const std::optional<ProfilePath>& in_profile,
                                const std::optional<VdexPath>& in_inputVdex,
                                PriorityClass in_priorityClass,
                                const DexoptOptions& in_dexoptOptions,
                                DexoptResult* _aidl_return) {
  _aidl_return->cancelled = false;

  std::string oat_path = OR_RETURN_FATAL(BuildOatPath(in_outputArtifacts.artifactsPath));
  std::string vdex_path = OatPathToVdexPath(oat_path);
  std::string art_path = OatPathToArtPath(oat_path);
  OR_RETURN_FATAL(ValidateDexPath(in_dexFile));
  std::optional<std::string> profile_path =
      in_profile.has_value() ?
          std::make_optional(OR_RETURN_FATAL(BuildProfileOrDmPath(in_profile.value()))) :
          std::nullopt;

  std::unique_ptr<ClassLoaderContext> context =
      ClassLoaderContext::Create(in_classLoaderContext.c_str());
  if (context == nullptr) {
    return Fatal("Class loader context '{}' is invalid"_format(in_classLoaderContext));
  }

  OR_RETURN_NON_FATAL(PrepareArtifactsDirs(in_outputArtifacts));

  CmdlineBuilder args;
  args.Add(OR_RETURN_FATAL(GetArtExec())).Add("--drop-capabilities");

  if (in_priorityClass < PriorityClass::BOOT) {
    args.Add("--set-task-profile=Dex2OatBootComplete").Add("--set-priority=background");
  }

  args.Add("--").Add(OR_RETURN_FATAL(GetDex2Oat()));
  FdLogger fd_logger;

  const FsPermission& fs_permission = in_outputArtifacts.permissionSettings.fileFsPermission;
  std::unique_ptr<NewFile> oat_file = OR_RETURN_NON_FATAL(NewFile::Create(oat_path, fs_permission));
  args.Add("--oat-fd=%d", oat_file->Fd()).Add("--oat-location=%s", oat_path);
  fd_logger.Add(*oat_file);

  std::unique_ptr<NewFile> vdex_file =
      OR_RETURN_NON_FATAL(NewFile::Create(vdex_path, fs_permission));
  args.Add("--output-vdex-fd=%d", vdex_file->Fd());
  fd_logger.Add(*vdex_file);

  std::vector<NewFile*> files_to_commit{oat_file.get(), vdex_file.get()};
  std::vector<std::string_view> files_to_delete;

  std::unique_ptr<NewFile> art_file = nullptr;
  if (in_dexoptOptions.generateAppImage) {
    art_file = OR_RETURN_NON_FATAL(NewFile::Create(art_path, fs_permission));
    args.Add("--app-image-fd=%d", art_file->Fd());
    fd_logger.Add(*art_file);
    files_to_commit.push_back(art_file.get());
  } else {
    files_to_delete.push_back(art_path);
  }

  std::unique_ptr<NewFile> swap_file = nullptr;
  if (ShouldCreateSwapFileForDexopt()) {
    swap_file = OR_RETURN_NON_FATAL(
        NewFile::Create("{}.swap"_format(oat_path), FsPermission{.uid = -1, .gid = -1}));
    args.Add("--swap-fd=%d", swap_file->Fd());
    fd_logger.Add(*swap_file);
  }

  std::unique_ptr<File> dex_file = OR_RETURN_NON_FATAL(OpenFileForReading(in_dexFile));
  args.Add("--zip-fd=%d", dex_file->Fd()).Add("--zip-location=%s", in_dexFile);
  fd_logger.Add(*dex_file);

  std::vector<std::string> flattened_context = context->FlattenDexPaths();
  std::string dex_dir = Dirname(in_dexFile.c_str());
  std::vector<std::unique_ptr<File>> context_files;
  std::vector<int> context_fds;
  for (const std::string& context_element : flattened_context) {
    std::string context_path = std::filesystem::path(dex_dir).append(context_element);
    OR_RETURN_FATAL(ValidateDexPath(context_path));
    std::unique_ptr<File> context_file = OR_RETURN_NON_FATAL(OpenFileForReading(context_path));
    context_fds.push_back(context_file->Fd());
    fd_logger.Add(*context_file);
    context_files.push_back(std::move(context_file));
  }
  args.Add("--class-loader-context-fds=%s", Join(context_fds, /*separator=*/':'))
      .Add("--class-loader-context=%s", in_classLoaderContext)
      .Add("--classpath-dir=%s", dex_dir);

  std::unique_ptr<File> input_vdex_file = nullptr;
  if (in_inputVdex.has_value()) {
    if (in_inputVdex->getTag() == VdexPath::dexMetadataPath) {
      std::string input_vdex_path = OR_RETURN_FATAL(BuildDexMetadataPath(in_inputVdex.value()));
      input_vdex_file = OR_RETURN_NON_FATAL(OpenFileForReading(input_vdex_path));
      args.Add("--dm-fd=%d", input_vdex_file->Fd());
    } else {
      std::string input_vdex_path = OR_RETURN_FATAL(BuildVdexPath(in_inputVdex.value()));
      input_vdex_file = OR_RETURN_NON_FATAL(OpenFileForReading(input_vdex_path));
      args.Add("--input-vdex-fd=%d", input_vdex_file->Fd());
    }
    fd_logger.Add(*input_vdex_file);
  }

  std::unique_ptr<File> profile_file = nullptr;
  if (profile_path.has_value()) {
    profile_file = OR_RETURN_NON_FATAL(OpenFileForReading(profile_path.value()));
    args.Add("--profile-file-fd=%d", profile_file->Fd());
    fd_logger.Add(*profile_file);
  }

  AddCompilerConfigFlags(
      in_instructionSet, in_compilerFilter, in_priorityClass, in_dexoptOptions, args);
  AddPerfConfigFlags(in_priorityClass, args);

  LOG(INFO) << "Running dex2oat: " << Join(args.Get(), /*separator=*/" ")
            << "\nOpened FDs: " << fd_logger;

  ProcessStat stat;
  Result<int> result = ExecAndReturnCode(args.Get(), kLongTimeoutSec, &stat);
  _aidl_return->wallTimeMs = stat.wall_time_ms;
  _aidl_return->cpuTimeMs = stat.cpu_time_ms;
  if (!result.ok()) {
    // TODO(b/244412198): Return cancelled=true if dexopt is cancelled upon request.
    return NonFatal("Failed to run dex2oat: " + result.error().message());
  }
  if (result.value() != 0) {
    return NonFatal("dex2oat returned an unexpected code: %d"_format(result.value()));
  }

  NewFile::CommitAllOrAbandon(files_to_commit, files_to_delete);

  return ScopedAStatus::ok();
}

Result<void> Artd::Start() {
  ScopedAStatus status = ScopedAStatus::fromStatus(
      AServiceManager_registerLazyService(this->asBinder().get(), kServiceName));
  if (!status.isOk()) {
    return Error() << status.getDescription();
  }

  ABinderProcess_startThreadPool();

  return {};
}

Result<OatFileAssistantContext*> Artd::GetOatFileAssistantContext() {
  std::lock_guard<std::mutex> lock(ofa_context_mu_);

  if (ofa_context_ == nullptr) {
    ofa_context_ = std::make_unique<OatFileAssistantContext>(
        std::make_unique<OatFileAssistantContext::RuntimeOptions>(
            OatFileAssistantContext::RuntimeOptions{
                .image_locations = *OR_RETURN(GetBootImageLocations()),
                .boot_class_path = *OR_RETURN(GetBootClassPath()),
                .boot_class_path_locations = *OR_RETURN(GetBootClassPath()),
                .deny_art_apex_data_files = DenyArtApexDataFiles(),
            }));
    std::string error_msg;
    if (!ofa_context_->FetchAll(&error_msg)) {
      return Error() << error_msg;
    }
  }

  return ofa_context_.get();
}

Result<const std::vector<std::string>*> Artd::GetBootImageLocations() {
  if (!cached_boot_image_locations_.has_value()) {
    std::string location_str;

    if (UseJitZygote()) {
      location_str = GetJitZygoteBootImageLocation();
    } else if (std::string value = props_->GetOrEmpty("dalvik.vm.boot-image"); !value.empty()) {
      location_str = std::move(value);
    } else {
      std::string error_msg;
      std::string android_root = GetAndroidRootSafe(&error_msg);
      if (!error_msg.empty()) {
        return Errorf("Failed to get ANDROID_ROOT: {}", error_msg);
      }
      location_str = GetDefaultBootImageLocation(android_root, DenyArtApexDataFiles());
    }

    cached_boot_image_locations_ = Split(location_str, ":");
  }

  return &cached_boot_image_locations_.value();
}

Result<const std::vector<std::string>*> Artd::GetBootClassPath() {
  if (!cached_boot_class_path_.has_value()) {
    const char* env_value = getenv("BOOTCLASSPATH");
    if (env_value == nullptr || strlen(env_value) == 0) {
      return Errorf("Failed to get environment variable 'BOOTCLASSPATH'");
    }
    cached_boot_class_path_ = Split(env_value, ":");
  }

  return &cached_boot_class_path_.value();
}

bool Artd::UseJitZygote() {
  if (!cached_use_jit_zygote_.has_value()) {
    cached_use_jit_zygote_ =
        props_->GetBool("dalvik.vm.profilebootclasspath",
                        "persist.device_config.runtime_native_boot.profilebootclasspath",
                        /*default_value=*/false);
  }

  return cached_use_jit_zygote_.value();
}

bool Artd::DenyArtApexDataFiles() {
  if (!cached_deny_art_apex_data_files_.has_value()) {
    cached_deny_art_apex_data_files_ =
        !props_->GetBool("odsign.verification.success", /*default_value=*/false);
  }

  return cached_deny_art_apex_data_files_.value();
}

Result<std::string> Artd::GetProfman() { return BuildArtBinPath("profman"); }

Result<std::string> Artd::GetArtExec() { return BuildArtBinPath("art_exec"); }

bool Artd::ShouldUseDex2Oat64() {
  return !props_->GetOrEmpty("ro.product.cpu.abilist64").empty() &&
         props_->GetBool("dalvik.vm.dex2oat64.enabled", /*default_value=*/false);
}

Result<std::string> Artd::GetDex2Oat() {
  std::string binary_name = ShouldUseDex2Oat64() ? "dex2oat64" : "dex2oat32";
  // TODO(b/234351700): Should we use the "d" variant?
  return BuildArtBinPath(binary_name);
}

bool Artd::ShouldCreateSwapFileForDexopt() {
  // Create a swap file by default. Dex2oat will decide whether to use it or not.
  return props_->GetBool("dalvik.vm.dex2oat-swap", /*default_value=*/true);
}

void Artd::AddCompilerConfigFlags(const std::string& instruction_set,
                                  const std::string& compiler_filter,
                                  PriorityClass priority_class,
                                  const DexoptOptions& dexopt_options,
                                  /*out*/ CmdlineBuilder& args) {
  args.Add("--instruction-set=%s", instruction_set);
  std::string features_prop = "dalvik.vm.isa.{}.features"_format(instruction_set);
  args.AddIfNonEmpty("--instruction-set-features=%s", props_->GetOrEmpty(features_prop));
  std::string variant_prop = "dalvik.vm.isa.{}.variant"_format(instruction_set);
  args.AddIfNonEmpty("--instruction-set-variant=%s", props_->GetOrEmpty(variant_prop));

  args.Add("--compiler-filter=%s", compiler_filter)
      .Add("--compilation-reason=%s", dexopt_options.compilationReason);

  args.AddIf(priority_class >= PriorityClass::INTERACTIVE, "--compact-dex-level=none");

  args.AddIfNonEmpty("--max-image-block-size=%s",
                     props_->GetOrEmpty("dalvik.vm.dex2oat-max-image-block-size"))
      .AddIfNonEmpty("--very-large-app-threshold=%s",
                     props_->GetOrEmpty("dalvik.vm.dex2oat-very-large"))
      .AddIfNonEmpty(
          "--resolve-startup-const-strings=%s",
          props_->GetOrEmpty("persist.device_config.runtime.dex2oat_resolve_startup_strings",
                             "dalvik.vm.dex2oat-resolve-startup-strings"));

  args.AddIf(dexopt_options.debuggable, "--debuggable")
      .AddIf(props_->GetBool("debug.generate-debug-info", /*default_value=*/false),
             "--generate-debug-info")
      .AddIf(props_->GetBool("dalvik.vm.dex2oat-minidebuginfo", /*default_value=*/false),
             "--generate-mini-debug-info");

  args.AddRuntimeIf(DenyArtApexDataFiles(), "-Xdeny-art-apex-data-files")
      .AddRuntime("-Xtarget-sdk-version:%d", dexopt_options.targetSdkVersion)
      .AddRuntimeIf(dexopt_options.hiddenApiPolicyEnabled, "-Xhidden-api-policy:enabled");
}

void Artd::AddPerfConfigFlags(PriorityClass priority_class, /*out*/ CmdlineBuilder& args) {
  // CPU set and number of threads.
  std::string default_cpu_set_prop = "dalvik.vm.dex2oat-cpu-set";
  std::string default_threads_prop = "dalvik.vm.dex2oat-threads";
  std::string cpu_set;
  std::string threads;
  if (priority_class >= PriorityClass::BOOT) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.boot-dex2oat-cpu-set");
    threads = props_->GetOrEmpty("dalvik.vm.boot-dex2oat-threads");
  } else if (priority_class >= PriorityClass::INTERACTIVE_FAST) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.restore-dex2oat-cpu-set", default_cpu_set_prop);
    threads = props_->GetOrEmpty("dalvik.vm.restore-dex2oat-threads", default_threads_prop);
  } else if (priority_class <= PriorityClass::BACKGROUND) {
    cpu_set = props_->GetOrEmpty("dalvik.vm.background-dex2oat-cpu-set", default_cpu_set_prop);
    threads = props_->GetOrEmpty("dalvik.vm.background-dex2oat-threads", default_threads_prop);
  } else {
    cpu_set = props_->GetOrEmpty(default_cpu_set_prop);
    threads = props_->GetOrEmpty(default_threads_prop);
  }
  args.AddIfNonEmpty("--cpu-set=%s", cpu_set).AddIfNonEmpty("-j%s", threads);

  args.AddRuntimeIfNonEmpty("-Xms%s", props_->GetOrEmpty("dalvik.vm.dex2oat-Xms"))
      .AddRuntimeIfNonEmpty("-Xmx%s", props_->GetOrEmpty("dalvik.vm.dex2oat-Xmx"));

  // Enable compiling dex files in isolation on low ram devices.
  // It takes longer but reduces the memory footprint.
  args.AddIf(props_->GetBool("ro.config.low_ram", /*default_value=*/false),
             "--compile-individually");
}

android::base::Result<int> Artd::ExecAndReturnCode(const std::vector<std::string>& args,
                                                   int timeout_sec,
                                                   ProcessStat* stat) const {
  bool ignored_timed_out = false;  // This information is encoded in the error message.
  std::string error_msg;
  int exit_code = exec_utils_->ExecAndReturnCode(
      args, timeout_sec, ExecCallbacks(), &ignored_timed_out, stat, &error_msg);
  if (exit_code < 0) {
    return Error() << error_msg;
  }
  return exit_code;
}

}  // namespace artd
}  // namespace art
