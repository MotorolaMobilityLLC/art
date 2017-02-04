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

LOCAL_PATH := $(call my-dir)
include art/build/Android.common_test.mk

# List of all tests of the form 003-omnibus-opcodes.
TEST_ART_RUN_TESTS := $(wildcard $(LOCAL_PATH)/[0-9]*)
TEST_ART_RUN_TESTS := $(subst $(LOCAL_PATH)/,, $(TEST_ART_RUN_TESTS))

########################################################################
# The art-run-tests module, used to build all run-tests into an image.

# The path where build only targets will be output, e.g.
# out/target/product/generic_x86_64/obj/PACKAGING/art-run-tests_intermediates/DATA
art_run_tests_build_dir := $(call intermediates-dir-for,JAVA_LIBRARIES,art-run-tests)/DATA
art_run_tests_install_dir := $(call intermediates-dir-for,PACKAGING,art-run-tests)/DATA

# A generated list of prerequisites that call 'run-test --build-only', the actual prerequisite is
# an empty file touched in the intermediate directory.
TEST_ART_RUN_TEST_BUILD_RULES :=

# Dependencies for actually running a run-test.
TEST_ART_RUN_TEST_DEPENDENCIES := \
  $(DX) \
  $(HOST_OUT_EXECUTABLES)/jasmin \
  $(HOST_OUT_EXECUTABLES)/smali \
  $(HOST_OUT_EXECUTABLES)/dexmerger \
  $(JACK)

TEST_ART_RUN_TEST_ORDERONLY_DEPENDENCIES := setup-jack-server

ifeq ($(ART_TEST_DEBUG_GC),true)
  ART_TEST_WITH_STRACE := true
endif

ifeq ($(ART_TEST_BISECTION),true)
  # Need to keep rebuilding the test to bisection search it.
  ART_TEST_RUN_TEST_NO_PREBUILD := true
  ART_TEST_RUN_TEST_PREBUILD := false
  # Bisection search writes to standard output.
  ART_TEST_QUIET := false
endif

# Helper to create individual build targets for tests. Must be called with $(eval).
# $(1): the test number
define define-build-art-run-test
  dmart_target := $(art_run_tests_build_dir)/art-run-tests/$(1)/touch
  dmart_install_target := $(art_run_tests_install_dir)/art-run-tests/$(1)/touch
  run_test_options = --build-only
  ifeq ($(ART_TEST_QUIET),true)
    run_test_options += --quiet
  endif
$$(dmart_target): PRIVATE_RUN_TEST_OPTIONS := $$(run_test_options)
$$(dmart_target): $(TEST_ART_RUN_TEST_DEPENDENCIES) $(TARGET_JACK_CLASSPATH_DEPENDENCIES) | $(TEST_ART_RUN_TEST_ORDERONLY_DEPENDENCIES)
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	  SMALI=$(abspath $(HOST_OUT_EXECUTABLES)/smali) \
	  DXMERGER=$(abspath $(HOST_OUT_EXECUTABLES)/dexmerger) \
	  JACK_VERSION=$(JACK_DEFAULT_VERSION) \
	  JACK=$(abspath $(JACK)) \
	  JACK_VERSION=$(JACK_DEFAULT_VERSION) \
	  JACK_CLASSPATH=$(TARGET_JACK_CLASSPATH) \
	  $(LOCAL_PATH)/run-test $$(PRIVATE_RUN_TEST_OPTIONS) --output-path $$(abspath $$(dir $$@)) $(1)
	$(hide) touch $$@

$$(dmart_install_target): $$(dmart_target)
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) cp $$(dir $$<)/* $$(dir $$@)/

  TEST_ART_RUN_TEST_BUILD_RULES += $$(dmart_install_target)
  dmart_target :=
  dmart_install_target :=
  run_test_options :=
endef
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-build-art-run-test,$(test))))

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := art-run-tests
LOCAL_ADDITIONAL_DEPENDENCIES := $(TEST_ART_RUN_TEST_BUILD_RULES)
# The build system use this flag to pick up files generated by declare-make-art-run-test.
LOCAL_PICKUP_FILES := $(art_run_tests_install_dir)

include $(BUILD_PHONY_PACKAGE)

# Clear temp vars.
art_run_tests_build_dir :=
art_run_tests_install_dir :=
define-build-art-run-test :=
TEST_ART_RUN_TEST_BUILD_RULES :=

########################################################################
# General rules to build and run a run-test.

TARGET_TYPES := host target

PREBUILD_TYPES := prebuild
PREBUILD_TYPES += no-prebuild
PREBUILD_TYPES += no-dex2oat

COMPILER_TYPES := interp-ac
COMPILER_TYPES += interpreter
COMPILER_TYPES += jit
COMPILER_TYPES += optimizing
COMPILER_TYPES += regalloc_gc

RELOCATE_TYPES := relocate
RELOCATE_TYPES += no-relocate
RELOCATE_TYPES += relocate-npatchoat

TRACE_TYPES := ntrace
TRACE_TYPES += trace
TRACE_TYPES += stream

GC_TYPES := cms
GC_TYPES += gcstress
GC_TYPES += gcverify

JNI_TYPES := checkjni
JNI_TYPES += forcecopy

IMAGE_TYPES := picimage
IMAGE_TYPES += no-image
IMAGE_TYPES += multipicimage
IMAGE_TYPES += npicimage
IMAGE_TYPES += multinpicimage

PICTEST_TYPES := npictest
PICTEST_TYPES += pictest

RUN_TYPES := debug
RUN_TYPES += ndebug

DEBUGGABLE_TYPES := ndebuggable
DEBUGGABLE_TYPES += debuggable

ALL_ADDRESS_SIZES := 64 32

define name-to-var
$(shell echo $(1) | tr '[:lower:]' '[:upper:]' | tr '-' '_')
endef  # name-to-var

# We need dex2oat and dalvikvm on the target as well as the core images (all images as we sync
# only once).
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_EXECUTABLES) $(TARGET_CORE_IMG_OUTS)

# Also need libartagent.
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libartagent)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libartagentd)
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libartagent)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libartagentd)
endif

# Also need libtiagent.
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libtiagent)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libtiagentd)
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libtiagent)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libtiagentd)
endif

# Also need libarttest.
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libarttest)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libarttestd)
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libarttest)
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libarttestd)
endif

# Also need libnativebridgetest.
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_ARCH)_libnativebridgetest)
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(OUT_DIR)/$(ART_TEST_LIST_device_$(TARGET_2ND_ARCH)_libnativebridgetest)
endif

# Also need libopenjdkjvmti.
TEST_ART_TARGET_SYNC_DEPS += libopenjdkjvmti
TEST_ART_TARGET_SYNC_DEPS += libopenjdkjvmtid

# All tests require the host executables. The tests also depend on the core images, but on
# specific version depending on the compiler.
ART_TEST_HOST_RUN_TEST_DEPENDENCIES := \
  $(ART_HOST_EXECUTABLES) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libtiagent) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libtiagentd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libartagent) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libartagentd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libarttest) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libarttestd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(ART_HOST_ARCH)_libnativebridgetest) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdk$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkd$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvmti$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvmtid$(ART_HOST_SHLIB_EXTENSION) \

ifneq ($(HOST_PREFER_32_BIT),true)
ART_TEST_HOST_RUN_TEST_DEPENDENCIES += \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libtiagent) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libtiagentd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libartagent) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libartagentd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libarttest) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libarttestd) \
  $(OUT_DIR)/$(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_libnativebridgetest) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdk$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkd$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvmti$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvmtid$(ART_HOST_SHLIB_EXTENSION) \

endif

host_prereq_rules := $(ART_TEST_HOST_RUN_TEST_DEPENDENCIES)
host_prereq_rules += $(HOST_JACK_CLASSPATH_DEPENDENCIES)
host_prereq_rules += $(HOST_CORE_IMAGE_OUT)
host_prereq_rules += $(TEST_ART_RUN_TEST_DEPENDENCIES)
host_prereq_rules += $(HOST_OUT_EXECUTABLES)/hprof-conv

target_prereq_rules := $(TARGET_JACK_CLASSPATH_DEPENDENCIES)
target_prereq_rules += test-art-target-sync

define core-image-dependencies
  ifeq ($(2),no-image)
    $(1)_prereq_rules += $($(call name-to-var,$(1))_CORE_IMAGE_$(3)_pic_$(4))
  else
    ifeq ($(2),npicimage)
      $(1)_prereq_rules += $($(call name-to-var,$(1))_CORE_IMAGE_$(3)_no-pic_$(4))
    else
      ifeq ($(2),picimage)
        $(1)_prereq_rules += $($(call name-to-var,$(1))_CORE_IMAGE_$(3)_pic_$(4))
      else
        ifeq ($(2),multinpicimage)
          $(1)_prereq_rules += $($(call name-to-var,$(1))_CORE_IMAGE_$(3)_no-pic_multi_$(4))
        else
          ifeq ($(2),multipicimage)
             $(1)_prereq_rules += $($(call name-to-var,$(1))_CORE_IMAGE_$(3)_pic_multi_$(4))
          endif
        endif
      endif
    endif
  endif
endef

# Generate list of dependencies required for given target - HOST or TARGET, IMAGE_TYPE,
# COMPILER_TYPE and ADDRESS_SIZE.
$(foreach target, $(TARGET_TYPES), \
  $(foreach image, $(IMAGE_TYPES), \
    $(foreach compiler, $(COMPILER_TYPES), \
      $(foreach address_size, $(ALL_ADDRESS_SIZES), $(eval \
        $(call core-image-dependencies,$(target),$(image),$(compiler),$(address_size)))))))

test-art-host-run-test-dependencies : $(host_prereq_rules)
test-art-target-run-test-dependencies : $(target_prereq_rules)
test-art-run-test-dependencies : test-art-host-run-test-dependencies test-art-target-run-test-dependencies

# Create a rule to build and run a test group of the following form:
# test-art-{1: host target}-run-test-{2: prebuild no-prebuild no-dex2oat \
# interp-ac interpreter jit optimizing regalloc_gc relocate no-relocate \
# relocate-npatchoat ntrace trace stream cms gcstress gcverify checkjni \
# forcecopy picimage no-image multinpicimage npictest pictest debug ndebug \
# ndebuggable debuggable}
#
# test-art-{1: host target}-run-test{2: 32 64}
define define-test-art-run-test-group
  ifneq (,$(filter $(2),$(ALL_ADDRESS_SIZES)))
    build_target := test-art-$(1)-run-test$(2)
  else
    build_target := test-art-$(1)-run-test-$(2)
  endif
  # Python testrunner support multi-threaded test run, however, to invoke the script, one
  # has to provide the concurrency level for the run. To provide that value, the concurrency
  # level of the make invocation is used. The make invocation information is extracted through
  # `ps` and subsequently the number of threads is determined.
  kati_pid = $(shell echo $$PPID)
  make_pid = $$(shell ps -o ppid= -p $$(kati_pid))
  grep_build_targets_from_ps_keyword := -e
  grep_build_targets_from_ps_keyword += $$(shell echo $(MAKECMDGOALS) | sed -e "s/\s\+/ -e /g")
  ps_data := $$(shell ps aux | grep $$(make_pid) | grep $$(grep_build_targets_from_ps_keyword) )
  concurrency := $$(shell [[ "$$(ps_data)" =~ -[a-zA-Z]*j[[:space:]]*[0-9]+ ]] && [[ $$$$BASH_REMATCH =~ [0-9]+ ]] && echo $$$$BASH_REMATCH || echo 1)

  .PHONY: $$(build_target)
  $$(build_target) : args := --$(1) --$(2) --verbose -j$$(concurrency)
  $$(build_target) : test-art-$(1)-run-test-dependencies
			./art/test/testrunner/testrunner.py  $$(args)
  build_target :=
  concurrency :=
  ps_data :=
  kati_pid :=
  make_pid :=
  grep_build_targets_from_ps_keyword :=
endef  # define-test-art-run-test-group

$(foreach target, $(TARGET_TYPES), \
  $(foreach prebuild, $(PREBUILD_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(prebuild)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach run-type, $(RUN_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(run-type)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach compiler, $(COMPILER_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(compiler)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach relocate, $(RELOCATE_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(relocate)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach trace, $(TRACE_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(trace)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach gc, $(GC_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(gc)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach jni, $(JNI_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(jni)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach debuggable, $(DEBUGGABLE_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(debuggable)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach image, $(IMAGE_TYPES), $(eval \
    $(call define-test-art-run-test-group,$(target),$(image)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach address_size, $(ADDRESS_SIZES_$(call name-to-var,$(target))), $(eval \
    $(call define-test-art-run-test-group,$(target),$(address_size)))))

# Create a rule to build and run a test group of the following form:
# test-art-{1: host target}-run-test
define define-test-art-host-or-target-run-test-group
  build_target := test-art-$(1)-run-test
  .PHONY: $$(build_target)
  # Python testrunner support multi-threaded test run, however, to invoke the script, one
  # has to provide the concurrency level for the run. To provide that value, the concurrency
  # level of the make invocation is used. The make invocation information is extracted through
  # `ps` and subsequently the number of threads is determined.
  kati_pid = $(shell echo $$PPID)
  make_pid = $$(shell ps -o ppid= -p $$(kati_pid))
  grep_build_targets_from_ps_keyword := -e
  grep_build_targets_from_ps_keyword += $$(shell echo $(MAKECMDGOALS) | sed -e "s/\s\+/ -e /g")
  ps_data := $$(shell ps aux | grep $$(make_pid) | grep $$(grep_build_targets_from_ps_keyword))
  concurrency := $$(shell [[ "$$(ps_data)" =~ -[a-zA-Z]*j[[:space:]]*[0-9]+ ]] && [[ $$$$BASH_REMATCH =~ [0-9]+ ]] && echo $$$$BASH_REMATCH || echo 1)

  $$(build_target) : args := --$(1) --verbose -j$$(concurrency)
  $$(build_target) : test-art-$(1)-run-test-dependencies
			./art/test/testrunner/testrunner.py $$(args)
  build_target :=
  args :=
  grep_build_targets_from_ps_keyword :=
  ps_data :=
  concurrency :=
  kati_pid :=
  make_pid :=
endef  # define-test-art-host-or-target-run-test-group

$(foreach target, $(TARGET_TYPES), $(eval \
  $(call define-test-art-host-or-target-run-test-group,$(target))))

TARGET_TYPES :=
PREBUILD_TYPES :=
COMPILER_TYPES :=
RELOCATE_TYPES :=
TRACE_TYPES :=
GC_TYPES :=
JNI_TYPES :=
IMAGE_TYPES :=
PICTEST_TYPES :=
RUN_TYPES :=
DEBUGGABLE_TYPES :=
ALL_ADDRESS_SIZES :=
LOCAL_PATH :=
