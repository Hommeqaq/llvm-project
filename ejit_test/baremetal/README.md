# Bare-metal Board Tests

SRE board-only demos: self-contained C files that run on the aarch64_be
board under the SRE shell, not on the host. They are **not registered in
`../build.sh`** — the board harness compiles and registers each file
standalone, supplying the public EJIT and SRE symbols the file declares.

Each file's header comment is the authoritative spec: worker/producer
cores, the three-step board flow, the assertion layers, and the
specialization trigger design.

| Test | Verifies |
|------|----------|
| `ejit_rodata_ref_test.c` | Const externalization: a JIT-specialized entry referencing read-only data (named const pointer/array, anonymous literal, const table) must resolve to the AOT image's own `.rodata` object, never a private JIT copy |

Run pattern (core numbers per each test's header): reset the board, run
`test_ejit_period` on the worker core (init + capture), then on the
producer core (activate + assert), then the test's `_print` function on
the worker core (object-level evidence).
