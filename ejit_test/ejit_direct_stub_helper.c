/**
 * EJIT direct-call-stub test - second TU: an AOT-resident helper.
 *
 * ejit_direct_stub_test.c's JIT entry calls aot_helper(). Because this TU is
 * compiled separately, aot_helper appears as an external declaration in the
 * JIT entry's bitcode - it is NOT pulled into the JIT closure (the closure
 * collector skips declarations), so JITLink routes the call through a PLT
 * pointer-jump stub. That stub is exactly what EJitDirectCallStubsPlugin
 * rewrites from ADRP+LDR+BR (via GOT) to ADRP+ADD+BR (direct) on AArch64.
 *
 * This TU has no ejit_entry / ejit_period annotations: it is plain AOT code.
 */

#include <stdint.h>

/* noinline + used: keep it a real out-of-line AOT call (not inlined into the
 * JIT entry, not eliminated by the linker). */
__attribute__((noinline, used))
uint32_t aot_helper(uint32_t x) {
  /* Non-trivial body so the call is not trivially folded away. The volatile
   * read also defeats any constant-propagation that would elide the call. */
  volatile uint32_t y = x;
  return y * 7U + 3U;
}
