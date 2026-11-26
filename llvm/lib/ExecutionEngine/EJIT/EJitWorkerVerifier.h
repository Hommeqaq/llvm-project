//===-- EJitWorkerVerifier.h - Worker-only substitution checks ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITWORKERVERIFIER_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITWORKERVERIFIER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPoolTypes.h"
#include <string>
#include <vector>

namespace llvm::ejit {

// Scan pacing and budgets, overridable at build time (target-scoped
// definitions on LLVMEJIT from the EJIT_WORKER_VERIFY_* CMake cache
// variables). The #ifndef defaults keep standalone consumers — including the
// host unit tests, which drive idle() directly and rely on deterministic
// budgets — independent of any board configuration.
// Product clock calibration: 1 platform tick ~ 1 ms, so the default
// IDLE_TICKS is one probe slice every ~5 seconds.
#ifndef EJIT_WORKER_VERIFY_IDLE_TICKS
#define EJIT_WORKER_VERIFY_IDLE_TICKS 5000
#endif
#ifndef EJIT_WORKER_VERIFY_STEP_BUDGET
#define EJIT_WORKER_VERIFY_STEP_BUDGET 32
#endif
// 0 disables in-slice yielding (tight-host builds only; leave >= 1 on the
// board so a print-heavy dump slice cannot starve the watchdog heartbeat).
#ifndef EJIT_WORKER_VERIFY_YIELD_EVERY
#define EJIT_WORKER_VERIFY_YIELD_EVERY 8
#endif
// Global budgets across all tracked versions: total watches (folded-load
// sites, each with its residue addresses) and total simultaneous versions.
// Overrun trims the NEWEST capture (existing watches are never evicted) and
// counts into `dropped`; the stats line reports watches=<n>/<cap> with a FULL
// marker once the budget is exhausted.
#ifndef EJIT_WORKER_VERIFY_MAX_WATCHES
#define EJIT_WORKER_VERIFY_MAX_WATCHES 81920
#endif
#ifndef EJIT_WORKER_VERIFY_MAX_VERSIONS
#define EJIT_WORKER_VERIFY_MAX_VERSIONS 1024
#endif

class EJitSharedTaskPool;

struct EJitWorkerVerifyWatch {
  // Widest remainder domain a watch may enumerate (compile-side cap in
  // EJitStructFieldPass.cpp; arrays below are sized to it).
  static constexpr unsigned MaxResidues = 16;

  // First-mismatch record, sparse per watch: the value seen (and whether it
  // was a changed pointer root) at the moment a residue FIRST mismatched.
  // Dump rows report this frozen value; nothing is re-read at print time.
  struct FirstMismatch {
    uint8_t residue;
    bool changedRoot;
    uint64_t value;
  };

  std::string function;
  std::string period; // fold's source period (dump rows report its name)
  std::string object; // fold's source global (C symbol name)
  // The address `offset` in the report row is relative to. For value folds
  // it is the registered object/array base; for a pointer slot compared as the
  // value (or as a changed root) it is the slot itself, with offset 0.
  uintptr_t objectBase = 0;
  // Driving free-dim parameter (function argument index) for folds with an
  // enumerable remainder domain; 0xFF when the index is fully constant.
  uint8_t freeArg = 0xFF;
  uint8_t width = 0;
  bool littleEndian = false;
  bool rootValue = false;
  bool bound = false; // fold of a borrowed bound-pointer pointee (kind=bound)
  uint64_t expected = 0;
  uintptr_t rootSlot = 0;
  uintptr_t expectedRoot = 0;
  SmallVector<uintptr_t, 10> addresses;
  // Per-residue sticky record: once a residue has ever mismatched it stays on
  // the dump report (with its rate) until the version retires; recovery does
  // not unregister it. Both counters saturate at 0xFFFF and then freeze, so
  // the printed ratio stays meaningful.
  uint16_t count[MaxResidues] = {};
  uint16_t samples[MaxResidues] = {};
  uint16_t seen = 0;
  SmallVector<FirstMismatch, 2> first;
};

// Compile-side only. No LLVM IR or borrowed calling arguments survive capture.
struct EJitWorkerVerifyCapture {
  static constexpr unsigned MaxWatches = 128;
  std::vector<EJitWorkerVerifyWatch> watches;
  unsigned uncovered = 0;
  unsigned dropped = 0;
};

class EJitWorkerVerifier {
public:
  void onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                  EJitWorkerVerifyCapture Capture);
  void onPublished(const EJitCompileRequest &Req, bool Published);

  /// One probe slice on the worker. Normal slices never print: mismatches
  /// only bump the per-residue sticky records and the global counters.
  /// \p Yield, when non-null, feeds the platform tick: once per
  /// EJIT_WORKER_VERIFY_YIELD_EVERY completed steps while scanning, and
  /// YieldDelayTicks after every row printed by a dump request (plus once
  /// after the stats line). The driver maps it to EJitSreTask::delay, the
  /// same primitive the worker loop's own per-round yield uses.
  using VerifyYieldFn = void (*)(void *Ctx, uint32_t Ticks);
  void idle(const EJitSharedTaskPool &Pool, bool DumpRequested,
            VerifyYieldFn Yield = nullptr, void *YieldCtx = nullptr);

private:
  struct Version {
    EJitCompileRequest request;
    uintptr_t function;
    EJitWorkerVerifyCapture capture;
    unsigned watch = 0;
    unsigned residue = 0;
  };
  static bool sameRequest(const EJitCompileRequest &A,
                          const EJitCompileRequest &B);
  void dropCurrent();
  void advance();
  void printRecordedRows(VerifyYieldFn Yield, void *YieldCtx) const;
  void dump(VerifyYieldFn Yield, void *YieldCtx) const;

  static constexpr unsigned MaxVersions = EJIT_WORKER_VERIFY_MAX_VERSIONS;
  static constexpr unsigned MaxWatches = EJIT_WORKER_VERIFY_MAX_WATCHES;
  static constexpr unsigned IdleTicks = EJIT_WORKER_VERIFY_IDLE_TICKS;
  static constexpr unsigned StepBudget = EJIT_WORKER_VERIFY_STEP_BUDGET;
  // Verifier yield pacing, deliberately fixed (not a CMake knob): every
  // platform yield the verifier makes — one per EJIT_WORKER_VERIFY_YIELD_EVERY
  // completed scan steps, after every printed problem row, and once after the
  // stats line — pauses this many platform ticks, so neither a large step
  // budget nor a long report can stress the console or the system.
  static constexpr unsigned YieldDelayTicks = 20;
  std::vector<Version> pending_, live_;
  size_t cursor_ = 0;
  unsigned retainedWatches_ = 0;
  unsigned idleTicks_ = 0;
  uint64_t checks_ = 0;
  uint64_t activeProbes_ = 0;
  uint64_t checkedActiveProbes_ = 0;
  uint64_t mismatchSamples_ = 0;
  uint64_t uncovered_ = 0;
  uint64_t dropped_ = 0;
  uint64_t blockedRoot_ = 0;
  uint64_t retired_ = 0;
};

} // namespace llvm::ejit
#endif
