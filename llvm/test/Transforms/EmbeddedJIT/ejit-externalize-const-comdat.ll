; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.mod.ll
; RUN: opt -passes=verify -disable-output %t-dump/*.bc
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
; RUN: FileCheck %s --check-prefix=MOD < %t.mod.ll
;
; A const global in a COMDAT that the ownership rule externalizes must drop
; the COMDAT membership: "Declaration may not be in a Comdat". The
; linkonce_odr table keeps its (linker-unique) name — the extracted
; declaration, the registration, and the AOT original all agree on it — and
; an alias sharing the COMDAT is dissolved before the table converts.
;
; A section attribute on the converted declaration is legal IR and is left
; in place: a declaration is never allocated, so the section is inert.
;
; The alias itself cannot join the COMDAT: the LL grammar for aliases does
; not accept a comdat() property (unlike global variables), so the alias
; simply references the COMDAT member. Dissolution does not care about
; membership — it dissolves any alias whose root is about to be externalized.

$tbl = comdat any
@tbl = linkonce_odr constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat($tbl), section ".rodata.tbl"
@tbl_alias = linkonce_odr alias [4 x i32], ptr @tbl

define i32 @entry(i32 %i) !ejit.metadata !0 {
  %i64 = zext i32 %i to i64
  %p = getelementptr [4 x i32], ptr @tbl_alias, i64 0, i64 %i64
  %v = load i32, ptr %p
  ret i32 %v
}

; EXT: @tbl = external constant [4 x i32]
; EXT-NOT: , comdat($tbl)
; EXT-NOT: tbl_alias
; EXT-NOT: [i32 1, i32 2, i32 3, i32 4]

; MOD: c"tbl\00"
; MOD: @tbl_alias = linkonce_odr alias [4 x i32], ptr @tbl
; MOD: call void @ejit_register_symbol(ptr @{{.*}}, ptr @tbl)

!0 = distinct !{!{!"ejit_entry"}}
