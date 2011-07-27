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

# $(1): target or host
define build-libarttest
  include $(CLEAR_VARS)
  ifeq ($(1),target)
   include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := libarttest
  LOCAL_MODULE_TAGS := tests
  LOCAL_SRC_FILES := $(LIBARTTEST_COMMON_SRC_FILES)
  LOCAL_CFLAGS := $(ART_CFLAGS) -UNDEBUG
  ifeq ($(1),target)
    LOCAL_SHARED_LIBRARIES := libstlport
  else
    LOCAL_LDLIBS := -lrt
  endif
  ifeq ($(1),target)
    include $(BUILD_SHARED_LIBRARY)
  else
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

$(eval $(call build-libarttest,target))
ifeq ($(WITH_HOST_DALVIK),true)
  $(eval $(call build-libarttest,host))
endif
