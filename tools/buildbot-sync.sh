#!/bin/bash
#
# Copyright (C) 2018 The Android Open Source Project
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

adb wait-for-device

if [[ -z "${ANDROID_PRODUCT_OUT}" ]]; then
  echo 'ANDROID_PRODUCT_OUT environment variable is empty; did you forget to run `lunch`?'
  exit 1
fi

if [[ -z "${ART_TEST_CHROOT}" ]]; then
  echo 'ART_TEST_CHROOT environment variable is empty'
  exit 1
fi

# Sync the system directory to the chroot.
adb push ${ANDROID_PRODUCT_OUT}/system ${ART_TEST_CHROOT}/
# Overwrite the default public.libraries.txt file with a smaller one that
# contains only the public libraries pushed to the chroot directory.
adb push ${ANDROID_BUILD_TOP}/art/tools/public.libraries.buildbot.txt \
  ${ART_TEST_CHROOT}/system/etc/public.libraries.txt

# Temporarily push a copy of the ICU data file into the Android Runtime Root
# location ("/apex/com.android.runtime"). This step is required in the time
# interval between:
# 1. the moment we stop setting `ART_TEST_ANDROID_RUNTIME_ROOT` to "/system"
#    (meaning Bionic will start looking for it in the default
#    `ANDROID_RUNTIME_ROOT` location, which is "/apex/com.android.runtime"); and
# 2. the moment we start installing and using the Runtime APEX (which includes
#    the ICU data file) within the chroot directory on device for target
#    testing.
adb shell rm -rf ${ART_TEST_CHROOT}/apex/com.android.runtime
adb push ${ANDROID_PRODUCT_OUT}/system/etc/icu ${ART_TEST_CHROOT}/apex/com.android.runtime/etc/icu

# Sync the data directory to the chroot.
adb push ${ANDROID_PRODUCT_OUT}/data ${ART_TEST_CHROOT}/
