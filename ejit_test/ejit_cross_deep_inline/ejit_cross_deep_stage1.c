/**
 * EJIT deep cross-TU inline 测试 - TU2 (stage1)
 *
 * 定义两个跨 TU 的 stage 函数:
 *   - norm_stage  : inline + always_inline  -> 链接期强制内联进 jit_telemetry_score
 *   - audit_stage : noinline                -> 链接后仍为真实 call (负对照)
 *
 * 两者都访问 TU1 中定义的 period 全局变量 (本 TU 以 plain extern 引用,
 * 不带 ejit_period 属性, 避免重复发射 may_const metadata)。
 */

#include <stdint.h>

#include "ejit_cross_deep_defs.h"

// TU1 定义, 本 TU 仅 extern 引用 (不加 period 属性)。
extern struct SysCfg  g_sys;
extern struct CellCfg g_cell[];

// norm_stage: 跨 TU, inline + always_inline。
// extern inline 保证发射外部定义供跨 TU 链接; always_inline 强制链接期内联。
extern inline __attribute__((always_inline))
uint32_t norm_stage(uint8_t idx)
{
  // L1 -> L2: 调用 TU3 的 transform_stage (extern inline, 成本模型内联)
  uint32_t t = transform_stage(idx);
  // 读取 g_sys.mode (ejit_may_const) -> 内联后 JIT 特化为常量
  return t + g_sys.mode;
}

// audit_stage: 跨 TU, noinline (负对照)。
// 链接期内联器必须尊重 noinline, 保留为真实 call; 其函数体内的 may_const
// load (auditBase) 仍会被 JIT 特化。
__attribute__((noinline))
uint32_t audit_stage(uint8_t idx)
{
  return g_cell[idx].auditBase;
}
