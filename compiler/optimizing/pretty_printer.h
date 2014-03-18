/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_
#define ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_

#include "nodes.h"

namespace art {

class HPrettyPrinter : public HGraphVisitor {
 public:
  explicit HPrettyPrinter(HGraph* graph) : HGraphVisitor(graph) { }

  virtual void VisitInstruction(HInstruction* instruction) {
    PrintString("  ");
    PrintInt(instruction->GetId());
    PrintString(": ");
    PrintString(instruction->DebugName());
    if (instruction->InputCount() != 0) {
      PrintString("(");
      bool first = true;
      for (HInputIterator it(instruction); !it.Done(); it.Advance()) {
        if (first) {
          first = false;
        } else {
          PrintString(", ");
        }
        PrintInt(it.Current()->GetId());
      }
      PrintString(")");
    }
    if (instruction->HasUses()) {
      PrintString(" [");
      bool first = true;
      for (HUseIterator it(instruction); !it.Done(); it.Advance()) {
        if (first) {
          first = false;
        } else {
          PrintString(", ");
        }
        PrintInt(it.Current()->GetId());
      }
      PrintString("]");
    }
    PrintNewLine();
  }

  virtual void VisitBasicBlock(HBasicBlock* block) {
    PrintString("BasicBlock ");
    PrintInt(block->GetBlockId());
    const GrowableArray<HBasicBlock*>* blocks = block->GetPredecessors();
    if (!blocks->IsEmpty()) {
      PrintString(", pred: ");
      for (size_t i = 0; i < blocks->Size() -1; i++) {
        PrintInt(blocks->Get(i)->GetBlockId());
        PrintString(", ");
      }
      PrintInt(blocks->Peek()->GetBlockId());
    }
    blocks = block->GetSuccessors();
    if (!blocks->IsEmpty()) {
      PrintString(", succ: ");
      for (size_t i = 0; i < blocks->Size() - 1; i++) {
        PrintInt(blocks->Get(i)->GetBlockId());
        PrintString(", ");
      }
      PrintInt(blocks->Peek()->GetBlockId());
    }
    PrintNewLine();
    HGraphVisitor::VisitBasicBlock(block);
  }

  virtual void PrintNewLine() = 0;
  virtual void PrintInt(int value) = 0;
  virtual void PrintString(const char* value) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(HPrettyPrinter);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_
