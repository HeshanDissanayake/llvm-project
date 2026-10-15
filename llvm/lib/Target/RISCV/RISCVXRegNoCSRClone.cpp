//===-- RISCVXRegNoCSRClone.cpp - xreg local clones for no-CSR calls ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xreg research (H9): with IPRA, LLVM already lets a function save no
// callee-saved registers (only ra) when all its callers are known: local
// linkage, address not taken, norecurse and never tail-called
// (TargetFrameLowering::isSafeForNoCSROpt). Each caller then gets the
// callee's exact clobber mask and keeps its live values in registers the
// callee doesn't touch. Most C functions have external linkage, so they never
// qualify, although most of their calls come from the same module.
//
// -riscv-xreg-nocsr-clone: for every defined external function F with direct
// calls from this module, create an internal copy F.xnocsr and redirect those
// calls to it. Calls from other modules still reach F, which keeps the normal
// convention, so this is safe without whole-program compilation. The copy is
// only made when it is not part of a cycle of direct calls within the module
// (so it can be marked norecurse); the redirected calls lose their "tail"
// marker. Calls to a clone use a preserve-nothing mask unless IPRA knows the
// clone's real clobbers (RISCVISelLowering), so codegen order can't make a
// caller assume registers survive. Needs -enable-ipra to have any effect.
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-xreg-nocsr-clone"

STATISTIC(NumCloned, "Number of functions given an internal no-CSR clone");
STATISTIC(NumRedirected, "Number of call sites redirected to a clone");

// Direct callees of F that are defined in this module.
static void definedCallees(Function &F, SmallVectorImpl<Function *> &Out) {
  for (Instruction &I : instructions(F))
    if (auto *CB = dyn_cast<CallBase>(&I))
      if (Function *G = CB->getCalledFunction())
        if (!G->isDeclaration())
          Out.push_back(G);
}

// True if F can reach itself through direct calls within the module.
static bool reachesItself(Function &F) {
  SmallVector<Function *, 16> Work;
  DenseSet<Function *> Seen;
  definedCallees(F, Work);
  while (!Work.empty()) {
    Function *G = Work.pop_back_val();
    if (G == &F)
      return true;
    if (Seen.insert(G).second)
      definedCallees(*G, Work);
  }
  return false;
}

PreservedAnalyses RISCVXRegNoCSRClonePass::run(Module &M,
                                               ModuleAnalysisManager &) {
  if (!RISCVXReg::noCSRClone())
    return PreservedAnalyses::all();

  SmallVector<std::pair<Function *, SmallVector<CallBase *, 4>>, 16> Todo;
  for (Function &F : M) {
    if (F.isDeclaration() || F.hasLocalLinkage() || F.isVarArg() ||
        F.hasFnAttribute(Attribute::Naked) || F.hasFnAttribute("interrupt") ||
        F.getName() == "main")
      continue;
    SmallVector<CallBase *, 4> Calls;
    bool Ok = true;
    for (User *U : F.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &F)
        continue; // other uses (address taken) keep using F
      if (CB->isMustTailCall()) {
        Ok = false;
        break;
      }
      Calls.push_back(CB);
    }
    if (!Ok || Calls.empty() || reachesItself(F))
      continue;
    Todo.push_back({&F, Calls});
  }

  DenseMap<Function *, Function *> Clone;
  for (auto &[F, Calls] : Todo) {
    ValueToValueMapTy VMap;
    Function *C = CloneFunction(F, VMap);
    C->setName(F->getName() + ".xnocsr");
    C->setLinkage(GlobalValue::InternalLinkage);
    C->setVisibility(GlobalValue::DefaultVisibility);
    C->setDSOLocal(true);
    C->setComdat(nullptr);
    C->addFnAttr(Attribute::NoRecurse);
    C->addFnAttr("xreg-nocsr");
    Clone[F] = C;
    ++NumCloned;
  }
  // Redirect every direct in-module call (including those inside the clones).
  for (Function &G : M)
    for (Instruction &I : instructions(G))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (Function *Callee = CB->getCalledFunction())
          if (Function *C = Clone.lookup(Callee)) {
            CB->setCalledFunction(C);
            if (auto *CI = dyn_cast<CallInst>(CB))
              CI->setTailCallKind(CallInst::TCK_None);
            ++NumRedirected;
          }
  return Todo.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
}
