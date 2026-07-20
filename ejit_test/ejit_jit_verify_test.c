//===-- ejit_jit_verify_test.c - JIT compile/cache/recompile correctness ---===//
//
// Multi-core bare-metal correctness demo (SRE) for the EJIT taskpool pipeline.
// Verifies that async JIT compilation actually happens, that the cache serves
// the second call, and that deactivate/reactivate with a changed may_const
// field triggers a fresh compile. Pure correctness - no perf measurement.
//
// Verification steps (each gated on wait_for_compile so the result check is on
// the JIT, not the AOT fallback returned by the first async call):
//   1. cell cellIdx (cellType=0xFD): first call enqueues compile; after the
//      worker publishes, the stable JIT hit returns 1000, asyncCompiles>=1,
//      and cacheHits>=1 (the second call is a hit).
//   2. trp trpIdx (trpType=1, multi-dim): first call enqueues an independent
//      compile; stable JIT hit returns 777 (cellType=0xFD && trpType==1),
//      asyncCompiles increased again.
//   3. cell ci2 (cellType=0xEC): a second cellIdx with a different cellType
//      triggers a new specialization; stable JIT hit returns 999,
//      asyncCompiles increased again.
//   4. deactivate/reactivate cellIdx with cellType changed 0xFD->0xEC: forces
//      a recompile; stable JIT hit returns 999, asyncCompiles increased.
//
// Bring-up order (mirrors testcase.log):
//   1. test_ejit_jit_verify runs on one core first. ejit_init creates/elects
//      the shared compile worker.
//   2. test_ejit_jit_verify runs on all remaining cores, excluding the worker.
//   3. Every non-worker core runs the verification loop.
//
// The function arguments are intentionally ignored. The cell indices are the
// file-scope globals g_ci (primary cellIdx, 0..15), g_ti (trpIdx, 0..7), and
// g_ci2 (second cellIdx for the recompile test, 0..15), set by the bare-metal
// env before calling the entry. No main(): the RTOS calls test_ejit_jit_verify
// on every core.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_bench_helpers.h"

//===-- Inputs: indices set by the bare-metal env -------------------------===//
uint8_t g_ci = 0;   // primary cellIdx, 0..15
uint8_t g_ti = 0;   // trpIdx, 0..7
uint8_t g_ci2 = 0;  // second cellIdx for the recompile test, 0..15

//===-- EJIT-attributed config structs and period arrays ------------------===//

struct CellCfg {
  ejit_may_const uint32_t cellType;
  ejit_may_const uint32_t cellId;
  uint32_t trafficLoad;
};

struct TrpCfg {
  ejit_may_const uint32_t trpType;
  uint32_t activeBeams;
};

#define N_CELL 16
#define M_TRP 8

ejit_period_arr(cell) struct CellCfg g_cellCfg[N_CELL];
ejit_period_arr(trp) struct TrpCfg g_trpCfg[M_TRP];

//===-- JIT entry functions (verification targets) ------------------------===//
// The branches depend on ejit_may_const fields -> the JIT specializes them to
// constants and folds the branch. Each (cellIdx[, trpIdx]) pair that is
// activated produces an independent specialization.

// Single-dim: branch on cellType. cellType==0xFD -> 1000, else 999.
ejit_entry __attribute__((noinline)) uint32_t
jit_cell_check_ejit(ejit_period_arr_ind(cell) uint8_t cellIdx) {
  if (g_cellCfg[cellIdx].cellType == 0xFDu)
    return 1000u;
  else
    return 999u;
}

// Multi-dim: compound condition on cellType + trpType.
// ct==0xFD && tt==1 -> 777; ct==0xEC && tt==2 -> 888; else 0.
ejit_entry __attribute__((noinline)) uint32_t
jit_cell_trp_check_ejit(ejit_period_arr_ind(cell) uint8_t cellIdx,
                        ejit_period_arr_ind(trp) uint8_t trpIdx) {
  uint32_t ct = g_cellCfg[cellIdx].cellType;
  uint32_t tt = g_trpCfg[trpIdx].trpType;

  if (ct == 0xFDu && tt == 1u)      return 777u;
  else if (ct == 0xECu && tt == 2u) return 888u;
  return 0u;
}

//===-- VERIFY macro (SRE_printf, tracks local `failures`) ----------------===//
// NOTE: references the local `core` and `failures` variables; only use inside
// the entry function where both are in scope.
#define VERIFY(cond, fmt, ...) do {                                    \
  if (!(cond)) {                                                        \
    SRE_printf("[BENCH][core=%u] FAIL: " fmt "\n", core, ##__VA_ARGS__); \
    failures++;                                                         \
  } else {                                                              \
    SRE_printf("[BENCH][core=%u] OK:   " fmt "\n", core, ##__VA_ARGS__); \
  }                                                                     \
} while (0)

static void init_data(void) {
  // Primary cell: cellType=0xFD -> jit_cell_check_ejit returns 1000.
  g_cellCfg[g_ci].cellType = 0xFDu;
  g_cellCfg[g_ci].cellId = 42u;
  g_cellCfg[g_ci].trafficLoad = 0u;
  // Multi-dim trp: trpType=1 -> jit_cell_trp_check_ejit returns 777 (with
  // cellType=0xFD on the primary cell).
  g_trpCfg[g_ti].trpType = 1u;
  g_trpCfg[g_ti].activeBeams = 0u;
  // ci2's cellType is set in step 3 (0xEC for the recompile test); zero it
  // here so the initial state is deterministic.
  g_cellCfg[g_ci2].cellType = 0u;
  g_cellCfg[g_ci2].cellId = 0u;
  g_cellCfg[g_ci2].trafficLoad = 0u;
}

//===-- Entry: called by the RTOS on every core ---------------------------===//
int test_ejit_jit_verify(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint8_t cellIdx = g_ci;
  const uint8_t trpIdx = g_ti;
  const uint8_t ci2 = g_ci2;
  const uint32_t core = local_core_id();
  uint32_t failures = 0;

  SRE_printf("\n=== EJIT JIT Verify Multi-Core Correctness Demo ===\n");
  SRE_printf("[BENCH][core=%u] enter cell=%u trp=%u ci2=%u\n", core, cellIdx,
             trpIdx, ci2);

  init_data();
  SRE_printf("[BENCH][core=%u] data initialized (cellCfg + trpCfg)\n", core);

  SRE_printf("[BENCH][core=%u] call_init_array_functions begin\n", core);
  call_init_array_functions();
  SRE_printf("[BENCH][core=%u] call_init_array_functions end\n", core);

  ejit_config_t config = {
      .compileMode = EJIT_COMPILE_ASYNC,
      .optLevel = EJIT_OPT_L2,
      .enableLogger = false,
      .forceStaticRegistry = true,
  };

  SRE_printf("[BENCH][core=%u] ejit_init begin mode=ASYNC opt=L2 "
             "staticRegistry=1\n",
             core);
  ejit_status_t initRc = ejit_init(&config);
  uint32_t workerCore = ejit_taskpool_get_worker_core();
  SRE_printf("[BENCH][core=%u] ejit_init end rc=%d workerCore=%u\n", core,
             (int)initRc, workerCore);
  if (initRc != EJIT_OK)
    idle_forever(core, "init-failed");

  if (core == workerCore) {
    SRE_printf("[BENCH][core=%u] this is the compile-worker core; "
               "skip verification\n",
               core);
    idle_forever(core, "worker");
  }

  //--- Step 1: cell cellIdx compile + cache hit ---------------------------
  // cellType=0xFD -> expect 1000. First call returns AOT fallback and
  // dispatches the async compile; the second call (after wait_for_compile)
  // is a stable JIT cache hit.
  SRE_printf("\n[BENCH][core=%u] --- Step 1: cell[%u] compile + cache hit "
             "(cellType=0xFD, expect 1000) ---\n",
             core, cellIdx);

  ejit_status_t cellRc = ejit_activate("cell", cellIdx);
  SRE_printf("[BENCH][core=%u] activate cell[%u] rc=%d active=%u\n", core,
             cellIdx, (int)cellRc,
             (unsigned)ejit_is_active("cell", cellIdx));
  if (cellRc != EJIT_OK)
    idle_forever(core, "activate-cell-failed");

  ejit_taskpool_stats_t s0;
  ejit_taskpool_get_stats(&s0);
  SRE_printf("[BENCH][core=%u] baseline stats: ready=%u hits=%llu "
             "compiles=%llu\n",
             core, s0.readyEntries, (unsigned long long)s0.cacheHits,
             (unsigned long long)s0.asyncCompiles);

  SRE_printf("[BENCH][core=%u] first active call: enqueue or join pending "
             "compile\n",
             core);
  uint32_t r1First = jit_cell_check_ejit(cellIdx);
  SRE_printf("[BENCH][core=%u] cell first=%u (AOT fallback, expect 1000)\n",
             core, r1First);

  SRE_printf("[BENCH][core=%u] warm-up delay=%u ticks; yielding to worker\n",
             core, WARMUP_COMPILE_DELAY_TICKS);
  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "cell-compile-timeout");

  uint32_t r1Hit = jit_cell_check_ejit(cellIdx);
  SRE_printf("[BENCH][core=%u] cell stable hit=%u (JIT, expect 1000)\n", core,
             r1Hit);
  VERIFY(r1Hit == 1000u, "step1 jit_cell_check_ejit(%u)=%u (expected 1000)",
         cellIdx, r1Hit);

  ejit_taskpool_stats_t s1;
  ejit_taskpool_get_stats(&s1);
  SRE_printf("[BENCH][core=%u] step1 stats: ready=%u hits=%llu "
             "compiles=%llu compileFailed=%llu\n",
             core, s1.readyEntries, (unsigned long long)s1.cacheHits,
             (unsigned long long)s1.asyncCompiles,
             (unsigned long long)s1.compileFailed);
  VERIFY(s1.asyncCompiles >= s0.asyncCompiles + 1u,
         "step1 JIT compiles increased (before=%llu after=%llu)",
         (unsigned long long)s0.asyncCompiles,
         (unsigned long long)s1.asyncCompiles);
  VERIFY(s1.cacheHits >= 1u, "step1 cacheHits>=1 (actual %llu)",
         (unsigned long long)s1.cacheHits);

  //--- Step 2: multi-dim trp compile (independent specialization) --------
  // trpType=1, cellType=0xFD -> expect 777. An independent JIT entry is
  // produced for the (cellIdx, trpIdx) pair.
  SRE_printf("\n[BENCH][core=%u] --- Step 2: trp[%u] multi-dim compile "
             "(trpType=1, expect 777) ---\n",
             core, trpIdx);

  ejit_status_t trpRc = ejit_activate("trp", trpIdx);
  SRE_printf("[BENCH][core=%u] activate trp[%u] rc=%d active=%u\n", core,
             trpIdx, (int)trpRc,
             (unsigned)ejit_is_active("trp", trpIdx));
  if (trpRc != EJIT_OK)
    idle_forever(core, "activate-trp-failed");

  SRE_printf("[BENCH][core=%u] first multi-dim call: enqueue or join pending "
             "compile\n",
             core);
  uint32_t r2First = jit_cell_trp_check_ejit(cellIdx, trpIdx);
  SRE_printf("[BENCH][core=%u] trp first=%u (AOT fallback, expect 777)\n", core,
             r2First);

  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "trp-compile-timeout");

  uint32_t r2Hit = jit_cell_trp_check_ejit(cellIdx, trpIdx);
  SRE_printf("[BENCH][core=%u] trp stable hit=%u (JIT, expect 777)\n", core,
             r2Hit);
  VERIFY(r2Hit == 777u,
         "step2 jit_cell_trp_check_ejit(%u,%u)=%u (expected 777)",
         cellIdx, trpIdx, r2Hit);

  ejit_taskpool_stats_t s2;
  ejit_taskpool_get_stats(&s2);
  SRE_printf("[BENCH][core=%u] step2 stats: ready=%u hits=%llu "
             "compiles=%llu\n",
             core, s2.readyEntries, (unsigned long long)s2.cacheHits,
             (unsigned long long)s2.asyncCompiles);
  VERIFY(s2.asyncCompiles >= s1.asyncCompiles + 1u,
         "step2 JIT compiles increased (before=%llu after=%llu)",
         (unsigned long long)s1.asyncCompiles,
         (unsigned long long)s2.asyncCompiles);

  //--- Step 3: recompile with a second cellIdx (different cellType) ------
  // cellType=0xEC -> expect 999. A different cellIdx with a different
  // may_const value produces a fresh specialization.
  SRE_printf("\n[BENCH][core=%u] --- Step 3: cell[%u] recompile "
             "(cellType=0xEC, expect 999) ---\n",
             core, ci2);

  g_cellCfg[ci2].cellType = 0xECu;
  ejit_status_t cell2Rc = ejit_activate("cell", ci2);
  SRE_printf("[BENCH][core=%u] activate cell[%u] rc=%d active=%u\n", core, ci2,
             (int)cell2Rc, (unsigned)ejit_is_active("cell", ci2));
  if (cell2Rc != EJIT_OK)
    idle_forever(core, "activate-cell2-failed");

  SRE_printf("[BENCH][core=%u] first cell2 call: enqueue or join pending "
             "compile\n",
             core);
  uint32_t r3First = jit_cell_check_ejit(ci2);
  SRE_printf("[BENCH][core=%u] cell2 first=%u (AOT fallback, expect 999)\n",
             core, r3First);

  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "cell2-compile-timeout");

  uint32_t r3Hit = jit_cell_check_ejit(ci2);
  SRE_printf("[BENCH][core=%u] cell2 stable hit=%u (JIT, expect 999)\n", core,
             r3Hit);
  VERIFY(r3Hit == 999u, "step3 jit_cell_check_ejit(%u)=%u (expected 999)",
         ci2, r3Hit);

  ejit_taskpool_stats_t s3;
  ejit_taskpool_get_stats(&s3);
  SRE_printf("[BENCH][core=%u] step3 stats: ready=%u hits=%llu "
             "compiles=%llu\n",
             core, s3.readyEntries, (unsigned long long)s3.cacheHits,
             (unsigned long long)s3.asyncCompiles);
  VERIFY(s3.asyncCompiles >= s2.asyncCompiles + 1u,
         "step3 JIT compiles increased (before=%llu after=%llu)",
         (unsigned long long)s2.asyncCompiles,
         (unsigned long long)s3.asyncCompiles);

  //--- Step 4: deactivate/reactivate with changed cellType -> recompile --
  // Change cellType 0xFD->0xEC on the primary cellIdx, then deactivate and
  // reactivate. The stale specialization is invalidated and a fresh compile
  // is dispatched; the stable JIT hit now returns 999.
  SRE_printf("\n[BENCH][core=%u] --- Step 4: cell[%u] deactivate/reactivate "
             "(cellType 0xFD->0xEC, expect 999) ---\n",
             core, cellIdx);

  ejit_status_t deactRc = ejit_deactivate("cell", cellIdx);
  SRE_printf("[BENCH][core=%u] deactivate cell[%u] rc=%d active=%u\n", core,
             cellIdx, (int)deactRc,
             (unsigned)ejit_is_active("cell", cellIdx));

  g_cellCfg[cellIdx].cellType = 0xECu;

  ejit_status_t reactRc = ejit_activate("cell", cellIdx);
  SRE_printf("[BENCH][core=%u] reactivate cell[%u] rc=%d active=%u\n", core,
             cellIdx, (int)reactRc,
             (unsigned)ejit_is_active("cell", cellIdx));
  if (reactRc != EJIT_OK)
    idle_forever(core, "reactivate-cell-failed");

  SRE_printf("[BENCH][core=%u] first reactivated call: enqueue or join pending "
             "compile\n",
             core);
  uint32_t r4First = jit_cell_check_ejit(cellIdx);
  SRE_printf("[BENCH][core=%u] reactivated first=%u (AOT fallback, expect "
             "999)\n",
             core, r4First);

  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "reactivate-compile-timeout");

  uint32_t r4Hit = jit_cell_check_ejit(cellIdx);
  SRE_printf("[BENCH][core=%u] reactivated stable hit=%u (JIT, expect 999)\n",
             core, r4Hit);
  VERIFY(r4Hit == 999u,
         "step4 jit_cell_check_ejit(%u) after type change=%u (expected 999)",
         cellIdx, r4Hit);

  ejit_taskpool_stats_t s4;
  ejit_taskpool_get_stats(&s4);
  SRE_printf("[BENCH][core=%u] step4 stats: ready=%u hits=%llu "
             "compiles=%llu\n",
             core, s4.readyEntries, (unsigned long long)s4.cacheHits,
             (unsigned long long)s4.asyncCompiles);
  VERIFY(s4.asyncCompiles > s3.asyncCompiles,
         "step4 recompile after deactivate/reactivate (before=%llu after=%llu)",
         (unsigned long long)s3.asyncCompiles,
         (unsigned long long)s4.asyncCompiles);

  //--- Summary -----------------------------------------------------------
  SRE_printf("\n--- EJIT JIT Verify Results ---\n");
  SRE_printf("[BENCH][core=%u] final stats ready=%u hits=%llu compiles=%llu "
             "enqueues=%llu pending=%u alreadyPending=%llu "
             "compileFailed=%llu publishFailed=%llu disabled=%llu\n",
             core, s4.readyEntries, (unsigned long long)s4.cacheHits,
             (unsigned long long)s4.asyncCompiles,
             (unsigned long long)s4.asyncEnqueues, s4.pendingEntries,
             (unsigned long long)s4.alreadyPending,
             (unsigned long long)s4.compileFailed,
             (unsigned long long)s4.publishFailed,
             (unsigned long long)s4.instanceDisabled);

  if (failures == 0u) {
    SRE_printf("[BENCH][core=%u] PASS: all JIT verify checks passed\n", core);
  } else {
    SRE_printf("[BENCH][core=%u] FAIL: %u verification(s) failed\n", core,
               failures);
  }

  /* Do not tear down the process-global shared worker from a peer core. */
  idle_forever(core, "benchmark-complete");
  return 0;
}
