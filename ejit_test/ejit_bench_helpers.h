//===-- ejit_bench_helpers.h - shared multi-core benchmark helpers ---------===//
//
// Shared helpers for the ejit_test multi-core bare-metal benchmark demos
// (ejit_perf_aot_vs_jit_test.c, ejit_got_probe_test.c). Mirrors the
// testcase.log reference structure: SRE platform externs, common timing
// constants, and the wait_for_compile / idle_forever / local_core_id helpers
// every bench demo needs.
//
// These demos run on the SRE bare-metal RTOS: there is no main(), the RTOS
// calls the test entry on every core, the worker core (elected by ejit_init)
// spins in idle_forever, and the remaining cores run the measurement loop.
// SRE_printf / SRE_CycleCountGet64 / SRE_TaskDelay are platform symbols the
// host does not provide - declared extern here.
//
// Only EJIT API + SRE platform symbols are used. No printf, no
// ejit_taskpool_trace_now, no ejit_shutdown from a non-worker core (the
// shared worker is process-global; testcase.log calls idle_forever instead).
//
//===----------------------------------------------------------------------===//

#ifndef EJIT_BENCH_HELPERS_H
#define EJIT_BENCH_HELPERS_H

#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

//===-- EJIT_SHARED_SECTION_ATTR -------------------------------------------===//
// CMake passes -DEJIT_SHARED_SECTION_ATTR=__attribute__((section(".mc_shared")))
// for the aarch64_be preset (cross-core shared globals). Globals WITHOUT it are
// per-core-private: the compile worker (a different core) would not see values
// a verifier core wrote, so specialization folds stale (BSS-zero) values. Mark
// all test data/inputs shared so the worker reads what init_data wrote.
// build.sh does NOT pass this -D to the test, so default it to the same
// .mc_shared section the preset uses (mirrors llvm/CMakePresets.json
// ejit-minimal-aarch64_be). On host (single-core) a named section is harmless.
#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

//===-- SRE bare-metal platform externs ------------------------------------===//
// The host libc does not provide these; the SRE RTOS does. Declared extern
// here so the demos compile against the EJIT runtime header without pulling
// in host stdio.
extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern uint64_t SRE_CycleCountGet64(void);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

//===-- Common timing constants (copied from testcase.log) -----------------===//
#define CALLS_PER_BATCH 200u
#define MEASURE_BATCHES 7u
#define WARMUP_COMPILE_DELAY_TICKS 5000u
#define COMPILE_WAIT_ROUNDS 200u
#define COMPILE_WAIT_TICKS 10u
#define BETWEEN_BATCH_TICKS 1u
#define IDLE_DELAY_TICKS 40000u

//===-- static inline helpers ----------------------------------------------===//

// Returns the calling core's ID. On SRE this is the RTOS-provided
// g_ucLocalCoreID, set by the platform before entering the test function.
static inline uint32_t local_core_id(void) { return (uint32_t)g_ucLocalCoreID; }

// Poll the shared taskpool until the worker has published at least one ready
// cache entry and the pending queue is drained. Bounded by COMPILE_WAIT_ROUNDS
// rounds of SRE_TaskDelay(COMPILE_WAIT_TICKS). Returns 1 on ready, 0 on
// timeout (prints a FAIL line). Mirrors testcase.log's wait_for_compile.
static inline int wait_for_compile(uint32_t core) {
  for (uint32_t round = 0; round < COMPILE_WAIT_ROUNDS; ++round) {
    uint32_t pending = ejit_taskpool_pending_count();
    if (pending == 0) {
      ejit_taskpool_stats_t stats;
      ejit_taskpool_get_stats(&stats);
      if (stats.readyEntries != 0) {
        SRE_printf("[BENCH][core=%u] compile ready: round=%u ready=%u "
                   "hits=%llu compiles=%llu alreadyPending=%llu\n",
                   core, round, stats.readyEntries,
                   (unsigned long long)stats.cacheHits,
                   (unsigned long long)stats.asyncCompiles,
                   (unsigned long long)stats.alreadyPending);
        return 1;
      }
    }

    if ((round % 20u) == 0u) {
      SRE_printf("[BENCH][core=%u] waiting compile: round=%u pending=%u\n",
                 core, round, pending);
    }
    SRE_TaskDelay(COMPILE_WAIT_TICKS);
  }

  SRE_printf("[BENCH][core=%u] FAIL: compile wait timeout\n", core);
  return 0;
}

// Spin forever yielding to the RTOS, printing a heartbeat line so the board
// log shows the core is parked (not crashed). Used by the worker core after
// ejit_init elects it, and by any benchmark core on completion / failure.
static inline void idle_forever(uint32_t core, const char *role) {
  for (;;) {
    SRE_TaskDelay(IDLE_DELAY_TICKS);
    SRE_printf("[BENCH][core=%u] idle role=%s\n", core, role);
  }
}

#endif // EJIT_BENCH_HELPERS_H
