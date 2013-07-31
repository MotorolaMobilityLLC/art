/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "sea_ir/types/type_inference.h"

namespace sea_ir {

bool TypeInference::IsPrimitiveDescriptor(char descriptor) {
  switch (descriptor) {
  case 'I':
  case 'C':
  case 'S':
  case 'B':
  case 'Z':
  case 'F':
  case 'D':
  case 'J':
    return true;
  default:
    return false;
  }
}

FunctionTypeInfo::FunctionTypeInfo(const SeaGraph* graph, art::verifier::RegTypeCache* types)
    : dex_file_(graph->GetDexFile()), dex_method_idx_(graph->method_idx_), type_cache_(types),
    method_access_flags_(graph->method_access_flags_) {
  const art::DexFile::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx_);
  const char* descriptor = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(method_id.class_idx_));
  declaring_class_ = &(type_cache_->FromDescriptor(NULL, descriptor, false));
}

std::vector<const Type*> FunctionTypeInfo::GetDeclaredArgumentTypes()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::vector<const Type*> argument_types;
  // Include the "this" pointer.
  size_t cur_arg = 0;
  if (!IsStatic()) {
    // If this is a constructor for a class other than java.lang.Object, mark the first ("this")
    // argument as uninitialized. This restricts field access until the superclass constructor is
    // called.
    const art::verifier::RegType& declaring_class = GetDeclaringClass();
    if (IsConstructor() && !declaring_class.IsJavaLangObject()) {
      argument_types.push_back(&(type_cache_->UninitializedThisArgument(declaring_class)));
    } else {
      argument_types.push_back(&declaring_class);
    }
    cur_arg++;
  }

  const art::DexFile::ProtoId& proto_id =
      dex_file_->GetMethodPrototype(dex_file_->GetMethodId(dex_method_idx_));
  art::DexFileParameterIterator iterator(*dex_file_, proto_id);

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();
    if (descriptor == NULL) {
      LOG(FATAL) << "Error: Encountered null type descriptor for function argument.";
    }
    switch (descriptor[0]) {
      case 'L':
      case '[':
        // We assume that reference arguments are initialized. The only way it could be otherwise
        // (assuming the caller was verified) is if the current method is <init>, but in that case
        // it's effectively considered initialized the instant we reach here (in the sense that we
        // can return without doing anything or call virtual methods).
        {
          const Type& reg_type = type_cache_->FromDescriptor(NULL, descriptor, false);
          argument_types.push_back(&reg_type);
        }
        break;
      case 'Z':
        argument_types.push_back(&type_cache_->Boolean());
        break;
      case 'C':
        argument_types.push_back(&type_cache_->Char());
        break;
      case 'B':
        argument_types.push_back(&type_cache_->Byte());
        break;
      case 'I':
        argument_types.push_back(&type_cache_->Integer());
        break;
      case 'S':
        argument_types.push_back(&type_cache_->Short());
        break;
      case 'F':
        argument_types.push_back(&type_cache_->Float());
        break;
      case 'J':
      case 'D': {
        // TODO: Figure out strategy for two-register operands (double, long)
        LOG(FATAL) << "Error: Type inference for 64-bit variables has not been implemented.";
        break;
      }
      default:
        LOG(FATAL) << "Error: Unexpected signature encountered during type inference.";
    }
    cur_arg++;
  }
  return argument_types;
}

// TODO: Lock is only used for dumping types (during development). Remove this for performance.
void TypeInference::ComputeTypes(SeaGraph* graph) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::vector<Region*>* regions = graph->GetRegions();
  std::list<InstructionNode*> worklist;
  // Fill the work-list with all instructions.
  for (std::vector<Region*>::const_iterator region_it = regions->begin();
      region_it != regions->end(); region_it++) {
    std::vector<PhiInstructionNode*>* phi_instructions = (*region_it)->GetPhiNodes();
    std::copy(phi_instructions->begin(), phi_instructions->end(), std::back_inserter(worklist));
    std::vector<InstructionNode*>* instructions = (*region_it)->GetInstructions();
    std::copy(instructions->begin(), instructions->end(), std::back_inserter(worklist));
  }
  TypeInferenceVisitor tiv(graph, type_cache_);
  // Sparse (SSA) fixed-point algorithm that processes each instruction in the work-list,
  // adding consumers of instructions whose result changed type back into the work-list.
  // Note: According to [1] list iterators should not be invalidated on insertion,
  //       which simplifies the implementation; not 100% sure other STL implementations
  //       maintain this invariant, but they should.
  //       [1] http://www.sgi.com/tech/stl/List.html
  // TODO: Making this conditional (as in sparse conditional constant propagation) would be good.
  for (std::list<InstructionNode*>::const_iterator instruction_it = worklist.begin();
        instruction_it != worklist.end(); instruction_it++) {
    std::cout << "Instruction: " << (*instruction_it)->Id() << std::endl;
    (*instruction_it)->Accept(&tiv);
    std::map<int, const Type*>::const_iterator old_result_it =
        type_map_.find((*instruction_it)->Id());
    const Type* new_type = tiv.GetType();
    bool first_time_set = (old_result_it == type_map_.end()) && (new_type != NULL);
    bool type_changed = (old_result_it != type_map_.end()) && ((*old_result_it).second != new_type);
    if (first_time_set || type_changed) {
      std::cout << " New type:" << new_type->IsIntegralTypes() << std::endl;
      std::cout << " Descriptor:" << new_type->Dump() << std::endl;
      type_map_[(*instruction_it)->Id()] = new_type;
      // Add SSA consumers of the current instruction to the work-list.
      std::vector<InstructionNode*> consumers = (*instruction_it)->GetSSAConsumers();
      for (std::vector<InstructionNode*>::iterator consumer = consumers.begin();
          consumer != consumers.end(); consumer++) {
        worklist.push_back(*consumer);
      }
    }
  }
}

}   // namespace sea_ir
