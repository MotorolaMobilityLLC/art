#
# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include $(CLEAR_VARS)

local_module_tags := tests

TEST_TARGET_ARCH := $(HOST_ARCH)
include $(build_path)/Android.common.mk
local_cpp_extension := $(LOCAL_CPP_EXTENSION)
local_cflags := $(LOCAL_CFLAGS)

local_shared_libraries := \
	libart

local_c_includes := \
	external/gtest/include

local_whole_static_libraries := \
	libgtest_host \
	libgtest_main_host

$(foreach file,$(TEST_LOCAL_SRC_FILES), \
  $(eval include $(CLEAR_VARS)) \
  $(eval LOCAL_CPP_EXTENSION := $(local_cpp_extension)) \
  $(eval LOCAL_MODULE := $(notdir $(file:%.cc=%))) \
  $(eval LOCAL_MODULE_TAGS := $(local_module_tags)) \
  $(eval LOCAL_SRC_FILES := $(file)) \
  $(eval LOCAL_CFLAGS := $(local_cflags)) \
  $(eval LOCAL_C_INCLUDES := $(local_c_includes)) \
  $(eval LOCAL_WHOLE_STATIC_LIBRARIES := $(local_whole_static_libraries)) \
  $(eval LOCAL_SHARED_LIBRARIES := $(local_shared_libraries)) \
  $(eval include $(BUILD_HOST_EXECUTABLE)) \
)
