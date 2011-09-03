// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include "UniquePtr.h"
#include "common_test.h"

namespace art {

class RuntimeTest : public CommonTest {};

TEST_F(RuntimeTest, ParsedOptions) {
  void* test_vfprintf = reinterpret_cast<void*>(0xa);
  void* test_abort = reinterpret_cast<void*>(0xb);
  void* test_exit = reinterpret_cast<void*>(0xc);
  void* null = reinterpret_cast<void*>(NULL);

  std::string lib_core = GetLibCoreDexFileName();

  std::string boot_class_path;
  boot_class_path += "-Xbootclasspath:";
  boot_class_path += lib_core;

  Runtime::Options options;
  options.push_back(std::make_pair(boot_class_path.c_str(), null));
  options.push_back(std::make_pair("-classpath", null));
  options.push_back(std::make_pair(lib_core.c_str(), null));
  options.push_back(std::make_pair("-cp", null));
  options.push_back(std::make_pair(lib_core.c_str(), null));
  options.push_back(std::make_pair("-Xbootimage:boot_image", null));
  options.push_back(std::make_pair("-Ximage:image_1", null));
  options.push_back(std::make_pair("-Ximage:image_2", null));
  options.push_back(std::make_pair("-Xcheck:jni", null));
  options.push_back(std::make_pair("-Xms2048", null));
  options.push_back(std::make_pair("-Xmx4k", null));
  options.push_back(std::make_pair("-Xss1m", null));
  options.push_back(std::make_pair("-Dfoo=bar", null));
  options.push_back(std::make_pair("-Dbaz=qux", null));
  options.push_back(std::make_pair("-verbose:gc,class,jni", null));
  options.push_back(std::make_pair("vfprintf", test_vfprintf));
  options.push_back(std::make_pair("abort", test_abort));
  options.push_back(std::make_pair("exit", test_exit));
  UniquePtr<Runtime::ParsedOptions> parsed(Runtime::ParsedOptions::Create(options, false));
  ASSERT_TRUE(parsed.get() != NULL);

  EXPECT_EQ(1U, parsed->boot_class_path_.size());
  EXPECT_EQ(1U, parsed->class_path_.size());
  EXPECT_STREQ("boot_image", parsed->boot_image_);
  EXPECT_EQ(2U, parsed->images_.size());
  EXPECT_STREQ("image_1", parsed->images_[0]);
  EXPECT_STREQ("image_2", parsed->images_[1]);
  EXPECT_EQ(true, parsed->check_jni_);
  EXPECT_EQ(2048U, parsed->heap_initial_size_);
  EXPECT_EQ(4 * KB, parsed->heap_maximum_size_);
  EXPECT_EQ(1 * MB, parsed->stack_size_);
  EXPECT_TRUE(test_vfprintf == parsed->hook_vfprintf_);
  EXPECT_TRUE(test_exit == parsed->hook_exit_);
  EXPECT_TRUE(test_abort == parsed->hook_abort_);
  ASSERT_EQ(3U, parsed->verbose_.size());
  EXPECT_TRUE(parsed->verbose_.find("gc") != parsed->verbose_.end());
  EXPECT_TRUE(parsed->verbose_.find("class") != parsed->verbose_.end());
  EXPECT_TRUE(parsed->verbose_.find("jni") != parsed->verbose_.end());
  ASSERT_EQ(2U, parsed->properties_.size());
  EXPECT_EQ("foo=bar", parsed->properties_[0]);
  EXPECT_EQ("baz=qux", parsed->properties_[1]);
}

}  // namespace art
