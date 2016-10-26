#
# Copyright (C) 2016 The Android Open Source Project
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

# Known broken tests for the ARM VIXL backend.
TEST_ART_BROKEN_OPTIMIZING_ARM_VIXL_RUN_TESTS := \
  002-sleep \
  003-omnibus-opcodes \
  004-InterfaceTest \
  004-JniTest \
  004-NativeAllocations \
  004-ReferenceMap \
  004-SignalTest \
  004-StackWalk \
  004-ThreadStress \
  004-UnsafeTest \
  004-checker-UnsafeTest18 \
  005-annotations \
  006-args \
  008-exceptions \
  009-instanceof \
  011-array-copy \
  012-math \
  015-switch \
  017-float \
  018-stack-overflow \
  019-wrong-array-type \
  020-string \
  021-string2 \
  022-interface \
  023-many-interfaces \
  024-illegal-access \
  025-access-controller \
  027-arithmetic \
  028-array-write \
  031-class-attributes \
  032-concrete-sub \
  035-enum \
  036-finalizer \
  037-inherit \
  041-narrowing \
  042-new-instance \
  044-proxy \
  045-reflect-array \
  046-reflect \
  047-returns \
  048-reflect-v8 \
  049-show-object \
  050-sync-test \
  051-thread \
  052-verifier-fun \
  054-uncaught \
  058-enum-order \
  059-finalizer-throw \
  061-out-of-memory \
  062-character-encodings \
  063-process-manager \
  064-field-access \
  065-mismatched-implements \
  066-mismatched-super \
  067-preemptive-unpark \
  068-classloader \
  069-field-type \
  070-nio-buffer \
  071-dexfile \
  072-precise-gc \
  074-gc-thrash \
  075-verification-error \
  076-boolean-put \
  079-phantom \
  080-oom-throw \
  080-oom-throw-with-finalizer \
  081-hot-exceptions \
  082-inline-execute \
  083-compiler-regressions \
  086-null-super \
  087-gc-after-link \
  088-monitor-verification \
  090-loop-formation \
  091-override-package-private-method \
  093-serialization \
  094-pattern \
  096-array-copy-concurrent-gc \
  098-ddmc \
  099-vmdebug \
  100-reflect2 \
  101-fibonacci \
  102-concurrent-gc \
  103-string-append \
  104-growth-limit \
  106-exceptions2 \
  107-int-math2 \
  108-check-cast \
  109-suspend-check \
  112-double-math \
  113-multidex \
  114-ParallelGC \
  117-nopatchoat \
  119-noimage-patchoat \
  120-hashcode \
  121-modifiers \
  122-npe \
  123-compiler-regressions-mt \
  123-inline-execute2 \
  127-checker-secondarydex \
  129-ThreadGetId \
  131-structural-change \
  132-daemon-locks-shutdown \
  133-static-invoke-super \
  134-reg-promotion \
  135-MirandaDispatch \
  136-daemon-jni-shutdown \
  137-cfi \
  138-duplicate-classes-check2 \
  139-register-natives \
  140-field-packing \
  141-class-unload \
  142-classloader2 \
  144-static-field-sigquit \
  145-alloc-tracking-stress \
  146-bad-interface \
  150-loadlibrary \
  201-built-in-except-detail-messages \
  302-float-conversion \
  304-method-tracing \
  406-fields \
  407-arrays \
  410-floats \
  411-optimizing-arith-mul \
  412-new-array \
  413-regalloc-regression \
  414-optimizing-arith-sub \
  414-static-fields \
  415-optimizing-arith-neg \
  416-optimizing-arith-not \
  417-optimizing-arith-div \
  419-long-parameter \
  421-exceptions \
  422-instanceof \
  422-type-conversion \
  423-invoke-interface \
  424-checkcast \
  425-invoke-super \
  426-monitor \
  427-bitwise \
  427-bounds \
  428-optimizing-arith-rem \
  429-ssa-builder \
  430-live-register-slow-path \
  431-optimizing-arith-shifts \
  431-type-propagation \
  432-optimizing-cmp \
  434-invoke-direct \
  436-rem-float \
  436-shift-constant \
  437-inline \
  438-volatile \
  439-npe \
  439-swap-double \
  440-stmp \
  441-checker-inliner \
  442-checker-constant-folding \
  444-checker-nce \
  445-checker-licm \
  447-checker-inliner3 \
  448-multiple-returns \
  449-checker-bce \
  450-checker-types \
  451-regression-add-float \
  451-spill-splot \
  452-multiple-returns2 \
  453-not-byte \
  454-get-vreg \
  456-baseline-array-set \
  457-regs \
  458-checker-instruct-simplification \
  458-long-to-fpu \
  459-dead-phi \
  460-multiple-returns3 \
  461-get-reference-vreg \
  463-checker-boolean-simplifier \
  466-get-live-vreg \
  467-regalloc-pair \
  468-checker-bool-simplif-regression \
  469-condition-materialization \
  471-deopt-environment \
  472-type-propagation \
  474-checker-boolean-input \
  474-fp-sub-neg \
  475-regression-inliner-ids \
  477-checker-bound-type \
  477-long-2-float-convers-precision \
  478-checker-clinit-check-pruning \
  483-dce-block \
  484-checker-register-hints \
  485-checker-dce-loop-update \
  485-checker-dce-switch \
  486-checker-must-do-null-check \
  488-checker-inline-recursive-calls \
  490-checker-inline \
  491-current-method \
  492-checker-inline-invoke-interface \
  493-checker-inline-invoke-interface \
  494-checker-instanceof-tests \
  495-checker-checkcast-tests \
  496-checker-inlining-class-loader \
  497-inlining-and-class-loader \
  498-type-propagation \
  499-bce-phi-array-length \
  500-instanceof \
  501-null-constant-dce \
  501-regression-packed-switch \
  503-dead-instructions \
  504-regression-baseline-entry \
  508-checker-disassembly \
  510-checker-try-catch \
  513-array-deopt \
  514-shifts \
  515-dce-dominator \
  517-checker-builder-fallthrough \
  518-null-array-get \
  519-bound-load-class \
  520-equivalent-phi \
  521-checker-array-set-null \
  521-regression-integer-field-set \
  522-checker-regression-monitor-exit \
  523-checker-can-throw-regression \
  525-checker-arrays-fields1 \
  525-checker-arrays-fields2 \
  526-checker-caller-callee-regs \
  526-long-regalloc \
  527-checker-array-access-split \
  528-long-hint \
  529-checker-unresolved \
  529-long-split \
  530-checker-loops1 \
  530-checker-loops2 \
  530-checker-loops3 \
  530-checker-lse \
  530-checker-regression-reftyp-final \
  530-instanceof-checkcast \
  532-checker-nonnull-arrayset \
  534-checker-bce-deoptimization \
  535-deopt-and-inlining \
  535-regression-const-val \
  536-checker-intrinsic-optimization \
  536-checker-needs-access-check \
  537-checker-inline-and-unverified \
  537-checker-jump-over-jump \
  538-checker-embed-constants \
  540-checker-rtp-bug \
  541-regression-inlined-deopt \
  542-bitfield-rotates \
  542-unresolved-access-check \
  543-checker-dce-trycatch \
  543-env-long-ref \
  545-tracing-and-jit \
  546-regression-simplify-catch \
  550-checker-multiply-accumulate \
  550-checker-regression-wide-store \
  551-checker-shifter-operand \
  551-implicit-null-checks \
  551-invoke-super \
  552-checker-primitive-typeprop \
  552-checker-sharpening \
  552-invoke-non-existent-super \
  553-invoke-super \
  554-checker-rtp-checkcast \
  555-UnsafeGetLong-regression \
  556-invoke-super \
  557-checker-instruct-simplifier-ror \
  558-switch \
  559-bce-ssa \
  559-checker-irreducible-loop \
  559-checker-rtp-ifnotnull \
  560-packed-switch \
  561-divrem \
  561-shared-slowpaths \
  562-bce-preheader \
  562-no-intermediate \
  563-checker-fakestring \
  564-checker-bitcount \
  564-checker-irreducible-loop \
  564-checker-negbitwise \
  565-checker-condition-liveness \
  565-checker-doublenegbitwise \
  565-checker-irreducible-loop \
  565-checker-rotate \
  566-polymorphic-inlining \
  568-checker-onebit \
  570-checker-osr \
  570-checker-select \
  571-irreducible-loop \
  572-checker-array-get-regression \
  573-checker-checkcast-regression \
  574-irreducible-and-constant-area \
  575-checker-isnan \
  575-checker-string-init-alias \
  577-checker-fp2int \
  578-bce-visit \
  580-checker-round \
  580-checker-string-fact-intrinsics \
  581-rtp \
  582-checker-bce-length \
  583-checker-zero \
  584-checker-div-bool \
  586-checker-null-array-get \
  587-inline-class-error \
  588-checker-irreducib-lifetime-hole \
  589-super-imt \
  590-checker-arr-set-null-regression \
  591-new-instance-string \
  592-checker-regression-bool-input \
  593-checker-long-2-float-regression \
  593-checker-shift-and-simplifier \
  594-checker-array-alias \
  594-invoke-super \
  594-load-string-regression \
  595-error-class \
  596-checker-dead-phi \
  597-deopt-new-string \
  599-checker-irreducible-loop \
  600-verifier-fails \
  601-method-access \
  602-deoptimizeable \
  603-checker-instanceof \
  604-hot-static-interface \
  605-new-string-from-bytes \
  608-checker-unresolved-lse \
  609-checker-inline-interface \
  609-checker-x86-bounds-check \
  610-arraycopy \
  611-checker-simplify-if \
  612-jit-dex-cache \
  613-inlining-dex-cache \
  614-checker-dump-constant-location \
  615-checker-arm64-store-zero \
  617-clinit-oome \
  618-checker-induction \
  700-LoadArgRegs \
  701-easy-div-rem \
  702-LargeBranchOffset \
  703-floating-point-div \
  704-multiply-accumulate \
  705-register-conflict \
  800-smali \
  802-deoptimization \
  960-default-smali \
  961-default-iface-resolution-gen \
  963-default-range-smali \
  965-default-verify \
  966-default-conflict \
  967-default-ame \
  968-default-partial-compile-gen \
  969-iface-super \
  971-iface-super \
  972-default-imt-collision \
  972-iface-super-multidex \
  973-default-multidex \
  974-verify-interface-super \
  975-iface-private
