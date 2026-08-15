//===-- EJitLibcallStubs.cpp - Codegen-synthesized runtime symbols --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Addresses for the symbols exposed by EJitLibcallStubs.h. See the header
// for the rationale.
//
// None of these symbols are defined here. memset/memcpy/memmove/memcmp are
// the freestanding-mandated C library functions (already linked into the AOT
// binary that hosts the EJIT runtime); __stack_chk_guard/__stack_chk_fail are
// the stack-protector ABI symbols provided by the target's underlying
// pseudo-OS / compiler-rt runtime. We only take their addresses so the engine
// can install them as absolute symbols in each specialization JITDylib —
// exactly like the user-registered symbols, just for names the AOT pass
// cannot collect.
//
// The one exception is __llvm_profile_instrument_target: it is defined here
// (weak) as a no-op because Tier-1 lowers llvm.instrprof.value.profile
// intrinsics to calls of it, and bare-metal / freestanding targets have no
// compiler-rt profile runtime. See the definition below for the rationale.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitLibcallStubs.h"

#include <cstdint>
#include <cstring>

// The stack-protector ABI symbols have no standard C++ header. Declare them
// extern so taking their address resolves to the target's existing
// definitions (uintptr_t matches the compiler-rt declaration). Provided by
// the bare-metal pseudo-OS runtime already linked into the AOT binary.
extern "C" {
extern uintptr_t __stack_chk_guard;
extern void __stack_chk_fail(void);
}

// The PGO indirect-call value-profiling hook, signature matching compiler-rt's
// InstrProfilingValue.c. Online PGO consumes only the __profc_ counters and
// the __profd_ FuncHash (Stage 1 is block layout; indirect-call promotion is
// not planned), so returning the target unchanged is semantically correct.
// Weak so a real profile runtime linked into the host binary takes precedence.
extern "C" __attribute__((weak))
void *__llvm_profile_instrument_target(void *TargetValue, void *Data,
                                       uint32_t CounterIndex) {
  return TargetValue;
}

namespace llvm {
namespace ejit {

ArrayRef<LibcallSymbol> getLibcallSymbols() {
  static const LibcallSymbol Symbols[] = {
      {"memset", reinterpret_cast<void *>(&std::memset)},
      {"memcpy", reinterpret_cast<void *>(&std::memcpy)},
      {"memmove", reinterpret_cast<void *>(&std::memmove)},
      {"memcmp", reinterpret_cast<void *>(&std::memcmp)},
      {"__stack_chk_guard", reinterpret_cast<void *>(&__stack_chk_guard)},
      {"__stack_chk_fail", reinterpret_cast<void *>(&__stack_chk_fail)},
      {"__llvm_profile_instrument_target",
       reinterpret_cast<void *>(&__llvm_profile_instrument_target)},
  };
  return Symbols;
}

} // namespace ejit
} // namespace llvm
