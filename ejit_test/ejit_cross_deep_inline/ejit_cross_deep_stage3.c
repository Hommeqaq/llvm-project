/**
 * EJIT deep cross-TU inline 测试 - TU4 (stage3)
 *
 * 定义链的最深两层:
 *   - reduce_stage : inline + always_inline  -> 链接期强制内联
 *   - leaf_weight  : static inline           -> TU 内编译期内联进 reduce_stage
 *
 * 两者都访问 TU1 的 period 数组 g_cell (本 TU 以 plain extern 引用)。
 */

#include <stdint.h>

#include "ejit_cross_deep_defs.h"

extern struct SysCfg  g_sys;
extern struct CellCfg g_cell[];

// leaf_weight: TU 内 (static inline), 编译期内联进 reduce_stage。
// 读取 g_cell[idx].weight (ejit_may_const)。
static inline
uint32_t leaf_weight(uint8_t idx)
{
  return g_cell[idx].weight;
}

// reduce_stage: 跨 TU, inline + always_inline -> 链接期强制内联。
// extern inline 保证发射外部定义供跨 TU 链接 (被 TU3 的 transform_stage 调用)。
extern inline __attribute__((always_inline))
uint32_t reduce_stage(uint8_t idx)
{
  // L3 -> L4: 调用本 TU 的 leaf_weight (static inline, 编译期内联)
  uint32_t w = leaf_weight(idx);
  // 读取 g_cell[idx].weight (ejit_may_const) -> 内联后 JIT 特化
  return w + g_cell[idx].weight * 100;
}
