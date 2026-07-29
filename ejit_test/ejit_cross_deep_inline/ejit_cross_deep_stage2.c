/**
 * EJIT deep cross-TU inline 测试 - TU3 (stage2)
 *
 * 定义 transform_stage: 跨 TU, C `extern inline` (产生 inlinehint)。
 *   - extern inline 保证发射外部定义供跨 TU 链接;
 *   - 不带 always_inline, 故由链接期成本模型 (ModuleInlinerWrapperPass) 决定
 *     是否内联 -- 因函数小 + inlinehint 提升阈值, 成本模型会内联它。
 *   这一路径专门演示 "inline 关键字驱动跨 TU 内联" (而非强制内联)。
 */

#include <stdint.h>

#include "ejit_cross_deep_defs.h"

extern struct SysCfg  g_sys;
extern struct CellCfg g_cell[];

// transform_stage: 跨 TU, extern inline (inlinehint, 成本模型内联)。
extern inline
uint32_t transform_stage(uint8_t idx)
{
  // L2 -> L3: 调用 TU4 的 reduce_stage (inline + always_inline)
  uint32_t r = reduce_stage(idx);
  // 读取 g_sys.scale (ejit_may_const) -> 内联后 JIT 特化
  return r + g_sys.scale * 10;
}
