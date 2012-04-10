// Generated with ../tools/gen_art_module_cc.sh


#pragma GCC diagnostic ignored "-Wframe-larger-than="
// TODO: Remove this pragma after llc can generate makeLLVMModuleContents()
// with smaller frame size.

#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/Module.h>
#include <llvm/Type.h>

#include <vector>

using namespace llvm;

namespace art {
namespace compiler_llvm {


// Generated by llvm2cpp - DO NOT MODIFY!


Module* makeLLVMModuleContents(Module *mod) {

mod->setModuleIdentifier("art_module.ll");

// Type Definitions
std::vector<Type*>FuncTy_0_args;
StructType *StructTy_JavaObject = mod->getTypeByName("JavaObject");
if (!StructTy_JavaObject) {
StructTy_JavaObject = StructType::create(mod->getContext(), "JavaObject");
}
std::vector<Type*>StructTy_JavaObject_fields;
if (StructTy_JavaObject->isOpaque()) {
StructTy_JavaObject->setBody(StructTy_JavaObject_fields, /*isPacked=*/false);
}

PointerType* PointerTy_1 = PointerType::get(StructTy_JavaObject, 0);

FuncTy_0_args.push_back(PointerTy_1);
StructType *StructTy_ShadowFrame = mod->getTypeByName("ShadowFrame");
if (!StructTy_ShadowFrame) {
StructTy_ShadowFrame = StructType::create(mod->getContext(), "ShadowFrame");
}
std::vector<Type*>StructTy_ShadowFrame_fields;
PointerType* PointerTy_2 = PointerType::get(StructTy_ShadowFrame, 0);

StructTy_ShadowFrame_fields.push_back(PointerTy_2);
StructTy_ShadowFrame_fields.push_back(PointerTy_1);
StructTy_ShadowFrame_fields.push_back(IntegerType::get(mod->getContext(), 32));
StructTy_ShadowFrame_fields.push_back(IntegerType::get(mod->getContext(), 32));
if (StructTy_ShadowFrame->isOpaque()) {
StructTy_ShadowFrame->setBody(StructTy_ShadowFrame_fields, /*isPacked=*/false);
}


FuncTy_0_args.push_back(PointerTy_2);
FunctionType* FuncTy_0 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_0_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_3_args;
FunctionType* FuncTy_3 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_3_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_4_args;
FuncTy_4_args.push_back(PointerTy_1);
FunctionType* FuncTy_4 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_4_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_5_args;
FunctionType* FuncTy_5 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_5_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_6_args;
FuncTy_6_args.push_back(PointerTy_2);
FunctionType* FuncTy_6 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_6_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_7_args;
FunctionType* FuncTy_7 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 1),
 /*Params=*/FuncTy_7_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_8_args;
FuncTy_8_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_8_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_8 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_8_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_9_args;
FuncTy_9_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_9 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_9_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_10_args;
FuncTy_10_args.push_back(PointerTy_1);
FuncTy_10_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_10 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_10_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_11_args;
FuncTy_11_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_11_args.push_back(PointerTy_1);
FunctionType* FuncTy_11 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_11_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_12_args;
FuncTy_12_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_12_args.push_back(PointerTy_1);
FuncTy_12_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_12 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_12_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_13_args;
FuncTy_13_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_13_args.push_back(PointerTy_1);
FunctionType* FuncTy_13 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_13_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_14_args;
FuncTy_14_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_14_args.push_back(PointerTy_1);
FuncTy_14_args.push_back(PointerTy_1);
FunctionType* FuncTy_14 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_14_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_15_args;
FuncTy_15_args.push_back(PointerTy_1);
FuncTy_15_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_15 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_15_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_16_args;
FuncTy_16_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_16_args.push_back(PointerTy_1);
FuncTy_16_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_16 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_16_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_17_args;
FuncTy_17_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_17_args.push_back(PointerTy_1);
FuncTy_17_args.push_back(IntegerType::get(mod->getContext(), 64));
FunctionType* FuncTy_17 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_17_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_18_args;
FuncTy_18_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_18_args.push_back(PointerTy_1);
FuncTy_18_args.push_back(PointerTy_1);
FunctionType* FuncTy_18 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_18_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_19_args;
FuncTy_19_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_19_args.push_back(PointerTy_1);
FunctionType* FuncTy_19 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_19_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_20_args;
FuncTy_20_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_20_args.push_back(PointerTy_1);
FunctionType* FuncTy_20 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 64),
 /*Params=*/FuncTy_20_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_21_args;
FuncTy_21_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_21_args.push_back(PointerTy_1);
FuncTy_21_args.push_back(PointerTy_1);
FuncTy_21_args.push_back(IntegerType::get(mod->getContext(), 32));
FunctionType* FuncTy_21 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_21_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_22_args;
FuncTy_22_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_22_args.push_back(PointerTy_1);
FuncTy_22_args.push_back(PointerTy_1);
FuncTy_22_args.push_back(IntegerType::get(mod->getContext(), 64));
FunctionType* FuncTy_22 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_22_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_23_args;
FuncTy_23_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_23_args.push_back(PointerTy_1);
FuncTy_23_args.push_back(PointerTy_1);
FuncTy_23_args.push_back(PointerTy_1);
FunctionType* FuncTy_23 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_23_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_24_args;
FuncTy_24_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_24_args.push_back(PointerTy_1);
FuncTy_24_args.push_back(PointerTy_1);
FunctionType* FuncTy_24 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 64),
 /*Params=*/FuncTy_24_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_25_args;
FuncTy_25_args.push_back(PointerTy_1);
FuncTy_25_args.push_back(PointerTy_1);
FunctionType* FuncTy_25 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_25_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_26_args;
FuncTy_26_args.push_back(PointerTy_1);
FuncTy_26_args.push_back(PointerTy_1);
FunctionType* FuncTy_26 = FunctionType::get(
 /*Result=*/IntegerType::get(mod->getContext(), 32),
 /*Params=*/FuncTy_26_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_27_args;
FuncTy_27_args.push_back(PointerTy_1);
FuncTy_27_args.push_back(PointerTy_1);
FunctionType* FuncTy_27 = FunctionType::get(
 /*Result=*/Type::getVoidTy(mod->getContext()),
 /*Params=*/FuncTy_27_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_28_args;
FuncTy_28_args.push_back(PointerTy_1);
FuncTy_28_args.push_back(PointerTy_1);
FuncTy_28_args.push_back(IntegerType::get(mod->getContext(), 32));
FuncTy_28_args.push_back(IntegerType::get(mod->getContext(), 1));
FunctionType* FuncTy_28 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_28_args,
 /*isVarArg=*/false);

std::vector<Type*>FuncTy_29_args;
FuncTy_29_args.push_back(PointerTy_1);
FunctionType* FuncTy_29 = FunctionType::get(
 /*Result=*/PointerTy_1,
 /*Params=*/FuncTy_29_args,
 /*isVarArg=*/false);


// Function Declarations

Function* func___art_type_list = mod->getFunction("__art_type_list");
if (!func___art_type_list) {
func___art_type_list = Function::Create(
 /*Type=*/FuncTy_0,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"__art_type_list", mod); // (external, no body)
func___art_type_list->setCallingConv(CallingConv::C);
}
AttrListPtr func___art_type_list_PAL;
func___art_type_list->setAttributes(func___art_type_list_PAL);

Function* func_art_get_current_thread_from_code = mod->getFunction("art_get_current_thread_from_code");
if (!func_art_get_current_thread_from_code) {
func_art_get_current_thread_from_code = Function::Create(
 /*Type=*/FuncTy_3,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get_current_thread_from_code", mod); // (external, no body)
func_art_get_current_thread_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get_current_thread_from_code_PAL;
func_art_get_current_thread_from_code->setAttributes(func_art_get_current_thread_from_code_PAL);

Function* func_art_set_current_thread_from_code = mod->getFunction("art_set_current_thread_from_code");
if (!func_art_set_current_thread_from_code) {
func_art_set_current_thread_from_code = Function::Create(
 /*Type=*/FuncTy_4,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set_current_thread_from_code", mod); // (external, no body)
func_art_set_current_thread_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set_current_thread_from_code_PAL;
func_art_set_current_thread_from_code->setAttributes(func_art_set_current_thread_from_code_PAL);

Function* func_art_lock_object_from_code = mod->getFunction("art_lock_object_from_code");
if (!func_art_lock_object_from_code) {
func_art_lock_object_from_code = Function::Create(
 /*Type=*/FuncTy_4,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_lock_object_from_code", mod); // (external, no body)
func_art_lock_object_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_lock_object_from_code_PAL;
func_art_lock_object_from_code->setAttributes(func_art_lock_object_from_code_PAL);

Function* func_art_unlock_object_from_code = mod->getFunction("art_unlock_object_from_code");
if (!func_art_unlock_object_from_code) {
func_art_unlock_object_from_code = Function::Create(
 /*Type=*/FuncTy_4,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_unlock_object_from_code", mod); // (external, no body)
func_art_unlock_object_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_unlock_object_from_code_PAL;
func_art_unlock_object_from_code->setAttributes(func_art_unlock_object_from_code_PAL);

Function* func_art_test_suspend_from_code = mod->getFunction("art_test_suspend_from_code");
if (!func_art_test_suspend_from_code) {
func_art_test_suspend_from_code = Function::Create(
 /*Type=*/FuncTy_5,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_test_suspend_from_code", mod); // (external, no body)
func_art_test_suspend_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_test_suspend_from_code_PAL;
func_art_test_suspend_from_code->setAttributes(func_art_test_suspend_from_code_PAL);

Function* func_art_push_shadow_frame_from_code = mod->getFunction("art_push_shadow_frame_from_code");
if (!func_art_push_shadow_frame_from_code) {
func_art_push_shadow_frame_from_code = Function::Create(
 /*Type=*/FuncTy_6,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_push_shadow_frame_from_code", mod); // (external, no body)
func_art_push_shadow_frame_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_push_shadow_frame_from_code_PAL;
func_art_push_shadow_frame_from_code->setAttributes(func_art_push_shadow_frame_from_code_PAL);

Function* func_art_pop_shadow_frame_from_code = mod->getFunction("art_pop_shadow_frame_from_code");
if (!func_art_pop_shadow_frame_from_code) {
func_art_pop_shadow_frame_from_code = Function::Create(
 /*Type=*/FuncTy_5,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_pop_shadow_frame_from_code", mod); // (external, no body)
func_art_pop_shadow_frame_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_pop_shadow_frame_from_code_PAL;
func_art_pop_shadow_frame_from_code->setAttributes(func_art_pop_shadow_frame_from_code_PAL);

Function* func_art_is_exception_pending_from_code = mod->getFunction("art_is_exception_pending_from_code");
if (!func_art_is_exception_pending_from_code) {
func_art_is_exception_pending_from_code = Function::Create(
 /*Type=*/FuncTy_7,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_is_exception_pending_from_code", mod); // (external, no body)
func_art_is_exception_pending_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_is_exception_pending_from_code_PAL;
func_art_is_exception_pending_from_code->setAttributes(func_art_is_exception_pending_from_code_PAL);

Function* func_art_throw_div_zero_from_code = mod->getFunction("art_throw_div_zero_from_code");
if (!func_art_throw_div_zero_from_code) {
func_art_throw_div_zero_from_code = Function::Create(
 /*Type=*/FuncTy_5,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_div_zero_from_code", mod); // (external, no body)
func_art_throw_div_zero_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_div_zero_from_code_PAL;
func_art_throw_div_zero_from_code->setAttributes(func_art_throw_div_zero_from_code_PAL);

Function* func_art_throw_array_bounds_from_code = mod->getFunction("art_throw_array_bounds_from_code");
if (!func_art_throw_array_bounds_from_code) {
func_art_throw_array_bounds_from_code = Function::Create(
 /*Type=*/FuncTy_8,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_array_bounds_from_code", mod); // (external, no body)
func_art_throw_array_bounds_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_array_bounds_from_code_PAL;
func_art_throw_array_bounds_from_code->setAttributes(func_art_throw_array_bounds_from_code_PAL);

Function* func_art_throw_no_such_method_from_code = mod->getFunction("art_throw_no_such_method_from_code");
if (!func_art_throw_no_such_method_from_code) {
func_art_throw_no_such_method_from_code = Function::Create(
 /*Type=*/FuncTy_9,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_no_such_method_from_code", mod); // (external, no body)
func_art_throw_no_such_method_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_no_such_method_from_code_PAL;
func_art_throw_no_such_method_from_code->setAttributes(func_art_throw_no_such_method_from_code_PAL);

Function* func_art_throw_null_pointer_exception_from_code = mod->getFunction("art_throw_null_pointer_exception_from_code");
if (!func_art_throw_null_pointer_exception_from_code) {
func_art_throw_null_pointer_exception_from_code = Function::Create(
 /*Type=*/FuncTy_9,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_null_pointer_exception_from_code", mod); // (external, no body)
func_art_throw_null_pointer_exception_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_null_pointer_exception_from_code_PAL;
func_art_throw_null_pointer_exception_from_code->setAttributes(func_art_throw_null_pointer_exception_from_code_PAL);

Function* func_art_throw_stack_overflow_from_code = mod->getFunction("art_throw_stack_overflow_from_code");
if (!func_art_throw_stack_overflow_from_code) {
func_art_throw_stack_overflow_from_code = Function::Create(
 /*Type=*/FuncTy_5,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_stack_overflow_from_code", mod); // (external, no body)
func_art_throw_stack_overflow_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_stack_overflow_from_code_PAL;
func_art_throw_stack_overflow_from_code->setAttributes(func_art_throw_stack_overflow_from_code_PAL);

Function* func_art_throw_exception_from_code = mod->getFunction("art_throw_exception_from_code");
if (!func_art_throw_exception_from_code) {
func_art_throw_exception_from_code = Function::Create(
 /*Type=*/FuncTy_4,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_throw_exception_from_code", mod); // (external, no body)
func_art_throw_exception_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_throw_exception_from_code_PAL;
func_art_throw_exception_from_code->setAttributes(func_art_throw_exception_from_code_PAL);

Function* func_art_find_catch_block_from_code = mod->getFunction("art_find_catch_block_from_code");
if (!func_art_find_catch_block_from_code) {
func_art_find_catch_block_from_code = Function::Create(
 /*Type=*/FuncTy_10,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_catch_block_from_code", mod); // (external, no body)
func_art_find_catch_block_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_catch_block_from_code_PAL;
func_art_find_catch_block_from_code->setAttributes(func_art_find_catch_block_from_code_PAL);

Function* func_art_alloc_object_from_code = mod->getFunction("art_alloc_object_from_code");
if (!func_art_alloc_object_from_code) {
func_art_alloc_object_from_code = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_alloc_object_from_code", mod); // (external, no body)
func_art_alloc_object_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_alloc_object_from_code_PAL;
func_art_alloc_object_from_code->setAttributes(func_art_alloc_object_from_code_PAL);

Function* func_art_alloc_object_from_code_with_access_check = mod->getFunction("art_alloc_object_from_code_with_access_check");
if (!func_art_alloc_object_from_code_with_access_check) {
func_art_alloc_object_from_code_with_access_check = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_alloc_object_from_code_with_access_check", mod); // (external, no body)
func_art_alloc_object_from_code_with_access_check->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_alloc_object_from_code_with_access_check_PAL;
func_art_alloc_object_from_code_with_access_check->setAttributes(func_art_alloc_object_from_code_with_access_check_PAL);

Function* func_art_alloc_array_from_code = mod->getFunction("art_alloc_array_from_code");
if (!func_art_alloc_array_from_code) {
func_art_alloc_array_from_code = Function::Create(
 /*Type=*/FuncTy_12,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_alloc_array_from_code", mod); // (external, no body)
func_art_alloc_array_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_alloc_array_from_code_PAL;
func_art_alloc_array_from_code->setAttributes(func_art_alloc_array_from_code_PAL);

Function* func_art_alloc_array_from_code_with_access_check = mod->getFunction("art_alloc_array_from_code_with_access_check");
if (!func_art_alloc_array_from_code_with_access_check) {
func_art_alloc_array_from_code_with_access_check = Function::Create(
 /*Type=*/FuncTy_12,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_alloc_array_from_code_with_access_check", mod); // (external, no body)
func_art_alloc_array_from_code_with_access_check->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_alloc_array_from_code_with_access_check_PAL;
func_art_alloc_array_from_code_with_access_check->setAttributes(func_art_alloc_array_from_code_with_access_check_PAL);

Function* func_art_check_and_alloc_array_from_code = mod->getFunction("art_check_and_alloc_array_from_code");
if (!func_art_check_and_alloc_array_from_code) {
func_art_check_and_alloc_array_from_code = Function::Create(
 /*Type=*/FuncTy_12,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_check_and_alloc_array_from_code", mod); // (external, no body)
func_art_check_and_alloc_array_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_check_and_alloc_array_from_code_PAL;
func_art_check_and_alloc_array_from_code->setAttributes(func_art_check_and_alloc_array_from_code_PAL);

Function* func_art_check_and_alloc_array_from_code_with_access_check = mod->getFunction("art_check_and_alloc_array_from_code_with_access_check");
if (!func_art_check_and_alloc_array_from_code_with_access_check) {
func_art_check_and_alloc_array_from_code_with_access_check = Function::Create(
 /*Type=*/FuncTy_12,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_check_and_alloc_array_from_code_with_access_check", mod); // (external, no body)
func_art_check_and_alloc_array_from_code_with_access_check->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_check_and_alloc_array_from_code_with_access_check_PAL;
func_art_check_and_alloc_array_from_code_with_access_check->setAttributes(func_art_check_and_alloc_array_from_code_with_access_check_PAL);

Function* func_art_find_instance_field_from_code = mod->getFunction("art_find_instance_field_from_code");
if (!func_art_find_instance_field_from_code) {
func_art_find_instance_field_from_code = Function::Create(
 /*Type=*/FuncTy_13,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_instance_field_from_code", mod); // (external, no body)
func_art_find_instance_field_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_instance_field_from_code_PAL;
func_art_find_instance_field_from_code->setAttributes(func_art_find_instance_field_from_code_PAL);

Function* func_art_find_static_field_from_code = mod->getFunction("art_find_static_field_from_code");
if (!func_art_find_static_field_from_code) {
func_art_find_static_field_from_code = Function::Create(
 /*Type=*/FuncTy_13,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_static_field_from_code", mod); // (external, no body)
func_art_find_static_field_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_static_field_from_code_PAL;
func_art_find_static_field_from_code->setAttributes(func_art_find_static_field_from_code_PAL);

Function* func_art_find_interface_method_from_code = mod->getFunction("art_find_interface_method_from_code");
if (!func_art_find_interface_method_from_code) {
func_art_find_interface_method_from_code = Function::Create(
 /*Type=*/FuncTy_14,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_interface_method_from_code", mod); // (external, no body)
func_art_find_interface_method_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_interface_method_from_code_PAL;
func_art_find_interface_method_from_code->setAttributes(func_art_find_interface_method_from_code_PAL);

Function* func_art_find_virtual_method_from_code = mod->getFunction("art_find_virtual_method_from_code");
if (!func_art_find_virtual_method_from_code) {
func_art_find_virtual_method_from_code = Function::Create(
 /*Type=*/FuncTy_14,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_virtual_method_from_code", mod); // (external, no body)
func_art_find_virtual_method_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_virtual_method_from_code_PAL;
func_art_find_virtual_method_from_code->setAttributes(func_art_find_virtual_method_from_code_PAL);

Function* func_art_find_super_method_from_code = mod->getFunction("art_find_super_method_from_code");
if (!func_art_find_super_method_from_code) {
func_art_find_super_method_from_code = Function::Create(
 /*Type=*/FuncTy_14,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_find_super_method_from_code", mod); // (external, no body)
func_art_find_super_method_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_find_super_method_from_code_PAL;
func_art_find_super_method_from_code->setAttributes(func_art_find_super_method_from_code_PAL);

Function* func_art_initialize_static_storage_from_code = mod->getFunction("art_initialize_static_storage_from_code");
if (!func_art_initialize_static_storage_from_code) {
func_art_initialize_static_storage_from_code = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_initialize_static_storage_from_code", mod); // (external, no body)
func_art_initialize_static_storage_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_initialize_static_storage_from_code_PAL;
func_art_initialize_static_storage_from_code->setAttributes(func_art_initialize_static_storage_from_code_PAL);

Function* func_art_initialize_type_from_code = mod->getFunction("art_initialize_type_from_code");
if (!func_art_initialize_type_from_code) {
func_art_initialize_type_from_code = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_initialize_type_from_code", mod); // (external, no body)
func_art_initialize_type_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_initialize_type_from_code_PAL;
func_art_initialize_type_from_code->setAttributes(func_art_initialize_type_from_code_PAL);

Function* func_art_initialize_type_and_verify_access_from_code = mod->getFunction("art_initialize_type_and_verify_access_from_code");
if (!func_art_initialize_type_and_verify_access_from_code) {
func_art_initialize_type_and_verify_access_from_code = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_initialize_type_and_verify_access_from_code", mod); // (external, no body)
func_art_initialize_type_and_verify_access_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_initialize_type_and_verify_access_from_code_PAL;
func_art_initialize_type_and_verify_access_from_code->setAttributes(func_art_initialize_type_and_verify_access_from_code_PAL);

Function* func_art_resolve_string_from_code = mod->getFunction("art_resolve_string_from_code");
if (!func_art_resolve_string_from_code) {
func_art_resolve_string_from_code = Function::Create(
 /*Type=*/FuncTy_15,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_resolve_string_from_code", mod); // (external, no body)
func_art_resolve_string_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_resolve_string_from_code_PAL;
func_art_resolve_string_from_code->setAttributes(func_art_resolve_string_from_code_PAL);

Function* func_art_set32_static_from_code = mod->getFunction("art_set32_static_from_code");
if (!func_art_set32_static_from_code) {
func_art_set32_static_from_code = Function::Create(
 /*Type=*/FuncTy_16,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set32_static_from_code", mod); // (external, no body)
func_art_set32_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set32_static_from_code_PAL;
func_art_set32_static_from_code->setAttributes(func_art_set32_static_from_code_PAL);

Function* func_art_set64_static_from_code = mod->getFunction("art_set64_static_from_code");
if (!func_art_set64_static_from_code) {
func_art_set64_static_from_code = Function::Create(
 /*Type=*/FuncTy_17,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set64_static_from_code", mod); // (external, no body)
func_art_set64_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set64_static_from_code_PAL;
func_art_set64_static_from_code->setAttributes(func_art_set64_static_from_code_PAL);

Function* func_art_set_obj_static_from_code = mod->getFunction("art_set_obj_static_from_code");
if (!func_art_set_obj_static_from_code) {
func_art_set_obj_static_from_code = Function::Create(
 /*Type=*/FuncTy_18,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set_obj_static_from_code", mod); // (external, no body)
func_art_set_obj_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set_obj_static_from_code_PAL;
func_art_set_obj_static_from_code->setAttributes(func_art_set_obj_static_from_code_PAL);

Function* func_art_get32_static_from_code = mod->getFunction("art_get32_static_from_code");
if (!func_art_get32_static_from_code) {
func_art_get32_static_from_code = Function::Create(
 /*Type=*/FuncTy_19,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get32_static_from_code", mod); // (external, no body)
func_art_get32_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get32_static_from_code_PAL;
func_art_get32_static_from_code->setAttributes(func_art_get32_static_from_code_PAL);

Function* func_art_get64_static_from_code = mod->getFunction("art_get64_static_from_code");
if (!func_art_get64_static_from_code) {
func_art_get64_static_from_code = Function::Create(
 /*Type=*/FuncTy_20,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get64_static_from_code", mod); // (external, no body)
func_art_get64_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get64_static_from_code_PAL;
func_art_get64_static_from_code->setAttributes(func_art_get64_static_from_code_PAL);

Function* func_art_get_obj_static_from_code = mod->getFunction("art_get_obj_static_from_code");
if (!func_art_get_obj_static_from_code) {
func_art_get_obj_static_from_code = Function::Create(
 /*Type=*/FuncTy_11,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get_obj_static_from_code", mod); // (external, no body)
func_art_get_obj_static_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get_obj_static_from_code_PAL;
func_art_get_obj_static_from_code->setAttributes(func_art_get_obj_static_from_code_PAL);

Function* func_art_set32_instance_from_code = mod->getFunction("art_set32_instance_from_code");
if (!func_art_set32_instance_from_code) {
func_art_set32_instance_from_code = Function::Create(
 /*Type=*/FuncTy_21,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set32_instance_from_code", mod); // (external, no body)
func_art_set32_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set32_instance_from_code_PAL;
func_art_set32_instance_from_code->setAttributes(func_art_set32_instance_from_code_PAL);

Function* func_art_set64_instance_from_code = mod->getFunction("art_set64_instance_from_code");
if (!func_art_set64_instance_from_code) {
func_art_set64_instance_from_code = Function::Create(
 /*Type=*/FuncTy_22,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set64_instance_from_code", mod); // (external, no body)
func_art_set64_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set64_instance_from_code_PAL;
func_art_set64_instance_from_code->setAttributes(func_art_set64_instance_from_code_PAL);

Function* func_art_set_obj_instance_from_code = mod->getFunction("art_set_obj_instance_from_code");
if (!func_art_set_obj_instance_from_code) {
func_art_set_obj_instance_from_code = Function::Create(
 /*Type=*/FuncTy_23,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_set_obj_instance_from_code", mod); // (external, no body)
func_art_set_obj_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_set_obj_instance_from_code_PAL;
func_art_set_obj_instance_from_code->setAttributes(func_art_set_obj_instance_from_code_PAL);

Function* func_art_get32_instance_from_code = mod->getFunction("art_get32_instance_from_code");
if (!func_art_get32_instance_from_code) {
func_art_get32_instance_from_code = Function::Create(
 /*Type=*/FuncTy_18,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get32_instance_from_code", mod); // (external, no body)
func_art_get32_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get32_instance_from_code_PAL;
func_art_get32_instance_from_code->setAttributes(func_art_get32_instance_from_code_PAL);

Function* func_art_get64_instance_from_code = mod->getFunction("art_get64_instance_from_code");
if (!func_art_get64_instance_from_code) {
func_art_get64_instance_from_code = Function::Create(
 /*Type=*/FuncTy_24,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get64_instance_from_code", mod); // (external, no body)
func_art_get64_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get64_instance_from_code_PAL;
func_art_get64_instance_from_code->setAttributes(func_art_get64_instance_from_code_PAL);

Function* func_art_get_obj_instance_from_code = mod->getFunction("art_get_obj_instance_from_code");
if (!func_art_get_obj_instance_from_code) {
func_art_get_obj_instance_from_code = Function::Create(
 /*Type=*/FuncTy_14,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_get_obj_instance_from_code", mod); // (external, no body)
func_art_get_obj_instance_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_get_obj_instance_from_code_PAL;
func_art_get_obj_instance_from_code->setAttributes(func_art_get_obj_instance_from_code_PAL);

Function* func_art_decode_jobject_in_thread = mod->getFunction("art_decode_jobject_in_thread");
if (!func_art_decode_jobject_in_thread) {
func_art_decode_jobject_in_thread = Function::Create(
 /*Type=*/FuncTy_25,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_decode_jobject_in_thread", mod); // (external, no body)
func_art_decode_jobject_in_thread->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_decode_jobject_in_thread_PAL;
func_art_decode_jobject_in_thread->setAttributes(func_art_decode_jobject_in_thread_PAL);

Function* func_art_is_assignable_from_code = mod->getFunction("art_is_assignable_from_code");
if (!func_art_is_assignable_from_code) {
func_art_is_assignable_from_code = Function::Create(
 /*Type=*/FuncTy_26,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_is_assignable_from_code", mod); // (external, no body)
func_art_is_assignable_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_is_assignable_from_code_PAL;
func_art_is_assignable_from_code->setAttributes(func_art_is_assignable_from_code_PAL);

Function* func_art_check_cast_from_code = mod->getFunction("art_check_cast_from_code");
if (!func_art_check_cast_from_code) {
func_art_check_cast_from_code = Function::Create(
 /*Type=*/FuncTy_27,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_check_cast_from_code", mod); // (external, no body)
func_art_check_cast_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_check_cast_from_code_PAL;
func_art_check_cast_from_code->setAttributes(func_art_check_cast_from_code_PAL);

Function* func_art_check_put_array_element_from_code = mod->getFunction("art_check_put_array_element_from_code");
if (!func_art_check_put_array_element_from_code) {
func_art_check_put_array_element_from_code = Function::Create(
 /*Type=*/FuncTy_27,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_check_put_array_element_from_code", mod); // (external, no body)
func_art_check_put_array_element_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_check_put_array_element_from_code_PAL;
func_art_check_put_array_element_from_code->setAttributes(func_art_check_put_array_element_from_code_PAL);

Function* func_art_ensure_resolved_from_code = mod->getFunction("art_ensure_resolved_from_code");
if (!func_art_ensure_resolved_from_code) {
func_art_ensure_resolved_from_code = Function::Create(
 /*Type=*/FuncTy_28,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_ensure_resolved_from_code", mod); // (external, no body)
func_art_ensure_resolved_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_ensure_resolved_from_code_PAL;
func_art_ensure_resolved_from_code->setAttributes(func_art_ensure_resolved_from_code_PAL);

Function* func_art_fix_stub_from_code = mod->getFunction("art_fix_stub_from_code");
if (!func_art_fix_stub_from_code) {
func_art_fix_stub_from_code = Function::Create(
 /*Type=*/FuncTy_29,
 /*Linkage=*/GlobalValue::ExternalLinkage,
 /*Name=*/"art_fix_stub_from_code", mod); // (external, no body)
func_art_fix_stub_from_code->setCallingConv(CallingConv::C);
}
AttrListPtr func_art_fix_stub_from_code_PAL;
func_art_fix_stub_from_code->setAttributes(func_art_fix_stub_from_code_PAL);

// Global Variable Declarations


// Constant Definitions

// Global Variable Definitions

// Function Definitions

return mod;

}

} // namespace compiler_llvm
} // namespace art
