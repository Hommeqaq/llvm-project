; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s

; Regression test for extern-const global registration.
;
; An ejit_entry function that reads an `extern const` global must cause that
; global to be registered with the JIT (ejit_register_symbol). extractAndSerialize
; keeps a const *declaration* (extern const, no initializer in this TU) as an
; external declaration in the extracted bitcode — it cannot be embedded, so it
; must be resolved from the host process at JIT link time. The symbol
; collectors previously skipped every const global (isConstant()), which left
; extern-const declarations unregistered and made JITLink fail with
; "Symbols not found". Only a const global *with a local definition* (embedded
; in the bitcode) should be skipped.

; extern const — the case that was broken.
@g_extern_const = external constant i32

; extern non-const — control; this one was already registered.
@g_extern_mut = external global i32

define i32 @ejit_entry_fn() !ejit.metadata !1 {
; CHECK-LABEL: define i32 @ejit_entry_fn()
  %a = load i32, ptr @g_extern_const
  %b = load i32, ptr @g_extern_mut
  %s = add i32 %a, %b
  ret i32 %s
}

; The auto-register function must emit ejit_register_symbol for BOTH externals
; referenced by the closure — including the const one.
; CHECK: define internal void @ejit_auto_register()
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@g_extern_const)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@g_extern_mut)

!0 = !{!"ejit_entry"}
!1 = !{!0}
