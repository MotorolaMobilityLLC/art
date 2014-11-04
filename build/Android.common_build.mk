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

ifndef ANDROID_COMMON_BUILD_MK
ANDROID_COMMON_BUILD_MK = true

include art/build/Android.common.mk

# These can be overridden via the environment or by editing to
# enable/disable certain build configuration.
#
# For example, to disable everything but the host debug build you use:
#
# (export ART_BUILD_TARGET_NDEBUG=false && export ART_BUILD_TARGET_DEBUG=false && export ART_BUILD_HOST_NDEBUG=false && ...)
#
# Beware that tests may use the non-debug build for performance, notable 055-enum-performance
#
ART_BUILD_TARGET_NDEBUG ?= true
ART_BUILD_TARGET_DEBUG ?= true
ART_BUILD_HOST_NDEBUG ?= true
ART_BUILD_HOST_DEBUG ?= true

ifeq ($(ART_BUILD_TARGET_NDEBUG),false)
$(info Disabling ART_BUILD_TARGET_NDEBUG)
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),false)
$(info Disabling ART_BUILD_TARGET_DEBUG)
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),false)
$(info Disabling ART_BUILD_HOST_NDEBUG)
endif
ifeq ($(ART_BUILD_HOST_DEBUG),false)
$(info Disabling ART_BUILD_HOST_DEBUG)
endif

#
# Used to enable smart mode
#
ART_SMALL_MODE := false
ifneq ($(wildcard art/SMALL_ART),)
$(info Enabling ART_SMALL_MODE because of existence of art/SMALL_ART)
ART_SMALL_MODE := true
endif
ifeq ($(WITH_ART_SMALL_MODE), true)
ART_SMALL_MODE := true
endif

#
# Used to enable SEA mode
#
ART_SEA_IR_MODE := false
ifneq ($(wildcard art/SEA_IR_ART),)
$(info Enabling ART_SEA_IR_MODE because of existence of art/SEA_IR_ART)
ART_SEA_IR_MODE := true
endif
ifeq ($(WITH_ART_SEA_IR_MODE), true)
ART_SEA_IR_MODE := true
endif

#
# Used to enable portable mode
#
ART_USE_PORTABLE_COMPILER := false
ifneq ($(wildcard art/USE_PORTABLE_COMPILER),)
$(info Enabling ART_USE_PORTABLE_COMPILER because of existence of art/USE_PORTABLE_COMPILER)
ART_USE_PORTABLE_COMPILER := true
endif
ifeq ($(WITH_ART_USE_PORTABLE_COMPILER),true)
$(info Enabling ART_USE_PORTABLE_COMPILER because WITH_ART_USE_PORTABLE_COMPILER=true)
ART_USE_PORTABLE_COMPILER := true
endif

#
# Used to change the default GC. Valid values are CMS, SS, GSS. The default is CMS.
#
art_default_gc_type ?= CMS
art_default_gc_type_cflags := -DART_DEFAULT_GC_TYPE_IS_$(art_default_gc_type)

ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LLVM_ROOT_PATH := external/llvm
  # Don't fail a dalvik minimal host build.
  -include $(LLVM_ROOT_PATH)/llvm.mk
endif

ART_HOST_CFLAGS :=
ART_TARGET_CFLAGS :=

# Clang build support.

# Host.
ART_HOST_CLANG := false
ifneq ($(WITHOUT_HOST_CLANG),true)
  # By default, host builds use clang for better warnings.
  ART_HOST_CLANG := true
endif

# Clang on the target. Target builds use GCC by default.
ifneq ($(USE_CLANG_PLATFORM_BUILD),)
ART_TARGET_CLANG := $(USE_CLANG_PLATFORM_BUILD)
else
ART_TARGET_CLANG := false
endif
ART_TARGET_CLANG_arm :=
ART_TARGET_CLANG_arm64 :=
ART_TARGET_CLANG_mips :=
ART_TARGET_CLANG_x86 :=
ART_TARGET_CLANG_x86_64 :=

define set-target-local-clang-vars
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    $(foreach arch,$(ART_TARGET_SUPPORTED_ARCH),
      ifneq ($$(ART_TARGET_CLANG_$(arch)),)
        LOCAL_CLANG_$(arch) := $$(ART_TARGET_CLANG_$(arch))
      endif)
endef

ART_TARGET_CLANG_CFLAGS :=
ART_TARGET_CLANG_CFLAGS_arm :=
ART_TARGET_CLANG_CFLAGS_arm64 :=
ART_TARGET_CLANG_CFLAGS_mips :=
ART_TARGET_CLANG_CFLAGS_x86 :=
ART_TARGET_CLANG_CFLAGS_x86_64 :=

# These are necessary for Clang ARM64 ART builds. TODO: remove.
ART_TARGET_CLANG_CFLAGS_arm64  += \
  -Wno-implicit-exception-spec-mismatch \
  -DNVALGRIND \
  -Wno-unused-value

# FIXME: upstream LLVM has a vectorizer bug that needs to be fixed
ART_TARGET_CLANG_CFLAGS_arm64 += \
  -fno-vectorize

# Colorize clang compiler warnings.
art_clang_cflags := -fcolor-diagnostics

# Warn about thread safety violations with clang.
art_clang_cflags += -Wthread-safety

# Warn if switch fallthroughs aren't annotated.
art_clang_cflags += -Wimplicit-fallthrough

# Enable float equality warnings.
art_clang_cflags += -Wfloat-equal

ifeq ($(ART_HOST_CLANG),true)
  ART_HOST_CFLAGS += $(art_clang_cflags)
endif
ifeq ($(ART_TARGET_CLANG),true)
  ART_TARGET_CFLAGS += $(art_clang_cflags)
endif

# Clear local variable now its use has ended.
art_clang_cflags :=

ART_CPP_EXTENSION := .cc

ART_C_INCLUDES := \
  external/gtest/include \
  external/valgrind/main/include \
  external/valgrind/main \
  external/vixl/src \
  external/zlib \
  frameworks/compile/mclinker/include

# Base set of cflags used by all things ART.
art_cflags := \
  -fno-rtti \
  -std=gnu++11 \
  -ggdb3 \
  -Wall \
  -Werror \
  -Wextra \
  -Wstrict-aliasing \
  -fstrict-aliasing \
  -Wunreachable-code \
  -fvisibility=protected \
  $(art_default_gc_type_cflags)

ifeq ($(ART_SMALL_MODE),true)
  art_cflags += -DART_SMALL_MODE=1
endif

ifeq ($(ART_SEA_IR_MODE),true)
  art_cflags += -DART_SEA_IR_MODE=1
endif

# Cflags for non-debug ART and ART tools.
art_non_debug_cflags := \
  -O3

# Cflags for debug ART and ART tools.
art_debug_cflags := \
  -O2 \
  -DDYNAMIC_ANNOTATIONS_ENABLED=1 \
  -UNDEBUG

art_host_non_debug_cflags := $(art_non_debug_cflags)
art_target_non_debug_cflags := $(art_non_debug_cflags)

ifeq ($(HOST_OS),linux)
  # Larger frame-size for host clang builds today
  art_host_non_debug_cflags += -Wframe-larger-than=2600
  art_target_non_debug_cflags += -Wframe-larger-than=1728
endif

ifndef LIBART_IMG_HOST_BASE_ADDRESS
  $(error LIBART_IMG_HOST_BASE_ADDRESS unset)
endif
ART_HOST_CFLAGS += $(art_cflags) -DANDROID_SMP=1 -DART_BASE_ADDRESS=$(LIBART_IMG_HOST_BASE_ADDRESS)
ART_HOST_CFLAGS += -DART_DEFAULT_INSTRUCTION_SET_FEATURES=default

ifndef LIBART_IMG_TARGET_BASE_ADDRESS
  $(error LIBART_IMG_TARGET_BASE_ADDRESS unset)
endif
ART_TARGET_CFLAGS += $(art_cflags) -DART_TARGET -DART_BASE_ADDRESS=$(LIBART_IMG_TARGET_BASE_ADDRESS)

ART_HOST_NON_DEBUG_CFLAGS := $(art_host_non_debug_cflags)
ART_TARGET_NON_DEBUG_CFLAGS := $(art_target_non_debug_cflags)
ART_HOST_DEBUG_CFLAGS := $(art_debug_cflags)
ART_TARGET_DEBUG_CFLAGS := $(art_debug_cflags)

ifndef LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA
  LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA=-0x1000000
endif
ifndef LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA
  LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA=0x1000000
endif
ART_HOST_CFLAGS += -DART_BASE_ADDRESS_MIN_DELTA=$(LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA)
ART_HOST_CFLAGS += -DART_BASE_ADDRESS_MAX_DELTA=$(LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA)

ifndef LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA
  LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA=-0x1000000
endif
ifndef LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA
  LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA=0x1000000
endif
ART_TARGET_CFLAGS += -DART_BASE_ADDRESS_MIN_DELTA=$(LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA)
ART_TARGET_CFLAGS += -DART_BASE_ADDRESS_MAX_DELTA=$(LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA)

ifeq ($(TARGET_CPU_SMP),true)
  ART_TARGET_CFLAGS += -DANDROID_SMP=1
else
  ifeq ($(TARGET_CPU_SMP),false)
    ART_TARGET_CFLAGS += -DANDROID_SMP=0
  else
    $(warning TARGET_CPU_SMP should be (true|false), found $(TARGET_CPU_SMP))
    # Make sure we emit barriers for the worst case.
    ART_TARGET_CFLAGS += -DANDROID_SMP=1
  endif
endif

# To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
# ART_TARGET_CFLAGS += -fno-omit-frame-pointer -marm -mapcs

# Clear locals now they've served their purpose.
art_cflags :=
art_debug_cflags :=
art_non_debug_cflags :=
art_host_non_debug_cflags :=
art_target_non_debug_cflags :=
art_default_gc_type :=
art_default_gc_type_cflags :=

ART_HOST_LDLIBS :=
ifneq ($(ART_HOST_CLANG),true)
  # GCC lacks libc++ assumed atomic operations, grab via libatomic.
  ART_HOST_LDLIBS += -latomic
endif

ART_TARGET_LDFLAGS :=

# $(1): ndebug_or_debug
define set-target-local-cflags-vars
  LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  LOCAL_CFLAGS_x86 += $(ART_TARGET_CFLAGS_x86)
  LOCAL_LDFLAGS += $(ART_TARGET_LDFLAGS)
  art_target_cflags_ndebug_or_debug := $(1)
  ifeq ($$(art_target_cflags_ndebug_or_debug),debug)
    LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
  else
    LOCAL_CFLAGS += $(ART_TARGET_NON_DEBUG_CFLAGS)
  endif

  # TODO: Also set when ART_TARGET_CLANG_$(arch)!=false and ART_TARGET_CLANG==true
  $(foreach arch,$(ART_SUPPORTED_ARCH),
    ifeq ($$(ART_TARGET_CLANG_$(arch)),true)
      LOCAL_CFLAGS_$(arch) += $$(ART_TARGET_CLANG_CFLAGS_$(arch))
  endif)

  # Clear locally used variables.
  art_target_cflags_ndebug_or_debug :=
endef

# Support for disabling certain builds.
ART_BUILD_TARGET := false
ART_BUILD_HOST := false
ART_BUILD_NDEBUG := false
ART_BUILD_DEBUG := false
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  ART_BUILD_TARGET := true
  ART_BUILD_NDEBUG := true
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  ART_BUILD_TARGET := true
  ART_BUILD_DEBUG := true
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  ART_BUILD_HOST := true
  ART_BUILD_NDEBUG := true
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  ART_BUILD_HOST := true
  ART_BUILD_DEBUG := true
endif

endif # ANDROID_COMMON_BUILD_MK
