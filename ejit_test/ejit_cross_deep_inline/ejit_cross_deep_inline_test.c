/**
 * EJIT deep cross-TU inline 集成测试 - 主 TU (TU1)
 *
 * 目的: 验证 -fejit-cross-inline 下, 一条跨 4 个 TU 的多层嵌套调用链在链接期
 * 被完整内联进唯一的 ejit_entry 父函数 jit_telemetry_score, !ejit.may_const
 * metadata 保留, JIT 可以正确特化整条链。
 *
 * 调用图 (5 层深, 跨 4 个 TU):
 *
 *   jit_telemetry_score(idx)        [TU1 本文件]  ejit_entry  ← 父函数
 *     ├─ norm_stage(idx)            [TU2]  inline + always_inline   L1 跨TU
 *     │    └─ transform_stage(idx)  [TU3]  extern inline (hint)     L2 跨TU
 *     │         └─ reduce_stage(idx)[TU4]  inline + always_inline   L3 跨TU
 *     │              └─ leaf_weight(idx) [TU4] static inline        L4 TU内
 *     └─ audit_stage(idx)           [TU2]  noinline                  负对照
 *
 * 内联控制策略 (对应需求 "通过 inline 关键字或其他内联阈值控制方法"):
 *   - norm_stage / reduce_stage : C `inline` + `__attribute__((always_inline))`
 *                                 → AlwaysInlinerPass 强制内联
 *   - transform_stage           : C `extern inline` (产生 inlinehint)
 *                                 → 成本模型内联 (跨 TU 的 inline 关键字路径)
 *   - leaf_weight               : `static inline` (TU 内, 编译期内联)
 *   - audit_stage               : `__attribute__((noinline))`
 *                                 → 保留为真实 call, 证明内联器尊重边界
 *
 * 期望结果 (seed: mode=2, scale=3, weight=4, auditBase=1000):
 *   leaf_weight      = weight              = 4
 *   reduce_stage     = leaf + weight*100   = 404
 *   transform_stage  = reduce + scale*10   = 434
 *   norm_stage       = transform + mode    = 436
 *   audit_stage      = auditBase           = 1000
 *   score            = norm + audit        = 1436
 *
 * 编译:  ./build.sh --arch=x86 ejit_cross_deep_inline_test
 * 运行:  ./out/ejit_cross_deep_inline_test [cellIdx]    (默认 0)
 *
 * 验证点:
 *   1. 链接期跨 TU 多层内联成功 (4 TU 合并 + 内联)
 *   2. 整条跨 TU 链 AOT 执行结果正确 = 1436 (证明 4 个 stage 链接成功 + 计算正确)
 *   3. (构建后) --save-temps 的 per-entry .bc 证明 4 个被内联函数的 call 消失,
 *      而 noinline 的 audit_stage call 保留 (见 ejit_cross_deep_inline_check.sh)
 *
 *   注: cross-link 注册表尚未 ctor-注册供 JIT 执行 (与 ejit_cross_inline_test.c
 *   同一已知缺口), 故首调用走 AOT body; 内联本身由 IR check 结构性证明, 不依赖
 *   JIT 编译计数。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"
#include "ejit_cross_deep_defs.h"

//===-- EJIT 周期全局变量 (本 TU 定义, 带 period 属性) ----------------------===//

ejit_period(static)   struct SysCfg  g_sys;            // mode + scale
ejit_period_arr(cell) struct CellCfg g_cell[N_CELL];   // weight + auditBase

//===-- EJIT entry 父函数 --------------------------------------------------===//

ejit_entry
uint32_t jit_telemetry_score(ejit_period_arr_ind(cell) uint8_t idx)
{
  // 跨 TU 多层链: norm_stage -> transform_stage -> reduce_stage -> leaf_weight
  uint32_t norm  = norm_stage(idx);   // 跨 TU, always_inline
  // 负对照: noinline, 链接后仍为真实 call
  uint32_t audit = audit_stage(idx);  // 跨 TU, noinline
  return norm + audit;
}

extern void ejit_shutdown(void);

//===-- 断言 ---------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                  \
  if (!(cond)) {                                     \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);      \
    g_failures++;                                    \
  } else {                                           \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);      \
  }                                                  \
} while (0)

//===-- main ---------------------------------------------------------------===//

int main(int argc, char **argv)
{
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  if (ci >= N_CELL) ci = 0;

  printf("=== EJIT deep cross-TU inline test ===\n");
  printf("cellIdx=%u\n\n", ci);

  // Seed the time-window constants.
  g_sys.mode = 2;
  g_sys.scale = 3;
  g_cell[ci].weight = 4;
  g_cell[ci].auditBase = 1000;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  ejit_activate("cell", ci);

  // The cross-TU inline link produced a loadable @__ejit_bitcode registry
  // (the build's POST_BUILD_CHECK proves the chain was inlined into the
  // per-entry bitcode). Executing jit_telemetry_score runs the AOT body of the
  // deep cross-TU chain end to end; the result must be the specialized 1436.
  //
  // NOTE: a cross-link registry is not yet ctor-registered for JIT execution
  // (same gap noted in ejit_cross_inline_test.c: "JIT compilation requires
  // runtime infra changes outside cross-link scope"), so the first call falls
  // through to the AOT body rather than a JIT-specialized entry. We therefore
  // assert the AOT correctness of the whole chain (which still exercises the
  // linked cross-TU stages) and print compile/cache stats only informationally.
  // The inlining itself is verified structurally by the IR check, not by a
  // JIT compile counter.
  uint32_t r1 = jit_telemetry_score(ci);
  VERIFY(r1 == 1436, "jit_telemetry_score(%u) = %u (expected 1436)", ci, r1);

  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t tp; memset(&tp, 0, sizeof(tp));
  ejit_taskpool_get_stats(&tp);
  printf("  stats: ready=%u hits=%llu compiles=%llu (informational; "
         "cross-link JIT registration is out of scope)\n",
         tp.readyEntries, (unsigned long long)tp.cacheHits,
         (unsigned long long)tp.asyncCompiles);
#else
  ejit_stats_t s; ejit_get_stats(&s);
  printf("  stats: entries=%zu hits=%llu misses=%llu (informational; "
         "cross-link JIT registration is out of scope)\n",
         s.entryCount, (unsigned long long)s.hits, (unsigned long long)s.misses);
#endif

  // Second call: the chain must still produce the correct specialized value.
  uint32_t r2 = jit_telemetry_score(ci);
  VERIFY(r2 == 1436, "jit_telemetry_score(%u) 2nd = %u (expected 1436)", ci, r2);

  ejit_shutdown();

  printf("\n=== %s (%d failures) ===\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
