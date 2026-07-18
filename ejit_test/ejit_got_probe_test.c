//===-- ejit_got_probe_test.c - dso_local / GOT verification demo ---------===//
//
// A small demo to verify the dso_local experiment (commit 33754cfdc82e: the
// #if 0 that disables dso_local clearing in EJitOrcEngine::loadBitcodeModule).
// Deliberately simpler than the product ejit_entry functions (e.g.
// DlschCcScheduler's ~31 GOT loads) so it is easier to debug.
//
// It covers the three verification items for the dso_local experiment:
//
//   1. No relocation overflow (slab in PC-relative reach):
//      got_probe reads 6 externalized non-const globals. With dso_local=true
//      the JIT codegen emits ADRP+LDR for them; if the JIT slab is within
//      adrp range (±4GiB) the compile succeeds and the result is correct. A
//      relocation overflow (slab too far) fails the compile -> wrong/zero
//      result -> Step 1 VERIFY fails.
//
//   2. GOT eliminated:
//      ejit_print_dumped("got_probe") prints the specialized asm. Inspect it
//      for ":got:" relocations. Expect ~6 with dso_local=false (old), 0 with
//      dso_local=true (the #if 0 experiment).
//
//   3. Perf (indicative):
//      AOT-vs-JIT per-call delta. Run the demo under both dso_local configs;
//      the JIT overhead should drop when GOT is eliminated. The signal is
//      small for only 6 globals - treat as indicative. For a strong signal
//      use the product DlschCcScheduler (~31 GOT loads) via the run15.log
//      --wrap cycle harness.
//
// Build/run: ./build.sh --run ejit_got_probe_test
//===----------------------------------------------------------------------===//

// clock_gettime/CLOCK_MONOTONIC are POSIX; define defensively (no-op if the
// build already defines it). Matches ejit_perf_bench's use of clock_gettime.
// Must precede any system header inclusion.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ejit_test_helpers.h"

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

ejit_entry
uint32_t got_probe(ejit_period_arr_ind(probe) uint8_t idx) {
  uint32_t s = g_probeA + g_probeB + g_probeC + g_probeD + g_probeE + g_probeF;
  return s + g_probeArr[idx];
}

//===-- Timing ------------------------------------------------------------===//

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define ITERS 200000
static uint64_t avg_call_ns(uint32_t (*fn)(uint8_t), uint8_t idx) {
  for (int i = 0; i < 2000; ++i) (void)fn(idx); // warmup
  uint64_t t0 = now_ns();
  volatile uint32_t acc = 0;
  for (int i = 0; i < ITERS; ++i) acc = fn(idx);
  uint64_t t1 = now_ns();
  (void)acc;
  return (t1 - t0) / ITERS;
}

//===-- Main --------------------------------------------------------------===//

static int g_failures = 0;
#define VERIFY(cond, fmt, ...) do {                                        \
  if (!(cond)) { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); g_failures++; } \
  else { printf("  OK:   " fmt "\n", ##__VA_ARGS__); }                       \
} while(0)

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("=== EJIT dso_local / GOT Probe Demo ===\n");
  printf("got_probe reads 6 externalized globals -> up to 6 GOT loads "
         "(dso_local=false) / 0 (dso_local=true, the #if 0 experiment)\n\n");

  uint8_t idx = 0;
  g_probeArr[idx] = 7;
  const uint32_t expected =
      g_probeA + g_probeB + g_probeC + g_probeD + g_probeE + g_probeF + 7;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  //--- Step 3a: AOT baseline (no activate yet -> AOT fallback body) ---------
  uint64_t aot_ns = avg_call_ns(got_probe, idx);
  printf("--- AOT baseline (before activate): %llu ns/call ---\n",
         (unsigned long long)aot_ns);

  // Enable asm capture BEFORE the first JIT compile so the dump is saved.
  ejit_dump_func("got_probe");

  //--- Step 1: activate + first call triggers JIT compile (overflow check) --
  ejit_activate("probe", idx);
  uint32_t r1 = got_probe(idx);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
#endif
  printf("\n--- Step 1: compile + correctness (relocation overflow check) ---\n");
  VERIFY(r1 == expected, "got_probe(%u) JIT = %u (expected %u)", idx, r1,
         expected);
  printf("  (wrong result / compile FAIL here => dso_local=true caused a "
         "relocation overflow: slab out of adrp reach)\n");

  //--- Step 2: dump the specialized asm for :got: inspection ----------------
  printf("\n--- Step 2: specialized asm dump (inspect for :got:) ---\n");
  printf("  >>> ejit_print_dumped(\"got_probe\") below: look for \":got:\" <<<\n");
  printf("  >>> expect ~6 with dso_local=false, 0 with dso_local=true <<<\n");
  ejit_print_dumped("got_probe");

  //--- Step 3b: JIT per-call (after compile) --------------------------------
  uint64_t jit_ns = avg_call_ns(got_probe, idx);
  printf("\n--- Step 3: JIT per-call (after compile): %llu ns/call ---\n",
         (unsigned long long)jit_ns);
  if (jit_ns >= aot_ns)
    printf("  JIT-AOT delta = +%llu ns/call (GOT overhead; expect ~0 with "
           "dso_local=true)\n", (unsigned long long)(jit_ns - aot_ns));
  else
    printf("  JIT-AOT delta = -%llu ns/call (JIT faster; specialization won)\n",
           (unsigned long long)(aot_ns - jit_ns));
  printf("  (small signal for 6 globals; for a strong signal use the product\n");
  printf("   DlschCcScheduler ~31 GOT loads via the run15.log harness)\n");

  ejit_shutdown();
  printf("\n=== %s: %d failures ===\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures > 0 ? 1 : 0;
}
