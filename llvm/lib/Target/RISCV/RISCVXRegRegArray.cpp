//===-- RISCVXRegRegArray.cpp - xreg register-indexed local arrays --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xreg research (H13): keep small local arrays that are indexed by a variable
// in a block of registers, accessed with the research instructions xrget /
// xrset (register-indexed read/write; LLVM-regsw docs/infra/xreg-encoding.md).
// SROA can only promote arrays whose indices are all constant; this covers
// the rest (table[x], buf[i] in a data-dependent loop).
//
// -riscv-xreg-regarray: the block pool is x(N)..x(255 - #register globals),
// above the allocatable registers (-riscv-num-gprs), so no other code uses
// it. First version (conservative): only functions that make no calls (after
// all IR optimization), so a callee can never overwrite the caller's block;
// each such function uses the pool from its start.
//
// An alloca is promoted if it is a static array of one scalar element type
// T (integer <= 64 bits or pointer), fits the remaining pool, and every use
// is (through GEPs whose offsets are whole elements) a non-volatile load or
// store of T, a lifetime marker, a memset of the whole array, or a memcpy of
// the whole array from a constant global. Out-of-bounds indices are undefined
// behaviour in C; in hardware they wrap modulo 256.
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsRISCV.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-xreg-regarray"

STATISTIC(NumPromoted, "Number of local arrays kept in a register block");
STATISTIC(NumAccesses, "Number of loads/stores turned into xrget/xrset");
STATISTIC(NumGlobalAccesses, "Number of global-array accesses turned into xrget/xrset");

// H13b: global arrays in registers, program-wide (like register globals).
static cl::opt<bool> GArrayReport(
    "riscv-xreg-garray-report", cl::Hidden, cl::init(false),
    cl::desc("xreg (H13b): print, for every global array, whether all its "
             "uses in this module allow keeping it in registers, and its "
             "profile-weighted access count"));
static cl::list<std::string> GArrays(
    "riscv-xreg-garrays", cl::Hidden, cl::CommaSeparated,
    cl::desc("xreg (H13b): name:base:elems:bits,... global arrays kept in "
             "registers x(base).. (chosen by the driver from the reports)"));

namespace {
struct Candidate {
  AllocaInst *AI;
  Type *ElTy;
  uint64_t Elems;
  SmallVector<Instruction *, 16> Accesses; // loads and stores
  SmallVector<Instruction *, 4> Erase;     // lifetime markers
  SmallVector<MemIntrinsic *, 2> Inits;    // whole-array memset / memcpy
  SmallVector<Instruction *, 8> GEPs;
};
} // namespace

// Element index of Ptr inside the array at Base (element size Sz): walk the
// GEP chain and require every offset to be a whole number of elements.
static Value *elementIndex(Value *Ptr, Value *Base, uint64_t Sz,
                           const DataLayout &DL, IRBuilder<> &B) {
  Type *I64 = B.getInt64Ty();
  Value *Idx = ConstantInt::get(I64, 0);
  while (Ptr != Base) {
    auto *GEP = dyn_cast<GEPOperator>(Ptr);
    if (!GEP)
      return nullptr;
    SmallMapVector<Value *, APInt, 4> Var;
    APInt Const(64, 0);
    if (!GEP->collectOffset(DL, 64, Var, Const))
      return nullptr;
    if (Const.getSExtValue() % (int64_t)Sz)
      return nullptr;
    Value *Sum = ConstantInt::get(I64, Const.getSExtValue() / (int64_t)Sz);
    for (auto &[V, Scale] : Var) {
      if (Scale.getSExtValue() % (int64_t)Sz)
        return nullptr;
      Value *X = B.CreateSExtOrTrunc(V, I64);
      int64_t K = Scale.getSExtValue() / (int64_t)Sz;
      if (K != 1)
        X = B.CreateMul(X, ConstantInt::get(I64, K));
      Sum = B.CreateAdd(Sum, X);
    }
    Idx = B.CreateAdd(Idx, Sum);
    Ptr = GEP->getPointerOperand();
  }
  return Idx;
}

// Collect the uses of AI; false if any use disqualifies it.
static bool collect(Candidate &C, const DataLayout &DL) {
  uint64_t Bytes = C.Elems * DL.getTypeStoreSize(C.ElTy);
  SmallVector<Value *, 8> Work{C.AI};
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    for (User *U : V->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        return false;
      if (auto *G = dyn_cast<GetElementPtrInst>(I)) {
        C.GEPs.push_back(G);
        Work.push_back(G);
      } else if (auto *L = dyn_cast<LoadInst>(I)) {
        if (L->isVolatile() || L->getType() != C.ElTy)
          return false;
        C.Accesses.push_back(L);
      } else if (auto *S = dyn_cast<StoreInst>(I)) {
        if (S->isVolatile() || S->getPointerOperand() != V ||
            S->getValueOperand()->getType() != C.ElTy)
          return false;
        C.Accesses.push_back(S);
      } else if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (II->isLifetimeStartOrEnd()) {
          C.Erase.push_back(II);
          continue;
        }
        auto *M = dyn_cast<MemIntrinsic>(II);
        auto *Len = M ? dyn_cast<ConstantInt>(M->getLength()) : nullptr;
        if (!M || M->isVolatile() || V != C.AI || M->getDest() != C.AI ||
            !Len || Len->getZExtValue() != Bytes)
          return false;
        if (auto *MS = dyn_cast<MemSetInst>(M)) {
          if (!isa<ConstantInt>(MS->getValue()))
            return false;
        } else if (auto *MC = dyn_cast<MemCpyInst>(M)) {
          auto *G = dyn_cast<GlobalVariable>(getUnderlyingObject(MC->getSource()));
          if (!G || !G->isConstant() || !G->hasDefinitiveInitializer())
            return false;
        } else {
          return false;
        }
        C.Inits.push_back(M);
      } else {
        return false; // escapes (call argument, stored, compared, ...)
      }
    }
  }
  return true;
}

static Value *toI64(Value *V, IRBuilder<> &B) {
  if (V->getType()->isPointerTy())
    return B.CreatePtrToInt(V, B.getInt64Ty());
  return B.CreateZExtOrTrunc(V, B.getInt64Ty());
}

static Value *fromI64(Value *V, Type *Ty, IRBuilder<> &B) {
  if (Ty->isPointerTy())
    return B.CreateIntToPtr(V, Ty);
  return B.CreateTrunc(V, Ty);
}

static bool runOnFunction(Function &F, unsigned PoolBegin, unsigned PoolEnd) {
  if (F.isDeclaration() || PoolBegin >= PoolEnd)
    return false;
  const DataLayout &DL = F.getDataLayout();
  SmallVector<Candidate, 4> Cands;
  for (Instruction &I : F.getEntryBlock()) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI || !AI->isStaticAlloca())
      continue;
    Type *T = AI->getAllocatedType();
    uint64_t N = 1;
    while (auto *AT = dyn_cast<ArrayType>(T)) {
      N *= AT->getNumElements();
      T = AT->getElementType();
    }
    if (N < 2 || !(T->isPointerTy() || (T->isIntegerTy() && T->getIntegerBitWidth() <= 64)))
      continue;
    Candidate C{AI, T, N, {}, {}, {}, {}};
    if (collect(C, DL))
      Cands.push_back(std::move(C));
  }
  if (Cands.empty())
    return false;
  // Leaf check, ignoring the memory intrinsics that belong to candidates.
  SmallPtrSet<Instruction *, 8> Own;
  for (auto &C : Cands)
    for (auto *M : C.Inits)
      Own.insert(M);
  for (Instruction &I : instructions(F))
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      auto *II = dyn_cast<IntrinsicInst>(CB);
      if (!II || (isa<MemIntrinsic>(II) && !Own.count(II)))
        return false;
    }

  Module *M = F.getParent();
  Function *Get = Intrinsic::getOrInsertDeclaration(M, Intrinsic::riscv_xreg_rget);
  Function *Set = Intrinsic::getOrInsertDeclaration(M, Intrinsic::riscv_xreg_rset);
  unsigned Next = PoolBegin;
  bool Changed = false;
  for (auto &C : Cands) {
    if (Next + C.Elems > PoolEnd)
      continue;
    uint64_t Base = Next;
    Next += C.Elems;
    uint64_t Sz = DL.getTypeStoreSize(C.ElTy);
    IRBuilder<> B(F.getContext());
    // An access: Base + Idx. A constant index folds into the immediate.
    auto Emit = [&](Instruction *At, Value *Idx, Value *Val) -> Value * {
      B.SetInsertPoint(At);
      uint64_t Imm = Base;
      if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
        Imm += CI->getSExtValue();
        Idx = B.getInt64(0);
      }
      if (Val)
        return B.CreateCall(Set, {toI64(Val, B), Idx, B.getInt64(Imm & 0xff)});
      return B.CreateCall(Get, {Idx, B.getInt64(Imm & 0xff)});
    };
    bool Ok = true;
    SmallVector<std::pair<Instruction *, Value *>, 16> Idxs;
    for (Instruction *A : C.Accesses) {
      B.SetInsertPoint(A);
      Value *Idx = elementIndex(getLoadStorePointerOperand(A), C.AI, Sz, DL, B);
      if (!Idx) {
        Ok = false;
        break;
      }
      Idxs.push_back({A, Idx});
    }
    if (!Ok)
      continue; // (dead index arithmetic is cleaned up later)
    for (auto &[A, Idx] : Idxs) {
      if (auto *L = dyn_cast<LoadInst>(A)) {
        Value *R = Emit(L, Idx, nullptr);
        L->replaceAllUsesWith(fromI64(R, C.ElTy, B));
      } else {
        Emit(A, Idx, cast<StoreInst>(A)->getValueOperand());
      }
      A->eraseFromParent();
      ++NumAccesses;
    }
    for (MemIntrinsic *MI : C.Inits) {
      for (uint64_t K = 0; K < C.Elems; ++K) {
        Value *V;
        if (auto *MS = dyn_cast<MemSetInst>(MI)) {
          uint64_t Byte = cast<ConstantInt>(MS->getValue())->getZExtValue() & 0xff, W = 0;
          for (uint64_t J = 0; J < Sz; ++J)
            W = (W << 8) | Byte;
          V = B.getInt64(W);
        } else {
          auto *G = cast<GlobalVariable>(getUnderlyingObject(cast<MemCpyInst>(MI)->getSource()));
          Constant *E = ConstantFoldLoadFromConst(
              G->getInitializer(), C.ElTy, APInt(64, K * Sz), DL);
          if (!E) {
            E = Constant::getNullValue(C.ElTy);
          }
          B.SetInsertPoint(MI);
          V = toI64(E, B);
        }
        Emit(MI, B.getInt64(K), V);
      }
      MI->eraseFromParent();
    }
    for (Instruction *E : C.Erase)
      E->eraseFromParent();
    for (Instruction *G : llvm::reverse(C.GEPs))
      if (G->use_empty())
        G->eraseFromParent();
    if (C.AI->use_empty())
      C.AI->eraseFromParent();
    ++NumPromoted;
    Changed = true;
  }
  return Changed;
}

// Scalar element type and count of a (nested) array type, or null.
static Type *arrayElem(Type *T, uint64_t &N) {
  N = 1;
  if (!T->isArrayTy())
    return nullptr;
  while (auto *AT = dyn_cast<ArrayType>(T)) {
    N *= AT->getNumElements();
    T = AT->getElementType();
  }
  if (T->isPointerTy() || (T->isIntegerTy() && T->getIntegerBitWidth() <= 64))
    return T;
  return nullptr;
}

// Loads/stores of G (through GEP instructions or constant GEPs); false if any
// other use exists or an access has the wrong type.
static bool globalAccesses(GlobalVariable &G, Type *T,
                           SmallVectorImpl<Instruction *> &Out) {
  SmallVector<Value *, 8> Work{&G};
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    for (User *U : V->users()) {
      if (isa<GEPOperator>(U)) {
        Work.push_back(U);
      } else if (auto *L = dyn_cast<LoadInst>(U)) {
        if (L->isVolatile() || L->getType() != T)
          return false;
        Out.push_back(L);
      } else if (auto *S = dyn_cast<StoreInst>(U)) {
        if (S->isVolatile() || S->getPointerOperand() != V ||
            S->getValueOperand()->getType() != T)
          return false;
        Out.push_back(S);
      } else {
        return false;
      }
    }
  }
  return true;
}

static void reportGlobalArrays(Module &M, ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  const DataLayout &DL = M.getDataLayout();
  for (GlobalVariable &G : M.globals()) {
    uint64_t N;
    Type *T = arrayElem(G.getValueType(), N);
    if (!T || N < 2 || N > 224 || G.isThreadLocal())
      continue;
    SmallVector<Instruction *, 16> Acc;
    bool Ok = globalAccesses(G, T, Acc);
    uint64_t Sz = DL.getTypeStoreSize(T), W = 0;
    for (Instruction *I : Acc) {
      IRBuilder<> B(I);
      if (!elementIndex(getLoadStorePointerOperand(I), &G, Sz, DL, B))
        Ok = false;
      auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(*I->getFunction());
      W += BFI.getBlockProfileCount(I->getParent()).value_or(1);
    }
    errs() << "xreg-garray: " << G.getName() << " " << N << " "
           << (T->isPointerTy() ? 64 : T->getIntegerBitWidth()) << " "
           << (Ok ? 1 : 0) << " " << W << "\n";
  }
}

static bool rewriteGlobalArrays(Module &M) {
  if (GArrays.empty())
    return false;
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  Function *Get = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::riscv_xreg_rget);
  Function *Set = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::riscv_xreg_rset);
  struct E { std::string Name; uint64_t Base, Elems, Bits; };
  SmallVector<E, 8> List;
  for (StringRef S : GArrays) {
    SmallVector<StringRef, 4> F;
    S.split(F, ':');
    uint64_t Base, Elems, Bits;
    if (F.size() != 4 || F[1].getAsInteger(10, Base) || F[2].getAsInteger(10, Elems) ||
        F[3].getAsInteger(10, Bits))
      report_fatal_error("xreg: bad -riscv-xreg-garrays entry " + S);
    List.push_back({F[0].str(), Base, Elems, Bits});
  }
  IRBuilder<> B(Ctx);
  for (E &En : List) {
    GlobalVariable *G = M.getGlobalVariable(En.Name, true);
    if (!G)
      continue;
    uint64_t N;
    Type *T = arrayElem(G->getValueType(), N);
    SmallVector<Instruction *, 16> Acc;
    if (!T || N != En.Elems || !globalAccesses(*G, T, Acc))
      report_fatal_error("xreg: global array " + En.Name + " can't be kept in registers");
    uint64_t Sz = DL.getTypeStoreSize(T);
    for (Instruction *I : Acc) {
      B.SetInsertPoint(I);
      Value *Idx = elementIndex(getLoadStorePointerOperand(I), G, Sz, DL, B);
      if (!Idx)
        report_fatal_error("xreg: global array " + En.Name + ": bad index");
      uint64_t Imm = En.Base;
      if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
        Imm += CI->getSExtValue();
        Idx = B.getInt64(0);
      }
      if (auto *L = dyn_cast<LoadInst>(I)) {
        Value *R = B.CreateCall(Get, {Idx, B.getInt64(Imm & 0xff)});
        L->replaceAllUsesWith(fromI64(R, T, B));
      } else {
        B.CreateCall(Set, {toI64(cast<StoreInst>(I)->getValueOperand(), B), Idx,
                           B.getInt64(Imm & 0xff)});
      }
      I->eraseFromParent();
      ++NumGlobalAccesses;
    }
  }
  // main copies each array's initial value from memory into its block.
  if (Function *Main = M.getFunction("main"); Main && !Main->isDeclaration()) {
    B.SetInsertPoint(&*Main->getEntryBlock().getFirstInsertionPt());
    for (E &En : List) {
      Type *T = Type::getIntNTy(Ctx, En.Bits);
      GlobalVariable *G = M.getGlobalVariable(En.Name, true);
      if (!G)
        G = new GlobalVariable(M, ArrayType::get(T, En.Elems), false,
                               GlobalValue::ExternalLinkage, nullptr, En.Name);
      for (uint64_t K = 0; K < En.Elems; ++K) {
        Value *P = B.CreateConstInBoundsGEP1_64(T, G, K);
        Value *V = B.CreateLoad(T, P, En.Name + ".xreg.init");
        B.CreateCall(Set, {B.CreateZExt(V, B.getInt64Ty()), B.getInt64(0),
                           B.getInt64((En.Base + K) & 0xff)});
      }
    }
  }
  return true;
}

PreservedAnalyses RISCVXRegRegArrayPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (GArrayReport)
    reportGlobalArrays(M, AM);
  bool Changed = rewriteGlobalArrays(M);
  if (!RISCVXReg::regArray())
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  // Local arrays use the registers after the global arrays' blocks.
  unsigned Begin = RISCVXReg::getNumGPRs();
  for (StringRef S : GArrays) {
    SmallVector<StringRef, 4> F;
    S.split(F, ':');
    uint64_t Base = 0, Elems = 0;
    if (F.size() == 4 && !F[1].getAsInteger(10, Base) && !F[2].getAsInteger(10, Elems))
      Begin = std::max<unsigned>(Begin, Base + Elems);
  }
  unsigned End = 256 - RISCVXReg::getGlobalRegNames().size();
  for (Function &F : M)
    Changed |= runOnFunction(F, Begin, End);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
