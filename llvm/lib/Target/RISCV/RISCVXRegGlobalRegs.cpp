//===-- RISCVXRegGlobalRegs.cpp - xreg whole-program register globals -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xreg research (H6): keep selected scalar globals in dedicated registers for
// the whole program, like GCC's global register variables.
//
// -riscv-xreg-gregs=g0[:bits],g1[:bits],... maps global g<i> to x(255-i);
// the optional width is used by main to copy the initial value of a global
// that main's module doesn't declare (default 64). The same list
// must be given to every translation unit of the program; the registers must
// be outside the allocatable range (-riscv-num-gprs <= 256 - #globals), so no
// function ever allocates them. In each module, every load of a listed global
// becomes a read of its register and every store a write. The global stays in
// memory for its initial value: the definition of `main` copies each listed
// global from memory into its register on entry.
//
// Only direct, simple loads and stores of integer (<= 64 bits) or pointer
// type are supported. Any other use (address taken, volatile, aggregate) is a
// fatal error, so an unsuitable list fails to compile instead of
// miscompiling. Choosing the list (which globals, and that names are unique
// program-wide) is done outside the compiler (scripts/xreg_gregs.py).
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-xreg-gregs"

STATISTIC(NumGlobals, "Number of globals mapped to registers");
STATISTIC(NumLoads, "Number of global loads replaced by register reads");
STATISTIC(NumStores, "Number of global stores replaced by register writes");

static cl::list<std::string>
    GlobalRegs("riscv-xreg-gregs", cl::Hidden, cl::CommaSeparated,
               cl::desc("xreg (H6): globals kept in registers x255, x254, ... "
                        "for the whole program"));

ArrayRef<std::string> RISCVXReg::getGlobalRegNames() { return GlobalRegs; }

static Value *toI64(IRBuilder<> &B, Value *V) {
  Type *T = V->getType();
  if (T->isPointerTy())
    return B.CreatePtrToInt(V, B.getInt64Ty());
  return B.CreateZExtOrTrunc(V, B.getInt64Ty());
}

static Value *fromI64(IRBuilder<> &B, Value *V, Type *T) {
  if (T->isPointerTy())
    return B.CreateIntToPtr(V, T);
  return B.CreateTrunc(V, T);
}

PreservedAnalyses RISCVXRegGlobalRegsPass::run(Module &M,
                                               ModuleAnalysisManager &) {
  SmallVector<std::string, 32> Names;
  SmallVector<unsigned, 32> Bits;
  for (StringRef E : GlobalRegs) {
    auto [N, W] = E.split(':');
    unsigned B = 64;
    if (!W.empty() && (W.getAsInteger(10, B) || !B || B > 64))
      reportFatalUsageError(Twine("-riscv-xreg-gregs: bad width in '") + E + "'");
    Names.push_back(N.str());
    Bits.push_back(B);
  }
  if (Names.empty())
    return PreservedAnalyses::all();
  if (RISCVXReg::getNumGPRs() > 256 - Names.size())
    reportFatalUsageError("-riscv-xreg-gregs: the global registers must not be "
                          "allocatable (-riscv-num-gprs <= 256 - #globals)");
  LLVMContext &Ctx = M.getContext();
  Function *ReadReg = Intrinsic::getOrInsertDeclaration(
      &M, Intrinsic::read_register, {Type::getInt64Ty(Ctx)});
  Function *WriteReg = Intrinsic::getOrInsertDeclaration(
      &M, Intrinsic::write_register, {Type::getInt64Ty(Ctx)});

  SmallVector<std::pair<GlobalVariable *, MetadataAsValue *>, 32> Mapped;
  for (unsigned I = 0; I < Names.size(); ++I) {
    GlobalVariable *G = M.getGlobalVariable(Names[I], /*AllowInternal=*/true);
    if (!G)
      continue;
    std::string RegName = "x" + std::to_string(255 - I);
    auto *RegMD = MetadataAsValue::get(
        Ctx, MDNode::get(Ctx, {MDString::get(Ctx, RegName)}));
    Type *VT = G->getValueType();
    if (!(VT->isIntegerTy() && VT->getIntegerBitWidth() <= 64) &&
        !VT->isPointerTy())
      reportFatalUsageError(Twine("-riscv-xreg-gregs: global '") + Names[I] +
                            "' is not an integer or pointer scalar");
    SmallVector<User *, 16> Users(G->users());
    for (User *U : Users) {
      if (auto *L = dyn_cast<LoadInst>(U)) {
        if (L->isSimple() && L->getPointerOperand() == G &&
            L->getType() == VT) {
          IRBuilder<> B(L);
          Value *R = B.CreateCall(ReadReg, {RegMD});
          L->replaceAllUsesWith(fromI64(B, R, VT));
          L->eraseFromParent();
          ++NumLoads;
          continue;
        }
      } else if (auto *S = dyn_cast<StoreInst>(U)) {
        if (S->isSimple() && S->getPointerOperand() == G &&
            S->getValueOperand()->getType() == VT) {
          IRBuilder<> B(S);
          B.CreateCall(WriteReg, {RegMD, toI64(B, S->getValueOperand())});
          S->eraseFromParent();
          ++NumStores;
          continue;
        }
      }
      reportFatalUsageError(Twine("-riscv-xreg-gregs: global '") + Names[I] +
                            "' has a use other than a simple load/store");
    }
    Mapped.push_back({G, RegMD});
    ++NumGlobals;
  }

  // Seed the registers from the globals' memory (initial values) on entry
  // to main.
  if (Function *Main = M.getFunction("main"); Main && !Main->isDeclaration()) {
    IRBuilder<> B(&*Main->getEntryBlock().getFirstInsertionPt());
    for (unsigned I = 0; I < Names.size(); ++I) {
      GlobalVariable *G = M.getGlobalVariable(Names[I], true);
      if (!G) // defined or used only in other modules: use the given width
        G = new GlobalVariable(M, Type::getIntNTy(Ctx, Bits[I]), false,
                               GlobalValue::ExternalLinkage, nullptr, Names[I]);
      std::string RegName = "x" + std::to_string(255 - I);
      auto *RegMD = MetadataAsValue::get(
          Ctx, MDNode::get(Ctx, {MDString::get(Ctx, RegName)}));
      Type *VT = G->getValueType();
      Value *V = B.CreateAlignedLoad(VT, G, G->getAlign().valueOrOne(),
                                     Names[I] + ".xreg.init");
      B.CreateCall(WriteReg, {RegMD, toI64(B, V)});
    }
  }
  return Mapped.empty() && !M.getFunction("main") ? PreservedAnalyses::all()
                                                  : PreservedAnalyses::none();
}
