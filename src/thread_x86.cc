// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include <asm/ldt.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "macros.h"

namespace art {

void Thread::InitCpu() {
  // Read LDT
  CHECK_EQ((size_t)LDT_ENTRY_SIZE, sizeof(uint64_t));
  uint64_t ldt[LDT_ENTRIES];
  memset(ldt, 0, sizeof(ldt));
  syscall(SYS_modify_ldt, 0, ldt, sizeof(ldt));
  // Create empty slot to point at current Thread*
  user_desc ldt_entry;
  memset(&ldt_entry, 0, sizeof(ldt_entry));
  ldt_entry.entry_number = -1;
  ldt_entry.base_addr = (unsigned int)this;
  ldt_entry.limit = kPageSize;
  ldt_entry.seg_32bit = 1;
  ldt_entry.contents = MODIFY_LDT_CONTENTS_DATA;
  ldt_entry.read_exec_only = 0;
  ldt_entry.limit_in_pages = 0;
  ldt_entry.seg_not_present = 0;
  ldt_entry.useable = 1;
  for (int i = 0; i < LDT_ENTRIES; i++) {
    if (ldt[i] == 0) {
      ldt_entry.entry_number = i;
      break;
    }
  }
  if (ldt_entry.entry_number >= LDT_ENTRIES) {
    LOG(FATAL) << "Failed to find available LDT slot";
  }
  // Update LDT
  CHECK_EQ(0, syscall(SYS_modify_ldt, 1, &ldt_entry, sizeof(ldt_entry)));
  // Change FS to be new LDT entry
  uint16_t table_indicator = 1 << 2;  // LDT
  uint16_t rpl = 3;  // Requested privilege level
  uint16_t selector = (ldt_entry.entry_number << 3) | table_indicator | rpl;
  // TODO: use our assembler to generate code
  asm volatile("movw %w0, %%fs"
      :    // output
      : "q"(selector)  // input
      :);  // clobber
  // Allow easy indirection back to Thread*
  self_ = this;
  // Sanity check reads from FS goes to this Thread*
  Thread* self_check;
  // TODO: use our assembler to generate code
  asm volatile("movl %%fs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(OFFSETOF_MEMBER(Thread, self_))  // input
      :);  // clobber
  CHECK_EQ(self_check, this);
}

}  // namespace art
