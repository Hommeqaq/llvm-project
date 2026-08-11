; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s
;
; Regression test: hidden-visibility external declarations must still be
; registered via ejit_register_symbol, and their visibility must be cleared
; in the extracted bitcode so JITLink can resolve them against the
; userSymbols map. Hidden visibility symbols are not exported from the AOT
; binary's dynamic symbol table; if preserved in the extracted bitcode,
; JITLink refuses to resolve them against externally-supplied absolute
; symbols, causing "Symbols not found" errors.
;
; The visibility clearing itself happens inside extractAndSerialize (the
; serialized bitcode opaque blob), so this test verifies the registration
; side — hidden symbols must appear in the auto-register function.

; Hidden-visibility external function — the case that was broken.
declare hidden void @hidden_func()
declare hidden i32 @hidden_func_ret()

; Default-visibility external function — control; should always work.
declare void @default_func()

; Hidden-visibility external global
@hidden_glob = external hidden global i32
; Default-visibility external global
@default_glob = external global i32

define i32 @ejit_entry_hidden() !ejit.metadata !1 {
; CHECK-LABEL: define i32 @ejit_entry_hidden()
  call void @hidden_func()
  %v = call i32 @hidden_func_ret()
  ret i32 %v
}

define i32 @ejit_entry_mixed() !ejit.metadata !2 {
; CHECK-LABEL: define i32 @ejit_entry_mixed()
  call void @default_func()
  call void @hidden_func()
  %a = load i32, ptr @hidden_glob
  %b = load i32, ptr @default_glob
  %s = add i32 %a, %b
  ret i32 %s
}

; The auto-register function must emit ejit_register_symbol for ALL
; externals referenced by the closure — including hidden-visibility ones.
; CHECK: define internal void @ejit_auto_register()
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@hidden_func)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@hidden_func_ret)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@default_func)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@hidden_glob)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@default_glob)

!0 = !{!"ejit_entry"}
!1 = !{!0}
!2 = !{!0}
