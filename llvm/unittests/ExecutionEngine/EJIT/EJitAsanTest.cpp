//===-- EJitAsanTest.cpp - Worker-only substitution checks ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitAsan.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <array>
#include <sstream>

using namespace llvm;
using namespace llvm::ejit;

namespace {
std::unique_ptr<Module> makeProbe(LLVMContext &C, bool Local, unsigned Mod) {
  std::string IR = R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    @cfg = external global [2 x [10 x i32]], !ejit.metadata !0
    declare i16 @__ejit_local_free_dim_v1_i16(i16) #0
    define i32 @probe(i32 %cell, i16 %slot) !ejit.metadata !3 {
  )";
  if (Local)
    IR += "%local = call i16 @__ejit_local_free_dim_v1_i16(i16 %slot)\n";
  IR += std::string("%r = urem i16 ") + (Local ? "%local, " : "%slot, ") +
        std::to_string(Mod) + "\n";
  IR += R"(
      %ci = zext i32 %cell to i64
      %ri = zext i16 %r to i64
      %p = getelementptr inbounds [2 x [10 x i32]], ptr @cfg,
                                  i64 0, i64 %ci, i64 %ri
      %v = load i32, ptr %p, align 4, !ejit.period_const !7
      ret i32 %v
    }
    attributes #0 = { nounwind willreturn memory(none) nocallback nosync "ejit.local_free_dim"="v1" }
    !0 = !{!1, !2}
    !1 = !{!"ejit_group_indexed", !"cell", i64 2}
    !2 = !{!"ejit_period_const_field", i64 0}
    !3 = distinct !{!4, !5, !6}
    !4 = !{!"ejit_entry"}
    !5 = !{!"ejit_dim", !"cell", i32 0}
    !6 = !{!"ejit_free_dim", !"", i32 1}
    !7 = !{}
  )";
  SMDiagnostic Err;
  return parseAssemblyString(IR, Err, C);
}

std::string printModule(const Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}

void codeMarker() {}
} // namespace

TEST(EJitAsan, CapturesEveryResidueWithoutChangingIR) {
  for (bool Local : {false, true})
    for (unsigned Mod : {5u, 10u}) {
      SCOPED_TRACE(::testing::Message() << Local << ':' << Mod);
      uint32_t Data[2][10];
      for (unsigned I = 0; I < 10; ++I) {
        Data[0][I] = 7;
        Data[1][I] = I == Mod - 1 ? 13 : 11;
      }
      PeriodArrayRegistry Registry;
      Registry.registerArray("cell", "cfg", Data, 2);
      LLVMContext WithContext, WithoutContext;
      auto With = makeProbe(WithContext, Local, Mod);
      auto Without = makeProbe(WithoutContext, Local, Mod);
      ASSERT_TRUE(With);
      ASSERT_TRUE(Without);
      SpecializationContext Ctx;
      Ctx.fnName = "probe";
      Ctx.dimensions.push_back({"cell", 1});
      EJitOptimizer Reference(Registry);
      Reference.runPipeline(*Without, Ctx);
      EJitAsanCapture Capture;
      Ctx.asanCapture = &Capture;
      EJitOptimizer Observed(Registry);
      Observed.runPipeline(*With, Ctx);
      EXPECT_EQ(printModule(*With), printModule(*Without));
      EXPECT_EQ(Capture.uncovered, 0u);
      ASSERT_EQ(Capture.watches.size(), 1u);
      const auto &W = Capture.watches.front();
      EXPECT_EQ(W.period, "cell");
      EXPECT_EQ(W.object, "cfg");
      EXPECT_EQ(W.objectBase, reinterpret_cast<uintptr_t>(Data));
      EXPECT_EQ(W.freeArg, 1);
      EXPECT_EQ(W.expected, 11u);
      ASSERT_EQ(W.addresses.size(), Mod);
      for (unsigned R = 0; R < Mod; ++R)
        EXPECT_EQ(W.addresses[R], reinterpret_cast<uintptr_t>(&Data[1][R]));
    }
}

TEST(EJitAsan, CapturesPhiWitnessFoldUnderWitnessBinding) {
  // A period_const load whose byte-offset GEP chain passes through a merge
  // PHI only folds via the phi-witness path: every feasible PHI binding
  // must fold to the same constant, and the fold exports the binding it
  // proved. Capture must keep up — the residue-domain walk consults
  // Assumed.phiInputs instead of rejecting every such fold as uncovered.
  // The fixture mirrors the proven EJitPhiWitness shapes: a static-root
  // object, a local-free-dim slot urem, and two constant PHI edges
  // (records 0 and 1) whose members repeat, so both bindings agree.
  constexpr unsigned RecordSize = 1240, Slots = 5;
  constexpr unsigned FieldOffset = 12, FieldAlign = 4;
  const uint64_t Expected = 1234;
  alignas(64) std::array<uint8_t, 19 * RecordSize * Slots> Data{};
  for (unsigned I = 0; I < 19; ++I)
    for (unsigned S = 0; S < Slots; ++S)
      for (unsigned B = 0; B < 4; ++B)
        Data[I * RecordSize * Slots + S * RecordSize + FieldOffset + B] =
            Expected >> (8 * B);

  auto Build = [&](LLVMContext &C, std::unique_ptr<Module> &Out) {
    std::ostringstream IR;
    IR << "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
          "%R = type { [12 x i8], i32, [5 x i8], i8, i8, [1217 x i8] }\n"
          "@root = external global [18 x [5 x %R]], !ejit.metadata !0\n"
          "declare i16 @__ejit_local_free_dim_v1_i16(i16) #0\n"
          "declare void @consume(i32, i16, ptr)\n"
          "define void @probe(i8 %cell, ptr %ctx, ptr %flags) !ejit.metadata "
          "!5 { ret void }\n"
          "attributes #0 = { nounwind willreturn memory(none) nocallback "
          "nosync \"ejit.local_free_dim\"=\"v1\" }\n"
          "!0 = !{!1";
    for (unsigned I = 0; I < 18 * Slots; ++I)
      IR << ", !" << 100 + I;
    IR << "}\n"
          "!1 = !{!\"ejit_group\", !\"static\"}\n"
          "!5 = distinct !{!6, !7}\n"
          "!6 = !{!\"ejit_entry\"}\n"
          "!7 = !{!\"ejit_dim\", !\"cell\", i32 0}\n";
    for (unsigned I = 0; I < 18 * Slots; ++I)
      IR << "!" << 100 + I << " = !{!\"ejit_period_const_field\", i64 "
         << I * RecordSize + FieldOffset << "}\n";
    SMDiagnostic Err;
    Out = parseAssemblyString(IR.str(), Err, C);
    ASSERT_TRUE(Out);
    Function *F = Out->getFunction("probe");
    F->getEntryBlock().eraseFromParent();
    IRBuilder<> B(C);
    auto Block = [&](const char *Name) {
      return BasicBlock::Create(C, Name, F);
    };
    BasicBlock *Entry = Block("entry"), *Default = Block("default"),
               *Dynamic = Block("dynamic"), *Valid = Block("valid"),
               *Merge = Block("merge"), *Exit = Block("exit");
    B.SetInsertPoint(Entry);
    Value *SlotP = B.CreateGEP(B.getInt8Ty(), F->getArg(1), B.getInt64(20));
    Value *RawSlot = B.CreateAlignedLoad(B.getInt16Ty(), SlotP, Align(4));
    Value *Slot = B.CreateCall(
        Out->getFunction("__ejit_local_free_dim_v1_i16"), {RawSlot});
    Slot = B.CreateZExt(B.CreateURem(Slot, B.getInt16(Slots)),
                        B.getInt64Ty());
    Value *Flag = B.CreateAlignedLoad(B.getInt8Ty(), F->getArg(2), Align(1));
    Value *Cond = B.CreateICmpNE(Flag, B.getInt8(0));
    B.CreateCondBr(Cond, Dynamic, Default);
    B.SetInsertPoint(Default);
    B.CreateBr(Merge);
    B.SetInsertPoint(Dynamic);
    Value *InputP = B.CreateGEP(B.getInt8Ty(), F->getArg(2), B.getInt64(17));
    Value *Raw = B.CreateAlignedLoad(B.getInt8Ty(), InputP, Align(1));
    B.CreateCondBr(B.CreateICmpULT(Raw, B.getInt8(18)), Valid, Exit);
    B.SetInsertPoint(Valid);
    B.CreateBr(Merge);
    B.SetInsertPoint(Merge);
    auto *Index = B.CreatePHI(B.getInt64Ty(), 2, "choice");
    // Record 1 on the default edge, record 0 on the guarded edge: constant
    // incoming values, so every feasible binding is enumerable, and the two
    // records repeat the same field value so the fold proves them equal.
    Index->addIncoming(B.getInt64(RecordSize * Slots), Default);
    Index->addIncoming(B.getInt64(0), Valid);
    Value *Ptr =
        B.CreateInBoundsGEP(B.getInt8Ty(), Out->getGlobalVariable("root"),
                            Index);
    Ptr = B.CreateInBoundsGEP(StructType::getTypeByName(C, "R"), Ptr, Slot);
    Ptr = B.CreateInBoundsGEP(B.getInt8Ty(), Ptr, B.getInt64(FieldOffset));
    auto *Load = B.CreateAlignedLoad(B.getInt32Ty(), Ptr, Align(FieldAlign));
    Load->setMetadata("ejit.period_const", MDNode::get(C, {}));
    B.CreateCall(Out->getFunction("consume"), {Load, RawSlot, Ptr});
    B.CreateBr(Exit);
    B.SetInsertPoint(Exit);
    B.CreateRetVoid();
    ASSERT_FALSE(verifyModule(*Out, &errs()));
  };

  LLVMContext WithContext, WithoutContext;
  std::unique_ptr<Module> With, Without;
  Build(WithContext, With);
  Build(WithoutContext, Without);
  PeriodArrayRegistry Registry;
  Registry.registerStaticVar("root", Data.data());
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 0});
  Ctx.optLevel = OptimizationLevel::L2;
  Ctx.tier = CompileTier::Baseline;
  EJitOptimizer Reference(Registry);
  Reference.runPipeline(*Without, Ctx);
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;
  EJitOptimizer Observed(Registry);
  Observed.runPipeline(*With, Ctx);
  EXPECT_EQ(printModule(*With), printModule(*Without));
  EXPECT_EQ(Capture.uncovered, 0u);
  ASSERT_EQ(Capture.watches.size(), 1u);
  const auto &W = Capture.watches.front();
  EXPECT_EQ(W.function, "probe");
  EXPECT_EQ(W.period, "static");
  EXPECT_EQ(W.object, "root");
  EXPECT_EQ(W.objectBase, reinterpret_cast<uintptr_t>(Data.data()));
  // The slot dimension is driven by a local free-dim marker wrapping a
  // load, not a function argument: the row reports src=const.
  EXPECT_EQ(W.freeArg, 0xFF);
  EXPECT_EQ(W.expected, Expected);
  // The residue enumeration evaluates under the exported witness binding
  // (record 0), so the probed addresses are record 0's slots.
  ASSERT_EQ(W.addresses.size(), Slots);
  for (unsigned R = 0; R < Slots; ++R)
    EXPECT_EQ(W.addresses[R],
              reinterpret_cast<uintptr_t>(Data.data() + R * RecordSize +
                                         FieldOffset));
}

TEST(EJitAsan, ReportsTailResidueOnlyAfterPublication) {
  auto State = std::make_unique<EJitSharedTaskPoolState>();
  EJitSharedTaskPool Pool;
  Pool.bind(State.get());
  Pool.setMode(EJitCompileMode::Async);
  EJitAsan Verifier;
  uint32_t Data[5] = {7, 7, 7, 7, 7};
  uintptr_t Root = reinterpret_cast<uintptr_t>(Data);
  struct Context {
    EJitAsan &verifier;
    uint32_t (&data)[5];
    uintptr_t &root;
  } Ctx{Verifier, Data, Root};
  Pool.setCompiler(
      [](void *Ptr, const EJitCompileRequest &Req, void **Out) {
        auto &C = *static_cast<Context *>(Ptr);
        EJitAsanCapture Capture;
        auto &W = Capture.watches.emplace_back();
        W.function = "probe";
        W.object = "cfg";
        W.period = "cell";
        W.objectBase = reinterpret_cast<uintptr_t>(C.data);
        W.freeArg = 0;
        W.width = 32;
        W.expected = 7;
        uint32_t Endian = 1;
        W.littleEndian = *reinterpret_cast<uint8_t *>(&Endian) != 0;
        for (auto &Value : C.data)
          W.addresses.push_back(reinterpret_cast<uintptr_t>(&Value));
        EJitAsanWatch Leaf;
        Leaf.function = "probe";
        Leaf.object = "pointer_cfg";
        Leaf.period = "cell";
        Leaf.objectBase = C.root;
        Leaf.width = 32;
        Leaf.expected = 7;
        Leaf.littleEndian = W.littleEndian;
        Leaf.rootSlot = reinterpret_cast<uintptr_t>(&C.root);
        Leaf.expectedRoot = C.root;
        Leaf.addresses.push_back(C.root);
        Capture.watches.push_back(std::move(Leaf));
        *Out = reinterpret_cast<void *>(&codeMarker);
        C.verifier.onCompiled(Req, reinterpret_cast<uintptr_t>(*Out),
                             std::move(Capture));
        return true;
      },
      &Ctx);
  Pool.setPublishCallback(
      [](void *Ptr, const EJitCompileRequest &Req, bool Published) {
        static_cast<Context *>(Ptr)->verifier.onPublished(Req, Published);
      },
      &Ctx);
  ASSERT_EQ(Pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_TRUE(Pool.setInstanceEnabled(0, 1, true));
  const EJitDimPair Dim{0, 1};
  ASSERT_EQ(Pool.compileOrGet(44, &Dim, 1,
                             reinterpret_cast<void *>(&codeMarker)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  auto Dump = [&] {
    testing::internal::CaptureStdout();
    EXPECT_TRUE(Verifier.dumpNow(Pool));
    return testing::internal::GetCapturedStdout();
  };
  EXPECT_NE(Dump().find("checks=0"), std::string::npos);
  ASSERT_TRUE(Pool.pollOne());
  auto Live = Dump();
  EXPECT_NE(Live.find("checked_active=6/6"), std::string::npos);
  EXPECT_NE(Live.find("watches=2/81920"), std::string::npos);
  __atomic_store_n(&Data[4], 9u, __ATOMIC_RELEASE);
  auto Mismatch = Dump();
  EXPECT_NE(Mismatch.find("residue=4/5 kind=value"), std::string::npos);
  EXPECT_NE(Mismatch.find("expected=0x7 first=0x9"), std::string::npos);
  EXPECT_NE(Mismatch.find("object=cfg@0x"), std::string::npos);
  EXPECT_NE(Mismatch.find("offset=0x10"), std::string::npos);
  EXPECT_NE(Mismatch.find("src=arg0 "), std::string::npos);
  // The rate is the location's lifetime ratio (samples accumulate from
  // before the first mismatch too), so its exact value depends on scan
  // rotation; assert presence and a non-zero ratio instead of an exact one.
  EXPECT_NE(Mismatch.find("count="), std::string::npos);
  EXPECT_NE(Mismatch.find("rate="), std::string::npos);
  EXPECT_EQ(Mismatch.find("rate=0.0%"), std::string::npos);
  // Sticky records: the recorded location re-reports on every dump; it is
  // not unregistered while still mismatching.
  EXPECT_NE(Dump().find("residue=4/5"), std::string::npos);
  __atomic_store_n(&Root, uintptr_t(1), __ATOMIC_RELEASE);
  auto ChangedRoot = Dump();
  EXPECT_NE(ChangedRoot.find("object=pointer_cfg@0x"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("kind=pointer-root"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("first=0x1"), std::string::npos);
  EXPECT_EQ(ChangedRoot.find("blocked_root=0"), std::string::npos);
  // Recovery does not unregister a recorded location either: the rows stay
  // listed with their frozen counts while samples keep growing, so the
  // printed rate drops below 100%.
  __atomic_store_n(&Data[4], 7u, __ATOMIC_RELEASE);
  __atomic_store_n(&Root, reinterpret_cast<uintptr_t>(Data),
                   __ATOMIC_RELEASE);
  auto Recovered = Dump();
  EXPECT_NE(Recovered.find("residue=4/5"), std::string::npos);
  EXPECT_NE(Recovered.find("residue=0/1"), std::string::npos);
  EXPECT_EQ(Recovered.find("rate=100.0%"), std::string::npos);
  EXPECT_TRUE(Pool.setInstanceEnabled(0, 1, false));
  EXPECT_NE(Dump().find("versions=0 staged=0 watches=0/81920"),
            std::string::npos);
  ejit_set_log_level(Level);
  Pool.ownerShutdown();
}
