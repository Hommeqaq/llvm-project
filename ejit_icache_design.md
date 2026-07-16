# EJIT Per-Function Inline Cache - v2 (Sticky Monomorphic)

> v2: **freeze at first resolution**. The cache slot is written once (first
> resolve) and read forever - no version/dims/generation re-validation, no
> refill, no `release_read` on the hit path. This is the aggressive
> simplification of v1: "不释放代码" was already true in production, so v2's
> only new semantic step is dropping the re-validation, and it *deletes* the
> HP-scan reclamation follow-up entirely.

## Hard precondition

v2 is correct **only if** every `ejit_entry`'s specialization is invariant for
process lifetime:

1. Period toggles do **not** change the constants baked into the specialization
   (version bumps may happen, but the baked code stays correct - v2 ignores them).
2. Each `ejit_entry` is monomorphic - always called with the same dim identity.
3. The first fill happens with the instance active (the runtime's `if (!fnPtr)
   return` guard already enforces this).

If any is violated, v2 silently runs a stale/wrong specialization. The safety
gate (below) does **not** cover this - it only covers UAF. This is a deployment
contract, not enforced at runtime.

## What changed from v1

| | v1 | v2 |
|---|---|---|
| Slot fields | valid, numDims, generation, dimTypes[4], instances[4], versions[4], fnPtr | **fnPtr only** |
| Table | per-core `gEJitICache[MAX_CORES][FUNC_SLOTS]` | **single-global `gEJitICache[FUNC_SLOTS]`** |
| Hit path | valid load + generation load + numDims cmp + N×(enabled+dimType+instance+version) + fnPtr load | **one acquire load + null check** |
| Refill | on every miss (overwrites slot with new version snapshot) | **never** (one-shot CAS; first resolver wins) |
| Reclamation | HP-scan retire planned as follow-up; safety gate auto-disables until then | **HP-scan permanently N/A** (code never freed); safety gate kept, more important |
| Probe signature | `ejit_icache_try[_Nd](funcIndex, dims..., &outFn)` | **`ejit_icache_try(funcIndex, &outFn)`** (no dims, no `_Nd`) |
| AOT hit path | computes dims, calls `_Nd` probe | **no dim loads on hit**; dims only on the miss path (`jit_call`) |

"Don't free the code" is unchanged: production already runs NO_RECLAIM with no
`releaseFn_` wired, so JIT code is never physically freed. v2 adds no new
reclamation machinery - it removes the need for it.

## Design

### Data structures
```c
EJitAtomicUPtr gEJitICache[EJIT_ICACHE_FUNC_SLOTS];  // process-static, zero-filled
```
One global slot per funcIndex holding the frozen specialization pointer (0 =
empty). Under `EJIT_SRE_SHARED_CODE_POINTERS=OFF` (production) only the owner
core uses the cache, so a single global copy suffices; with sharing on, all
cores share the one slot (still correct).

### Hot path (hit) - one load
```c
icacheTry(funcIndex, &outFn):
  if (!icacheReclamationSafe_) return false;       // safety gate
  if (!state_ || funcIndex >= FUNC_SLOTS) return false;
  if (state_->initState.loadAcquire() != kReady) return false;
  if (!mayReadPtr)  return false;                  // code-sharing gate
  p = gEJitICache[funcIndex].loadAcquire();
  if (p == 0) return false;
  *outFn = p; return true;
// caller: call outFn(args);  -- NO release_read, NO ejit_icache_exit
```

### Fill - one-shot
```c
icacheFill(funcIndex, fnPtr):
  if (!icacheReclamationSafe_) return;
  if (!state_ || !fnPtr || funcIndex >= FUNC_SLOTS) return;
  expected = 0;
  gEJitICache[funcIndex].compareExchange(expected, (uintptr_t)fnPtr);  // first wins
```
Called from every `ejit_taskpool_compile_or_get[_Nd]` success path (cache hit
or fresh compile) via `ejitIcacheFillOnSuccess`. The first resolver wins; later
resolves carry the same invariant pointer and no-op.

### Safety gate (kept, more important than v1)
`setReleaser(fn, ctx)` sets `icacheReclamationSafe_ = (fn == nullptr)`. v2 does
**no** HP-scan retire and never will, so a wired releaser (code may be freed) +
the cache = UAF. The gate auto-disables the cache (`icacheTry` always misses,
`icacheFill` no-op) while a releaser is wired. Production wires no releaser, so
the gate stays open and the cache is unconditionally safe.

### Code-sharing gate (kept)
Mirrors `resolveMatchedSlot`: a non-owner core may read a cached pointer only
when `EJIT_SRE_SHARED_CODE_POINTERS` is platform-validated; otherwise it misses
and falls back to `ejit_taskpool_compile_or_get` (which returns
ready-but-not-shareable -> AOT fallback). Unchanged from v1.

### Reentrancy
v2 is strictly safer than v1: the slot is immutable after the one-shot fill, so
a reentrant `ejit_entry` on the same core reads a stable pointer. v1's "no
same-core reentrancy" assumption is no longer needed for the icache itself.

## AOT changes - `EJitWrapperGen`

`-ejit-inline-cache` (default off) inserts two blocks between `jit_entry` and
`jit_call`:
```
jit_entry   -> jit_icache (funcidx valid) | jit_fallback
jit_icache  : call ejit_icache_try(funcIdx, &outFn) -> HIT? jit_icache_dispatch : jit_call
              (no dim identity computed here)
jit_icache_dispatch: call outFn(args); ret   (NO release_read)
jit_call    : [unchanged] compile_or_get(...); runtime FILLS icache on success
jit_dispatch: [unchanged] indirect call + release_read + ret
jit_fallback: [unchanged] AOT body
```
No dims are computed on the hit path (dims only in `jit_call`), so a cache hit
pays no dim loads. `isAlreadyWrapped` (idempotency) is unchanged.

## Wrapper timing on the icache path

`-ejit-wrapper-timing` instruments the slow path (`jit_call`/`jit_dispatch`) with
`ejit_taskpool_trace_wrapper(funcIdx, status, fnPtr, bucket, tBeforeLookup,
tAfterLookup, tAfterFn, tAfterRelease)`, aggregating `get_fn` (lookup) /
`fn_call` / `release` / `total` per `(funcIndex, status)`. The icache hit path
bypasses `jit_call`/`jit_dispatch`, so v2 instruments it too (gated by
`-ejit-wrapper-timing`, so zero hot-path cost when off):

- `jit_icache`: `tBeforeIcache` before the probe, `tAfterIcache` after.
- `jit_icache_dispatch`: `tAfterFn` after the indirect call; then
  `trace_wrapper(funcIdx, kEJitIcacheHitTimingStatus=0xFE, outFn, 0,
  tBeforeIcache, tAfterIcache, tAfterFn, tAfterFn)` - no release, so
  `tAfterRelease = tAfterFn` and `release_avg = 0`.

The sentinel `0xFE` (collision-free: real `ejit_status_t` as u32 is `0` or
`0xFFFFFFF6..0xFFFFFFFF`) makes icache-hit samples aggregate as their own report
line (`status=254`: `get_fn_avg` = probe cost, `release_avg = 0`), separate from
slow-path samples. Steady state is pure-icache (first call misses, then all
hit), so the slot accumulates cleanly; the lone warmup slow sample resets
silently (existing churn behavior).

## Benefit (hit, per `ejit_entry` call)

| | slow path (v1/miss) | icache hit (v2) |
|---|---|---|
| C calls | 2 (`compile_or_get`+`release_read`) | **1** (`ejit_icache_try`) |
| cross-core RMW | 2 (`readers_` fetchAdd+fetchSub) | **0** |
| slot scan | up to 16 slots × loads | **0** |
| version/dims/generation loads | N + 1 + N | **0** |
| dim loads | 1+N | **0** (dims only on miss) |
| fnPtr acquire | 1 | 1 |
| indirect call | 1 | 1 |

v2 over v1: drops the N version loads + dims/generation compares + (if timing is
off) leaves a single-load probe small enough to inline; the big win (cross-core
RMW + slot scan + `release_read`) was already captured by v1. v2's main extra
wins are the AOT/code-size simplification (no `_Nd`, no dim loads on hit) and
removing the HP-scan reclamation follow-up entirely.

## Configuration

| Flag / define | Default | Effect |
|---|---|---|
| `-ejit-inline-cache` (AOT+runtime) | off | emit/use the icache |
| `-ejit-wrapper-timing` | off | instrument both paths (icache uses sentinel `0xFE`) |
| `EJIT_ICACHE_FUNC_SLOTS` | 64 | table size (override per platform) |

## What is NOT done (deleted vs v1's plans)

- HP-scan retire / `activeDepth` / re-check / `EJIT_ICACHE_RECLAMATION_SAFE` -
  permanently N/A (code never freed).
- Per-core table, `_Nd` probe variants, version/dims/generation snapshot fields -
  gone.
- Polymorphic/multi-slot cache - out of scope (v2 is monomorphic by contract).
