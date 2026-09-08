; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.out
; RUN: FileCheck %s --check-prefix=MOD < %t.out
; RUN: opt -passes=verify -disable-output %t-dump/*.bc
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
;
; Globals are externalized out of the extracted bitcode.
;
; A const definition kept in the extracted bitcode materializes as JIT-side
; .rodata: every adrp+ldr that survives pre-optimization reads a private
; copy instead of the AOT original. extractAndSerialize therefore turns every
; surviving const definition into an external declaration, and both
; registration emitters record it so the JIT linker resolves the name to the
; AOT image's own rodata. Internal globals (static const tables, private
; string literals — and static mutable state, which shares the same flat
; symbol table) are renamed to their deterministic ejit_static.* key —
; module-local names are not process-unique and the runtime's flat symbol
; table would collide across TUs. Externally linked globals keep their name.
;
; Period objects follow the registry name contract: PASS2 registers them
; under their ORIGINAL name and the JIT optimizer looks arrays up by that
; name, so they never rename. Ownership: a mutable period object is shared
; host state — it externalizes like any other mutable global (a JIT-private
; copy would silently diverge from the AOT/shared object), while a CONST
; period object is the specialization array itself and keeps its definition.
;
; NOTE: the EXT assertions assume preOptimizeBitcode actually RUNS: it is
; #ifdef NDEBUG-guarded (cyclic link dependency in debug/shared builds), so
; a debug opt is a no-op there - the entry body then still folds nothing,
; but the externalization below does not depend on preopt. Run the EJIT lit
; suite with a release/NDEBUG opt (see CLAUDE.md's note on debug builds).
;
; NOTE: the pass mutates only its extraction clone; the -S output (MOD) is
; the ORIGINAL module plus the embedded bitcode, the ejit_auto_register
; calls, and the .ejit_bitcode registry section.

; Externally linked const — registered under its own (unique) name.
@msg = constant [11 x i8] c"helloworld\00", align 1

; Internal const array — renamed to the deterministic key.
@tbl = internal constant [4 x i32] [i32 10, i32 20, i32 30, i32 40]

; Extern const (declaration in this TU) — was already registered before
; const externalization; must stay registered.
@g_ext_const = external constant i32

; External mutable global — externalized under its own name, as before.
@counter = global i32 0

; Internal mutable global — the same rename rule as internal constants:
; the bitcode declaration AND the registration must both use the
; ejit_static.* key, or the JIT link leaves an unresolved external under
; the original name.
@counter_local = internal global i32 7

; Mutable period array — externalizes (shared host state) but keeps its
; original name (period registry contract).
@cells = global [2 x i32] zeroinitializer, !ejit.metadata !2

; Const period array — the specialization array itself: keeps its
; definition, name and metadata.
@const_cells = constant [2 x i32] [i32 5, i32 6], !ejit.metadata !3

define i32 @entry(i32 %i) !ejit.metadata !1 {
entry:
  %pi = getelementptr [11 x i8], ptr @msg, i32 0, i32 %i
  %c = load i8, ptr %pi
  %ci = zext i8 %c to i32
  %pt = getelementptr [4 x i32], ptr @tbl, i32 0, i32 %i
  %v = load i32, ptr %pt
  %e = load i32, ptr @g_ext_const
  %n = load i32, ptr @counter
  %nl = load i32, ptr @counter_local
  %pc = getelementptr [2 x i32], ptr @cells, i32 0, i32 %i
  %p = load i32, ptr %pc
  %pcc = getelementptr [2 x i32], ptr @const_cells, i32 0, i32 %i
  %q = load i32, ptr %pcc
  %s1 = add i32 %n, %ci
  %s2 = add i32 %s1, %v
  %s3 = add i32 %s2, %e
  %s4 = add i32 %s3, %nl
  %s5 = add i32 %s4, %p
  %s6 = add i32 %s5, %q
  store i32 %s6, ptr @counter
  store i32 %s6, ptr @cells
  ret i32 %s6
}

; MOD side: the registration strings (globals print before functions), then
; the ctor registration calls. Internal globals register under their
; ejit_static.* key; the period array only appears via the static registry
; table (the ctor path skips period definitions).
; MOD: c"ejit_static._stdin_.{{0x[0-9a-f]+}}.counter_local\00"
; MOD: c"cells\00"
; MOD: define internal void @ejit_auto_register()
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @msg)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @g_ext_const)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @counter)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @counter_local)
; MOD-NOT: call void @ejit_register_symbol(ptr @{{.*}}, ptr @cells)

; EXT side: the extracted bitcode declares (does not define) every
; externalized global; internal globals are renamed to their deterministic
; key, the mutable period array keeps its plain name as a declaration, and
; the const period array keeps its definition. Renamed internal globals keep
; dso_local (set by their original internal linkage and not cleared by the
; external conversion) so data access stays direct ADRP+ADD — no per-global
; GOT entry.
; EXT: @msg = external constant
; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.tbl = external dso_local constant
; EXT: @g_ext_const = external constant
; EXT: @counter = external global
; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.counter_local = external dso_local global
; EXT: @cells = external global [2 x i32]
; EXT: @const_cells = constant [2 x i32] [i32 5, i32 6]

; ...and no externalized initializer survives anywhere in the extracted
; module:
; EXT-NOT: c"helloworld\00"
; EXT-NOT: [i32 10, i32 20, i32 30, i32 40]
; EXT-NOT: zeroinitializer

!0 = !{!"ejit_entry"}
!1 = distinct !{!0}
!2 = distinct !{!{!"ejit_period_arr", !"cell", i32 0}}
!3 = distinct !{!{!"ejit_period_arr", !"ccell", i32 0}}
