//===-- RISCVXRegRegArray.cpp - xreg register-indexed arrays --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xreg research (H13): keep small arrays that are indexed by a variable in a
// block of registers, accessed with the research instructions xrget / xrset
// (register-indexed read/write; LLVM-regsw docs/infra/xreg-encoding.md).
// SROA can only promote arrays whose indices are all constant; this covers
// the rest (table[x], buf[i] in a data-dependent loop, *p++ over a table).
//
// Pointers derived from the array (GEPs with whole-element offsets, phis and
// selects of such pointers) are rewritten as element indices; loads/stores
// become xrget/xrset and comparisons between derived pointers compare
// indices. Any other use (passing the address to a call, storing it,
// converting it to an integer, a volatile access, a different element type)
// keeps the array in memory.
//
// H13a, -riscv-xreg-regarray: local arrays (static allocas) of functions
// that make no calls, from the pool above the global-array blocks; a callee
// can never overwrite them. Whole-array memset and memcpy from a constant
// global are expanded.
//
// H13b, program-wide global arrays (like register globals):
//  -riscv-xreg-garray-report prints, per global array, whether every use in
//  this module allows it and its profile-weighted access count;
//  -riscv-xreg-garrays=name:base:elems:bits,... (chosen by the driver from
//  all modules' reports) rewrites the accesses; main copies the initial
//  values into the blocks on entry.
//
// The pool is x(N)..x(255 - #register globals), above the allocatable
// registers (-riscv-num-gprs). Out-of-bounds indices are undefined behaviour
// in C; in this hardware they wrap modulo 256.
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-xreg-regarray"

STATISTIC(NumPromoted, "Number of local arrays kept in a register block");
STATISTIC(NumAccesses, "Number of loads/stores turned into xrget/xrset");
STATISTIC(NumGlobalAccesses, "Number of global-array accesses turned into xrget/xrset");

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
// The pointers derived from one array and the instructions that use them.
class Derived {
public:
  Derived(Value *Base, Type *T, uint64_t Elems, const DataLayout &DL)
      : Base(Base), T(T), Elems(Elems), Sz(DL.getTypeStoreSize(T)), DL(DL) {}

  // Collect all uses; false if any use keeps the array in memory.
  // AllowInit: whole-array memset / memcpy from a constant global (locals).
  bool analyze(bool AllowInit);
  // Rewrite every access to register block x(BaseReg).. (Inits are handled
  // by the caller before this).
  void rewrite(uint64_t BaseReg, Function *Get, Function *Set);

  Value *Base;
  Type *T;
  uint64_t Elems, Sz;
  const DataLayout &DL;
  SetVector<Value *> Ptrs;                 // Base and derived pointers
  SmallVector<Instruction *, 16> Accesses; // loads and stores
  SmallVector<ICmpInst *, 4> Cmps;         // derived pointer vs derived pointer
  SmallVector<Instruction *, 4> Lifetimes;
  SmallVector<MemIntrinsic *, 2> Inits;

private:
  bool wholeElements(GEPOperator *G) const;
  Value *index(Value *P);
  DenseMap<Value *, Value *> Idx;
};
} // namespace

bool Derived::wholeElements(GEPOperator *G) const {
  SmallMapVector<Value *, APInt, 4> Var;
  APInt Const(64, 0);
  if (!G->collectOffset(DL, 64, Var, Const) || Const.getSExtValue() % (int64_t)Sz)
    return false;
  for (auto &[V, Scale] : Var)
    if (Scale.getSExtValue() % (int64_t)Sz)
      return false;
  return true;
}

bool Derived::analyze(bool AllowInit) {
  uint64_t Bytes = Elems * Sz;
  SmallVector<Value *, 16> Work{Base};
  Ptrs.insert(Base);
  SmallVector<Instruction *, 8> Merges; // phis and selects to verify
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    for (User *U : V->users()) {
      auto Add = [&](Value *P) {
        if (Ptrs.insert(P))
          Work.push_back(P);
      };
      if (auto *G = dyn_cast<GEPOperator>(U)) {
        if (G->getPointerOperand() != V || !wholeElements(G))
          return false;
        Add(G);
      } else if (auto *P = dyn_cast<PHINode>(U)) {
        Merges.push_back(P);
        Add(P);
      } else if (auto *S = dyn_cast<SelectInst>(U)) {
        if (S->getCondition() == V)
          return false;
        Merges.push_back(S);
        Add(S);
      } else if (auto *L = dyn_cast<LoadInst>(U)) {
        if (L->isVolatile() || L->getType() != T)
          return false;
        Accesses.push_back(L);
      } else if (auto *St = dyn_cast<StoreInst>(U)) {
        if (St->isVolatile() || St->getPointerOperand() != V ||
            St->getValueOperand()->getType() != T)
          return false;
        Accesses.push_back(St);
      } else if (auto *C = dyn_cast<ICmpInst>(U)) {
        Cmps.push_back(C);
      } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
        if (II->isLifetimeStartOrEnd()) {
          Lifetimes.push_back(II);
          continue;
        }
        auto *M = dyn_cast<MemIntrinsic>(II);
        auto *Len = M ? dyn_cast<ConstantInt>(M->getLength()) : nullptr;
        if (!AllowInit || !M || M->isVolatile() || V != Base ||
            M->getDest() != Base || !Len || Len->getZExtValue() != Bytes)
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
        Inits.push_back(M);
      } else {
        return false; // escapes: call argument, stored, ptrtoint, ...
      }
    }
  }
  // Every merged pointer and every compared pointer must be derived too.
  for (Instruction *I : Merges) {
    if (auto *P = dyn_cast<PHINode>(I)) {
      for (Value *In : P->incoming_values())
        if (!Ptrs.count(In))
          return false;
    } else {
      auto *S = cast<SelectInst>(I);
      if (!Ptrs.count(S->getTrueValue()) || !Ptrs.count(S->getFalseValue()))
        return false;
    }
  }
  for (ICmpInst *C : Cmps)
    if (!Ptrs.count(C->getOperand(0)) || !Ptrs.count(C->getOperand(1)))
      return false;
  return true;
}

// Element index of derived pointer P (memoized; phis may be cyclic).
Value *Derived::index(Value *P) {
  if (Value *V = Idx.lookup(P))
    return V;
  Type *I64 = Type::getInt64Ty(P->getContext());
  if (P == Base)
    return Idx[P] = ConstantInt::get(I64, 0);
  if (auto *Phi = dyn_cast<PHINode>(P)) {
    IRBuilder<> B(Phi);
    PHINode *N = B.CreatePHI(I64, Phi->getNumIncomingValues(), Phi->getName() + ".xidx");
    Idx[P] = N;
    for (unsigned K = 0; K < Phi->getNumIncomingValues(); ++K)
      N->addIncoming(index(Phi->getIncomingValue(K)), Phi->getIncomingBlock(K));
    return N;
  }
  if (auto *S = dyn_cast<SelectInst>(P)) {
    Value *TI = index(S->getTrueValue()), *FI = index(S->getFalseValue());
    IRBuilder<> B(S);
    return Idx[P] = B.CreateSelect(S->getCondition(), TI, FI, S->getName() + ".xidx");
  }
  auto *G = cast<GEPOperator>(P);
  Value *BaseIdx = index(G->getPointerOperand());
  SmallMapVector<Value *, APInt, 4> Var;
  APInt Const(64, 0);
  G->collectOffset(DL, 64, Var, Const);
  int64_t C = Const.getSExtValue() / (int64_t)Sz;
  if (Var.empty() && isa<ConstantInt>(BaseIdx))
    return Idx[P] = ConstantInt::get(I64, cast<ConstantInt>(BaseIdx)->getSExtValue() + C);
  // A GEP with variable offsets (or a non-constant base) is an instruction.
  IRBuilder<> B(cast<Instruction>(G));
  Value *Sum = B.CreateAdd(BaseIdx, ConstantInt::get(I64, C));
  for (auto &[V, Scale] : Var) {
    Value *X = B.CreateSExtOrTrunc(V, I64);
    int64_t K = Scale.getSExtValue() / (int64_t)Sz;
    Sum = B.CreateAdd(Sum, K == 1 ? X : B.CreateMul(X, ConstantInt::get(I64, K)));
  }
  return Idx[P] = Sum;
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

void Derived::rewrite(uint64_t BaseReg, Function *Get, Function *Set) {
  IRBuilder<> B(Base->getContext());
  for (Instruction *A : Accesses) {
    Value *I = index(getLoadStorePointerOperand(A));
    B.SetInsertPoint(A);
    uint64_t Imm = BaseReg;
    if (auto *CI = dyn_cast<ConstantInt>(I)) {
      Imm += CI->getSExtValue();
      I = B.getInt64(0);
    }
    if (auto *L = dyn_cast<LoadInst>(A)) {
      Value *R = B.CreateCall(Get, {I, B.getInt64(Imm & 0xff)});
      L->replaceAllUsesWith(fromI64(R, T, B));
    } else {
      B.CreateCall(Set, {toI64(cast<StoreInst>(A)->getValueOperand(), B), I,
                         B.getInt64(Imm & 0xff)});
    }
    A->eraseFromParent();
  }
  for (ICmpInst *C : Cmps) {
    Value *L = index(C->getOperand(0)), *R = index(C->getOperand(1));
    B.SetInsertPoint(C);
    C->replaceAllUsesWith(B.CreateICmp(C->getPredicate(), L, R));
    C->eraseFromParent();
  }
  for (Instruction *L : Lifetimes)
    L->eraseFromParent();
  // The derived pointer instructions are now dead (phis may use each other).
  for (Value *P : llvm::reverse(Ptrs))
    if (auto *I = dyn_cast<Instruction>(P); I && I != Base) {
      I->replaceAllUsesWith(PoisonValue::get(I->getType()));
      I->eraseFromParent();
    }
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

// H13a: local arrays of a function that makes no calls.
static bool runOnFunction(Function &F, unsigned PoolBegin, unsigned PoolEnd,
                          Function *Get, Function *Set) {
  if (F.isDeclaration() || PoolBegin >= PoolEnd)
    return false;
  const DataLayout &DL = F.getDataLayout();
  SmallVector<std::unique_ptr<Derived>, 4> Cands;
  for (Instruction &I : F.getEntryBlock()) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    uint64_t N = 0;
    Type *T = AI && AI->isStaticAlloca() ? arrayElem(AI->getAllocatedType(), N) : nullptr;
    if (!T || N < 2)
      continue;
    auto D = std::make_unique<Derived>(AI, T, N, DL);
    if (D->analyze(/*AllowInit=*/true))
      Cands.push_back(std::move(D));
  }
  if (Cands.empty())
    return false;
  // Leaf check: no calls except intrinsics; memset/memcpy only if they are
  // whole-array initializations that will be expanded.
  SmallPtrSet<Instruction *, 8> Own;
  for (auto &C : Cands)
    for (auto *M : C->Inits)
      Own.insert(M);
  for (Instruction &I : instructions(F))
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      auto *II = dyn_cast<IntrinsicInst>(CB);
      if (!II || (isa<MemIntrinsic>(II) && !Own.count(II)))
        return false;
    }
  unsigned Next = PoolBegin;
  bool Changed = false;
  IRBuilder<> B(F.getContext());
  for (auto &C : Cands) {
    if (Next + C->Elems > PoolEnd)
      continue;
    uint64_t BaseReg = Next;
    Next += C->Elems;
    NumAccesses += C->Accesses.size();
    for (MemIntrinsic *MI : C->Inits) {
      B.SetInsertPoint(MI);
      for (uint64_t K = 0; K < C->Elems; ++K) {
        Value *V;
        if (auto *MS = dyn_cast<MemSetInst>(MI)) {
          uint64_t Byte = cast<ConstantInt>(MS->getValue())->getZExtValue() & 0xff, W = 0;
          for (uint64_t J = 0; J < C->Sz; ++J)
            W = (W << 8) | Byte;
          V = B.getInt64(W);
        } else {
          auto *G = cast<GlobalVariable>(getUnderlyingObject(cast<MemCpyInst>(MI)->getSource()));
          Constant *E = ConstantFoldLoadFromConst(G->getInitializer(), C->T,
                                                  APInt(64, K * C->Sz), DL);
          V = toI64(E ? E : Constant::getNullValue(C->T), B);
        }
        B.CreateCall(Set, {V, B.getInt64(0), B.getInt64((BaseReg + K) & 0xff)});
      }
      MI->eraseFromParent();
    }
    C->rewrite(BaseReg, Get, Set);
    auto *AI = cast<AllocaInst>(C->Base);
    if (AI->use_empty())
      AI->eraseFromParent();
    ++NumPromoted;
    Changed = true;
  }
  return Changed;
}

static void reportGlobalArrays(Module &M, ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  const DataLayout &DL = M.getDataLayout();
  bool HasMain = M.getFunction("main") && !M.getFunction("main")->isDeclaration();
  for (GlobalVariable &G : M.globals()) {
    uint64_t N = 0;
    Type *T = arrayElem(G.getValueType(), N);
    if (!T || N < 2 || N > 224 || G.isThreadLocal())
      continue;
    Derived D(&G, T, N, DL);
    bool Ok = D.analyze(/*AllowInit=*/false);
    uint64_t W = 0;
    for (Instruction *I : D.Accesses) {
      auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(*I->getFunction());
      W += std::max<uint64_t>(1, BFI.getBlockProfileCount(I->getParent()).value_or(1));
    }
    errs() << "xreg-garray: " << G.getName() << " " << N << " "
           << (T->isPointerTy() ? 64 : T->getIntegerBitWidth()) << " "
           << (Ok ? 1 : 0) << " " << W << " " << (G.hasLocalLinkage() ? "I" : "E")
           << " " << HasMain << "\n";
  }
}

// H13b: rewrite the driver-chosen global arrays; returns the first register
// after their blocks (the start of the local-array pool).
static unsigned rewriteGlobalArrays(Module &M, Function *Get, Function *Set,
                                    bool &Changed) {
  unsigned End = RISCVXReg::getNumGPRs();
  if (GArrays.empty())
    return End;
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  struct E { std::string Name; uint64_t Base, Elems, Bits; };
  SmallVector<E, 8> List;
  for (StringRef S : GArrays) {
    SmallVector<StringRef, 4> F;
    S.split(F, ':');
    uint64_t Base, Elems, Bits;
    if (F.size() != 4 || F[1].getAsInteger(10, Base) || F[2].getAsInteger(10, Elems) ||
        F[3].getAsInteger(10, Bits))
      report_fatal_error(Twine("xreg: bad -riscv-xreg-garrays entry ") + S);
    List.push_back({F[0].str(), Base, Elems, Bits});
    End = std::max<unsigned>(End, Base + Elems);
  }
  for (E &En : List) {
    GlobalVariable *G = M.getGlobalVariable(En.Name, true);
    if (!G)
      continue;
    uint64_t N = 0;
    Type *T = arrayElem(G->getValueType(), N);
    if (!T || N != En.Elems)
      report_fatal_error(Twine("xreg: global array ") + En.Name + " has a different shape");
    Derived D(G, T, N, DL);
    if (!D.analyze(/*AllowInit=*/false))
      report_fatal_error(Twine("xreg: global array ") + En.Name + " can't be kept in registers");
    NumGlobalAccesses += D.Accesses.size();
    D.rewrite(En.Base, Get, Set);
    Changed = true;
  }
  // main copies each array's initial value from memory into its block.
  if (Function *Main = M.getFunction("main"); Main && !Main->isDeclaration()) {
    IRBuilder<> B(&*Main->getEntryBlock().getFirstInsertionPt());
    for (E &En : List) {
      Type *T = Type::getIntNTy(Ctx, En.Bits);
      GlobalVariable *G = M.getGlobalVariable(En.Name, true);
      if (!G)
        G = new GlobalVariable(M, ArrayType::get(T, En.Elems), false,
                               GlobalValue::ExternalLinkage, nullptr, En.Name);
      for (uint64_t K = 0; K < En.Elems; ++K) {
        Value *V = B.CreateLoad(T, B.CreateConstInBoundsGEP1_64(T, G, K),
                                En.Name + ".xreg.init");
        B.CreateCall(Set, {B.CreateZExt(V, B.getInt64Ty()), B.getInt64(0),
                           B.getInt64((En.Base + K) & 0xff)});
      }
    }
    Changed = true;
  }
  return End;
}

PreservedAnalyses RISCVXRegRegArrayPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (GArrayReport)
    reportGlobalArrays(M, AM);
  if (GArrays.empty() && !RISCVXReg::regArray())
    return PreservedAnalyses::all();
  Function *Get = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::riscv_xreg_rget);
  Function *Set = Intrinsic::getOrInsertDeclaration(&M, Intrinsic::riscv_xreg_rset);
  bool Changed = false;
  unsigned Begin = rewriteGlobalArrays(M, Get, Set, Changed);
  if (RISCVXReg::regArray()) {
    unsigned End = 256 - RISCVXReg::getGlobalRegNames().size();
    for (Function &F : M)
      Changed |= runOnFunction(F, Begin, End, Get, Set);
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
