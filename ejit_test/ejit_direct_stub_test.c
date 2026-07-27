/**
 * EJIT direct-call-stub test - main TU.
 *
 * Verifies that a JIT specialization can call an AOT-resident function via a
 * PLT stub, and (on AArch64) that EJitDirectCallStubsPlugin rewrites that stub
 * without breaking the call.
 *
 * aot_helper is defined in ejit_direct_stub_helper.c (separate TU), so from the
 * JIT entry's bitcode it is an external declaration -> JITLink emits a PLT
 * pointer-jump stub (ADRP+LDR+BR via a GOT entry) for the call. The plugin
 * rewrites it to a direct ADRP+ADD+BR stub on AArch64 when the target is within
 * +-4GiB (dropping the GOT data load); on x86 it is a no-op. Either way the
 * call must return the correct value - this test is a correctness regression
 * guard for the stub path.
 *
 * On AArch64, to confirm the rewrite fired, build with EJIT_DIAG_ENABLE and
 * look for this line in the log (rewritten>=1, fallback==0 when the slab is
 * within +-4GiB of .text):
 *   [EJIT] ... direct-stub: rewritten=N fallback=M in spec_...
 *
 * Build/run:
 *   ./build.sh release x86            # (or aarch64) - static libs
 *   cd ejit_test && ./build.sh --run ejit_direct_stub_test 0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- EJIT 属性 -----------------------------------------------------------===//

struct CellCfg {
  ejit_may_const uint32_t cellType;
  uint32_t trafficLoad;
};

ejit_period(static) uint32_t g_sysVer;

#define N_CELL 8
ejit_period_arr(cell) struct CellCfg g_cellCfg[N_CELL];

//===-- AOT helper (defined in ejit_direct_stub_helper.c) ------------------===//

extern uint32_t aot_helper(uint32_t x);

//===-- JIT entry ----------------------------------------------------------===//

// JIT folds cellType to a constant (may_const), then calls the AOT helper via a
// PLT stub (external call). On AArch64 the stub is the direct-rewrite target.
ejit_entry
uint32_t jit_calls_aot(ejit_period_arr_ind(cell) uint8_t cellIdx) {
  uint32_t base = g_cellCfg[cellIdx].cellType;  // may_const -> JIT folds
  return aot_helper(base);
}

//===-- 断言 ---------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                                \
  if (!(cond)) {                                                    \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
    g_failures++;                                                   \
  } else {                                                          \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);                    \
  }                                                                 \
} while (0)

extern void ejit_shutdown(void);

//===-- main ---------------------------------------------------------------===//

int main(int argc, char **argv)
{
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  if (ci >= N_CELL)
    ci = 0;

  printf("=== EJIT Direct-Call-Stub Test ===\n");
  printf("cellIdx=%u\n\n", ci);

  g_cellCfg[ci].cellType = 0xFD;  /* 253 */
  g_cellCfg[ci].trafficLoad = 0;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  ejit_activate("cell", ci);

  /* AOT-side reference (direct AOT call, no stub). */
  uint32_t expected = aot_helper(0xFD);

  /*--- 验证 1: JIT 调用 AOT 函数（经 PLT 桩）结果正确 ---*/
  printf("\n--- 验证 1: JIT 经桩调用 AOT 函数 ---\n");
  uint32_t r1 = jit_calls_aot(ci);
  VERIFY(r1 == expected, "jit_calls_aot(%u) = %u (expected %u)", ci, r1, expected);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
  ejit_taskpool_stats_t s1; memset(&s1, 0, sizeof(s1));
  ejit_taskpool_get_stats(&s1);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         s1.readyEntries, (unsigned long long)s1.cacheHits,
         (unsigned long long)s1.asyncCompiles);
  VERIFY(s1.asyncCompiles >= 1, "JIT compiles >= 1 (actual %llu)",
         (unsigned long long)s1.asyncCompiles);
#else
  ejit_stats_t s1;
  ejit_get_stats(&s1);
  printf("  stats: entries=%zu hits=%llu misses=%llu\n", s1.entryCount,
         (unsigned long long)s1.hits, (unsigned long long)s1.misses);
  VERIFY(s1.entryCount >= 1, "JIT entries >= 1 (actual %zu)", s1.entryCount);
#endif

  /*--- 验证 2: 第二次调用（缓存命中）结果一致 ---*/
  printf("\n--- 验证 2: 第二次调用 (缓存命中) ---\n");
  uint32_t r2 = jit_calls_aot(ci);
  VERIFY(r2 == expected, "jit_calls_aot(%u) 2nd call = %u (expected %u)",
         ci, r2, expected);

  ejit_shutdown();

  printf("\n%s\n", g_failures ? "FAIL" : "ALL OK");
  return g_failures ? 1 : 0;
}
