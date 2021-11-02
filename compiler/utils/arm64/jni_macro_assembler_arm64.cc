/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "jni_macro_assembler_arm64.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "managed_register_arm64.h"
#include "offsets.h"
#include "thread.h"

using namespace vixl::aarch64;  // NOLINT(build/namespaces)

namespace art {
namespace arm64 {

#ifdef ___
#error "ARM64 Assembler macro already defined."
#else
#define ___   asm_.GetVIXLAssembler()->
#endif

#define reg_x(X) Arm64Assembler::reg_x(X)
#define reg_w(W) Arm64Assembler::reg_w(W)
#define reg_d(D) Arm64Assembler::reg_d(D)
#define reg_s(S) Arm64Assembler::reg_s(S)

// The AAPCS64 requires 16-byte alignment. This is the same as the Managed ABI stack alignment.
static constexpr size_t kAapcs64StackAlignment = 16u;
static_assert(kAapcs64StackAlignment == kStackAlignment);

Arm64JNIMacroAssembler::~Arm64JNIMacroAssembler() {
}

void Arm64JNIMacroAssembler::FinalizeCode() {
  ___ FinalizeCode();
}

void Arm64JNIMacroAssembler::GetCurrentThread(ManagedRegister dest) {
  ___ Mov(reg_x(dest.AsArm64().AsXRegister()), reg_x(TR));
}

void Arm64JNIMacroAssembler::GetCurrentThread(FrameOffset offset) {
  StoreToOffset(TR, SP, offset.Int32Value());
}

// See Arm64 PCS Section 5.2.2.1.
void Arm64JNIMacroAssembler::IncreaseFrameSize(size_t adjust) {
  if (adjust != 0u) {
    CHECK_ALIGNED(adjust, kStackAlignment);
    AddConstant(SP, -adjust);
    cfi().AdjustCFAOffset(adjust);
  }
}

// See Arm64 PCS Section 5.2.2.1.
void Arm64JNIMacroAssembler::DecreaseFrameSize(size_t adjust) {
  if (adjust != 0u) {
    CHECK_ALIGNED(adjust, kStackAlignment);
    AddConstant(SP, adjust);
    cfi().AdjustCFAOffset(-adjust);
  }
}

ManagedRegister Arm64JNIMacroAssembler::CoreRegisterWithSize(ManagedRegister m_src, size_t size) {
  DCHECK(size == 4u || size == 8u) << size;
  Arm64ManagedRegister src = m_src.AsArm64();
  // Switch between X and W registers using the `XRegister` and `WRegister` enumerations.
  static_assert(W0 == static_cast<std::underlying_type_t<XRegister>>(X0));
  static_assert(W30 == static_cast<std::underlying_type_t<XRegister>>(X30));
  static_assert(WSP == static_cast<std::underlying_type_t<XRegister>>(SP));
  static_assert(WZR == static_cast<std::underlying_type_t<XRegister>>(XZR));
  if (src.IsXRegister()) {
    if (size == 8u) {
      return m_src;
    }
    auto id = static_cast<std::underlying_type_t<XRegister>>(src.AsXRegister());
    return Arm64ManagedRegister::FromWRegister(enum_cast<WRegister>(id));
  } else {
    CHECK(src.IsWRegister());
    if (size == 4u) {
      return m_src;
    }
    auto id = static_cast<std::underlying_type_t<WRegister>>(src.AsWRegister());
    return Arm64ManagedRegister::FromXRegister(enum_cast<XRegister>(id));
  }
}

void Arm64JNIMacroAssembler::AddConstant(XRegister rd, int32_t value, Condition cond) {
  AddConstant(rd, rd, value, cond);
}

void Arm64JNIMacroAssembler::AddConstant(XRegister rd,
                                         XRegister rn,
                                         int32_t value,
                                         Condition cond) {
  if ((cond == al) || (cond == nv)) {
    // VIXL macro-assembler handles all variants.
    ___ Add(reg_x(rd), reg_x(rn), value);
  } else {
    // temp = rd + value
    // rd = cond ? temp : rn
    UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
    temps.Exclude(reg_x(rd), reg_x(rn));
    Register temp = temps.AcquireX();
    ___ Add(temp, reg_x(rn), value);
    ___ Csel(reg_x(rd), temp, reg_x(rd), cond);
  }
}

void Arm64JNIMacroAssembler::StoreWToOffset(StoreOperandType type,
                                            WRegister source,
                                            XRegister base,
                                            int32_t offset) {
  switch (type) {
    case kStoreByte:
      ___ Strb(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    case kStoreHalfword:
      ___ Strh(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    case kStoreWord:
      ___ Str(reg_w(source), MEM_OP(reg_x(base), offset));
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Arm64JNIMacroAssembler::StoreToOffset(XRegister source, XRegister base, int32_t offset) {
  CHECK_NE(source, SP);
  ___ Str(reg_x(source), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::StoreSToOffset(SRegister source, XRegister base, int32_t offset) {
  ___ Str(reg_s(source), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::StoreDToOffset(DRegister source, XRegister base, int32_t offset) {
  ___ Str(reg_d(source), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::Store(FrameOffset offs, ManagedRegister m_src, size_t size) {
  Store(Arm64ManagedRegister::FromXRegister(SP), MemberOffset(offs.Int32Value()), m_src, size);
}

void Arm64JNIMacroAssembler::Store(ManagedRegister m_base,
                                   MemberOffset offs,
                                   ManagedRegister m_src,
                                   size_t size) {
  Arm64ManagedRegister base = m_base.AsArm64();
  Arm64ManagedRegister src = m_src.AsArm64();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsWRegister()) {
    CHECK_EQ(4u, size);
    StoreWToOffset(kStoreWord, src.AsWRegister(), base.AsXRegister(), offs.Int32Value());
  } else if (src.IsXRegister()) {
    CHECK_EQ(8u, size);
    StoreToOffset(src.AsXRegister(), base.AsXRegister(), offs.Int32Value());
  } else if (src.IsSRegister()) {
    StoreSToOffset(src.AsSRegister(), base.AsXRegister(), offs.Int32Value());
  } else {
    CHECK(src.IsDRegister()) << src;
    StoreDToOffset(src.AsDRegister(), base.AsXRegister(), offs.Int32Value());
  }
}

void Arm64JNIMacroAssembler::StoreRef(FrameOffset offs, ManagedRegister m_src) {
  Arm64ManagedRegister src = m_src.AsArm64();
  CHECK(src.IsXRegister()) << src;
  StoreWToOffset(kStoreWord, src.AsOverlappingWRegister(), SP,
                 offs.Int32Value());
}

void Arm64JNIMacroAssembler::StoreRawPtr(FrameOffset offs, ManagedRegister m_src) {
  Arm64ManagedRegister src = m_src.AsArm64();
  CHECK(src.IsXRegister()) << src;
  StoreToOffset(src.AsXRegister(), SP, offs.Int32Value());
}

void Arm64JNIMacroAssembler::StoreImmediateToFrame(FrameOffset offs, uint32_t imm) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireW();
  ___ Mov(scratch, imm);
  ___ Str(scratch, MEM_OP(reg_x(SP), offs.Int32Value()));
}

void Arm64JNIMacroAssembler::StoreStackOffsetToThread(ThreadOffset64 tr_offs, FrameOffset fr_offs) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  ___ Add(scratch, reg_x(SP), fr_offs.Int32Value());
  ___ Str(scratch, MEM_OP(reg_x(TR), tr_offs.Int32Value()));
}

void Arm64JNIMacroAssembler::StoreStackPointerToThread(ThreadOffset64 tr_offs) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  ___ Mov(scratch, reg_x(SP));
  ___ Str(scratch, MEM_OP(reg_x(TR), tr_offs.Int32Value()));
}

void Arm64JNIMacroAssembler::StoreSpanning(FrameOffset dest_off ATTRIBUTE_UNUSED,
                                           ManagedRegister m_source ATTRIBUTE_UNUSED,
                                           FrameOffset in_off ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);  // This case is not applicable to ARM64.
}

// Load routines.
void Arm64JNIMacroAssembler::LoadImmediate(XRegister dest, int32_t value, Condition cond) {
  if ((cond == al) || (cond == nv)) {
    ___ Mov(reg_x(dest), value);
  } else {
    // temp = value
    // rd = cond ? temp : rd
    if (value != 0) {
      UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
      temps.Exclude(reg_x(dest));
      Register temp = temps.AcquireX();
      ___ Mov(temp, value);
      ___ Csel(reg_x(dest), temp, reg_x(dest), cond);
    } else {
      ___ Csel(reg_x(dest), reg_x(XZR), reg_x(dest), cond);
    }
  }
}

void Arm64JNIMacroAssembler::LoadWFromOffset(LoadOperandType type,
                                             WRegister dest,
                                             XRegister base,
                                             int32_t offset) {
  switch (type) {
    case kLoadSignedByte:
      ___ Ldrsb(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadSignedHalfword:
      ___ Ldrsh(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadUnsignedByte:
      ___ Ldrb(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadUnsignedHalfword:
      ___ Ldrh(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    case kLoadWord:
      ___ Ldr(reg_w(dest), MEM_OP(reg_x(base), offset));
      break;
    default:
        LOG(FATAL) << "UNREACHABLE";
  }
}

// Note: We can extend this member by adding load type info - see
// sign extended A64 load variants.
void Arm64JNIMacroAssembler::LoadFromOffset(XRegister dest, XRegister base, int32_t offset) {
  CHECK_NE(dest, SP);
  ___ Ldr(reg_x(dest), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::LoadSFromOffset(SRegister dest, XRegister base, int32_t offset) {
  ___ Ldr(reg_s(dest), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::LoadDFromOffset(DRegister dest, XRegister base, int32_t offset) {
  ___ Ldr(reg_d(dest), MEM_OP(reg_x(base), offset));
}

void Arm64JNIMacroAssembler::Load(Arm64ManagedRegister dest,
                                  XRegister base,
                                  int32_t offset,
                                  size_t size) {
  if (dest.IsNoRegister()) {
    CHECK_EQ(0u, size) << dest;
  } else if (dest.IsWRegister()) {
    CHECK_EQ(4u, size) << dest;
    ___ Ldr(reg_w(dest.AsWRegister()), MEM_OP(reg_x(base), offset));
  } else if (dest.IsXRegister()) {
    CHECK_NE(dest.AsXRegister(), SP) << dest;

    if (size == 1u) {
      ___ Ldrb(reg_w(dest.AsOverlappingWRegister()), MEM_OP(reg_x(base), offset));
    } else if (size == 4u) {
      ___ Ldr(reg_w(dest.AsOverlappingWRegister()), MEM_OP(reg_x(base), offset));
    }  else {
      CHECK_EQ(8u, size) << dest;
      ___ Ldr(reg_x(dest.AsXRegister()), MEM_OP(reg_x(base), offset));
    }
  } else if (dest.IsSRegister()) {
    ___ Ldr(reg_s(dest.AsSRegister()), MEM_OP(reg_x(base), offset));
  } else {
    CHECK(dest.IsDRegister()) << dest;
    ___ Ldr(reg_d(dest.AsDRegister()), MEM_OP(reg_x(base), offset));
  }
}

void Arm64JNIMacroAssembler::Load(ManagedRegister m_dst, FrameOffset src, size_t size) {
  return Load(m_dst.AsArm64(), SP, src.Int32Value(), size);
}

void Arm64JNIMacroAssembler::Load(ManagedRegister m_dst,
                                  ManagedRegister m_base,
                                  MemberOffset offs,
                                  size_t size) {
  return Load(m_dst.AsArm64(), m_base.AsArm64().AsXRegister(), offs.Int32Value(), size);
}

void Arm64JNIMacroAssembler::LoadFromThread(ManagedRegister m_dst,
                                            ThreadOffset64 src,
                                            size_t size) {
  return Load(m_dst.AsArm64(), TR, src.Int32Value(), size);
}

void Arm64JNIMacroAssembler::LoadRef(ManagedRegister m_dst, FrameOffset offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  CHECK(dst.IsXRegister()) << dst;
  LoadWFromOffset(kLoadWord, dst.AsOverlappingWRegister(), SP, offs.Int32Value());
}

void Arm64JNIMacroAssembler::LoadRef(ManagedRegister m_dst,
                                     ManagedRegister m_base,
                                     MemberOffset offs,
                                     bool unpoison_reference) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(dst.IsXRegister() && base.IsXRegister());
  LoadWFromOffset(kLoadWord, dst.AsOverlappingWRegister(), base.AsXRegister(),
                  offs.Int32Value());
  if (unpoison_reference) {
    WRegister ref_reg = dst.AsOverlappingWRegister();
    asm_.MaybeUnpoisonHeapReference(reg_w(ref_reg));
  }
}

void Arm64JNIMacroAssembler::LoadRawPtr(ManagedRegister m_dst,
                                        ManagedRegister m_base,
                                        Offset offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(dst.IsXRegister() && base.IsXRegister());
  // Remove dst and base form the temp list - higher level API uses IP1, IP0.
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  temps.Exclude(reg_x(dst.AsXRegister()), reg_x(base.AsXRegister()));
  ___ Ldr(reg_x(dst.AsXRegister()), MEM_OP(reg_x(base.AsXRegister()), offs.Int32Value()));
}

void Arm64JNIMacroAssembler::LoadRawPtrFromThread(ManagedRegister m_dst, ThreadOffset64 offs) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  CHECK(dst.IsXRegister()) << dst;
  LoadFromOffset(dst.AsXRegister(), TR, offs.Int32Value());
}

// Copying routines.
void Arm64JNIMacroAssembler::MoveArguments(ArrayRef<ArgumentLocation> dests,
                                           ArrayRef<ArgumentLocation> srcs) {
  DCHECK_EQ(dests.size(), srcs.size());
  auto get_mask = [](ManagedRegister reg) -> uint64_t {
    Arm64ManagedRegister arm64_reg = reg.AsArm64();
    if (arm64_reg.IsXRegister()) {
      size_t core_reg_number = static_cast<size_t>(arm64_reg.AsXRegister());
      DCHECK_LT(core_reg_number, 31u);  // xSP, xZR not allowed.
      return UINT64_C(1) << core_reg_number;
    } else if (arm64_reg.IsWRegister()) {
      size_t core_reg_number = static_cast<size_t>(arm64_reg.AsWRegister());
      DCHECK_LT(core_reg_number, 31u);  // wSP, wZR not allowed.
      return UINT64_C(1) << core_reg_number;
    } else if (arm64_reg.IsDRegister()) {
      size_t fp_reg_number = static_cast<size_t>(arm64_reg.AsDRegister());
      DCHECK_LT(fp_reg_number, 32u);
      return (UINT64_C(1) << 32u) << fp_reg_number;
    } else {
      DCHECK(arm64_reg.IsSRegister());
      size_t fp_reg_number = static_cast<size_t>(arm64_reg.AsSRegister());
      DCHECK_LT(fp_reg_number, 32u);
      return (UINT64_C(1) << 32u) << fp_reg_number;
    }
  };
  // Collect registers to move while storing/copying args to stack slots.
  // More than 8 core or FP reg args are very rare, so we do not optimize
  // for that case by using LDP/STP.
  // TODO: LDP/STP will be useful for normal and @FastNative where we need
  // to spill even the leading arguments.
  uint64_t src_regs = 0u;
  uint64_t dest_regs = 0u;
  for (size_t i = 0, arg_count = srcs.size(); i != arg_count; ++i) {
    const ArgumentLocation& src = srcs[i];
    const ArgumentLocation& dest = dests[i];
    DCHECK_EQ(src.GetSize(), dest.GetSize());
    if (dest.IsRegister()) {
      if (src.IsRegister() && src.GetRegister().Equals(dest.GetRegister())) {
        // Nothing to do.
      } else {
        if (src.IsRegister()) {
          src_regs |= get_mask(src.GetRegister());
        }
        dest_regs |= get_mask(dest.GetRegister());
      }
    } else {
      if (src.IsRegister()) {
        Store(dest.GetFrameOffset(), src.GetRegister(), dest.GetSize());
      } else {
        Copy(dest.GetFrameOffset(), src.GetFrameOffset(), dest.GetSize());
      }
    }
  }
  // Fill destination registers.
  // There should be no cycles, so this simple algorithm should make progress.
  while (dest_regs != 0u) {
    uint64_t old_dest_regs = dest_regs;
    for (size_t i = 0, arg_count = srcs.size(); i != arg_count; ++i) {
      const ArgumentLocation& src = srcs[i];
      const ArgumentLocation& dest = dests[i];
      if (!dest.IsRegister()) {
        continue;  // Stored in first loop above.
      }
      uint64_t dest_reg_mask = get_mask(dest.GetRegister());
      if ((dest_reg_mask & dest_regs) == 0u) {
        continue;  // Equals source, or already filled in one of previous iterations.
      }
      if ((dest_reg_mask & src_regs) != 0u) {
        continue;  // Cannot clobber this register yet.
      }
      if (src.IsRegister()) {
        Move(dest.GetRegister(), src.GetRegister(), dest.GetSize());
        src_regs &= ~get_mask(src.GetRegister());  // Allow clobbering source register.
      } else {
        Load(dest.GetRegister(), src.GetFrameOffset(), dest.GetSize());
      }
      dest_regs &= ~get_mask(dest.GetRegister());  // Destination register was filled.
    }
    CHECK_NE(old_dest_regs, dest_regs);
    DCHECK_EQ(0u, dest_regs & ~old_dest_regs);
  }
}

void Arm64JNIMacroAssembler::Move(ManagedRegister m_dst, ManagedRegister m_src, size_t size) {
  Arm64ManagedRegister dst = m_dst.AsArm64();
  if (kIsDebugBuild) {
    // Check that the destination is not a scratch register.
    UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
    if (dst.IsXRegister()) {
      CHECK(!temps.IsAvailable(reg_x(dst.AsXRegister())));
    } else if (dst.IsWRegister()) {
      CHECK(!temps.IsAvailable(reg_w(dst.AsWRegister())));
    } else if (dst.IsSRegister()) {
      CHECK(!temps.IsAvailable(reg_s(dst.AsSRegister())));
    } else {
      CHECK(!temps.IsAvailable(reg_d(dst.AsDRegister())));
    }
  }
  Arm64ManagedRegister src = m_src.AsArm64();
  if (!dst.Equals(src)) {
    if (dst.IsXRegister()) {
      if (size == 4) {
        CHECK(src.IsWRegister());
        ___ Mov(reg_w(dst.AsOverlappingWRegister()), reg_w(src.AsWRegister()));
      } else {
        if (src.IsXRegister()) {
          ___ Mov(reg_x(dst.AsXRegister()), reg_x(src.AsXRegister()));
        } else {
          ___ Mov(reg_x(dst.AsXRegister()), reg_x(src.AsOverlappingXRegister()));
        }
      }
    } else if (dst.IsWRegister()) {
      CHECK(src.IsWRegister()) << src;
      ___ Mov(reg_w(dst.AsWRegister()), reg_w(src.AsWRegister()));
    } else if (dst.IsSRegister()) {
      CHECK(src.IsSRegister()) << src;
      ___ Fmov(reg_s(dst.AsSRegister()), reg_s(src.AsSRegister()));
    } else {
      CHECK(dst.IsDRegister()) << dst;
      CHECK(src.IsDRegister()) << src;
      ___ Fmov(reg_d(dst.AsDRegister()), reg_d(src.AsDRegister()));
    }
  }
}

void Arm64JNIMacroAssembler::CopyRawPtrFromThread(FrameOffset fr_offs, ThreadOffset64 tr_offs) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  ___ Ldr(scratch, MEM_OP(reg_x(TR), tr_offs.Int32Value()));
  ___ Str(scratch, MEM_OP(sp, fr_offs.Int32Value()));
}

void Arm64JNIMacroAssembler::CopyRawPtrToThread(ThreadOffset64 tr_offs,
                                                FrameOffset fr_offs,
                                                ManagedRegister m_scratch) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  CHECK(scratch.IsXRegister()) << scratch;
  LoadFromOffset(scratch.AsXRegister(), SP, fr_offs.Int32Value());
  StoreToOffset(scratch.AsXRegister(), TR, tr_offs.Int32Value());
}

void Arm64JNIMacroAssembler::CopyRef(FrameOffset dest, FrameOffset src) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireW();
  ___ Ldr(scratch, MEM_OP(reg_x(SP), src.Int32Value()));
  ___ Str(scratch, MEM_OP(reg_x(SP), dest.Int32Value()));
}

void Arm64JNIMacroAssembler::CopyRef(FrameOffset dest,
                                     ManagedRegister base,
                                     MemberOffset offs,
                                     bool unpoison_reference) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireW();
  ___ Ldr(scratch, MEM_OP(reg_x(base.AsArm64().AsXRegister()), offs.Int32Value()));
  if (unpoison_reference) {
    asm_.MaybeUnpoisonHeapReference(scratch);
  }
  ___ Str(scratch, MEM_OP(reg_x(SP), dest.Int32Value()));
}

void Arm64JNIMacroAssembler::Copy(FrameOffset dest, FrameOffset src, size_t size) {
  DCHECK(size == 4 || size == 8) << size;
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = (size == 8) ? temps.AcquireX() : temps.AcquireW();
  ___ Ldr(scratch, MEM_OP(reg_x(SP), src.Int32Value()));
  ___ Str(scratch, MEM_OP(reg_x(SP), dest.Int32Value()));
}

void Arm64JNIMacroAssembler::Copy(FrameOffset dest,
                                  ManagedRegister src_base,
                                  Offset src_offset,
                                  ManagedRegister m_scratch,
                                  size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister base = src_base.AsArm64();
  CHECK(base.IsXRegister()) << base;
  CHECK(scratch.IsXRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadWFromOffset(kLoadWord, scratch.AsWRegister(), base.AsXRegister(),
                   src_offset.Int32Value());
    StoreWToOffset(kStoreWord, scratch.AsWRegister(), SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(scratch.AsXRegister(), base.AsXRegister(), src_offset.Int32Value());
    StoreToOffset(scratch.AsXRegister(), SP, dest.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64JNIMacroAssembler::Copy(ManagedRegister m_dest_base,
                                  Offset dest_offs,
                                  FrameOffset src,
                                  ManagedRegister m_scratch,
                                  size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister base = m_dest_base.AsArm64();
  CHECK(base.IsXRegister()) << base;
  CHECK(scratch.IsXRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadWFromOffset(kLoadWord, scratch.AsWRegister(), SP, src.Int32Value());
    StoreWToOffset(kStoreWord, scratch.AsWRegister(), base.AsXRegister(),
                   dest_offs.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(scratch.AsXRegister(), SP, src.Int32Value());
    StoreToOffset(scratch.AsXRegister(), base.AsXRegister(), dest_offs.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64JNIMacroAssembler::Copy(FrameOffset /*dst*/,
                                  FrameOffset /*src_base*/,
                                  Offset /*src_offset*/,
                                  ManagedRegister /*mscratch*/,
                                  size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "Unimplemented Copy() variant";
}

void Arm64JNIMacroAssembler::Copy(ManagedRegister m_dest,
                                  Offset dest_offset,
                                  ManagedRegister m_src,
                                  Offset src_offset,
                                  ManagedRegister m_scratch,
                                  size_t size) {
  Arm64ManagedRegister scratch = m_scratch.AsArm64();
  Arm64ManagedRegister src = m_src.AsArm64();
  Arm64ManagedRegister dest = m_dest.AsArm64();
  CHECK(dest.IsXRegister()) << dest;
  CHECK(src.IsXRegister()) << src;
  CHECK(scratch.IsXRegister() || scratch.IsWRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    if (scratch.IsWRegister()) {
      LoadWFromOffset(kLoadWord, scratch.AsWRegister(), src.AsXRegister(),
                    src_offset.Int32Value());
      StoreWToOffset(kStoreWord, scratch.AsWRegister(), dest.AsXRegister(),
                   dest_offset.Int32Value());
    } else {
      LoadWFromOffset(kLoadWord, scratch.AsOverlappingWRegister(), src.AsXRegister(),
                    src_offset.Int32Value());
      StoreWToOffset(kStoreWord, scratch.AsOverlappingWRegister(), dest.AsXRegister(),
                   dest_offset.Int32Value());
    }
  } else if (size == 8) {
    LoadFromOffset(scratch.AsXRegister(), src.AsXRegister(), src_offset.Int32Value());
    StoreToOffset(scratch.AsXRegister(), dest.AsXRegister(), dest_offset.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Arm64JNIMacroAssembler::Copy(FrameOffset /*dst*/,
                                  Offset /*dest_offset*/,
                                  FrameOffset /*src*/,
                                  Offset /*src_offset*/,
                                  ManagedRegister /*scratch*/,
                                  size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "Unimplemented Copy() variant";
}

void Arm64JNIMacroAssembler::MemoryBarrier(ManagedRegister m_scratch ATTRIBUTE_UNUSED) {
  // TODO: Should we check that m_scratch is IP? - see arm.
  ___ Dmb(InnerShareable, BarrierAll);
}

void Arm64JNIMacroAssembler::SignExtend(ManagedRegister mreg, size_t size) {
  Arm64ManagedRegister reg = mreg.AsArm64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsWRegister()) << reg;
  if (size == 1) {
    ___ Sxtb(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  } else {
    ___ Sxth(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  }
}

void Arm64JNIMacroAssembler::ZeroExtend(ManagedRegister mreg, size_t size) {
  Arm64ManagedRegister reg = mreg.AsArm64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsWRegister()) << reg;
  if (size == 1) {
    ___ Uxtb(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  } else {
    ___ Uxth(reg_w(reg.AsWRegister()), reg_w(reg.AsWRegister()));
  }
}

void Arm64JNIMacroAssembler::VerifyObject(ManagedRegister /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void Arm64JNIMacroAssembler::VerifyObject(FrameOffset /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references.
}

void Arm64JNIMacroAssembler::Jump(ManagedRegister m_base, Offset offs) {
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(base.IsXRegister()) << base;
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  ___ Ldr(scratch, MEM_OP(reg_x(base.AsXRegister()), offs.Int32Value()));
  ___ Br(scratch);
}

void Arm64JNIMacroAssembler::Call(ManagedRegister m_base, Offset offs) {
  Arm64ManagedRegister base = m_base.AsArm64();
  CHECK(base.IsXRegister()) << base;
  ___ Ldr(lr, MEM_OP(reg_x(base.AsXRegister()), offs.Int32Value()));
  ___ Blr(lr);
}

void Arm64JNIMacroAssembler::Call(FrameOffset base, Offset offs) {
  // Call *(*(SP + base) + offset)
  ___ Ldr(lr, MEM_OP(reg_x(SP), base.Int32Value()));
  ___ Ldr(lr, MEM_OP(lr, offs.Int32Value()));
  ___ Blr(lr);
}

void Arm64JNIMacroAssembler::CallFromThread(ThreadOffset64 offset) {
  // Call *(TR + offset)
  ___ Ldr(lr, MEM_OP(reg_x(TR), offset.Int32Value()));
  ___ Blr(lr);
}

void Arm64JNIMacroAssembler::CreateJObject(ManagedRegister m_out_reg,
                                           FrameOffset spilled_reference_offset,
                                           ManagedRegister m_in_reg,
                                           bool null_allowed) {
  Arm64ManagedRegister out_reg = m_out_reg.AsArm64();
  Arm64ManagedRegister in_reg = m_in_reg.AsArm64();
  // For now we only hold stale handle scope entries in x registers.
  CHECK(in_reg.IsNoRegister() || in_reg.IsXRegister()) << in_reg;
  CHECK(out_reg.IsXRegister()) << out_reg;
  if (null_allowed) {
    // Null values get a jobject value null. Otherwise, the jobject is
    // the address of the spilled reference.
    // e.g. out_reg = (in == 0) ? 0 : (SP+spilled_reference_offset)
    if (in_reg.IsNoRegister()) {
      LoadWFromOffset(kLoadWord, out_reg.AsOverlappingWRegister(), SP,
                      spilled_reference_offset.Int32Value());
      in_reg = out_reg;
    }
    ___ Cmp(reg_w(in_reg.AsOverlappingWRegister()), 0);
    if (!out_reg.Equals(in_reg)) {
      LoadImmediate(out_reg.AsXRegister(), 0, eq);
    }
    AddConstant(out_reg.AsXRegister(), SP, spilled_reference_offset.Int32Value(), ne);
  } else {
    AddConstant(out_reg.AsXRegister(), SP, spilled_reference_offset.Int32Value(), al);
  }
}

void Arm64JNIMacroAssembler::CreateJObject(FrameOffset out_off,
                                           FrameOffset spilled_reference_offset,
                                           bool null_allowed) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  if (null_allowed) {
    Register scratch2 = temps.AcquireW();
    ___ Ldr(scratch2, MEM_OP(reg_x(SP), spilled_reference_offset.Int32Value()));
    ___ Add(scratch, reg_x(SP), spilled_reference_offset.Int32Value());
    // Null values get a jobject value null. Otherwise, the jobject is
    // the address of the spilled reference.
    // e.g. scratch = (scratch == 0) ? 0 : (SP+spilled_reference_offset)
    ___ Cmp(scratch2, 0);
    ___ Csel(scratch, scratch, xzr, ne);
  } else {
    ___ Add(scratch, reg_x(SP), spilled_reference_offset.Int32Value());
  }
  ___ Str(scratch, MEM_OP(reg_x(SP), out_off.Int32Value()));
}

void Arm64JNIMacroAssembler::SuspendCheck(JNIMacroLabel* label) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireW();
  ___ Ldrh(scratch, MEM_OP(reg_x(TR), Thread::ThreadFlagsOffset<kArm64PointerSize>().Int32Value()));
  ___ Cbnz(scratch, Arm64JNIMacroLabel::Cast(label)->AsArm64());
}

void Arm64JNIMacroAssembler::ExceptionPoll(JNIMacroLabel* label) {
  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register scratch = temps.AcquireX();
  ___ Ldr(scratch, MEM_OP(reg_x(TR), Thread::ExceptionOffset<kArm64PointerSize>().Int32Value()));
  ___ Cbnz(scratch, Arm64JNIMacroLabel::Cast(label)->AsArm64());
}

void Arm64JNIMacroAssembler::DeliverPendingException() {
  // Pass exception object as argument.
  // Don't care about preserving X0 as this won't return.
  // Note: The scratch register from `ExceptionPoll()` may have been clobbered.
  ___ Ldr(reg_x(X0), MEM_OP(reg_x(TR), Thread::ExceptionOffset<kArm64PointerSize>().Int32Value()));
  ___ Ldr(lr,
          MEM_OP(reg_x(TR),
                 QUICK_ENTRYPOINT_OFFSET(kArm64PointerSize, pDeliverException).Int32Value()));
  ___ Blr(lr);
  // Call should never return.
  ___ Brk();
}

std::unique_ptr<JNIMacroLabel> Arm64JNIMacroAssembler::CreateLabel() {
  return std::unique_ptr<JNIMacroLabel>(new Arm64JNIMacroLabel());
}

void Arm64JNIMacroAssembler::Jump(JNIMacroLabel* label) {
  CHECK(label != nullptr);
  ___ B(Arm64JNIMacroLabel::Cast(label)->AsArm64());
}

void Arm64JNIMacroAssembler::TestGcMarking(JNIMacroLabel* label, JNIMacroUnaryCondition cond) {
  CHECK(label != nullptr);

  UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
  Register test_reg;
  DCHECK_EQ(Thread::IsGcMarkingSize(), 4u);
  DCHECK(kUseReadBarrier);
  if (kUseBakerReadBarrier) {
    // TestGcMarking() is used in the JNI stub entry when the marking register is up to date.
    if (kIsDebugBuild && emit_run_time_checks_in_debug_mode_) {
      Register temp = temps.AcquireW();
      asm_.GenerateMarkingRegisterCheck(temp);
    }
    test_reg = reg_w(MR);
  } else {
    test_reg = temps.AcquireW();
    int32_t is_gc_marking_offset = Thread::IsGcMarkingOffset<kArm64PointerSize>().Int32Value();
    ___ Ldr(test_reg, MEM_OP(reg_x(TR), is_gc_marking_offset));
  }
  switch (cond) {
    case JNIMacroUnaryCondition::kZero:
      ___ Cbz(test_reg, Arm64JNIMacroLabel::Cast(label)->AsArm64());
      break;
    case JNIMacroUnaryCondition::kNotZero:
      ___ Cbnz(test_reg, Arm64JNIMacroLabel::Cast(label)->AsArm64());
      break;
    default:
      LOG(FATAL) << "Not implemented unary condition: " << static_cast<int>(cond);
      UNREACHABLE();
  }
}

void Arm64JNIMacroAssembler::Bind(JNIMacroLabel* label) {
  CHECK(label != nullptr);
  ___ Bind(Arm64JNIMacroLabel::Cast(label)->AsArm64());
}

void Arm64JNIMacroAssembler::BuildFrame(size_t frame_size,
                                        ManagedRegister method_reg,
                                        ArrayRef<const ManagedRegister> callee_save_regs) {
  // Setup VIXL CPURegList for callee-saves.
  CPURegList core_reg_list(CPURegister::kRegister, kXRegSize, 0);
  CPURegList fp_reg_list(CPURegister::kVRegister, kDRegSize, 0);
  for (auto r : callee_save_regs) {
    Arm64ManagedRegister reg = r.AsArm64();
    if (reg.IsXRegister()) {
      core_reg_list.Combine(reg_x(reg.AsXRegister()).GetCode());
    } else {
      DCHECK(reg.IsDRegister());
      fp_reg_list.Combine(reg_d(reg.AsDRegister()).GetCode());
    }
  }
  size_t core_reg_size = core_reg_list.GetTotalSizeInBytes();
  size_t fp_reg_size = fp_reg_list.GetTotalSizeInBytes();

  // Increase frame to required size.
  DCHECK_ALIGNED(frame_size, kStackAlignment);
  // Must at least have space for Method* if we're going to spill it.
  DCHECK_GE(frame_size,
            core_reg_size + fp_reg_size + (method_reg.IsRegister() ? kXRegSizeInBytes : 0u));
  IncreaseFrameSize(frame_size);

  // Save callee-saves.
  asm_.SpillRegisters(core_reg_list, frame_size - core_reg_size);
  asm_.SpillRegisters(fp_reg_list, frame_size - core_reg_size - fp_reg_size);

  if (method_reg.IsRegister()) {
    // Write ArtMethod*
    DCHECK(X0 == method_reg.AsArm64().AsXRegister());
    StoreToOffset(X0, SP, 0);
  }
}

void Arm64JNIMacroAssembler::RemoveFrame(size_t frame_size,
                                         ArrayRef<const ManagedRegister> callee_save_regs,
                                         bool may_suspend) {
  // Setup VIXL CPURegList for callee-saves.
  CPURegList core_reg_list(CPURegister::kRegister, kXRegSize, 0);
  CPURegList fp_reg_list(CPURegister::kVRegister, kDRegSize, 0);
  for (auto r : callee_save_regs) {
    Arm64ManagedRegister reg = r.AsArm64();
    if (reg.IsXRegister()) {
      core_reg_list.Combine(reg_x(reg.AsXRegister()).GetCode());
    } else {
      DCHECK(reg.IsDRegister());
      fp_reg_list.Combine(reg_d(reg.AsDRegister()).GetCode());
    }
  }
  size_t core_reg_size = core_reg_list.GetTotalSizeInBytes();
  size_t fp_reg_size = fp_reg_list.GetTotalSizeInBytes();

  // For now we only check that the size of the frame is large enough to hold spills and method
  // reference.
  DCHECK_GE(frame_size, core_reg_size + fp_reg_size);
  DCHECK_ALIGNED(frame_size, kAapcs64StackAlignment);

  cfi().RememberState();

  // Restore callee-saves.
  asm_.UnspillRegisters(core_reg_list, frame_size - core_reg_size);
  asm_.UnspillRegisters(fp_reg_list, frame_size - core_reg_size - fp_reg_size);

  if (kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    vixl::aarch64::Register mr = reg_x(MR);  // Marking Register.
    vixl::aarch64::Register tr = reg_x(TR);  // Thread Register.

    if (may_suspend) {
      // The method may be suspended; refresh the Marking Register.
      ___ Ldr(mr.W(), MemOperand(tr, Thread::IsGcMarkingOffset<kArm64PointerSize>().Int32Value()));
    } else {
      // The method shall not be suspended; no need to refresh the Marking Register.

      // The Marking Register is a callee-save register and thus has been
      // preserved by native code following the AAPCS64 calling convention.

      // The following condition is a compile-time one, so it does not have a run-time cost.
      if (kIsDebugBuild) {
        // The following condition is a run-time one; it is executed after the
        // previous compile-time test, to avoid penalizing non-debug builds.
        if (emit_run_time_checks_in_debug_mode_) {
          // Emit a run-time check verifying that the Marking Register is up-to-date.
          UseScratchRegisterScope temps(asm_.GetVIXLAssembler());
          Register temp = temps.AcquireW();
          // Ensure we are not clobbering a callee-save register that was restored before.
          DCHECK(!core_reg_list.IncludesAliasOf(temp.X()))
              << "core_reg_list should not contain scratch register X" << temp.GetCode();
          asm_.GenerateMarkingRegisterCheck(temp);
        }
      }
    }
  }

  // Decrease frame size to start of callee saved regs.
  DecreaseFrameSize(frame_size);

  // Return to LR.
  ___ Ret();

  // The CFI should be restored for any code that follows the exit block.
  cfi().RestoreState();
  cfi().DefCFAOffset(frame_size);
}

#undef ___

}  // namespace arm64
}  // namespace art
