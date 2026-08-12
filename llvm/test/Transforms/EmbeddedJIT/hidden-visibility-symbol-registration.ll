; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck --check-prefix=EXTRACTED %s
; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck --check-prefix=REG %s
;
; This test verifies two things:
;
;  1. Hidden-visibility external declarations referenced by ejit_entry
;     functions still get ejit_register_symbol calls (REG check — the
;     registration side). No regression from existing behaviour.
;
;  2. In the *extracted bitcode* (the module that the JIT actually
;     compiles), hidden/protected visibility on declarations is cleared
;     to DefaultVisibility. This is the actual fix — without it JITLink
;     refuses to resolve hidden symbols against the userSymbols map.
;     The EXTRACTED checks run against the bitcode dumped by
;     -ejit-dump-bitcode-dir.

; Hidden-visibility external function
declare hidden void @hidden_func()
; Protected-visibility external function
declare protected void @protected_func()
; Default-visibility external function — control
declare void @default_func()
; Hidden-visibility external global
@hidden_glob = external hidden global i32
; Default-visibility external global
@default_glob = external global i32

define i32 @ejit_entry_hidden() !ejit.metadata !1 {
; REG-LABEL: define i32 @ejit_entry_hidden()
  call void @hidden_func()
  call void @protected_func()
  ret i32 0
}

define i32 @ejit_entry_mixed() !ejit.metadata !2 {
; REG-LABEL: define i32 @ejit_entry_mixed()
  call void @default_func()
  call void @hidden_func()
  %a = load i32, ptr @hidden_glob
  %b = load i32, ptr @default_glob
  %s = add i32 %a, %b
  ret i32 %s
}

; Registration-side: all externals (including hidden) must be registered.
; REG: define internal void @ejit_auto_register()
; REG-DAG: call void @ejit_register_symbol({{.*}}@hidden_func)
; REG-DAG: call void @ejit_register_symbol({{.*}}@protected_func)
; REG-DAG: call void @ejit_register_symbol({{.*}}@default_func)
; REG-DAG: call void @ejit_register_symbol({{.*}}@hidden_glob)
; REG-DAG: call void @ejit_register_symbol({{.*}}@default_glob)

; Extracted-bitcode side: all declarations must have default visibility.
; The exact spelling may include dso_local (X86 default) — use DAG to
; allow any interleaving.
; EXTRACTED-DAG: declare{{.*}} void @hidden_func()
; EXTRACTED-DAG: declare{{.*}} void @protected_func()
; EXTRACTED-DAG: declare void @default_func()
; EXTRACTED-DAG: @hidden_glob = external{{.*}} global i32
; EXTRACTED-DAG: @default_glob = external global i32
; No "hidden" or "protected" keyword on any declaration.
; EXTRACTED-NOT: declare hidden
; EXTRACTED-NOT: declare protected
; EXTRACTED-NOT: external hidden
; EXTRACTED-NOT: external protected

!0 = !{!"ejit_entry"}
!1 = !{!0}
!2 = !{!0}
