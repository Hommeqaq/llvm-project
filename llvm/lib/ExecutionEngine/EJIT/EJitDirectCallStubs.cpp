//===-- EJitDirectCallStubs.cpp - AArch64 direct PLT stub rewrite ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements EJitDirectCallStubsPlugin. See the header for background.
//
// The rewrite runs as a PreFixup JITLink pass. By that phase external symbols
// have been resolved to absolute addresses (lookup + applyLookupResult are
// done in linkPhase2/3, before PreFixup) and block content is still the
// mutable working memory set by BasicLayout::apply, but no fixup has been
// applied yet. So we can retarget the stub's edges to the real target and
// let the subsequent fixUpBlocks pass apply Page21/PageOffset12 with the
// target's address - exactly as it would for a natively-emitted direct stub.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitDirectCallStubs.h"

#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/MathExtras.h"

#include <cstring>

using namespace llvm;
using namespace llvm::jitlink;

namespace llvm {
namespace ejit {

namespace {

// ADRP x16, <target>@page21       (identical to PointerJumpStubContent[0..3])
// ADD  x16, x16, <target>@pageoff12  (replaces LDR x16,[x16,#off])
// BR   x16                        (identical to PointerJumpStubContent[8..11])
//
// Only the middle instruction differs from aarch64::PointerJumpStubContent
// (LDR 0xf9400210 -> ADD 0x91000210). Page21 applies to the ADRP, PageOffset12
// to the ADD (getPageOffset12Shift returns 0 for non-LD/ST, so the 12-bit
// immediate lands in bits[21:10] - the ADD imm12 field). Both fixups are
// already supported by aarch64::applyFixup; no new edge kind is needed.
constexpr char DirectJumpStubContent[12] = {
    0x10, 0x00, 0x00, (char)0x90u, // ADRP x16, <imm>@page21
    0x10, 0x02, 0x00, (char)0x91u, // ADD  x16, x16, <imm>@pageoff12
    0x00, 0x02, 0x1f, (char)0xd6u  // BR   x16
};

// Resolve the real target symbol T that a GOT-indirect PLT stub jumps to.
//
// A PointerJumpStub block (createPointerJumpStubBlock) has two edges -
// Page21@0 and PageOffset12@4 - both targeting the same GOT-entry symbol G.
// G is an anonymous symbol on an 8-byte block in $__GOT whose Pointer64@0 edge
// points at the real target T (createAnonymousPointer). Returns T or nullptr
// if the block is not a standard GOT-indirect PLT stub.
static Symbol *resolveStubTarget(Block &B, Symbol *&GOTEntryOut,
                                 Edge *&Page21EdgeOut,
                                 Edge *&PageOff12EdgeOut) {
  if (B.getSize() != sizeof(aarch64::PointerJumpStubContent))
    return nullptr;
  if (memcmp(B.getContent().data(), aarch64::PointerJumpStubContent,
             sizeof(aarch64::PointerJumpStubContent)) != 0)
    return nullptr;

  Edge *Page21 = nullptr;
  Edge *PageOff12 = nullptr;
  for (Edge &E : B.edges()) {
    if (E.getOffset() == 0 && E.getKind() == aarch64::Page21) {
      if (Page21)
        return nullptr; // duplicate
      Page21 = &E;
    } else if (E.getOffset() == 4 && E.getKind() == aarch64::PageOffset12) {
      if (PageOff12)
        return nullptr; // duplicate
      PageOff12 = &E;
    }
  }
  if (!Page21 || !PageOff12)
    return nullptr;
  if (&Page21->getTarget() != &PageOff12->getTarget())
    return nullptr; // both edges must share the same GOT entry

  Symbol &G = Page21->getTarget();
  if (!G.isDefined())
    return nullptr;
  Block &GOTBlock = G.getBlock();
  for (Edge &E : GOTBlock.edges()) {
    if (E.getOffset() == 0 && E.getKind() == aarch64::Pointer64) {
      GOTEntryOut = &G;
      Page21EdgeOut = Page21;
      PageOff12EdgeOut = PageOff12;
      return &E.getTarget();
    }
  }
  return nullptr;
}

static Error rewriteDirectCallStubs(LinkGraph &G) {
  // AArch64 ELF only. No-op (and avoid touching $__STUBS semantics) elsewhere.
  if (!G.getTargetTriple().isAArch64())
    return Error::success();

  Section *Stubs = G.findSectionByName(aarch64::PLTTableManager::getSectionName());
  if (!Stubs)
    return Error::success();

  uint64_t Rewritten = 0, Fallback = 0;

  for (Block *B : Stubs->blocks()) {
    Symbol *GOTEntry = nullptr;
    Edge *Page21Edge = nullptr;
    Edge *PageOff12Edge = nullptr;
    Symbol *T = resolveStubTarget(*B, GOTEntry, Page21Edge, PageOff12Edge);
    if (!T)
      continue; // not a standard GOT-indirect PLT stub - leave untouched

    orc::ExecutorAddr tAddr = T->getAddress();
    if (tAddr.isNull()) {
      ++Fallback; // target unresolved - keep the GOT-indirect stub
      continue;
    }

    // Reachability: mirror aarch64::applyFixup's Page21 check exactly so a
    // rewritten stub never triggers a fixup-time out-of-range error. The ADRP
    // sits at offset 0, so PCPage = stub address & ~0xfff; addend is 0.
    uint64_t TargetPage = tAddr.getValue() & ~static_cast<uint64_t>(0xfff);
    uint64_t PCPage = B->getAddress().getValue() & ~static_cast<uint64_t>(0xfff);
    int64_t PageDelta = static_cast<int64_t>(TargetPage - PCPage);
    if (!isInt<33>(PageDelta)) {
      ++Fallback; // beyond ADRP's +-4GiB - keep the GOT-indirect stub
      continue;
    }

    // Rewrite the middle instruction LDR -> ADD in the mutable working memory
    // (the same buffer applyFixup writes to). ADRP/BR bytes are identical, so
    // only bytes [4..7] change.
    MutableArrayRef<char> Content = B->getAlreadyMutableContent();
    std::memcpy(&Content[0], DirectJumpStubContent, sizeof(DirectJumpStubContent));

    // Retarget both edges from the GOT entry to the real target. addend stays
    // 0; the fixup phase will apply Page21 (ADRP -> target page) and
    // PageOffset12 (ADD -> target page offset) with T's address.
    Page21Edge->setTarget(*T);
    PageOff12Edge->setTarget(*T);

    // The GOT entry is left in place (its Pointer64 edge is still fixed up as a
    // harmless dead write). v1 does not reclaim it - see plan section 4/11.
    ++Rewritten;
  }

  if (Rewritten || Fallback)
    EJIT_DIAG_VERBOSE("direct-stub: rewritten=%lu fallback=%lu in %s",
                      (unsigned long)Rewritten, (unsigned long)Fallback,
                      G.getName().c_str());

  return Error::success();
}

} // namespace

void EJitDirectCallStubsPlugin::modifyPassConfig(
    orc::MaterializationResponsibility &MR, jitlink::LinkGraph &G,
    jitlink::PassConfiguration &Config) {
  Config.PreFixupPasses.push_back(rewriteDirectCallStubs);
}

} // namespace ejit
} // namespace llvm
