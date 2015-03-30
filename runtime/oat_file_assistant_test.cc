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

#include "oat_file_assistant.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <sys/param.h>

#include <backtrace/BacktraceMap.h>
#include <gtest/gtest.h>

#include "class_linker.h"
#include "common_runtime_test.h"
#include "compiler_callbacks.h"
#include "mem_map.h"
#include "os.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

class OatFileAssistantTest : public CommonRuntimeTest {
 public:
  virtual void SetUp() {
    ReserveImageSpace();
    CommonRuntimeTest::SetUp();

    // Create a scratch directory to work from.
    scratch_dir_ = android_data_ + "/OatFileAssistantTest";
    ASSERT_EQ(0, mkdir(scratch_dir_.c_str(), 0700));

    // Create a subdirectory in scratch for odex files.
    odex_oat_dir_ = scratch_dir_ + "/oat";
    ASSERT_EQ(0, mkdir(odex_oat_dir_.c_str(), 0700));

    odex_dir_ = odex_oat_dir_ + "/" + std::string(GetInstructionSetString(kRuntimeISA));
    ASSERT_EQ(0, mkdir(odex_dir_.c_str(), 0700));


    // Verify the environment is as we expect
    uint32_t checksum;
    std::string error_msg;
    ASSERT_TRUE(OS::FileExists(GetImageFile().c_str()))
      << "Expected pre-compiled boot image to be at: " << GetImageFile();
    ASSERT_TRUE(OS::FileExists(GetDexSrc1().c_str()))
      << "Expected dex file to be at: " << GetDexSrc1();
    ASSERT_TRUE(OS::FileExists(GetStrippedDexSrc1().c_str()))
      << "Expected stripped dex file to be at: " << GetStrippedDexSrc1();
    ASSERT_FALSE(DexFile::GetChecksum(GetStrippedDexSrc1().c_str(), &checksum, &error_msg))
      << "Expected stripped dex file to be stripped: " << GetStrippedDexSrc1();
    ASSERT_TRUE(OS::FileExists(GetMultiDexSrc1().c_str()))
      << "Expected multidex file to be at: " << GetMultiDexSrc1();
    ASSERT_TRUE(OS::FileExists(GetDexSrc2().c_str()))
      << "Expected dex file to be at: " << GetDexSrc2();
  }

  virtual void SetUpRuntimeOptions(RuntimeOptions* options) {
    // options->push_back(std::make_pair("-verbose:oat", nullptr));

    // Set up the image location.
    options->push_back(std::make_pair("-Ximage:" + GetImageLocation(),
          nullptr));
    // Make sure compilercallbacks are not set so that relocation will be
    // enabled.
    callbacks_.reset();
  }

  virtual void PreRuntimeCreate() {
    UnreserveImageSpace();
  }

  virtual void PostRuntimeCreate() {
    ReserveImageSpace();
  }

  virtual void TearDown() {
    ClearDirectory(odex_dir_.c_str());
    ASSERT_EQ(0, rmdir(odex_dir_.c_str()));

    ClearDirectory(odex_oat_dir_.c_str());
    ASSERT_EQ(0, rmdir(odex_oat_dir_.c_str()));

    ClearDirectory(scratch_dir_.c_str());
    ASSERT_EQ(0, rmdir(scratch_dir_.c_str()));

    CommonRuntimeTest::TearDown();
  }

  void Copy(std::string src, std::string dst) {
    std::ifstream  src_stream(src, std::ios::binary);
    std::ofstream  dst_stream(dst, std::ios::binary);

    dst_stream << src_stream.rdbuf();
  }

  // Returns the directory where the pre-compiled core.art can be found.
  // TODO: We should factor out this into common tests somewhere rather than
  // re-hardcoding it here (This was copied originally from the elf writer
  // test).
  std::string GetImageDirectory() {
    if (IsHost()) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return std::string(host_dir) + "/framework";
    } else {
      return std::string("/data/art-test");
    }
  }

  std::string GetImageLocation() {
    return GetImageDirectory() + "/core.art";
  }

  std::string GetImageFile() {
    return GetImageDirectory() + "/" + GetInstructionSetString(kRuntimeISA)
      + "/core.art";
  }

  std::string GetDexSrc1() {
    return GetTestDexFileName("Main");
  }

  // Returns the path to a dex file equivalent to GetDexSrc1, but with the dex
  // file stripped.
  std::string GetStrippedDexSrc1() {
    return GetTestDexFileName("MainStripped");
  }

  std::string GetMultiDexSrc1() {
    return GetTestDexFileName("MultiDex");
  }

  std::string GetDexSrc2() {
    return GetTestDexFileName("Nested");
  }

  // Scratch directory, for dex and odex files (oat files will go in the
  // dalvik cache).
  std::string GetScratchDir() {
    return scratch_dir_;
  }

  // Odex directory is the subdirectory in the scratch directory where odex
  // files should be located.
  std::string GetOdexDir() {
    return odex_dir_;
  }

  // Generate an odex file for the purposes of test.
  // If pic is true, generates a PIC odex.
  void GenerateOdexForTest(const std::string& dex_location,
                           const std::string& odex_location,
                           bool pic = false) {
    // For this operation, we temporarily redirect the dalvik cache so dex2oat
    // doesn't find the relocated image file.
    std::string android_data_tmp = GetScratchDir() + "AndroidDataTmp";
    setenv("ANDROID_DATA", android_data_tmp.c_str(), 1);
    std::vector<std::string> args;
    args.push_back("--dex-file=" + dex_location);
    args.push_back("--oat-file=" + odex_location);
    if (pic) {
      args.push_back("--compile-pic");
    } else {
      args.push_back("--include-patch-information");

      // We need to use the quick compiler to generate non-PIC code, because
      // the optimizing compiler always generates PIC.
      args.push_back("--compiler-backend=Quick");
    }
    args.push_back("--runtime-arg");
    args.push_back("-Xnorelocate");
    std::string error_msg;
    ASSERT_TRUE(OatFileAssistant::Dex2Oat(args, &error_msg)) << error_msg;
    setenv("ANDROID_DATA", android_data_.c_str(), 1);
  }

  void GeneratePicOdexForTest(const std::string& dex_location,
                              const std::string& odex_location) {
    GenerateOdexForTest(dex_location, odex_location, true);
  }

 private:
  // Reserve memory around where the image will be loaded so other memory
  // won't conflict when it comes time to load the image.
  // This can be called with an already loaded image to reserve the space
  // around it.
  void ReserveImageSpace() {
    MemMap::Init();

    // Ensure a chunk of memory is reserved for the image space.
    uintptr_t reservation_start = ART_BASE_ADDRESS + ART_BASE_ADDRESS_MIN_DELTA;
    uintptr_t reservation_end = ART_BASE_ADDRESS + ART_BASE_ADDRESS_MAX_DELTA
        // Include the main space that has to come right after the
        // image in case of the GSS collector.
        + 384 * MB;

    std::string error_msg;
    std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(getpid(), true));
    ASSERT_TRUE(map.get() != nullptr) << "Failed to build process map";
    for (BacktraceMap::const_iterator it = map->begin();
        reservation_start < reservation_end && it != map->end(); ++it) {
      if (it->end <= reservation_start) {
        continue;
      }

      if (it->start < reservation_start) {
        reservation_start = std::min(reservation_end, it->end);
      }

      image_reservation_.push_back(std::unique_ptr<MemMap>(
          MemMap::MapAnonymous("image reservation",
              reinterpret_cast<uint8_t*>(reservation_start),
              std::min(it->start, reservation_end) - reservation_start,
              PROT_NONE, false, false, &error_msg)));
      ASSERT_TRUE(image_reservation_.back().get() != nullptr) << error_msg;
      LOG(INFO) << "Reserved space for image " <<
        reinterpret_cast<void*>(image_reservation_.back()->Begin()) << "-" <<
        reinterpret_cast<void*>(image_reservation_.back()->End());
      reservation_start = it->end;
    }
  }


  // Unreserve any memory reserved by ReserveImageSpace. This should be called
  // before the image is loaded.
  void UnreserveImageSpace() {
    image_reservation_.clear();
  }

  std::string scratch_dir_;
  std::string odex_oat_dir_;
  std::string odex_dir_;
  std::vector<std::unique_ptr<MemMap>> image_reservation_;
};

class OatFileAssistantNoDex2OatTest : public OatFileAssistantTest {
 public:
  virtual void SetUpRuntimeOptions(RuntimeOptions* options) {
    OatFileAssistantTest::SetUpRuntimeOptions(options);
    options->push_back(std::make_pair("-Xnodex2oat", nullptr));
  }
};

// Generate an oat file for the purposes of test, as opposed to testing
// generation of oat files.
static void GenerateOatForTest(const char* dex_location) {
  OatFileAssistant oat_file_assistant(dex_location, kRuntimeISA, false);

  std::string error_msg;
  ASSERT_TRUE(oat_file_assistant.GenerateOatFile(&error_msg)) << error_msg;
}

// Case: We have a DEX file, but no OAT file for it.
// Expect: The oat file status is kOutOfDate.
TEST_F(OatFileAssistantTest, DexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.OdexFileStatus());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.OatFileStatus());
}

// Case: We have no DEX file and no OAT file.
// Expect: Status is out of date. Loading should fail, but not crash.
TEST_F(OatFileAssistantTest, NoDexNoOat) {
  std::string dex_location = GetScratchDir() + "/NoDexNoOat.jar";

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  EXPECT_EQ(nullptr, oat_file.get());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: The oat file status is kUpToDate.
TEST_F(OatFileAssistantTest, OatUpToDate) {
  std::string dex_location = GetScratchDir() + "/OatUpToDate.jar";
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str());

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  EXPECT_EQ(OatFileAssistant::kUpToDate, oat_file_assistant.GetStatus());
  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kUpToDate, oat_file_assistant.OatFileStatus());
}

// Case: We have a MultiDEX file and up-to-date OAT file for it.
// Expect: The oat file status is kUpToDate.
TEST_F(OatFileAssistantTest, MultiDexOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/MultiDexOatUpToDate.jar";
  Copy(GetMultiDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str());

  // Verify we can load both dex files.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

// Case: We have a MultiDEX file and up-to-date OAT file for it with relative
// encoded dex locations.
// Expect: The oat file status is kUpToDate.
TEST_F(OatFileAssistantTest, RelativeEncodedDexLocation) {
  std::string dex_location = GetScratchDir() + "/RelativeEncodedDexLocation.jar";
  std::string oat_location = GetOdexDir() + "/RelativeEncodedDexLocation.oat";

  // Create the dex file
  Copy(GetMultiDexSrc1(), dex_location);

  // Create the oat file with relative encoded dex location.
  std::vector<std::string> args;
  args.push_back("--dex-file=" + dex_location);
  args.push_back("--dex-location=" + std::string("RelativeEncodedDexLocation.jar"));
  args.push_back("--oat-file=" + oat_location);

  std::string error_msg;
  ASSERT_TRUE(OatFileAssistant::Dex2Oat(args, &error_msg)) << error_msg;

  // Verify we can load both dex files.
  OatFileAssistant oat_file_assistant(dex_location.c_str(),
                                      oat_location.c_str(),
                                      kRuntimeISA, true);
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

// Case: We have a DEX file and out of date OAT file.
// Expect: The oat file status is kOutOfDate.
TEST_F(OatFileAssistantTest, OatOutOfDate) {
  std::string dex_location = GetScratchDir() + "/OatOutOfDate.jar";

  // We create a dex, generate an oat for it, then overwrite the dex with a
  // different dex to make the oat out of date.
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str());
  Copy(GetDexSrc2(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// Case: We have a DEX file and an ODEX file, but no OAT file.
// Expect: The oat file status is kNeedsRelocation.
TEST_F(OatFileAssistantTest, DexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  EXPECT_EQ(OatFileAssistant::kNeedsRelocation, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_EQ(OatFileAssistant::kNeedsRelocation, oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// Case: We have a stripped DEX file and an ODEX file, but no OAT file.
// Expect: The oat file status is kNeedsRelocation.
TEST_F(OatFileAssistantTest, StrippedDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/StrippedDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/StrippedDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Strip the dex file
  Copy(GetStrippedDexSrc1(), dex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_EQ(OatFileAssistant::kNeedsRelocation, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_TRUE(oat_file_assistant.MakeUpToDate(&error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kUpToDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());

  // Verify we can load the dex files from it.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a stripped DEX file, an ODEX file, and an out of date OAT file.
// Expect: The oat file status is kNeedsRelocation.
TEST_F(OatFileAssistantTest, StrippedDexOdexOat) {
  std::string dex_location = GetScratchDir() + "/StrippedDexOdexOat.jar";
  std::string odex_location = GetOdexDir() + "/StrippedDexOdexOat.odex";

  // Create the oat file from a different dex file so it looks out of date.
  Copy(GetDexSrc2(), dex_location);
  GenerateOatForTest(dex_location.c_str());

  // Create the odex file
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Strip the dex file.
  Copy(GetStrippedDexSrc1(), dex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_EQ(OatFileAssistant::kNeedsRelocation, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_TRUE(oat_file_assistant.MakeUpToDate(&error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kUpToDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());

  // Verify we can load the dex files from it.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file, an ODEX file and an OAT file, where the ODEX and
// OAT files both have patch delta of 0.
// Expect: It shouldn't crash.
TEST_F(OatFileAssistantTest, OdexOatOverlap) {
  std::string dex_location = GetScratchDir() + "/OdexOatOverlap.jar";
  std::string odex_location = GetOdexDir() + "/OdexOatOverlap.odex";
  std::string oat_location = GetOdexDir() + "/OdexOatOverlap.oat";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Create the oat file by copying the odex so they are located in the same
  // place in memory.
  Copy(odex_location, oat_location);

  // Verify things don't go bad.
  OatFileAssistant oat_file_assistant(dex_location.c_str(),
      oat_location.c_str(), kRuntimeISA, true);

  EXPECT_EQ(OatFileAssistant::kNeedsRelocation, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());

  // Things aren't relocated, so it should fall back to interpreted.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file and a PIC ODEX file, but no OAT file.
// Expect: The oat file status is kUpToDate, because PIC needs no relocation.
TEST_F(OatFileAssistantTest, DexPicOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexPicOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexPicOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GeneratePicOdexForTest(dex_location, odex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  EXPECT_EQ(OatFileAssistant::kUpToDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: We should load an executable dex file.
TEST_F(OatFileAssistantTest, LoadOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/LoadOatUpToDate.jar";

  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str());

  // Load the oat using an oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: Loading non-executable should load the oat non-executable.
TEST_F(OatFileAssistantTest, LoadNoExecOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/LoadNoExecOatUpToDate.jar";

  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str());

  // Load the oat using an oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file.
// Expect: We should load an executable dex file from an alternative oat
// location.
TEST_F(OatFileAssistantTest, LoadDexNoAlternateOat) {
  std::string dex_location = GetScratchDir() + "/LoadDexNoAlternateOat.jar";
  std::string oat_location = GetScratchDir() + "/LoadDexNoAlternateOat.oat";

  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(
      dex_location.c_str(), oat_location.c_str(), kRuntimeISA, true);
  std::string error_msg;
  ASSERT_TRUE(oat_file_assistant.MakeUpToDate(&error_msg)) << error_msg;

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());

  EXPECT_TRUE(OS::FileExists(oat_location.c_str()));

  // Verify it didn't create an oat in the default location.
  OatFileAssistant ofm(dex_location.c_str(), kRuntimeISA, false);
  EXPECT_FALSE(ofm.OatFileExists());
}

// Case: Non-existent Dex location.
// Expect: The dex code is out of date, and trying to update it fails.
TEST_F(OatFileAssistantTest, NonExsistentDexLocation) {
  std::string dex_location = GetScratchDir() + "/BadDexLocation.jar";

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());

  std::string error_msg;
  EXPECT_FALSE(oat_file_assistant.MakeUpToDate(&error_msg));
  EXPECT_FALSE(error_msg.empty());
}

// Turn an absolute path into a path relative to the current working
// directory.
static std::string MakePathRelative(std::string target) {
  char buf[MAXPATHLEN];
  std::string cwd = getcwd(buf, MAXPATHLEN);

  // Split the target and cwd paths into components.
  std::vector<std::string> target_path;
  std::vector<std::string> cwd_path;
  Split(target, '/', &target_path);
  Split(cwd, '/', &cwd_path);

  // Reverse the path components, so we can use pop_back().
  std::reverse(target_path.begin(), target_path.end());
  std::reverse(cwd_path.begin(), cwd_path.end());

  // Drop the common prefix of the paths. Because we reversed the path
  // components, this becomes the common suffix of target_path and cwd_path.
  while (!target_path.empty() && !cwd_path.empty()
      && target_path.back() == cwd_path.back()) {
    target_path.pop_back();
    cwd_path.pop_back();
  }

  // For each element of the remaining cwd_path, add '..' to the beginning
  // of the target path. Because we reversed the path components, we add to
  // the end of target_path.
  for (unsigned int i = 0; i < cwd_path.size(); i++) {
    target_path.push_back("..");
  }

  // Reverse again to get the right path order, and join to get the result.
  std::reverse(target_path.begin(), target_path.end());
  return Join(target_path, '/');
}

// Case: Non-absolute path to Dex location.
// Expect: Not sure, but it shouldn't crash.
TEST_F(OatFileAssistantTest, NonAbsoluteDexLocation) {
  std::string abs_dex_location = GetScratchDir() + "/NonAbsoluteDexLocation.jar";
  Copy(GetDexSrc1(), abs_dex_location);

  std::string dex_location = MakePathRelative(abs_dex_location);
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// Case: Very short, non-existent Dex location.
// Expect: Dex code is out of date, and trying to update it fails.
TEST_F(OatFileAssistantTest, ShortDexLocation) {
  std::string dex_location = "/xx";

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());

  std::string error_msg;
  EXPECT_FALSE(oat_file_assistant.MakeUpToDate(&error_msg));
  EXPECT_FALSE(error_msg.empty());
}

// Case: Non-standard extension for dex file.
// Expect: The oat file status is kOutOfDate.
TEST_F(OatFileAssistantTest, LongDexExtension) {
  std::string dex_location = GetScratchDir() + "/LongDexExtension.jarx";
  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false);

  EXPECT_EQ(OatFileAssistant::kOutOfDate, oat_file_assistant.GetStatus());

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// A task to generate a dex location. Used by the RaceToGenerate test.
class RaceGenerateTask : public Task {
 public:
  explicit RaceGenerateTask(const std::string& dex_location, const std::string& oat_location)
    : dex_location_(dex_location), oat_location_(oat_location),
      loaded_oat_file_(nullptr)
  {}

  void Run(Thread* self) {
    UNUSED(self);

    // Load the dex files, and save a pointer to the loaded oat file, so that
    // we can verify only one oat file was loaded for the dex location.
    ClassLinker* linker = Runtime::Current()->GetClassLinker();
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    std::vector<std::string> error_msgs;
    dex_files = linker->OpenDexFilesFromOat(dex_location_.c_str(), oat_location_.c_str(), &error_msgs);
    CHECK(!dex_files.empty()) << Join(error_msgs, '\n');
    loaded_oat_file_ = dex_files[0]->GetOatFile();
  }

  const OatFile* GetLoadedOatFile() const {
    return loaded_oat_file_;
  }

 private:
  std::string dex_location_;
  std::string oat_location_;
  const OatFile* loaded_oat_file_;
};

// Test the case where multiple processes race to generate an oat file.
// This simulates multiple processes using multiple threads.
//
// We want only one Oat file to be loaded when there is a race to load, to
// avoid using up the virtual memory address space.
TEST_F(OatFileAssistantTest, RaceToGenerate) {
  std::string dex_location = GetScratchDir() + "/RaceToGenerate.jar";
  std::string oat_location = GetOdexDir() + "/RaceToGenerate.oat";

  // We use the lib core dex file, because it's large, and hopefully should
  // take a while to generate.
  Copy(GetLibCoreDexFileName(), dex_location);

  const int kNumThreads = 32;
  Thread* self = Thread::Current();
  ThreadPool thread_pool("Oat file assistant test thread pool", kNumThreads);
  std::vector<std::unique_ptr<RaceGenerateTask>> tasks;
  for (int i = 0; i < kNumThreads; i++) {
    std::unique_ptr<RaceGenerateTask> task(new RaceGenerateTask(dex_location, oat_location));
    thread_pool.AddTask(self, task.get());
    tasks.push_back(std::move(task));
  }
  thread_pool.StartWorkers(self);
  thread_pool.Wait(self, true, false);

  // Verify every task got the same pointer.
  const OatFile* expected = tasks[0]->GetLoadedOatFile();
  for (auto& task : tasks) {
    EXPECT_EQ(expected, task->GetLoadedOatFile());
  }
}

// Case: We have a DEX file and an ODEX file, no OAT file, and dex2oat is
// disabled.
// Expect: We should load the odex file non-executable.
TEST_F(OatFileAssistantNoDex2OatTest, LoadDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/LoadDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/LoadDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Load the oat using an executable oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a MultiDEX file and an ODEX file, no OAT file, and dex2oat is
// disabled.
// Expect: We should load the odex file non-executable.
TEST_F(OatFileAssistantNoDex2OatTest, LoadMultiDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/LoadMultiDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/LoadMultiDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetMultiDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location);

  // Load the oat using an executable oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

TEST(OatFileAssistantUtilsTest, DexFilenameToOdexFilename) {
  std::string error_msg;
  std::string odex_file;

  EXPECT_TRUE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz.jar", kArm, &odex_file, &error_msg)) << error_msg;
  EXPECT_EQ("/foo/bar/oat/arm/baz.odex", odex_file);

  EXPECT_TRUE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz.funnyext", kArm, &odex_file, &error_msg)) << error_msg;
  EXPECT_EQ("/foo/bar/oat/arm/baz.odex", odex_file);

  EXPECT_FALSE(OatFileAssistant::DexFilenameToOdexFilename(
        "nopath.jar", kArm, &odex_file, &error_msg));
  EXPECT_FALSE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz_noext", kArm, &odex_file, &error_msg));
}


// TODO: More Tests:
//  * Test class linker falls back to unquickened dex for DexNoOat
//  * Test class linker falls back to unquickened dex for MultiDexNoOat
//  * Test multidex files:
//     - Multidex with only classes2.dex out of date should have status
//       kOutOfDate
//  * Test using secondary isa
//  * Test with profiling info?
//  * Test for status of oat while oat is being generated (how?)
//  * Test case where 32 and 64 bit boot class paths differ,
//      and we ask IsInBootClassPath for a class in exactly one of the 32 or
//      64 bit boot class paths.
//  * Test unexpected scenarios (?):
//    - Dex is stripped, don't have odex.
//    - Oat file corrupted after status check, before reload unexecutable
//    because it's unrelocated and no dex2oat

}  // namespace art
