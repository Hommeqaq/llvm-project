//===-- ejit_got_probe_test.c - dso_local / GOT verification demo ---------===//
//
// Multi-core bare-metal demo (SRE) for the dso_local experiment (commit
// 33754cfdc82e: the #if 0 that disables dso_local clearing in
// EJitOrcEngine::loadBitcodeModule). Verifies three things:
//
//   1. No relocation overflow (slab in PC-relative reach):
//      got_probe reads 6 externalized non-const globals. With dso_local=true
//      the JIT codegen emits ADRP+LDR for them; if the JIT slab is within
//      adrp range (±4GiB) the compile succeeds and the result is correct. A
//      relocation overflow (slab too far) fails the compile -> async
//      compileFailed>0 -> Step 1 PASS/FAIL reports FAIL.
//
//   2. GOT eliminated (asm dump):
//      ejit_dump_func("got_probe_ejit") BEFORE the first compile enables asm
//      capture; ejit_print_dumped("got_probe_ejit") prints the specialized
//      asm. Inspect it for ":got:" relocations. Expect ~6 with
//      dso_local=false (old), 0 with dso_local=true (the #if 0 experiment).
//
//   3. Perf (indicative):
//      AOT-vs-JIT per-call delta via measure_batches. The signal is small for
//      only 6 globals - treat as indicative. For a strong signal use the
//      product DlschCcScheduler (~31 GOT loads) via the run15.log --wrap
//      cycle harness.
//
// Bring-up order (mirrors testcase.log):
//   1. test_ejit_got_probe runs on one core first. ejit_init creates/elects
//      the shared compile worker.
//   2. test_ejit_got_probe runs on all remaining cores, excluding the
//      worker.
//   3. Every non-worker core runs the dump + measure loop.
//
// The function arguments are intentionally ignored. The cell index is the
// file-scope global g_ci (set by the bare-metal env before calling the
// entry). No main(): the RTOS calls test_ejit_got_probe on every core.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_bench_helpers.h"

//===-- Input: cell index set by the bare-metal env -----------------------===//
uint8_t g_ci = 0;

//===-- 6 externalized non-const globals ----------------------------------===//
// Non-const global definitions referenced by an ejit_entry are externalized by
// EJitRegisterBitcode (initializer dropped, ExternalLinkage) and resolved from
// the host at JIT link time. Under dso_local=false each access becomes a
// GOT-indirect load (:got:); under dso_local=true each is a direct ADRP+LDR.
uint32_t g_probeA = 10;
uint32_t g_probeB = 20;
uint32_t g_probeC = 30;
uint32_t g_probeD = 40;
uint32_t g_probeE = 50;
uint32_t g_probeF = 60;

// A period array drives specialization (and ensures JIT compilation). Its
// element is specialized to a constant -> NOT a GOT candidate.
ejit_period_arr(probe) uint32_t g_probeArr[4];

//===-- JIT entry: touches all 6 external globals -------------------------===//

#define GOT_PROBE_BODY(CELL_IDX, SEED)                                         \
  do {                                                                         \
    uint32_t s =                                                               \
        g_probeA + g_probeB + g_probeC + g_probeD + g_probeE + g_probeF;       \
    return (uint64_t)(s + g_probeArr[(CELL_IDX)]) + (SEED);                    \
  } while (0)

__attribute__((noinline)) static uint64_t
got_probe_nojit(uint8_t cellIdx, uint32_t seed) {
  GOT_PROBE_BODY(cellIdx, seed);
}

ejit_entry __attribute__((noinline)) uint64_t
got_probe_ejit(ejit_period_arr_ind(probe) uint8_t cellIdx, uint32_t seed) {
  GOT_PROBE_BODY(cellIdx, seed);
}

//===-- Uniform bench signature + measure_batches -------------------------===//
typedef uint64_t (*bench_fn_t)(uint8_t cellIdx, uint32_t seed);

struct MeasureResult {
  uint64_t averageCyclesPerCall;
  uint64_t bestCyclesPerCall;
  uint64_t checksum;
};

static void init_data(void) {
  g_probeA = 10;
  g_probeB = 20;
  g_probeC = 30;
  g_probeD = 40;
  g_probeE = 50;
  g_probeF = 60;
  g_probeArr[0] = 7;
  g_probeArr[1] = 11;
  g_probeArr[2] = 13;
  g_probeArr[3] = 17;
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
int test_ejit_got_probe(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint8_t cellIdx = g_ci;
  const uint32_t seed = 0x12345678u;
  const uint32_t core = local_core_id();

  SRE_printf("\n=== EJIT dso_local / GOT Probe Multi-Core Demo ===\n");
  SRE_printf("[BENCH][core=%u] enter cell=%u calls=%u batches=%u\n", core,
             cellIdx, CALLS_PER_BATCH, MEASURE_BATCHES);
  SRE_printf("[BENCH][core=%u] got_probe reads 6 externalized globals -> "
             "up to 6 GOT loads (dso_local=false) / 0 (dso_local=true)\n",
             core);

  init_data();
  SRE_printf("[BENCH][core=%u] data initialized (probeA..F + probeArr)\n",
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

  //--- Step 2 setup: enable asm capture BEFORE the first compile ---------
  // The dump name must match the ejit_entry function name exactly.
  ejit_dump_func("got_probe_ejit");
  SRE_printf("[BENCH][core=%u] asm capture enabled for got_probe_ejit\n",
             core);

  ejit_status_t probeRc = ejit_activate("probe", cellIdx);
  SRE_printf("[BENCH][core=%u] activate probe[%u] rc=%d active=%u\n", core,
             cellIdx, (int)probeRc,
             (unsigned)ejit_is_active("probe", cellIdx));
  if (probeRc != EJIT_OK)
    idle_forever(core, "activate-failed");

  //--- Step 1: compile + correctness (relocation overflow check) --------
  // Async/shared-taskpool: the first call returns the AOT fallback (compile
  // happens in the worker). Compile SUCCESS (no relocation overflow) is
  // verified via wait_for_compile + checksum + compileFailed==0.
  uint64_t expected = got_probe_nojit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] first active call: enqueue or join pending "
             "compile\n",
             core);
  uint64_t firstActive = got_probe_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] first active=0x%llx AOT=0x%llx [%s]\n", core,
             (unsigned long long)firstActive, (unsigned long long)expected,
             firstActive == expected ? "MATCH" : "MISMATCH");

  SRE_printf("[BENCH][core=%u] warm-up delay=%u ticks; yielding to worker\n",
             core, WARMUP_COMPILE_DELAY_TICKS);
  SRE_TaskDelay(WARMUP_COMPILE_DELAY_TICKS);
  if (!wait_for_compile(core))
    idle_forever(core, "compile-timeout");

  uint64_t jitHit = got_probe_ejit(cellIdx, seed);
  SRE_printf("[BENCH][core=%u] stable hit=0x%llx expected=0x%llx [%s]\n",
             core, (unsigned long long)jitHit,
             (unsigned long long)expected,
             jitHit == expected ? "MATCH" : "MISMATCH");

  // Overflow check: async compile must have succeeded (compileFailed==0) and
  // the JIT result must match the AOT result.
  ejit_taskpool_stats_t stats;
  ejit_taskpool_get_stats(&stats);
  int overflowOk = (jitHit == expected) && (stats.compileFailed == 0);
  SRE_printf("[BENCH][core=%u] --- Step 1: relocation overflow check ---\n",
             core);
  SRE_printf("[BENCH][core=%u] %s: got_probe JIT=%s compileFailed=%llu "
             "(0 => slab within adrp reach)\n",
             core, overflowOk ? "PASS" : "FAIL",
             jitHit == expected ? "MATCH" : "MISMATCH",
             (unsigned long long)stats.compileFailed);
  if (!overflowOk)
    idle_forever(core, "overflow-failed");

  //--- Step 2: dump the specialized asm for :got: inspection -------------
  SRE_printf("\n[BENCH][core=%u] --- Step 2: specialized asm dump ---\n",
             core);
  SRE_printf("[BENCH][core=%u] >>> inspect output below for \":got:\" <<<\n",
             core);
  SRE_printf("[BENCH][core=%u] >>> expect ~6 with dso_local=false, "
             "0 with dso_local=true <<<\n",
             core);
  ejit_print_dumped("got_probe_ejit");

  //--- Step 3: AOT vs JIT per-call --------------------------------------
  SRE_printf("\n[BENCH][core=%u] --- Step 3: AOT-vs-JIT measure ---\n",
             core);
  struct MeasureResult aot =
      measure_batches(got_probe_nojit, cellIdx, seed, "GOT AOT");
  struct MeasureResult jit =
      measure_batches(got_probe_ejit, cellIdx, seed, "GOT JIT");
  if (aot.checksum != jit.checksum) {
    SRE_printf("[BENCH][core=%u] FAIL: checksum mismatch AOT=0x%llx "
               "JIT=0x%llx\n",
               core, (unsigned long long)aot.checksum,
               (unsigned long long)jit.checksum);
    idle_forever(core, "checksum-mismatch");
  }

  SRE_printf("\n--- EJIT GOT Probe Results ---\n");
  print_result("got_probe AOT", &aot);
  print_result("got_probe JIT", &jit);
  if (jit.averageCyclesPerCall >= aot.averageCyclesPerCall)
    SRE_printf("[BENCH][core=%u] JIT-AOT delta=+%llu cyc/call (GOT "
               "overhead; expect ~0 with dso_local=true)\n",
               core,
               (unsigned long long)(jit.averageCyclesPerCall -
                                    aot.averageCyclesPerCall));
  else
    SRE_printf("[BENCH][core=%u] JIT-AOT delta=-%llu cyc/call (JIT faster; "
               "specialization won)\n",
               core,
               (unsigned long long)(aot.averageCyclesPerCall -
                                    jit.averageCyclesPerCall));

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
  SRE_printf("[BENCH][core=%u] PASS: overflow check ok, AOT/JIT checksums "
             "match\n",
             core);

  /* Do not tear down the process-global shared worker from a peer core. */
  idle_forever(core, "benchmark-complete");
  return 0;
}
