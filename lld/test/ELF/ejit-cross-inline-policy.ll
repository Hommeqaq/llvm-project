# REQUIRES: x86
## EJIT link-time hot/cold inline policy (runRealInliner's EJitInlineAdvisor).
##
## A call site in a hot branch (branch weight 2000) is inlined; a call site in
## a cold branch (branch weight 1) is not (cold threshold defaults to 0). The
## -mllvm -ejit-inline-diag switch prints the raw reason each call site was not
## inlined, plus a summary; off by default it is silent.

# RUN: rm -rf %t && split-file %s %t

## Stage 1: compile each TU with the production pass so the object carries a
## .ejit_cross section holding the full-module bitcode.
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/entry.ll -o %t/entry.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/entry.bc -o %t/entry.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/helper.ll -o %t/helper.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/helper.bc -o %t/helper.o

## Stage 2: link with the default policy (on). --save-temps keeps the per-entry
## bitcode so we can prove what was inlined.
# RUN: ld.lld --ejit-cross-inline --save-temps -shared \
# RUN:   --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/out.so

## The hot helper was inlined away; the cold helper survived as a real call and
## its definition was kept (it is still referenced).
# RUN: llvm-dis %t/out.so.ejit-cross.ejit_entry_fn.bc -o - | \
# RUN:   FileCheck %s --check-prefix=INLINED
# INLINED-LABEL: define{{.*}} i32 @ejit_entry_fn
# INLINED-NOT: call {{.*}}@hot_helper
# INLINED: call i32 @cold_helper()
# INLINED-LABEL: define{{.*}} i32 @cold_helper()

## Diagnostics off by default: no [ejit-inline] output.
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/quiet.so 2>&1 | \
# RUN:   FileCheck %s --check-prefix=QUIET --allow-empty
# QUIET-NOT: [ejit-inline]

## Diagnostics on: the cold call site is reported as a cold flow (with the
## branch-weight evidence), plus the summary line.
# RUN: ld.lld --ejit-cross-inline -mllvm -ejit-inline-diag -shared \
# RUN:   --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/diag.so 2>&1 | \
# RUN:   FileCheck %s --check-prefix=DIAG
# DIAG: [ejit-inline] ejit_entry_fn -> cold_helper: not inlined (cold flow (cold branch (edge weight 1 <= cutoff 1)))
# DIAG: [ejit-inline] summary: considered={{[0-9]+}} inlined={{[0-9]+}} not_inlined={{[0-9]+}}

## policy=off falls back to the original inliner and emits no diagnostics.
# RUN: ld.lld --ejit-cross-inline -mllvm -ejit-inline-policy=off -shared \
# RUN:   --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/off.so 2>&1 | \
# RUN:   FileCheck %s --check-prefix=POLICYOFF --allow-empty
# POLICYOFF-NOT: [ejit-inline]

#--- entry.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @hot_helper()
declare i32 @cold_helper()

; br weights 2000:1 -> successor 0 (hot) is hot, successor 1 (cold) is cold.
define i32 @ejit_entry_fn(i1 %cond) !ejit.metadata !0 {
entry:
  br i1 %cond, label %hot, label %cold, !prof !1
hot:
  %h = call i32 @hot_helper()
  br label %merge
cold:
  %c = call i32 @cold_helper()
  br label %merge
merge:
  %p = phi i32 [ %h, %hot ], [ %c, %cold ]
  ret i32 %p
}

!0 = !{!{!"ejit_entry"}}
!1 = !{!"branch_weights", i32 2000, i32 1}

#--- helper.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @ext_sink(i32)

; Small helper: inlines freely in the hot branch.
define i32 @hot_helper() {
  ret i32 10
}

; Several external calls keep the inline cost positive (and defeat O2 folding),
; so the cold threshold (0) keeps this as a real call in the cold branch.
define i32 @cold_helper() {
entry:
  call void @ext_sink(i32 1)
  call void @ext_sink(i32 2)
  call void @ext_sink(i32 3)
  call void @ext_sink(i32 4)
  call void @ext_sink(i32 5)
  ret i32 20
}
