//===-- RISCVXRegScalarRepl.cpp - xreg scalar replacement across iterations ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xreg research (H4b): scalar replacement of array loads across iterations of
// an innermost loop ("predictive commoning" for loads), meant for machines
// with many registers.
//
// Loads whose addresses are affine in the loop with the same constant stride
// and constant distances between them (e.g. A[j-1], A[j], A[j+1]) read, in
// iteration i, the values that the load with the next-higher offset read in
// iteration i-1. Only the load with the highest offset is kept; the others
// become a chain of header phis (one register per offset), seeded by loads
// in the preheader.
//
// Legality: the loop is in rotated, simplified form with a single exit at the
// latch; every group load is simple and executes in every iteration (its
// block dominates the latch); no store or call in the loop may modify any
// location the group reads (alias analysis). Then the preheader loads touch
// only addresses between two addresses read in the first iteration.
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <map>

using namespace llvm;

#define DEBUG_TYPE "riscv-xreg-scalar-repl"

STATISTIC(NumGroups, "Number of load groups scalar-replaced");
STATISTIC(NumLoadsReplaced, "Number of loads replaced by registers");
STATISTIC(NumPhis, "Number of rotating registers (phis) created");

static cl::opt<bool> UnrollRotation(
    "riscv-xreg-sr-unroll", cl::Hidden, cl::init(true),
    cl::desc("xreg (H4b): ask the unroller to unroll a transformed loop by "
             "(largest span + 1), so the register rotation becomes renaming "
             "instead of copies"));

static cl::opt<unsigned> MaxSpan(
    "riscv-xreg-sr-max-span", cl::Hidden, cl::init(16),
    cl::desc("xreg (H4b): maximum offset span (registers) per load group"));

namespace {
struct Cand {
  LoadInst *Ld;
  const SCEVAddRecExpr *AR;
  int64_t Off = 0; // in strides, relative to the group's first load
};

class ScalarRepl {
  Loop *L;
  ScalarEvolution &SE;
  AAResults &AA;
  DominatorTree &DT;
  OptimizationRemarkEmitter &ORE;

public:
  ScalarRepl(Loop *L, ScalarEvolution &SE, AAResults &AA, DominatorTree &DT,
             OptimizationRemarkEmitter &ORE)
      : L(L), SE(SE), AA(AA), DT(DT), ORE(ORE) {}

  bool run() {
    BasicBlock *Pre = L->getLoopPreheader(), *H = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Pre || !Latch || L->getExitingBlock() != Latch ||
        !H->hasNPredecessors(2)) {
      LLVM_DEBUG(dbgs() << "xreg-sr: " << *L << " not in rotated simple form\n");
      return false;
    }

    SmallVector<Cand, 16> Cands;
    SmallVector<Instruction *, 8> Writers;
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB) {
        if (auto *Ld = dyn_cast<LoadInst>(&I)) {
          if (!Ld->isSimple() || !DT.dominates(BB, Latch))
            continue;
          auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(Ld->getPointerOperand()));
          if (!AR || AR->getLoop() != L || !AR->isAffine() ||
              !isa<SCEVConstant>(AR->getStepRecurrence(SE))) {
            LLVM_DEBUG(dbgs() << "xreg-sr: not affine: " << *Ld << " : "
                              << *SE.getSCEV(Ld->getPointerOperand()) << "\n");
            continue;
          }
          Cands.push_back({Ld, AR});
        } else if (I.mayWriteToMemory()) {
          Writers.push_back(&I);
        }
      }

    // Group loads with the same type and stride whose addresses differ by a
    // constant multiple of the stride.
    SmallVector<SmallVector<Cand, 8>, 8> Groups;
    for (Cand &C : Cands) {
      int64_t Step = cast<SCEVConstant>(C.AR->getStepRecurrence(SE))
                         ->getAPInt()
                         .getSExtValue();
      if (Step == 0)
        continue;
      bool Placed = false;
      for (auto &G : Groups) {
        Cand &R = G.front();
        if (R.Ld->getType() != C.Ld->getType() ||
            R.AR->getStepRecurrence(SE) != C.AR->getStepRecurrence(SE))
          continue;
        auto *D = dyn_cast<SCEVConstant>(SE.getMinusSCEV(C.AR, R.AR));
        if (!D)
          continue;
        int64_t Diff = D->getAPInt().getSExtValue();
        if (Diff % Step)
          continue;
        C.Off = Diff / Step;
        G.push_back(C);
        Placed = true;
        break;
      }
      if (!Placed) {
        C.Off = 0;
        Groups.push_back({C});
      }
    }

    unsigned MaxUsedSpan = 0;
    // Split each group into clusters of nearby offsets (e.g. one per row of
    // a stencil): a new cluster starts when the span would exceed MaxSpan.
    bool Changed = false;
    for (auto &G : Groups) {
      llvm::stable_sort(G, [](const Cand &A, const Cand &B) { return A.Off < B.Off; });
      for (size_t I = 0; I < G.size();) {
        size_t J = I;
        while (J < G.size() && uint64_t(G[J].Off - G[I].Off) <= MaxSpan)
          ++J;
        SmallVector<Cand, 8> Cluster(G.begin() + I, G.begin() + J);
        if (transform(Cluster, Writers, Pre, H, Latch)) {
          Changed = true;
          MaxUsedSpan = std::max<unsigned>(
              MaxUsedSpan, Cluster.back().Off - Cluster.front().Off);
        }
        I = J;
      }
    }
    if (Changed && UnrollRotation && !hasUnrollMetadata())
      addUnrollCount(MaxUsedSpan + 1);
    return Changed;
  }

private:
  bool hasUnrollMetadata() {
    if (MDNode *LoopID = L->getLoopID())
      for (const MDOperand &Op : drop_begin(LoopID->operands()))
        if (auto *MD = dyn_cast<MDNode>(Op))
          if (auto *S = dyn_cast<MDString>(MD->getOperand(0)))
            if (S->getString().starts_with("llvm.loop.unroll"))
              return true;
    return false;
  }

  void addUnrollCount(unsigned Count) {
    LLVMContext &Ctx = L->getHeader()->getContext();
    SmallVector<Metadata *, 4> MDs;
    MDs.push_back(nullptr); // self reference
    if (MDNode *LoopID = L->getLoopID())
      for (const MDOperand &Op : drop_begin(LoopID->operands()))
        MDs.push_back(Op.get());
    MDs.push_back(MDNode::get(
        Ctx, {MDString::get(Ctx, "llvm.loop.unroll.count"),
              ConstantAsMetadata::get(
                  ConstantInt::get(Type::getInt32Ty(Ctx), Count))}));
    MDNode *NewID = MDNode::getDistinct(Ctx, MDs);
    NewID->replaceOperandWith(0, NewID);
    L->setLoopID(NewID);
  }

  bool safe(ArrayRef<Cand> G, ArrayRef<Instruction *> Writers) {
    for (const Cand &C : G) {
      MemoryLocation Loc = MemoryLocation::getBeforeOrAfter(
          C.Ld->getPointerOperand(), C.Ld->getAAMetadata());
      for (Instruction *W : Writers)
        if (isModSet(AA.getModRefInfo(W, Loc)))
          return false;
    }
    return true;
  }

  bool transform(SmallVectorImpl<Cand> &G, ArrayRef<Instruction *> Writers,
                 BasicBlock *Pre, BasicBlock *H, BasicBlock *Latch) {
    int64_t Min = G[0].Off, Max = G[0].Off;
    for (Cand &C : G) {
      Min = std::min(Min, C.Off);
      Max = std::max(Max, C.Off);
    }
    LLVM_DEBUG(dbgs() << "xreg-sr: group of " << G.size() << " loads, span "
                      << Max - Min << " first " << *G[0].Ld << "\n");
    if (Max == Min || uint64_t(Max - Min) > MaxSpan)
      return false;
    if (!safe(G, Writers)) {
      LLVM_DEBUG(dbgs() << "xreg-sr:   unsafe (aliasing writer)\n");
      return false;
    }

    // The load at the highest offset reads a new location every iteration;
    // keep the first one (in the latch's dominator order) as the leader.
    LoadInst *Lead = nullptr;
    for (Cand &C : G)
      if (C.Off == Max && (!Lead || DT.dominates(C.Ld, Lead)))
        Lead = C.Ld;
    const SCEVAddRecExpr *LeadAR = nullptr;
    for (Cand &C : G)
      if (C.Ld == Lead)
        LeadAR = C.AR;

    Type *Ty = Lead->getType();
    const DataLayout &DL = H->getModule()->getDataLayout();
    Type *IdxTy = DL.getIndexType(Lead->getPointerOperandType());
    Align A = Lead->getAlign();
    for (Cand &C : G)
      A = std::min(A, C.Ld->getAlign());
    int64_t Step = cast<SCEVConstant>(LeadAR->getStepRecurrence(SE))
                       ->getAPInt()
                       .getSExtValue();

    SCEVExpander Exp(SE, "xreg.sr");
    Instruction *InsPt = Pre->getTerminator();
    IRBuilder<> B(InsPt);
    // Values of offsets Min..Max in the current iteration.
    SmallVector<Value *, 16> Val(Max - Min + 1, nullptr);
    Val[Max - Min] = Lead;
    for (int64_t O = Max - 1; O >= Min; --O) {
      // Address of offset O in iteration 0: Lead's start + (O - Max) strides.
      const SCEV *P = SE.getAddExpr(
          LeadAR->getStart(), SE.getConstant(IdxTy, (O - Max) * Step, true));
      Value *Ptr = Exp.expandCodeFor(P, Lead->getPointerOperandType(), InsPt);
      LoadInst *Init = B.CreateAlignedLoad(Ty, Ptr, A, "xreg.sr.init");
      Init->setAAMetadata(Lead->getAAMetadata());
      PHINode *Phi = PHINode::Create(Ty, 2, "xreg.sr");
      Phi->insertBefore(H->begin());
      Phi->addIncoming(Init, Pre);
      Val[O - Min] = Phi;
      ++NumPhis;
    }
    // Latch inputs: offset O in the next iteration = offset O+1 now.
    for (int64_t O = Min; O < Max; ++O)
      cast<PHINode>(Val[O - Min])->addIncoming(Val[O + 1 - Min], Latch);

    for (Cand &C : G)
      if (C.Ld != Lead) {
        C.Ld->replaceAllUsesWith(Val[C.Off - Min]);
        C.Ld->eraseFromParent();
        ++NumLoadsReplaced;
      }
    ++NumGroups;
    ORE.emit([&]() {
      return OptimizationRemark(DEBUG_TYPE, "ScalarReplaced", Lead)
             << "replaced " << ore::NV("Loads", unsigned(G.size() - 1))
             << " loads with " << ore::NV("Registers", unsigned(Max - Min))
             << " rotating registers";
    });
    return true;
  }
};
} // namespace

PreservedAnalyses RISCVXRegScalarReplPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &AA = AM.getResult<AAManager>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder())
    if (L->isInnermost())
      Changed |= ScalarRepl(L, SE, AA, DT, ORE).run();
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
