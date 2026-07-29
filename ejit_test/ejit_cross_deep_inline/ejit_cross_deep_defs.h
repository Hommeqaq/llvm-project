//===-- ejit_cross_deep_defs.h - shared defs for deep cross-TU inline test ===//
//
// Shared ABI for ejit_cross_deep_inline_test: struct layouts (with
// ejit_may_const fields) and the cross-TU stage function declarations.
//
// The stage functions are deliberately split across 4 translation units so
// that PASS1 (compile-time) can only see `declare external` for the cross-TU
// callees.  -fejit-cross-inline merges every TU's full-module bitcode at link
// time and inlines the cross-TU stages into the ejit_entry parent
// (jit_telemetry_score).  See jit_design_doc/EJIT_CROSS_TU_INLINE.md.
//
// Global period variables are NOT declared here: only the defining TU
// (ejit_cross_deep_inline.c) attaches ejit_period / ejit_period_arr, which is
// what records the !ejit.metadata may_const field offsets.  Each consuming TU
// declares them as plain `extern` so no second TU re-emits that metadata.
//===----------------------------------------------------------------------===//

#ifndef EJIT_CROSS_DEEP_DEFS_H
#define EJIT_CROSS_DEEP_DEFS_H

#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#define N_CELL 8

// "static" period singleton: mode + scale are time-window constants.
struct SysCfg {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t scale;
};

// "cell" period array: per-cell weight + auditBase are time-window constants.
struct CellCfg {
  ejit_may_const uint32_t weight;
  ejit_may_const uint32_t auditBase;
};

//===-- Cross-TU stage declarations ----------------------------------------===//
//
// Each stage is defined in its own TU.  The inline-control strategy per stage
// is documented at the definition site; see the call graph in
// ejit_cross_deep_inline.c.
extern uint32_t norm_stage(uint8_t idx);       // TU2: inline + always_inline
extern uint32_t transform_stage(uint8_t idx);  // TU3: extern inline (hint)
extern uint32_t reduce_stage(uint8_t idx);     // TU4: inline + always_inline
extern uint32_t audit_stage(uint8_t idx);      // TU2: noinline (survives)

#endif // EJIT_CROSS_DEEP_DEFS_H
