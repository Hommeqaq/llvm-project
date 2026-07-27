//===-- EJitCompileTiming.h - EJIT compile-duration timing brackets --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bracket macros for measuring EJIT cold-path compile duration (Total /
// Frontend / Backend) per funcIndex. The aggregator (CompileTimingSlot) and
// the C ABI (ejit_accumulate_compile_timing / ejit_get_compile_timing /
// ejit_print_compile_timing / ejit_reset_compile_timing) live in EJitRuntime.cpp
// and are ALWAYS compiled. This header gates only the auto-instrumentation at
// the compile sites:
//
//   - EJitCompileDriver::compileCold  brackets the whole cold compile (Total).
//   - The IR transform lambda          brackets runPipeline (Frontend).
//   Backend = Total - Frontend (codegen + JITLink run after the lambda returns
//   inside EJitOrcEngine::lookup).
//
// Compile with -DEJIT_COMPILE_TIMING_ENABLE to embed the brackets. When not
// defined, every macro expands to nothing - zero cost on the cold path - and
// the aggregator stays empty (ejit_print_compile_timing reports no samples).
// The timestamp unit is platform-defined (SRE_CycleCountGet64 on freestanding,
// steady_clock ns on host) via ejit_taskpool_trace_now().
//
// The FE brackets call EJitOrcEngine::setFrontendCycles/takeFrontendCycles
// (declared in EJitOrcEngine.h under the same EJIT_COMPILE_TIMING_ENABLE gate);
// the expansion sites must therefore include EJitOrcEngine.h, which both
// EJitCompileDriver.cpp and EJitOrcEngine.cpp do.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILETIMING_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILETIMING_H

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#ifdef EJIT_COMPILE_TIMING_ENABLE

/// Stamp the Total timer at compileCold entry. Declares \p t0var. The variable
/// is [[maybe_unused]] because early-return paths in compileCold (before
/// lookup) skip the RECORD call.
#define EJIT_COMPILE_TIMING_START(t0var)                                        \
  [[maybe_unused]] uint64_t t0var = ejit_taskpool_trace_now()

/// Record one cold compile: Total = now - t0var, Frontend = the engine's
/// captured FE cycles (consumed and reset). Call once after lookup() returns.
#define EJIT_COMPILE_TIMING_RECORD(funcIdx, t0var, eng)                        \
  ejit_accumulate_compile_timing(                                              \
      (funcIdx), ejit_taskpool_trace_now() - (t0var),                          \
      (eng) ? (eng)->takeFrontendCycles() : 0)

/// Stamp the front-end (runPipeline) entry inside the IR transform lambda.
/// Declares \p t1var. \p eng is the EJitOrcEngine*.
#define EJIT_COMPILE_TIMING_FE_BEGIN(eng, t1var)                               \
  [[maybe_unused]] uint64_t t1var = ejit_taskpool_trace_now()

/// Capture Frontend = now - t1var onto the engine for compileCold to consume.
#define EJIT_COMPILE_TIMING_FE_END(eng, t1var)                                 \
  (eng)->setFrontendCycles(ejit_taskpool_trace_now() - (t1var))

#else // !EJIT_COMPILE_TIMING_ENABLE

#define EJIT_COMPILE_TIMING_START(t0var) ((void)0)
#define EJIT_COMPILE_TIMING_RECORD(funcIdx, t0var, eng) ((void)0)
#define EJIT_COMPILE_TIMING_FE_BEGIN(eng, t1var) ((void)0)
#define EJIT_COMPILE_TIMING_FE_END(eng, t1var) ((void)0)

#endif // EJIT_COMPILE_TIMING_ENABLE

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILETIMING_H
