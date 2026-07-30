#!/usr/bin/env python3
"""Repeatable benchmark for lld's EJIT cross-TU inline link path.

The generated translation unit carries a ``.ejit_cross`` section holding many
``ejit_entry`` functions; each entry pulls in a small private closure (a chain
of ``noinline`` helpers plus a few private globals) while the composite as a
whole contains every entry's closure. That shape is what stresses the per-entry
extraction step: the composite is large, yet each entry needs only a slice.

Measurement is split into three explicit, non-overlapping modes so that a wall
time is never contaminated by diagnostic work:

  --mode=production   No --save-temps, no --time-trace. This is the number that
                      represents real link cost (wall time, peak RSS).
  --mode=trace        --time-trace only. Reports per-stage share of the link.
                      Its wall time is NOT a production result.
  --mode=compare      --save-temps only. Reports total per-entry bitcode size
                      and a hash, to prove the output is preserved.
  --mode=all          Runs the three above in sequence with separate tables.

With both --baseline-bin and --optimized-bin the production and compare tables
show before/after columns (speedup, RSS, and per-entry bitcode hash equality).
opt/llc are always taken from --optimized-bin (they are identical).

Example:
    lld/utils/ejit-cross-link-bench.py --mode=all \
        --baseline-bin /tmp/baseline/bin \
        --optimized-bin build_release_x86/bin \
        --entries 1,8,32,64,128,256
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time


def gen_module(num_entries, helpers_per_entry, globals_per_entry):
    """Emit LLVM IR: each entry_i owns a private helper chain and globals."""
    out = []
    out.append('target datalayout = '
               '"e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"')
    out.append('target triple = "x86_64-unknown-linux-gnu"')
    out.append('')
    out.append('declare i32 @ejit_extern_sink(i32)')
    out.append('@ejit_extern_global = external global i32')
    out.append('')
    for e in range(num_entries):
        for g in range(globals_per_entry):
            out.append('@g_%d_%d = internal global i32 %d' % (e, g, e * 7 + g))
        for h in range(helpers_per_entry):
            nxt = ('call i32 @helper_%d_%d(i32 %%x)' % (e, h + 1)
                   if h + 1 < helpers_per_entry
                   else 'call i32 @ejit_extern_sink(i32 %x)')
            gref = ('@g_%d_%d' % (e, h % globals_per_entry)
                    if globals_per_entry else '@ejit_extern_global')
            out.append('define internal i32 @helper_%d_%d(i32 %%x) noinline {'
                       % (e, h))
            out.append('  %%gv = load i32, ptr %s' % gref)
            out.append('  %%c = %s' % nxt)
            out.append('  %r = add i32 %c, %gv')
            out.append('  ret i32 %r')
            out.append('}')
        out.append('define i32 @ejit_entry_%d(i32 %%x) !ejit.metadata !0 {' % e)
        if helpers_per_entry:
            out.append('  %%h = call i32 @helper_%d_0(i32 %%x)' % e)
        else:
            out.append('  %h = call i32 @ejit_extern_sink(i32 %x)')
        out.append('  %eg = load i32, ptr @ejit_extern_global')
        out.append('  %r = add i32 %h, %eg')
        out.append('  ret i32 %r')
        out.append('}')
        out.append('')
    out.append('!ejit.metadata = !{}')
    out.append('!0 = !{!{!"ejit_entry"}}')
    out.append('')
    return '\n'.join(out)


def run(cmd, capture_rss):
    """Run cmd; return (wall_seconds, peak_rss_kb_or_None)."""
    prefix = ['/usr/bin/time', '-v'] if capture_rss else []
    start = time.monotonic()
    proc = subprocess.run(prefix + cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)
    wall = time.monotonic() - start
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode('utf-8', 'replace'))
        raise SystemExit('command failed: %s' % ' '.join(cmd))
    rss = None
    if capture_rss:
        for line in proc.stderr.decode('utf-8', 'replace').splitlines():
            if 'Maximum resident set size' in line:
                rss = int(line.rsplit(':', 1)[1].strip())
    return wall, rss


def best_of(cmd, capture_rss, repeats):
    """Return the best (min) wall time over repeats and the max RSS seen."""
    best_wall = None
    best_rss = None
    for _ in range(repeats):
        wall, rss = run(cmd, capture_rss)
        if best_wall is None or wall < best_wall:
            best_wall = wall
        if rss is not None and (best_rss is None or rss > best_rss):
            best_rss = rss
    return best_wall, best_rss


def link_cmd(lld, obj, out, extra):
    return [lld, '--ejit-cross-inline', '-shared',
            '--unresolved-symbols=ignore-all', obj, '-o', out] + extra


def per_entry_hash(workdir, out_name):
    """Sha256 over the sorted per-entry .bc files produced by --save-temps."""
    names = sorted(n for n in os.listdir(workdir)
                   if n.startswith(out_name + '.ejit-cross.'))
    h = hashlib.sha256()
    total = 0
    for n in names:
        with open(os.path.join(workdir, n), 'rb') as f:
            data = f.read()
        h.update(n.encode())
        h.update(data)
        total += len(data)
    return total, h.hexdigest()


def per_entry_ir_hash(bins, workdir, out_name):
    """Sha256 over the disassembled IR of the per-entry .bc files.

    The textual IR is the semantically meaningful comparison: the raw bitcode
    bytes can differ between the full-clone and closure-only paths (different
    internal value ordering) even when the modules are identical. The cosmetic
    '; ModuleID =' line is derived from the input file name by llvm-dis, so it
    is stripped before hashing.
    """
    names = sorted(n for n in os.listdir(workdir)
                   if n.startswith(out_name + '.ejit-cross.'))
    h = hashlib.sha256()
    dis = os.path.join(bins, 'llvm-dis')
    for n in names:
        proc = subprocess.run([dis, os.path.join(workdir, n), '-o', '-'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr.decode('utf-8', 'replace'))
            raise SystemExit('llvm-dis failed on %s' % n)
        for line in proc.stdout.decode('utf-8', 'replace').splitlines():
            if line.startswith('; ModuleID ='):
                continue
            h.update(line.encode())
            h.update(b'\n')
    return h.hexdigest()


def build_inputs(bins, workdir, count, helpers, globals_):
    ll = os.path.join(workdir, 'm_%d.ll' % count)
    bc = os.path.join(workdir, 'm_%d.bc' % count)
    obj = os.path.join(workdir, 'm_%d.o' % count)
    with open(ll, 'w') as f:
        f.write(gen_module(count, helpers, globals_))
    run([os.path.join(bins, 'opt'), '-passes=default<O2>',
         '-enable-ejit-bitcode', '-ejit-cross-inline', ll, '-o', bc], False)
    run([os.path.join(bins, 'llc'), '-filetype=obj', '-relocation-model=pic',
         bc, '-o', obj], False)
    return obj


def mode_production(args, workdir, counts):
    capture_rss = os.path.exists('/usr/bin/time')
    have_base = bool(args.baseline_bin)
    print('== production (no --save-temps, no --time-trace) ==')
    if have_base:
        print('# %-8s %13s %12s %8s %13s %12s' %
              ('entries', 'base_wall_s', 'opt_wall_s', 'speedup',
               'base_rss_kb', 'opt_rss_kb'))
    else:
        print('# %-8s %12s %12s' % ('entries', 'opt_wall_s', 'opt_rss_kb'))
    for count in counts:
        obj = build_inputs(args.optimized_bin, workdir, count,
                           args.helpers, args.globals)
        out = os.path.join(workdir, 'p_%d.so' % count)
        opt_wall, opt_rss = best_of(
            link_cmd(os.path.join(args.optimized_bin, 'ld.lld'), obj, out, []),
            capture_rss, args.repeats)
        if not have_base:
            print('  %-8d %12.3f %12s' %
                  (count, opt_wall, opt_rss if opt_rss is not None else 'n/a'))
            continue
        base_wall, base_rss = best_of(
            link_cmd(os.path.join(args.baseline_bin, 'ld.lld'), obj, out, []),
            capture_rss, args.repeats)
        speedup = base_wall / opt_wall if opt_wall else 0.0
        print('  %-8d %13.3f %12.3f %7.2fx %13s %12s' %
              (count, base_wall, opt_wall, speedup,
               base_rss if base_rss is not None else 'n/a',
               opt_rss if opt_rss is not None else 'n/a'))


def mode_trace(args, workdir, counts):
    print('== trace (--time-trace only; phase share, NOT a production time) ==')
    lld = os.path.join(args.optimized_bin, 'ld.lld')
    for count in counts:
        obj = build_inputs(args.optimized_bin, workdir, count,
                           args.helpers, args.globals)
        out = os.path.join(workdir, 't_%d.so' % count)
        trace = out + '.time-trace'
        run(link_cmd(lld, obj, out,
                     ['--time-trace=' + trace, '--time-trace-granularity=0']),
            False)
        with open(trace) as f:
            data = json.load(f)
        totals = {}
        for ev in data.get('traceEvents', []):
            name = ev.get('name', '')
            if name.startswith('EJitCross') and ev.get('ph') == 'X':
                totals[name] = totals.get(name, 0) + ev.get('dur', 0)
        print('  entries=%d' % count)
        for k in sorted(totals, key=lambda n: -totals[n]):
            print('      %-32s %10.3f ms' % (k, totals[k] / 1000.0))


def mode_compare(args, workdir, counts):
    have_base = bool(args.baseline_bin)
    print('== compare (--save-temps; per-entry bitcode size and hash) ==')
    if have_base:
        print('# %-8s %14s %14s %9s %8s' %
              ('entries', 'base_bc_bytes', 'opt_bc_bytes', 'raw_eq', 'ir_eq'))
    else:
        print('# %-8s %14s %18s' % ('entries', 'opt_bc_bytes', 'opt_hash16'))
    for count in counts:
        obj = build_inputs(args.optimized_bin, workdir, count,
                           args.helpers, args.globals)
        opt_out = os.path.join(workdir, 'c_opt_%d.so' % count)
        run(link_cmd(os.path.join(args.optimized_bin, 'ld.lld'), obj, opt_out,
                     ['--save-temps']), False)
        opt_bytes, opt_hash = per_entry_hash(workdir,
                                            os.path.basename(opt_out))
        if not have_base:
            print('  %-8d %14d %18s' % (count, opt_bytes, opt_hash[:16]))
            continue
        opt_ir = per_entry_ir_hash(args.optimized_bin, workdir,
                                  os.path.basename(opt_out))
        base_out = os.path.join(workdir, 'c_base_%d.so' % count)
        run(link_cmd(os.path.join(args.baseline_bin, 'ld.lld'), obj, base_out,
                     ['--save-temps']), False)
        base_bytes, base_hash = per_entry_hash(workdir,
                                              os.path.basename(base_out))
        base_ir = per_entry_ir_hash(args.optimized_bin, workdir,
                                   os.path.basename(base_out))
        print('  %-8d %14d %14d %9s %8s' %
              (count, base_bytes, opt_bytes,
               'yes' if base_hash == opt_hash else 'no',
               'yes' if base_ir == opt_ir else 'NO'))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--optimized-bin', required=True,
                    help='directory holding the optimized opt/llc/ld.lld')
    ap.add_argument('--baseline-bin', default='',
                    help='directory with a baseline ld.lld (for before/after)')
    ap.add_argument('--mode', choices=['production', 'trace', 'compare', 'all'],
                    default='all')
    ap.add_argument('--entries', default='1,8,32,64,128,256')
    ap.add_argument('--helpers', type=int, default=20)
    ap.add_argument('--globals', type=int, default=6)
    ap.add_argument('--repeats', type=int, default=3,
                    help='production runs per point; the best wall is reported')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    for tool in ('opt', 'llc', 'ld.lld'):
        if not os.path.exists(os.path.join(args.optimized_bin, tool)):
            raise SystemExit('missing tool: %s' % tool)
    if args.baseline_bin and not os.path.exists(
            os.path.join(args.baseline_bin, 'ld.lld')):
        raise SystemExit('missing baseline ld.lld')

    counts = [int(x) for x in args.entries.split(',')]
    workdir = tempfile.mkdtemp(prefix='ejit-cross-bench.')
    print('# work dir: %s' % workdir)
    print('# helpers=%d globals=%d repeats=%d' %
          (args.helpers, args.globals, args.repeats))
    try:
        modes = (['production', 'trace', 'compare']
                 if args.mode == 'all' else [args.mode])
        for m in modes:
            if m == 'production':
                mode_production(args, workdir, counts)
            elif m == 'trace':
                mode_trace(args, workdir, counts)
            elif m == 'compare':
                mode_compare(args, workdir, counts)
    finally:
        if args.keep:
            print('# kept: %s' % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    main()
