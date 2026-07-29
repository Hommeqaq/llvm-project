#!/usr/bin/env bash
#===-- ejit_cross_deep_inline_check.sh ------------------------------------===//
#
# Post-build IR proof that -fejit-cross-inline really inlined the multi-level
# cross-TU call chain into the ejit_entry parent.
#
# ld.lld --save-temps (passed via -Wl,--save-temps) writes one per-entry
# bitcode file per ejit_entry:
#   <outdir>/<test>.ejit-cross.<funcName>.bc
# For this test the only entry is jit_telemetry_score, so we disassemble
#   <outdir>/ejit_cross_deep_inline_test.ejit-cross.jit_telemetry_score.bc
# and assert:
#   * the 4 inlined stages (norm_stage / transform_stage / reduce_stage /
#     leaf_weight) are gone entirely (no call, no define, no declare) --
#     they were inlined into jit_telemetry_score and trimmed away;
#   * the noinline audit_stage survives as a real call (boundary respected);
#   * the may_const globals @g_sys / @g_cell are still referenced so JIT
#     specialization can fold them, and !ejit.may_const was re-annotated.
#
# Usage: ./ejit_cross_deep_inline_check.sh [BUILD_DIR] [OUT_DIR]
#   BUILD_DIR  release build with llvm-dis (default: ../build_release_x86)
#   OUT_DIR    dir holding the test binary + save-temps (default: ./out)
#===----------------------------------------------------------------------===//
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${1:-${ROOT_DIR}/build_release_x86}"
OUT_DIR="${2:-${SCRIPT_DIR}/out}"

LLVM_DIS="${BUILD_DIR}/bin/llvm-dis"
[[ -x "${LLVM_DIS}" ]] || { echo "ERROR: llvm-dis not found at ${LLVM_DIS}"; exit 2; }

BC="${OUT_DIR}/ejit_cross_deep_inline_test.ejit-cross.jit_telemetry_score.bc"
if [[ ! -f "${BC}" ]]; then
  echo "ERROR: per-entry bitcode not found: ${BC}"
  echo "       (build with -Wl,--save-temps; check the test name / entry name)"
  exit 2
fi

LL=$(mktemp --suffix=.ll)
trap 'rm -f "${LL}"' EXIT
"${LLVM_DIS}" "${BC}" -o "${LL}" || { echo "ERROR: llvm-dis failed on ${BC}"; exit 2; }

fails=0
ok()   { printf "  OK:   %s\n" "$1"; }
fail() { printf "  FAIL: %s\n" "$1"; fails=$((fails+1)); }

# Count occurrences of a regex in the disassembled module. grep -c prints the
# count (0 when none) and exits 1 on zero matches; command substitution keeps
# the printed "0" either way, so no `|| echo` (that would double it).
count() { local n; n=$(grep -Ec "$1" "${LL}" 2>/dev/null); echo "${n:-0}"; }

echo "=== IR inlining proof: $(basename "${BC}") ==="

# 1. The 4 inlined stages must be gone entirely (inlined + trimmed).
for fn in norm_stage transform_stage reduce_stage leaf_weight; do
  n=$(count "@${fn}\b")
  if [[ "${n}" -eq 0 ]]; then
    ok "@${fn} fully inlined into jit_telemetry_score (0 refs)"
  else
    fail "@${fn} still referenced ${n}x -- cross-TU inline did NOT collapse the chain"
  fi
done

# 2. The noinline audit_stage must survive as a real call.
n_call=$(count 'call .*@audit_stage\b')
n_def=$(count 'define.*@audit_stage\b')
if [[ "${n_call}" -ge 1 && "${n_def}" -ge 1 ]]; then
  ok "@audit_stage noinline respected: ${n_call} call(s), ${n_def} definition survived"
else
  fail "@audit_stage noinline NOT respected (calls=${n_call}, defs=${n_def})"
fi

# 3. may_const globals still referenced through the inlined chain.
for gv in '@g_sys\b' '@g_cell\b'; do
  n=$(count "${gv}")
  if [[ "${n}" -ge 1 ]]; then
    ok "${gv} referenced (${n}x) -- may_const load preserved for JIT specialization"
  else
    fail "${gv} not referenced -- may_const load lost across cross-TU inline"
  fi
done

# 4. reAnnotateMayConst re-tagged the loads.
n_mc=$(count '!ejit.may_const')
if [[ "${n_mc}" -ge 1 ]]; then
  ok "!ejit.may_const re-annotated (${n_mc}x) after cross-TU inline"
else
  fail "!ejit.may_const missing after cross-TU inline"
fi

echo ""
if [[ "${fails}" -eq 0 ]]; then
  echo "=== IR CHECK PASS ==="
  exit 0
else
  echo "=== IR CHECK FAIL (${fails}) ==="
  exit 1
fi
