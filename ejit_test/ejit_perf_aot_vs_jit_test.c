//===-- ejit_perf_aot_vs_jit_test.c - AOT vs JIT perf benchmark -----------===//
//
// Measures per-call cost of ejit_entry functions before (AOT fallback) vs
// after (JIT cache-hit) ejit_activate, via ejit_taskpool_trace_now() - which
// is SRE_CycleCountGet64() on the freestanding/SRE target (wall-cycle,
// matching run15.log) and steady_clock nanoseconds on host. Two contrasting
// function shapes isolate when specialization wins:
//
//   perf_globals: 8 externalized non-const globals, minimal specialization.
//     Shows the GOT penalty: JIT slower (GOT overhead > specialization
//     savings). Run under dso_local=true to see it recover.
//
//   perf_fold: foldable branch + constant-trip loop (n=64), NO external
//     globals. Amplified specialization gain: mode and the loop bound n are
//     period-array fields -> specialized to constants, so JIT folds the
//     per-iteration branch and unrolls the loop. AOT keeps a real loop with a
//     per-iter branch and a variable bound. No GOT noise -> the delta isolates
//     the specialization gain.
//
// The contrast demonstrates: specialization wins only when hot-path work
// specializes away AND GOT/dispatch overhead does not dominate.
//
// Run: ./build.sh --run ejit_perf_aot_vs_jit_test
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- perf_globals: GOT-heavy, low specialization -----------------------===//
// 8 non-const globals -> externalized -> GOT loads under dso_local=false.
uint32_t g_pg0 = 1, g_pg1 = 2, g_pg2 = 3, g_pg3 = 4;
uint32_t g_pg4 = 5, g_pg5 = 6, g_pg6 = 7, g_pg7 = 8;
ejit_period_arr(pg) uint32_t g_pgPeriod[4];

ejit_entry
uint32_t perf_globals(ejit_period_arr_ind(pg) uint8_t i) {
  uint32_t s = g_pg0 + g_pg1 + g_pg2 + g_pg3 + g_pg4 + g_pg5 + g_pg6 + g_pg7;
  return s + g_pgPeriod[i];
}

//===-- perf_fold: amplified specialization gain, no external globals -----===//
struct PfCfg {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t n;
};
ejit_period_arr(pf) struct PfCfg g_pfCfg[4];

ejit_entry
uint32_t perf_fold(uint32_t x, ejit_period_arr_ind(pf) uint8_t i) {
  uint32_t mode = g_pfCfg[i].mode; // -> constant after specialization
  uint32_t n = g_pfCfg[i].n;       // -> constant loop bound after specialization
  uint32_t acc = x;
  for (uint32_t k = 0; k < n; k++) {
    if (mode == 1) acc = acc * 3 + k;
    else if (mode == 2) acc = acc * 5 + k;
    else acc = acc + k;
  }
  return acc;
}

//===-- Timing + bench ----------------------------------------------------===//

#define ITERS 100000

static uint64_t bench1(uint32_t (*fn)(uint8_t), uint8_t i) {
  for (int w = 0; w < 2000; w++) (void)fn(i); // warmup
  uint64_t t0 = ejit_taskpool_trace_now();
  volatile uint32_t acc = 0;
  for (int k = 0; k < ITERS; k++) acc = fn(i);
  uint64_t t1 = ejit_taskpool_trace_now();
  (void)acc;
  return (t1 - t0) / ITERS;
}

static uint64_t bench2(uint32_t (*fn)(uint32_t, uint8_t), uint32_t x,
                       uint8_t i) {
  for (int w = 0; w < 2000; w++) (void)fn(x, i); // warmup
  uint64_t t0 = ejit_taskpool_trace_now();
  volatile uint32_t acc = 0;
  for (int k = 0; k < ITERS; k++) acc = fn(x, i);
  uint64_t t1 = ejit_taskpool_trace_now();
  (void)acc;
  return (t1 - t0) / ITERS;
}

static const char *tick_unit(void) {
#ifdef EJIT_FREESTANDING
  return "cyc"; // SRE_CycleCountGet64 (x ratio)
#else
  return "ns";
#endif
}

static void report(const char *name, uint64_t aot, uint64_t jit) {
  int64_t d = (int64_t)jit - (int64_t)aot;
  const char *verdict =
      (d < 0) ? "JIT FASTER (specialization won)"
              : (d > 0) ? "JIT slower (overhead > savings)"
                        : "tie";
  printf("  %-13s AOT=%6llu  JIT=%6llu %s/call  delta=%+lld  %s\n", name,
         (unsigned long long)aot, (unsigned long long)jit, tick_unit(),
         (long long)d, verdict);
}

//===-- Main --------------------------------------------------------------===//

static int g_failures = 0;
#define VERIFY(cond, fmt, ...) do {                                         \
  if (!(cond)) { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); g_failures++; } \
  else { printf("  OK:   " fmt "\n", ##__VA_ARGS__); }                       \
} while(0)

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("=== EJIT AOT-vs-JIT Perf Benchmark ===\n");
  printf("timing: ejit_taskpool_trace_now() = SRE_CycleCountGet64 on target "
         "(%s/call)\n\n", tick_unit());

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  VERIFY(ejit_init(&cfg) == 0, "ejit_init");

  //--- perf_globals: GOT-heavy ---------------------------------------------
  printf("--- perf_globals (8 external globals, minimal specialization) ---\n");
  g_pgPeriod[0] = 100;
  uint64_t pg_aot = bench1(perf_globals, 0); // AOT (before activate)
  uint32_t pg_aot_r = perf_globals(0);
  ejit_activate("pg", 0);
  uint32_t pg_jit_r = perf_globals(0); // first call compiles
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
#endif
  uint64_t pg_jit = bench1(perf_globals, 0); // JIT cache-hit
  VERIFY(pg_aot_r == pg_jit_r, "perf_globals AOT=%u JIT=%u", pg_aot_r, pg_jit_r);
  report("perf_globals", pg_aot, pg_jit);

  //--- perf_fold: amplified specialization gain ----------------------------
  printf("\n--- perf_fold (foldable branch + n=64 loop, no external globals) ---\n");
  g_pfCfg[0].mode = 1;
  g_pfCfg[0].n = 64;
  uint32_t x = 12345;
  uint64_t pf_aot = bench2(perf_fold, x, 0); // AOT
  uint32_t pf_aot_r = perf_fold(x, 0);
  ejit_activate("pf", 0);
  uint32_t pf_jit_r = perf_fold(x, 0); // first call compiles
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
#endif
  uint64_t pf_jit = bench2(perf_fold, x, 0); // JIT cache-hit
  VERIFY(pf_aot_r == pf_jit_r, "perf_fold AOT=%u JIT=%u", pf_aot_r, pf_jit_r);
  report("perf_fold", pf_aot, pf_jit);

  printf("\nInterpretation:\n");
  printf("  perf_globals slower => GOT overhead dominates (run under\n");
  printf("    dso_local=true to see it recover). perf_fold faster =>\n");
  printf("    specialization wins when hot-path work folds away and\n");
  printf("    GOT/dispatch overhead is small. (JIT path includes ~77c\n");
  printf("    dispatch; specialization savings must exceed it to net win.)\n");

  ejit_shutdown();
  printf("\n=== %s: %d failures ===\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures > 0 ? 1 : 0;
}
