//===-- ejit_perf_aot_vs_jit_test.c - AOT-vs-JIT perf benchmark ------------===//
//
// Multi-core bare-metal benchmark (SRE) contrasting two ejit_entry function
// shapes to isolate when specialization wins:
//
//   perf_globals: 8 externalized non-const globals + a period-array field,
//     minimal specialization. Shows the GOT penalty: under dso_local=false
//     each global becomes a GOT-indirect load, so JIT is slower than AOT
//     (GOT overhead > specialization savings).
//
//   perf_fold: foldable branch + constant-trip loop (n=64), NO external
//     globals. Amplified specialization gain: mode and the loop bound n are
//     period-array fields -> specialized to constants, so JIT folds the
//     per-iteration branch and unrolls the loop. AOT keeps a real loop with
//     a per-iter branch and a variable bound. No GOT noise -> the delta
//     isolates the specialization gain.
//
// Bring-up order (mirrors testcase.log):
//   1. test_ejit_perf runs on one core first. ejit_init creates/elects the
//      shared compile worker.
//   2. test_ejit_perf runs on all remaining cores, excluding the worker.
//   3. Every non-worker core measures the same AOT and stable JIT workload.
//
// The function arguments are intentionally ignored. The cell index is the
// file-scope global g_ci (set by the bare-metal env before calling the
// entry), so shared-taskpool dedup/cache behavior is observable across
// cores. No main(): the RTOS calls test_ejit_perf on every core.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_bench_helpers.h"

//===-- Input: cell index set by the bare-metal env -----------------------===//
uint8_t g_ci = 0;

//===-- perf_globals: GOT-heavy, low specialization -----------------------===//
// 8 non-const globals -> externalized -> GOT loads under dso_local=false.
uint32_t g_pg0 = 1, g_pg1 = 2, g_pg2 = 3, g_pg3 = 4;
uint32_t g_pg4 = 5, g_pg5 = 6, g_pg6 = 7, g_pg7 = 8;
ejit_period_arr(pg) uint32_t g_pgPeriod[4];

#define PERF_GLOBALS_BODY(CELL_IDX, SEED)                                      \
  do {                                                                         \
    return (uint64_t)(g_pg0 + g_pg1 + g_pg2 + g_pg3 + g_pg4 + g_pg5 + g_pg6 +  \
                      g_pg7 + g_pgPeriod[(CELL_IDX)]) +                        \
           (SEED);                                                             \
  } while (0)

__attribute__((noinline)) static uint64_t
perf_globals_nojit(uint8_t cellIdx, uint32_t seed) {
  PERF_GLOBALS_BODY(cellIdx, seed);
}

ejit_entry __attribute__((noinline)) uint64_t
perf_globals_ejit(ejit_period_arr_ind(pg) uint8_t cellIdx, uint32_t seed) {
  PERF_GLOBALS_BODY(cellIdx, seed);
}

//===-- perf_fold: amplified specialization gain, no external globals -----===//
struct PfCfg {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t n;
};
ejit_period_arr(pf) struct PfCfg g_pfCfg[4];

#define PERF_FOLD_BODY(CELL_IDX, SEED)                                         \
  do {                                                                         \
    uint32_t mode = g_pfCfg[(CELL_IDX)].mode;                                  \
    uint32_t n = g_pfCfg[(CELL_IDX)].n;                                        \
    uint64_t acc = (SEED);                                                     \
    for (uint32_t k = 0; k < n; k++) {                                         \
      if (mode == 1)                                                           \
        acc = acc * 3 + k;                                                     \
      else if (mode == 2)                                                      \
        acc = acc * 5 + k;                                                     \
      else                                                                     \
        acc = acc + k;                                                         \
    }                                                                          \
    return acc;                                                                \
  } while (0)

__attribute__((noinline)) static uint64_t
perf_fold_nojit(uint8_t cellIdx, uint32_t seed) {
  PERF_FOLD_BODY(cellIdx, seed);
}

ejit_entry __attribute__((noinline)) uint64_t
perf_fold_ejit(ejit_period_arr_ind(pf) uint8_t cellIdx, uint32_t seed) {
  PERF_FOLD_BODY(cellIdx, seed);
}

//===-- Uniform bench signature + measure_batches -------------------------===//
typedef uint64_t (*bench_fn_t)(uint8_t cellIdx, uint32_t seed);

struct MeasureResult {
  uint64_t averageCyclesPerCall;
  uint64_t bestCyclesPerCall;
  uint64_t checksum;
};

static void init_data(void) {
  g_pg0 = 1;
  g_pg1 = 2;
  g_pg2 = 3;
  g_pg3 = 4;
  g_pg4 = 5;
  g_pg5 = 6;
  g_pg6 = 7;
  g_pg7 = 8;
  g_pgPeriod[0] = 100;
  g_pgPeriod[1] = 103;
  g_pgPeriod[2] = 107;
  g_pgPeriod[3] = 109;
  for (uint32_t i = 0; i < 4; ++i) {
    g_pfCfg[i].mode = 1;
    g_pfCfg[i].n = 64;
  }
}

__attribute__((noinline)) static struct MeasureResult
measure_batches(bench_fn_t fn, uint8_t cellIdx, uint32_t seedBase,
                const char *label) {
  const uint32_t core = local_core_id();
  uint64_t totalCycles = 0;
  uint64_t bestCycles = UINT64_MAX;
  uint64_t checksum = 0;

  for (uint32_t batch = 0; batch < MEASURE_BATCHES; ++batch) {
    uint64_t begin = SRE_CycleCountGet64();
    for (uint32_t call = 0; call < CALLS_PER_BATCH; ++call) {
      checksum ^= fn(cellIdx, seedBase + batch * CALLS_PER_BATCH + call);
    }
    uint64_t elapsed = SRE_CycleCountGet64() - begin;

    totalCycles += elapsed;
    if (elapsed < bestCycles)
      bestCycles = elapsed;

    /* This log is after the end timestamp and is not part of elapsed. */
    SRE_printf("[BENCH][core=%u] %-14s batch=%u/%u cycles=%llu "
               "avg=%llu/call checksum=0x%llx\n",
               core, label, batch + 1u, MEASURE_BATCHES,
               (unsigned long long)elapsed,
               (unsigned long long)(elapsed / CALLS_PER_BATCH),
               (unsigned long long)checksum);
    SRE_TaskDelay(BETWEEN_BATCH_TICKS);
  }

  const uint64_t totalCalls = (uint64_t)MEASURE_BATCHES * CALLS_PER_BATCH;
  struct MeasureResult result;
  result.averageCyclesPerCall = totalCycles / totalCalls;
  result.bestCyclesPerCall = bestCycles / CALLS_PER_BATCH;
  result.checksum = checksum;
  return result;
}

static void print_result(const char *label, const struct MeasureResult *r) {
  SRE_printf("[BENCH][core=%u] %-18s avg=%llu cycles/call "
             "best=%llu cycles/call checksum=0x%llx\n",
             local_core_id(), label,
             (unsigned long long)r->averageCyclesPerCall,
             (unsigned long long)r->bestCyclesPerCall,
             (unsigned long long)r->checksum);
}

//===-- Entry: called by the RTOS on every core ---------------------------===//
int test_ejit_perf(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint8_t cellIdx = g_ci;
  const uint32_t seed = 0x12345678u;
  const uint32_t core = local_core_id();

  SRE_printf("\n=== EJIT AOT-vs-JIT Perf Multi-Core Benchmark ===\n");
  SRE_printf("[BENCH][core=%u] enter cell=%u calls=%u batches=%u\n", core,
             cellIdx, CALLS_PER_BATCH, MEASURE_BATCHES);

  init_data();
  SRE_printf("[BENCH][core=%u] data initialized (pg globals + pgPeriod + "
             "pfCfg)\n",
             core);

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
               "skip business benchmark\n",
               core);
    idle_forever(core, "worker");
  }

  //--- perf_globals: GOT-heavy -------------------------------------------
  ejit_status_t pgRc = ejit_activate("pg", cellIdx);
  ejit_status_t pfRc = ejit_activate("pf", cellIdx);
  SRE_printf("[BENCH][core=%u] activate pg[%u] rc=%d active=%u\n", core,
             cellIdx, (int)pgRc, (unsigned)ejit_is_active("pg", cellIdx));
  SRE_printf("[BENCH][core=%u] activate pf[%u] rc=%d active=%u\n", core,
             cellIdx, (int)pfRc, (unsigned)ejit_is_active("pf", cellIdx));
  if (pgRc != EJIT_OK || pfRc != EJIT_OK)
    idle_forever(core, "activate-failed");

  SRE_printf("\n[BENCH][core=%u] --- perf_globals (8 external globals, "
             "minimal specialization) ---\n",
             core);
  uint64_t pgExpected = perf_globals_nojit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] first active call: enqueue or join pending "
             "compile\n",
             core);
  uint64_t pgFirst = perf_globals_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] pg first=0x%llx AOT=0x%llx [%s]\n", core,
             (unsigned long long)pgFirst, (unsigned long long)pgExpected,
             pgFirst == pgExpected ? "MATCH" : "MISMATCH");
  if (pgFirst != pgExpected)
    idle_forever(core, "pg-first-result-mismatch");

  SRE_printf("[BENCH][core=%u] warm-up delay=%u ticks; yielding to worker\n",
             core, WARMUP_COMPILE_DELAY_TICKS);
  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "pg-compile-timeout");

  uint64_t pgHit = perf_globals_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] pg stable hit=0x%llx expected=0x%llx [%s]\n",
             core, (unsigned long long)pgHit,
             (unsigned long long)pgExpected,
             pgHit == pgExpected ? "MATCH" : "MISMATCH");
  if (pgHit != pgExpected)
    idle_forever(core, "pg-jit-result-mismatch");

  SRE_printf("[BENCH][core=%u] measure perf_globals AOT begin\n", core);
  struct MeasureResult pgAot =
      measure_batches(perf_globals_nojit, cellIdx, seed, "PG AOT");
  SRE_printf("[BENCH][core=%u] measure perf_globals JIT begin\n", core);
  struct MeasureResult pgJit =
      measure_batches(perf_globals_ejit, cellIdx, seed, "PG JIT");
  if (pgAot.checksum != pgJit.checksum) {
    SRE_printf("[BENCH][core=%u] FAIL: pg checksum mismatch AOT=0x%llx "
               "JIT=0x%llx\n",
               core, (unsigned long long)pgAot.checksum,
               (unsigned long long)pgJit.checksum);
    idle_forever(core, "pg-checksum-mismatch");
  }

  //--- perf_fold: amplified specialization gain --------------------------
  SRE_printf("\n[BENCH][core=%u] --- perf_fold (foldable branch + n=64 loop, "
             "no external globals) ---\n",
             core);
  uint64_t pfExpected = perf_fold_nojit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] first active call: enqueue or join pending "
             "compile\n",
             core);
  uint64_t pfFirst = perf_fold_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] pf first=0x%llx AOT=0x%llx [%s]\n", core,
             (unsigned long long)pfFirst, (unsigned long long)pfExpected,
             pfFirst == pfExpected ? "MATCH" : "MISMATCH");
  if (pfFirst != pfExpected)
    idle_forever(core, "pf-first-result-mismatch");

  SRE_printf("[BENCH][core=%u] warm-up delay=%u ticks; yielding to worker\n",
             core, WARMUP_COMPILE_DELAY_TICKS);
  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "pf-compile-timeout");

  uint64_t pfHit = perf_fold_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] pf stable hit=0x%llx expected=0x%llx [%s]\n",
             core, (unsigned long long)pfHit,
             (unsigned long long)pfExpected,
             pfHit == pfExpected ? "MATCH" : "MISMATCH");
  if (pfHit != pfExpected)
    idle_forever(core, "pf-jit-result-mismatch");

  SRE_printf("[BENCH][core=%u] measure perf_fold AOT begin\n", core);
  struct MeasureResult pfAot =
      measure_batches(perf_fold_nojit, cellIdx, seed, "PF AOT");
  SRE_printf("[BENCH][core=%u] measure perf_fold JIT begin\n", core);
  struct MeasureResult pfJit =
      measure_batches(perf_fold_ejit, cellIdx, seed, "PF JIT");
  if (pfAot.checksum != pfJit.checksum) {
    SRE_printf("[BENCH][core=%u] FAIL: pf checksum mismatch AOT=0x%llx "
               "JIT=0x%llx\n",
               core, (unsigned long long)pfAot.checksum,
               (unsigned long long)pfJit.checksum);
    idle_forever(core, "pf-checksum-mismatch");
  }

  //--- Results -----------------------------------------------------------
  SRE_printf("\n--- EJIT AOT-vs-JIT Perf Results ---\n");
  print_result("perf_globals AOT", &pgAot);
  print_result("perf_globals JIT", &pgJit);
  print_result("perf_fold AOT", &pfAot);
  print_result("perf_fold JIT", &pfJit);

  SRE_printf("\nInterpretation:\n");
  SRE_printf("  perf_globals: JIT slower => GOT overhead dominates (run "
             "under dso_local=true to see it recover).\n");
  SRE_printf("  perf_fold:    JIT faster => specialization wins when "
             "hot-path work folds away and GOT/dispatch overhead is small.\n");

  ejit_taskpool_stats_t stats;
  ejit_taskpool_get_stats(&stats);
  SRE_printf("[BENCH][core=%u] stats ready=%u hits=%llu compiles=%llu "
             "enqueues=%llu pending=%u alreadyPending=%llu "
             "compileFailed=%llu publishFailed=%llu disabled=%llu\n",
             core, stats.readyEntries, (unsigned long long)stats.cacheHits,
             (unsigned long long)stats.asyncCompiles,
             (unsigned long long)stats.asyncEnqueues, stats.pendingEntries,
             (unsigned long long)stats.alreadyPending,
             (unsigned long long)stats.compileFailed,
             (unsigned long long)stats.publishFailed,
             (unsigned long long)stats.instanceDisabled);

  /* Do not tear down the process-global shared worker from a peer core. */
  idle_forever(core, "benchmark-complete");
  return 0;
}
