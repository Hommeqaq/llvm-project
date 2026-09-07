; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.mod.ll
; RUN: opt -passes=verify -disable-output %t-dump/*.bc
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
; RUN: FileCheck %s --check-prefix=MOD < %t.mod.ll
;
; An alias rooted at an EXTERNALLY linked const global must be dissolved in
; the extracted clone before the table becomes a declaration. The external
; constant keeps its already process-unique name, so the extracted
; declaration, the AOT alias, and both registration paths all agree on it.

@msg = constant [8 x i8] c"GOODBYR\00", align 1
@msg_alias = alias [8 x i8], ptr @msg

define i32 @entry(i32 %i) !ejit.metadata !0 {
  %i64 = zext i32 %i to i64
  %p = getelementptr [8 x i8], ptr @msg_alias, i64 0, i64 %i64
  %v = load i8, ptr %p
  %r = zext i8 %v to i32
  ret i32 %r
}

; EXT: @msg = external constant [8 x i8]
; EXT-NOT: msg_alias
; EXT: getelementptr [8 x i8], ptr @msg, i64 0, i64 %
; EXT-NOT: c"GOODBYR\00"

; MOD: c"msg\00"
; MOD: @msg_alias = alias [8 x i8], ptr @msg
; MOD: call void @ejit_register_symbol(ptr @{{.*}}, ptr @msg)

!0 = distinct !{!{!"ejit_entry"}}
