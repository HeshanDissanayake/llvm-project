//===-- RISCVXReg.h - xreg research extension helpers -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers for the xreg research extension (integer registers x32..x255 reached
// through an xpfx prefix; spec: LLVM-regsw docs/infra/xreg-encoding.md).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_RISCVXREG_H
#define LLVM_LIB_TARGET_RISCV_RISCVXREG_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/PassManager.h"
#include <string>
#include "llvm/MC/MCRegister.h"

namespace llvm {
class MachineInstr;
class MCRegisterInfo;

namespace RISCVXReg {

/// Number of allocatable integer registers x0..x(N-1) (-riscv-num-gprs).
/// 32 means stock behaviour: x32..x255 are reserved.
unsigned getNumGPRs();

/// H2: number K of extra registers x(N-K)..x(N-1) that are callee-saved
/// (-riscv-xreg-callee-saved; default 0 = all extras caller-saved, D7).
unsigned getNumCalleeSavedExtra();

/// H2b: RAGreedy cost of the first use of a callee-saved register, in spill-
/// cost units per function entry (-riscv-xreg-csr-cost). 0 = stock (the
/// RISC-V getCSRCost() of 5/16384 per entry, i.e. practically free).
unsigned getCSRFirstUseCost();

/// H4a: maximum unroll-and-jam count for outer loops (-riscv-xreg-unj;
/// 0 = stock).
unsigned getUnrollAndJamCount();

/// H4b: scalar replacement of loads across innermost-loop iterations
/// (-riscv-xreg-sr; added at the vectorizer-start extension point).
bool scalarRepl();

/// H9: internal clones of external functions for in-module calls, so that
/// IPRA's no-CSR convention applies (-riscv-xreg-nocsr-clone).
bool noCSRClone();

/// H7: full-unroll size threshold for loops that unroll-and-jam won't tile
/// (-riscv-xreg-full-unroll; 0 = stock).
unsigned getFullUnrollThreshold();
/// H14: I-cache footprint budget in bytes for H7a/H7c full unrolling and the
/// H7b inlining bonus (-riscv-xreg-icache-budget, 0 = off), and the estimated
/// code bytes of IRInsts IR instructions (4 bytes each, plus the prefix
/// allowance -riscv-xreg-icache-pfx when N > 32).
unsigned getICacheBudget();
/// H14b: full unrolling only where it is useful (-riscv-xreg-full-unroll-gate).
bool fullUnrollGate();
uint64_t codeBytes(uint64_t IRInsts);
/// H7c: also fully unroll the innermost loop of a tiled nest when its reused
/// values fit the register budget (-riscv-xreg-full-unroll-reuse).
bool fullUnrollReuse();
/// H7b: inlining bonus for calls passing pointers to the caller's local
/// aggregates while they fit the register budget (-riscv-xreg-inline-agg,
/// -riscv-xreg-inline-agg-budget in percent of N).
unsigned getInlineAggBonus();
unsigned getInlineAggBudgetPercent();

/// H6: names of globals kept in x255, x254, ... (-riscv-xreg-gregs).
ArrayRef<std::string> getGlobalRegNames();

/// H4c: register-aware unroll-and-jam (-riscv-xreg-unj-auto), maximum count
/// (-riscv-xreg-unj-max) and register budget in percent of N
/// (-riscv-xreg-unj-budget).
bool unrollAndJamAuto();
/// H4c.1: prefer factors leaving <= 1/8 of a known trip count as remainder
/// (-riscv-xreg-unj-tripaware, default on).
bool unrollAndJamTripAware();
/// H4d: count shifted (stencil-row) reuse as sharing (-riscv-xreg-unj-shifted).
bool unrollAndJamShifted();
/// H4e: diminishing-returns target in percent (-riscv-xreg-unj-dimret, 0 = off).
unsigned getUnrollAndJamDimRet();
unsigned getUnrollAndJamMax();
unsigned getUnrollAndJamBudgetPercent();

/// True if \p Reg is one of x32..x255 (or one of their sub-registers).
bool isExtendedGPR(MCRegister Reg, const MCRegisterInfo &MRI);

/// True if \p MI has any operand that is an extended GPR, i.e. it needs an
/// xpfx prefix when emitted.
bool needsPrefix(const MachineInstr &MI, const MCRegisterInfo &MRI);

/// Emit the .xreg_memtags / .xreg_fnstats sections (-riscv-xreg-tags).
bool emitTags();

/// H3: let pressure-limited heuristics (MachineLICM, MachineSink) see
/// x32..x(N-1) (-riscv-xreg-pressure-hint).
bool pressureHint();

} // namespace RISCVXReg

/// xreg (H6): whole-program register globals (RISCVXRegGlobalRegs.cpp).
class RISCVXRegGlobalRegsPass : public PassInfoMixin<RISCVXRegGlobalRegsPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

/// xreg (H9): internal no-CSR clones (RISCVXRegNoCSRClone.cpp).
class RISCVXRegNoCSRClonePass : public PassInfoMixin<RISCVXRegNoCSRClonePass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

/// xreg (H4b): scalar replacement across iterations (RISCVXRegScalarRepl.cpp).
class RISCVXRegScalarReplPass
    : public PassInfoMixin<RISCVXRegScalarReplPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // namespace llvm

#endif
