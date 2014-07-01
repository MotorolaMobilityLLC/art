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

# Tests that are timing sensitive and flaky on heavily loaded systems.
TEST_ART_TIMING_SENSITIVE_RUN_TESTS := \
  test-art-host-run-test-default-053-wait-some32 \
  test-art-host-run-test-default-053-wait-some64 \
  test-art-host-run-test-interpreter-053-wait-some32 \
  test-art-host-run-test-interpreter-053-wait-some64 \
  test-art-host-run-test-optimizing-053-wait-some32 \
  test-art-host-run-test-optimizing-053-wait-some64 \
  test-art-host-run-test-default-055-enum-performance32 \
  test-art-host-run-test-default-055-enum-performance64 \
  test-art-host-run-test-interpreter-055-enum-performance32 \
  test-art-host-run-test-interpreter-055-enum-performance64 \
  test-art-host-run-test-optimizing-055-enum-performance32 \
  test-art-host-run-test-optimizing-055-enum-performance64

 # disable timing sensitive tests on "dist" builds.
ifdef dist_goal
  ART_TEST_KNOWN_BROKEN += $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS)
endif

# The path where build only targets will be output, e.g.
# out/target/product/generic_x86_64/obj/PACKAGING/art-run-tests_intermediates/DATA
art_run_tests_dir := $(call intermediates-dir-for,PACKAGING,art-run-tests)/DATA

# A generated list of prerequisites that call 'run-test --build-only', the actual prerequisite is
# an empty file touched in the intermediate directory.
TEST_ART_RUN_TEST_BUILD_RULES :=

# Helper to create individual build targets for tests. Must be called with $(eval).
# $(1): the test number
define define-build-art-run-test
  dmart_target := $(art_run_tests_dir)/art-run-tests/$(1)/touch
$$(dmart_target): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	  $(LOCAL_PATH)/run-test --build-only --output-path $$(abspath $$(dir $$@)) $(1)
	$(hide) touch $$@

  TEST_ART_RUN_TEST_BUILD_RULES += $$(dmart_target)
  dmart_target :=
endef
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-build-art-run-test,$(test))))

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := art-run-tests
LOCAL_ADDITIONAL_DEPENDENCIES := $(TEST_ART_RUN_TEST_BUILD_RULES)
# The build system use this flag to pick up files generated by declare-make-art-run-test.
LOCAL_PICKUP_FILES := $(art_run_tests_dir)

include $(BUILD_PHONY_PACKAGE)

# Clear temp vars.
TEST_ART_RUN_TEST_BUILD_RULES :=
art_run_tests_dir :=
define-build-art-run-test :=

########################################################################

ART_TEST_TARGET_RUN_TEST_ALL_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=

# We need dex2oat and dalvikvm on the target as well as the core image.
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_EXECUTABLES) $(TARGET_CORE_IMG_OUT) $(2ND_TARGET_CORE_IMG_OUT)

# All tests require the host executables and the core images.
ART_TEST_HOST_RUN_TEST_DEPENDENCIES := \
  $(ART_HOST_EXECUTABLES) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(HOST_CORE_IMG_OUT)

ifneq ($(HOST_PREFER_32_BIT),true)
ART_TEST_HOST_RUN_TEST_DEPENDENCIES += \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_HOST_CORE_IMG_OUT)
endif

# For a given test create all the combinations of host/target, compiler and suffix such as:
# test-art-host-run-test-optimizing-003-omnibus-opcodes32
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): host or target
# $(3): default, optimizing or interpreter
# $(4): 32 or 64
define define-test-art-run-test
  run_test_options := $(addprefix --runtime-option ,$(DALVIKVM_FLAGS))
  uc_host_or_target :=
  prereq_rule :=
  ifeq ($(2),host)
    uc_host_or_target := HOST
    run_test_options += --host
    prereq_rule := $(ART_TEST_HOST_RUN_TEST_DEPENDENCIES)
  else
    ifeq ($(2),target)
      uc_host_or_target := TARGET
      prereq_rule := test-art-target-sync
    else
      $$(error found $(2) expected host or target)
    endif
  endif
  uc_compiler :=
  ifeq ($(3),optimizing)
    uc_compiler := OPTIMIZING
    run_test_options += -Xcompiler-option --compiler-backend=Optimizing
  else
    ifeq ($(3),interpreter)
      uc_compiler := INTERPRETER
      run_test_options += --interpreter
    else
      ifeq ($(3),default)
        uc_compiler := DEFAULT
      else
        $$(error found $(3) expected optimizing, interpreter or default)
      endif
    endif
  endif
  ifeq ($(4),64)
    run_test_options += --64
  else
    ifneq ($(4),32)
      $$(error found $(4) expected 32 or 64)
    endif
  endif
  run_test_rule_name := test-art-$(2)-run-test-$(3)-$(1)$(4)
  run_test_options := --output-path $(ART_HOST_TEST_DIR)/run-test-output/$$(run_test_rule_name) \
    $$(run_test_options)
$$(run_test_rule_name): PRIVATE_RUN_TEST_OPTIONS := $$(run_test_options)
.PHONY: $$(run_test_rule_name)
$$(run_test_rule_name): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin $$(prereq_rule)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	    art/test/run-test $$(PRIVATE_RUN_TEST_OPTIONS) $(1) \
	      && $$(call ART_TEST_PASSED,$$@) || $$(call ART_TEST_FAILED,$$@)
	$$(hide) (echo $(MAKECMDGOALS) | grep -q $$@ && \
	  echo "run-test run as top-level target, removing test directory $(ART_HOST_TEST_DIR)" && \
	  rm -r $(ART_HOST_TEST_DIR)) || true

  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)$(4)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_$(1)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$(1)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_ALL_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_ALL$(4)_RULES += $$(run_test_rule_name)

  # Clear locally defined variables.
  run_test_options :=
  run_test_rule_name :=
  uc_host_or_target :=
  prereq_rule :=
  uc_compiler :=
endef  # define-test-art-run-test

# Define a phony rule whose purpose is to test its prerequisites.
# $(1): rule name, e.g. test-art-host-run-test32
# $(2): list of prerequisites
define define-test-art-run-test-group-rule
.PHONY: $(1)
$(1): $(2)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

endef  # define-test-art-run-test-group-rule

# Create rules for a group of run tests.
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): host or target
define define-test-art-run-test-group
  group_uc_host_or_target :=
  ifeq ($(2),host)
    group_uc_host_or_target := HOST
  else
    ifeq ($(2),target)
      group_uc_host_or_target := TARGET
    else
      $$(error found $(2) expected host or target)
    endif
  endif

  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES :=
  $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
  do_second := false
  ifeq ($(2),host)
    ifneq ($$(HOST_PREFER_32_BIT),true)
      do_second := true
    endif
  else
    ifdef TARGET_2ND_ARCH
      do_second := true
    endif
  endif
  ifeq (true,$$(do_second))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX)))
  endif

  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-default-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-interpreter-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-optimizing-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES)))

  # Clear locally defined variables.
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES :=
  group_uc_host_or_target :=
  do_second :=
endef  # define-test-art-run-test-group

$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-test-art-run-test-group,$(test),target)))
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-test-art-run-test-group,$(test),host)))

$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test, \
  $(ART_TEST_TARGET_RUN_TEST_ALL_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default, \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter, \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing, \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
ifdef TARGET_2ND_ARCH
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
endif

$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test, \
  $(ART_TEST_HOST_RUN_TEST_ALL_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default, \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter, \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing, \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_ALL$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
ifneq ($(HOST_PREFER_32_BIT),true)
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
endif

define-test-art-run-test :=
define-test-art-run-test-group-rule :=
define-test-art-run-test-group :=
ART_TEST_TARGET_RUN_TEST_ALL_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
