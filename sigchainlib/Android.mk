#
# Copyright (C) 2014 The Android Open Source Project
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

LOCAL_PATH:= $(call my-dir)

include art/build/Android.common.mk

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
LOCAL_SRC_FILES := sigchain.cc
LOCAL_MODULE:= libsigchain
LOCAL_SHARED_LIBRARIES += liblog libdl
LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
include $(BUILD_SHARED_LIBRARY)
