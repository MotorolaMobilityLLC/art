/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include <string>
#include <vector>
#include <sstream>

#include <sys/types.h>
#include <unistd.h>

#include "base/stringprintf.h"
#include "common_runtime_test.h"
#include "utils.h"

namespace art {

class DexLayoutTest : public CommonRuntimeTest {
 protected:
  virtual void SetUp() {
    CommonRuntimeTest::SetUp();
    // TODO: Test with other dex files for improved coverage.
    // Dogfood our own lib core dex file.
    dex_file_ = GetLibCoreDexFileNames()[0];
  }

  // Runs FullPlainOutput test.
  bool FullPlainOutputExec(std::string* error_msg) {
    // TODO: dexdump2 -> dexdump ?
    ScratchFile dexdump_output;
    std::string dexdump_filename = dexdump_output.GetFilename();
    std::string dexdump = GetTestAndroidRoot() + "/bin/dexdump2";
    EXPECT_TRUE(OS::FileExists(dexdump.c_str())) << dexdump << " should be a valid file path";
    std::vector<std::string> dexdump_exec_argv =
        { dexdump, "-d", "-f", "-h", "-l", "plain", "-o", dexdump_filename, dex_file_ };

    ScratchFile dexlayout_output;
    std::string dexlayout_filename = dexlayout_output.GetFilename();
    std::string dexlayout = GetTestAndroidRoot() + "/bin/dexlayout";
    EXPECT_TRUE(OS::FileExists(dexlayout.c_str())) << dexlayout << " should be a valid file path";
    std::vector<std::string> dexlayout_exec_argv =
        { dexlayout, "-d", "-f", "-h", "-l", "plain", "-o", dexlayout_filename, dex_file_ };

    if (!::art::Exec(dexdump_exec_argv, error_msg)) {
      return false;
    }
    if (!::art::Exec(dexlayout_exec_argv, error_msg)) {
      return false;
    }
    std::vector<std::string> diff_exec_argv =
        { "/usr/bin/diff", dexdump_filename, dexlayout_filename };
    if (!::art::Exec(diff_exec_argv, error_msg)) {
      return false;
    }
    return true;
  }

  // Runs FullPlainOutput test.
  bool DexFileOutputExec(std::string* error_msg) {
    ScratchFile dexlayout_output;
    std::string dexlayout_filename = dexlayout_output.GetFilename();
    std::string dexlayout = GetTestAndroidRoot() + "/bin/dexlayout";
    EXPECT_TRUE(OS::FileExists(dexlayout.c_str())) << dexlayout << " should be a valid file path";
    std::vector<std::string> dexlayout_exec_argv =
        { dexlayout, "-d", "-f", "-h", "-l", "plain", "-w", "-o", dexlayout_filename, dex_file_ };

    if (!::art::Exec(dexlayout_exec_argv, error_msg)) {
      return false;
    }

    size_t last_slash = dexlayout_filename.rfind("/");
    std::string scratch_directory = dexlayout_filename.substr(0, last_slash + 1);
    std::vector<std::string> unzip_exec_argv =
        { "/usr/bin/unzip", dex_file_, "classes.dex", "-d", scratch_directory};
    if (!::art::Exec(unzip_exec_argv, error_msg)) {
      return false;
    }

    std::vector<std::string> diff_exec_argv =
        { "/usr/bin/diff", scratch_directory + "classes.dex" , dex_file_ + ".out" };
    if (!::art::Exec(diff_exec_argv, error_msg)) {
      return false;
    }

    std::vector<std::string> rm_exec_argv = { "/bin/rm", scratch_directory + "classes.dex" };
    if (!::art::Exec(rm_exec_argv, error_msg)) {
      return false;
    }

    return true;
  }

  std::string dex_file_;
};


TEST_F(DexLayoutTest, FullPlainOutput) {
  // Disable test on target.
  TEST_DISABLED_FOR_TARGET();
  std::string error_msg;
  ASSERT_TRUE(FullPlainOutputExec(&error_msg)) << error_msg;
}

TEST_F(DexLayoutTest, DexFileOutput) {
  // Disable test on target.
  TEST_DISABLED_FOR_TARGET();
  std::string error_msg;
  ASSERT_TRUE(DexFileOutputExec(&error_msg)) << error_msg;
}

}  // namespace art
