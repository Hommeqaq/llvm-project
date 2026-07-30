# REQUIRES: x86
## ld.lld is the single owner of EJIT cross-TU inline processing (PR #102
## hardening). This exercises the real production path end to end:
##   * the merge/inline/registry runs only with --ejit-cross-inline (default
##     links are byte-for-byte unchanged),
##   * it runs exactly once and emits a single .ejit_bitcode registry,
##   * the consumed .ejit_cross sections are discarded from the output,
##   * an external function and an external global referenced across the TU
##     boundary are registered legally (a cross-module reference would fail the
##     internal verifyModule and thus fail the link), and
##   * a malformed .ejit_cross fails the link instead of silently dropping to
##     the per-TU AOT bitcode.

# RUN: rm -rf %t && split-file %s %t

## Stage 1: compile each TU with the production pass so the object carries a
## .ejit_cross section holding the full-module bitcode.
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/entry.ll -o %t/entry.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/entry.bc -o %t/entry.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/helper.ll -o %t/helper.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/helper.bc -o %t/helper.o

## The inputs carry .ejit_cross and no registry yet.
# RUN: llvm-readelf -S %t/entry.o | FileCheck %s --check-prefix=INPUT
# INPUT: .ejit_cross
# INPUT-NOT: .ejit_bitcode

## Stage 2 (opt-in): exactly one registry is produced and every consumed
## .ejit_cross section is discarded from the output.
# RUN: ld.lld --ejit-cross-inline --save-temps -shared \
# RUN:   --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/out.so
# RUN: llvm-readelf -S %t/out.so | FileCheck %s --check-prefix=LINKED
# LINKED:     .ejit_bitcode
# LINKED-NOT: .ejit_cross

## The registry contains exactly two bitcode entries and two deduplicated
## external-symbol entries. On ELF64 each entry is 40 bytes, so exactly one
## table is 0xa0 bytes; checking only the section name would not catch duplicate
## tables or duplicate externals collected from both per-entry modules.
# RUN: llvm-readelf -SW %t/out.so | FileCheck %s --check-prefix=REGISTRY
# REGISTRY: .ejit_bitcode PROGBITS {{.*}} 0000a0

## --save-temps preserves the actual per-entry module produced by the same
## production path. The ordinary two-level helper chain must be gone, while the
## true external function/global references remain declarations. A function
## reached through a cross-TU constant table must survive closure trimming;
## cleanup may then fold the table load into a direct call.
# RUN: llvm-dis %t/out.so.ejit-cross.ejit_entry_fn.bc -o - | \
# RUN:   FileCheck %s --check-prefix=INLINED
# INLINED-LABEL: define{{.*}} i32 @ejit_entry_fn()
# INLINED-NOT: call i32 @helper
# INLINED-NOT: call i32 @helper_leaf
# INLINED: call i32 @table_target()
# INLINED: call i32 @some_extern_fn
# INLINED: load i32, ptr @extern_global
# INLINED: define{{.*}} i32 @table_target()
# RUN: llvm-dis %t/out.so.ejit-cross.ejit_entry_two.bc -o - | \
# RUN:   FileCheck %s --check-prefix=SECOND
# SECOND-LABEL: define{{.*}} i32 @ejit_entry_two()
# SECOND: call i32 @some_extern_fn(i32 5)
# SECOND: load i32, ptr @extern_global

## Default (no flag): behaviour is unchanged. The .ejit_cross sections are left
## untouched and no registry is synthesised.
# RUN: ld.lld -shared --unresolved-symbols=ignore-all \
# RUN:   %t/entry.o %t/helper.o -o %t/def.so
# RUN: llvm-readelf -S %t/def.so | FileCheck %s --check-prefix=DEFAULT
# DEFAULT:     .ejit_cross
# DEFAULT-NOT: .ejit_bitcode

## A malformed .ejit_cross must fail the link with an actionable diagnostic
## (input file + stage + underlying error), never silently fall back to AOT.
# RUN: llvm-mc -filetype=obj -triple=x86_64 %t/bad.s -o %t/bad.o
# RUN: not ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   %t/bad.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADERR
# BADERR: ejit-cross-inline: parse bitcode failed for {{.*}}bad.o

## A literal archive with .ejit_cross members is processed inline, but only for
## the members lld actually selects. entry.o is pulled in via -u; helper.o is a
## direct object. The registry materialises and the consumed sections disappear.
# RUN: llvm-ar crs %t/libcross.a %t/entry.o
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   -u ejit_entry_fn %t/libcross.a %t/helper.o -o %t/archive.so
# RUN: llvm-readelf -S %t/archive.so | FileCheck %s --check-prefix=ARCHIVE
# ARCHIVE:     .ejit_bitcode
# ARCHIVE-NOT: .ejit_cross

## An archive selected through -l is handled the same way: because cross-link
## processing now runs after symbol resolution over ctx.objectFiles, the member
## lld selects (here via -u) is discovered and its .ejit_cross is consumed. This
## previously produced a hard "unprocessed .ejit_cross" error.
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   -u ejit_entry_fn -L%t -lcross -o %t/archive-l.so
# RUN: llvm-readelf -S %t/archive-l.so | FileCheck %s --check-prefix=ARCHIVE-L
# ARCHIVE-L:     .ejit_bitcode
# ARCHIVE-L-NOT: .ejit_cross

#--- entry.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; @helper is an ordinary cross-TU helper (defined in helper.ll, no
; always_inline) that the real inliner must fold away. @some_extern_fn and
; @extern_global stay external and must be registered.
declare i32 @helper()
declare i32 @some_extern_fn(i32)
@extern_global = external global i32
@helper_table = external constant [1 x ptr]

define i32 @ejit_entry_fn() !ejit.metadata !0 {
  %h = call i32 @helper()
  %fp.addr = getelementptr [1 x ptr], ptr @helper_table, i64 0, i64 0
  %fp = load ptr, ptr %fp.addr
  %t = call i32 %fp()
  %arg = add i32 %h, %t
  %e = call i32 @some_extern_fn(i32 %arg)
  %g = load i32, ptr @extern_global
  %r = add i32 %e, %g
  ret i32 %r
}

define i32 @ejit_entry_two() !ejit.metadata !0 {
  %e = call i32 @some_extern_fn(i32 5)
  %g = load i32, ptr @extern_global
  %r = add i32 %e, %g
  ret i32 %r
}

!0 = !{!{!"ejit_entry"}}

#--- helper.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@helper_table = constant [1 x ptr] [ptr @table_target]

define i32 @helper() {
  %v = call i32 @helper_leaf()
  ret i32 %v
}

define i32 @helper_leaf() {
  ret i32 204
}

define i32 @table_target() {
  ret i32 7
}

#--- bad.s
.section .ejit_cross,"a",@progbits
.asciz "this is not a bitcode file"
