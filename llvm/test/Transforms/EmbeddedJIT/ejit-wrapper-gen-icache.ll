; -ejit-inline-cache (ON): emits an inline probe DIRECTLY in the ejit_entry
; wrapper - a load atomic of the per-function @__ejit_icache_fn_<name> slot +
; null-check; the hit path (jit_icache_dispatch) calls the cached specialization
; directly with NO ejit_icache_try call and NO ejit_taskpool_release_read. The
; probe takes no dims, so a hit pays no dim loads. Default (OFF): the original
; 4-block wrapper, no icache. Idempotent: running the pass twice does not
; duplicate the probe. With -ejit-wrapper-timing the icache hit path is
; instrumented (trace_now + trace_wrapper, sentinel status).

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s --check-prefix=ICACHE
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=NOICACHE
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s --check-prefix=IDEM
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-wrapper-timing -S %s | FileCheck %s --check-prefix=TIMING

; --- 1D entry: inline probe = load atomic @__ejit_icache_fn + null-check; hit
; --- calls the cached ptr directly (NO release_read). NO ejit_icache_try call. ---
; ICACHE-LABEL: define i32 @one_dim_entry(
; ICACHE-NOT: ejit_icache_try
; ICACHE: load atomic ptr, ptr @__ejit_icache_fn_one_dim_entry acquire, align 8
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE: call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 3D entry: same probe shape (load atomic, no dims in the probe). ---
; ICACHE-LABEL: define i32 @three_dim_entry(
; ICACHE-NOT: ejit_icache_try
; ICACHE: load atomic ptr, ptr @__ejit_icache_fn_three_dim_entry acquire, align 8

; --- Default (flag OFF): no icache anywhere; original compile_or_get path. ---
; NOICACHE-LABEL: define i32 @one_dim_entry(
; NOICACHE-NOT: ejit_icache_try
; NOICACHE-NOT: __ejit_icache_fn
; NOICACHE-NOT: ejit_register_icache_slot
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_1d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; NOICACHE-NOT: ejit_icache_try

; --- Idempotent: two passes emit the probe exactly once. ---
; IDEM-LABEL: define i32 @one_dim_entry(
; IDEM-COUNT-1: load atomic ptr, ptr @__ejit_icache_fn_one_dim_entry acquire, align 8

; --- Timing: the icache hit path is instrumented (trace_now + trace_wrapper). ---
; TIMING-LABEL: define i32 @one_dim_entry(
; TIMING: call i64 @ejit_taskpool_trace_now()
; TIMING: load atomic ptr, ptr @__ejit_icache_fn_one_dim_entry acquire, align 8
; TIMING: call i64 @ejit_taskpool_trace_now()
; TIMING-LABEL: jit_icache_dispatch:
; TIMING: call i64 @ejit_taskpool_trace_now()
; TIMING: call void @ejit_taskpool_trace_wrapper(i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, i32 {{.*}}, i64 {{.*}}, i64 {{.*}}, i64 {{.*}}, i64 {{.*}})
; TIMING-NOT: call void @ejit_taskpool_release_read
; TIMING: ret

define i32 @one_dim_entry(i32 %idx1) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @three_dim_entry(i32 %a, i32 %b, i32 %c) !ejit.metadata !1 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  %v3 = load i32, ptr @data3
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11
@data3 = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}, !{!"ejit_period_arr_ind", !"grp", i32 2}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}
!12 = distinct !{!{!"ejit_period_arr", !"grp", i32 48}}
