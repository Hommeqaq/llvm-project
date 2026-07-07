; Default (flag OFF): the wrapper keeps the generic variable-dimension entry.
; RUN: opt -passes=ejit-wrapper-gen -S %s \
; RUN:   | FileCheck %s --check-prefix=GENERIC
; Flag ON: 0/1/2-dim entries call the matching fixed fast path; 3/4-dim entries
; keep using the generic entry (their fixed calling convention would spill args
; on aarch64, cancelling the specialized-lookup saving).
; RUN: opt -passes=ejit-wrapper-gen -ejit-wrapper-fixed-dim-entry -S %s \
; RUN:   | FileCheck %s --check-prefix=FIXED

; --- 2D entry: fixed under the flag, generic by default. ---
; GENERIC-LABEL: define i32 @two_dim_entry(
; GENERIC: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 2, ptr {{.*}}, ptr {{.*}})
; GENERIC-NOT: @ejit_taskpool_compile_or_get_2d

; FIXED-LABEL: define i32 @two_dim_entry(
; FIXED: call i32 @ejit_taskpool_compile_or_get_2d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})

define i32 @two_dim_entry(i32 %idx1, i32 %idx2) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  ret i32 0
}

; --- 3D entry: generic in BOTH modes (fixed 3D not selected by the wrapper). ---
; GENERIC-LABEL: define i32 @three_dim_entry(
; GENERIC: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 3, ptr {{.*}}, ptr {{.*}})

; FIXED-LABEL: define i32 @three_dim_entry(
; FIXED: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 3, ptr {{.*}}, ptr {{.*}})

define i32 @three_dim_entry(i32 %a, i32 %b, i32 %c) !ejit.metadata !1 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  %v3 = load i32, ptr @data3
  ret i32 0
}

; No fixed 3D/4D calls are ever emitted.
; FIXED-NOT: @ejit_taskpool_compile_or_get_3d
; FIXED-NOT: @ejit_taskpool_compile_or_get_4d

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11
@data3 = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}, !{!"ejit_period_arr_ind", !"grp", i32 2}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}
!12 = distinct !{!{!"ejit_period_arr", !"grp", i32 48}}
