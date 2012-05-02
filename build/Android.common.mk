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

# TODO: move the LLVM compiler out into a separate shared library too...
# Use llvm as the backend
ifneq ($(wildcard art/USE_LLVM_COMPILER),)
ART_USE_LLVM_COMPILER := true
else
ART_USE_LLVM_COMPILER := false
endif

ifeq ($(ART_USE_LLVM_COMPILER),true)
LLVM_ROOT_PATH := external/llvm
include $(LLVM_ROOT_PATH)/llvm.mk
endif

# directory used for gtests on device
ART_NATIVETEST_DIR := /data/nativetest/art
ART_NATIVETEST_OUT := $(TARGET_OUT_DATA_NATIVE_TESTS)/art

# directory used for tests on device
ART_TEST_DIR := /data/art-test
ART_TEST_OUT := $(TARGET_OUT_DATA)/art-test

ART_CPP_EXTENSION := .cc

ART_C_INCLUDES := \
	external/gtest/include \
	external/icu4c/common \
	external/icu4c/i18n \
	external/valgrind/dynamic_annotations \
	external/zlib \
	art/src

art_cflags := \
	-O2 \
	-ggdb3 \
	-Wall \
	-Werror \
	-Wextra \
	-Wstrict-aliasing=3 \
	-fno-align-jumps \
	-fstrict-aliasing

ifeq ($(HOST_OS),linux)
  art_non_debug_cflags := \
	-Wframe-larger-than=1728
endif

art_debug_cflags := \
	-DDYNAMIC_ANNOTATIONS_ENABLED=1 \
	-UNDEBUG

ART_HOST_CFLAGS := $(art_cflags) -DANDROID_SMP=1

ART_TARGET_CFLAGS := $(art_cflags)
ifeq ($(TARGET_CPU_SMP),true)
  ART_TARGET_CFLAGS += -DANDROID_SMP=1
else
  ART_TARGET_CFLAGS += -DANDROID_SMP=0
endif

# To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
# ART_TARGET_CFLAGS += -fno-omit-frame-pointer -marm -mapcs

ART_HOST_NON_DEBUG_CFLAGS := $(art_non_debug_cflags)
ART_TARGET_NON_DEBUG_CFLAGS := $(art_non_debug_cflags)

# TODO: move -fkeep-inline-functions to art_debug_cflags when target gcc > 4.4 (and -lsupc++)
ART_HOST_DEBUG_CFLAGS := $(art_debug_cflags) -fkeep-inline-functions
ART_HOST_DEBUG_LDLIBS := -lsupc++

ifneq ($(HOST_OS),linux)
  # Some Mac OS pthread header files are broken with -fkeep-inline-functions.
  ART_HOST_DEBUG_CFLAGS := $(filter-out -fkeep-inline-functions,$(ART_HOST_DEBUG_CFLAGS))
endif

ART_TARGET_DEBUG_CFLAGS := $(art_debug_cflags)

ifeq ($(ART_USE_LLVM_COMPILER),true)
PARALLEL_ART_COMPILE_JOBS := -j8
endif

DEX2OAT_SRC_FILES := \
	src/dex2oat.cc

OATDUMP_SRC_FILES := \
	src/oatdump.cc \
	src/disassembler.cc \
	src/disassembler_arm.cc \
	src/disassembler_mips.cc \
	src/disassembler_x86.cc

OATEXEC_SRC_FILES := \
	src/oatexec.cc

LIBART_COMMON_SRC_FILES := \
	src/atomic.cc.arm \
	src/card_table.cc \
	src/check_jni.cc \
	src/class_linker.cc \
	src/class_loader.cc \
	src/compiled_method.cc \
	src/compiler.cc \
	src/debugger.cc \
	src/dex_cache.cc \
	src/dex_file.cc \
	src/dex_file_verifier.cc \
	src/dex_instruction.cc \
	src/dlmalloc.c \
	src/file.cc \
	src/file_linux.cc \
	src/heap.cc \
	src/heap_bitmap.cc \
	src/hprof/hprof.cc \
	src/hprof/hprof_record.cc \
	src/image.cc \
	src/image_writer.cc \
	src/indirect_reference_table.cc \
	src/intern_table.cc \
	src/jdwp/jdwp_event.cc \
	src/jdwp/jdwp_expand_buf.cc \
	src/jdwp/jdwp_handler.cc \
	src/jdwp/jdwp_main.cc \
	src/jdwp/jdwp_socket.cc \
	src/jni_internal.cc \
	src/logging.cc \
	src/mark_stack.cc \
	src/mark_sweep.cc \
	src/mem_map.cc \
	src/memory_region.cc \
	src/monitor.cc \
	src/mutex.cc \
	src/native/dalvik_system_DexFile.cc \
	src/native/dalvik_system_VMDebug.cc \
	src/native/dalvik_system_VMRuntime.cc \
	src/native/dalvik_system_VMStack.cc \
	src/native/dalvik_system_Zygote.cc \
	src/native/java_lang_Class.cc \
	src/native/java_lang_Object.cc \
	src/native/java_lang_Runtime.cc \
	src/native/java_lang_String.cc \
	src/native/java_lang_System.cc \
	src/native/java_lang_Thread.cc \
	src/native/java_lang_Throwable.cc \
	src/native/java_lang_VMClassLoader.cc \
	src/native/java_lang_reflect_Array.cc \
	src/native/java_lang_reflect_Constructor.cc \
	src/native/java_lang_reflect_Field.cc \
	src/native/java_lang_reflect_Method.cc \
	src/native/java_lang_reflect_Proxy.cc \
	src/native/java_util_concurrent_atomic_AtomicLong.cc \
	src/native/org_apache_harmony_dalvik_ddmc_DdmServer.cc \
	src/native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc \
	src/native/sun_misc_Unsafe.cc \
	src/oat/runtime/arm/stub_arm.cc \
	src/oat/runtime/x86/stub_x86.cc \
	src/oat/utils/arm/assembler_arm.cc \
	src/oat/utils/arm/managed_register_arm.cc \
	src/oat/utils/assembler.cc \
	src/oat/utils/x86/assembler_x86.cc \
	src/oat/utils/x86/managed_register_x86.cc \
	src/oat.cc \
	src/oat_file.cc \
	src/oat_writer.cc \
	src/object.cc \
	src/offsets.cc \
	src/os_linux.cc \
	src/primitive.cc \
	src/reference_table.cc \
	src/reflection.cc \
	src/runtime.cc \
	src/runtime_support.cc \
	src/scoped_thread_list_lock.cc \
	src/signal_catcher.cc \
	src/space.cc \
	src/stack.cc \
	src/stringpiece.cc \
	src/stringprintf.cc \
	src/thread.cc \
	src/thread_list.cc \
	src/trace.cc \
	src/utf.cc \
	src/utils.cc \
	src/zip_archive.cc \
	src/verifier/gc_map.cc \
	src/verifier/method_verifier.cc \
	src/verifier/reg_type.cc \
	src/verifier/reg_type_cache.cc \
	src/verifier/register_line.cc

ifeq ($(ART_USE_LLVM_COMPILER),true)
LIBART_COMMON_SRC_FILES += \
	src/compiler_llvm/elf_loader.cc \
	src/compiler_llvm/inferred_reg_category_map.cc \
	src/compiler_llvm/runtime_support_llvm.cc
endif

LIBART_COMMON_SRC_FILES += \
	src/oat/runtime/context.cc \
	src/oat/runtime/support_alloc.cc \
	src/oat/runtime/support_cast.cc \
	src/oat/runtime/support_debug.cc \
	src/oat/runtime/support_dexcache.cc \
	src/oat/runtime/support_field.cc \
	src/oat/runtime/support_fillarray.cc \
	src/oat/runtime/support_invoke.cc \
	src/oat/runtime/support_jni.cc \
	src/oat/runtime/support_locks.cc \
	src/oat/runtime/support_math.cc \
	src/oat/runtime/support_proxy.cc \
	src/oat/runtime/support_stubs.cc \
	src/oat/runtime/support_thread.cc \
	src/oat/runtime/support_throw.cc \
	src/oat/runtime/support_trace.cc

LIBART_TARGET_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/jdwp/jdwp_adb.cc \
	src/logging_android.cc \
	src/monitor_android.cc \
	src/runtime_android.cc \
	src/thread_android.cc

ifeq ($(TARGET_ARCH),arm)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/arm/context_arm.cc.arm \
	src/oat/runtime/arm/oat_support_entrypoints_arm.cc \
	src/oat/runtime/arm/runtime_support_arm.S
else # TARGET_ARCH != arm
ifeq ($(TARGET_ARCH),x86)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/x86/context_x86.cc \
	src/oat/runtime/x86/oat_support_entrypoints_x86.cc \
	src/oat/runtime/x86/runtime_support_x86.S
else # TARGET_ARCH != x86
ifeq ($(TARGET_ARCH),mips)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/mips/oat_support_entrypoints_mips.cc \
	src/oat/runtime/mips/runtime_support_mips.S
else # TARGET_ARCH != mips
$(error unsupported TARGET_ARCH=$(TARGET_ARCH))
endif # TARGET_ARCH != mips
endif # TARGET_ARCH != x86
endif # TARGET_ARCH != arm

ifeq ($(TARGET_ARCH),arm)
LIBART_TARGET_SRC_FILES += src/thread_arm.cc
else # TARGET_ARCH != arm
ifeq ($(TARGET_ARCH),x86)
LIBART_TARGET_SRC_FILES += src/thread_x86.cc
else # TARGET_ARCH != x86
ifeq ($(TARGET_ARCH),mips)
LIBART_TARGET_SRC_FILES += src/thread_mips.cc
else # TARGET_ARCH != mips
$(error unsupported TARGET_ARCH=$(TARGET_ARCH))
endif # TARGET_ARCH != mips
endif # TARGET_ARCH != x86
endif # TARGET_ARCH != arm

LIBART_HOST_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/logging_linux.cc \
	src/monitor_linux.cc \
	src/runtime_linux.cc \
	src/thread_linux.cc

ifeq ($(HOST_ARCH),x86)
LIBART_HOST_SRC_FILES += \
	src/oat/runtime/x86/oat_support_entrypoints_x86.cc \
	src/oat/runtime/x86/context_x86.cc \
	src/oat/runtime/x86/runtime_support_x86.S
else # HOST_ARCH != x86
$(error unsupported HOST_ARCH=$(HOST_ARCH))
endif # HOST_ARCH != x86

ifeq ($(HOST_ARCH),x86)
LIBART_HOST_SRC_FILES += src/thread_x86.cc
else # HOST_ARCH != x86
$(error unsupported HOST_ARCH=$(HOST_ARCH))
endif # HOST_ARCH != x86


LIBART_ENUM_OPERATOR_OUT_HEADER_FILES := \
	src/indirect_reference_table.h \
	src/instruction_set.h \
	src/invoke_type.h \
	src/jdwp/jdwp.h \
	src/jdwp/jdwp_constants.h \
	src/mutex.h \
	src/thread.h \
	src/verifier/method_verifier.h

LIBARTTEST_COMMON_SRC_FILES := \
	test/StackWalk/stack_walk_jni.cc \
	test/ReferenceMap/stack_walk_refmap_jni.cc

TEST_COMMON_SRC_FILES := \
	src/class_linker_test.cc \
	src/compiler_test.cc \
	src/dex_cache_test.cc \
	src/dex_file_test.cc \
	src/dex_instruction_visitor_test.cc \
	src/exception_test.cc \
	src/file_test.cc \
	src/heap_test.cc \
	src/image_test.cc \
	src/indirect_reference_table_test.cc \
	src/intern_table_test.cc \
	src/jni_internal_test.cc \
	src/jni_compiler_test.cc \
	src/oat/utils/arm/managed_register_arm_test.cc \
	src/oat/utils/x86/managed_register_x86_test.cc \
	src/mutex_test.cc \
	src/oat_test.cc \
	src/object_test.cc \
	src/reference_table_test.cc \
	src/runtime_test.cc \
	src/space_test.cc \
	src/utils_test.cc \
	src/zip_archive_test.cc \
	src/verifier/method_verifier_test.cc \
	src/verifier/reg_type_test.cc

TEST_TARGET_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES)

TEST_HOST_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES) \
	src/oat/utils/x86/assembler_x86_test.cc

# subdirectories of test/ which are used as inputs for gtests
TEST_DEX_DIRECTORIES := \
	AbstractMethod \
	AllFields \
	CreateMethodSignature \
	ExceptionHandle \
	IntMath \
	Interfaces \
	Main \
	MyClass \
	MyClassNatives \
	Nested \
	NonStaticLeafMethods \
	ProtoCompare \
	ProtoCompare2 \
	StaticLeafMethods \
	Statics \
	StaticsFromCode \
	XandY

# subdirectories of test/ which are used with test-art-target-oat
# Declare the simplest tests (Main, HelloWorld, and Fibonacci) first, the rest are alphabetical
TEST_OAT_DIRECTORIES := \
	Main \
	HelloWorld \
	Fibonacci \
	\
	ExceptionTest \
	GrowthLimit \
	IntMath \
	Invoke \
	MemUsage \
	ParallelGC \
	ReferenceMap \
	ReflectionTest \
	StackWalk \
	ThreadStress

# TODO: Enable when the StackWalk2 tests are passing
#	StackWalk2 \

ART_BUILD_TARGET := false
ART_BUILD_HOST := false
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  ART_BUILD_TARGET := true
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  ART_BUILD_TARGET := true
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  ART_BUILD_HOST := true
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  ART_BUILD_HOST := true
endif
