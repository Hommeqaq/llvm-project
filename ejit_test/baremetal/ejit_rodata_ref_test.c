//===-- ejit_rodata_ref_test.c - bare-metal rodata reference test ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Purpose: prove on the board that commit 05fa409820f8 ("Externalize const
// globals from extracted bitcode to the AOT image") works end to end. A
// JIT-specialized entry that references read-only data — a named const
// pointer, a named const array, an anonymous string literal, a const table —
// must reference the AOT image's OWN object in .rodata, not a private copy
// materialized inside the JIT object. IR-level counterpart:
// llvm/test/Transforms/EmbeddedJIT/ejit-externalize-const-globals.ll.
//
// The ownership flip (const period arrays externalize too): the probe set
// additionally carries a CONST period array (g_ro_const_cells) sharing
// period "cell" with the mutable trigger. Before the flip the extracted
// bitcode kept its definition — a JIT-side private constant pool whose
// initializer the JIT-side fold (after param substitution) baked into the
// specialized body, silently bypassing the runtime memory read. After the
// flip the module carries an external dso_local constant resolved to the
// AOT original, the param-indexed may_const read is substituted from live
// memory, and the per-compile rodata-extern accounting counts it on the
// extern side (extern=95B/5, kept=0B/0).
//
// Core falsifiable assertion — address consistency. Three entries return
// the address of a read-only object as a uint64_t; the producer compares
// each against the address of the SAME object taken in test code (same
// translation unit, first-hand AOT view; the linker does not merge plain
// .rodata arrays, so these are strictly the same objects):
//
//   jit_ro_msg_addr(CELL) == (uint64_t)g_ro_msg    // named const char *const
//   jit_ro_str_addr(CELL) == (uint64_t)g_ro_str    // named const char[11]
//   jit_ro_lit_addr(CELL) == (uint64_t)"GOODBYRE"  // anonymous literal GV
//
// Before the externalization the extracted bitcode kept every const
// definition, so JITLink materialized a private .rodata and the probes
// returned JIT-internal addresses (the assertions went red — that red was
// the pre-feature baseline this test was designed around). After it the
// bitcode declares each const under its registration key
// (ejit_static.<TU>.<hash>.<name> for internal/private constants, the
// process-unique name for external ones), the JIT linker resolves the key
// to the AOT original, and the probes must return the AOT addresses.
// jit_ro_msg_addr and jit_ro_lit_addr must also agree with each other:
// within one TU every "GOODBYRE" literal is the same anonymous GV, and
// g_ro_msg is initialized with it.
//
// Specialization trigger. Every entry takes an ejit_period_arr_ind(cell)
// index and reads a may_const field of a period array — for probes 1–5 the
// shared mutable trigger array g_rodata_ref_cells (the field is the trigger
// what the JIT specializes on, the const-data probes are the payload; after
// substitution the trigger's guard folds away, so the specialized body
// reduces to the pure probe and the returned address stays comparable), and
// for probe 6 the const period array g_ro_const_cells itself (the flip
// probe: its may_const read is trigger AND payload). Both guards compare
// against a sentinel the producer never writes; a may_const load is what
// newer toolchains require (an ejit_entry whose closure reads no may_const
// field gets the "no JIT specialization value" diagnostic), and what makes
// this a faithful production shape: a real specialized entry carries both
// a may_const branch and read-only data.
//
// Fallback assertions (correct under any implementation): a libc-free
// content check (the board has no strcmp/strlen), a const-table sum with a
// runtime index, repeated-call cache stability, and taskpool gates
// (asyncCompiles >= baseline + 6, readyEntries >= 6, compileFailed == 0,
// publishFailed == 0, queue drained). The gates prove the specializations
// exist and were published — without them a silently-failed compile would
// keep every call on the AOT path and the probes would pass vacuously.
//
// Execution evidence. None of the gates attribute the second calls: with
// the default inline cache the wrapper's hit path is one cell load plus a
// tail call and never enters the taskpool, so cacheHits cannot serve as an
// assertion. The producer therefore also calls the specializations
// directly, pinning "the JIT body executes"; the second calls above then
// exercise the wrapper (inline-cache probe) route on top of it. The
// funcIndex numbering is an internal contract — the wrapper globals
// holding the authoritative values are internal-linkage, and the values
// the name-keyed registry resolves before init do not track the post-init
// dispatch numbering (on the board the two disagreed in both registry
// configurations) — so the direct-call block is numbering-agnostic: it
// probes every index and requires the collected results to cover every
// expected body value. Only the period's dimType is captured pre-init
// (into .mc_shared, by the worker, before the first ejit_init freezes the
// registry — even an idempotent re-resolution is rejected afterwards).
//
// Probe entries are deliberately pure: no SRE_printf (or any call taking a
// string literal argument) inside an ejit_entry body — a live format
// string would keep a read-only section in the specialized graph
// regardless of the externalization. All diagnostics print from test code.
// Returns use fixed-width integers only (uint32_t/uint64_t): wrapper-gen's
// typed indirect-call forwarding is lit-covered for wide integers, and
// _Bool (i1) returns are not.
//
// Board flow (baseline path — ejit_init, not ejit_init_pgo: deterministic,
// no PGO hot threshold; mirrors ejit_bound_ptr_sre_multicore_test.c):
//   1. Reset the board and run test_ejit_period on core 6. It captures the
//      cell dimType into .mc_shared before anything can freeze it,
//      initializes EJIT, wins the pinned owner election, arms the IR+ASM
//      capture, prints "worker ready", and returns to its shell.
//   2. Run test_ejit_period on core 25. It attaches as a peer, activates
//      cell 1, sets the may_const field through the ejit_period_guard
//      setter, calls each entry once (AOT fallback + async enqueue), waits
//      for the six compiles to publish, calls each entry again (JIT
//      path), runs every assertion, and prints the PASS/FAIL summary.
//   3. Run test_ejit_rodata_ref_print on core 6. It prints the compiled
//      list, the optimized entry IR, and the full specialization module —
//      the object-level evidence: every const the entry touches appears as
//      a DECLARATION resolved to the AOT original, never a definition.
//
// Worker-core contract: llvm/CMakePresets.json pins
// EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE=6 for the aarch64_be preset, and a
// pinned build rejects any other core winning the owner election — run
// core 6 first (a peer initializing before the designated core bounded-
// spins and then fails cleanly). To run against a differently pinned
// board, rebuild with -DRODATA_REF_WORKER_CORE=<that core>. The producer
// core (25) is only a convention; any non-worker core works once the
// define is changed.
//
// Do not call ejit_shutdown(): the owner worker and attached peers must
// stay alive across the per-core shell invocations.
//
//===----------------------------------------------------------------------===//

// Self-contained for direct board integration: no project or libc headers
// are required by this file (the harness supplies the public EJIT and SRE
// symbols declared below).
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;
typedef _Bool bool;

#define true 1
#define false 0

#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x) __attribute__((ejit_dim(#x)))
// ejit_period_guard is the canonical spelling (Attr.td's first spelling;
// ejit_period_lc is an alias), matching EJitRuntime.h's EJIT_PERIOD_GUARD.
#define EJIT_PERIOD_GUARD(x) __attribute__((ejit_period_guard(#x)))
#define EJIT_ENTRY __attribute__((ejit_entry))
#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x) EJIT_DIM(x)
#define ejit_entry EJIT_ENTRY

typedef enum {
  EJIT_OK = 0,
} ejit_status_t;

typedef enum {
  EJIT_COMPILE_SYNC = 0,
  EJIT_COMPILE_ASYNC = 1,
} ejit_compile_mode_t;

typedef enum {
  EJIT_OPT_L1 = 1,
  EJIT_OPT_L2 = 2,
  EJIT_OPT_L3 = 3,
} ejit_opt_level_t;

typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  size_t maxCodeMemory;
  size_t maxDataMemory;
  size_t maxCacheEntries;
  size_t maxCacheSize;
  bool enableLogger;
  bool forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;

typedef struct {
  uint64_t cacheHits;
  uint64_t asyncCompiles;
  uint64_t asyncEnqueues;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint64_t instanceDisabledPreActivate;
  uint32_t readyEntries;
  uint32_t pendingEntries;
  uint32_t queueApproxSize;
  uint32_t reserved;
} ejit_taskpool_stats_t;

extern ejit_status_t ejit_init(const ejit_config_t *config);
extern ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
extern void ejit_clear_cache(void);
extern unsigned ejit_taskpool_pending_count(void);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern void ejit_dump_func(const char *name);
extern void ejit_print_dumped(const char *name);
extern void ejit_print_dumped_module(const char *name);
// Name-keyed registry the wrappers' auto-registration fills. Idempotent by
// name — but only BEFORE the first ejit_init freezes registration, so it is
// called exclusively by the worker-side pre-init capture (see
// capture_cell_dim), never post-init. The funcIndex registry is deliberately
// NOT used: the values readable in the pre-init window do not track the
// authoritative post-init numbering the wrappers dispatch on (the wrapper
// globals are internal-linkage), so the explicit-dispatch block is
// numbering-agnostic instead.
extern void ejit_register_lifecycle(const char *lifecycleName,
                                    uint32_t *slotOut);
extern ejit_status_t ejit_taskpool_compile_or_get_1d(uint32_t funcIndex,
                                                     uint32_t dim0,
                                                     uint32_t inst0,
                                                     void **outFn,
                                                     uint32_t *outBucket);
extern void ejit_taskpool_release_read(uint32_t bucketIndex);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#ifndef RODATA_REF_WORKER_CORE
#define RODATA_REF_WORKER_CORE 6u
#endif
#ifndef RODATA_REF_PRODUCER_CORE
#define RODATA_REF_PRODUCER_CORE 25u
#endif
#define RODATA_REF_CELL 1u
#define RODATA_REF_CELL_COUNT 8u
#define RODATA_REF_TRIGGER 0xC0u
// Sentinel the producer never writes: the may_const guard compares against
// it, so the guard folds away after specialization and the returned
// address/value stays independent of the trigger.
#define RODATA_REF_SENTINEL 0xFFFFFFFFu
#define RODATA_REF_ENTRY_COUNT 6u
// kEJitInvalidFuncIndex / kEJitInvalidDimType (EJitCommon.h): the sentinel a
// failed name-keyed registry resolution leaves in the slot.
#define RODATA_REF_INVALID_INDEX 0xFFFFFFFFu
#define RODATA_REF_WAIT_ROUNDS 6000u
#define RODATA_REF_WAIT_TICKS 10u
// The dump-armed entry: the const-period flip probe. Its specialization
// module is the print step's object-level evidence — g_ro_const_cells shows
// up as an external dso_local constant with no const definition anywhere in
// the module (and g_ro_str keeps its own evidence through the address
// assertions).
#define RODATA_REF_DUMP_ENTRY "jit_ro_const_val"

enum RodataRefStage {
  RODATA_REF_RESET = 0,
  RODATA_REF_WORKER_READY = 1,
  RODATA_REF_COMPILED = 2,
  RODATA_REF_PRINTED = 3,
};

// Cross-core state lives in the shared section (a plain .bss object is not
// guaranteed to be shared across cores). failures is written by the
// producer and re-read by the worker-side print step.
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_rodata_ref_stage;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_rodata_ref_failures;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_rodata_ref_sink;

// The period's dimType for the explicit-dispatch block, captured by the
// worker BEFORE its ejit_init: once the first init freezes registration,
// the runtime rejects every ejit_register_* call — including an idempotent
// re-resolution of an already-registered name (the worker reads the
// registry lock-free, so nothing may mutate it post-freeze). The worker
// captures in the same pre-freeze window the constructors'
// auto-registration uses; the stage protocol (WORKER_READY release store /
// producer acquire load) orders the capture before the producer reads it.
EJIT_SHARED_SECTION_ATTR uint32_t g_rodata_ref_cell_dim;

// The specialization trigger: a may_const field in the shared period
// array. Writing it requires the ejit_period_guard setter below (else
// clang warns with -Wembedded-jit).
struct RodataRefCell {
  ejit_may_const uint32_t cellType;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct RodataRefCell g_rodata_ref_cells[RODATA_REF_CELL_COUNT];

// The const period array: the ownership-flip probe. It shares period
// "cell" with the mutable trigger array above — one time-window, two arrays
// resolved by their own var names (PASS2 registers both; the registry's
// multi-array path keys each by name) — so no second dimType capture,
// activation, or dispatch dimension is needed. No shared-section attribute:
// const data belongs in the AOT image's own .rodata. Before the flip the
// extracted bitcode kept this definition (a JIT-side private constant pool,
// and the JIT-side fold after param substitution baked the extraction-time
// cell value into the specialized body); after the flip the bitcode carries
// an external dso_local constant resolved to THIS original, and the
// param-indexed may_const read is substituted from live memory.
struct RodataRefConstCell {
  ejit_may_const uint32_t value;
};

ejit_period_arr(cell) const struct RodataRefConstCell
    g_ro_const_cells[RODATA_REF_CELL_COUNT] = {
        {1000u}, {1100u}, {1200u}, {1300u},
        {1400u}, {1500u}, {1600u}, {1700u},
};

//===-- AOT-side read-only data -------------------------------------------===//

// Named read-only data. The extracted bitcode used to carry a definition
// for each of these (a private JIT-side copy); after externalization it
// carries a declaration resolved to these AOT originals.
// g_ro_msg's pointee and g_ro_str deliberately differ: the linker merges
// identical contents, and overlapping probes would weaken the assertion.
static const char *const g_ro_msg = "GOODBYRE";
static const char g_ro_str[] = "helloworld";
static const uint32_t g_ro_tbl[8] = {10u, 20u, 30u, 40u,
                                     50u, 60u, 70u, 80u};

// Byte-wise comparison, no libc (the board provides no strcmp/strlen).
static int ro_streq(const char *a, const char *b, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (a[i] != b[i])
      return 0;
  return 1;
}

// The may_const writer: only this guarded function may set cellType.
EJIT_PERIOD_GUARD(cell)
void rodata_ref_set_cell_type(ejit_period_arr_ind(cell) uint8_t cellIdx,
                              uint32_t cellType) {
  g_rodata_ref_cells[cellIdx].cellType = cellType;
}

//===-- Probe entries -----------------------------------------------------===//
//
// Probes 1–5 follow the same shape: read the may_const trigger (the
// specialization substrate — the JIT substitutes the period index, then
// folds the field load and the sentinel guard away), then perform the
// read-only-data probe (the payload this test is about). Probe 6's trigger
// IS its payload: a param-indexed may_const read of the const period array
// itself.

// Probe 1: value of the named const pointer g_ro_msg — the address of the
// "GOODBYRE" literal it points to. Exercises the externalized g_ro_msg
// declaration and, through its initializer, the pointee literal.
ejit_entry uint64_t
jit_ro_msg_addr(ejit_period_arr_ind(cell) uint8_t cellIndex) {
  if (g_rodata_ref_cells[cellIndex].cellType == RODATA_REF_SENTINEL)
    return 0u;
  return (uint64_t)g_ro_msg;
}

// Probe 2: address of the named const array g_ro_str.
ejit_entry uint64_t
jit_ro_str_addr(ejit_period_arr_ind(cell) uint8_t cellIndex) {
  if (g_rodata_ref_cells[cellIndex].cellType == RODATA_REF_SENTINEL)
    return 0u;
  return (uint64_t)g_ro_str;
}

// Probe 3: address of an anonymous string literal (private unnamed_addr
// constant) — the ejit_static.<TU>.<hash>.<name> rename path.
ejit_entry uint64_t
jit_ro_lit_addr(ejit_period_arr_ind(cell) uint8_t cellIndex) {
  if (g_rodata_ref_cells[cellIndex].cellType == RODATA_REF_SENTINEL)
    return 0u;
  return (uint64_t)"GOODBYRE";
}

// Probe 4: content check (behavioral fallback; green under any
// implementation). Hand-rolled compare: g_ro_str == "helloworld" and
// g_ro_msg -> "GOODBYRE", including the NUL terminators.
ejit_entry uint32_t
jit_ro_check_content(ejit_period_arr_ind(cell) uint8_t cellIndex) {
  if (g_rodata_ref_cells[cellIndex].cellType == RODATA_REF_SENTINEL)
    return 0u;
  if (!ro_streq(g_ro_str, "helloworld", 10u) || g_ro_str[10] != 0)
    return 0u;
  if (!ro_streq(g_ro_msg, "GOODBYRE", 8u) || g_ro_msg[8] != 0)
    return 0u;
  return 1u;
}

// Probe 5: const table lookup. The runtime index keeps the load live
// through optimization, so the specialized code reads the table (the AOT
// original after externalization) instead of a folded immediate.
ejit_entry uint32_t
jit_ro_sum_tbl(ejit_period_arr_ind(cell) uint8_t cellIndex, uint8_t i) {
  if (g_rodata_ref_cells[cellIndex].cellType == RODATA_REF_SENTINEL)
    return 0u;
  return g_ro_tbl[i & 7u] + g_ro_tbl[0];
}

// Probe 6 (the dump entry): the const period array after the ownership
// flip. The param-indexed may_const read is the trigger AND the payload:
// with the externalized declaration there is no initializer left to fold
// against, so the read survives extraction and JIT-side optimization to be
// substituted from live memory (this .rodata original) — before the flip
// the JIT-side fold against the kept definition baked the extraction-time
// value instead. The printed specialization module must show
// @g_ro_const_cells as an external dso_local constant, with no definition
// anywhere in the module.
ejit_entry uint32_t
jit_ro_const_val(ejit_period_arr_ind(cell) uint8_t cellIndex) {
  if (g_ro_const_cells[cellIndex].value == RODATA_REF_SENTINEL)
    return 0u;
  return g_ro_const_cells[cellIndex].value;
}

//===-- Assertions ----------------------------------------------------===//

#define VERIFY(cond, fmt, ...)                                                 \
  do {                                                                         \
    if (cond) {                                                                \
      SRE_printf("  OK:   " fmt "\n", ##__VA_ARGS__);                          \
    } else {                                                                   \
      SRE_printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                          \
      ++g_rodata_ref_failures;                                                 \
    }                                                                          \
  } while (0)

// Bounded wait until the worker published `target` compiles and the queue
// drained. Fails fast on compile/publish failures; prints sparsely (every
// 500 rounds) to be gentle on the SRE serial ring buffer.
static int wait_for_compiles(uint64_t target, uint32_t core) {
  for (uint32_t round = 0; round < RODATA_REF_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK) {
      SRE_printf("[RODATA-REF][core=%u] FAIL get taskpool stats\n", core);
      return 0;
    }
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[RODATA-REF][core=%u] FAIL compile=%llu publish=%llu\n",
                 core, (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= target && ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[RODATA-REF][core=%u] waiting compiles=%llu/%llu "
                 "pending=%u\n",
                 core, (unsigned long long)stats.asyncCompiles,
                 (unsigned long long)target,
                 ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(RODATA_REF_WAIT_TICKS);
  }
  SRE_printf("[RODATA-REF][core=%u] FAIL compile timeout\n", core);
  ejit_taskpool_print_stats();
  return 0;
}

// Explicit-dispatch evidence: resolve specialization \p probeIdx through
// the same taskpool entry the wrapper's slow path uses and return its
// address (null + counted failure on any miss). The caller owns the read
// token in *outBucket and must release it after the direct call — the same
// contract the generated wrapper honors.
static void *lookup_jit_fn(uint32_t probeIdx, uint32_t dim, uint32_t inst,
                           uint32_t *outBucket) {
  void *fn = 0;
  uint32_t bucket = 0;
  const ejit_status_t st =
      ejit_taskpool_compile_or_get_1d(probeIdx, dim, inst, &fn, &bucket);
  if (st != EJIT_OK || fn == 0) {
    SRE_printf("  FAIL: lookup index %u rc=%d fn=0x%llx\n", probeIdx, (int)st,
               (unsigned long long)fn);
    ++g_rodata_ref_failures;
    return 0;
  }
  *outBucket = bucket;
  return fn;
}

//===-- Roles ---------------------------------------------------===//

// Resolve the period's dimType (worker only, before its ejit_init freezes
// registration). Idempotent by name: the constructors' auto-registration
// already assigned it, so this re-resolves the same value the wrappers
// load. A capture failure is not counted here: run_worker's RESET branch
// zeroes the failure counter after this, so the producer's VERIFY on the
// captured value carries the accounting.
static void capture_cell_dim(void) {
  g_rodata_ref_cell_dim = RODATA_REF_INVALID_INDEX;
  ejit_register_lifecycle("cell", &g_rodata_ref_cell_dim);
  SRE_printf("[RODATA-REF] captured cellDim=%u\n", g_rodata_ref_cell_dim);
}

static int run_worker(void) {
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (__atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE) ==
      RODATA_REF_RESET) {
    __atomic_store_n(&g_rodata_ref_sink, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_rodata_ref_failures, 0u, __ATOMIC_RELEASE);
    // Arm the capture before any compile can happen: the worker is the
    // only core that can arm it and hold the payload.
    ejit_dump_func(RODATA_REF_DUMP_ENTRY);
    __atomic_store_n(&g_rodata_ref_stage, RODATA_REF_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[RODATA-REF][core=%u] worker ready; run test_ejit_period "
               "on core %u\n",
               core, RODATA_REF_PRODUCER_CORE);
    return 0;
  }
  SRE_printf("[RODATA-REF][core=%u] worker already initialized stage=%u; "
             "use test_ejit_rodata_ref_print\n",
             core,
             __atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE));
  return 0;
}

static int run_producer(void) {
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (__atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE) !=
      RODATA_REF_WORKER_READY) {
    SRE_printf("[RODATA-REF][core=%u] FAIL stage=%u; run core %u first\n",
               core,
               __atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE),
               RODATA_REF_WORKER_CORE);
    return -2;
  }

  if (ejit_activate("cell", RODATA_REF_CELL) != EJIT_OK) {
    SRE_printf("[RODATA-REF][core=%u] FAIL activate cell=%u\n", core,
               RODATA_REF_CELL);
    return -3;
  }
  ejit_clear_cache();
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK) {
    SRE_printf("[RODATA-REF][core=%u] FAIL baseline stats\n", core);
    return -4;
  }
  const uint64_t baseline = before.asyncCompiles;

  // Set the may_const trigger so the specialization has a reason to exist
  // (and so the guard's comparison folds to "not sentinel" at JIT time).
  rodata_ref_set_cell_type((uint8_t)RODATA_REF_CELL, RODATA_REF_TRIGGER);

  // First-hand AOT addresses of the same named objects (same translation
  // unit). Never mirror the data into second copies for comparison: the
  // linker does not merge plain .rodata arrays, and identical-content
  // literal merging is an accident, not a contract. Within this TU every
  // "GOODBYRE" literal is one anonymous GV — the one g_ro_msg points at —
  // so aot_msg and aot_lit denote the same object.
  const uint64_t aot_msg = (uint64_t)g_ro_msg;
  const uint64_t aot_str = (uint64_t)g_ro_str;
  const uint64_t aot_lit = (uint64_t)"GOODBYRE";
  SRE_printf("[RODATA-REF][core=%u] AOT baselines: msg=0x%llx str=0x%llx "
             "lit=0x%llx\n",
             core, (unsigned long long)aot_msg, (unsigned long long)aot_str,
             (unsigned long long)aot_lit);

  // --- First calls: AOT fallback bodies while the worker compiles -------
  SRE_printf("\n--- first calls (AOT fallback + async enqueue) ---\n");
  const uint8_t cell = (uint8_t)RODATA_REF_CELL;
  // First-hand view of the const period cell (same translation unit; the
  // compiler folds it to the static initializer — never mirror the data
  // into a second copy for comparison).
  const uint32_t aot_const_val = g_ro_const_cells[RODATA_REF_CELL].value;
  const uint64_t aot_path_msg = jit_ro_msg_addr(cell);
  const uint64_t aot_path_str = jit_ro_str_addr(cell);
  const uint64_t aot_path_lit = jit_ro_lit_addr(cell);
  const uint32_t aot_path_content = jit_ro_check_content(cell);
  const uint32_t aot_path_sum = jit_ro_sum_tbl(cell, 3u);
  const uint32_t aot_path_const = jit_ro_const_val(cell);

  VERIFY(aot_path_content == 1u,
         "AOT jit_ro_check_content() == 1 (got %u)", aot_path_content);
  VERIFY(aot_path_sum == 50u, "AOT jit_ro_sum_tbl(3) == 50 (got %u)",
         aot_path_sum);
  VERIFY(aot_path_const == aot_const_val,
         "AOT jit_ro_const_val() == %u (got %u)", aot_const_val,
         aot_path_const);
  VERIFY(aot_path_msg == aot_msg && aot_path_str == aot_str &&
             aot_path_lit == aot_lit,
         "AOT path sees the original addresses (msg/str/lit)");

  if (!wait_for_compiles(baseline + RODATA_REF_ENTRY_COUNT, core))
    return -5;

  // --- Second calls: JIT specializations ---------------------------------
  SRE_printf("\n--- second calls (JIT path) ---\n");
  const uint64_t jit_msg = jit_ro_msg_addr(cell);
  const uint64_t jit_str = jit_ro_str_addr(cell);
  const uint64_t jit_lit = jit_ro_lit_addr(cell);
  const uint32_t jit_content = jit_ro_check_content(cell);
  const uint32_t jit_sum = jit_ro_sum_tbl(cell, 3u);
  const uint32_t jit_const = jit_ro_const_val(cell);

  SRE_printf("  g_ro_msg: AOT=0x%llx JIT=0x%llx\n",
             (unsigned long long)aot_msg, (unsigned long long)jit_msg);
  SRE_printf("  g_ro_str: AOT=0x%llx JIT=0x%llx\n",
             (unsigned long long)aot_str, (unsigned long long)jit_str);
  SRE_printf("  literal : AOT=0x%llx JIT=0x%llx\n",
             (unsigned long long)aot_lit, (unsigned long long)jit_lit);

  // Core falsifiable assertions: the JIT references the AOT originals.
  VERIFY(jit_msg == aot_msg,
         "JIT g_ro_msg points at the AOT original (jit=0x%llx aot=0x%llx)",
         (unsigned long long)jit_msg, (unsigned long long)aot_msg);
  VERIFY(jit_str == aot_str,
         "JIT g_ro_str is the AOT original array (jit=0x%llx aot=0x%llx)",
         (unsigned long long)jit_str, (unsigned long long)aot_str);
  VERIFY(jit_lit == aot_lit,
         "JIT literal is the AOT original (jit=0x%llx aot=0x%llx)",
         (unsigned long long)jit_lit, (unsigned long long)aot_lit);
  VERIFY(jit_msg == jit_lit,
         "msg and literal probes agree (same AOT literal, 0x%llx)",
         (unsigned long long)jit_msg);

  // Behavioral fallbacks (correct with or without externalization).
  VERIFY(jit_content == 1u, "JIT jit_ro_check_content() == 1 (got %u)",
         jit_content);
  VERIFY(jit_sum == 50u, "JIT jit_ro_sum_tbl(3) == 50 (got %u)", jit_sum);
  // The const period cell through the JIT path: the value itself cannot
  // distinguish snapshot from original (a genuinely const object never
  // changes), so this pins the mechanism's result; the externality itself
  // is the print step's module evidence plus the accounting count.
  VERIFY(jit_const == aot_const_val,
         "JIT jit_ro_const_val() == %u (got %u)", aot_const_val, jit_const);

  // Repeated calls stay stable (cache-hit path).
  VERIFY(jit_ro_sum_tbl(cell, 3u) == jit_sum &&
             jit_ro_str_addr(cell) == jit_str,
         "repeated calls stable");

  // --- Direct calls over all indices: pin that the JIT bodies execute ----
  //
  // The second calls above went through the wrappers; with the default
  // inline cache the hit path never enters the taskpool, so no counter can
  // attribute them (cacheHits is deliberately not asserted). The funcIndex
  // numbering itself is an internal contract: the wrapper globals holding
  // the authoritative values are internal-linkage, and the name-keyed
  // registry values readable before init do not track the post-init
  // numbering (board runs in both registry configurations showed the
  // capture disagreeing with the dispatch numbering). The evidence
  // therefore does not depend on the numbering: probe every index, call
  // each returned specialization directly, and require the collected
  // results to cover every expected body value — each JIT body, called
  // outside any wrapper, must compute its own AOT-original result.
  //
  // The classifier below buckets on raw value equality, so the const period
  // initializers must stay distinct from the other bodies' results (1, 50)
  // and from any address (the AOT image sits far above 0x1000; the values
  // 1000–1700 hold) — a colliding initializer would miscount as "an
  // unexpected value" or silently inflate another bucket.
  //
  // All six bodies are callable through one prototype: the extra second
  // argument is ignored by the one-argument bodies (AAPCS), a w0 return
  // reads as the zero-extended x0 value, and the two address probes share
  // one anonymous literal, so their bucket counts together.
  SRE_printf("\n--- direct calls over all indices (JIT bodies) ---\n");
  {
    const uint32_t cellDim = g_rodata_ref_cell_dim;
    VERIFY(cellDim != RODATA_REF_INVALID_INDEX,
           "dimType captured for period cell (%u)", cellDim);

    uint32_t addrBodies = 0, strBodies = 0, contentBodies = 0, sumBodies = 0;
    uint32_t constBodies = 0;
    for (uint32_t idx = 0; idx < RODATA_REF_ENTRY_COUNT; ++idx) {
      uint32_t bucket = 0;
      void *fn = lookup_jit_fn(idx, cellDim, RODATA_REF_CELL, &bucket);
      if (!fn)
        continue;
      const uint64_t v = ((uint64_t (*)(uint8_t, uint8_t))fn)(cell, 3u);
      ejit_taskpool_release_read(bucket);
      if (v == aot_msg) {
        ++addrBodies; // the g_ro_msg or the literal body (same AOT literal)
      } else if (v == aot_str) {
        ++strBodies;
      } else if (v == 1u) {
        ++contentBodies;
      } else if (v == 50u) {
        ++sumBodies;
      } else if (v == aot_const_val) {
        ++constBodies;
      } else {
        VERIFY(0, "index %u body returned an unexpected value (0x%llx)", idx,
               (unsigned long long)v);
      }
    }
    VERIFY(addrBodies == 2u,
           "both address-probe bodies ran (msg/lit, %u/2)", addrBodies);
    VERIFY(strBodies == 1u, "the g_ro_str body ran (%u/1)", strBodies);
    VERIFY(contentBodies == 1u, "the content-check body ran (%u/1)",
           contentBodies);
    VERIFY(sumBodies == 1u, "the table-sum body ran (%u/1)", sumBodies);
    VERIFY(constBodies == 1u, "the const-period body ran (%u/1)", constBodies);
  }

  // Taskpool gates: the specializations exist, are published, and nothing
  // failed. They do NOT attribute the second calls (the inline-cache hit
  // path bypasses the taskpool) — the explicit-dispatch block above carries
  // that evidence; these gates rule out the vacuous pass where compiles
  // silently failed and every call stayed on the AOT path.
  ejit_taskpool_stats_t after = {0};
  (void)ejit_taskpool_get_stats(&after);
  SRE_printf("  stats: ready=%u compiles=%llu(+%llu) hits=%llu "
             "failed=%llu pending=%u\n",
             after.readyEntries, (unsigned long long)after.asyncCompiles,
             (unsigned long long)(after.asyncCompiles - baseline),
             (unsigned long long)after.cacheHits,
             (unsigned long long)after.compileFailed,
             ejit_taskpool_pending_count());
  VERIFY(after.asyncCompiles >= baseline + RODATA_REF_ENTRY_COUNT,
         "asyncCompiles >= baseline+%u", RODATA_REF_ENTRY_COUNT);
  VERIFY(after.readyEntries >= RODATA_REF_ENTRY_COUNT,
         "readyEntries >= %u (got %u)", RODATA_REF_ENTRY_COUNT,
         after.readyEntries);
  VERIFY(after.compileFailed == 0 && after.publishFailed == 0,
         "no compile/publish failures");
  VERIFY(ejit_taskpool_pending_count() == 0, "queue drained");

  __atomic_store_n(&g_rodata_ref_sink,
                   jit_msg ^ jit_str ^ jit_lit ^ (uint64_t)jit_sum ^
                       (uint64_t)jit_const,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_rodata_ref_stage, RODATA_REF_COMPILED,
                   __ATOMIC_RELEASE);

  if (g_rodata_ref_failures != 0) {
    SRE_printf("[RODATA-REF][core=%u] FAIL: %u assertion(s)\n", core,
               g_rodata_ref_failures);
    return -6;
  }
  SRE_printf("[RODATA-REF][core=%u] PASS: JIT references the AOT rodata "
             "originals; run test_ejit_rodata_ref_print on core %u\n",
             core, RODATA_REF_WORKER_CORE);
  return 0;
}

//===-- Shell entries -----------------------------------------------------===//

int test_ejit_rodata_ref_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != RODATA_REF_WORKER_CORE) {
    SRE_printf("[RODATA-REF][core=%u] FAIL: print is worker-local; run on "
               "core %u\n",
               core, RODATA_REF_WORKER_CORE);
    return -7;
  }

  const uint32_t stage =
      __atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE);
  if (stage < RODATA_REF_COMPILED) {
    SRE_printf("[RODATA-REF][core=%u] FAIL stage=%u; run test_ejit_period "
               "on core %u first\n",
               core, stage, RODATA_REF_PRODUCER_CORE);
    return -8;
  }
  if (stage == RODATA_REF_PRINTED) {
    SRE_printf("[RODATA-REF][core=%u] dump already printed\n", core);
    return 0;
  }

  // Final sanity from the worker side: queue drained, nothing failed.
  ejit_taskpool_stats_t final = {0};
  if (ejit_taskpool_get_stats(&final) != EJIT_OK || final.compileFailed ||
      final.publishFailed || ejit_taskpool_pending_count() != 0) {
    SRE_printf("[RODATA-REF][core=%u] FAIL final taskpool state\n", core);
    ejit_taskpool_print_stats();
    return -9;
  }

  // Stop further captures, then print the object-level evidence.
  ejit_dump_func("");
  SRE_printf("\n[RODATA-REF] === COMPILED VERSIONS ===\n");
  ejit_taskpool_print_compiled();
  SRE_printf("\n[RODATA-REF] === OPTIMIZED %s ===\n", RODATA_REF_DUMP_ENTRY);
  ejit_print_dumped(RODATA_REF_DUMP_ENTRY);
  SRE_printf("\n[RODATA-REF] === SPECIALIZATION MODULE ===\n");
  ejit_print_dumped_module(RODATA_REF_DUMP_ENTRY);
  SRE_printf("[RODATA-REF][core=%u] expect: %s references g_ro_const_cells "
             "as an external dso_local constant DECLARATION (the const "
             "period array keeps no definition in the module); the resolve "
             "binds it to the AOT .rodata original, and the per-compile "
             "rodata-extern accounting must count 5 externalized consts "
             "(extern=95B/5) with nothing kept\n",
             core, RODATA_REF_DUMP_ENTRY);
  SRE_printf("[RODATA-REF][core=%u] PASS sink=0x%llx failures=%u\n", core,
             (unsigned long long)__atomic_load_n(&g_rodata_ref_sink,
                                                 __ATOMIC_ACQUIRE),
             __atomic_load_n(&g_rodata_ref_failures, __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_rodata_ref_stage, RODATA_REF_PRINTED,
                   __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT rodata reference test (core=%u) ===\n", core);
  call_init_array_functions();

  // The worker captures the period's dimType before its ejit_init freezes
  // registration (the producer attaches after the freeze is already in
  // place, so it can only read the captured value).
  if (core == RODATA_REF_WORKER_CORE &&
      __atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE) ==
          RODATA_REF_RESET)
    capture_cell_dim();

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  // forceStaticRegistry keeps its default (false): the constructor
  // registration path. This file runs call_init_array_functions() on every
  // core, so the registrations exist before init, and the producer's
  // post-freeze constructor calls are harmlessly rejected (the shared
  // objects are already registered).
  const ejit_status_t rc = ejit_init(&cfg);
  const uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[RODATA-REF][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_rodata_ref_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != RODATA_REF_WORKER_CORE) {
    SRE_printf("[RODATA-REF][core=%u] FAIL worker=%u; reset and run core "
               "%u first\n",
               core, worker, RODATA_REF_WORKER_CORE);
    return -2;
  }

  if (core == RODATA_REF_WORKER_CORE)
    return run_worker();
  if (core == RODATA_REF_PRODUCER_CORE)
    return run_producer();

  SRE_printf("[RODATA-REF][core=%u] skip: use cores %u and %u\n", core,
             RODATA_REF_WORKER_CORE, RODATA_REF_PRODUCER_CORE);
  return 0;
}
