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
art_run_tests_dir := $(call intermediates-dir-for,PACKAGING,art-run-tests)/DATA

# A generated list of prerequisites that call 'run-test --build-only', the actual prerequisite is
# an empty file touched in the intermediate directory.
TEST_ART_RUN_TEST_BUILD_RULES :=

# Helper to create individual build targets for tests. Must be called with $(eval).
# $(1): the test number
define define-build-art-run-test
  dmart_target := $(art_run_tests_dir)/art-run-tests/$(1)/touch
$$(dmart_target): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin $(HOST_OUT_EXECUTABLES)/smali $(HOST_OUT_EXECUTABLES)/dexmerger
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	  SMALI=$(abspath $(HOST_OUT_EXECUTABLES)/smali) \
	  DXMERGER=$(abspath $(HOST_OUT_EXECUTABLES)/dexmerger) \
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
art_run_tests_dir :=
define-build-art-run-test :=
TEST_ART_RUN_TEST_BUILD_RULES :=

########################################################################
# General rules to build and run a run-test.

TARGET_TYPES := host target
PREBUILD_TYPES :=
ifeq ($(ART_TEST_RUN_TEST_PREBUILD),true)
  PREBUILD_TYPES += prebuild
endif
ifeq ($(ART_TEST_RUN_TEST_NO_PREBUILD),true)
  PREBUILD_TYPES += no-prebuild
endif
ifeq ($(ART_TEST_RUN_TEST_NO_DEX2OAT),true)
  PREBUILD_TYPES += no-dex2oat
endif
COMPILER_TYPES :=
ifeq ($(ART_TEST_DEFAULT_COMPILER),true)
  COMPILER_TYPES += default
endif
ifeq ($(ART_TEST_INTERPRETER),true)
  COMPILER_TYPES += interpreter
endif
ifeq ($(ART_TEST_OPTIMIZING),true)
  COMPILER_TYPES += optimizing
endif
RELOCATE_TYPES := relocate
ifeq ($(ART_TEST_RUN_TEST_NO_RELOCATE),true)
  RELOCATE_TYPES += no-relocate
endif
ifeq ($(ART_TEST_RUN_TEST_RELOCATE_NO_PATCHOAT),true)
  RELOCATE_TYPES := relocate-no-patchoat
endif
TRACE_TYPES := no-trace
ifeq ($(ART_TEST_TRACE),true)
  TRACE_TYPES += trace
endif
GC_TYPES := cms
ifeq ($(ART_TEST_GC_STRESS),true)
  GC_TYPES += gcstress
endif
ifeq ($(ART_TEST_GC_VERIFY),true)
  GC_TYPES += gcverify
endif
JNI_TYPES := checkjni
ifeq ($(ART_TEST_JNI_FORCECOPY),true)
  JNI_TYPES += forcecopy
endif
IMAGE_TYPES := image
ifeq ($(ART_TEST_RUN_TEST_NO_IMAGE),true)
  IMAGE_TYPES += no-image
endif
ifeq ($(ART_TEST_PIC_IMAGE),true)
  IMAGE_TYPES += picimage
endif
PICTEST_TYPES := nopictest
ifeq ($(ART_TEST_PIC_TEST),true)
  PICTEST_TYPES += pictest
endif
RUN_TYPES :=
ifeq ($(ART_TEST_RUN_TEST_DEBUG),true)
  RUN_TYPES += debug
endif
ifeq ($(ART_TEST_RUN_TEST_NDEBUG),true)
  RUN_TYPES += ndebug
endif
ADDRESS_SIZES_TARGET := $(ART_PHONY_TEST_TARGET_SUFFIX)
ADDRESS_SIZES_HOST := $(ART_PHONY_TEST_HOST_SUFFIX)
ifeq ($(ART_TEST_RUN_TEST_2ND_ARCH),true)
  ADDRESS_SIZES_TARGET += $(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
  ADDRESS_SIZES_HOST += $(2ND_ART_PHONY_TEST_HOST_SUFFIX)
endif
ALL_ADDRESS_SIZES := 64 32

# List all run test names with number arguments agreeing with the comment above.
define all-run-test-names
  $(foreach target, $(1), \
    $(foreach run-type, $(2), \
      $(foreach prebuild, $(3), \
        $(foreach compiler, $(4), \
          $(foreach relocate, $(5), \
            $(foreach trace, $(6), \
              $(foreach gc, $(7), \
                $(foreach jni, $(8), \
                  $(foreach image, $(9), \
                    $(foreach pictest, $(10), \
                      $(foreach test, $(11), \
                        $(foreach address_size, $(12), \
                          test-art-$(target)-run-test-$(run-type)-$(prebuild)-$(compiler)-$(relocate)-$(trace)-$(gc)-$(jni)-$(image)-$(pictest)-$(test)$(address_size) \
                    ))))))))))))
endef  # all-run-test-names

# To generate a full list or tests:
# $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES),$(COMPILER_TYPES), \
#        $(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES),$(IMAGE_TYPES), \
#        $(TEST_ART_RUN_TESTS), $(ALL_ADDRESS_SIZES))

# Convert's a rule name to the form used in variables, e.g. no-relocate to NO_RELOCATE
define name-to-var
$(shell echo $(1) | tr '[:lower:]' '[:upper:]' | tr '-' '_')
endef  # name-to-var

# Tests that are timing sensitive and flaky on heavily loaded systems.
TEST_ART_TIMING_SENSITIVE_RUN_TESTS := \
  053-wait-some \
  055-enum-performance

 # disable timing sensitive tests on "dist" builds.
ifdef dist_goal
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
        $(COMPILER_TYPES),$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
        $(IMAGE_TYPES), $(PICTEST_TYPES), $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(ALL_ADDRESS_SIZES))
endif

TEST_ART_TIMING_SENSITIVE_RUN_TESTS :=

# Note 116-nodex2oat is not broken per-se it just doesn't (and isn't meant to) work with --prebuild.
TEST_ART_BROKEN_PREBUILD_RUN_TESTS := \
  116-nodex2oat

ifneq (,$(filter prebuild,$(PREBUILD_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),prebuild, \
      $(COMPILER_TYPES),$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES), $(PICTEST_TYPES), $(TEST_ART_BROKEN_PREBUILD_RUN_TESTS), $(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_PREBUILD_RUN_TESTS :=

TEST_ART_BROKEN_NO_PREBUILD_TESTS := \
  117-nopatchoat

ifneq (,$(filter no-prebuild,$(PREBUILD_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),no-prebuild, \
      $(COMPILER_TYPES),$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES), $(PICTEST_TYPES), $(TEST_ART_BROKEN_NO_PREBUILD_TESTS), $(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_NO_PREBUILD_TESTS :=

# Note 117-nopatchoat is not broken per-se it just doesn't work (and isn't meant to) without
# --prebuild --relocate
TEST_ART_BROKEN_NO_RELOCATE_TESTS := \
  117-nopatchoat

ifneq (,$(filter no-relocate,$(RELOCATE_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      $(COMPILER_TYPES), no-relocate,$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES), $(PICTEST_TYPES), $(TEST_ART_BROKEN_NO_RELOCATE_TESTS), $(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_NO_RELOCATE_TESTS :=

# Tests that are broken with GC stress.
TEST_ART_BROKEN_GCSTRESS_RUN_TESTS := \
  114-ParallelGC

ifneq (,$(filter gcstress,$(GC_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      $(COMPILER_TYPES),$(RELOCATE_TYPES),$(TRACE_TYPES),gcstress,$(JNI_TYPES), \
      $(IMAGE_TYPES), $(PICTEST_TYPES), $(TEST_ART_BROKEN_GCSTRESS_RUN_TESTS), $(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_GCSTRESS_RUN_TESTS :=

# 115-native-bridge setup is complicated. Need to implement it correctly for the target.
ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,target,$(RUN_TYPES),$(PREBUILD_TYPES),$(COMPILER_TYPES), \
    $(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES),$(IMAGE_TYPES),$(PICTEST_TYPES),115-native-bridge, \
    $(ALL_ADDRESS_SIZES))

# All these tests check that we have sane behavior if we don't have a patchoat or dex2oat.
# Therefore we shouldn't run them in situations where we actually don't have these since they
# explicitly test for them. These all also assume we have an image.
TEST_ART_BROKEN_FALLBACK_RUN_TESTS := \
  116-nodex2oat \
  117-nopatchoat \
  118-noimage-dex2oat \
  119-noimage-patchoat

ifneq (,$(filter no-dex2oat,$(PREBUILD_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),no-dex2oat, \
      $(COMPILER_TYPES),$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES),$(IMAGE_TYPES), \
      $(PICTEST_TYPES),$(TEST_ART_BROKEN_FALLBACK_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif


ifneq (,$(filter no-image,$(IMAGE_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      $(COMPILER_TYPES), $(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES),no-image, \
      $(PICTEST_TYPES), $(TEST_ART_BROKEN_FALLBACK_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif

ifneq (,$(filter relocate-no-patchoat,$(RELOCATE_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      $(COMPILER_TYPES), relocate-no-patchoat,$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES),$(PICTEST_TYPES),$(TEST_ART_BROKEN_FALLBACK_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_FALLBACK_RUN_TESTS :=

# The following tests use libarttest.so, which is linked against libartd.so, so will
# not work when libart.so is the one loaded.
# TODO: Find a way to run these tests in ndebug mode.
TEST_ART_BROKEN_NDEBUG_TESTS := \
  004-JniTest \
  004-ReferenceMap \
  004-SignalTest \
  004-StackWalk \
  004-UnsafeTest \
  051-thread \
  115-native-bridge \
  116-nodex2oat \
  117-nopatchoat \
  118-noimage-dex2oat \
  119-noimage-patchoat \

ifneq (,$(filter ndebug,$(RUN_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),ndebug,$(PREBUILD_TYPES), \
      $(COMPILER_TYPES), $(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES),$(IMAGE_TYPES), \
      $(PICTEST_TYPES),$(TEST_ART_BROKEN_NDEBUG_TESTS),$(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_NDEBUG_TESTS :=

# Known broken tests for the default compiler (Quick).
TEST_ART_BROKEN_DEFAULT_RUN_TESTS :=

ifneq (,$(filter default,$(COMPILER_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      default,$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES),$(PICTEST_TYPES),$(TEST_ART_BROKEN_DEFAULT_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_DEFAULT_RUN_TESTS :=

# Known broken tests for the arm64 optimizing compiler backend.
TEST_ART_BROKEN_OPTIMIZING_ARM64_RUN_TESTS :=

ifneq (,$(filter optimizing,$(COMPILER_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,target,$(RUN_TYPES),$(PREBUILD_TYPES), \
      optimizing,$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES),$(PICTEST_TYPES),$(TEST_ART_BROKEN_OPTIMIZING_ARM64_RUN_TESTS),64)
endif

TEST_ART_BROKEN_OPTIMIZING_ARM64_RUN_TESTS :=

# Known broken tests for the optimizing compiler.
TEST_ART_BROKEN_OPTIMIZING_RUN_TESTS := \
  099-vmdebug \ # b/18098594

ifneq (,$(filter optimizing,$(COMPILER_TYPES)))
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      optimizing,$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES),$(PICTEST_TYPES),$(TEST_ART_BROKEN_OPTIMIZING_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif

# If ART_USE_OPTIMIZING_COMPILER is set to true, then the default core.art has been
# compiled with the optimizing compiler.
ifeq ($(ART_USE_OPTIMIZING_COMPILER),true)
  ART_TEST_KNOWN_BROKEN += $(call all-run-test-names,$(TARGET_TYPES),$(RUN_TYPES),$(PREBUILD_TYPES), \
      default,$(RELOCATE_TYPES),$(TRACE_TYPES),$(GC_TYPES),$(JNI_TYPES), \
      $(IMAGE_TYPES),$(PICTEST_TYPES),$(TEST_ART_BROKEN_OPTIMIZING_RUN_TESTS),$(ALL_ADDRESS_SIZES))
endif

TEST_ART_BROKEN_OPTIMIZING_RUN_TESTS :=


# Clear variables ahead of appending to them when defining tests.
$(foreach target, $(TARGET_TYPES), $(eval ART_RUN_TEST_$(call name-to-var,$(target))_RULES :=))
$(foreach target, $(TARGET_TYPES), \
  $(foreach prebuild, $(PREBUILD_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(prebuild))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach compiler, $(COMPILER_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(compiler))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach relocate, $(RELOCATE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(relocate))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach trace, $(TRACE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(trace))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach gc, $(GC_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(gc))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach jni, $(JNI_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(jni))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach image, $(IMAGE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(image))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach test, $(TEST_ART_RUN_TESTS), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(test))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach address_size, $(ALL_ADDRESS_SIZES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(address_size))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach run_type, $(RUN_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(run_type))_RULES :=)))

# We need dex2oat and dalvikvm on the target as well as the core images (all images as we sync
# only once).
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_EXECUTABLES) $(TARGET_CORE_IMG_OUTS)

# Also need libarttest.
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_TEST_OUT)/$(TARGET_ARCH)/libarttest.so
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_TEST_OUT)/$(TARGET_2ND_ARCH)/libarttest.so
endif

# Also need libnativebridgetest.
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_TEST_OUT)/$(TARGET_ARCH)/libnativebridgetest.so
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_TEST_OUT)/$(TARGET_2ND_ARCH)/libnativebridgetest.so
endif

# All tests require the host executables. The tests also depend on the core images, but on
# specific version depending on the compiler.
ART_TEST_HOST_RUN_TEST_DEPENDENCIES := \
  $(ART_HOST_EXECUTABLES) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libarttest$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libnativebridgetest$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION)

ifneq ($(HOST_PREFER_32_BIT),true)
ART_TEST_HOST_RUN_TEST_DEPENDENCIES += \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libarttest$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libnativebridgetest$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION)
endif

# Create a rule to build and run a tests following the form:
# test-art-{1: host or target}-run-test-{2: debug ndebug}-{3: prebuild no-prebuild no-dex2oat}-
#    {4: interpreter default optimizing}-{5: relocate no-relocate relocate-no-patchoat}-
#    {6: trace or no-trace}-{7: gcstress gcverify cms}-{8: forcecopy checkjni jni}-
#    {9: no-image image picimage}-{10: pictest nopictest}-{11: test name}{12: 32 or 64}
define define-test-art-run-test
  run_test_options :=
  prereq_rule :=
  test_groups :=
  uc_host_or_target :=
  ifeq ($(ART_TEST_RUN_TEST_ALWAYS_CLEAN),true)
    run_test_options += --always-clean
  endif
  ifeq ($(1),host)
    uc_host_or_target := HOST
    test_groups := ART_RUN_TEST_HOST_RULES
    run_test_options += --host
    prereq_rule := $(ART_TEST_HOST_RUN_TEST_DEPENDENCIES)
  else
    ifeq ($(1),target)
      uc_host_or_target := TARGET
      test_groups := ART_RUN_TEST_TARGET_RULES
      prereq_rule := test-art-target-sync
    else
      $$(error found $(1) expected $(TARGET_TYPES))
    endif
  endif
  ifeq ($(2),debug)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_DEBUG_RULES
  else
    ifeq ($(2),ndebug)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_RELEASE_RULES
      run_test_options += -O
    else
      $$(error found $(2) expected $(RUN_TYPES))
    endif
  endif
  ifeq ($(3),prebuild)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_PREBUILD_RULES
    run_test_options += --prebuild
  else
    ifeq ($(3),no-prebuild)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_NO_PREBUILD_RULES
      run_test_options += --no-prebuild
    else
      ifeq ($(3),no-dex2oat)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_NO_DEX2OAT_RULES
        run_test_options += --no-prebuild --no-dex2oat
      else
        $$(error found $(3) expected $(PREBUILD_TYPES))
      endif
    endif
  endif
  ifeq ($(4),optimizing)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_OPTIMIZING_RULES
    run_test_options += --optimizing
  else
    ifeq ($(4),interpreter)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_INTERPRETER_RULES
      run_test_options += --interpreter
    else
      ifeq ($(4),default)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_DEFAULT_RULES
        run_test_options += --quick
      else
        $$(error found $(4) expected $(COMPILER_TYPES))
      endif
    endif
  endif

  ifeq ($(5),relocate)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_RELOCATE_RULES
    run_test_options += --relocate
  else
    ifeq ($(5),no-relocate)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_NO_RELOCATE_RULES
      run_test_options += --no-relocate
    else
      ifeq ($(5),relocate-no-patchoat)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_RELOCATE_NO_PATCHOAT_RULES
        run_test_options += --relocate --no-patchoat
      else
        $$(error found $(5) expected $(RELOCATE_TYPES))
      endif
    endif
  endif
  ifeq ($(6),trace)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_TRACE_RULES
    run_test_options += --trace
  else
    ifeq ($(6),no-trace)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_NO_TRACE_RULES
    else
      $$(error found $(6) expected $(TRACE_TYPES))
    endif
  endif
  ifeq ($(7),gcverify)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_GCVERIFY_RULES
    run_test_options += --gcverify
  else
    ifeq ($(7),gcstress)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_GCSTRESS_RULES
      run_test_options += --gcstress
    else
      ifeq ($(7),cms)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_CMS_RULES
      else
        $$(error found $(7) expected $(GC_TYPES))
      endif
    endif
  endif
  ifeq ($(8),forcecopy)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_FORCECOPY_RULES
    run_test_options += --runtime-option -Xjniopts:forcecopy
    ifneq ($$(ART_TEST_JNI_FORCECOPY),true)
      skip_test := true
    endif
  else
    ifeq ($(8),checkjni)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_CHECKJNI_RULES
      run_test_options += --runtime-option -Xcheck:jni
    else
      ifeq ($(8),jni)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_JNI_RULES
      else
        $$(error found $(8) expected $(JNI_TYPES))
      endif
    endif
  endif
  ifeq ($(9),no-image)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_NO_IMAGE_RULES
    run_test_options += --no-image
    # Add the core dependency. This is required for pre-building.
    ifeq ($(1),host)
      prereq_rule += $(HOST_CORE_IMAGE_$(4)_no-pic_$(12))
    else
      prereq_rule += $(TARGET_CORE_IMAGE_$(4)_no-pic_$(12))
    endif
  else
    ifeq ($(9),image)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_IMAGE_RULES
      # Add the core dependency.
      ifeq ($(1),host)
        prereq_rule += $(HOST_CORE_IMAGE_$(4)_no-pic_$(12))
      else
        prereq_rule += $(TARGET_CORE_IMAGE_$(4)_no-pic_$(12))
      endif
    else
      ifeq ($(9),picimage)
        test_groups += ART_RUN_TEST_$$(uc_host_or_target)_PICIMAGE_RULES
        run_test_options += --pic-image
        ifeq ($(1),host)
          prereq_rule += $(HOST_CORE_IMAGE_$(4)_pic_$(12))
        else
          prereq_rule += $(TARGET_CORE_IMAGE_$(4)_pic_$(12))
        endif
      else
        $$(error found $(9) expected $(IMAGE_TYPES))
      endif
    endif
  endif
  ifeq ($(10),pictest)
    run_test_options += --pic-test
  else
    ifeq ($(10),nopictest)
      # Nothing to be done.
    else
      $$(error found $(10) expected $(PICTEST_TYPES))
    endif
  endif
  # $(11) is the test name
  test_groups += ART_RUN_TEST_$$(uc_host_or_target)_$(call name-to-var,$(11))_RULES
  ifeq ($(12),64)
    test_groups += ART_RUN_TEST_$$(uc_host_or_target)_64_RULES
    run_test_options += --64
  else
    ifeq ($(12),32)
      test_groups += ART_RUN_TEST_$$(uc_host_or_target)_32_RULES
    else
      $$(error found $(12) expected $(ALL_ADDRESS_SIZES))
    endif
  endif
  run_test_rule_name := test-art-$(1)-run-test-$(2)-$(3)-$(4)-$(5)-$(6)-$(7)-$(8)-$(9)-$(10)-$(11)$(12)
  run_test_options := --output-path $(ART_HOST_TEST_DIR)/run-test-output/$$(run_test_rule_name) \
      $$(run_test_options)
  ifneq ($(ART_TEST_ANDROID_ROOT),)
    run_test_options := --android-root $(ART_TEST_ANDROID_ROOT) $$(run_test_options)
  endif
$$(run_test_rule_name): PRIVATE_RUN_TEST_OPTIONS := $$(run_test_options)
.PHONY: $$(run_test_rule_name)
$$(run_test_rule_name): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin $(HOST_OUT_EXECUTABLES)/smali $(HOST_OUT_EXECUTABLES)/dexmerger $$(prereq_rule)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	    SMALI=$(abspath $(HOST_OUT_EXECUTABLES)/smali) \
	    DXMERGER=$(abspath $(HOST_OUT_EXECUTABLES)/dexmerger) \
	    art/test/run-test $$(PRIVATE_RUN_TEST_OPTIONS) $(11) \
	      && $$(call ART_TEST_PASSED,$$@) || $$(call ART_TEST_FAILED,$$@)
	$$(hide) (echo $(MAKECMDGOALS) | grep -q $$@ && \
	  echo "run-test run as top-level target, removing test directory $(ART_HOST_TEST_DIR)" && \
	  rm -r $(ART_HOST_TEST_DIR)) || true

  $$(foreach test_group,$$(test_groups), $$(eval $$(value test_group) += $$(run_test_rule_name)))

  # Clear locally defined variables.
  uc_host_or_target :=
  test_groups :=
  run_test_options :=
  run_test_rule_name :=
  prereq_rule :=
endef  # define-test-art-run-test

$(foreach target, $(TARGET_TYPES), \
  $(foreach test, $(TEST_ART_RUN_TESTS), \
    $(foreach run_type, $(RUN_TYPES), \
      $(foreach address_size, $(ADDRESS_SIZES_$(call name-to-var,$(target))), \
        $(foreach prebuild, $(PREBUILD_TYPES), \
          $(foreach compiler, $(COMPILER_TYPES), \
            $(foreach relocate, $(RELOCATE_TYPES), \
              $(foreach trace, $(TRACE_TYPES), \
                $(foreach gc, $(GC_TYPES), \
                  $(foreach jni, $(JNI_TYPES), \
                    $(foreach image, $(IMAGE_TYPES), \
                      $(foreach pictest, $(PICTEST_TYPES), \
                        $(eval $(call define-test-art-run-test,$(target),$(run_type),$(prebuild),$(compiler),$(relocate),$(trace),$(gc),$(jni),$(image),$(pictest),$(test),$(address_size))) \
                  ))))))))))))
define-test-art-run-test :=

# Define a phony rule whose purpose is to test its prerequisites.
# $(1): host or target
# $(2): list of prerequisites
define define-test-art-run-test-group
.PHONY: $(1)
$(1): $(2)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

endef  # define-test-art-run-test-group


$(foreach target, $(TARGET_TYPES), $(eval \
  $(call define-test-art-run-test-group,test-art-$(target)-run-test,$(ART_RUN_TEST_$(call name-to-var,$(target))_RULES))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach prebuild, $(PREBUILD_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(prebuild),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(prebuild))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach run-type, $(RUN_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(run-type),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(run-type))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach compiler, $(COMPILER_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(compiler),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(compiler))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach relocate, $(RELOCATE_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(relocate),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(relocate))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach trace, $(TRACE_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(trace),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(trace))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach gc, $(GC_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(gc),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(gc))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach jni, $(JNI_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(jni),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(jni))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach image, $(IMAGE_TYPES), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(image),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(image))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach test, $(TEST_ART_RUN_TESTS), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test-$(test),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(test))_RULES)))))
$(foreach target, $(TARGET_TYPES), \
  $(foreach address_size, $(ADDRESS_SIZES_$(call name-to-var,$(target))), $(eval \
    $(call define-test-art-run-test-group,test-art-$(target)-run-test$(address_size),$(ART_RUN_TEST_$(call name-to-var,$(target))_$(address_size)_RULES)))))

# Clear variables now we're finished with them.
$(foreach target, $(TARGET_TYPES), $(eval ART_RUN_TEST_$(call name-to-var,$(target))_RULES :=))
$(foreach target, $(TARGET_TYPES), \
  $(foreach prebuild, $(PREBUILD_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(prebuild))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach compiler, $(COMPILER_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(compiler))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach relocate, $(RELOCATE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(relocate))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach trace, $(TRACE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(trace))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach gc, $(GC_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(gc))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach jni, $(JNI_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(jni))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach image, $(IMAGE_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(image))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach test, $(TEST_ART_RUN_TESTS), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(test))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach address_size, $(ALL_ADDRESS_SIZES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(address_size))_RULES :=)))
$(foreach target, $(TARGET_TYPES), \
  $(foreach run_type, $(RUN_TYPES), \
    $(eval ART_RUN_TEST_$(call name-to-var,$(target))_$(call name-to-var,$(run_type))_RULES :=)))
define-test-art-run-test-group :=
TARGET_TYPES :=
PREBUILD_TYPES :=
COMPILER_TYPES :=
RELOCATE_TYPES :=
TRACE_TYPES :=
GC_TYPES :=
JNI_TYPES :=
IMAGE_TYPES :=
ADDRESS_SIZES_TARGET :=
ADDRESS_SIZES_HOST :=
ALL_ADDRESS_SIZES :=
RUN_TYPES :=

include $(LOCAL_PATH)/Android.libarttest.mk
include art/test/Android.libnativebridgetest.mk
