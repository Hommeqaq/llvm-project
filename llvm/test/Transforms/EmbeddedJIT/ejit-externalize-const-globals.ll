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
; name, so they never rename. Ownership: every period object — const or
; mutable — is shared host state whose live values the specialization chain
; reads through the period registry at runtime, so it externalizes like any
; other global. A kept const period definition would bake extraction-time
; values into JIT-side constant pools and let the JIT-side fold bypass the
; runtime memory read (the specialization trigger); externalized, the
; trigger load survives to be substituted from live memory.
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

; Const period array — externalizes like every other period object (dso_local
; here is clang-realistic: clang emits it on file-scope const definitions),
; keeping its original name and metadata.
@const_cells = dso_local constant [2 x i32] [i32 5, i32 6], !ejit.metadata !3

; Alias to the const period array. Pre-flip the alias survived (its root
; kept its definition); post-flip the root becomes a declaration and an
; alias may not point at one, so the alias dissolves into its aliasee and
; every use resolves through @const_cells itself.
@alias_to_const_period = alias [2 x i32], ptr @const_cells

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
  %pcc = getelementptr [2 x i32], ptr @alias_to_const_period, i32 0, i32 %i
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
; ejit_static.* key; period objects appear via the static registry table only
; (the ctor path skips period registrations — both arrays' plain-name symbol
; entries come from the .ejit_bitcode section, where they provide the
; bare-metal JIT-link fallback for the externalized declarations).
; MOD: c"ejit_static._stdin_.{{0x[0-9a-f]+}}.counter_local\00"
; MOD: c"cells\00"
; MOD: c"const_cells\00"
; MOD: define internal void @ejit_auto_register()
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @msg)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @g_ext_const)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @counter)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @counter_local)
; MOD-NOT: call void @ejit_register_symbol(ptr @{{.*}}, ptr @cells)
; MOD-NOT: call void @ejit_register_symbol(ptr @{{.*}}, ptr @const_cells)

; EXT side: the extracted bitcode declares (does not define) every
; externalized global; internal globals are renamed to their deterministic
; key and both period arrays keep their plain names as declarations. Renamed
; internal globals keep dso_local (set by their original internal linkage and
; not cleared by the external conversion), and an explicitly emitted dso_local
; (const period, clang-realistic) survives the conversion too, so data access
; stays direct ADRP+ADD — no per-global GOT entry.
; EXT: @msg = external constant
; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.tbl = external dso_local constant
; EXT: @g_ext_const = external constant
; EXT: @counter = external global
; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.counter_local = external dso_local global
; EXT: @cells = external global [2 x i32]
; EXT: @const_cells = external dso_local constant [2 x i32]

; ...and no externalized initializer survives anywhere in the extracted
; module:
; EXT-NOT: c"helloworld\00"
; EXT-NOT: [i32 10, i32 20, i32 30, i32 40]
; EXT-NOT: zeroinitializer
; EXT-NOT: [i32 5, i32 6]

; The alias rooted at the now-external const period array dissolves (an
; alias may not point at a declaration): no @alias_to_const_period survives
; and the entry's GEP resolves through @const_cells itself.
; EXT-NOT: @alias_to_const_period

; Volume accounting for the runtime's Tier-2 "rodata-extern" diagnostic:
; externalized const bytes = @msg [11 x i8] 11 + @tbl [4 x i32] 16 +
; @const_cells [2 x i32] 8 = 35B in 3 globals; nothing is kept (only
; code-address dispatch tables would keep their definitions, and this module
; has none). Mutable globals and already-external declarations are not
; counted.
; EXT: !ejit.rodata_extern = !{![[ROEXT:[0-9]+]]}
; EXT: ![[ROEXT]] = !{i64 35, i64 3, i64 0, i64 0}

!0 = !{!"ejit_entry"}
!1 = distinct !{!0}
!2 = distinct !{!{!"ejit_period_arr", !"cell", i32 0}}
!3 = distinct !{!{!"ejit_period_arr", !"ccell", i32 0}}
