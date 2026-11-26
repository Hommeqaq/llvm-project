//===-- EJitWorkerVerifier.cpp - Worker-only substitution checks ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitWorkerVerifier.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/Support/ErrorHandling.h"
#include <utility>

using namespace llvm::ejit;

namespace {
template <typename T> uint64_t readWord(uintptr_t Address, bool LittleEndian) {
  const T V = __atomic_load_n(reinterpret_cast<const T *>(Address),
                              __ATOMIC_ACQUIRE);
  const auto *Bytes = reinterpret_cast<const unsigned char *>(&V);
  uint64_t Bits = 0;
  for (unsigned I = 0; I < sizeof(T); ++I)
    Bits |= uint64_t(Bytes[I]) << (8 * (LittleEndian ? I : sizeof(T) - 1 - I));
  return Bits;
}

uint64_t readWatch(uintptr_t Address, unsigned Width, bool LittleEndian) {
  switch (Width) {
  case 1:
    return readWord<uint8_t>(Address, LittleEndian) & 1;
  case 8:
    return readWord<uint8_t>(Address, LittleEndian);
  case 16:
    return readWord<uint16_t>(Address, LittleEndian);
  case 32:
    return readWord<uint32_t>(Address, LittleEndian);
  case 64:
    return readWord<uint64_t>(Address, LittleEndian);
  }
  llvm_unreachable("unsupported verifier read width");
}
} // namespace

bool EJitWorkerVerifier::sameRequest(const EJitCompileRequest &A,
                                     const EJitCompileRequest &B) {
  if (A.funcIndex != B.funcIndex || A.numDims != B.numDims ||
      A.generation != B.generation)
    return false;
  for (unsigned I = 0; I < A.numDims; ++I)
    if (A.dims[I].dimType != B.dims[I].dimType ||
        A.dims[I].instanceId != B.dims[I].instanceId ||
        A.versions[I] != B.versions[I])
      return false;
  return true;
}

void EJitWorkerVerifier::onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                                   EJitWorkerVerifyCapture Capture) {
  if (Capture.watches.empty() && !Capture.uncovered && !Capture.dropped)
    return;
  if (pending_.size() + live_.size() >= MaxVersions) {
    dropped_ += Capture.watches.size() + Capture.uncovered + Capture.dropped;
    return;
  }
  while (Capture.watches.size() + retainedWatches_ > MaxWatches) {
    Capture.watches.pop_back();
    ++Capture.dropped;
  }
  retainedWatches_ += Capture.watches.size();
  EJitCompileRequest Identity{};
  Identity.funcIndex = Req.funcIndex;
  Identity.generation = Req.generation;
  Identity.numDims = Req.numDims;
  for (unsigned I = 0; I < Req.numDims; ++I) {
    Identity.dims[I] = Req.dims[I];
    Identity.versions[I] = Req.versions[I];
  }
  pending_.push_back({Identity, Fn, std::move(Capture)});
}

void EJitWorkerVerifier::onPublished(const EJitCompileRequest &Req,
                                    bool Published) {
  for (auto I = pending_.begin(); I != pending_.end(); ++I) {
    if (!sameRequest(I->request, Req))
      continue;
    if (Published) {
      uncovered_ += I->capture.uncovered;
      dropped_ += I->capture.dropped;
      if (!I->capture.watches.empty()) {
        for (const auto &W : I->capture.watches)
          activeProbes_ += W.addresses.size();
        live_.push_back(std::move(*I));
        pending_.erase(I);
        return;
      }
    }
    retainedWatches_ -= I->capture.watches.size();
    pending_.erase(I);
    return;
  }
}

void EJitWorkerVerifier::dropCurrent() {
  auto &Current = live_[cursor_];
  for (const auto &W : Current.capture.watches) {
    activeProbes_ -= W.addresses.size();
    for (unsigned R = 0; R < W.addresses.size(); ++R)
      checkedActiveProbes_ -= (W.seen >> R) & 1;
  }
  retainedWatches_ -= Current.capture.watches.size();
  if (cursor_ + 1 != live_.size())
    Current = std::move(live_.back());
  live_.pop_back();
  if (cursor_ == live_.size())
    cursor_ = 0;
  ++retired_;
}

void EJitWorkerVerifier::advance() {
  if (++cursor_ == live_.size())
    cursor_ = 0;
}

void EJitWorkerVerifier::printRecordedRows(VerifyYieldFn Yield,
                                           void *YieldCtx) const {
  // Dump-only console output. Every location that has EVER mismatched is
  // reported (sticky: recovery does not unregister it), with its saturating
  // count/samples pair as a rate and the value seen at the FIRST mismatch
  // (`first`); nothing is re-read at print time. `object=<symbol>@<base>
  // offset=<bytes>` pins the member inside its shared object (for a changed
  // pointer root the compared member is the slot itself: base=slot,
  // offset=0). `src` names the driving free-dim argument of the remainder
  // domain (or `const` when the index is fully constant); the domain size is
  // the residue denominator. After every row the worker pauses
  // DumpRowDelayTicks so the report trickles out and never stresses the
  // console or the heartbeat.
  for (const auto &V : live_)
    for (const auto &W : V.capture.watches)
      for (const auto &Rec : W.first) {
        const unsigned R = Rec.residue;
        const unsigned Rate =
            W.samples[R] ? (W.count[R] * 1000u) / W.samples[R] : 0;
        const auto &Q = V.request;
        const unsigned long long Base =
            Rec.changedRoot ? W.rootSlot : W.objectBase;
        const unsigned long long Offset =
            Rec.changedRoot ? 0 : W.addresses[R] - W.objectBase;
        const char *Kind =
            (Rec.changedRoot || W.rootValue) ? "pointer-root" : "value";
        if (W.freeArg != 0xFF)
          EJIT_DIAG_RAW(
              "[WORKER-VERIFY] MISMATCH tier=%u gen=%u "
              "dims=%u:%u@%u,%u:%u@%u,%u:%u@%u,%u:%u@%u "
              "period=%s fn=%s object=%s@0x%llx offset=0x%llx src=arg%u "
              "residue=%u/%zu kind=%s expected=0x%llx first=0x%llx "
              "count=%u/%u rate=%u.%u%%",
              decodeReqTier(Q.funcIndex), Q.generation,
              Q.dims[0].dimType, Q.dims[0].instanceId, Q.versions[0],
              Q.dims[1].dimType, Q.dims[1].instanceId, Q.versions[1],
              Q.dims[2].dimType, Q.dims[2].instanceId, Q.versions[2],
              Q.dims[3].dimType, Q.dims[3].instanceId, Q.versions[3],
              W.period.c_str(), W.function.c_str(), W.object.c_str(), Base,
              Offset, W.freeArg, R, W.addresses.size(), Kind,
              (unsigned long long)(Rec.changedRoot ? W.expectedRoot
                                                   : W.expected),
              (unsigned long long)Rec.value,
              W.count[R], W.samples[R], Rate / 10u, Rate % 10u);
        else
          EJIT_DIAG_RAW(
              "[WORKER-VERIFY] MISMATCH tier=%u gen=%u "
              "dims=%u:%u@%u,%u:%u@%u,%u:%u@%u,%u:%u@%u "
              "period=%s fn=%s object=%s@0x%llx offset=0x%llx src=const "
              "residue=%u/%zu kind=%s expected=0x%llx first=0x%llx "
              "count=%u/%u rate=%u.%u%%",
              decodeReqTier(Q.funcIndex), Q.generation,
              Q.dims[0].dimType, Q.dims[0].instanceId, Q.versions[0],
              Q.dims[1].dimType, Q.dims[1].instanceId, Q.versions[1],
              Q.dims[2].dimType, Q.dims[2].instanceId, Q.versions[2],
              Q.dims[3].dimType, Q.dims[3].instanceId, Q.versions[3],
              W.period.c_str(), W.function.c_str(), W.object.c_str(), Base,
              Offset, R, W.addresses.size(), Kind,
              (unsigned long long)(Rec.changedRoot ? W.expectedRoot
                                                   : W.expected),
              (unsigned long long)Rec.value,
              W.count[R], W.samples[R], Rate / 10u, Rate % 10u);
        if (Yield)
          Yield(YieldCtx, DumpRowDelayTicks);
      }
}

void EJitWorkerVerifier::dump(VerifyYieldFn Yield, void *YieldCtx) const {
  unsigned Recorded = 0;
  for (const auto &V : live_)
    for (const auto &W : V.capture.watches)
      Recorded += W.first.size();
  const bool Full = retainedWatches_ >= MaxWatches;
  // Blocked-root samples count into mismatch_samples_ but not into checks_,
  // so the global ratio is clamped to 100%.
  unsigned GlobalRate =
      checks_ ? (unsigned)((mismatchSamples_ * 1000) / checks_) : 0;
  if (GlobalRate > 1000)
    GlobalRate = 1000;
  EJIT_DIAG_RAW(
      "[WORKER-VERIFY] versions=%zu staged=%zu watches=%u/%u%s "
      "checked_active=%llu/%llu checks=%llu mismatch_samples=%llu "
      "rate=%u.%u%% recorded=%u uncovered=%llu dropped=%llu "
      "blocked_root=%llu retired=%llu",
      live_.size(), pending_.size(), retainedWatches_, MaxWatches,
      Full ? " FULL" : "",
      (unsigned long long)checkedActiveProbes_,
      (unsigned long long)activeProbes_, (unsigned long long)checks_,
      (unsigned long long)mismatchSamples_, GlobalRate / 10u, GlobalRate % 10u,
      Recorded, (unsigned long long)uncovered_, (unsigned long long)dropped_,
      (unsigned long long)blockedRoot_, (unsigned long long)retired_);
  if (Yield)
    Yield(YieldCtx, DumpRowDelayTicks);
}

void EJitWorkerVerifier::idle(const EJitSharedTaskPool &Pool, bool DumpRequested,
                              VerifyYieldFn Yield, void *YieldCtx) {
  if (++idleTicks_ < IdleTicks && !DumpRequested)
    return;
  idleTicks_ = 0;
  for (unsigned N = 0; N < StepBudget && !live_.empty(); ++N) {
    // Interleave a platform tick inside one slice so a print-heavy dump or a
    // large configured step budget cannot starve the worker's heartbeat
    // (bare-metal watchdog) between the loop's own per-round yields.
    if (Yield && EJIT_WORKER_VERIFY_YIELD_EVERY &&
        N % EJIT_WORKER_VERIFY_YIELD_EVERY == 0 && N != 0)
      Yield(YieldCtx, 1);
    auto &V = live_[cursor_];
    auto IsLive = Pool.workerVerifyLive(V.request, V.function);
    if (!IsLive || !*IsLive) {
      if (IsLive)
        dropCurrent();
      else
        advance();
      continue;
    }
    auto &W = V.capture.watches[V.watch];
    const unsigned R = V.residue;
    auto *PointerSlot = reinterpret_cast<const uintptr_t *>(W.rootSlot);
    uintptr_t Root =
        PointerSlot ? __atomic_load_n(PointerSlot, __ATOMIC_ACQUIRE) : 0;
    // Do not dereference a newly installed root, even to confirm a mismatch.
    bool ChangedRoot = PointerSlot && Root != W.expectedRoot;
    const uint64_t ActualField =
        ChangedRoot ? 0 : readWatch(W.addresses[R], W.width, W.littleEndian);
    if (PointerSlot && !ChangedRoot) {
      Root = __atomic_load_n(PointerSlot, __ATOMIC_ACQUIRE);
      ChangedRoot = Root != W.expectedRoot;
    }
    IsLive = Pool.workerVerifyLive(V.request, V.function);
    if (!IsLive || !*IsLive) {
      if (IsLive)
        dropCurrent();
      else
        advance();
      continue;
    }

    const unsigned Bit = 1u << R;
    const uint64_t Expected = ChangedRoot ? W.expectedRoot : W.expected;
    const uint64_t Actual = ChangedRoot ? Root : ActualField;
    if (ChangedRoot)
      ++blockedRoot_;
    else if (!(W.seen & Bit)) {
      ++checkedActiveProbes_;
      W.seen |= Bit;
    }
    if (!ChangedRoot)
      ++checks_;
    // Sticky per-residue record: every probe attempt counts as a sample,
    // every mismatch into the location's count; both saturate at 0xFFFF and
    // freeze so the printed ratio stays meaningful. The FIRST mismatching
    // value (and whether it was a changed pointer root) is frozen into the
    // sparse record; nothing prints here — rows surface only on a dump.
    if (W.samples[R] != 0xFFFF)
      ++W.samples[R];
    if (Actual != Expected) {
      ++mismatchSamples_;
      if (W.count[R] == 0)
        W.first.push_back({uint8_t(R), ChangedRoot, Actual});
      if (W.count[R] != 0xFFFF)
        ++W.count[R];
    }
    if (++V.residue == W.addresses.size()) {
      V.residue = 0;
      if (++V.watch == V.capture.watches.size())
        V.watch = 0;
    }
    advance();
  }
  if (DumpRequested) {
    printRecordedRows(Yield, YieldCtx);
    dump(Yield, YieldCtx);
  }
}
