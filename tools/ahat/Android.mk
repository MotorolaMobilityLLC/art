#
# Copyright (C) 2015 The Android Open Source Project
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

LOCAL_PATH := $(call my-dir)

include art/build/Android.common_path.mk

# Determine the location of the ri-test-dump.jar and ri-test-dump.hprof.
AHAT_RI_TEST_DUMP_JAR := $(call intermediates-dir-for,JAVA_LIBRARIES,ahat-ri-test-dump,HOST)/javalib.jar
AHAT_RI_TEST_DUMP_COMMON := $(call intermediates-dir-for,JAVA_LIBRARIES,ahat-ri-test-dump,HOST,COMMON)
AHAT_RI_TEST_DUMP_HPROF := $(AHAT_RI_TEST_DUMP_COMMON)/ri-test-dump.hprof

# Run ahat-ri-test-dump.jar to generate ri-test-dump.hprof
$(AHAT_RI_TEST_DUMP_HPROF): PRIVATE_AHAT_RI_TEST_DUMP_JAR := $(AHAT_RI_TEST_DUMP_JAR)
$(AHAT_RI_TEST_DUMP_HPROF): $(AHAT_RI_TEST_DUMP_JAR)
	rm -rf $@
	java -cp $(PRIVATE_AHAT_RI_TEST_DUMP_JAR) Main $@

# --- ahat-tests.jar --------------
# To run these tests, use: atest ahat-tests --host
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(call all-java-files-under, src/test)
LOCAL_JAR_MANIFEST := etc/ahat-tests.mf
LOCAL_JAVA_RESOURCE_FILES := \
  $(LOCAL_PATH)/etc/test-dump.hprof \
  $(LOCAL_PATH)/etc/test-dump-base.hprof \
  $(LOCAL_PATH)/etc/test-dump.map \
  $(AHAT_RI_TEST_DUMP_HPROF) \
  $(LOCAL_PATH)/etc/L.hprof \
  $(LOCAL_PATH)/etc/O.hprof \
  $(LOCAL_PATH)/etc/RI.hprof
LOCAL_STATIC_JAVA_LIBRARIES := ahat junit-host
LOCAL_IS_HOST_MODULE := true
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := ahat-tests
LOCAL_LICENSE_KINDS := SPDX-license-identifier-Apache-2.0
LOCAL_LICENSE_CONDITIONS := notice
LOCAL_NOTICE_FILE := $(LOCAL_PATH)/../../NOTICE
LOCAL_TEST_CONFIG := ahat-tests.xml
LOCAL_COMPATIBILITY_SUITE := general-tests
include $(BUILD_HOST_JAVA_LIBRARY)
AHAT_TEST_JAR := $(LOCAL_BUILT_MODULE)

# Clean up local variables.
AHAT_TEST_JAR :=

AHAT_RI_TEST_DUMP_JAR :=
AHAT_RI_TEST_DUMP_COMMON :=
AHAT_RI_TEST_DUMP_HPROF :=
