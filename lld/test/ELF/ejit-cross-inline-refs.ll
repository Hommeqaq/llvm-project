# REQUIRES: x86
## Reference-form coverage for the selective (closure-only) per-entry clone.
## The closure walk must keep every statically reachable definition an entry
## needs, drop everything else, and preserve alias/ifunc/const/mutable/metadata
## semantics. The internal verifyModule (run per entry) fails the link if the
## clone ever drops a still-referenced definition, so a successful link plus the
## disassembly checks below pin down the behaviour.

# RUN: rm -rf %t && split-file %s %t
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/entry.ll -o %t/entry.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/entry.bc -o %t/entry.o
# RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline \
# RUN:   %t/helper.ll -o %t/helper.bc
# RUN: llc -filetype=obj -relocation-model=pic %t/helper.bc -o %t/helper.o

# RUN: ld.lld --ejit-cross-inline -shared --unresolved-symbols=ignore-all \
# RUN:   --save-temps %t/entry.o %t/helper.o -o %t/out.so
# RUN: llvm-readelf -S %t/out.so | FileCheck %s --check-prefix=SECTION
# SECTION: .ejit_bitcode

## Entry one pulls in the full variety of reference forms. The internal
## verifyModule (per entry) already guarantees no still-referenced definition
## was dropped; these checks pin down the surviving symbols. O2 may devirtualise
## a constant function-pointer load into a direct call, so we assert on the call
## targets that must survive rather than on the (foldable) pointer tables. The
## dead helper and the externally visible dead alias must never leak in.
# RUN: llvm-dis %t/out.so.ejit-cross.ejit_entry_one.bc -o - \
# RUN:   | FileCheck %s --check-prefix=ONE \
# RUN:       --implicit-check-not=dead_helper --implicit-check-not=dead_alias
# ONE-DAG: define{{.*}} i32 @ejit_entry_one
## cross-TU helper and a helper reached only through invoke (a CallBase) with a
## personality function both survive.
# ONE-DAG: define internal i32 @shared_helper
# ONE-DAG: define{{.*}} i32 @cross_helper
# ONE-DAG: define internal i32 @invoke_helper{{.*}}personality
## call targets reached via a function-pointer table, a constant aggregate and a
## bitcast/const-expr pointer.
# ONE-DAG: define internal i32 @tab_a
# ONE-DAG: define internal i32 @agg_target
# ONE-DAG: define internal i32 @ce_target
## a referenced alias stays an alias; the referenced ifunc stays a valid ifunc
## with its resolver definition.
# ONE-DAG: @live_alias = internal alias
# ONE-DAG: @my_ifunc = {{.*}}ifunc i32 (i32), ptr @ifunc_resolver
# ONE-DAG: define internal i32 @alias_target
# ONE-DAG: define internal{{.*}} ptr @ifunc_resolver
## a mutable global and the may_const/period global are externalised
## (declaration, no initializer); the external function/global stay declarations
## so the JIT registry can resolve them.
# ONE-DAG: @mut = external{{.*}}global i32
# ONE-DAG: @period = external{{.*}}global [4 x i32]
# ONE-DAG: declare i32 @ext_func
# ONE-DAG: @ext_global = external global

## Entry two only calls the shared helper, so its module contains that helper
## but none of entry one's private closure.
# RUN: llvm-dis %t/out.so.ejit-cross.ejit_entry_two.bc -o - \
# RUN:   | FileCheck %s --check-prefix=TWO \
# RUN:       --implicit-check-not=tab_a --implicit-check-not=agg_target \
# RUN:       --implicit-check-not=live_alias --implicit-check-not=my_ifunc \
# RUN:       --implicit-check-not=invoke_helper --implicit-check-not=dead_helper
# TWO-DAG: define{{.*}} i32 @ejit_entry_two
# TWO-DAG: define internal i32 @shared_helper

#--- entry.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @ext_func(i32)
declare i32 @__gxx_personality_v0(...)
@ext_global = external global i32

; Cross-TU helper defined in helper.ll.
declare i32 @cross_helper(i32)

; Function-pointer table (const global, initializer holds function addresses).
@fptr_table = internal constant [2 x ptr] [ptr @tab_a, ptr @tab_b]
; Function address embedded in a constant aggregate (struct).
@agg = internal constant { ptr, i32 } { ptr @agg_target, i32 3 }
; A const global that is kept verbatim.
@const_tab = internal constant [3 x i32] [i32 1, i32 2, i32 3]
; A mutable global -> externalised by the extractor.
@mut = internal global i32 5
; A period/may_const global carrying ejit metadata.
@period = internal global [4 x i32] zeroinitializer, !ejit.metadata !20
; const-expr (bitcast) callee.
@ce_ptr = internal constant ptr bitcast (i32 (i32)* @ce_target to ptr)

; A referenced alias (kept as an alias) and an unreferenced, externally visible
; alias (must be dropped from every per-entry module).
@live_alias = internal alias i32 (i32), ptr @alias_target
@dead_alias = alias i32 (i32), ptr @alias_target

; An ifunc that entry one calls.
@my_ifunc = ifunc i32 (i32), ptr @ifunc_resolver

define internal i32 @tab_a() noinline { ret i32 1 }
define internal i32 @tab_b() noinline { ret i32 2 }
define internal i32 @agg_target() noinline { ret i32 7 }
define internal i32 @ce_target(i32 %x) noinline { ret i32 %x }
define internal i32 @alias_target(i32 %x) noinline { ret i32 %x }

define internal ptr @ifunc_resolver() {
  ret ptr @ifunc_impl
}
define internal i32 @ifunc_impl(i32 %x) noinline { ret i32 %x }

; Shared helper used by both entries.
define internal i32 @shared_helper(i32 %x) noinline {
  %e = call i32 @ext_func(i32 %x)
  ret i32 %e
}

; Helper reached only through an invoke (CallBase) with a personality.
define internal i32 @invoke_helper(i32 %x) noinline personality ptr @__gxx_personality_v0 {
  %r = invoke i32 @ext_func(i32 %x) to label %ok unwind label %bad
ok:
  ret i32 %r
bad:
  %lp = landingpad { ptr, i32 } cleanup
  ret i32 0
}

; Never referenced by an entry: must not appear in any per-entry module.
define internal i32 @dead_helper() noinline { ret i32 999 }

define i32 @ejit_entry_one(i32 %x) !ejit.metadata !0 {
  %h = call i32 @shared_helper(i32 %x)
  %c = call i32 @cross_helper(i32 %x)
  %iv = call i32 @invoke_helper(i32 %x)
  %p0 = getelementptr [2 x ptr], ptr @fptr_table, i64 0, i64 0
  %f0 = load ptr, ptr %p0
  %t = call i32 %f0()
  %ap = getelementptr { ptr, i32 }, ptr @agg, i64 0, i32 0
  %af = load ptr, ptr %ap
  %a = call i32 %af()
  %cef = load ptr, ptr @ce_ptr
  %ce = call i32 %cef(i32 %x)
  %al = call i32 @live_alias(i32 %x)
  %if = call i32 @my_ifunc(i32 %x)
  %m = load i32, ptr @mut
  %eg = load i32, ptr @ext_global
  %ct0 = getelementptr [3 x i32], ptr @const_tab, i64 0, i64 0
  %ctv = load i32, ptr %ct0
  %pp = getelementptr [4 x i32], ptr @period, i64 0, i64 0
  %pv = load i32, ptr %pp
  %s1 = add i32 %h, %c
  %s2 = add i32 %s1, %iv
  %s3 = add i32 %s2, %t
  %s4 = add i32 %s3, %a
  %s5 = add i32 %s4, %ce
  %s6 = add i32 %s5, %al
  %s7 = add i32 %s6, %if
  %s8 = add i32 %s7, %m
  %s9 = add i32 %s8, %eg
  %s10 = add i32 %s9, %ctv
  %s11 = add i32 %s10, %pv
  ret i32 %s11
}

define i32 @ejit_entry_two(i32 %x) !ejit.metadata !0 {
  %h = call i32 @shared_helper(i32 %x)
  ret i32 %h
}

!ejit.metadata = !{}
!0 = !{!{!"ejit_entry"}}
!20 = !{!{!"ejit_may_const_field", i64 0}}

#--- helper.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @cross_helper(i32 %x) noinline {
  %r = mul i32 %x, 3
  ret i32 %r
}
