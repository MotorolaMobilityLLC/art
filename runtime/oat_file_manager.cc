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

#include "oat_file_manager.h"

#include <memory>
#include <queue>
#include <vector>

#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "art_field-inl.h"
#include "base/bit_vector-inl.h"
#include "base/file_utils.h"
#include "base/logging.h"  // For VLOG.
#include "base/mutex-inl.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_loader.h"
#include "dex/dex_file_tracking_registrar.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/image_space.h"
#include "handle_scope-inl.h"
#include "jni/jni_internal.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "oat_file.h"
#include "oat_file_assistant.h"
#include "obj_ptr-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {

using android::base::StringPrintf;

// If true, we attempt to load the application image if it exists.
static constexpr bool kEnableAppImage = true;

const OatFile* OatFileManager::RegisterOatFile(std::unique_ptr<const OatFile> oat_file) {
  WriterMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  CHECK(!only_use_system_oat_files_ ||
        LocationIsOnSystem(oat_file->GetLocation().c_str()) ||
        !oat_file->IsExecutable())
      << "Registering a non /system oat file: " << oat_file->GetLocation();
  DCHECK(oat_file != nullptr);
  if (kIsDebugBuild) {
    CHECK(oat_files_.find(oat_file) == oat_files_.end());
    for (const std::unique_ptr<const OatFile>& existing : oat_files_) {
      CHECK_NE(oat_file.get(), existing.get()) << oat_file->GetLocation();
      // Check that we don't have an oat file with the same address. Copies of the same oat file
      // should be loaded at different addresses.
      CHECK_NE(oat_file->Begin(), existing->Begin()) << "Oat file already mapped at that location";
    }
  }
  const OatFile* ret = oat_file.get();
  oat_files_.insert(std::move(oat_file));
  return ret;
}

void OatFileManager::UnRegisterAndDeleteOatFile(const OatFile* oat_file) {
  WriterMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  DCHECK(oat_file != nullptr);
  std::unique_ptr<const OatFile> compare(oat_file);
  auto it = oat_files_.find(compare);
  CHECK(it != oat_files_.end());
  oat_files_.erase(it);
  compare.release();  // NOLINT b/117926937
}

const OatFile* OatFileManager::FindOpenedOatFileFromDexLocation(
    const std::string& dex_base_location) const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    const std::vector<const OatDexFile*>& oat_dex_files = oat_file->GetOatDexFiles();
    for (const OatDexFile* oat_dex_file : oat_dex_files) {
      if (DexFileLoader::GetBaseLocation(oat_dex_file->GetDexFileLocation()) == dex_base_location) {
        return oat_file.get();
      }
    }
  }
  return nullptr;
}

const OatFile* OatFileManager::FindOpenedOatFileFromOatLocation(const std::string& oat_location)
    const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  return FindOpenedOatFileFromOatLocationLocked(oat_location);
}

const OatFile* OatFileManager::FindOpenedOatFileFromOatLocationLocked(
    const std::string& oat_location) const {
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    if (oat_file->GetLocation() == oat_location) {
      return oat_file.get();
    }
  }
  return nullptr;
}

std::vector<const OatFile*> OatFileManager::GetBootOatFiles() const {
  std::vector<gc::space::ImageSpace*> image_spaces =
      Runtime::Current()->GetHeap()->GetBootImageSpaces();
  std::vector<const OatFile*> oat_files;
  oat_files.reserve(image_spaces.size());
  for (gc::space::ImageSpace* image_space : image_spaces) {
    oat_files.push_back(image_space->GetOatFile());
  }
  return oat_files;
}

const OatFile* OatFileManager::GetPrimaryOatFile() const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  std::vector<const OatFile*> boot_oat_files = GetBootOatFiles();
  if (!boot_oat_files.empty()) {
    for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
      if (std::find(boot_oat_files.begin(), boot_oat_files.end(), oat_file.get()) ==
          boot_oat_files.end()) {
        return oat_file.get();
      }
    }
  }
  return nullptr;
}

OatFileManager::OatFileManager()
    : only_use_system_oat_files_(false) {}

OatFileManager::~OatFileManager() {
  // Explicitly clear oat_files_ since the OatFile destructor calls back into OatFileManager for
  // UnRegisterOatFileLocation.
  oat_files_.clear();
}

std::vector<const OatFile*> OatFileManager::RegisterImageOatFiles(
    const std::vector<gc::space::ImageSpace*>& spaces) {
  std::vector<const OatFile*> oat_files;
  oat_files.reserve(spaces.size());
  for (gc::space::ImageSpace* space : spaces) {
    oat_files.push_back(RegisterOatFile(space->ReleaseOatFile()));
  }
  return oat_files;
}

class TypeIndexInfo {
 public:
  explicit TypeIndexInfo(const DexFile* dex_file)
      : type_indexes_(GenerateTypeIndexes(dex_file)),
        iter_(type_indexes_.Indexes().begin()),
        end_(type_indexes_.Indexes().end()) { }

  BitVector& GetTypeIndexes() {
    return type_indexes_;
  }
  BitVector::IndexIterator& GetIterator() {
    return iter_;
  }
  BitVector::IndexIterator& GetIteratorEnd() {
    return end_;
  }
  void AdvanceIterator() {
    iter_++;
  }

 private:
  static BitVector GenerateTypeIndexes(const DexFile* dex_file) {
    BitVector type_indexes(/*start_bits=*/0, /*expandable=*/true, Allocator::GetMallocAllocator());
    for (uint16_t i = 0; i < dex_file->NumClassDefs(); ++i) {
      const dex::ClassDef& class_def = dex_file->GetClassDef(i);
      uint16_t type_idx = class_def.class_idx_.index_;
      type_indexes.SetBit(type_idx);
    }
    return type_indexes;
  }

  // BitVector with bits set for the type indexes of all classes in the input dex file.
  BitVector type_indexes_;
  BitVector::IndexIterator iter_;
  BitVector::IndexIterator end_;
};

class DexFileAndClassPair : ValueObject {
 public:
  DexFileAndClassPair(const DexFile* dex_file, TypeIndexInfo* type_info, bool from_loaded_oat)
     : type_info_(type_info),
       dex_file_(dex_file),
       cached_descriptor_(dex_file_->StringByTypeIdx(dex::TypeIndex(*type_info->GetIterator()))),
       from_loaded_oat_(from_loaded_oat) {
    type_info_->AdvanceIterator();
  }

  DexFileAndClassPair(const DexFileAndClassPair& rhs) = default;

  DexFileAndClassPair& operator=(const DexFileAndClassPair& rhs) = default;

  const char* GetCachedDescriptor() const {
    return cached_descriptor_;
  }

  bool operator<(const DexFileAndClassPair& rhs) const {
    const int cmp = strcmp(cached_descriptor_, rhs.cached_descriptor_);
    if (cmp != 0) {
      // Note that the order must be reversed. We want to iterate over the classes in dex files.
      // They are sorted lexicographically. Thus, the priority-queue must be a min-queue.
      return cmp > 0;
    }
    return dex_file_ < rhs.dex_file_;
  }

  bool DexFileHasMoreClasses() const {
    return type_info_->GetIterator() != type_info_->GetIteratorEnd();
  }

  void Next() {
    cached_descriptor_ = dex_file_->StringByTypeIdx(dex::TypeIndex(*type_info_->GetIterator()));
    type_info_->AdvanceIterator();
  }

  bool FromLoadedOat() const {
    return from_loaded_oat_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_;
  }

 private:
  TypeIndexInfo* type_info_;
  const DexFile* dex_file_;
  const char* cached_descriptor_;
  bool from_loaded_oat_;  // We only need to compare mismatches between what we load now
                          // and what was loaded before. Any old duplicates must have been
                          // OK, and any new "internal" duplicates are as well (they must
                          // be from multidex, which resolves correctly).
};

static void AddDexFilesFromOat(
    const OatFile* oat_file,
    /*out*/std::vector<const DexFile*>* dex_files,
    std::vector<std::unique_ptr<const DexFile>>* opened_dex_files) {
  for (const OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
    std::string error;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error);
    if (dex_file == nullptr) {
      LOG(WARNING) << "Could not create dex file from oat file: " << error;
    } else if (dex_file->NumClassDefs() > 0U) {
      dex_files->push_back(dex_file.get());
      opened_dex_files->push_back(std::move(dex_file));
    }
  }
}

static void AddNext(/*inout*/DexFileAndClassPair& original,
                    /*inout*/std::priority_queue<DexFileAndClassPair>& heap) {
  if (original.DexFileHasMoreClasses()) {
    original.Next();
    heap.push(std::move(original));
  }
}

static bool CheckClassCollision(const OatFile* oat_file,
    const ClassLoaderContext* context,
    std::string* error_msg /*out*/) {
  std::vector<const DexFile*> dex_files_loaded = context->FlattenOpenedDexFiles();

  // Vector that holds the newly opened dex files live, this is done to prevent leaks.
  std::vector<std::unique_ptr<const DexFile>> opened_dex_files;

  ScopedTrace st("Collision check");
  // Add dex files from the oat file to check.
  std::vector<const DexFile*> dex_files_unloaded;
  AddDexFilesFromOat(oat_file, &dex_files_unloaded, &opened_dex_files);

  // Generate type index information for each dex file.
  std::vector<TypeIndexInfo> loaded_types;
  loaded_types.reserve(dex_files_loaded.size());
  for (const DexFile* dex_file : dex_files_loaded) {
    loaded_types.push_back(TypeIndexInfo(dex_file));
  }
  std::vector<TypeIndexInfo> unloaded_types;
  unloaded_types.reserve(dex_files_unloaded.size());
  for (const DexFile* dex_file : dex_files_unloaded) {
    unloaded_types.push_back(TypeIndexInfo(dex_file));
  }

  // Populate the queue of dex file and class pairs with the loaded and unloaded dex files.
  std::priority_queue<DexFileAndClassPair> queue;
  for (size_t i = 0; i < dex_files_loaded.size(); ++i) {
    if (loaded_types[i].GetIterator() != loaded_types[i].GetIteratorEnd()) {
      queue.emplace(dex_files_loaded[i], &loaded_types[i], /*from_loaded_oat=*/true);
    }
  }
  for (size_t i = 0; i < dex_files_unloaded.size(); ++i) {
    if (unloaded_types[i].GetIterator() != unloaded_types[i].GetIteratorEnd()) {
      queue.emplace(dex_files_unloaded[i], &unloaded_types[i], /*from_loaded_oat=*/false);
    }
  }

  // Now drain the queue.
  bool has_duplicates = false;
  error_msg->clear();
  while (!queue.empty()) {
    // Modifying the top element is only safe if we pop right after.
    DexFileAndClassPair compare_pop(queue.top());
    queue.pop();

    // Compare against the following elements.
    while (!queue.empty()) {
      DexFileAndClassPair top(queue.top());
      if (strcmp(compare_pop.GetCachedDescriptor(), top.GetCachedDescriptor()) == 0) {
        // Same descriptor. Check whether it's crossing old-oat-files to new-oat-files.
        if (compare_pop.FromLoadedOat() != top.FromLoadedOat()) {
          error_msg->append(
              StringPrintf("Found duplicated class when checking oat files: '%s' in %s and %s\n",
                           compare_pop.GetCachedDescriptor(),
                           compare_pop.GetDexFile()->GetLocation().c_str(),
                           top.GetDexFile()->GetLocation().c_str()));
          if (!VLOG_IS_ON(oat)) {
            return true;
          }
          has_duplicates = true;
        }
        queue.pop();
        AddNext(top, queue);
      } else {
        // Something else. Done here.
        break;
      }
    }
    AddNext(compare_pop, queue);
  }

  return has_duplicates;
}

// Check for class-def collisions in dex files.
//
// This first walks the class loader chain present in the given context, getting all the dex files
// from the class loader.
//
// If the context is null (which means the initial class loader was null or unsupported)
// this returns false. b/37777332.
//
// This first checks whether all class loaders in the context have the same type and
// classpath. If so, we exit early. Otherwise, we do the collision check.
//
// The collision check works by maintaining a heap with one class from each dex file, sorted by the
// class descriptor. Then a dex-file/class pair is continually removed from the heap and compared
// against the following top element. If the descriptor is the same, it is now checked whether
// the two elements agree on whether their dex file was from an already-loaded oat-file or the
// new oat file. Any disagreement indicates a collision.
OatFileManager::CheckCollisionResult OatFileManager::CheckCollision(
    const OatFile* oat_file,
    const ClassLoaderContext* context,
    /*out*/ std::string* error_msg) const {
  DCHECK(oat_file != nullptr);
  DCHECK(error_msg != nullptr);

  // The context might be null if there are unrecognized class loaders in the chain or they
  // don't meet sensible sanity conditions. In this case we assume that the app knows what it's
  // doing and accept the oat file.
  // Note that this has correctness implications as we cannot guarantee that the class resolution
  // used during compilation is OK (b/37777332).
  if (context == nullptr) {
    LOG(WARNING) << "Skipping duplicate class check due to unsupported classloader";
    return CheckCollisionResult::kSkippedUnsupportedClassLoader;
  }

  // If the oat file loading context matches the context used during compilation then we accept
  // the oat file without addition checks
  ClassLoaderContext::VerificationResult result = context->VerifyClassLoaderContextMatch(
      oat_file->GetClassLoaderContext(),
      /*verify_names=*/ true,
      /*verify_checksums=*/ true);
  switch (result) {
    case ClassLoaderContext::VerificationResult::kForcedToSkipChecks:
      return CheckCollisionResult::kSkippedClassLoaderContextSharedLibrary;
    case ClassLoaderContext::VerificationResult::kMismatch:
      // Mismatched context, do the actual collision check.
      break;
    case ClassLoaderContext::VerificationResult::kVerifies:
      return CheckCollisionResult::kNoCollisions;
  }

  // The class loader context does not match. Perform a full duplicate classes check.
  return CheckClassCollision(oat_file, context, error_msg)
      ? CheckCollisionResult::kPerformedHasCollisions : CheckCollisionResult::kNoCollisions;
}

bool OatFileManager::AcceptOatFile(CheckCollisionResult result) const {
  // Take the file only if it has no collisions, or we must take it because of preopting.
  // Also accept oat files for shared libraries and unsupported class loaders.
  return result != CheckCollisionResult::kPerformedHasCollisions;
}

bool OatFileManager::ShouldLoadAppImage(CheckCollisionResult check_collision_result,
                                        const OatFile* source_oat_file,
                                        ClassLoaderContext* context,
                                        std::string* error_msg) {
  Runtime* const runtime = Runtime::Current();
  if (kEnableAppImage && (!runtime->IsJavaDebuggable() || source_oat_file->IsDebuggable())) {
    // If we verified the class loader context (skipping due to the special marker doesn't
    // count), then also avoid the collision check.
    bool load_image = check_collision_result == CheckCollisionResult::kNoCollisions;
    // If we skipped the collision check, we need to reverify to be sure its OK to load the
    // image.
    if (!load_image &&
        check_collision_result ==
            CheckCollisionResult::kSkippedClassLoaderContextSharedLibrary) {
      // We can load the app image only if there are no collisions. If we know the
      // class loader but didn't do the full collision check in HasCollisions(),
      // do it now. b/77342775
      load_image = !CheckClassCollision(source_oat_file, context, error_msg);
    }
    return load_image;
  }
  return false;
}

std::vector<std::unique_ptr<const DexFile>> OatFileManager::OpenDexFilesFromOat(
    const char* dex_location,
    jobject class_loader,
    jobjectArray dex_elements,
    const OatFile** out_oat_file,
    std::vector<std::string>* error_msgs) {
  ScopedTrace trace(__FUNCTION__);
  CHECK(dex_location != nullptr);
  CHECK(error_msgs != nullptr);

  // Verify we aren't holding the mutator lock, which could starve GC if we
  // have to generate or relocate an oat file.
  Thread* const self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  Runtime* const runtime = Runtime::Current();

  std::unique_ptr<ClassLoaderContext> context;
  // If the class_loader is null there's not much we can do. This happens if a dex files is loaded
  // directly with DexFile APIs instead of using class loaders.
  if (class_loader == nullptr) {
    LOG(WARNING) << "Opening an oat file without a class loader. "
                 << "Are you using the deprecated DexFile APIs?";
    context = nullptr;
  } else {
    context = ClassLoaderContext::CreateContextForClassLoader(class_loader, dex_elements);
  }

  OatFileAssistant oat_file_assistant(dex_location,
                                      kRuntimeISA,
                                      !runtime->IsAotCompiler(),
                                      only_use_system_oat_files_);

  // Get the oat file on disk.
  std::unique_ptr<const OatFile> oat_file(oat_file_assistant.GetBestOatFile().release());
  VLOG(oat) << "OatFileAssistant(" << dex_location << ").GetBestOatFile()="
            << reinterpret_cast<uintptr_t>(oat_file.get())
            << " (executable=" << (oat_file != nullptr ? oat_file->IsExecutable() : false) << ")";

  const OatFile* source_oat_file = nullptr;
  CheckCollisionResult check_collision_result = CheckCollisionResult::kPerformedHasCollisions;
  std::string error_msg;
  if ((class_loader != nullptr || dex_elements != nullptr) && oat_file != nullptr) {
    // Prevent oat files from being loaded if no class_loader or dex_elements are provided.
    // This can happen when the deprecated DexFile.<init>(String) is called directly, and it
    // could load oat files without checking the classpath, which would be incorrect.
    // Take the file only if it has no collisions, or we must take it because of preopting.
    check_collision_result = CheckCollision(oat_file.get(), context.get(), /*out*/ &error_msg);
    bool accept_oat_file = AcceptOatFile(check_collision_result);
    if (!accept_oat_file) {
      // Failed the collision check. Print warning.
      if (runtime->IsDexFileFallbackEnabled()) {
        if (!oat_file_assistant.HasOriginalDexFiles()) {
          // We need to fallback but don't have original dex files. We have to
          // fallback to opening the existing oat file. This is potentially
          // unsafe so we warn about it.
          accept_oat_file = true;

          LOG(WARNING) << "Dex location " << dex_location << " does not seem to include dex file. "
                       << "Allow oat file use. This is potentially dangerous.";
        } else {
          // We have to fallback and found original dex files - extract them from an APK.
          // Also warn about this operation because it's potentially wasteful.
          LOG(WARNING) << "Found duplicate classes, falling back to extracting from APK : "
                       << dex_location;
          LOG(WARNING) << "NOTE: This wastes RAM and hurts startup performance.";
        }
      } else {
        // TODO: We should remove this. The fact that we're here implies -Xno-dex-file-fallback
        // was set, which means that we should never fallback. If we don't have original dex
        // files, we should just fail resolution as the flag intended.
        if (!oat_file_assistant.HasOriginalDexFiles()) {
          accept_oat_file = true;
        }

        LOG(WARNING) << "Found duplicate classes, dex-file-fallback disabled, will be failing to "
                        " load classes for " << dex_location;
      }

      LOG(WARNING) << error_msg;
    }

    if (accept_oat_file) {
      VLOG(class_linker) << "Registering " << oat_file->GetLocation();
      source_oat_file = RegisterOatFile(std::move(oat_file));
      *out_oat_file = source_oat_file;
    }
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;

  // Load the dex files from the oat file.
  if (source_oat_file != nullptr) {
    bool added_image_space = false;
    if (source_oat_file->IsExecutable()) {
      ScopedTrace app_image_timing("AppImage:Loading");

      // We need to throw away the image space if we are debuggable but the oat-file source of the
      // image is not otherwise we might get classes with inlined methods or other such things.
      std::unique_ptr<gc::space::ImageSpace> image_space;
      if (ShouldLoadAppImage(check_collision_result,
                             source_oat_file,
                             context.get(),
                             &error_msg)) {
        image_space = oat_file_assistant.OpenImageSpace(source_oat_file);
      }
      if (image_space != nullptr) {
        ScopedObjectAccess soa(self);
        StackHandleScope<1> hs(self);
        Handle<mirror::ClassLoader> h_loader(
            hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
        // Can not load app image without class loader.
        if (h_loader != nullptr) {
          std::string temp_error_msg;
          // Add image space has a race condition since other threads could be reading from the
          // spaces array.
          {
            ScopedThreadSuspension sts(self, kSuspended);
            gc::ScopedGCCriticalSection gcs(self,
                                            gc::kGcCauseAddRemoveAppImageSpace,
                                            gc::kCollectorTypeAddRemoveAppImageSpace);
            ScopedSuspendAll ssa("Add image space");
            runtime->GetHeap()->AddSpace(image_space.get());
          }
          {
            ScopedTrace trace2(StringPrintf("Adding image space for location %s", dex_location));
            added_image_space = runtime->GetClassLinker()->AddImageSpace(image_space.get(),
                                                                         h_loader,
                                                                         dex_elements,
                                                                         dex_location,
                                                                         /*out*/&dex_files,
                                                                         /*out*/&temp_error_msg);
          }
          if (added_image_space) {
            // Successfully added image space to heap, release the map so that it does not get
            // freed.
            image_space.release();  // NOLINT b/117926937

            // Register for tracking.
            for (const auto& dex_file : dex_files) {
              dex::tracking::RegisterDexFile(dex_file.get());
            }
          } else {
            LOG(INFO) << "Failed to add image file " << temp_error_msg;
            dex_files.clear();
            {
              ScopedThreadSuspension sts(self, kSuspended);
              gc::ScopedGCCriticalSection gcs(self,
                                              gc::kGcCauseAddRemoveAppImageSpace,
                                              gc::kCollectorTypeAddRemoveAppImageSpace);
              ScopedSuspendAll ssa("Remove image space");
              runtime->GetHeap()->RemoveSpace(image_space.get());
            }
            // Non-fatal, don't update error_msg.
          }
        }
      }
    }
    if (!added_image_space) {
      DCHECK(dex_files.empty());
      dex_files = oat_file_assistant.LoadDexFiles(*source_oat_file, dex_location);

      // Register for tracking.
      for (const auto& dex_file : dex_files) {
        dex::tracking::RegisterDexFile(dex_file.get());
      }
    }
    if (dex_files.empty()) {
      error_msgs->push_back("Failed to open dex files from " + source_oat_file->GetLocation());
    } else {
      // Opened dex files from an oat file, madvise them to their loaded state.
       for (const std::unique_ptr<const DexFile>& dex_file : dex_files) {
         OatDexFile::MadviseDexFile(*dex_file, MadviseState::kMadviseStateAtLoad);
       }
    }
  }

  // Fall back to running out of the original dex file if we couldn't load any
  // dex_files from the oat file.
  if (dex_files.empty()) {
    if (oat_file_assistant.HasOriginalDexFiles()) {
      if (Runtime::Current()->IsDexFileFallbackEnabled()) {
        static constexpr bool kVerifyChecksum = true;
        const ArtDexFileLoader dex_file_loader;
        if (!dex_file_loader.Open(dex_location,
                                  dex_location,
                                  Runtime::Current()->IsVerificationEnabled(),
                                  kVerifyChecksum,
                                  /*out*/ &error_msg,
                                  &dex_files)) {
          LOG(WARNING) << error_msg;
          error_msgs->push_back("Failed to open dex files from " + std::string(dex_location)
                                + " because: " + error_msg);
        }
      } else {
        error_msgs->push_back("Fallback mode disabled, skipping dex files.");
      }
    } else {
      error_msgs->push_back("No original dex files found for dex location "
          + std::string(dex_location));
    }
  }

  return dex_files;
}

void OatFileManager::SetOnlyUseSystemOatFiles(bool assert_no_files_loaded) {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  if (assert_no_files_loaded) {
    // Make sure all files that were loaded up to this point are on /system. Skip the image
    // files.
    std::vector<const OatFile*> boot_vector = GetBootOatFiles();
    std::unordered_set<const OatFile*> boot_set(boot_vector.begin(), boot_vector.end());

    for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
      if (boot_set.find(oat_file.get()) == boot_set.end()) {
        CHECK(LocationIsOnSystem(oat_file->GetLocation().c_str())) << oat_file->GetLocation();
      }
    }
  }
  only_use_system_oat_files_ = true;
}

void OatFileManager::DumpForSigQuit(std::ostream& os) {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  std::vector<const OatFile*> boot_oat_files = GetBootOatFiles();
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    if (ContainsElement(boot_oat_files, oat_file.get())) {
      continue;
    }
    os << oat_file->GetLocation() << ": " << oat_file->GetCompilerFilter() << "\n";
  }
}

}  // namespace art
