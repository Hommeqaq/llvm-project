; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck --check-prefix=EXTRACTED %s
;
; This test verifies that non-ejit_entry function definitions in the extracted
; bitcode are internalized (set to InternalLinkage) at AOT time, before
; serialization.  This prevents the JIT's IRMaterializationUnit from claiming
; them in MR->getSymbols(), avoiding spurious "Missing definitions" errors
; when the JIT-side runInterproceduralPropagation later internalizes them
; inside the transform callback (after the MU claim set is already fixed).
;
; Entry functions that carry !ejit.metadata !{!"ejit_entry"} must keep their
; external linkage so the JIT can look them up.

; A helper function that the entry calls — should be internalized in the
; extracted bitcode.
define i32 @helper_add(i32 %a, i32 %b) {
  %sum = add i32 %a, %b
  ret i32 %sum
}

; A second helper called by the first helper — transitive internalization.
define i32 @helper_mul(i32 %a, i32 %b) {
  %prod = mul i32 %a, %b
  ret i32 %prod
}

; The entry function — must NOT be internalized.
define i32 @ejit_entry_calc(i32 %x) !ejit.metadata !2 {
  %a = call i32 @helper_add(i32 %x, i32 1)
  %b = call i32 @helper_mul(i32 %a, i32 2)
  ret i32 %b
}

; A declaration (no body) — must NOT be internalized (already a declaration).
declare void @external_lib_func()

; Entry function that calls an external declaration.
define void @ejit_entry_ext() !ejit.metadata !3 {
  call void @external_lib_func()
  ret void
}

; Check that helpers become internal in the extracted bitcode.
; EXTRACTED: define internal i32 @helper_add(
; EXTRACTED: define internal i32 @helper_mul(

; Entry functions must keep their original linkage (not internal).
; EXTRACTED: define{{.*}} i32 @ejit_entry_calc(
; EXTRACTED-NOT: define internal {{.*}}@ejit_entry_calc
; EXTRACTED: declare{{.*}} void @external_lib_func()
; EXTRACTED: define{{.*}} void @ejit_entry_ext(
; EXTRACTED-NOT: define internal {{.*}}@ejit_entry_ext

!0 = !{!"ejit_entry"}
!1 = !{!0}
!2 = !{!0}
!3 = !{!0}
