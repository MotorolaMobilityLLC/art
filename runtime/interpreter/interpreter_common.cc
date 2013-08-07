/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "interpreter_common.h"

namespace art {
namespace interpreter {

template<InvokeType type, bool is_range, bool do_access_check>
bool DoInvoke(Thread* self, ShadowFrame& shadow_frame,
              const Instruction* inst, JValue* result) {
  uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  Object* receiver = (type == kStatic) ? NULL : shadow_frame.GetVRegReference(vregC);
  ArtMethod* method = FindMethodFromCode(method_idx, receiver, shadow_frame.GetMethod(), self,
                                         do_access_check, type);
  if (UNLIKELY(method == NULL)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  } else if (UNLIKELY(method->IsAbstract())) {
    ThrowAbstractMethodError(method);
    result->SetJ(0);
    return false;
  }

  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (LIKELY(code_item != NULL)) {
    num_regs = code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else {
    DCHECK(method->IsNative() || method->IsProxyMethod());
    num_regs = num_ins = ArtMethod::NumArgRegisters(mh.GetShorty());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }

  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* new_shadow_frame(ShadowFrame::Create(num_regs, &shadow_frame, method, 0, memory));
  size_t cur_reg = num_regs - num_ins;
  if (receiver != NULL) {
    new_shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  }

  size_t arg_offset = (receiver == NULL) ? 0 : 1;
  const char* shorty = mh.GetShorty();
  uint32_t arg[5];
  if (!is_range) {
    inst->GetArgs(arg);
  }
  for (size_t shorty_pos = 0; cur_reg < num_regs; ++shorty_pos, cur_reg++, arg_offset++) {
    DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
    size_t arg_pos = is_range ? vregC + arg_offset : arg[arg_offset];
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        Object* o = shadow_frame.GetVRegReference(arg_pos);
        new_shadow_frame->SetVRegReference(cur_reg, o);
        break;
      }
      case 'J': case 'D': {
        uint64_t wide_value = (static_cast<uint64_t>(shadow_frame.GetVReg(arg_pos + 1)) << 32) |
                              static_cast<uint32_t>(shadow_frame.GetVReg(arg_pos));
        new_shadow_frame->SetVRegLong(cur_reg, wide_value);
        cur_reg++;
        arg_offset++;
        break;
      }
      default:
        new_shadow_frame->SetVReg(cur_reg, shadow_frame.GetVReg(arg_pos));
        break;
    }
  }

  if (LIKELY(Runtime::Current()->IsStarted())) {
    (method->GetEntryPointFromInterpreter())(self, mh, code_item, new_shadow_frame, result);
  } else {
    UnstartedRuntimeInvoke(self, mh, code_item, new_shadow_frame, result, num_regs - num_ins);
  }
  return !self->IsExceptionPending();
}

template<bool is_range>
bool DoInvokeVirtualQuick(Thread* self, ShadowFrame& shadow_frame,
                          const Instruction* inst, JValue* result) {
  uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  Object* receiver = shadow_frame.GetVRegReference(vregC);
  if (UNLIKELY(receiver == NULL)) {
    // We lost the reference to the method index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
    return false;
  }
  uint32_t vtable_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  // TODO: use ObjectArray<T>::GetWithoutChecks ?
  ArtMethod* method = receiver->GetClass()->GetVTable()->Get(vtable_idx);
  if (UNLIKELY(method == NULL)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  } else if (UNLIKELY(method->IsAbstract())) {
    ThrowAbstractMethodError(method);
    result->SetJ(0);
    return false;
  }

  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (code_item != NULL) {
    num_regs = code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else {
    DCHECK(method->IsNative() || method->IsProxyMethod());
    num_regs = num_ins = ArtMethod::NumArgRegisters(mh.GetShorty());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }

  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* new_shadow_frame(ShadowFrame::Create(num_regs, &shadow_frame,
                                                    method, 0, memory));
  size_t cur_reg = num_regs - num_ins;
  if (receiver != NULL) {
    new_shadow_frame->SetVRegReference(cur_reg, receiver);
    ++cur_reg;
  }

  size_t arg_offset = (receiver == NULL) ? 0 : 1;
  const char* shorty = mh.GetShorty();
  uint32_t arg[5];
  if (!is_range) {
    inst->GetArgs(arg);
  }
  for (size_t shorty_pos = 0; cur_reg < num_regs; ++shorty_pos, cur_reg++, arg_offset++) {
    DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
    size_t arg_pos = is_range ? vregC + arg_offset : arg[arg_offset];
    switch (shorty[shorty_pos + 1]) {
      case 'L': {
        Object* o = shadow_frame.GetVRegReference(arg_pos);
        new_shadow_frame->SetVRegReference(cur_reg, o);
        break;
      }
      case 'J': case 'D': {
        uint64_t wide_value = (static_cast<uint64_t>(shadow_frame.GetVReg(arg_pos + 1)) << 32) |
                              static_cast<uint32_t>(shadow_frame.GetVReg(arg_pos));
        new_shadow_frame->SetVRegLong(cur_reg, wide_value);
        cur_reg++;
        arg_offset++;
        break;
      }
      default:
        new_shadow_frame->SetVReg(cur_reg, shadow_frame.GetVReg(arg_pos));
        break;
    }
  }

  if (LIKELY(Runtime::Current()->IsStarted())) {
    (method->GetEntryPointFromInterpreter())(self, mh, code_item, new_shadow_frame, result);
  } else {
    UnstartedRuntimeInvoke(self, mh, code_item, new_shadow_frame, result, num_regs - num_ins);
  }
  return !self->IsExceptionPending();
}

template <bool is_range, bool do_access_check>
bool DoFilledNewArray(const Instruction* inst, const ShadowFrame& shadow_frame,
                      Thread* self, JValue* result) {
  DCHECK(inst->Opcode() == Instruction::FILLED_NEW_ARRAY ||
         inst->Opcode() == Instruction::FILLED_NEW_ARRAY_RANGE);
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (!is_range) {
    // Checks FILLED_NEW_ARRAY's length does not exceed 5 arguments.
    CHECK_LE(length, 5);
  }
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return false;
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  Class* arrayClass = ResolveVerifyAndClinit(type_idx, shadow_frame.GetMethod(),
                                             self, false, do_access_check);
  if (UNLIKELY(arrayClass == NULL)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  CHECK(arrayClass->IsArrayClass());
  Class* componentClass = arrayClass->GetComponentType();
  if (UNLIKELY(componentClass->IsPrimitive() && !componentClass->IsPrimitiveInt())) {
    if (componentClass->IsPrimitiveLong() || componentClass->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(componentClass).c_str());
    } else {
      self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                               "Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but \'int\'",
                               PrettyDescriptor(componentClass).c_str());
    }
    return false;
  }
  Object* newArray = Array::Alloc(self, arrayClass, length);
  if (UNLIKELY(newArray == NULL)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  if (is_range) {
    uint32_t vregC = inst->VRegC_3rc();
    const bool is_primitive_int_component = componentClass->IsPrimitiveInt();
    for (int32_t i = 0; i < length; ++i) {
      if (is_primitive_int_component) {
        newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(vregC + i));
      } else {
        newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(vregC + i));
      }
    }
  } else {
    uint32_t arg[5];
    inst->GetArgs(arg);
    const bool is_primitive_int_component = componentClass->IsPrimitiveInt();
    for (int32_t i = 0; i < length; ++i) {
      if (is_primitive_int_component) {
        newArray->AsIntArray()->Set(i, shadow_frame.GetVReg(arg[i]));
      } else {
        newArray->AsObjectArray<Object>()->Set(i, shadow_frame.GetVRegReference(arg[i]));
      }
    }
  }

  result->SetL(newArray);
  return true;
}

void UnstartedRuntimeInvoke(Thread* self, MethodHelper& mh,
                            const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame,
                            JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "java.lang.Class java.lang.Class.forName(java.lang.String)") {
    std::string descriptor(DotToDescriptor(shadow_frame->GetVRegReference(arg_offset)->AsString()->ToModifiedUtf8().c_str()));
    ClassLoader* class_loader = NULL;  // shadow_frame.GetMethod()->GetDeclaringClass()->GetClassLoader();
    Class* found = Runtime::Current()->GetClassLinker()->FindClass(descriptor.c_str(),
                                                                   class_loader);
    CHECK(found != NULL) << "Class.forName failed in un-started runtime for class: "
        << PrettyDescriptor(descriptor);
    result->SetL(found);
  } else if (name == "java.lang.Object java.lang.Class.newInstance()") {
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    ArtMethod* c = klass->FindDeclaredDirectMethod("<init>", "()V");
    CHECK(c != NULL);
    SirtRef<Object> obj(self, klass->AllocObject(self));
    CHECK(obj.get() != NULL);
    EnterInterpreterFromInvoke(self, c, obj.get(), NULL, NULL);
    result->SetL(obj.get());
  } else if (name == "java.lang.reflect.Field java.lang.Class.getDeclaredField(java.lang.String)") {
    // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
    // going the reflective Dex way.
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    String* name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
    ArtField* found = NULL;
    FieldHelper fh;
    ObjectArray<ArtField>* fields = klass->GetIFields();
    for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
      ArtField* f = fields->Get(i);
      fh.ChangeField(f);
      if (name->Equals(fh.GetName())) {
        found = f;
      }
    }
    if (found == NULL) {
      fields = klass->GetSFields();
      for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
        ArtField* f = fields->Get(i);
        fh.ChangeField(f);
        if (name->Equals(fh.GetName())) {
          found = f;
        }
      }
    }
    CHECK(found != NULL)
      << "Failed to find field in Class.getDeclaredField in un-started runtime. name="
      << name->ToModifiedUtf8() << " class=" << PrettyDescriptor(klass);
    // TODO: getDeclaredField calls GetType once the field is found to ensure a
    //       NoClassDefFoundError is thrown if the field's type cannot be resolved.
    Class* jlr_Field = self->DecodeJObject(WellKnownClasses::java_lang_reflect_Field)->AsClass();
    SirtRef<Object> field(self, jlr_Field->AllocObject(self));
    CHECK(field.get() != NULL);
    ArtMethod* c = jlr_Field->FindDeclaredDirectMethod("<init>", "(Ljava/lang/reflect/ArtField;)V");
    uint32_t args[1];
    args[0] = reinterpret_cast<uint32_t>(found);
    EnterInterpreterFromInvoke(self, c, field.get(), args, NULL);
    result->SetL(field.get());
  } else if (name == "void java.lang.System.arraycopy(java.lang.Object, int, java.lang.Object, int, int)" ||
             name == "void java.lang.System.arraycopy(char[], int, char[], int, int)") {
    // Special case array copying without initializing System.
    Class* ctype = shadow_frame->GetVRegReference(arg_offset)->GetClass()->GetComponentType();
    jint srcPos = shadow_frame->GetVReg(arg_offset + 1);
    jint dstPos = shadow_frame->GetVReg(arg_offset + 3);
    jint length = shadow_frame->GetVReg(arg_offset + 4);
    if (!ctype->IsPrimitive()) {
      ObjectArray<Object>* src = shadow_frame->GetVRegReference(arg_offset)->AsObjectArray<Object>();
      ObjectArray<Object>* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsObjectArray<Object>();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveChar()) {
      CharArray* src = shadow_frame->GetVRegReference(arg_offset)->AsCharArray();
      CharArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveInt()) {
      IntArray* src = shadow_frame->GetVRegReference(arg_offset)->AsIntArray();
      IntArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsIntArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else {
      UNIMPLEMENTED(FATAL) << "System.arraycopy of unexpected type: " << PrettyDescriptor(ctype);
    }
  } else {
    // Not special, continue with regular interpreter execution.
    artInterpreterToInterpreterBridge(self, mh, code_item, shadow_frame, result);
  }
}

// Explicit DoInvoke template function declarations.
#define EXPLICIT_DO_INVOKE_TEMPLATE_DECL(_type, _is_range_, _check) \
  template bool DoInvoke<_type, _is_range_, _check>(Thread* self, ShadowFrame& shadow_frame, \
                                                    const Instruction* inst, JValue* result)

#define EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(_type) \
  EXPLICIT_DO_INVOKE_TEMPLATE_DECL(_type, false, false); \
  EXPLICIT_DO_INVOKE_TEMPLATE_DECL(_type, false, true);  \
  EXPLICIT_DO_INVOKE_TEMPLATE_DECL(_type, true, false);  \
  EXPLICIT_DO_INVOKE_TEMPLATE_DECL(_type, true, true)

EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(kStatic);
EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(kDirect);
EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(kVirtual);
EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(kSuper);
EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS(kInterface);

#undef EXPLICIT_DO_INVOKE_TEMPLATE_DECL_VARIANTS
#undef EXPLICIT_DO_INVOKE_TEMPLATE_DECL

// Explicit DoInvokeVirtualQuick template function declarations.
#define EXPLICIT_DO_INVOKE_VIRTUAL_QUICK_TEMPLATE_DECL(_is_range)                       \
template bool DoInvokeVirtualQuick<_is_range>(Thread* self, ShadowFrame& shadow_frame,  \
                                              const Instruction* inst, JValue* result)

EXPLICIT_DO_INVOKE_VIRTUAL_QUICK_TEMPLATE_DECL(false);
EXPLICIT_DO_INVOKE_VIRTUAL_QUICK_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_INVOKE_VIRTUAL_QUICK_TEMPLATE_DECL

// Explicit DoFilledNewArray template function declarations.
#define EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(_is_range_, _check)                \
  template bool DoFilledNewArray<_is_range_, _check>(const Instruction* inst,         \
                                                     const ShadowFrame& shadow_frame, \
                                                     Thread* self, JValue* result)
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, false);
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, true);
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, false);
EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, true);
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL

}  // namespace interpreter
}  // namespace art
