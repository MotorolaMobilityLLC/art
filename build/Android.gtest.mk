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

# The path for which all the dex files are relative, not actually the current directory.
LOCAL_PATH := art/test

include art/build/Android.common_test.mk
include art/build/Android.common_path.mk
include art/build/Android.common_build.mk

# Subdirectories in art/test which contain dex files used as inputs for gtests.
GTEST_DEX_DIRECTORIES := \
  AbstractMethod \
  AllFields \
  DexToDexDecompiler \
  ErroneousA \
  ErroneousB \
  ExceptionHandle \
  GetMethodSignature \
  ImageLayoutA \
  ImageLayoutB \
  IMTA \
  IMTB \
  Instrumentation \
  Interfaces \
  Lookup \
  Main \
  MethodTypes \
  MultiDex \
  MultiDexModifiedSecondary \
  MyClass \
  MyClassNatives \
  Nested \
  NonStaticLeafMethods \
  Packages \
  ProtoCompare \
  ProtoCompare2 \
  ProfileTestMultiDex \
  StaticLeafMethods \
  Statics \
  StaticsFromCode \
  Transaction \
  XandY

# Create build rules for each dex file recording the dependency.
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-gtest,$(dir), \
  $(ART_TARGET_NATIVETEST_OUT),art/build/Android.gtest.mk,ART_TEST_TARGET_GTEST_$(dir)_DEX, \
  ART_TEST_HOST_GTEST_$(dir)_DEX)))

# Create rules for MainStripped, a copy of Main with the classes.dex stripped
# for the oat file assistant tests.
ART_TEST_HOST_GTEST_MainStripped_DEX := $(basename $(ART_TEST_HOST_GTEST_Main_DEX))Stripped$(suffix $(ART_TEST_HOST_GTEST_Main_DEX))
ART_TEST_TARGET_GTEST_MainStripped_DEX := $(basename $(ART_TEST_TARGET_GTEST_Main_DEX))Stripped$(suffix $(ART_TEST_TARGET_GTEST_Main_DEX))

$(ART_TEST_HOST_GTEST_MainStripped_DEX): $(ART_TEST_HOST_GTEST_Main_DEX)
	cp $< $@
	$(call dexpreopt-remove-classes.dex,$@)

$(ART_TEST_TARGET_GTEST_MainStripped_DEX): $(ART_TEST_TARGET_GTEST_Main_DEX)
	cp $< $@
	$(call dexpreopt-remove-classes.dex,$@)

ART_TEST_GTEST_VerifierDeps_SRC := $(abspath $(wildcard $(LOCAL_PATH)/VerifierDeps/*.smali))
ART_TEST_HOST_GTEST_VerifierDeps_DEX := $(dir $(ART_TEST_HOST_GTEST_Main_DEX))$(subst Main,VerifierDeps,$(basename $(notdir $(ART_TEST_HOST_GTEST_Main_DEX))))$(suffix $(ART_TEST_HOST_GTEST_Main_DEX))
ART_TEST_TARGET_GTEST_VerifierDeps_DEX := $(dir $(ART_TEST_TARGET_GTEST_Main_DEX))$(subst Main,VerifierDeps,$(basename $(notdir $(ART_TEST_TARGET_GTEST_Main_DEX))))$(suffix $(ART_TEST_TARGET_GTEST_Main_DEX))

$(ART_TEST_HOST_GTEST_VerifierDeps_DEX): $(ART_TEST_GTEST_VerifierDeps_SRC) $(HOST_OUT_EXECUTABLES)/smali
	 $(HOST_OUT_EXECUTABLES)/smali --output=$@ $(filter %.smali,$^)

$(ART_TEST_TARGET_GTEST_VerifierDeps_DEX): $(ART_TEST_GTEST_VerifierDeps_SRC) $(HOST_OUT_EXECUTABLES)/smali
	 $(HOST_OUT_EXECUTABLES)/smali --output=$@ $(filter %.smali,$^)

# Dex file dependencies for each gtest.
ART_GTEST_dex2oat_environment_tests_DEX_DEPS := Main MainStripped MultiDex MultiDexModifiedSecondary Nested

ART_GTEST_atomic_method_ref_map_test_DEX_DEPS := Interfaces
ART_GTEST_class_linker_test_DEX_DEPS := ErroneousA ErroneousB Interfaces MethodTypes MultiDex MyClass Nested Statics StaticsFromCode
ART_GTEST_class_table_test_DEX_DEPS := XandY
ART_GTEST_compiler_driver_test_DEX_DEPS := AbstractMethod StaticLeafMethods ProfileTestMultiDex
ART_GTEST_dex_cache_test_DEX_DEPS := Main Packages MethodTypes
ART_GTEST_dex_file_test_DEX_DEPS := GetMethodSignature Main Nested
ART_GTEST_dex2oat_test_DEX_DEPS := $(ART_GTEST_dex2oat_environment_tests_DEX_DEPS) Statics
ART_GTEST_exception_test_DEX_DEPS := ExceptionHandle
ART_GTEST_image_test_DEX_DEPS := ImageLayoutA ImageLayoutB
ART_GTEST_imtable_test_DEX_DEPS := IMTA IMTB
ART_GTEST_instrumentation_test_DEX_DEPS := Instrumentation
ART_GTEST_jni_compiler_test_DEX_DEPS := MyClassNatives
ART_GTEST_jni_internal_test_DEX_DEPS := AllFields StaticLeafMethods
ART_GTEST_oat_file_assistant_test_DEX_DEPS := $(ART_GTEST_dex2oat_environment_tests_DEX_DEPS)
ART_GTEST_oat_file_test_DEX_DEPS := Main MultiDex
ART_GTEST_oat_test_DEX_DEPS := Main
ART_GTEST_object_test_DEX_DEPS := ProtoCompare ProtoCompare2 StaticsFromCode XandY
ART_GTEST_proxy_test_DEX_DEPS := Interfaces
ART_GTEST_reflection_test_DEX_DEPS := Main NonStaticLeafMethods StaticLeafMethods
ART_GTEST_profile_assistant_test_DEX_DEPS := ProfileTestMultiDex
ART_GTEST_profile_compilation_info_test_DEX_DEPS := ProfileTestMultiDex
ART_GTEST_runtime_callbacks_test_DEX_DEPS := XandY
ART_GTEST_stub_test_DEX_DEPS := AllFields
ART_GTEST_transaction_test_DEX_DEPS := Transaction
ART_GTEST_type_lookup_table_test_DEX_DEPS := Lookup
ART_GTEST_verifier_deps_test_DEX_DEPS := VerifierDeps MultiDex
ART_GTEST_dex_to_dex_decompiler_test_DEX_DEPS := VerifierDeps DexToDexDecompiler

# The elf writer test has dependencies on core.oat.
ART_GTEST_elf_writer_test_HOST_DEPS := $(HOST_CORE_IMAGE_optimizing_no-pic_64) $(HOST_CORE_IMAGE_optimizing_no-pic_32)
ART_GTEST_elf_writer_test_TARGET_DEPS := $(TARGET_CORE_IMAGE_optimizing_no-pic_64) $(TARGET_CORE_IMAGE_optimizing_no-pic_32)

ART_GTEST_dex2oat_environment_tests_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_pic_64) \
  $(HOST_CORE_IMAGE_optimizing_pic_32) \
  $(HOST_CORE_IMAGE_interpreter_pic_64) \
  $(HOST_CORE_IMAGE_interpreter_pic_32) \
  $(HOST_OUT_EXECUTABLES)/patchoatd
ART_GTEST_dex2oat_environment_tests_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_pic_32) \
  $(TARGET_CORE_IMAGE_interpreter_pic_64) \
  $(TARGET_CORE_IMAGE_interpreter_pic_32) \
  $(TARGET_OUT_EXECUTABLES)/patchoatd

ART_GTEST_oat_file_assistant_test_HOST_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_HOST_DEPS)
ART_GTEST_oat_file_assistant_test_TARGET_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_TARGET_DEPS)


ART_GTEST_dex2oat_test_HOST_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_HOST_DEPS)
ART_GTEST_dex2oat_test_TARGET_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_TARGET_DEPS)

# TODO: document why this is needed.
ART_GTEST_proxy_test_HOST_DEPS := $(HOST_CORE_IMAGE_optimizing_no-pic_64) $(HOST_CORE_IMAGE_optimizing_no-pic_32)

# The dexdump test requires an image and the dexdump utility.
# TODO: rename into dexdump when migration completes
ART_GTEST_dexdump_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_no-pic_64) \
  $(HOST_CORE_IMAGE_optimizing_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/dexdump2
ART_GTEST_dexdump_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_32) \
  dexdump2

# The dexlayout test requires an image and the dexlayout utility.
# TODO: rename into dexdump when migration completes
ART_GTEST_dexlayout_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_no-pic_64) \
  $(HOST_CORE_IMAGE_optimizing_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/dexlayout \
  $(HOST_OUT_EXECUTABLES)/dexdump2
ART_GTEST_dexlayout_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_32) \
  dexlayout \
  dexdump2

# The dexlist test requires an image and the dexlist utility.
ART_GTEST_dexlist_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_no-pic_64) \
  $(HOST_CORE_IMAGE_optimizing_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/dexlist
ART_GTEST_dexlist_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_32) \
  dexlist

# The imgdiag test has dependencies on core.oat since it needs to load it during the test.
# For the host, also add the installed tool (in the base size, that should suffice). For the
# target, just the module is fine, the sync will happen late enough.
ART_GTEST_imgdiag_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_no-pic_64) \
  $(HOST_CORE_IMAGE_optimizing_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/imgdiagd
ART_GTEST_imgdiag_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_32) \
  imgdiagd

# Oatdump test requires an image and oatfile to dump.
ART_GTEST_oatdump_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_optimizing_no-pic_64) \
  $(HOST_CORE_IMAGE_optimizing_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/oatdumpd \
  $(HOST_OUT_EXECUTABLES)/oatdumpds
ART_GTEST_oatdump_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_64) \
  $(TARGET_CORE_IMAGE_optimizing_no-pic_32) \
  oatdump

# Profile assistant tests requires profman utility.
ART_GTEST_profile_assistant_test_HOST_DEPS := \
  $(HOST_OUT_EXECUTABLES)/profmand
ART_GTEST_profile_assistant_test_TARGET_DEPS := \
  profman

# The path for which all the source files are relative, not actually the current directory.
LOCAL_PATH := art

ART_TEST_MODULES := \
    art_cmdline_tests \
    art_compiler_tests \
    art_compiler_host_tests \
    art_dex2oat_tests \
    art_dexdump_tests \
    art_dexlayout_tests \
    art_dexlist_tests \
    art_imgdiag_tests \
    art_oatdump_tests \
    art_profman_tests \
    art_runtime_tests \
    art_runtime_compiler_tests \

ART_TARGET_GTEST_FILES := $(foreach m,$(ART_TEST_MODULES),\
    $(ART_TEST_LIST_device_$(TARGET_ARCH)_$(m)))

ifdef TARGET_2ND_ARCH
2ND_ART_TARGET_GTEST_FILES := $(foreach m,$(ART_TEST_MODULES),\
    $(ART_TEST_LIST_device_$(2ND_TARGET_ARCH)_$(m)))
endif

ART_HOST_GTEST_FILES := $(foreach m,$(ART_TEST_MODULES),\
    $(ART_TEST_LIST_host_$(ART_HOST_ARCH)_$(m)))

ifneq ($(HOST_PREFER_32_BIT),true)
2ND_ART_HOST_GTEST_FILES += $(foreach m,$(ART_TEST_MODULES),\
    $(ART_TEST_LIST_host_$(2ND_ART_HOST_ARCH)_$(m)))
endif

# Variables holding collections of gtest pre-requisits used to run a number of gtests.
ART_TEST_HOST_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST_RULES :=
ART_TEST_TARGET_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST_RULES :=
ART_TEST_HOST_GTEST_DEPENDENCIES :=

ART_GTEST_TARGET_ANDROID_ROOT := '/system'
ifneq ($(ART_TEST_ANDROID_ROOT),)
  ART_GTEST_TARGET_ANDROID_ROOT := $(ART_TEST_ANDROID_ROOT)
endif

ART_VALGRIND_TARGET_DEPENDENCIES := \
  $(TARGET_OUT_EXECUTABLES)/valgrind \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/memcheck-$(TARGET_ARCH)-linux \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/vgpreload_core-$(TARGET_ARCH)-linux.so \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/vgpreload_memcheck-$(TARGET_ARCH)-linux.so \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/default.supp

ifdef TARGET_2ND_ARCH
ART_VALGRIND_TARGET_DEPENDENCIES += \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/memcheck-$(TARGET_2ND_ARCH)-linux \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/vgpreload_core-$(TARGET_2ND_ARCH)-linux.so \
  $(TARGET_OUT_SHARED_LIBRARIES)/valgrind/vgpreload_memcheck-$(TARGET_2ND_ARCH)-linux.so
endif

include $(CLEAR_VARS)
LOCAL_MODULE := valgrind-target-suppressions.txt
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := test/valgrind-target-suppressions.txt
LOCAL_MODULE_PATH := $(ART_TARGET_TEST_OUT)
include $(BUILD_PREBUILT)

# Define a make rule for a target device gtest.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): path relative to $OUT to the test binary
# $(3): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
# $(4): LD_LIBRARY_PATH or undefined - used in case libartd.so is not in /system/lib/
define define-art-gtest-rule-target
  gtest_rule := test-art-target-gtest-$(1)$$($(3)ART_PHONY_TEST_TARGET_SUFFIX)
  gtest_exe := $(OUT_DIR)/$(2)
  gtest_target_exe := $$(patsubst $(PRODUCT_OUT)/%,/%,$$(gtest_exe))

  # Add the test dependencies to test-art-target-sync, which will be a prerequisite for the test
  # to ensure files are pushed to the device.
  TEST_ART_TARGET_SYNC_DEPS += \
    $$(ART_GTEST_$(1)_TARGET_DEPS) \
    $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_TARGET_GTEST_$(file)_DEX)) \
    $$(gtest_exe) \
    $$($(3)TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so \
    $$($(3)TARGET_OUT_SHARED_LIBRARIES)/libopenjdkd.so \
    $$(TARGET_OUT_JAVA_LIBRARIES)/core-libart-testdex.jar \
    $$(TARGET_OUT_JAVA_LIBRARIES)/core-oj-testdex.jar \
    $$(ART_TARGET_TEST_OUT)/valgrind-target-suppressions.txt

$$(gtest_rule) valgrind-$$(gtest_rule): PRIVATE_TARGET_EXE := $$(gtest_target_exe)

.PHONY: $$(gtest_rule)
$$(gtest_rule): test-art-target-sync
	$(hide) adb shell touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID
	$(hide) adb shell rm $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID
	$(hide) adb shell chmod 755 $$(PRIVATE_TARGET_EXE)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  (adb shell "$(GCOV_ENV) LD_LIBRARY_PATH=$(4) ANDROID_ROOT=$(ART_GTEST_TARGET_ANDROID_ROOT) \
	    $$(PRIVATE_TARGET_EXE) && touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID" \
	  && (adb pull $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID /tmp/ \
	      && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@))
	$(hide) rm -f /tmp/$$@-$$$$PPID

  ART_TEST_TARGET_GTEST$($(3)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_$(1)_RULES += $$(gtest_rule)

.PHONY: valgrind-$$(gtest_rule)
valgrind-$$(gtest_rule): $(ART_VALGRIND_TARGET_DEPENDENCIES) test-art-target-sync
	$(hide) adb shell touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID
	$(hide) adb shell rm $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID
	$(hide) adb shell chmod 755 $$(PRIVATE_TARGET_EXE)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  (adb shell "$(GCOV_ENV) LD_LIBRARY_PATH=$(4) ANDROID_ROOT=$(ART_GTEST_TARGET_ANDROID_ROOT) \
	    valgrind --leak-check=full --error-exitcode=1 --workaround-gcc296-bugs=yes \
	    --suppressions=$(ART_TARGET_TEST_DIR)/valgrind-target-suppressions.txt \
	    --num-callers=50 --show-mismatched-frees=no \
	    $$(PRIVATE_TARGET_EXE) && touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID" \
	  && (adb pull $(ART_TARGET_TEST_DIR)/$(TARGET_$(3)ARCH)/$$@-$$$$PPID /tmp/ \
	      && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@))
	$(hide) rm -f /tmp/$$@-$$$$PPID

  ART_TEST_TARGET_VALGRIND_GTEST$$($(3)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += valgrind-$$(gtest_rule)
  ART_TEST_TARGET_VALGRIND_GTEST_RULES += valgrind-$$(gtest_rule)
  ART_TEST_TARGET_VALGRIND_GTEST_$(1)_RULES += valgrind-$$(gtest_rule)

  # Clear locally defined variables.
  valgrind_gtest_rule :=
  gtest_rule :=
  gtest_exe :=
  gtest_target_exe :=
endef  # define-art-gtest-rule-target

ART_VALGRIND_DEPENDENCIES := \
  $(HOST_OUT_EXECUTABLES)/valgrind \
  $(HOST_OUT)/lib64/valgrind/memcheck-amd64-linux \
  $(HOST_OUT)/lib64/valgrind/memcheck-x86-linux \
  $(HOST_OUT)/lib64/valgrind/default.supp \
  $(HOST_OUT)/lib64/valgrind/vgpreload_core-amd64-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_core-x86-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_memcheck-amd64-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_memcheck-x86-linux.so

# Define make rules for a host gtests.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): path relative to $OUT to the test binary
# $(3): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-rule-host
  gtest_rule := test-art-host-gtest-$(1)$$($(3)ART_PHONY_TEST_HOST_SUFFIX)
  gtest_exe := $(OUT_DIR)/$(2)
  # Dependencies for all host gtests.
  gtest_deps := $$(HOST_CORE_DEX_LOCATIONS) \
    $$($(3)ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$$(ART_HOST_SHLIB_EXTENSION) \
    $$($(3)ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkd$$(ART_HOST_SHLIB_EXTENSION) \
    $$(gtest_exe) \
    $$(ART_GTEST_$(1)_HOST_DEPS) \
    $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_HOST_GTEST_$(file)_DEX))

  ART_TEST_HOST_GTEST_DEPENDENCIES += $$(gtest_deps)

.PHONY: $$(gtest_rule)
$$(gtest_rule): $$(gtest_exe) $$(gtest_deps)
	$(hide) ($$(call ART_TEST_SKIP,$$@) && $$< && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@)

  ART_TEST_HOST_GTEST$$($(3)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_$(1)_RULES += $$(gtest_rule)


.PHONY: valgrind-$$(gtest_rule)
valgrind-$$(gtest_rule): $$(gtest_exe) $$(gtest_deps) $(ART_VALGRIND_DEPENDENCIES)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  VALGRIND_LIB=$(HOST_OUT)/lib64/valgrind \
	  $(HOST_OUT_EXECUTABLES)/valgrind --leak-check=full --error-exitcode=1 \
	    --suppressions=art/test/valgrind-suppressions.txt --num-callers=50 \
	    $$< && \
	    $$(call ART_TEST_PASSED,$$@) || $$(call ART_TEST_FAILED,$$@)

  ART_TEST_HOST_VALGRIND_GTEST$$($(3)ART_PHONY_TEST_HOST_SUFFIX)_RULES += valgrind-$$(gtest_rule)
  ART_TEST_HOST_VALGRIND_GTEST_RULES += valgrind-$$(gtest_rule)
  ART_TEST_HOST_VALGRIND_GTEST_$(1)_RULES += valgrind-$$(gtest_rule)

  # Clear locally defined variables.
  valgrind_gtest_rule :=
  gtest_rule :=
  gtest_exe :=
  gtest_deps :=
endef  # define-art-gtest-rule-host

# Define the rules to build and run host and target gtests.
# $(1): file name
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-target
  art_gtest_filename := $(1)

  include $$(CLEAR_VARS)
  art_gtest_name := $$(notdir $$(basename $$(art_gtest_filename)))

  library_path :=
  2ND_library_path :=
  ifneq ($$(ART_TEST_ANDROID_ROOT),)
    ifdef TARGET_2ND_ARCH
      2ND_library_path := $$(ART_TEST_ANDROID_ROOT)/lib
      library_path := $$(ART_TEST_ANDROID_ROOT)/lib64
    else
      ifneq ($(filter %64,$(TARGET_ARCH)),)
        library_path := $$(ART_TEST_ANDROID_ROOT)/lib64
      else
        library_path := $$(ART_TEST_ANDROID_ROOT)/lib
      endif
    endif
  endif

  ifndef ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES
    ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
    ART_TEST_TARGET_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
  endif
  $$(eval $$(call define-art-gtest-rule-target,$$(art_gtest_name),$$(art_gtest_filename),$(2),$$($(2)library_path)))

  # Clear locally defined variables.
  art_gtest_filename :=
  art_gtest_name :=
  library_path :=
  2ND_library_path :=
endef  # define-art-gtest-target

# $(1): file name
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-host
  art_gtest_filename := $(1)

  include $$(CLEAR_VARS)
  art_gtest_name := $$(notdir $$(basename $$(art_gtest_filename)))
  ifndef ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES
    ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES :=
    ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
  endif
  $$(eval $$(call define-art-gtest-rule-host,$$(art_gtest_name),$$(art_gtest_filename),$(2)))

  # Clear locally defined variables.
  art_gtest_filename :=
  art_gtest_name :=
endef  # define-art-gtest-host

# Define the rules to build and run gtests for both archs on target.
# $(1): test name
define define-art-gtest-target-both
  art_gtest_name := $(1)

    # A rule to run the different architecture versions of the gtest.
.PHONY: test-art-target-gtest-$$(art_gtest_name)
test-art-target-gtest-$$(art_gtest_name): $$(ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

.PHONY: valgrind-test-art-target-gtest-$$(art_gtest_name)
valgrind-test-art-target-gtest-$$(art_gtest_name): $$(ART_TEST_TARGET_VALGRIND_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear now unused variables.
  ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
  ART_TEST_TARGET_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
  art_gtest_name :=
endef  # define-art-gtest-target-both

# Define the rules to build and run gtests for both archs on host.
# $(1): test name
define define-art-gtest-host-both
  art_gtest_name := $(1)

.PHONY: test-art-host-gtest-$$(art_gtest_name)
test-art-host-gtest-$$(art_gtest_name): $$(ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

.PHONY: valgrind-test-art-host-gtest-$$(art_gtest_name)
valgrind-test-art-host-gtest-$$(art_gtest_name): $$(ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear now unused variables.
  ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES :=
  ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
  art_gtest_name :=
endef  # define-art-gtest-host-both

ifeq ($(ART_BUILD_TARGET),true)
  $(foreach file,$(ART_TARGET_GTEST_FILES), $(eval $(call define-art-gtest-target,$(file),)))
  ifdef TARGET_2ND_ARCH
    $(foreach file,$(2ND_ART_TARGET_GTEST_FILES), $(eval $(call define-art-gtest-target,$(file),2ND_)))
  endif
  # Rules to run the different architecture versions of the gtest.
  $(foreach file,$(ART_TARGET_GTEST_FILES), $(eval $(call define-art-gtest-target-both,$$(notdir $$(basename $$(file))))))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(foreach file,$(ART_HOST_GTEST_FILES), $(eval $(call define-art-gtest-host,$(file),)))
  ifneq ($(HOST_PREFER_32_BIT),true)
    $(foreach file,$(2ND_ART_HOST_GTEST_FILES), $(eval $(call define-art-gtest-host,$(file),2ND_)))
  endif
  # Rules to run the different architecture versions of the gtest.
  $(foreach file,$(ART_HOST_GTEST_FILES), $(eval $(call define-art-gtest-host-both,$$(notdir $$(basename $$(file))))))
endif

# Used outside the art project to get a list of the current tests
RUNTIME_TARGET_GTEST_MAKE_TARGETS :=
$(foreach file, $(ART_TARGET_GTEST_FILES), $(eval RUNTIME_TARGET_GTEST_MAKE_TARGETS += $$(notdir $$(basename $$(file)))))
COMPILER_TARGET_GTEST_MAKE_TARGETS :=

# Define all the combinations of host/target, valgrind and suffix such as:
# test-art-host-gtest or valgrind-test-art-host-gtest64
# $(1): host or target
# $(2): HOST or TARGET
# $(3): valgrind- or undefined
# $(4): undefined, 32 or 64
define define-test-art-gtest-combination
  ifeq ($(1),host)
    ifneq ($(2),HOST)
      $$(error argument mismatch $(1) and ($2))
    endif
  else
    ifneq ($(1),target)
      $$(error found $(1) expected host or target)
    endif
    ifneq ($(2),TARGET)
      $$(error argument mismatch $(1) and ($2))
    endif
  endif

  rule_name := $(3)test-art-$(1)-gtest$(4)
  ifeq ($(3),valgrind-)
    dependencies := $$(ART_TEST_$(2)_VALGRIND_GTEST$(4)_RULES)
  else
    dependencies := $$(ART_TEST_$(2)_GTEST$(4)_RULES)
  endif

.PHONY: $$(rule_name)
$$(rule_name): $$(dependencies)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear locally defined variables.
  rule_name :=
  dependencies :=
endef  # define-test-art-gtest-combination

$(eval $(call define-test-art-gtest-combination,target,TARGET,,))
$(eval $(call define-test-art-gtest-combination,target,TARGET,valgrind-,))
$(eval $(call define-test-art-gtest-combination,target,TARGET,,$(ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,target,TARGET,valgrind-,$(ART_PHONY_TEST_TARGET_SUFFIX)))
ifdef TARGET_2ND_ARCH
$(eval $(call define-test-art-gtest-combination,target,TARGET,,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,target,TARGET,valgrind-,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
endif
$(eval $(call define-test-art-gtest-combination,host,HOST,,))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,))
$(eval $(call define-test-art-gtest-combination,host,HOST,,$(ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,$(ART_PHONY_TEST_HOST_SUFFIX)))
ifneq ($(HOST_PREFER_32_BIT),true)
$(eval $(call define-test-art-gtest-combination,host,HOST,,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
endif

# Clear locally defined variables.
define-art-gtest-rule-target :=
define-art-gtest-rule-host :=
define-art-gtest :=
define-test-art-gtest-combination :=
RUNTIME_GTEST_COMMON_SRC_FILES :=
COMPILER_GTEST_COMMON_SRC_FILES :=
RUNTIME_GTEST_TARGET_SRC_FILES :=
RUNTIME_GTEST_HOST_SRC_FILES :=
COMPILER_GTEST_TARGET_SRC_FILES :=
COMPILER_GTEST_HOST_SRC_FILES :=
ART_TEST_HOST_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST_RULES :=
ART_TEST_TARGET_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_VALGRIND_GTEST_RULES :=
ART_GTEST_TARGET_ANDROID_ROOT :=
ART_GTEST_class_linker_test_DEX_DEPS :=
ART_GTEST_class_table_test_DEX_DEPS :=
ART_GTEST_compiler_driver_test_DEX_DEPS :=
ART_GTEST_dex_file_test_DEX_DEPS :=
ART_GTEST_exception_test_DEX_DEPS :=
ART_GTEST_elf_writer_test_HOST_DEPS :=
ART_GTEST_elf_writer_test_TARGET_DEPS :=
ART_GTEST_imtable_test_DEX_DEPS :=
ART_GTEST_jni_compiler_test_DEX_DEPS :=
ART_GTEST_jni_internal_test_DEX_DEPS :=
ART_GTEST_oat_file_assistant_test_DEX_DEPS :=
ART_GTEST_oat_file_assistant_test_HOST_DEPS :=
ART_GTEST_oat_file_assistant_test_TARGET_DEPS :=
ART_GTEST_dex2oat_test_DEX_DEPS :=
ART_GTEST_dex2oat_test_HOST_DEPS :=
ART_GTEST_dex2oat_test_TARGET_DEPS :=
ART_GTEST_object_test_DEX_DEPS :=
ART_GTEST_proxy_test_DEX_DEPS :=
ART_GTEST_reflection_test_DEX_DEPS :=
ART_GTEST_stub_test_DEX_DEPS :=
ART_GTEST_transaction_test_DEX_DEPS :=
ART_GTEST_dex2oat_environment_tests_DEX_DEPS :=
ART_GTEST_verifier_deps_test_DEX_DEPS :=
ART_VALGRIND_DEPENDENCIES :=
ART_VALGRIND_TARGET_DEPENDENCIES :=
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_TARGET_GTEST_$(dir)_DEX :=))
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_HOST_GTEST_$(dir)_DEX :=))
ART_TEST_HOST_GTEST_MainStripped_DEX :=
ART_TEST_TARGET_GTEST_MainStripped_DEX :=
ART_TEST_GTEST_VerifierDeps_SRC :=
ART_TEST_HOST_GTEST_VerifierDeps_DEX :=
ART_TEST_TARGET_GTEST_VerifierDeps_DEX :=
GTEST_DEX_DIRECTORIES :=
LOCAL_PATH :=
