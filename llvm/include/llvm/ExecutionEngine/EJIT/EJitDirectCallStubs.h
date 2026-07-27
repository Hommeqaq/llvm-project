//===-- EJitDirectCallStubs.h - AArch64 direct PLT stub rewrite plugin ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ORC ObjectLinkingLayer plugin that rewrites AArch64 JITLink PLT
// pointer-jump stubs into direct PC-relative stubs.
//
// Background: the JIT code slab is SRE_MemAlloc'd ~1.5-2.2GB above AOT .text,
// beyond AArch64 BL's +-128MB reach (so a call stub is unavoidable) but within
// ADRP's +-4GB reach. JITLink therefore routes external calls through a
// PointerJumpStub (ADRP x16,page; LDR x16,[x16,#off]; BR x16) that loads the
// target address from a GOT entry - one data memory load + one indirect branch
// per call. This plugin rewrites such stubs, when the target is within +-4GB,
// into a direct form (ADRP x16,page; ADD x16,x16,#off; BR x16) that addresses
// the target directly, dropping the GOT data load. The indirect branch remains
// (unavoidable for >128MB calls on AArch64); eliminating it entirely requires
// co-locating the slab within +-128MB (a separate, deferred platform change).
//
// Safety: if a target is beyond +-4GB (the slab base is uncontrolled) or its
// address is unavailable, the stub is left as the original GOT-indirect form -
// zero regression. AArch64 ELF only; no-op on other targets.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"

namespace llvm {
namespace ejit {

/// Plugin installed on the ORC ObjectLinkingLayer (when
/// EJIT_DIRECT_CALL_STUBS is defined) to rewrite AArch64 PLT stubs as
/// described above. The rewrite runs as a PreFixup JITLink pass: by then
/// external symbols have been resolved to absolute addresses (lookup +
/// applyLookupResult complete) and block content is still mutable, but
/// fixups have not yet been applied - so retargeted edges are fixed up with
/// the real target address.
class EJitDirectCallStubsPlugin : public orc::LinkGraphLinkingLayer::Plugin {
public:
  void modifyPassConfig(orc::MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override;

  Error notifyFailed(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyEmitted(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyRemovingResources(orc::JITDylib &JD,
                                orc::ResourceKey K) override {
    return Error::success();
  }
  void notifyTransferringResources(orc::JITDylib &JD, orc::ResourceKey DstKey,
                                   orc::ResourceKey SrcKey) override {}

  static std::shared_ptr<EJitDirectCallStubsPlugin> create() {
    return std::make_shared<EJitDirectCallStubsPlugin>();
  }
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H
