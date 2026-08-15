//===-- EJitLibcallStubs.h - Codegen-synthesized runtime symbols ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The JIT back-end lowers llvm.memset/memcpy/memmove/memcmp intrinsics and
// stack-protector function attributes into references to plain C library and
// ABI symbols (memset, memcpy, __stack_chk_guard, __stack_chk_fail, ...).
// These symbols never appear as IR declarations in the extracted bitcode, so
// the AOT symbol collector cannot register them. On freestanding targets the
// engine also disables process-symbol lookup, leaving the references
// unresolved and every JIT compilation failing at link time.
//
// This header exposes that small set of symbols together with addresses the
// engine can install as absolute symbols in each specialization JITDylib.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITLIBCALLSTUBS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITLIBCALLSTUBS_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
namespace ejit {

struct LibcallSymbol {
  const char *name;
  void *addr;
};

/// Returns the codegen-synthesized runtime symbols the JIT must resolve but
/// the AOT pass cannot collect: memset/memcpy/memmove/memcmp (lowered from
/// the llvm.mem* intrinsics) and __stack_chk_guard/__stack_chk_fail (emitted
/// by -fstack-protector). Those symbols are not defined by the EJIT runtime:
/// the mem* entries point at the freestanding-mandated C library functions
/// and the stack-protector entries point at the target's existing
/// __stack_chk_guard/__stack_chk_fail (provided by its pseudo-OS /
/// compiler-rt runtime). The one symbol defined by the EJIT runtime is
/// __llvm_profile_instrument_target, the PGO value-profiling hook lowered
/// from llvm.instrprof.value.profile (a weak no-op; see EJitLibcallStubs.cpp).
/// The engine only forwards these addresses as absolute symbols.
ArrayRef<LibcallSymbol> getLibcallSymbols();

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITLIBCALLSTUBS_H
