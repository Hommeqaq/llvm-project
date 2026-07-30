# REQUIRES: x86
## Archive member selection for --ejit-cross-inline. Cross-TU processing runs
## after symbol resolution over ctx.objectFiles, so only the archive members lld
## actually selects are merged and registered. This covers two regressions:
##   1. a plain cross object listed *before* an archive must not stop the scan of
##      later archive members. The previous pre-parse scanner used a single
##      global "HasCross" flag to break out of the per-archive member loop, so
##      once any earlier object had set it, only the first member of a following
##      archive was inspected -- an entry in a later member was missed and the
##      link failed with "no ejit_entry".
##   2. an unselected entry member (even one whose symbols would conflict) must
##      never enter the composite or the .ejit_bitcode registry.

# RUN: rm -rf %t && split-file %s %t

## helperx and both entry TUs carry .ejit_cross; plain does not.
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/helperx.ll -o %t/helperx.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/helperx.bc -o %t/helperx.o
# RUN: llc -filetype=obj -relocation-model=pic %t/plain.ll -o %t/plain.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/entryA.ll -o %t/entryA.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/entryA.bc -o %t/entryA.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/entryB.ll -o %t/entryB.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/entryB.bc -o %t/entryB.o

## Finding 1: a plain cross object (helperx.o, no entry) precedes an archive
## whose *second* member (entryA.o) holds the selected entry; the first member
## (plain.o) has no .ejit_cross. entryA is pulled via -u. The link must succeed
## with exactly one registry and no leftover .ejit_cross.
# RUN: llvm-ar crs %t/liblate.a %t/plain.o %t/entryA.o
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --save-temps -u ejit_entry_A %t/helperx.o %t/liblate.a -o %t/late.so
# RUN: llvm-readelf -S %t/late.so | FileCheck %s --check-prefix=LATE
# LATE:     .ejit_bitcode
# LATE-NOT: .ejit_cross
## The selected entry is registered; the missed-member regression would have
## produced no per-entry bitcode at all.
# RUN: ls %t/late.so.ejit-cross.ejit_entry_A.bc

## Finding 2: an archive holds two entry members that both define @gDup (a hard
## conflict if both were merged). Only ejit_entry_A is selected, so only it is
## registered and the unselected conflicting member is ignored.
# RUN: llvm-ar crs %t/libtwo.a %t/entryA.o %t/entryB.o
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --save-temps -u ejit_entry_A %t/libtwo.a %t/helperx.o -o %t/sel.so
# RUN: llvm-readelf -S %t/sel.so | FileCheck %s --check-prefix=SEL
# SEL:     .ejit_bitcode
# SEL-NOT: .ejit_cross
# RUN: ls %t/sel.so.ejit-cross.ejit_entry_A.bc
# RUN: not ls %t/sel.so.ejit-cross.ejit_entry_B.bc

## Input order must not change the result: swapping the archive and the direct
## object still registers only the selected entry.
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --save-temps -u ejit_entry_A %t/helperx.o %t/libtwo.a -o %t/sel2.so
# RUN: ls %t/sel2.so.ejit-cross.ejit_entry_A.bc
# RUN: not ls %t/sel2.so.ejit-cross.ejit_entry_B.bc

## A registry can expose an archive dependency that the optimized native object
## no longer has. Model that legitimate pre-O2/post-O2 difference explicitly:
## late-entry.o has no native reference to late_helper, while its raw
## .ejit_cross bitcode does. The provisional registry therefore selects
## helper-late.o from the archive. Cross-link must iterate, consume the newly
## selected member's .ejit_cross, and emit one final registry whose entry
## contains the helper definition.
# RUN: llvm-as %t/late-entry-cross.ll -o %t/late-entry-cross.bc
# RUN: llvm-mc -filetype=obj -triple=x86_64 %t/late-entry-native.s \
# RUN:   -o %t/late-entry-native.o
# RUN: llvm-objcopy --add-section=.ejit_cross=%t/late-entry-cross.bc \
# RUN:   --set-section-flags=.ejit_cross=alloc,readonly \
# RUN:   %t/late-entry-native.o %t/late-entry.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/helper-late.ll -o %t/helper-late.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/helper-late.bc \
# RUN:   -o %t/helper-late.o
# RUN: llvm-ar crs %t/liblatehelper.a %t/helper-late.o
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --save-temps %t/late-entry.o %t/liblatehelper.a -o %t/late-helper.so
# RUN: llvm-readelf -S %t/late-helper.so \
# RUN:   | FileCheck %s --check-prefix=LATE-HELPER
# LATE-HELPER:     .ejit_bitcode
# LATE-HELPER-NOT: .ejit_cross
# RUN: llvm-dis %t/late-helper.so.ejit-cross.late_entry.bc -o - \
# RUN:   | FileCheck %s --check-prefix=LATE-IR
# LATE-IR: define{{.*}} i32 @late_entry
# LATE-IR: define{{.*}} i32 @late_helper

#--- helperx.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @helperx_fn(i32 %x) noinline {
  ret i32 %x
}

#--- plain.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @plain_fn() {
  ret i32 0
}

#--- entryA.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @ext_sink(i32)
@gDup = global i32 1

define i32 @ejit_entry_A() !ejit.metadata !0 {
  %v = load i32, ptr @gDup
  %r = call i32 @ext_sink(i32 %v)
  ret i32 %r
}

!ejit.metadata = !{}
!0 = !{!{!"ejit_entry"}}

#--- late-entry-cross.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @late_helper()

define i32 @late_entry() !ejit.metadata !0 {
  %v = call i32 @late_helper()
  ret i32 %v
}

!ejit.metadata = !{}
!0 = !{!{!"ejit_entry"}}

#--- late-entry-native.s
.text
.globl late_entry
.type late_entry,@function
late_entry:
  ret

#--- helper-late.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @late_helper() noinline {
  ret i32 99
}

#--- entryB.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @ext_sink(i32)
@gDup = global i32 2

define i32 @ejit_entry_B() !ejit.metadata !0 {
  %v = load i32, ptr @gDup
  %r = call i32 @ext_sink(i32 %v)
  ret i32 %r
}

!ejit.metadata = !{}
!0 = !{!{!"ejit_entry"}}
