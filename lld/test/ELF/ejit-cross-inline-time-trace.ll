# REQUIRES: x86
## The EJIT cross-TU inline link path is instrumented with TimeTraceScopes so
## its cost can be attributed per stage. The scopes must be:
##   * emitted only under --time-trace (never in a default link), and
##   * present for every major stage of the merge/extract pipeline.
## This also doubles as the smallest repeatable input for the cross-link
## benchmark (lld/utils/ejit-cross-link-bench.py generates larger variants).

# RUN: rm -rf %t && split-file %s %t

# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/a.ll -o %t/a.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/a.bc -o %t/a.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/b.ll -o %t/b.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/b.bc -o %t/b.o

## With --time-trace the per-stage scopes are recorded in the trace file. A
## fine granularity is required because the sub-stages of a tiny link are far
## below the default 500us reporting threshold. Two inputs are linked so the
## module-merge stage is exercised too.
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --time-trace=%t/trace.json --time-trace-granularity=0 \
# RUN:   %t/a.o %t/b.o -o %t/out.so
# RUN: FileCheck %s --check-prefix=TRACE --input-file=%t/trace.json
# TRACE-DAG: "name":"EJitCross:DetectScan"
# TRACE-DAG: "name":"EJitCross:ParseBitcode"
# TRACE-DAG: "name":"EJitCross:MergeModule"
# TRACE-DAG: "name":"EJitCross:EntryScan"
# TRACE-DAG: "name":"EJitCross:UnionClosure"
# TRACE-DAG: "name":"EJitCross:Inliner"
# TRACE-DAG: "name":"EJitCross:PerEntryExtraction"
# TRACE-DAG: "name":"EJitCross:PerEntryClosure"
# TRACE-DAG: "name":"EJitCross:PerEntryClone"
# TRACE-DAG: "name":"EJitCross:PerEntryTrim"
# TRACE-DAG: "name":"EJitCross:PerEntryVerify"
# TRACE-DAG: "name":"EJitCross:PerEntrySerialize"
# TRACE-DAG: "name":"EJitCross:RegistryGen"

## A default link (no --time-trace) writes no trace file at all. When time
## tracing is disabled each scope adds only the standard TimeTraceScope
## inactive-profiler branch and emits no records/output. Assert both that the
## link succeeds and that no trace file is produced (lld only writes
## <output>.time-trace when --time-trace is passed).
# RUN: rm -f %t/out2.so.time-trace
# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   %t/a.o %t/b.o -o %t/out2.so
# RUN: llvm-readelf -S %t/out2.so | FileCheck %s --check-prefix=NOTRACE
# RUN: not ls %t/out2.so.time-trace
# NOTRACE: .ejit_bitcode

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @sink(i32)
declare i32 @helper_b()

define internal i32 @helper_a(i32 %x) noinline {
  %r = call i32 @sink(i32 %x)
  ret i32 %r
}

define i32 @ejit_entry_a(i32 %x) !ejit.metadata !0 {
  %h = call i32 @helper_a(i32 %x)
  %b = call i32 @helper_b()
  %r = add i32 %h, %b
  ret i32 %r
}

define i32 @ejit_entry_b(i32 %x) !ejit.metadata !0 {
  %h = call i32 @helper_a(i32 %x)
  %r = add i32 %h, 1
  ret i32 %r
}

!ejit.metadata = !{}
!0 = !{!{!"ejit_entry"}}

#--- b.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @helper_b() noinline {
  ret i32 7
}
