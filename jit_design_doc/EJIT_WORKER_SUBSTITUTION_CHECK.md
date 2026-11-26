# EJIT worker-only substitution verifier

This optional diagnostic checks values actually folded by the normal
specialization pass. It emits no verifier IR, call, load, or hit counter into
business or JIT code. The old instrumented verifier instead checked the load
that a business call actually executed and left the original value in use.
The worker check is not a replacement for that coverage or a correctness guard.

Configure the runtime (not the business Clang):

```text
-DEJIT_WORKER_VERIFY_SUBSTITUTION=ON
```

The feature defaults to OFF. It requires diagnostics (`EJIT_DIAG_ENABLE`) and
monitors **all periods at once**: every worker compile records every coverable
period_const fold of every registered period, and each watch carries its own
source period for the report rows. Use the normal `ejit_init` / `ejit_init_pgo`
in Async mode: records are collected by the shared worker's own compiles.

## Pacing and budgets

- `EJIT_WORKER_VERIFY_IDLE_TICKS` (default 5000): one probe slice every Nth
  worker idle round. Each idle round already costs one platform tick
  (`SRE_TaskDelay` via the injected idle hook), so the two-check interval is
  N platform ticks plus poll overhead — with the product clock (1 tick ~ 1 ms)
  the default is one scan every ~5 seconds. A dump request bypasses the
  throttle on the next idle round, and compile/publish work always takes
  priority.
- `EJIT_WORKER_VERIFY_STEP_BUDGET` (default 32): max probe/retirement steps
  per slice. A slice always ends with one fixed rest, and inside a slice the
  worker pauses `EJIT_WORKER_VERIFY_YIELD_EVERY` (default 8) completed steps
  for the fixed yield below, so a large budget cannot starve the bare-metal
  watchdog heartbeat. Work/rest scale: one probe step is microseconds
  (well under a tick), so a 32-step slice's work is far below the ~50-tick
  work window; the fixed rests dominate by design.
- `EJIT_WORKER_VERIFY_MAX_WATCHES` (default 81920) /
  `EJIT_WORKER_VERIFY_MAX_VERSIONS` (default 1024): global budgets across all
  tracked versions (watch = one folded-load site with its residue addresses).
  Overrun trims the NEWEST capture and counts into `dropped`; existing
  watches are never evicted.
- Verifier yield pacing is fixed (not configurable): every platform pause the
  verifier makes — one per `EJIT_WORKER_VERIFY_YIELD_EVERY` completed scan
  steps, after every printed problem row, and once after the stats line —
  lasts 20 platform ticks, so neither scanning nor a report ever stresses the
  console or the system.

This is a work budget, not a time or detection guarantee.

## Output model

Normal scanning never prints: a mismatching probe only bumps the location's
sticky record and the global counters. Output happens only when the worker
core calls `ejit_worker_verify_dump()`:

1. one scan slice runs as usual (same pacing as any other slice);
2. every location that has EVER mismatched is reported, one row each (nothing
   is re-read at print time):

   ```text
   [WORKER-VERIFY] MISMATCH tier=0 gen=1 dims=0:1@1,0:0@0,0:0@0,0:0@0
     period=worker_verify_cell fn=ejit_worker_verify_probe
     object=gEJitWorkerVerifyCells@0x82f40000 offset=0x14 src=arg1
     residue=4/5 kind=value expected=0x7 first=0x11 count=7/7 rate=100.0%
   ```

   - `tier/gen/dims` — which specialization version (tier, generation,
     dimType:instance@version).
   - `period/fn` — the fold's source period and function.
   - `object=<symbol>@<base> offset=<bytes>` — the watched member's exact
     position: the fold's source global (C symbol) with its base address,
     and the member's byte offset inside it. For a changed pointer root the
     compared member is the slot itself (`object=<ptr>@<slot> offset=0x0`).
   - `src` — the driving free-dim parameter of the remainder domain
     (`arg<N>`, or `const` for fully constant indices); the domain size is
     the residue denominator.
   - `residue=r/n` — which slot of the free-dim domain (e.g. `slot % 5`
     folds watch all 5 candidate addresses; each slot is probed and reported
     independently).
   - `kind` — what the row compares: `value` (shared bytes vs the constant
     baked into the code) or `pointer-root` (the shared pointer slot vs the
     pointer baked into the code).
   - `expected`/`first` — the value baked into the code and the value seen at
     the FIRST mismatch (frozen; later mismatches do not update it).
   - `count=m/s rate=x.y%` — saturating per-location counters (mismatches /
     probe attempts); recovery does NOT unregister a recorded location: the
     row stays listed with its frozen count while samples keep growing, so
     the rate decays after recovery.
3. the stats line: `versions`, `staged`, `watches=<used>/<cap>` (with a FULL
   marker once the budget is exhausted), `checked_active=A/B`, `checks`,
   `mismatch_samples`, `rate` (global, clamped to 100%), `recorded`
   (recorded locations), `uncovered`, `dropped`, `blocked_root`, `retired`.

`ejit_worker_verify_dump()` returns as soon as the request is queued; the
rows appear on the worker later. `checks=0` is not a pass; `uncovered`
counts folds whose shape the verifier cannot watch.

Registration failures are discoverable at compile time, not only via a
dump: a capture fully dropped by the version budget logs one
`[WORKER-VERIFY] capture DROPPED` line, and any trimmed capture (per-capture
fold cap or global watch budget) logs one
`[WORKER-VERIFY] capture TRIMMED` line, both from the worker at INFO level.
Uncoverable fold shapes additionally log at VERBOSE level during capture.

### Mapping offsets to source-level members

The embedded bitcode carries IR-level layouts only: struct member NAMES are
stripped during lowering, so the runtime cannot print `slot5[4].value`
directly. To resolve `object=...@0x... offset=0x14` to a field, build a `-g`
(full debug info, not `-gline-tables-only`) image once and read the DWARF:

```bash
llvm-dwarfdump --name=gEJitWorkerVerifyCells board.elf
```

The struct type tree lists every member with its byte offset and size; the
row's offset picks the member (and the residue picks the array slot) without
any runtime support. This mapping stays offline tooling by design.

## Coverage and contract

Coverage is limited to scalar integer/pointer folds with a registered source,
constant offsets and one fully enumerable unsigned remainder (residues <= 16),
including `%5` and `%10`. For root pointers it compares the current shared
slot against the materialized pointer. For bounded fields behind them it
double-checks the root and does not follow a newly installed pointer.
Borrowed bound-pointer pointees are watched as `kind=bound` rows
(`object=bound@<rawPtr> offset=...`, period from the ejit_bound_ptr
annotation): product discipline is that a bound pointee is HEAP memory whose
lifetime spans the active specialization version — a stack address would be
reused after return and produce false mismatches. PHI-witness folds are
watched like the period fold they resolve to (the row carries the witness's
source object; the residue enumeration evaluates under the same PHI binding
the fold proved). A load that never folded is not a verifier failure.

The selected source ranges must be coherent shared RAM with the same contents
on worker and producer. The globals and registered objects of every monitored
period must remain mapped and resident through shutdown; shared mapping alone
does not make a freed or reused allocation readable. Updates must not tear or
race ordinary loads; deliberate diagnostic mutations use naturally aligned
atomic stores. Do not opt volatile/MMIO or moving pointees into this feature.
A generation check discards a sample that overlaps a lifecycle change; it does
not pin storage.

## Board smoke

Link only `ejit_test/ejit_worker_verify_sre_test.c` using normal spec5
annotations and build flags (the verifier runtime requires no new Clang flag).
Enable stats for the runtime build. Follow the product's existing init-array
and shared section configuration:

1. Core 6: `test_ejit_worker_verify_start`.
2. Core 16: `test_ejit_worker_verify_sample` (waits for T1 and T2).
3. Core 6: `ejit_worker_verify_dump`. The stats line must show full checked
   coverage (`checked_active=15/15`, `watches=2/81920`) with zero recorded
   locations and zero mismatch_samples. With just this demo there is nothing
   to report, so no problem rows print.
4. Core 16: `test_ejit_worker_verify_inject`. It changes only residues 4 and 9
   and does not call the optimized function — nothing prints on its own.
5. Core 6: `ejit_worker_verify_dump` again. Wait for the two problem rows
   (`residue=4/5` and `residue=9/10`, one printed every 20 ticks) followed by
   the stats line, with `mismatch_samples` and `recorded` both non-zero.
   A dump also advances one scan slice. Reboot before repeating; never use
   the injection command in a production image.
