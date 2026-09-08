; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.mod.ll
; RUN: opt -passes=verify -disable-output %t-dump/*.bc
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
; RUN: FileCheck %s --check-prefix=MOD < %t.mod.ll
;
; An alias rooted at an INTERNAL const global that the ownership rule
; externalizes must be dissolved in the extracted clone: an alias may not
; point at a declaration ("Alias must point to a definition"). The use sites
; keep the exact aliasee expression (here a non-zero constant GEP, through an
; alias chain), the internal table is renamed to its deterministic
; ejit_static.* key, the AOT-side alias is untouched, and the extracted
; module passes the verifier.
;
; The runtime index keeps the load from folding at preOpt, so the GEP shape
; is asserted in both debug (no-op preOpt) and release (NDEBUG) opt builds.

@ro = internal constant [4 x i32] [i32 1, i32 2, i32 3, i32 4]
@ro_alias = internal alias [4 x i32], getelementptr inbounds ([4 x i32], ptr @ro, i64 0, i64 2)
@ro_alias_chain = internal alias [4 x i32], ptr @ro_alias

define i32 @entry(i32 %i) !ejit.metadata !0 {
  %i64 = zext i32 %i to i64
  %p = getelementptr [4 x i32], ptr @ro_alias_chain, i64 0, i64 %i64
  %v = load i32, ptr %p
  ret i32 %v
}

; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.ro = external dso_local constant [4 x i32]
; EXT-NOT: ro_alias
; EXT: getelementptr inbounds ([4 x i32], ptr @ejit_static._stdin_.{{0x[0-9a-f]+}}.ro, i64 0, i64 2)
; EXT-NOT: [i32 1, i32 2, i32 3, i32 4]

; MOD: c"ejit_static._stdin_.{{0x[0-9a-f]+}}.ro\00"
; MOD: @ro_alias = internal alias [4 x i32], getelementptr inbounds ([4 x i32], ptr @ro, i64 0, i64 2)
; MOD: @ro_alias_chain = internal alias [4 x i32], ptr @ro_alias
; MOD: call void @ejit_register_symbol(ptr @{{.*}}, ptr @ro)

!0 = distinct !{!{!"ejit_entry"}}
