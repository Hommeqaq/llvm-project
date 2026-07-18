//===-- EJitSrePlatform.cpp - SRE platform adapter for the code pool ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Wires EJitCodePoolManager to the real SRE platform primitives. Compiled
//  only when EJIT_SRE_CODE_POOL is enabled. The platform symbols (enable_ex,
//  split_2m_to_4k, SRE_MemDbgAlloc) are ONLY declared here — never defined and
//  never given weak fallbacks. The real platform / business link environment
//  must supply their strong definitions; if a symbol is missing it must surface
//  as a link-time error rather than be silently satisfied by a no-op. Host unit
//  tests do not reference makeSreCodePoolManager (they inject mock callbacks
//  into EJitCodePoolManager directly), so this translation unit's external
//  references are never pulled into a host test link.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_CODE_POOL

#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"

#include <cstdint>

#ifndef EJIT_SRE_CODE_POOL_SIZE
#define EJIT_SRE_CODE_POOL_SIZE                                                \
  (static_cast<unsigned long long>(2) * 1024 * 1024)
#endif

#ifndef EJIT_SRE_CODE_POOL_PTNO
#define EJIT_SRE_CODE_POOL_PTNO 8
#endif

// Memory module id passed to SRE_MemDbgAlloc. Not architecturally significant
// for the pool; overridable if a deployment needs a specific id.
#ifndef EJIT_SRE_CODE_POOL_MID
#define EJIT_SRE_CODE_POOL_MID 0
#endif

namespace {
constexpr unsigned long long kSrePoolSize = EJIT_SRE_CODE_POOL_SIZE;
constexpr unsigned char kSrePtNo =
    static_cast<unsigned char>(EJIT_SRE_CODE_POOL_PTNO);
constexpr unsigned kSreMid = static_cast<unsigned>(EJIT_SRE_CODE_POOL_MID);
constexpr size_t k2MiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t k4KiB = static_cast<size_t>(4) * 1024;
} // namespace

//===----------------------------------------------------------------------===//
// Platform primitives (declaration only — defined by the platform/business)
//
// enable_ex / split_2m_to_4k are renamed via asm labels so the generic
// identifiers (ejit_sre_enable_ex / ejit_sre_split_2m_to_4k) are used in C++
// while the linker sees the real platform symbol names. These are intentionally
// NOT given weak fallbacks: in static-pack / partial-link / platform-SDK
// scenarios a weak local definition could shadow or collide with the real
// symbol or bind incorrectly. EmbeddedJIT only declares and calls them; the
// platform must provide the strong definitions.
//===----------------------------------------------------------------------===//
extern "C" unsigned
ejit_sre_enable_ex(unsigned startLevel,
                   unsigned long long va) __asm__("enable_ex");

// Split a 2MiB-aligned [va, va + size) window into 4KiB mappings. Must be
// called before any per-page enable_ex on that window. Returns 0 on success.
extern "C" unsigned
ejit_sre_split_2m_to_4k(unsigned long long va,
                        unsigned long long size) __asm__("split_2m_to_4k");

extern "C" void *SRE_MemDbgAlloc(unsigned int mid, unsigned char ptNo,
                                 unsigned long size, const char *func,
                                 unsigned int line);

namespace {
/// AArch64 self-modifying-code cache synchronization for [Va, Va + Size).
///
/// JIT code is written as data into a RW page, so it lands in the D-cache. The
/// I-cache does not snoop the D-cache on AArch64, so before the core may
/// execute the new code it must: clean the D-cache to the Point of Unification
/// (DC CVAU, so the new instructions reach memory visible to instruction
/// fetch), invalidate the I-cache lines for that range (IC IVAU, so they are
/// re-fetched), then context-synchronize (ISB) on this core. This is the
/// sequence enable_ex does NOT perform - enable_ex only flips the PTE to RX
/// (RO+X, clears PXN/UXN) and flushes the TLB (OsTlbLocalFlushAll = DSB +
/// TLBI VMALLE1 + DSB + ISB). Without it the I-cache serves stale lines / the
/// instruction fetch reads stale memory.
///
/// Per-core: every core that executes JIT code calls this in its own
/// translation context (via the per-core seal). The DC CVAU is redundant on a
/// peer core (the writer already cleaned to PoU) but harmless; the IC IVAU +
/// ISB on each executing core is what makes the new code observable to it.
/// (IC IVAU is inner-shareable, but each PE still needs its own ISB before
/// execution, which the per-core seal guarantees.)
///
/// Implemented with inline asm rather than __builtin___clear_cache (or
/// llvm::sys::Memory::InvalidateInstructionCache, which calls the same
/// external __clear_cache symbol): both resolve to compiler-rt/libgcc's
/// __clear_cache, which the freestanding SRE link does not provide. The line
/// sizes are read from CTR_EL0 so the per-line loop covers every line on any
/// implementation.
void syncCodeCaches(uintptr_t Va, size_t Size) {
  if (Size == 0)
    return;
#ifdef __aarch64__
  uint64_t Ctr;
  __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(Ctr));
  size_t DLine = static_cast<size_t>(4) << ((Ctr >> 16) & 0xF);
  size_t ILine = static_cast<size_t>(4) << (Ctr & 0xF);

  uintptr_t End = Va + Size;
  uintptr_t P = Va & ~static_cast<uintptr_t>(DLine - 1);
  for (; P < End; P += DLine)
    __asm__ __volatile__("dc cvau, %0" :: "r"(P) : "memory");
  __asm__ __volatile__("dsb ish" ::: "memory");

  P = Va & ~static_cast<uintptr_t>(ILine - 1);
  for (; P < End; P += ILine)
    __asm__ __volatile__("ic ivau, %0" :: "r"(P) : "memory");
  __asm__ __volatile__("dsb ish" ::: "memory");
  __asm__ __volatile__("isb" ::: "memory");
#else
  // Non-AArch64 (host) fallback: compiler-rt/libgcc is available here, so the
  // builtin's __clear_cache resolves. The SRE target is always AArch64.
  __builtin___clear_cache(reinterpret_cast<char *>(Va),
                          reinterpret_cast<char *>(Va + Size));
#endif
}

/// Seal one code range on the calling core: sync caches, then flip the page to
/// RX via enable_ex. enable_ex only changes PTE permission and flushes the TLB;
/// syncCodeCaches above is what makes the just-written JIT code executable
/// without serving stale I-cache lines. Returns enable_ex's rc (0 = success).
unsigned sealAndSyncCache(uintptr_t Va, size_t Size) {
  syncCodeCaches(Va, Size);
  return ejit_sre_enable_ex(1, static_cast<unsigned long long>(Va));
}
} // namespace

std::unique_ptr<llvm::ejit::EJitCodePoolManager>
llvm::ejit::makeSreCodePoolManager() {
  EJitCodePoolManager::Options Opts;
  Opts.poolSize = static_cast<size_t>(kSrePoolSize);
  Opts.poolAlign = k2MiB; // large-page / split granularity
  Opts.minCodeAlign = 64;
  EJIT_DIAG_VERBOSE("makeSreCodePoolManager: poolSize=%llu poolAlign=%zu",
                    kSrePoolSize, k2MiB);
#ifdef EJIT_CODE_POOL_4K_SEAL
  // Adapt to the platform's 4K execute-permission interface: the 2MiB pool is
  // split into 4K mappings at creation and sealed one 4KiB page at a time.
  Opts.fourKSeal = true;
  Opts.sealPageSize = k4KiB;
#endif

  auto RawAlloc = [](size_t Bytes) -> void * {
    return SRE_MemDbgAlloc(kSreMid, kSrePtNo, static_cast<unsigned long>(Bytes),
                           __func__, __LINE__);
  };

  auto Seal = [](void *Va) -> unsigned {
#ifdef EJIT_SRE_ENABLE_EX
    // In 4K seal mode Va is a single 4KiB page; in legacy mode it is the 2MiB
    // pool base. sealAndSyncCache syncs the I-cache for the written range then
    // flips the page containing Va to RX (enable_ex does NOT do the cache sync).
#ifdef EJIT_CODE_POOL_4K_SEAL
    return sealAndSyncCache(reinterpret_cast<uintptr_t>(Va), k4KiB);
#else
    return sealAndSyncCache(reinterpret_cast<uintptr_t>(Va),
                            static_cast<size_t>(kSrePoolSize));
#endif
#else
    // Code-pool routing without permission flips (bring-up / measurement).
    (void)Va;
    return 0;
#endif
  };

  auto Split = [](void *Base, size_t Size) -> unsigned {
#ifdef EJIT_CODE_POOL_4K_SEAL
    return ejit_sre_split_2m_to_4k(reinterpret_cast<unsigned long long>(Base),
                                   static_cast<unsigned long long>(Size));
#else
    (void)Base;
    (void)Size;
    return 0;
#endif
  };

  return std::make_unique<EJitCodePoolManager>(Opts, RawAlloc, Seal, Split);
}

bool llvm::ejit::prepareSreCodeForCurrentCore(const void *FnPtr) {
#if !defined(EJIT_SRE_ENABLE_EX) || defined(EJIT_CODE_POOL_4K_SEAL)
  EJIT_DIAG("prepareSreCode: unsupported config (FnPtr=%p), clean fallback",
            FnPtr);
  (void)FnPtr;
  return false;
#else
  if (!FnPtr) {
    EJIT_DIAG("prepareSreCode: null FnPtr, reject");
    return false;
  }
  const auto Address = reinterpret_cast<uintptr_t>(FnPtr);
  const auto PoolBase = Address & ~(static_cast<uintptr_t>(k2MiB) - 1);
  unsigned Rc = sealAndSyncCache(PoolBase, static_cast<size_t>(kSrePoolSize));
  if (Rc != 0) {
    EJIT_DIAG("prepareSreCode FAIL: enable_ex poolBase=0x%llx rc=%u",
              static_cast<unsigned long long>(PoolBase), Rc);
    return false;
  }
  return true;
#endif
}

bool llvm::ejit::ejitSreSplitPoolForCurrentCore(uintptr_t PoolBase,
                                                uint64_t PoolSize) {
#if defined(EJIT_SRE_ENABLE_EX) && defined(EJIT_CODE_POOL_4K_SEAL)
  if (PoolBase == 0 || PoolSize == 0) {
    EJIT_DIAG("splitPoolForCurrentCore reject: poolBase=0x%llx size=%llu",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize));
    return false;
  }
  // Per-core: this splits the 2MiB large page into 4K mappings in the calling
  // core's stage-1 translation only. enable_ex per page follows.
  unsigned Rc = ejit_sre_split_2m_to_4k(
      static_cast<unsigned long long>(PoolBase),
      static_cast<unsigned long long>(PoolSize));
  if (Rc != 0) {
    EJIT_DIAG("splitPoolForCurrentCore FAIL: split_2m_to_4k poolBase=0x%llx "
              "size=%llu rc=%u",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("splitPoolForCurrentCore unsupported config: poolBase=0x%llx",
            static_cast<unsigned long long>(PoolBase));
  (void)PoolBase;
  (void)PoolSize;
  return false;
#endif
}

bool llvm::ejit::ejitSreSealPageForCurrentCore(uintptr_t PageVA) {
#ifdef EJIT_SRE_ENABLE_EX
  if (PageVA == 0) {
    EJIT_DIAG("sealPageForCurrentCore reject: null PageVA");
    return false;
  }
  // Per-core: flips the 4KiB page containing PageVA to RX in the calling core's
  // translation context AND syncs its I-cache for that page. enable_ex does NOT
  // do the cache sync, so it is done here (see sealAndSyncCache). Every core
  // that executes shared JIT code seals its own translation context here before
  // first execution.
  unsigned Rc = sealAndSyncCache(PageVA, k4KiB);
  if (Rc != 0) {
    EJIT_DIAG("sealPageForCurrentCore FAIL: enable_ex pageVA=0x%llx rc=%u",
              static_cast<unsigned long long>(PageVA), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("sealPageForCurrentCore unsupported config: pageVA=0x%llx",
            static_cast<unsigned long long>(PageVA));
  (void)PageVA;
  return false;
#endif
}

#endif // EJIT_SRE_CODE_POOL
