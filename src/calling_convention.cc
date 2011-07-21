// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "calling_convention.h"
#include "logging.h"
#include "utils.h"

namespace art {

// Managed runtime calling convention

size_t ManagedRuntimeCallingConvention::FrameSize() {
  LOG(FATAL) << "Unimplemented";
  return 0;
}

bool ManagedRuntimeCallingConvention::HasNext() {
  return itr_position_ < GetMethod()->NumArgs();
}

void ManagedRuntimeCallingConvention::Next() {
  CHECK(HasNext());
  if (((itr_position_ != 0) || GetMethod()->IsStatic()) &&
      GetMethod()->IsParamALongOrDouble(itr_position_)) {
    itr_longs_and_doubles_++;
  }
  itr_position_++;
}

bool ManagedRuntimeCallingConvention::IsCurrentParamPossiblyNull() {
  // for a virtual method, this should never be NULL
  return GetMethod()->IsStatic() || (itr_position_ != 0);
}

size_t ManagedRuntimeCallingConvention::CurrentParamSize() {
  return GetMethod()->ParamSize(itr_position_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamAReference() {
  return GetMethod()->IsParamAReference(itr_position_);
}

// JNI calling convention

size_t JniCallingConvention::FrameSize() {
  // Return address and Method*
  size_t frame_data_size = 2 * kPointerSize;
  // Handles plus 2 words for SHB header
  size_t handle_area_size = (HandleCount() + 2) * kPointerSize;
  return RoundUp(frame_data_size + handle_area_size + SizeOfReturnValue(), 16);
}

size_t JniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kPointerSize, 16);
}

size_t JniCallingConvention::HandleCount() {
  const Method* method = GetMethod();
  return method->NumReferenceArgs() + (method->IsStatic() ? 1 : 0);
}

FrameOffset JniCallingConvention::ReturnValueSaveLocation() {
  size_t start_of_shb = ShbLinkOffset().Int32Value() +  kPointerSize;
  size_t handle_size = kPointerSize * HandleCount();  // size excluding header
  return FrameOffset(start_of_shb + handle_size);
}

bool JniCallingConvention::HasNext() {
  if (itr_position_ <= kObjectOrClass) {
    return true;
  } else {
    unsigned int arg_pos = itr_position_ - (GetMethod()->IsStatic() ? 2 : 1);
    return arg_pos < GetMethod()->NumArgs();
  }
}

void JniCallingConvention::Next() {
  CHECK(HasNext());
  if (itr_position_ > kObjectOrClass) {
    int arg_pos = itr_position_ - (GetMethod()->IsStatic() ? 2 : 1);
    if (GetMethod()->IsParamALongOrDouble(arg_pos)) {
      itr_longs_and_doubles_++;
    }
  }
  itr_position_++;
}

bool JniCallingConvention::IsCurrentParamAReference() {
  switch (itr_position_) {
    case kJniEnv:
      return false;  // JNIEnv*
    case kObjectOrClass:
      return true;   // jobject or jclass
    default: {
      int arg_pos = itr_position_ - (GetMethod()->IsStatic() ? 2 : 1);
      return GetMethod()->IsParamAReference(arg_pos);
    }
  }
}

// Return position of handle holding reference at the current iterator position
FrameOffset JniCallingConvention::CurrentParamHandleOffset() {
  CHECK(IsCurrentParamAReference());
  CHECK_GT(ShbLinkOffset(), ShbNumRefsOffset());
  // Address of 1st handle
  int result = ShbLinkOffset().Int32Value() + kPointerSize;
  if (itr_position_ != kObjectOrClass) {
    bool is_static = GetMethod()->IsStatic();
    int arg_pos = itr_position_ - (is_static ? 2 : 1);
    int previous_refs = GetMethod()->NumReferenceArgsBefore(arg_pos);
    if (is_static) {
      previous_refs++;  // account for jclass
    }
    result += previous_refs * kPointerSize;
  }
  CHECK_GT(result, ShbLinkOffset().Int32Value());
  return FrameOffset(result);
}

size_t JniCallingConvention::CurrentParamSize() {
  if (itr_position_ <= kObjectOrClass) {
    return kPointerSize;  // JNIEnv or jobject/jclass
  } else {
    int arg_pos = itr_position_ - (GetMethod()->IsStatic() ? 2 : 1);
    return GetMethod()->ParamSize(arg_pos);
  }
}

}  // namespace art
