//===-- EJitCrossLink.cpp - EJIT Cross-TU Inlining at Link Time -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ld.lld is the single owner of EJIT cross-TU inline processing. It runs at
// most once per link, and only when --ejit-cross-inline is passed (clang emits
// that flag exactly for -fejit-cross-inline links that use ld.lld). The clang
// driver never performs the merge itself; when the selected linker is not lld
// it emits a hard error instead of silently leaving a huge .ejit_cross section
// in the output. See jit_design_doc/EJIT_CROSS_TU_INLINE.md.
//
// Every stage that can fail after a .ejit_cross section has been observed
// reports an llvm::Error carrying the input file name, the failing stage and
// the underlying LLVM diagnostic. The caller (Driver.cpp) turns that into a
// fatal link error, so a broken cross-inline never silently falls back to the
// per-TU AOT bitcode.
//
//===----------------------------------------------------------------------===//

#include "EJitCrossLink.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// The ELF section that -fejit-cross-inline compiles emit, holding one TU's
/// full module bitcode (see EJitRegisterBitcode.cpp::embedBitcodeInSection).
constexpr StringRef kCrossSection = ".ejit_cross";

//===-- EJIT inline policy options -----------------------------------------===//
//
// These run the link-time inliner in runRealInliner (ld.lld, -mllvm). They are
// link-time only (the compile-time PASS1 cross-inline mode is unaffected).
// ld.lld parses -mllvm args (via parseClangOption -> cl::ParseCommandLineOptions)
// before runEJitCrossLink runs, so they are live when the advisor is built.
//
//   clang -fejit-cross-inline -Wl,-mllvm,-ejit-inline-hot-threshold=4000 ...
//   ld.lld --ejit-cross-inline -mllvm -ejit-inline-diag ...
enum class EJitInlinePolicy { Off, On };
cl::opt<EJitInlinePolicy> EJitInlinePolicyOpt(
    "ejit-inline-policy", cl::init(EJitInlinePolicy::On), cl::Hidden,
    cl::desc("EJIT link-time inline policy for the ejit_entry subtree:"),
    cl::values(clEnumValN(EJitInlinePolicy::Off, "off",
                          "current behavior (AlwaysInliner + default inliner)"),
               clEnumValN(EJitInlinePolicy::On, "on",
                          "EJIT hot/cold advisor with configurable thresholds")));
cl::opt<int> EJitInlineThreshold(
    "ejit-inline-threshold", cl::init(225), cl::Hidden,
    cl::desc("EJIT inline threshold for WARM call sites"));
cl::opt<int> EJitInlineHotThreshold(
    "ejit-inline-hot-threshold", cl::init(3000), cl::Hidden,
    cl::desc("EJIT inline threshold for HOT call sites"));
// Cold call sites are never inlined (a cold branch is kept as a call); the
// cold threshold is intentionally not an option. always_inline still forces
// inlining (it is decided before zone classification).
cl::opt<unsigned> EJitInlineColdCutoff(
    "ejit-inline-cold-cutoff", cl::init(1), cl::Hidden,
    cl::desc("Branch-weight cutoff: a call site reached via an edge with weight "
             "<= this is classified COLD"));
cl::opt<unsigned> EJitInlineHotCutoff(
    "ejit-inline-hot-cutoff", cl::init(2000), cl::Hidden,
    cl::desc("Branch-weight cutoff: a call site reached via an edge with weight "
             ">= this (and greater than its sibling) is classified HOT"));
cl::opt<bool> EJitInlineDiag(
    "ejit-inline-diag", cl::init(false), cl::Hidden,
    cl::desc("Print the raw reason each call site was NOT inlined, plus a "
             "summary"));

/// Build a contextual error. Every cross-inline failure carries the input
/// file, the failing stage and the underlying diagnostic so the link error is
/// actionable (Area 5: no silent swallow, no ambiguous empty string).
static Error crossError(StringRef Stage, StringRef File, const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(),
                           "ejit-cross-inline: " + Stage + " failed for '" +
                               File + "': " + Msg);
}
static Error crossError(StringRef Stage, StringRef File, Error E) {
  return crossError(Stage, File, toString(std::move(E)));
}

static Error writeModuleBitcode(const Module &M, StringRef Path) {
  std::error_code EC;
  raw_fd_ostream Out(Path, EC, sys::fs::OF_None);
  if (EC)
    return createStringError(EC, "cannot open '%s'", Path.str().c_str());
  WriteBitcodeToFile(M, Out);
  Out.flush();
  if (!Out.has_error())
    return Error::success();
  std::error_code WriteEC = Out.error();
  Out.clear_error();
  return createStringError(WriteEC, "cannot write '%s'", Path.str().c_str());
}

static std::string saveTempEntryPath(StringRef Prefix, StringRef FuncName) {
  std::string SafeName;
  SafeName.reserve(FuncName.size());
  for (char C : FuncName)
    SafeName.push_back(llvm::isAlnum(C) || C == '_' ? C : '_');
  return (Twine(Prefix) + ".ejit-cross." + SafeName + ".bc").str();
}

// ---- Metadata / entry discovery (mirrors EJitRegisterBitcode.cpp) ----

static bool hasMDStringEntry(const MDNode *Node, StringRef Name) {
  if (!Node)
    return false;
  for (const MDOperand &Op : Node->operands())
    if (auto *Sub = dyn_cast<MDNode>(Op.get()))
      if (Sub->getNumOperands() >= 1)
        if (auto *S = dyn_cast<MDString>(Sub->getOperand(0)))
          if (S->getString() == Name)
            return true;
  return false;
}

static void collectEntryFunctions(Module &M,
                                  SmallVectorImpl<Function *> &EntryFuncs) {
  for (Function &F : M.functions())
    if (hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
      EntryFuncs.push_back(&F);
}

// ---- Conservative reference collection (Area 4) --------------------------
//
// A single, conservative walk feeds both the closure (which functions/globals
// to keep) and the external-symbol registry. It follows every statically
// resolvable reference form so trimming never deletes a still-referenced
// definition:
//   * direct/bitcasted/const-expr callees (call, invoke, callbr are all
//     CallBase and are covered because the callee is an operand),
//   * a function or global used as a plain operand / stored into a table,
//   * GlobalAlias and GlobalIFunc targets,
//   * function pointers embedded in ConstantExpr / ConstantAggregate,
//   * references inside a kept global's initializer.
// A genuinely runtime-supplied indirect target cannot be inferred here; the
// indirect call remains intact and is resolved through its runtime value.
// Targets present in module-owned tables are covered by initializer scanning.

static void collectFromConstant(const Constant *C,
                                SmallVectorImpl<Function *> &FuncWL,
                                SetVector<GlobalVariable *> &Globals,
                                SmallPtrSetImpl<const Constant *> &Visited) {
  if (!C || !Visited.insert(C).second)
    return;
  if (auto *F = dyn_cast<Function>(C)) {
    FuncWL.push_back(const_cast<Function *>(F));
    return;
  }
  if (auto *GV = dyn_cast<GlobalVariable>(C)) {
    Globals.insert(const_cast<GlobalVariable *>(GV));
    return;
  }
  if (auto *GA = dyn_cast<GlobalAlias>(C)) {
    collectFromConstant(GA->getAliasee(), FuncWL, Globals, Visited);
    return;
  }
  if (auto *GI = dyn_cast<GlobalIFunc>(C)) {
    collectFromConstant(GI->getResolver(), FuncWL, Globals, Visited);
    return;
  }
  // ConstantExpr (bitcast/gep/...), ConstantAggregate (array/struct/vector):
  // recurse through operands to reach any embedded function/global.
  for (const Use &U : C->operands())
    if (auto *OpC = dyn_cast<Constant>(U.get()))
      collectFromConstant(OpC, FuncWL, Globals, Visited);
}

static void collectReferences(Function &F, SmallVectorImpl<Function *> &FuncWL,
                              SetVector<GlobalVariable *> &Globals,
                              SmallPtrSetImpl<const Constant *> &Visited) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (auto *C = dyn_cast<Constant>(Op))
          collectFromConstant(C, FuncWL, Globals, Visited);
}

/// Compute the transitive closure of functions and globals reachable from the
/// entry set. Global initializers are scanned so function-pointer tables keep
/// their targets alive.
static void
computeTransitiveClosure(ArrayRef<Function *> EntryFuncs,
                         SetVector<Function *> &ClosureFuncs,
                         SetVector<GlobalVariable *> &ClosureGlobals) {
  SmallPtrSet<const Constant *, 32> Visited;
  SmallVector<Function *, 16> FuncWL(EntryFuncs.begin(), EntryFuncs.end());
  SetVector<GlobalVariable *> PendingGlobals;

  auto drainGlobals = [&]() {
    // Follow initializers of newly discovered globals (function tables etc.).
    for (unsigned I = 0; I < PendingGlobals.size(); ++I) {
      GlobalVariable *GV = PendingGlobals[I];
      if (!ClosureGlobals.insert(GV))
        continue;
      if (GV->hasInitializer())
        collectFromConstant(GV->getInitializer(), FuncWL, PendingGlobals,
                            Visited);
    }
  };

  while (!FuncWL.empty()) {
    Function *F = FuncWL.pop_back_val();
    if (!ClosureFuncs.insert(F))
      continue;
    if (F->isDeclaration())
      continue; // declarations are handled by the symbol registry
    collectReferences(*F, FuncWL, PendingGlobals, Visited);
    drainGlobals();
  }
  drainGlobals();
}

/// Delete every defined function/global not in the closure, without leaving a
/// dangling reference in either Debug or Release (Area 4.7). Bodies are dropped
/// first (turning definitions into declarations, which severs internal
/// references), then GlobalDCE removes what is now provably dead while
/// conservatively keeping anything still referenced.
static void trimToClosure(Module &M, const SetVector<Function *> &ClosureFuncs,
                          const SetVector<GlobalVariable *> &ClosureGlobals) {
  for (Function &F : M.functions())
    if (!F.isDeclaration() && !ClosureFuncs.contains(&F)) {
      F.replaceAllUsesWith(UndefValue::get(F.getType()));
      F.deleteBody();
    }
  for (GlobalVariable &GV : M.globals())
    if (!GV.isDeclaration() && !ClosureGlobals.contains(&GV)) {
      GV.replaceAllUsesWith(UndefValue::get(GV.getType()));
      GV.setInitializer(nullptr);
      GV.setLinkage(GlobalValue::ExternalLinkage);
    }
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  ModulePassManager MPM;
  MPM.addPass(GlobalDCEPass());
  MPM.run(M, MAM);
}

/// Re-annotate !ejit.may_const on loads using the shared canonical resolver
/// (EJitCommon.h). Reusing ejitMayConstFieldOffset /
/// ejitAccessFitsMayConstField keeps every may_const form PR #78 supports
/// (array/struct, alias, one-level global-pointer indirection, conservative
/// phi/select) working after cross-inline instead of the earlier simplified
/// copy that only understood a direct struct GEP.
static void reAnnotateMayConst(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  auto MayConstKind = Ctx.getMDKindID(MD_EJIT_MAY_CONST);

  DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>> MayConstMap;
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    SmallVector<uint64_t, 4> Offsets;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
        continue;
      if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(1)))
        Offsets.push_back(CI->getZExtValue());
    }
    if (!Offsets.empty())
      MayConstMap[&GV] = std::move(Offsets);
  }
  if (MayConstMap.empty())
    return;

  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || LI->hasMetadata(MayConstKind) || LI->isVolatile() ||
            LI->isAtomic())
          continue;
        const GlobalVariable *GV = nullptr;
        auto Off = ejitMayConstFieldOffset(LI->getPointerOperand(), DL, GV);
        if (!Off || !GV)
          continue;
        auto It = MayConstMap.find(GV);
        if (It == MayConstMap.end() || !is_contained(It->second, *Off))
          continue;
        TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
        if (AccessSize.isScalable() ||
            !ejitAccessFitsMayConstField(GV, *Off, AccessSize.getFixedValue(),
                                         DL))
          continue;
        LI->setMetadata(MayConstKind, MDNode::get(Ctx, {}));
      }
  }
}

//===-- EJIT hot/cold inline policy ---------------------------------------===//
//
// A custom InlineAdvisor injected via PluginInlineAdvisorAnalysis. The CGSCC
// InlinerPass does the actual inlining (and iterates to a fixpoint, so the
// policy is transitive: children of children are covered). The advisor only
// decides per call site and records the reason. always_inline is handled by
// AlwaysInlinerPass (not the advisor), so it never appears in diagnostics.

enum class Zone { Hot, Warm, Cold };

static StringRef zoneStr(Zone Z) {
  switch (Z) {
  case Zone::Hot: return "hot";
  case Zone::Warm: return "warm";
  case Zone::Cold: return "cold";
  }
  return "warm";
}

static std::string formatCallLoc(const CallBase &CB) {
  const DebugLoc &DL = CB.getDebugLoc();
  if (!DL)
    return {};
  DILocation *Loc = DL.get();
  if (!Loc)
    return {};
  StringRef File = Loc->getFilename();
  if (File.empty())
    return Twine(Loc->getLine()).str();
  return (File + ":" + Twine(Loc->getLine())).str();
}

/// One inline decision, captured for diagnostics. Reason is populated for
/// not-inlined call sites (the raw LLVM reason for legality failures, or the
/// cost/threshold numbers for cost-based misses).
struct EJitInlineDecision {
  std::string CallerName;
  std::string CalleeName;
  std::string Loc;
  Zone Z = Zone::Warm;
  int Cost = 0;
  int Threshold = 0;
  std::string Reason;
};

/// Collects inline decisions and prints the not-inlined ones (with their raw
/// reason) plus a summary, when -mllvm -ejit-inline-diag is on. A file-scope
/// instance is used because the PluginInlineAdvisorAnalysis factory is a plain
/// function pointer and cannot capture state; runRealInliner clears/flushes it
/// around the single link-time run.
class EJitInlineDiagRecorder {
public:
  void clear() {
    Considered = Inlined = NotInlined = 0;
    Missed.clear();
  }
  void recordInlined(const EJitInlineDecision &) {
    ++Considered;
    ++Inlined;
  }
  void recordMissed(const EJitInlineDecision &D) {
    ++Considered;
    ++NotInlined;
    Missed.push_back(D);
  }
  void flush(raw_ostream &OS) const {
    if (!EJitInlineDiag)
      return;
    for (const auto &D : Missed) {
      OS << "[ejit-inline] " << D.CallerName << " -> " << D.CalleeName;
      if (!D.Loc.empty())
        OS << " at " << D.Loc;
      OS << ": not inlined (" << D.Reason << ")\n";
    }
    OS << "[ejit-inline] summary: considered=" << Considered
       << " inlined=" << Inlined << " not_inlined=" << NotInlined << "\n";
  }

private:
  unsigned Considered = 0, Inlined = 0, NotInlined = 0;
  SmallVector<EJitInlineDecision, 16> Missed;
};

static EJitInlineDiagRecorder gDiag;

/// Result of classifying a call site: the zone plus the IR evidence that put it
/// there (used verbatim in the diagnostic reason).
struct ZoneResult {
  Zone Z = Zone::Warm;
  std::string Evidence;
};

/// Classify a call site's zone from IR signals (no PGO required). The standard
/// inliner without a profile summary ignores the `hot` attribute and only gives
/// `!prof`-hot edges the local 525 threshold; this classifier is what lets the
/// hot threshold (default 3000) apply, and what marks cold flow as never-inline.
static ZoneResult classifyZone(CallBase &CB, Function &Callee, DominatorTree &DT,
                               BlockFrequencyInfo *BFI) {
  ZoneResult R;
  Function &Caller = *CB.getCaller();
  BasicBlock *BB = CB.getParent();

  // 1. cold attribute on callee or caller.
  if (Callee.hasFnAttribute(Attribute::Cold)) {
    R.Z = Zone::Cold;
    R.Evidence = "cold function attribute (callee)";
    return R;
  }
  if (Caller.hasFnAttribute(Attribute::Cold)) {
    R.Z = Zone::Cold;
    R.Evidence = "cold function attribute (caller)";
    return R;
  }

  // 2. unreachable / noreturn cold path (mirrors InlineCost allowSizeGrowth).
  if (isa<UnreachableInst>(BB->getTerminator())) {
    R.Z = Zone::Cold;
    R.Evidence = "unreachable path";
    return R;
  }
  if (auto *II = dyn_cast<InvokeInst>(&CB))
    if (isa<UnreachableInst>(II->getNormalDest()->getTerminator())) {
      R.Z = Zone::Cold;
      R.Evidence = "unreachable normal destination";
      return R;
    }

  // 3. !prof on dominating conditional branches. Walk the dominator tree up
  // from the call's block; at each conditional branch with branch weights, the
  // successor that dominates the call's block is the edge taken toward it.
  unsigned ColdCut = EJitInlineColdCutoff;
  unsigned HotCut = EJitInlineHotCutoff;
  bool SeenHot = false;
  std::string HotEvidence;
  for (DomTreeNode *N = DT.getNode(BB); N; N = N->getIDom()) {
    BasicBlock *DomBB = N->getBlock();
    if (!DomBB || DomBB == BB)
      continue; // BB's own terminator does not guard the call.
    auto *BI = dyn_cast<BranchInst>(DomBB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    uint64_t T = 0, F = 0;
    if (!extractBranchWeights(*BI, T, F))
      continue;
    BasicBlock *SuccT = BI->getSuccessor(0);
    BasicBlock *SuccF = BI->getSuccessor(1);
    bool TDom = DT.dominates(SuccT, BB);
    bool FDom = DT.dominates(SuccF, BB);
    uint64_t EdgeW, OtherW;
    if (TDom && !FDom) {
      EdgeW = T;
      OtherW = F;
    } else if (FDom && !TDom) {
      EdgeW = F;
      OtherW = T;
    } else {
      continue; // both/neither reach BB: this branch does not exclusively guard it
    }
    if (EdgeW <= ColdCut) {
      R.Z = Zone::Cold;
      R.Evidence =
          ("cold branch (edge weight " + Twine(EdgeW) + " <= cutoff " +
           Twine(ColdCut) + ")")
              .str();
      return R;
    }
    if (EdgeW >= HotCut && OtherW < EdgeW) {
      SeenHot = true;
      HotEvidence =
          ("hot branch (edge weight " + Twine(EdgeW) + " >= cutoff " +
           Twine(HotCut) + ")")
              .str();
    }
  }

  // 4. hot attribute / inlinehint on callee (standard inliner ignores hot w/o
  // PGO; inlinehint only gets 325 there).
  if (Callee.hasFnAttribute(Attribute::Hot)) {
    R.Z = Zone::Hot;
    R.Evidence = "hot function attribute";
    return R;
  }
  if (Callee.hasFnAttribute(Attribute::InlineHint)) {
    R.Z = Zone::Hot;
    R.Evidence = "inline hint (C inline keyword)";
    return R;
  }
  if (SeenHot) {
    R.Z = Zone::Hot;
    R.Evidence = HotEvidence;
    return R;
  }

  // 5. must-execute: the call's block is reached on every function entry (or
  // more, if loop-amplified) -- block freq >= entry freq. This is the
  // unannotated "must-run" case. A __builtin_expect likely branch sits at
  // ~0.9995 of entry (< entry) but is already Hot via the !prof hot-edge check
  // above, so the two paths do not conflict.
  if (BFI && BFI->getBlockFreq(BB) >= BFI->getEntryFreq()) {
    R.Z = Zone::Hot;
    R.Evidence = "must-execute (block freq >= entry freq)";
    return R;
  }

  R.Z = Zone::Warm;
  return R;
}

class EJitInlineAdvice : public InlineAdvice {
public:
  EJitInlineAdvice(InlineAdvisor *A, CallBase &CB,
                   OptimizationRemarkEmitter &ORE, bool Recommend,
                   EJitInlineDiagRecorder &Sink, EJitInlineDecision Dec)
      : InlineAdvice(A, CB, ORE, Recommend), Sink(Sink), Dec(std::move(Dec)) {}

private:
  void recordInliningImpl() override { Sink.recordInlined(Dec); }
  void recordInliningWithCalleeDeletedImpl() override { Sink.recordInlined(Dec); }
  void recordUnattemptedInliningImpl() override { Sink.recordMissed(Dec); }
  void recordUnsuccessfulInliningImpl(const InlineResult &Result) override {
    Dec.Reason = Result.getFailureReason();
    Sink.recordMissed(Dec);
  }
  EJitInlineDiagRecorder &Sink;
  EJitInlineDecision Dec;
};

class EJitInlineAdvisor : public InlineAdvisor {
public:
  EJitInlineAdvisor(Module &M, FunctionAnalysisManager &FAM, InlineParams P,
                    InlineContext IC, EJitInlineDiagRecorder &Sink)
      : InlineAdvisor(M, FAM, IC), Params(std::move(P)), Sink(Sink) {}

private:
  std::unique_ptr<InlineAdvice> getAdviceImpl(CallBase &CB) override {
    Function &Caller = *CB.getCaller();
    Function *Callee = CB.getCalledFunction();
    auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(Caller);

    EJitInlineDecision Dec;
    Dec.CallerName = Caller.getName().str();
    Dec.CalleeName = Callee ? Callee->getName().str() : "<indirect>";
    Dec.Loc = formatCallLoc(CB);

    // Indirect call: cannot inline.
    if (!Callee) {
      Dec.Reason = "indirect call";
      return std::make_unique<EJitInlineAdvice>(this, CB, ORE, false, Sink, Dec);
    }
    // No definition: cannot inline.
    if (Callee->isDeclaration()) {
      Dec.Reason = "unavailable definition";
      return std::make_unique<EJitInlineAdvice>(this, CB, ORE, false, Sink, Dec);
    }

    // Fetch analyses (mirror DefaultInlineAdvisor::getDefaultInlineAdvice).
    ProfileSummaryInfo *PSI =
        FAM.getResult<ModuleAnalysisManagerFunctionProxy>(Caller)
            .getCachedResult<ProfileSummaryAnalysis>(M);
    auto &CalleeTTI = FAM.getResult<TargetIRAnalysis>(*Callee);
    auto GetAssumptionCache = [&](Function &F) -> AssumptionCache & {
      return FAM.getResult<AssumptionAnalysis>(F);
    };
    auto GetTLI = [&](Function &F) -> const TargetLibraryInfo & {
      return FAM.getResult<TargetLibraryAnalysis>(F);
    };
    auto GetBFI = [&](Function &F) -> BlockFrequencyInfo & {
      return FAM.getResult<BlockFrequencyAnalysis>(F);
    };

    // ComputeFullInlineCost so we get a real cost (not a Never) when over
    // threshold; the zone threshold comparison is ours. ORE=nullptr: we capture
    // reasons ourselves and do not want inliner optimization remarks.
    InlineCost IC = getInlineCost(CB, Params, CalleeTTI, GetAssumptionCache,
                                  GetTLI, GetBFI, PSI, /*ORE=*/nullptr);

    if (IC.isAlways()) {
      // always_inline (mandatory). AlwaysInlinerPass normally handled these
      // already; counted as inlined, no missed reason.
      return std::make_unique<EJitInlineAdvice>(this, CB, ORE, true, Sink, Dec);
    }
    if (IC.isNever()) {
      Dec.Reason = IC.getReason();
      return std::make_unique<EJitInlineAdvice>(this, CB, ORE, false, Sink, Dec);
    }

    // Variable cost: classify zone, apply the policy.
    DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(Caller);
    BlockFrequencyInfo &BFI = FAM.getResult<BlockFrequencyAnalysis>(Caller);
    ZoneResult ZR = classifyZone(CB, *Callee, DT, &BFI);
    Dec.Z = ZR.Z;
    Dec.Cost = IC.getCost();

    // Cold flow: never inline. always_inline was already handled above
    // (isAlways); noinline/legality returned isNever. Report the cold evidence.
    if (ZR.Z == Zone::Cold) {
      Dec.Threshold = 0;
      Dec.Reason = "cold flow (" + ZR.Evidence + ")";
      return std::make_unique<EJitInlineAdvice>(this, CB, ORE,
                                                /*Recommend=*/false, Sink, Dec);
    }

    int Threshold = (ZR.Z == Zone::Hot) ? (int)EJitInlineHotThreshold
                                        : (int)EJitInlineThreshold;
    Dec.Threshold = Threshold;
    bool Recommend = IC.getCost() <= Threshold;
    if (!Recommend) {
      std::string Ev = ZR.Evidence.empty() ? std::string() : ("; " + ZR.Evidence);
      Dec.Reason = ("cost=" + Twine(IC.getCost()) + " exceeds threshold=" +
                    Twine(Threshold) + " (zone=" + zoneStr(ZR.Z) + ")" + Ev)
                       .str();
    }
    return std::make_unique<EJitInlineAdvice>(this, CB, ORE, Recommend, Sink, Dec);
  }

  InlineParams Params;
  EJitInlineDiagRecorder &Sink;
};

static InlineAdvisor *ejitAdvisorFactory(Module &M,
                                         FunctionAnalysisManager &FAM,
                                         InlineParams Params,
                                         InlineContext IC) {
  return new EJitInlineAdvisor(M, FAM, Params, IC, gDiag);
}

/// Real, cost-model-driven inliner. With -ejit-inline-policy=on (default) it
/// injects the EJIT hot/cold advisor; with =off it keeps the original
/// AlwaysInliner + default ModuleInliner behavior.
static void runRealInliner(Module &M) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;

  bool PolicyOn = (EJitInlinePolicyOpt == EJitInlinePolicy::On);
  if (PolicyOn) {
    // Materialize @llvm.expect -> !prof so branch-weight classification works.
    FunctionPassManager EarlyFPM;
    EarlyFPM.addPass(LowerExpectIntrinsicPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(EarlyFPM)));
    // always_inline first (does not consult the advisor; not in diagnostics).
    MPM.addPass(AlwaysInlinerPass());

    // InlineParams aligned with the EJIT zones; ComputeFullInlineCost so the
    // advisor gets a real cost even when over threshold. Cold is set to 0 for
    // the cost model's bonus logic, but the advisor never inlines cold flow
    // regardless of cost (see classifyZone / getAdviceImpl).
    InlineParams P;
    P.DefaultThreshold = EJitInlineThreshold;
    P.HotCallSiteThreshold = EJitInlineHotThreshold;
    P.LocallyHotCallSiteThreshold = EJitInlineHotThreshold;
    P.ColdCallSiteThreshold = 0;
    P.ColdThreshold = 0;
    P.HintThreshold = EJitInlineHotThreshold; // C `inline` -> hot
    P.ComputeFullInlineCost = true;
    P.EnableDeferral = false;

    gDiag.clear();
    MAM.registerPass(
        [&] { return PluginInlineAdvisorAnalysis(ejitAdvisorFactory); });
    MPM.addPass(ModuleInlinerWrapperPass(P));
  } else {
    MPM.addPass(AlwaysInlinerPass());
    MPM.addPass(ModuleInlinerWrapperPass());
  }

  FunctionPassManager FPM;
  FPM.addPass(PromotePass());
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(M, MAM);

  if (PolicyOn)
    gDiag.flush(errs());
}

static GlobalVariable *embedBitcode(Module &M, StringRef Bitcode,
                                    StringRef FuncName) {
  LLVMContext &Ctx = M.getContext();
  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), Bitcode.size());
  auto *Const = ConstantDataArray::get(
      Ctx, ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bitcode.data()),
                             Bitcode.size()));
  auto *GV = new GlobalVariable(
      M, ArrTy, /*isConstant=*/true, GlobalValue::InternalLinkage, Const,
      (Twine(GV_EJIT_BITCODE) + "_" + FuncName).str());
  GV->setAlignment(Align(1));
  return GV;
}

/// Get-or-create a declaration of an external symbol *inside* the registry
/// module (Area 3). The registry table lives in TmpM, so every address
/// constant it stores must reference a value owned by TmpM; referencing the
/// composite's function/global directly is a cross-module reference and is
/// illegal. Calling convention, function type and visibility are copied so the
/// final link resolves to the correct AOT symbol.
static Expected<Function *> externalFuncDeclIn(Module &TmpM,
                                               const Function &Src) {
  if (GlobalValue *Existing = TmpM.getNamedValue(Src.getName())) {
    auto *Decl = dyn_cast<Function>(Existing);
    if (!Decl || Decl->getFunctionType() != Src.getFunctionType() ||
        Decl->getAddressSpace() != Src.getAddressSpace())
      return createStringError(inconvertibleErrorCode(),
                               "incompatible duplicate external function '%s'",
                               Src.getName().str().c_str());
    return Decl;
  }

  auto *Decl =
      Function::Create(Src.getFunctionType(), GlobalValue::ExternalLinkage,
                       Src.getAddressSpace(), Src.getName(), &TmpM);
  Decl->setCallingConv(Src.getCallingConv());
  Decl->setAttributes(Src.getAttributes());
  Decl->setVisibility(Src.getVisibility());
  Decl->setDLLStorageClass(Src.getDLLStorageClass());
  return Decl;
}

static Expected<GlobalVariable *>
externalGlobalDeclIn(Module &TmpM, const GlobalVariable &Src) {
  if (GlobalValue *Existing = TmpM.getNamedValue(Src.getName())) {
    auto *Decl = dyn_cast<GlobalVariable>(Existing);
    if (!Decl || Decl->getValueType() != Src.getValueType() ||
        Decl->getAddressSpace() != Src.getAddressSpace())
      return createStringError(inconvertibleErrorCode(),
                               "incompatible duplicate external global '%s'",
                               Src.getName().str().c_str());
    return Decl;
  }

  auto *Decl = new GlobalVariable(
      TmpM, Src.getValueType(), Src.isConstant(), GlobalValue::ExternalLinkage,
      /*Init=*/nullptr, Src.getName(), /*InsertBefore=*/nullptr,
      Src.getThreadLocalMode(), Src.getAddressSpace());
  Decl->setVisibility(Src.getVisibility());
  Decl->setDLLStorageClass(Src.getDLLStorageClass());
  return Decl;
}

/// Build the static .ejit_bitcode registry table entirely inside TmpM.
///   { i32 type, ptr name1, ptr name2, ptr data, i64 size }
///     type 0 = EJIT_REG_BITCODE, type 3 = EJIT_REG_SYMBOL.
/// External functions and globals referenced by the closure are registered so
/// the JIT can resolve them without dlsym on bare-metal.
static void
generateRegistryTable(Module &TmpM, ArrayRef<Function *> EntryFuncs,
                      ArrayRef<GlobalVariable *> BitcodeGVs,
                      const SetVector<Function *> &ExternalFuncs,
                      const SetVector<GlobalVariable *> &ExternalGlobals) {
  LLVMContext &Ctx = TmpM.getContext();
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);
  StructType *EntryTy =
      StructType::get(Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, false);

  auto nameConst = [&](StringRef Name) -> Constant * {
    Constant *Str = ConstantDataArray::getString(Ctx, Name, true);
    auto *NameGV =
        new GlobalVariable(TmpM, Str->getType(), true,
                           GlobalValue::PrivateLinkage, Str, ".ejit.str.");
    return ConstantExpr::getBitCast(NameGV, PtrTy);
  };

  SmallVector<Constant *, 16> Entries;
  for (size_t I = 0; I < EntryFuncs.size(); ++I) {
    Entries.push_back(ConstantStruct::get(
        EntryTy,
        {ConstantInt::get(I32Ty, 0), // EJIT_REG_BITCODE
         nameConst(EntryFuncs[I]->getName()), ConstantPointerNull::get(PtrTy),
         ConstantExpr::getBitCast(BitcodeGVs[I], PtrTy),
         ConstantInt::get(I64Ty, cast<ArrayType>(BitcodeGVs[I]->getValueType())
                                     ->getNumElements())}));
  }
  for (Function *F : ExternalFuncs)
    Entries.push_back(ConstantStruct::get(
        EntryTy,
        {ConstantInt::get(I32Ty, 3), // EJIT_REG_SYMBOL
         nameConst(F->getName()), ConstantPointerNull::get(PtrTy),
         ConstantExpr::getBitCast(F, PtrTy), ConstantInt::get(I64Ty, 0)}));
  for (GlobalVariable *GV : ExternalGlobals)
    Entries.push_back(ConstantStruct::get(
        EntryTy,
        {ConstantInt::get(I32Ty, 3), // EJIT_REG_SYMBOL
         nameConst(GV->getName()), ConstantPointerNull::get(PtrTy),
         ConstantExpr::getBitCast(GV, PtrTy), ConstantInt::get(I64Ty, 0)}));

  auto *ArrTy = ArrayType::get(EntryTy, Entries.size());
  auto *TableGV = new GlobalVariable(
      TmpM, ArrTy, false, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrTy, Entries), ".ejit.registry.bitcode");
  TableGV->setSection(".ejit_bitcode");
  appendToCompilerUsed(TmpM, {TableGV});
}

/// Collect the external functions/globals a closure references, using the same
/// conservative rules as the closure walk, so every symbol kept as a
/// declaration in the extracted bitcode is registered.
static Error
collectExternalSymbols(const SetVector<Function *> &ClosureFuncs,
                       const SetVector<GlobalVariable *> &ClosureGlobals,
                       Module &RegistryM, SetVector<Function *> &ExternalFuncs,
                       SetVector<GlobalVariable *> &ExternalGlobals) {
  for (const Function *F : ClosureFuncs)
    if (F->isDeclaration() && !F->isIntrinsic()) {
      Expected<Function *> Decl = externalFuncDeclIn(RegistryM, *F);
      if (!Decl)
        return Decl.takeError();
      ExternalFuncs.insert(*Decl);
    }
  for (const GlobalVariable *GV : ClosureGlobals) {
    // A const global with a local definition is embedded in the bitcode and
    // needs no external resolution; everything else (extern decl, or a mutable
    // non-period global) must be registered so JITLink can resolve it.
    if (GV->isConstant() && !GV->isDeclaration())
      continue;
    if (GV->hasMetadata(MD_EJIT_METADATA) && !GV->isDeclaration())
      continue; // period array: resolved by the EJIT runtime itself
    Expected<GlobalVariable *> Decl = externalGlobalDeclIn(RegistryM, *GV);
    if (!Decl)
      return Decl.takeError();
    ExternalGlobals.insert(*Decl);
  }
  return Error::success();
}

} // end anonymous namespace

namespace lld {
namespace elf {

Expected<EJitCrossLinkResult> runEJitCrossLink(ArrayRef<std::string> InputFiles,
                                               StringRef TargetTriple,
                                               StringRef SaveTempsPrefix) {
  // ---- Phase 1: detect .ejit_cross. Distinguish "no section" (normal skip)
  // from "corrupt input" (Area 5). Non-object inputs (scripts, shared libs)
  // are skipped; archive members with .ejit_cross are collected for Phase 2.
  bool HasCross = false;
  StringSet<> CrossArchives; // archive paths whose members carry .ejit_cross
  for (StringRef F : InputFiles) {
    auto Buf = MemoryBuffer::getFile(F);
    if (!Buf)
      continue; // not a readable regular file (e.g. -l resolved elsewhere)
    file_magic Magic = identify_magic(Buf->get()->getBuffer());
    if (Magic == file_magic::archive) {
      auto ArOrErr = object::Archive::create(Buf->get()->getMemBufferRef());
      if (!ArOrErr)
        return crossError("read archive", F, ArOrErr.takeError());
      Error Err = Error::success();
      for (const auto &Child : (*ArOrErr)->children(Err)) {
        auto ChildBuf = Child.getMemoryBufferRef();
        if (!ChildBuf)
          return crossError("read archive member", F, ChildBuf.takeError());
        auto Obj = object::ObjectFile::createObjectFile(*ChildBuf);
        if (!Obj) {
          consumeError(Obj.takeError());
          continue;
        }
        for (const object::SectionRef &Sec : (*Obj)->sections()) {
          Expected<StringRef> N = Sec.getName();
          if (N && *N == kCrossSection) {
            HasCross = true;
            CrossArchives.insert(F);
            break;
          }
          if (!N)
            consumeError(N.takeError());
        }
        if (HasCross)
          break;
      }
      if (Err)
        return crossError("iterate archive", F, std::move(Err));
      continue;
    }
    auto Obj =
        object::ObjectFile::createObjectFile(Buf->get()->getMemBufferRef());
    if (!Obj) {
      consumeError(Obj.takeError());
      continue; // not a relocatable object we understand; skip
    }
    for (const object::SectionRef &Sec : (*Obj)->sections()) {
      Expected<StringRef> N = Sec.getName();
      if (!N) {
        consumeError(N.takeError());
        continue;
      }
      if (*N == kCrossSection) {
        HasCross = true;
        break;
      }
    }
  }
  if (!HasCross)
    return EJitCrossLinkResult{}; // nothing to do: normal AOT path is preserved

  // ---- Phase 2: parse and merge every .ejit_cross module into a composite.
  // From here on, any failure aborts the link (a .ejit_cross was promised).
  auto Ctx = std::make_unique<LLVMContext>();
  std::unique_ptr<Module> Composite;
  StringSet<> ConsumedFileSet;
  SmallVector<std::string, 4> ConsumedFiles;

  // Helper to process a single bitcode buffer into the composite.
  auto mergeBitcodeIntoComposite =
      [&](MemoryBufferRef BCBuf, StringRef DisplayName) -> Error {
    auto M = parseBitcodeFile(BCBuf, *Ctx);
    if (!M)
      return crossError("parse bitcode", DisplayName, M.takeError());
    if (ConsumedFileSet.insert(DisplayName).second)
      ConsumedFiles.push_back(DisplayName.str());
    if (!Composite) {
      Composite = std::move(*M);
      return Error::success();
    }
    if (Linker(*Composite).linkInModule(std::move(*M), Linker::Flags::None))
      return crossError("merge module", DisplayName,
                        "Linker::linkInModule reported a conflict while "
                        "merging modules");
    return Error::success();
  };

  for (StringRef F : InputFiles) {
    auto Buf = MemoryBuffer::getFile(F);
    if (!Buf)
      return crossError("read input", F, errorCodeToError(Buf.getError()));
    file_magic Magic = identify_magic(Buf->get()->getBuffer());

    if (Magic == file_magic::archive && CrossArchives.contains(F)) {
      auto ArOrErr = object::Archive::create(Buf->get()->getMemBufferRef());
      if (!ArOrErr)
        return crossError("read archive", F, ArOrErr.takeError());
      Error Err = Error::success();
      for (const auto &Child : (*ArOrErr)->children(Err)) {
        auto ChildBuf = Child.getMemoryBufferRef();
        if (!ChildBuf)
          return crossError("read archive member", F, ChildBuf.takeError());
        auto Obj = object::ObjectFile::createObjectFile(*ChildBuf);
        if (!Obj) {
          consumeError(Obj.takeError());
          continue;
        }
        for (const object::SectionRef &Sec : (*Obj)->sections()) {
          Expected<StringRef> N = Sec.getName();
          if (!N)
            return crossError("read section name", F, N.takeError());
          if (*N != kCrossSection)
            continue;
          Expected<StringRef> C = Sec.getContents();
          if (!C)
            return crossError("read section", F, C.takeError());
          // ChildBuf.getBufferIdentifier() returns the canonical
          // "archive_path(member_name)" string that lld's InputFiles
          // uses for ObjFile::getName(), so ConsumedFiles will match.
          StringRef DisplayName = ChildBuf->getBufferIdentifier();
          if (Error E = mergeBitcodeIntoComposite(
                  MemoryBufferRef(*C, DisplayName), DisplayName))
            return E;
        }
      }
      if (Err)
        return crossError("iterate archive", F, std::move(Err));
      continue;
    }

    if (Magic != file_magic::elf_relocatable &&
        Magic != file_magic::elf_shared_object &&
        Magic != file_magic::elf_executable)
      continue;
    auto Obj =
        object::ObjectFile::createObjectFile(Buf->get()->getMemBufferRef());
    if (!Obj)
      return crossError("parse object", F, Obj.takeError());
    for (const object::SectionRef &Sec : (*Obj)->sections()) {
      Expected<StringRef> N = Sec.getName();
      if (!N)
        return crossError("read section name", F, N.takeError());
      if (*N != kCrossSection)
        continue;
      Expected<StringRef> C = Sec.getContents();
      if (!C)
        return crossError("read section", F, C.takeError());
      if (Error E = mergeBitcodeIntoComposite(MemoryBufferRef(*C, F), F))
        return E;
    }
  }
  if (!Composite)
    return crossError("merge", "<inputs>",
                      "a .ejit_cross section was detected but no module could "
                      "be extracted");

  // ---- Phase 3: entry discovery.
  SmallVector<Function *, 4> EntryFuncs;
  collectEntryFunctions(*Composite, EntryFuncs);
  if (EntryFuncs.empty())
    return crossError("entry scan", "<composite>",
                      "merged .ejit_cross modules contain no ejit_entry "
                      "function");

  // ---- Phase 4: union closure + trim, then real inlining, then re-annotate.
  {
    SetVector<Function *> ClosureFuncs;
    SetVector<GlobalVariable *> ClosureGlobals;
    computeTransitiveClosure(EntryFuncs, ClosureFuncs, ClosureGlobals);
    trimToClosure(*Composite, ClosureFuncs, ClosureGlobals);
  }
  // The composite exists only to produce JIT bitcode; it is not the native AOT
  // object. Bind definitions from the merged TUs locally so ELF interposition
  // does not prevent the cost-model inliner from seeing ordinary helpers as
  // legal inline candidates. External declarations remain unresolved and are
  // registered below.
  for (Function &F : *Composite)
    if (!F.isDeclaration())
      F.setDSOLocal(true);
  runRealInliner(*Composite);
  reAnnotateMayConst(*Composite);

  // ---- Phase 5: per-entry bitcode + one registry, all owned by TmpM.
  auto TmpM = std::make_unique<Module>("ejit_cross_link", *Ctx);
  TmpM->setDataLayout(Composite->getDataLayoutStr());
  TmpM->setTargetTriple(Triple(TargetTriple.empty()
                                   ? Composite->getTargetTriple().str()
                                   : TargetTriple.str()));

  SmallVector<Function *, 4> ValidEntryFuncs;
  SmallVector<GlobalVariable *, 4> BitcodeGVs;
  SetVector<Function *> ExternalFuncs;
  SetVector<GlobalVariable *> ExternalGlobals;

  for (Function *F : EntryFuncs) {
    auto PerFunc = CloneModule(*Composite);
    Function *ClonedF = PerFunc->getFunction(F->getName());
    if (!ClonedF)
      return crossError("clone", F->getName(),
                        "entry function vanished after cloning the composite");

    SetVector<Function *> PerFuncs;
    SetVector<GlobalVariable *> PerGlobals;
    SmallVector<Function *, 1> Single{ClonedF};
    computeTransitiveClosure(Single, PerFuncs, PerGlobals);
    trimToClosure(*PerFunc, PerFuncs, PerGlobals);

    // Externalise non-const global definitions so JITLink resolves them from
    // the host image, mirroring the single-TU extractor.
    for (GlobalVariable &GV : PerFunc->globals())
      if (!GV.isDeclaration() && !GV.isConstant()) {
        GV.setInitializer(nullptr);
        GV.setLinkage(GlobalValue::ExternalLinkage);
      }

    if (Error E = collectExternalSymbols(PerFuncs, PerGlobals, *TmpM,
                                         ExternalFuncs, ExternalGlobals))
      return crossError("collect external symbols", F->getName(), std::move(E));

    if (verifyModule(*PerFunc, &errs()))
      return crossError("verify per-entry module", F->getName(),
                        "cloned/trimmed module failed verification");

    if (!SaveTempsPrefix.empty()) {
      std::string Path = saveTempEntryPath(SaveTempsPrefix, F->getName());
      if (Error E = writeModuleBitcode(*PerFunc, Path))
        return crossError("save per-entry bitcode", Path, std::move(E));
    }

    std::string BC;
    raw_string_ostream OS(BC);
    WriteBitcodeToFile(*PerFunc, OS);
    OS.flush();
    BitcodeGVs.push_back(embedBitcode(*TmpM, BC, F->getName()));
    ValidEntryFuncs.push_back(F);
  }

  generateRegistryTable(*TmpM, ValidEntryFuncs, BitcodeGVs, ExternalFuncs,
                        ExternalGlobals);

  // ---- Phase 6: verify the registry module before writing anything.
  if (verifyModule(*TmpM, &errs()))
    return crossError("verify registry module", "<ejit_cross_link>",
                      "generated registry module failed verification");

  // ---- Phase 7: write the merged bitcode to a temp file. Cleanup of the
  // temp file is owned by the caller (Driver.cpp), which unlinks it once the
  // link finishes and registers it for signal-time removal.
  SmallString<128> TmpPath;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("ejit_cross", "bc", TmpPath))
    return crossError("create temp file", TmpPath,
                      "cannot create temporary bitcode: " + EC.message());
  std::error_code EC;
  raw_fd_ostream Out(TmpPath, EC, sys::fs::OF_None);
  if (EC) {
    sys::fs::remove(TmpPath);
    return crossError("open temp file", TmpPath, EC.message());
  }
  WriteBitcodeToFile(*TmpM, Out);
  Out.flush();
  if (Out.has_error()) {
    std::error_code WE = Out.error();
    Out.clear_error();
    sys::fs::remove(TmpPath);
    return crossError("write temp file", TmpPath, WE.message());
  }
  EJitCrossLinkResult Result;
  Result.tempPath = std::string(TmpPath.str());
  Result.consumedFiles = std::move(ConsumedFiles);
  return Result;
}

} // namespace elf
} // namespace lld
