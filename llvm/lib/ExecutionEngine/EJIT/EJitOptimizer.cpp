//===-- EJitOptimizer.cpp - JIT Optimization Pipeline ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"
#include "llvm/Support/Debug.h"
// JIT Inline disabled: AOT pre-optimization already inlines.
// #include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-optimizer"

EJitOptimizer::EJitOptimizer(PeriodArrayRegistry &reg)
    : registry_(reg) {
  EJitPassBuilder::registerFunctionAnalyses(FAM_);
  EJitPassBuilder::registerLoopAnalyses(LAM_);
  EJitPassBuilder::registerCGSCCAnalyses(CGAM_);
  EJitPassBuilder::registerModuleAnalyses(MAM_);
  EJitPassBuilder::crossRegisterProxies(LAM_, FAM_, CGAM_, MAM_);

  // Pre-build the two cached FunctionPassManagers of the optimization pipeline.
  //
  // By the time these run, runPipeline has turned the period-index argument and
  // every may_const field into a compile-time constant. The pipeline's whole job
  // is to exploit those constants maximally, so it is structured as a fold →
  // propagate → simplify fixed point followed by loop folding.
  //
  // mainFPM_ — Phase 2 (scalar) then Phase 3 (loops):
  //
  //   Phase 2 drives the substituted constants to a fixed point. The first round
  //   peephole-folds the constants (InstCombine), propagates them and folds the
  //   now-constant branches (SCCP), and deletes the unreachable blocks
  //   (SimplifyCFG). Folding a branch merges blocks and exposes fresh constants
  //   and dead code, so a second InstCombine + SimplifyCFG round catches that
  //   cascade; ADCE then removes whatever became dead.
  mainFPM_.addPass(InstCombinePass());
  mainFPM_.addPass(SCCPPass());
  mainFPM_.addPass(SimplifyCFGPass());
  mainFPM_.addPass(InstCombinePass());
  mainFPM_.addPass(SimplifyCFGPass());
  mainFPM_.addPass(ADCEPass());
  //   Phase 3 folds loops whose bounds became constant (a no-op on loopless
  //   functions). LoopSimplify canonicalizes; LoopFullUnroll unrolls small
  //   bounded loops; IndVarSimplify uses SCEV to replace a larger loop's
  //   accumulator phi with its closed-form exit value; LoopDeletion removes the
  //   emptied loop; Promote returns any loop-created allocas to SSA.
  mainFPM_.addPass(LoopSimplifyPass());
  {
    LoopPassManager LPM;
    LPM.addPass(LoopFullUnrollPass());
    LPM.addPass(IndVarSimplifyPass());
    LPM.addPass(LoopDeletionPass());
    mainFPM_.addPass(createFunctionToLoopPassAdaptor(std::move(LPM)));
  }
  mainFPM_.addPass(PromotePass());

  // cleanupFPM_ — Phase 4, run after the second StructFieldPass. Unrolling turns
  // a loop-variant array access g_arr[k].field into constant-index GEPs
  // (g_arr[0]/[1]/...), which only then become substitutable. Once
  // StructFieldPass replaces them, this fold/propagate/simplify pass collapses
  // the freshly-constant values and drops what became dead — the same treatment
  // Phase 2 gave the first wave of constants.
  cleanupFPM_.addPass(InstCombinePass());
  cleanupFPM_.addPass(SCCPPass());
  cleanupFPM_.addPass(SimplifyCFGPass());
  cleanupFPM_.addPass(ADCEPass());
}

void EJitOptimizer::clearAnalyses() {
  FAM_.clear();
  LAM_.clear();
  CGAM_.clear();
  MAM_.clear();
}

void EJitOptimizer::runPipeline(Module &M,
                                const SpecializationContext &ctx) {
  EJIT_DIAG_VERBOSE("pipeline begin func=%s key=0x%016lx opt=%d dims=%zu "
                    "module=%s",
                    ctx.fnName.c_str(), ctx.cacheKey,
                    static_cast<int>(ctx.optLevel), ctx.dimensions.size(),
                    M.getName().str().c_str());

  // Phase 1 — specialize: turn the period index and every may_const field into
  // a compile-time constant.
  //   (a) Substitute the ejit_period_arr_ind argument with its constant index.
  preReplacePeriodIndices(M, ctx);
  EJIT_DIAG_DEBUG("pipeline phase1a done: preReplacePeriodIndices");
  //   (b) Fold the constant GEP chains that exposes so StructFieldPass can
  //       compute the byte offset of each may_const field access.
  runInstCombine(M);
  EJIT_DIAG_DEBUG("pipeline phase1b done: InstCombine");
  // Inlining is intentionally not run here: the AOT pre-optimization
  // (EJitRegisterBitcodePass: AlwaysInline + ModuleInliner(O2)) already expanded
  // callee bodies in the embedded bitcode, so their may_const GEP chains are
  // already traceable to the global.
  //   (c) Replace the may_const loads with their runtime constant values.
  runStructFieldPass(M);
  EJIT_DIAG_DEBUG("pipeline phase1c done: StructFieldPass");

  // Phases 2-4 — exploit those constants (scalar fixed point → loops →
  // re-specialize → cleanup). ctx.optLevel is accepted for ABI compatibility
  // and does not affect the pipeline.
  runOptimizationPipeline(M, ctx.optLevel);
  EJIT_DIAG_VERBOSE("pipeline done func=%s key=0x%016lx", ctx.fnName.c_str(),
                    ctx.cacheKey);
}

void EJitOptimizer::preReplacePeriodIndices(
    Module &M, const SpecializationContext &ctx) {
  LLVM_DEBUG(dbgs() << "ejit-optimizer: preReplacePeriodIndices, "
                    << ctx.dimensions.size() << " dim(s)\n");
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 3)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
        continue;

      auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
      if (!PN || !IdxC)
        continue;

      unsigned argIdx = static_cast<unsigned>(IdxC->getZExtValue());
      if (argIdx >= F.arg_size())
        continue;

      for (auto &dim : ctx.dimensions) {
        if (dim.periodName == PN->getString()) {
          Argument *arg = F.getArg(argIdx);
          arg->replaceAllUsesWith(
              ConstantInt::get(arg->getType(), dim.cellIdx));
          break;
        }
      }
    }
  }
}

void EJitOptimizer::runInstCombine(Module &M) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());

  for (Function &F : M.functions())
    if (!F.isDeclaration())
      FPM.run(F, FAM_);
}

void EJitOptimizer::runStructFieldPass(Module &M) {
  EJitStructFieldPass structField(registry_);
  structField.initFromModule(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      structField.run(F, FAM_);
}

void EJitOptimizer::runOptimizationPipeline(Module &M,
                                            OptimizationLevel level) {
  // One fixed pipeline; `level` does not affect it.
  (void)level;
  EJIT_DIAG_DEBUG("pipeline stage5: optimization pipeline module=%s",
                  M.getName().str().c_str());

  // Phases 2-3: scalar fold/propagate/simplify fixed point, then loop folding.
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      mainFPM_.run(F, FAM_);

  // Phase 4: unrolling exposed new constant-index array accesses
  // (g_arr[k].field → g_arr[0].field, g_arr[1].field, ...). Substitute them,
  // then fold/propagate/simplify the freshly-constant values.
  runStructFieldPass(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      cleanupFPM_.run(F, FAM_);
}
