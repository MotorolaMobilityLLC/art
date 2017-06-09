/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_ASM_SUPPORT_H_
#define ART_RUNTIME_ASM_SUPPORT_H_

#if defined(__cplusplus)
#include "art_method.h"
#include "base/bit_utils.h"
#include "base/callee_save_type.h"
#include "gc/accounting/card_table.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "jit/jit.h"
#include "lock_word.h"
#include "mirror/class.h"
#include "mirror/dex_cache.h"
#include "mirror/string.h"
#include "utils/dex_cache_arrays_layout.h"
#include "runtime.h"
#include "stack.h"
#include "thread.h"
#endif

#include "read_barrier_c.h"

#if defined(__arm__) || defined(__mips__)
// In quick code for ARM and MIPS we make poor use of registers and perform frequent suspend
// checks in the event of loop back edges. The SUSPEND_CHECK_INTERVAL constant is loaded into a
// register at the point of an up-call or after handling a suspend check. It reduces the number of
// loads of the TLS suspend check value by the given amount (turning it into a decrement and compare
// of a register). This increases the time for a thread to respond to requests from GC and the
// debugger, damaging GC performance and creating other unwanted artifacts. For example, this count
// has the effect of making loops and Java code look cold in profilers, where the count is reset
// impacts where samples will occur. Reducing the count as much as possible improves profiler
// accuracy in tools like traceview.
// TODO: get a compiler that can do a proper job of loop optimization and remove this.
#define SUSPEND_CHECK_INTERVAL 96
#endif

#if defined(__cplusplus)

#ifndef ADD_TEST_EQ  // Allow #include-r to replace with their own.
#define ADD_TEST_EQ(x, y) CHECK_EQ(x, y);
#endif

static inline void CheckAsmSupportOffsetsAndSizes() {
#else
#define ADD_TEST_EQ(x, y)
#endif

#if defined(__LP64__)
#define POINTER_SIZE_SHIFT 3
#define POINTER_SIZE art::PointerSize::k64
#else
#define POINTER_SIZE_SHIFT 2
#define POINTER_SIZE art::PointerSize::k32
#endif
ADD_TEST_EQ(static_cast<size_t>(1U << POINTER_SIZE_SHIFT),
            static_cast<size_t>(__SIZEOF_POINTER__))

// Import platform-independent constant defines from our autogenerated list.
// Export new defines (for assembly use) by editing cpp-define-generator def files.
#define DEFINE_CHECK_EQ ADD_TEST_EQ
#include "asm_support_gen.h"

// Offset of field Thread::tlsPtr_.exception.
#define THREAD_EXCEPTION_OFFSET (THREAD_CARD_TABLE_OFFSET + __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_EXCEPTION_OFFSET,
            art::Thread::ExceptionOffset<POINTER_SIZE>().Int32Value())

// Offset of field Thread::tlsPtr_.managed_stack.top_quick_frame_.
#define THREAD_TOP_QUICK_FRAME_OFFSET (THREAD_CARD_TABLE_OFFSET + (3 * __SIZEOF_POINTER__))
ADD_TEST_EQ(THREAD_TOP_QUICK_FRAME_OFFSET,
            art::Thread::TopOfManagedStackOffset<POINTER_SIZE>().Int32Value())

// Offset of field Thread::tlsPtr_.self.
#define THREAD_SELF_OFFSET (THREAD_CARD_TABLE_OFFSET + (9 * __SIZEOF_POINTER__))
ADD_TEST_EQ(THREAD_SELF_OFFSET,
            art::Thread::SelfOffset<POINTER_SIZE>().Int32Value())

// Offset of field Thread::tlsPtr_.thread_local_pos.
#define THREAD_LOCAL_POS_OFFSET (THREAD_CARD_TABLE_OFFSET + 34 * __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_LOCAL_POS_OFFSET,
            art::Thread::ThreadLocalPosOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.thread_local_end.
#define THREAD_LOCAL_END_OFFSET (THREAD_LOCAL_POS_OFFSET + __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_LOCAL_END_OFFSET,
            art::Thread::ThreadLocalEndOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.thread_local_objects.
#define THREAD_LOCAL_OBJECTS_OFFSET (THREAD_LOCAL_END_OFFSET + 2 * __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_LOCAL_OBJECTS_OFFSET,
            art::Thread::ThreadLocalObjectsOffset<POINTER_SIZE>().Int32Value())

// Offset of field Thread::tlsPtr_.mterp_current_ibase.
#define THREAD_CURRENT_IBASE_OFFSET \
    (THREAD_LOCAL_OBJECTS_OFFSET + __SIZEOF_SIZE_T__ + (1 + 161) * __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_CURRENT_IBASE_OFFSET,
            art::Thread::MterpCurrentIBaseOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.mterp_default_ibase.
#define THREAD_DEFAULT_IBASE_OFFSET (THREAD_CURRENT_IBASE_OFFSET + __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_DEFAULT_IBASE_OFFSET,
            art::Thread::MterpDefaultIBaseOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.mterp_alt_ibase.
#define THREAD_ALT_IBASE_OFFSET (THREAD_DEFAULT_IBASE_OFFSET + __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_ALT_IBASE_OFFSET,
            art::Thread::MterpAltIBaseOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.rosalloc_runs.
#define THREAD_ROSALLOC_RUNS_OFFSET (THREAD_ALT_IBASE_OFFSET + __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_ROSALLOC_RUNS_OFFSET,
            art::Thread::RosAllocRunsOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.thread_local_alloc_stack_top.
#define THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET (THREAD_ROSALLOC_RUNS_OFFSET + 16 * __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET,
            art::Thread::ThreadLocalAllocStackTopOffset<POINTER_SIZE>().Int32Value())
// Offset of field Thread::tlsPtr_.thread_local_alloc_stack_end.
#define THREAD_LOCAL_ALLOC_STACK_END_OFFSET (THREAD_ROSALLOC_RUNS_OFFSET + 17 * __SIZEOF_POINTER__)
ADD_TEST_EQ(THREAD_LOCAL_ALLOC_STACK_END_OFFSET,
            art::Thread::ThreadLocalAllocStackEndOffset<POINTER_SIZE>().Int32Value())

// Offsets within ShadowFrame.
#define SHADOWFRAME_LINK_OFFSET 0
ADD_TEST_EQ(SHADOWFRAME_LINK_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::LinkOffset()))
#define SHADOWFRAME_METHOD_OFFSET (SHADOWFRAME_LINK_OFFSET + 1 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_METHOD_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::MethodOffset()))
#define SHADOWFRAME_RESULT_REGISTER_OFFSET (SHADOWFRAME_LINK_OFFSET + 2 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_RESULT_REGISTER_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::ResultRegisterOffset()))
#define SHADOWFRAME_DEX_PC_PTR_OFFSET (SHADOWFRAME_LINK_OFFSET + 3 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_DEX_PC_PTR_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::DexPCPtrOffset()))
#define SHADOWFRAME_CODE_ITEM_OFFSET (SHADOWFRAME_LINK_OFFSET + 4 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_CODE_ITEM_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::CodeItemOffset()))
#define SHADOWFRAME_LOCK_COUNT_DATA_OFFSET (SHADOWFRAME_LINK_OFFSET + 5 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_LOCK_COUNT_DATA_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::LockCountDataOffset()))
#define SHADOWFRAME_NUMBER_OF_VREGS_OFFSET (SHADOWFRAME_LINK_OFFSET + 6 * __SIZEOF_POINTER__)
ADD_TEST_EQ(SHADOWFRAME_NUMBER_OF_VREGS_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::NumberOfVRegsOffset()))
#define SHADOWFRAME_DEX_PC_OFFSET (SHADOWFRAME_NUMBER_OF_VREGS_OFFSET + 4)
ADD_TEST_EQ(SHADOWFRAME_DEX_PC_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::DexPCOffset()))
#define SHADOWFRAME_CACHED_HOTNESS_COUNTDOWN_OFFSET (SHADOWFRAME_NUMBER_OF_VREGS_OFFSET + 8)
ADD_TEST_EQ(SHADOWFRAME_CACHED_HOTNESS_COUNTDOWN_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::CachedHotnessCountdownOffset()))
#define SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET (SHADOWFRAME_NUMBER_OF_VREGS_OFFSET + 10)
ADD_TEST_EQ(SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::HotnessCountdownOffset()))
#define SHADOWFRAME_VREGS_OFFSET (SHADOWFRAME_NUMBER_OF_VREGS_OFFSET + 12)
ADD_TEST_EQ(SHADOWFRAME_VREGS_OFFSET,
            static_cast<int32_t>(art::ShadowFrame::VRegsOffset()))

#if defined(USE_BROOKS_READ_BARRIER)
#define MIRROR_OBJECT_HEADER_SIZE 16
#else
#define MIRROR_OBJECT_HEADER_SIZE 8
#endif
ADD_TEST_EQ(size_t(MIRROR_OBJECT_HEADER_SIZE), sizeof(art::mirror::Object))

// Offsets within java.lang.Class.
#define MIRROR_CLASS_COMPONENT_TYPE_OFFSET (4 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_COMPONENT_TYPE_OFFSET,
            art::mirror::Class::ComponentTypeOffset().Int32Value())
#define MIRROR_CLASS_IF_TABLE_OFFSET (16 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_IF_TABLE_OFFSET,
            art::mirror::Class::IfTableOffset().Int32Value())
#define MIRROR_CLASS_ACCESS_FLAGS_OFFSET (56 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_ACCESS_FLAGS_OFFSET,
            art::mirror::Class::AccessFlagsOffset().Int32Value())
#define MIRROR_CLASS_OBJECT_SIZE_OFFSET (88 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_OBJECT_SIZE_OFFSET,
            art::mirror::Class::ObjectSizeOffset().Int32Value())
#define MIRROR_CLASS_OBJECT_SIZE_ALLOC_FAST_PATH_OFFSET (92 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_OBJECT_SIZE_ALLOC_FAST_PATH_OFFSET,
            art::mirror::Class::ObjectSizeAllocFastPathOffset().Int32Value())
#define MIRROR_CLASS_OBJECT_PRIMITIVE_TYPE_OFFSET (96 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_OBJECT_PRIMITIVE_TYPE_OFFSET,
            art::mirror::Class::PrimitiveTypeOffset().Int32Value())
#define MIRROR_CLASS_STATUS_OFFSET (104 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CLASS_STATUS_OFFSET,
            art::mirror::Class::StatusOffset().Int32Value())

#define PRIMITIVE_TYPE_SIZE_SHIFT_SHIFT 16
ADD_TEST_EQ(PRIMITIVE_TYPE_SIZE_SHIFT_SHIFT,
            static_cast<int>(art::mirror::Class::kPrimitiveTypeSizeShiftShift))

// Array offsets.
#define MIRROR_ARRAY_LENGTH_OFFSET      MIRROR_OBJECT_HEADER_SIZE
ADD_TEST_EQ(MIRROR_ARRAY_LENGTH_OFFSET, art::mirror::Array::LengthOffset().Int32Value())

#define MIRROR_CHAR_ARRAY_DATA_OFFSET   (4 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_CHAR_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value())

#define MIRROR_BOOLEAN_ARRAY_DATA_OFFSET MIRROR_CHAR_ARRAY_DATA_OFFSET
ADD_TEST_EQ(MIRROR_BOOLEAN_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(uint8_t)).Int32Value())

#define MIRROR_BYTE_ARRAY_DATA_OFFSET MIRROR_CHAR_ARRAY_DATA_OFFSET
ADD_TEST_EQ(MIRROR_BYTE_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(int8_t)).Int32Value())

#define MIRROR_SHORT_ARRAY_DATA_OFFSET MIRROR_CHAR_ARRAY_DATA_OFFSET
ADD_TEST_EQ(MIRROR_SHORT_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(int16_t)).Int32Value())

#define MIRROR_INT_ARRAY_DATA_OFFSET MIRROR_CHAR_ARRAY_DATA_OFFSET
ADD_TEST_EQ(MIRROR_INT_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(int32_t)).Int32Value())

#define MIRROR_WIDE_ARRAY_DATA_OFFSET (8 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_WIDE_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(uint64_t)).Int32Value())

#define MIRROR_OBJECT_ARRAY_DATA_OFFSET (4 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_OBJECT_ARRAY_DATA_OFFSET,
    art::mirror::Array::DataOffset(
        sizeof(art::mirror::HeapReference<art::mirror::Object>)).Int32Value())

#define MIRROR_OBJECT_ARRAY_COMPONENT_SIZE 4
ADD_TEST_EQ(static_cast<size_t>(MIRROR_OBJECT_ARRAY_COMPONENT_SIZE),
            sizeof(art::mirror::HeapReference<art::mirror::Object>))

#define MIRROR_LONG_ARRAY_DATA_OFFSET (8 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_LONG_ARRAY_DATA_OFFSET,
            art::mirror::Array::DataOffset(sizeof(uint64_t)).Int32Value())

// Offsets within java.lang.String.
#define MIRROR_STRING_COUNT_OFFSET  MIRROR_OBJECT_HEADER_SIZE
ADD_TEST_EQ(MIRROR_STRING_COUNT_OFFSET, art::mirror::String::CountOffset().Int32Value())

#define MIRROR_STRING_VALUE_OFFSET (8 + MIRROR_OBJECT_HEADER_SIZE)
ADD_TEST_EQ(MIRROR_STRING_VALUE_OFFSET, art::mirror::String::ValueOffset().Int32Value())

// String compression feature.
#define STRING_COMPRESSION_FEATURE 1
ADD_TEST_EQ(STRING_COMPRESSION_FEATURE, art::mirror::kUseStringCompression);

#if defined(__cplusplus)
}  // End of CheckAsmSupportOffsets.
#endif

#endif  // ART_RUNTIME_ASM_SUPPORT_H_
