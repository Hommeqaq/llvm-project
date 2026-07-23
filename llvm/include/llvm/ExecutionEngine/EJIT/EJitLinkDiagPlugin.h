//===-- EJitLinkDiagPlugin.h - JITLink branch-reloc diagnostic plugin -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A LinkGraphLinkingLayer plugin that proves JITLink's special handling of
// branch relocations on AArch64: when a BL/B target is external (e.g. an AOT
// function) or out of the ±128MB direct-branch range, JITLink does NOT emit a
// direct BL. Instead it routes the call through a PointerJumpStub in $__STUBS
//
//     ADRP x16, <got page> ; LDR x16, [x16, #<got off>] ; BR x16
//
// whose GOT entry (in $__GOT) holds the real target address.
//
// The plugin appends a PostFixup pass to every linked LinkGraph. After fixup
// the graph carries the full chain JITLink built, so the pass can report each
// branch relocation with:
//   * the instruction label (bl / b / tbz / tbnz / b.cond), derived from the
//     edge kind plus a 4-byte little-endian opcode read of the fixed-up bytes,
//   * the fixup address, the resolved target, the PC-relative distance,
//   * whether the branch was bridged through a stub+GOT (and the direct
//     distance to the real target, so it is visible whether it exceeds the
//     ±128MB BL range) or kept as a direct in-range branch.
//
// This is the post-link "assembly" view of the branch instructions, obtained
// directly from inside JITLink - no separate dump-then-disassemble roundtrip,
// and no MCDisassembler dependency (which is trimmed out of the embedded
// build). Output goes through EJIT_DIAG_VERBOSE (SRE_printf on bare-metal).
//
// The plugin is always attached but does nothing unless the EJIT log level is
// VERBOSE or above (ejit_set_log_level(EJIT_LOG_LVL_VERBOSE)); zero-cost
// otherwise.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITLINKDIAGPLUGIN_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITLINKDIAGPLUGIN_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"

namespace llvm {
namespace ejit {

class EJitLinkDiagPlugin : public orc::LinkGraphLinkingLayer::Plugin {
public:
  EJitLinkDiagPlugin() = default;

  void modifyPassConfig(orc::MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override;

  // Diagnostic-only plugin: link outcomes are never affected.
  Error notifyFailed(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyRemovingResources(orc::JITDylib &JD,
                                orc::ResourceKey K) override {
    return Error::success();
  }
  void notifyTransferringResources(orc::JITDylib &JD, orc::ResourceKey DstKey,
                                   orc::ResourceKey SrcKey) override {}
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITLINKDIAGPLUGIN_H
