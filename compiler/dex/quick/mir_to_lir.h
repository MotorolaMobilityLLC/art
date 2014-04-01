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

#ifndef ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_
#define ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_

#include "invoke_type.h"
#include "compiled_method.h"
#include "dex/compiler_enums.h"
#include "dex/compiler_ir.h"
#include "dex/reg_storage.h"
#include "dex/backend.h"
#include "driver/compiler_driver.h"
#include "leb128.h"
#include "safe_map.h"
#include "utils/arena_allocator.h"
#include "utils/growable_array.h"

namespace art {

/*
 * TODO: refactoring pass to move these (and other) typdefs towards usage style of runtime to
 * add type safety (see runtime/offsets.h).
 */
typedef uint32_t DexOffset;          // Dex offset in code units.
typedef uint16_t NarrowDexOffset;    // For use in structs, Dex offsets range from 0 .. 0xffff.
typedef uint32_t CodeOffset;         // Native code offset in bytes.

// Set to 1 to measure cost of suspend check.
#define NO_SUSPEND 0

#define IS_BINARY_OP         (1ULL << kIsBinaryOp)
#define IS_BRANCH            (1ULL << kIsBranch)
#define IS_IT                (1ULL << kIsIT)
#define IS_LOAD              (1ULL << kMemLoad)
#define IS_QUAD_OP           (1ULL << kIsQuadOp)
#define IS_QUIN_OP           (1ULL << kIsQuinOp)
#define IS_SEXTUPLE_OP       (1ULL << kIsSextupleOp)
#define IS_STORE             (1ULL << kMemStore)
#define IS_TERTIARY_OP       (1ULL << kIsTertiaryOp)
#define IS_UNARY_OP          (1ULL << kIsUnaryOp)
#define NEEDS_FIXUP          (1ULL << kPCRelFixup)
#define NO_OPERAND           (1ULL << kNoOperand)
#define REG_DEF0             (1ULL << kRegDef0)
#define REG_DEF1             (1ULL << kRegDef1)
#define REG_DEF2             (1ULL << kRegDef2)
#define REG_DEFA             (1ULL << kRegDefA)
#define REG_DEFD             (1ULL << kRegDefD)
#define REG_DEF_FPCS_LIST0   (1ULL << kRegDefFPCSList0)
#define REG_DEF_FPCS_LIST2   (1ULL << kRegDefFPCSList2)
#define REG_DEF_LIST0        (1ULL << kRegDefList0)
#define REG_DEF_LIST1        (1ULL << kRegDefList1)
#define REG_DEF_LR           (1ULL << kRegDefLR)
#define REG_DEF_SP           (1ULL << kRegDefSP)
#define REG_USE0             (1ULL << kRegUse0)
#define REG_USE1             (1ULL << kRegUse1)
#define REG_USE2             (1ULL << kRegUse2)
#define REG_USE3             (1ULL << kRegUse3)
#define REG_USE4             (1ULL << kRegUse4)
#define REG_USEA             (1ULL << kRegUseA)
#define REG_USEC             (1ULL << kRegUseC)
#define REG_USED             (1ULL << kRegUseD)
#define REG_USEB             (1ULL << kRegUseB)
#define REG_USE_FPCS_LIST0   (1ULL << kRegUseFPCSList0)
#define REG_USE_FPCS_LIST2   (1ULL << kRegUseFPCSList2)
#define REG_USE_LIST0        (1ULL << kRegUseList0)
#define REG_USE_LIST1        (1ULL << kRegUseList1)
#define REG_USE_LR           (1ULL << kRegUseLR)
#define REG_USE_PC           (1ULL << kRegUsePC)
#define REG_USE_SP           (1ULL << kRegUseSP)
#define SETS_CCODES          (1ULL << kSetsCCodes)
#define USES_CCODES          (1ULL << kUsesCCodes)
#define USE_FP_STACK         (1ULL << kUseFpStack)

// Common combo register usage patterns.
#define REG_DEF01            (REG_DEF0 | REG_DEF1)
#define REG_DEF01_USE2       (REG_DEF0 | REG_DEF1 | REG_USE2)
#define REG_DEF0_USE01       (REG_DEF0 | REG_USE01)
#define REG_DEF0_USE0        (REG_DEF0 | REG_USE0)
#define REG_DEF0_USE12       (REG_DEF0 | REG_USE12)
#define REG_DEF0_USE123      (REG_DEF0 | REG_USE123)
#define REG_DEF0_USE1        (REG_DEF0 | REG_USE1)
#define REG_DEF0_USE2        (REG_DEF0 | REG_USE2)
#define REG_DEFAD_USEAD      (REG_DEFAD_USEA | REG_USED)
#define REG_DEFAD_USEA       (REG_DEFA_USEA | REG_DEFD)
#define REG_DEFA_USEA        (REG_DEFA | REG_USEA)
#define REG_USE012           (REG_USE01 | REG_USE2)
#define REG_USE014           (REG_USE01 | REG_USE4)
#define REG_USE01            (REG_USE0 | REG_USE1)
#define REG_USE02            (REG_USE0 | REG_USE2)
#define REG_USE12            (REG_USE1 | REG_USE2)
#define REG_USE23            (REG_USE2 | REG_USE3)
#define REG_USE123           (REG_USE1 | REG_USE2 | REG_USE3)

struct BasicBlock;
struct CallInfo;
struct CompilationUnit;
struct InlineMethod;
struct MIR;
struct LIR;
struct RegLocation;
struct RegisterInfo;
class DexFileMethodInliner;
class MIRGraph;
class Mir2Lir;

typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int,
                            const MethodReference& target_method,
                            uint32_t method_idx, uintptr_t direct_code,
                            uintptr_t direct_method, InvokeType type);

typedef std::vector<uint8_t> CodeBuffer;

struct UseDefMasks {
  uint64_t use_mask;        // Resource mask for use.
  uint64_t def_mask;        // Resource mask for def.
};

struct AssemblyInfo {
  LIR* pcrel_next;           // Chain of LIR nodes needing pc relative fixups.
};

struct LIR {
  CodeOffset offset;             // Offset of this instruction.
  NarrowDexOffset dalvik_offset;   // Offset of Dalvik opcode in code units (16-bit words).
  int16_t opcode;
  LIR* next;
  LIR* prev;
  LIR* target;
  struct {
    unsigned int alias_info:17;  // For Dalvik register disambiguation.
    bool is_nop:1;               // LIR is optimized away.
    unsigned int size:4;         // Note: size of encoded instruction is in bytes.
    bool use_def_invalid:1;      // If true, masks should not be used.
    unsigned int generation:1;   // Used to track visitation state during fixup pass.
    unsigned int fixup:8;        // Fixup kind.
  } flags;
  union {
    UseDefMasks m;               // Use & Def masks used during optimization.
    AssemblyInfo a;              // Instruction info used during assembly phase.
  } u;
  int32_t operands[5];           // [0..4] = [dest, src1, src2, extra, extra2].
};

// Target-specific initialization.
Mir2Lir* ArmCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* MipsCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);

// Utility macros to traverse the LIR list.
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

// Defines for alias_info (tracks Dalvik register references).
#define DECODE_ALIAS_INFO_REG(X)        (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE_FLAG     (0x10000)
#define DECODE_ALIAS_INFO_WIDE(X)       ((X & DECODE_ALIAS_INFO_WIDE_FLAG) ? 1 : 0)
#define ENCODE_ALIAS_INFO(REG, ISWIDE)  (REG | (ISWIDE ? DECODE_ALIAS_INFO_WIDE_FLAG : 0))

// Common resource macros.
#define ENCODE_CCODE            (1ULL << kCCode)
#define ENCODE_FP_STATUS        (1ULL << kFPStatus)

// Abstract memory locations.
#define ENCODE_DALVIK_REG       (1ULL << kDalvikReg)
#define ENCODE_LITERAL          (1ULL << kLiteral)
#define ENCODE_HEAP_REF         (1ULL << kHeapRef)
#define ENCODE_MUST_NOT_ALIAS   (1ULL << kMustNotAlias)

#define ENCODE_ALL              (~0ULL)
#define ENCODE_MEM              (ENCODE_DALVIK_REG | ENCODE_LITERAL | \
                                 ENCODE_HEAP_REF | ENCODE_MUST_NOT_ALIAS)

#define ENCODE_REG_PAIR(low_reg, high_reg) ((low_reg & 0xff) | ((high_reg & 0xff) << 8))
#define DECODE_REG_PAIR(both_regs, low_reg, high_reg) \
  do { \
    low_reg = both_regs & 0xff; \
    high_reg = (both_regs >> 8) & 0xff; \
  } while (false)

// Mask to denote sreg as the start of a double.  Must not interfere with low 16 bits.
#define STARTING_DOUBLE_SREG 0x10000

// TODO: replace these macros
#define SLOW_FIELD_PATH (cu_->enable_debug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cu_->enable_debug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cu_->enable_debug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowestStringPath))

class Mir2Lir : public Backend {
  public:
    /*
     * Auxiliary information describing the location of data embedded in the Dalvik
     * byte code stream.
     */
    struct EmbeddedData {
      CodeOffset offset;        // Code offset of data block.
      const uint16_t* table;      // Original dex data.
      DexOffset vaddr;            // Dalvik offset of parent opcode.
    };

    struct FillArrayData : EmbeddedData {
      int32_t size;
    };

    struct SwitchTable : EmbeddedData {
      LIR* anchor;                // Reference instruction for relative offsets.
      LIR** targets;              // Array of case targets.
    };

    /* Static register use counts */
    struct RefCounts {
      int count;
      int s_reg;
    };

    /*
     * Data structure tracking the mapping between a Dalvik register (pair) and a
     * native register (pair). The idea is to reuse the previously loaded value
     * if possible, otherwise to keep the value in a native register as long as
     * possible.
     */
    struct RegisterInfo {
      int reg;                    // Reg number
      bool in_use;                // Has it been allocated?
      bool is_temp;               // Can allocate as temp?
      bool pair;                  // Part of a register pair?
      int partner;                // If pair, other reg of pair.
      bool live;                  // Is there an associated SSA name?
      bool dirty;                 // If live, is it dirty?
      int s_reg;                  // Name of live value.
      LIR *def_start;             // Starting inst in last def sequence.
      LIR *def_end;               // Ending inst in last def sequence.
    };

    struct RegisterPool {
       int num_core_regs;
       RegisterInfo *core_regs;
       int next_core_reg;
       int num_fp_regs;
       RegisterInfo *FPRegs;
       int next_fp_reg;
     };

    struct PromotionMap {
      RegLocationType core_location:3;
      uint8_t core_reg;
      RegLocationType fp_location:3;
      uint8_t FpReg;
      bool first_in_pair;
    };

    //
    // Slow paths.  This object is used generate a sequence of code that is executed in the
    // slow path.  For example, resolving a string or class is slow as it will only be executed
    // once (after that it is resolved and doesn't need to be done again).  We want slow paths
    // to be placed out-of-line, and not require a (mispredicted, probably) conditional forward
    // branch over them.
    //
    // If you want to create a slow path, declare a class derived from LIRSlowPath and provide
    // the Compile() function that will be called near the end of the code generated by the
    // method.
    //
    // The basic flow for a slow path is:
    //
    //     CMP reg, #value
    //     BEQ fromfast
    //   cont:
    //     ...
    //     fast path code
    //     ...
    //     more code
    //     ...
    //     RETURN
    ///
    //   fromfast:
    //     ...
    //     slow path code
    //     ...
    //     B cont
    //
    // So you see we need two labels and two branches.  The first branch (called fromfast) is
    // the conditional branch to the slow path code.  The second label (called cont) is used
    // as an unconditional branch target for getting back to the code after the slow path
    // has completed.
    //

    class LIRSlowPath {
     public:
      LIRSlowPath(Mir2Lir* m2l, const DexOffset dexpc, LIR* fromfast,
                  LIR* cont = nullptr) :
        m2l_(m2l), current_dex_pc_(dexpc), fromfast_(fromfast), cont_(cont) {
      }
      virtual ~LIRSlowPath() {}
      virtual void Compile() = 0;

      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocData);
      }

     protected:
      LIR* GenerateTargetLabel();

      Mir2Lir* const m2l_;
      const DexOffset current_dex_pc_;
      LIR* const fromfast_;
      LIR* const cont_;
    };

    virtual ~Mir2Lir() {}

    int32_t s4FromSwitchData(const void* switch_data) {
      return *reinterpret_cast<const int32_t*>(switch_data);
    }

    RegisterClass oat_reg_class_by_size(OpSize size) {
      return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte ||
              size == kSignedByte) ? kCoreReg : kAnyReg;
    }

    size_t CodeBufferSizeInBytes() {
      return code_buffer_.size() / sizeof(code_buffer_[0]);
    }

    static bool IsPseudoLirOp(int opcode) {
      return (opcode < 0);
    }

    /*
     * LIR operands are 32-bit integers.  Sometimes, (especially for managing
     * instructions which require PC-relative fixups), we need the operands to carry
     * pointers.  To do this, we assign these pointers an index in pointer_storage_, and
     * hold that index in the operand array.
     * TUNING: If use of these utilities becomes more common on 32-bit builds, it
     * may be worth conditionally-compiling a set of identity functions here.
     */
    uint32_t WrapPointer(void* pointer) {
      uint32_t res = pointer_storage_.Size();
      pointer_storage_.Insert(pointer);
      return res;
    }

    void* UnwrapPointer(size_t index) {
      return pointer_storage_.Get(index);
    }

    // strdup(), but allocates from the arena.
    char* ArenaStrdup(const char* str) {
      size_t len = strlen(str) + 1;
      char* res = reinterpret_cast<char*>(arena_->Alloc(len, kArenaAllocMisc));
      if (res != NULL) {
        strncpy(res, str, len);
      }
      return res;
    }

    // Shared by all targets - implemented in codegen_util.cc
    void AppendLIR(LIR* lir);
    void InsertLIRBefore(LIR* current_lir, LIR* new_lir);
    void InsertLIRAfter(LIR* current_lir, LIR* new_lir);

    /**
     * @brief Provides the maximum number of compiler temporaries that the backend can/wants
     * to place in a frame.
     * @return Returns the maximum number of compiler temporaries.
     */
    size_t GetMaxPossibleCompilerTemps() const;

    /**
     * @brief Provides the number of bytes needed in frame for spilling of compiler temporaries.
     * @return Returns the size in bytes for space needed for compiler temporary spill region.
     */
    size_t GetNumBytesForCompilerTempSpillRegion();

    DexOffset GetCurrentDexPc() const {
      return current_dalvik_offset_;
    }

    int ComputeFrameSize();
    virtual void Materialize();
    virtual CompiledMethod* GetCompiledMethod();
    void MarkSafepointPC(LIR* inst);
    void SetupResourceMasks(LIR* lir);
    void SetMemRefType(LIR* lir, bool is_load, int mem_type);
    void AnnotateDalvikRegAccess(LIR* lir, int reg_id, bool is_load, bool is64bit);
    void SetupRegMask(uint64_t* mask, int reg);
    void DumpLIRInsn(LIR* arg, unsigned char* base_addr);
    void DumpPromotionMap();
    void CodegenDump();
    LIR* RawLIR(DexOffset dalvik_offset, int opcode, int op0 = 0, int op1 = 0,
                int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
    LIR* NewLIR0(int opcode);
    LIR* NewLIR1(int opcode, int dest);
    LIR* NewLIR2(int opcode, int dest, int src1);
    LIR* NewLIR2NoDest(int opcode, int src, int info);
    LIR* NewLIR3(int opcode, int dest, int src1, int src2);
    LIR* NewLIR4(int opcode, int dest, int src1, int src2, int info);
    LIR* NewLIR5(int opcode, int dest, int src1, int src2, int info1, int info2);
    LIR* ScanLiteralPool(LIR* data_target, int value, unsigned int delta);
    LIR* ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi);
    LIR* AddWordData(LIR* *constant_list_p, int value);
    LIR* AddWideData(LIR* *constant_list_p, int val_lo, int val_hi);
    void ProcessSwitchTables();
    void DumpSparseSwitchTable(const uint16_t* table);
    void DumpPackedSwitchTable(const uint16_t* table);
    void MarkBoundary(DexOffset offset, const char* inst_str);
    void NopLIR(LIR* lir);
    void UnlinkLIR(LIR* lir);
    bool EvaluateBranch(Instruction::Code opcode, int src1, int src2);
    bool IsInexpensiveConstant(RegLocation rl_src);
    ConditionCode FlipComparisonOrder(ConditionCode before);
    ConditionCode NegateComparison(ConditionCode before);
    virtual void InstallLiteralPools();
    void InstallSwitchTables();
    void InstallFillArrayData();
    bool VerifyCatchEntries();
    void CreateMappingTables();
    void CreateNativeGcMap();
    int AssignLiteralOffset(CodeOffset offset);
    int AssignSwitchTablesOffset(CodeOffset offset);
    int AssignFillArrayDataOffset(CodeOffset offset);
    LIR* InsertCaseLabel(DexOffset vaddr, int keyVal);
    void MarkPackedCaseLabels(Mir2Lir::SwitchTable* tab_rec);
    void MarkSparseCaseLabels(Mir2Lir::SwitchTable* tab_rec);
    // Handle bookkeeping to convert a wide RegLocation to a narow RegLocation.  No code generated.
    RegLocation NarrowRegLoc(RegLocation loc);

    // Shared by all targets - implemented in local_optimizations.cc
    void ConvertMemOpIntoMove(LIR* orig_lir, RegStorage dest, RegStorage src);
    void ApplyLoadStoreElimination(LIR* head_lir, LIR* tail_lir);
    void ApplyLoadHoisting(LIR* head_lir, LIR* tail_lir);
    void ApplyLocalOptimizations(LIR* head_lir, LIR* tail_lir);

    // Shared by all targets - implemented in ralloc_util.cc
    int GetSRegHi(int lowSreg);
    bool oat_live_out(int s_reg);
    int oatSSASrc(MIR* mir, int num);
    void SimpleRegAlloc();
    void ResetRegPool();
    void CompilerInitPool(RegisterInfo* regs, int* reg_nums, int num);
    void DumpRegPool(RegisterInfo* p, int num_regs);
    void DumpCoreRegPool();
    void DumpFpRegPool();
    /* Mark a temp register as dead.  Does not affect allocation state. */
    void Clobber(int reg) {
      ClobberBody(GetRegInfo(reg));
    }
    void Clobber(RegStorage reg);
    void ClobberSRegBody(RegisterInfo* p, int num_regs, int s_reg);
    void ClobberSReg(int s_reg);
    int SRegToPMap(int s_reg);
    void RecordCorePromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedCoreReg(int s_reg);
    void RecordFpPromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedSingle(int s_reg);
    RegStorage AllocPreservedDouble(int s_reg);
    RegStorage AllocTempBody(RegisterInfo* p, int num_regs, int* next_temp, bool required);
    virtual RegStorage AllocTempDouble();
    RegStorage AllocFreeTemp();
    RegStorage AllocTemp();
    RegStorage AllocTempFloat();
    RegisterInfo* AllocLiveBody(RegisterInfo* p, int num_regs, int s_reg);
    RegisterInfo* AllocLive(int s_reg, int reg_class);
    void FreeTemp(int reg);
    void FreeTemp(RegStorage reg);
    RegisterInfo* IsLive(int reg);
    bool IsLive(RegStorage reg);
    RegisterInfo* IsTemp(int reg);
    bool IsTemp(RegStorage reg);
    RegisterInfo* IsPromoted(int reg);
    bool IsPromoted(RegStorage reg);
    bool IsDirty(int reg);
    bool IsDirty(RegStorage reg);
    void LockTemp(int reg);
    void LockTemp(RegStorage reg);
    void ResetDef(int reg);
    void ResetDef(RegStorage reg);
    void NullifyRange(LIR *start, LIR *finish, int s_reg1, int s_reg2);
    void MarkDef(RegLocation rl, LIR *start, LIR *finish);
    void MarkDefWide(RegLocation rl, LIR *start, LIR *finish);
    RegLocation WideToNarrow(RegLocation rl);
    void ResetDefLoc(RegLocation rl);
    virtual void ResetDefLocWide(RegLocation rl);
    void ResetDefTracking();
    void ClobberAllRegs();
    void FlushSpecificReg(RegisterInfo* info);
    void FlushAllRegsBody(RegisterInfo* info, int num_regs);
    void FlushAllRegs();
    bool RegClassMatches(int reg_class, RegStorage reg);
    void MarkLive(RegStorage reg, int s_reg);
    void MarkTemp(int reg);
    void MarkTemp(RegStorage reg);
    void UnmarkTemp(int reg);
    void UnmarkTemp(RegStorage reg);
    void MarkPair(int low_reg, int high_reg);
    void MarkClean(RegLocation loc);
    void MarkDirty(RegLocation loc);
    void MarkInUse(int reg);
    void MarkInUse(RegStorage reg);
    void CopyRegInfo(int new_reg, int old_reg);
    void CopyRegInfo(RegStorage new_reg, RegStorage old_reg);
    bool CheckCorePoolSanity();
    RegLocation UpdateLoc(RegLocation loc);
    virtual RegLocation UpdateLocWide(RegLocation loc);
    RegLocation UpdateRawLoc(RegLocation loc);

    /**
     * @brief Used to load register location into a typed temporary or pair of temporaries.
     * @see EvalLoc
     * @param loc The register location to load from.
     * @param reg_class Type of register needed.
     * @param update Whether the liveness information should be updated.
     * @return Returns the properly typed temporary in physical register pairs.
     */
    virtual RegLocation EvalLocWide(RegLocation loc, int reg_class, bool update);

    /**
     * @brief Used to load register location into a typed temporary.
     * @param loc The register location to load from.
     * @param reg_class Type of register needed.
     * @param update Whether the liveness information should be updated.
     * @return Returns the properly typed temporary in physical register.
     */
    virtual RegLocation EvalLoc(RegLocation loc, int reg_class, bool update);

    void CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs);
    void DumpCounts(const RefCounts* arr, int size, const char* msg);
    void DoPromotion();
    int VRegOffset(int v_reg);
    int SRegOffset(int s_reg);
    RegLocation GetReturnWide(bool is_double);
    RegLocation GetReturn(bool is_float);
    RegisterInfo* GetRegInfo(int reg);

    // Shared by all targets - implemented in gen_common.cc.
    void AddIntrinsicLaunchpad(CallInfo* info, LIR* branch, LIR* resume = nullptr);
    bool HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                          RegLocation rl_src, RegLocation rl_dest, int lit);
    bool HandleEasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit);
    void HandleSuspendLaunchPads();
    void HandleThrowLaunchPads();
    void HandleSlowPaths();
    void GenBarrier();
    LIR* GenCheck(ConditionCode c_code, ThrowKind kind);
    void MarkPossibleNullPointerException(int opt_flags);
    void MarkPossibleStackOverflowException();
    void ForceImplicitNullCheck(RegStorage reg, int opt_flags);
    LIR* GenImmedCheck(ConditionCode c_code, RegStorage reg, int imm_val, ThrowKind kind);
    LIR* GenNullCheck(RegStorage m_reg, int opt_flags);
    LIR* GenExplicitNullCheck(RegStorage m_reg, int opt_flags);
    LIR* GenRegRegCheck(ConditionCode c_code, RegStorage reg1, RegStorage reg2, ThrowKind kind);
    void GenCompareAndBranch(Instruction::Code opcode, RegLocation rl_src1,
                             RegLocation rl_src2, LIR* taken, LIR* fall_through);
    void GenCompareZeroAndBranch(Instruction::Code opcode, RegLocation rl_src,
                                 LIR* taken, LIR* fall_through);
    void GenIntToLong(RegLocation rl_dest, RegLocation rl_src);
    void GenIntNarrowing(Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src);
    void GenNewArray(uint32_t type_idx, RegLocation rl_dest,
                     RegLocation rl_src);
    void GenFilledNewArray(CallInfo* info);
    void GenSput(MIR* mir, RegLocation rl_src,
                 bool is_long_or_double, bool is_object);
    void GenSget(MIR* mir, RegLocation rl_dest,
                 bool is_long_or_double, bool is_object);
    void GenIGet(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenIPut(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                        RegLocation rl_src);

    void GenConstClass(uint32_t type_idx, RegLocation rl_dest);
    void GenConstString(uint32_t string_idx, RegLocation rl_dest);
    void GenNewInstance(uint32_t type_idx, RegLocation rl_dest);
    void GenThrow(RegLocation rl_src);
    void GenInstanceof(uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);
    void GenCheckCast(uint32_t insn_idx, uint32_t type_idx, RegLocation rl_src);
    void GenLong3Addr(OpKind first_op, OpKind second_op, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_src2);
    void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_shift);
    void GenArithOpIntLit(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src, int lit);
    void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_src2);
    void GenConversionCall(ThreadOffset<4> func_offset, RegLocation rl_dest,
                           RegLocation rl_src);
    void GenSuspendTest(int opt_flags);
    void GenSuspendTestAndBranch(int opt_flags, LIR* target);

    // This will be overridden by x86 implementation.
    virtual void GenConstWide(RegLocation rl_dest, int64_t value);
    virtual void GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                       RegLocation rl_src1, RegLocation rl_src2);

    // Shared by all targets - implemented in gen_invoke.cc.
    LIR* CallHelper(RegStorage r_tgt, ThreadOffset<4> helper_offset, bool safepoint_pc,
                    bool use_link = true);
    RegStorage CallHelperSetup(ThreadOffset<4> helper_offset);
    void CallRuntimeHelperImm(ThreadOffset<4> helper_offset, int arg0, bool safepoint_pc);
    void CallRuntimeHelperReg(ThreadOffset<4> helper_offset, RegStorage arg0, bool safepoint_pc);
    void CallRuntimeHelperRegLocation(ThreadOffset<4> helper_offset, RegLocation arg0,
                                      bool safepoint_pc);
    void CallRuntimeHelperImmImm(ThreadOffset<4> helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmRegLocation(ThreadOffset<4> helper_offset, int arg0,
                                         RegLocation arg1, bool safepoint_pc);
    void CallRuntimeHelperRegLocationImm(ThreadOffset<4> helper_offset, RegLocation arg0,
                                         int arg1, bool safepoint_pc);
    void CallRuntimeHelperImmReg(ThreadOffset<4> helper_offset, int arg0, RegStorage arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegImm(ThreadOffset<4> helper_offset, RegStorage arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmMethod(ThreadOffset<4> helper_offset, int arg0,
                                    bool safepoint_pc);
    void CallRuntimeHelperRegMethod(ThreadOffset<4> helper_offset, RegStorage arg0,
                                    bool safepoint_pc);
    void CallRuntimeHelperRegMethodRegLocation(ThreadOffset<4> helper_offset, RegStorage arg0,
                                               RegLocation arg2, bool safepoint_pc);
    void CallRuntimeHelperRegLocationRegLocation(ThreadOffset<4> helper_offset,
                                                 RegLocation arg0, RegLocation arg1,
                                                 bool safepoint_pc);
    void CallRuntimeHelperRegReg(ThreadOffset<4> helper_offset, RegStorage arg0, RegStorage arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegRegImm(ThreadOffset<4> helper_offset, RegStorage arg0, RegStorage arg1,
                                    int arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodRegLocation(ThreadOffset<4> helper_offset, int arg0,
                                               RegLocation arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodImm(ThreadOffset<4> helper_offset, int arg0, int arg2,
                                       bool safepoint_pc);
    void CallRuntimeHelperImmRegLocationRegLocation(ThreadOffset<4> helper_offset,
                                                    int arg0, RegLocation arg1, RegLocation arg2,
                                                    bool safepoint_pc);
    void CallRuntimeHelperRegLocationRegLocationRegLocation(ThreadOffset<4> helper_offset,
                                                            RegLocation arg0, RegLocation arg1,
                                                            RegLocation arg2,
                                                            bool safepoint_pc);
    void GenInvoke(CallInfo* info);
    void GenInvokeNoInline(CallInfo* info);
    void FlushIns(RegLocation* ArgLocs, RegLocation rl_method);
    int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                             NextCallInsn next_call_insn,
                             const MethodReference& target_method,
                             uint32_t vtable_idx,
                             uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                             bool skip_this);
    int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);

    /**
     * @brief Used to determine the register location of destination.
     * @details This is needed during generation of inline intrinsics because it finds destination
     *  of return,
     * either the physical register or the target of move-result.
     * @param info Information about the invoke.
     * @return Returns the destination location.
     */
    RegLocation InlineTarget(CallInfo* info);

    /**
     * @brief Used to determine the wide register location of destination.
     * @see InlineTarget
     * @param info Information about the invoke.
     * @return Returns the destination location.
     */
    RegLocation InlineTargetWide(CallInfo* info);

    bool GenInlinedCharAt(CallInfo* info);
    bool GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty);
    bool GenInlinedReverseBytes(CallInfo* info, OpSize size);
    bool GenInlinedAbsInt(CallInfo* info);
    bool GenInlinedAbsLong(CallInfo* info);
    bool GenInlinedAbsFloat(CallInfo* info);
    bool GenInlinedAbsDouble(CallInfo* info);
    bool GenInlinedFloatCvt(CallInfo* info);
    bool GenInlinedDoubleCvt(CallInfo* info);
    virtual bool GenInlinedIndexOf(CallInfo* info, bool zero_based);
    bool GenInlinedStringCompareTo(CallInfo* info);
    bool GenInlinedCurrentThread(CallInfo* info);
    bool GenInlinedUnsafeGet(CallInfo* info, bool is_long, bool is_volatile);
    bool GenInlinedUnsafePut(CallInfo* info, bool is_long, bool is_object,
                             bool is_volatile, bool is_ordered);
    int LoadArgRegs(CallInfo* info, int call_state,
                    NextCallInsn next_call_insn,
                    const MethodReference& target_method,
                    uint32_t vtable_idx,
                    uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                    bool skip_this);

    // Shared by all targets - implemented in gen_loadstore.cc.
    RegLocation LoadCurrMethod();
    void LoadCurrMethodDirect(RegStorage r_tgt);
    LIR* LoadConstant(RegStorage r_dest, int value);
    LIR* LoadWordDisp(RegStorage r_base, int displacement, RegStorage r_dest);
    RegLocation LoadValue(RegLocation rl_src, RegisterClass op_kind);
    RegLocation LoadValueWide(RegLocation rl_src, RegisterClass op_kind);
    void LoadValueDirect(RegLocation rl_src, RegStorage r_dest);
    void LoadValueDirectFixed(RegLocation rl_src, RegStorage r_dest);
    void LoadValueDirectWide(RegLocation rl_src, RegStorage r_dest);
    void LoadValueDirectWideFixed(RegLocation rl_src, RegStorage r_dest);
    LIR* StoreWordDisp(RegStorage r_base, int displacement, RegStorage r_src);

    /**
     * @brief Used to do the final store in the destination as per bytecode semantics.
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. Can be either physical register or dalvik register.
     */
    void StoreValue(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store in a wide destination as per bytecode semantics.
     * @see StoreValue
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. Can be either physical register or dalvik
     *  register.
     */
    void StoreValueWide(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store to a destination as per bytecode semantics.
     * @see StoreValue
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. It must be kLocPhysReg
     *
     * This is used for x86 two operand computations, where we have computed the correct
     * register value that now needs to be properly registered.  This is used to avoid an
     * extra register copy that would result if StoreValue was called.
     */
    void StoreFinalValue(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store in a wide destination as per bytecode semantics.
     * @see StoreValueWide
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. It must be kLocPhysReg
     *
     * This is used for x86 two operand computations, where we have computed the correct
     * register values that now need to be properly registered.  This is used to avoid an
     * extra pair of register copies that would result if StoreValueWide was called.
     */
    void StoreFinalValueWide(RegLocation rl_dest, RegLocation rl_src);

    // Shared by all targets - implemented in mir_to_lir.cc.
    void CompileDalvikInstruction(MIR* mir, BasicBlock* bb, LIR* label_list);
    void HandleExtendedMethodMIR(BasicBlock* bb, MIR* mir);
    bool MethodBlockCodeGen(BasicBlock* bb);
    bool SpecialMIR2LIR(const InlineMethod& special);
    void MethodMIR2LIR();

    /*
     * @brief Load the address of the dex method into the register.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    void LoadCodeAddress(const MethodReference& target_method, InvokeType type,
                         SpecialTargetRegister symbolic_reg);

    /*
     * @brief Load the Method* of a dex method into the register.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    virtual void LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                   SpecialTargetRegister symbolic_reg);

    /*
     * @brief Load the Class* of a Dex Class type into the register.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    virtual void LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg);

    // Routines that work for the generic case, but may be overriden by target.
    /*
     * @brief Compare memory to immediate, and branch if condition true.
     * @param cond The condition code that when true will branch to the target.
     * @param temp_reg A temporary register that can be used if compare to memory is not
     * supported by the architecture.
     * @param base_reg The register holding the base address.
     * @param offset The offset from the base.
     * @param check_value The immediate to compare to.
     * @returns The branch instruction that was generated.
     */
    virtual LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                   int offset, int check_value, LIR* target);

    // Required for target - codegen helpers.
    virtual bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual LIR* CheckSuspendUsingLoad() = 0;
    virtual RegStorage LoadHelper(ThreadOffset<4> offset) = 0;
    virtual LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest, OpSize size,
                              int s_reg) = 0;
    virtual LIR* LoadBaseDispWide(RegStorage r_base, int displacement, RegStorage r_dest,
                                  int s_reg) = 0;
    virtual LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) = 0;
    virtual LIR* LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                     int displacement, RegStorage r_dest, RegStorage r_dest_hi,
                                     OpSize size, int s_reg) = 0;
    virtual LIR* LoadConstantNoClobber(RegStorage r_dest, int value) = 0;
    virtual LIR* LoadConstantWide(RegStorage r_dest, int64_t value) = 0;
    virtual LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                               OpSize size) = 0;
    virtual LIR* StoreBaseDispWide(RegStorage r_base, int displacement, RegStorage r_src) = 0;
    virtual LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                  int scale, OpSize size) = 0;
    virtual LIR* StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                      int displacement, RegStorage r_src, RegStorage r_src_hi,
                                      OpSize size, int s_reg) = 0;
    virtual void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) = 0;

    // Required for target - register utilities.
    virtual bool IsFpReg(int reg) = 0;
    virtual bool IsFpReg(RegStorage reg) = 0;
    virtual bool SameRegType(int reg1, int reg2) = 0;
    virtual RegStorage AllocTypedTemp(bool fp_hint, int reg_class) = 0;
    virtual RegStorage AllocTypedTempWide(bool fp_hint, int reg_class) = 0;
    // TODO: elminate S2d.
    virtual int S2d(int low_reg, int high_reg) = 0;
    virtual RegStorage TargetReg(SpecialTargetRegister reg) = 0;
    virtual RegStorage GetArgMappingToPhysicalReg(int arg_num) = 0;
    virtual RegLocation GetReturnAlt() = 0;
    virtual RegLocation GetReturnWideAlt() = 0;
    virtual RegLocation LocCReturn() = 0;
    virtual RegLocation LocCReturnDouble() = 0;
    virtual RegLocation LocCReturnFloat() = 0;
    virtual RegLocation LocCReturnWide() = 0;
    // TODO: use to reduce/eliminate xx_FPREG() macro use.
    virtual uint32_t FpRegMask() = 0;
    virtual uint64_t GetRegMaskCommon(int reg) = 0;
    virtual void AdjustSpillMask() = 0;
    virtual void ClobberCallerSave() = 0;
    virtual void FlushReg(RegStorage reg) = 0;
    virtual void FlushRegWide(RegStorage reg) = 0;
    virtual void FreeCallTemps() = 0;
    virtual void FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) = 0;
    virtual void LockCallTemps() = 0;
    virtual void MarkPreservedSingle(int v_reg, int reg) = 0;
    virtual void CompilerInitializeRegAlloc() = 0;

    // Required for target - miscellaneous.
    virtual void AssembleLIR() = 0;
    virtual void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix) = 0;
    virtual void SetupTargetResourceMasks(LIR* lir, uint64_t flags) = 0;
    virtual const char* GetTargetInstFmt(int opcode) = 0;
    virtual const char* GetTargetInstName(int opcode) = 0;
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) = 0;
    virtual uint64_t GetPCUseDefEncoding() = 0;
    virtual uint64_t GetTargetInstFlags(int opcode) = 0;
    virtual int GetInsnSize(LIR* lir) = 0;
    virtual bool IsUnconditionalBranch(LIR* lir) = 0;

    // Required for target - Dalvik-level generators.
    virtual void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenMulLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAddLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAndLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenArithOpDouble(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2) = 0;
    virtual void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenConversion(Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) = 0;
    virtual bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object) = 0;

    /**
     * @brief Used to generate code for intrinsic java\.lang\.Math methods min and max.
     * @details This is also applicable for java\.lang\.StrictMath since it is a simple algorithm
     * that applies on integers. The generated code will write the smallest or largest value
     * directly into the destination register as specified by the invoke information.
     * @param info Information about the invoke.
     * @param is_min If true generates code that computes minimum. Otherwise computes maximum.
     * @return Returns true if successfully generated
     */
    virtual bool GenInlinedMinMaxInt(CallInfo* info, bool is_min) = 0;

    virtual bool GenInlinedSqrt(CallInfo* info) = 0;
    virtual bool GenInlinedPeek(CallInfo* info, OpSize size) = 0;
    virtual bool GenInlinedPoke(CallInfo* info, OpSize size) = 0;
    virtual void GenNegLong(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenOrLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2) = 0;
    virtual void GenSubLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenXorLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual LIR* GenRegMemCheck(ConditionCode c_code, RegStorage reg1, RegStorage base,
                                int offset, ThrowKind kind) = 0;
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi,
                                  bool is_div) = 0;
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit,
                                     bool is_div) = 0;
    /*
     * @brief Generate an integer div or rem operation by a literal.
     * @param rl_dest Destination Location.
     * @param rl_src1 Numerator Location.
     * @param rl_src2 Divisor Location.
     * @param is_div 'true' if this is a division, 'false' for a remainder.
     * @param check_zero 'true' if an exception should be generated if the divisor is 0.
     */
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2, bool is_div, bool check_zero) = 0;
    /*
     * @brief Generate an integer div or rem operation by a literal.
     * @param rl_dest Destination Location.
     * @param rl_src Numerator Location.
     * @param lit Divisor.
     * @param is_div 'true' if this is a division, 'false' for a remainder.
     */
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit,
                                     bool is_div) = 0;
    virtual void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) = 0;

    /**
     * @brief Used for generating code that throws ArithmeticException if both registers are zero.
     * @details This is used for generating DivideByZero checks when divisor is held in two
     *  separate registers.
     * @param reg_lo The register holding the lower 32-bits.
     * @param reg_hi The register holding the upper 32-bits.
     */
    virtual void GenDivZeroCheck(RegStorage reg) = 0;

    virtual void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) = 0;
    virtual void GenExitSequence() = 0;
    virtual void GenFillArrayData(DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double) = 0;
    virtual void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) = 0;

    /**
     * @brief Lowers the kMirOpSelect MIR into LIR.
     * @param bb The basic block in which the MIR is from.
     * @param mir The MIR whose opcode is kMirOpSelect.
     */
    virtual void GenSelect(BasicBlock* bb, MIR* mir) = 0;

    /**
     * @brief Used to generate a memory barrier in an architecture specific way.
     * @details The last generated LIR will be considered for use as barrier. Namely,
     * if the last LIR can be updated in a way where it will serve the semantics of
     * barrier, then it will be used as such. Otherwise, a new LIR will be generated
     * that can keep the semantics.
     * @param barrier_kind The kind of memory barrier to generate.
     */
    virtual void GenMemBarrier(MemBarrierKind barrier_kind) = 0;

    virtual void GenMoveException(RegLocation rl_dest) = 0;
    virtual void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) = 0;
    virtual void GenNegDouble(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenNegFloat(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) = 0;
    virtual void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale,
                             bool card_mark) = 0;
    virtual void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift) = 0;

    // Required for target - single operation generators.
    virtual LIR* OpUnconditionalBranch(LIR* target) = 0;
    virtual LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) = 0;
    virtual LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value,
                                LIR* target) = 0;
    virtual LIR* OpCondBranch(ConditionCode cc, LIR* target) = 0;
    virtual LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) = 0;
    virtual LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpIT(ConditionCode cond, const char* guide) = 0;
    virtual LIR* OpMem(OpKind op, RegStorage r_base, int disp) = 0;
    virtual LIR* OpPcRelLoad(RegStorage reg, LIR* target) = 0;
    virtual LIR* OpReg(OpKind op, RegStorage r_dest_src) = 0;
    virtual LIR* OpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value) = 0;
    virtual LIR* OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset) = 0;
    virtual LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) = 0;

    /**
     * @brief Used to generate an LIR that does a load from mem to reg.
     * @param r_dest The destination physical register.
     * @param r_base The base physical register for memory operand.
     * @param offset The displacement for memory operand.
     * @param move_type Specification on the move desired (size, alignment, register kind).
     * @return Returns the generate move LIR.
     */
    virtual LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset,
                             MoveType move_type) = 0;

    /**
     * @brief Used to generate an LIR that does a store from reg to mem.
     * @param r_base The base physical register for memory operand.
     * @param offset The displacement for memory operand.
     * @param r_src The destination physical register.
     * @param bytes_to_move The number of bytes to move.
     * @param is_aligned Whether the memory location is known to be aligned.
     * @return Returns the generate move LIR.
     */
    virtual LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src,
                             MoveType move_type) = 0;

    /**
     * @brief Used for generating a conditional register to register operation.
     * @param op The opcode kind.
     * @param cc The condition code that when true will perform the opcode.
     * @param r_dest The destination physical register.
     * @param r_src The source physical register.
     * @return Returns the newly created LIR or null in case of creation failure.
     */
    virtual LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) = 0;

    virtual LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) = 0;
    virtual LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1,
                             RegStorage r_src2) = 0;
    virtual LIR* OpTestSuspend(LIR* target) = 0;
    virtual LIR* OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) = 0;
    virtual LIR* OpVldm(RegStorage r_base, int count) = 0;
    virtual LIR* OpVstm(RegStorage r_base, int count) = 0;
    virtual void OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale,
                       int offset) = 0;
    virtual void OpRegCopyWide(RegStorage dest, RegStorage src) = 0;
    virtual void OpTlsCmp(ThreadOffset<4> offset, int val) = 0;
    virtual bool InexpensiveConstantInt(int32_t value) = 0;
    virtual bool InexpensiveConstantFloat(int32_t value) = 0;
    virtual bool InexpensiveConstantLong(int64_t value) = 0;
    virtual bool InexpensiveConstantDouble(int64_t value) = 0;

    // May be optimized by targets.
    virtual void GenMonitorEnter(int opt_flags, RegLocation rl_src);
    virtual void GenMonitorExit(int opt_flags, RegLocation rl_src);

    // Temp workaround
    void Workaround7250540(RegLocation rl_dest, RegStorage zero_reg);

  protected:
    Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    CompilationUnit* GetCompilationUnit() {
      return cu_;
    }
    /*
     * @brief Returns the index of the lowest set bit in 'x'.
     * @param x Value to be examined.
     * @returns The bit number of the lowest bit set in the value.
     */
    int32_t LowestSetBit(uint64_t x);
    /*
     * @brief Is this value a power of two?
     * @param x Value to be examined.
     * @returns 'true' if only 1 bit is set in the value.
     */
    bool IsPowerOfTwo(uint64_t x);
    /*
     * @brief Do these SRs overlap?
     * @param rl_op1 One RegLocation
     * @param rl_op2 The other RegLocation
     * @return 'true' if the VR pairs overlap
     *
     * Check to see if a result pair has a misaligned overlap with an operand pair.  This
     * is not usual for dx to generate, but it is legal (for now).  In a future rev of
     * dex, we'll want to make this case illegal.
     */
    bool BadOverlap(RegLocation rl_op1, RegLocation rl_op2);

    /*
     * @brief Force a location (in a register) into a temporary register
     * @param loc location of result
     * @returns update location
     */
    RegLocation ForceTemp(RegLocation loc);

    /*
     * @brief Force a wide location (in registers) into temporary registers
     * @param loc location of result
     * @returns update location
     */
    RegLocation ForceTempWide(RegLocation loc);

    virtual void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx,
                                    RegLocation rl_dest, RegLocation rl_src);

    void AddSlowPath(LIRSlowPath* slowpath);

    virtual void GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                            bool type_known_abstract, bool use_declaring_class,
                                            bool can_assume_type_is_in_dex_cache,
                                            uint32_t type_idx, RegLocation rl_dest,
                                            RegLocation rl_src);
    /*
     * @brief Generate the debug_frame FDE information if possible.
     * @returns pointer to vector containg CFE information, or NULL.
     */
    virtual std::vector<uint8_t>* ReturnCallFrameInformation();

    /**
     * @brief Used to insert marker that can be used to associate MIR with LIR.
     * @details Only inserts marker if verbosity is enabled.
     * @param mir The mir that is currently being generated.
     */
    void GenPrintLabel(MIR* mir);

    /**
     * @brief Used to generate return sequence when there is no frame.
     * @details Assumes that the return registers have already been populated.
     */
    virtual void GenSpecialExitSequence() = 0;

    /**
     * @brief Used to generate code for special methods that are known to be
     * small enough to work in frameless mode.
     * @param bb The basic block of the first MIR.
     * @param mir The first MIR of the special method.
     * @param special Information about the special method.
     * @return Returns whether or not this was handled successfully. Returns false
     * if caller should punt to normal MIR2LIR conversion.
     */
    virtual bool GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special);

  private:
    void ClobberBody(RegisterInfo* p);
    void ResetDefBody(RegisterInfo* p) {
      p->def_start = NULL;
      p->def_end = NULL;
    }

    void SetCurrentDexPc(DexOffset dexpc) {
      current_dalvik_offset_ = dexpc;
    }

    /**
     * @brief Used to lock register if argument at in_position was passed that way.
     * @details Does nothing if the argument is passed via stack.
     * @param in_position The argument number whose register to lock.
     * @param wide Whether the argument is wide.
     */
    void LockArg(int in_position, bool wide = false);

    /**
     * @brief Used to load VR argument to a physical register.
     * @details The load is only done if the argument is not already in physical register.
     * LockArg must have been previously called.
     * @param in_position The argument number to load.
     * @param wide Whether the argument is 64-bit or not.
     * @return Returns the register (or register pair) for the loaded argument.
     */
    RegStorage LoadArg(int in_position, bool wide = false);

    /**
     * @brief Used to load a VR argument directly to a specified register location.
     * @param in_position The argument number to place in register.
     * @param rl_dest The register location where to place argument.
     */
    void LoadArgDirect(int in_position, RegLocation rl_dest);

    /**
     * @brief Used to generate LIR for special getter method.
     * @param mir The mir that represents the iget.
     * @param special Information about the special getter method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIGet(MIR* mir, const InlineMethod& special);

    /**
     * @brief Used to generate LIR for special setter method.
     * @param mir The mir that represents the iput.
     * @param special Information about the special setter method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIPut(MIR* mir, const InlineMethod& special);

    /**
     * @brief Used to generate LIR for special return-args method.
     * @param mir The mir that represents the return of argument.
     * @param special Information about the special return-args method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIdentity(MIR* mir, const InlineMethod& special);


  public:
    // TODO: add accessors for these.
    LIR* literal_list_;                        // Constants.
    LIR* method_literal_list_;                 // Method literals requiring patching.
    LIR* class_literal_list_;                  // Class literals requiring patching.
    LIR* code_literal_list_;                   // Code literals requiring patching.
    LIR* first_fixup_;                         // Doubly-linked list of LIR nodes requiring fixups.

  protected:
    CompilationUnit* const cu_;
    MIRGraph* const mir_graph_;
    GrowableArray<SwitchTable*> switch_tables_;
    GrowableArray<FillArrayData*> fill_array_data_;
    GrowableArray<LIR*> throw_launchpads_;
    GrowableArray<LIR*> suspend_launchpads_;
    GrowableArray<RegisterInfo*> tempreg_info_;
    GrowableArray<RegisterInfo*> reginfo_map_;
    GrowableArray<void*> pointer_storage_;
    CodeOffset current_code_offset_;    // Working byte offset of machine instructons.
    CodeOffset data_offset_;            // starting offset of literal pool.
    size_t total_size_;                   // header + code size.
    LIR* block_label_list_;
    PromotionMap* promotion_map_;
    /*
     * TODO: The code generation utilities don't have a built-in
     * mechanism to propagate the original Dalvik opcode address to the
     * associated generated instructions.  For the trace compiler, this wasn't
     * necessary because the interpreter handled all throws and debugging
     * requests.  For now we'll handle this by placing the Dalvik offset
     * in the CompilationUnit struct before codegen for each instruction.
     * The low-level LIR creation utilites will pull it from here.  Rework this.
     */
    DexOffset current_dalvik_offset_;
    size_t estimated_native_code_size_;     // Just an estimate; used to reserve code_buffer_ size.
    RegisterPool* reg_pool_;
    /*
     * Sanity checking for the register temp tracking.  The same ssa
     * name should never be associated with one temp register per
     * instruction compilation.
     */
    int live_sreg_;
    CodeBuffer code_buffer_;
    // The encoding mapping table data (dex -> pc offset and pc offset -> dex) with a size prefix.
    std::vector<uint8_t> encoded_mapping_table_;
    std::vector<uint32_t> core_vmap_table_;
    std::vector<uint32_t> fp_vmap_table_;
    std::vector<uint8_t> native_gc_map_;
    int num_core_spills_;
    int num_fp_spills_;
    int frame_size_;
    unsigned int core_spill_mask_;
    unsigned int fp_spill_mask_;
    LIR* first_lir_insn_;
    LIR* last_lir_insn_;

    GrowableArray<LIRSlowPath*> slow_paths_;
};  // Class Mir2Lir

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_
