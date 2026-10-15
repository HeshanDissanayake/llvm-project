//===-- RISCVXReg.cpp - xreg research extension helpers -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVXReg.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

static cl::opt<unsigned> NumGPRs(
    "riscv-num-gprs", cl::Hidden, cl::init(32),
    cl::desc("xreg: number of allocatable integer registers x0..x(N-1), "
             "32..256 (default 32 = stock)"));

static cl::opt<bool> EmitXRegTags(
    "riscv-xreg-tags", cl::Hidden, cl::init(false),
    cl::desc("xreg: emit .xreg_memtags/.xreg_fnstats measurement sections"));

unsigned RISCVXReg::getNumGPRs() {
  if (NumGPRs < 32 || NumGPRs > 256)
    report_fatal_error("-riscv-num-gprs must be in [32, 256]");
  return NumGPRs;
}

static cl::opt<bool> XRegPressureHint(
    "riscv-xreg-pressure-hint", cl::Hidden, cl::init(false),
    cl::desc("xreg (H3): let MachineLICM/MachineSink pressure limits include "
             "x32..x(N-1)"));

static cl::opt<unsigned> XRegCalleeSaved(
    "riscv-xreg-callee-saved", cl::Hidden, cl::init(0),
    cl::desc("xreg (H2): make the top K extra registers x(N-K)..x(N-1) "
             "callee-saved (default 0)"));

unsigned RISCVXReg::getNumCalleeSavedExtra() {
  if (XRegCalleeSaved > getNumGPRs() - 32)
    report_fatal_error("-riscv-xreg-callee-saved must be <= riscv-num-gprs - 32");
  return XRegCalleeSaved;
}

static cl::opt<unsigned> XRegCSRCost(
    "riscv-xreg-csr-cost", cl::Hidden, cl::init(0),
    cl::desc("xreg (H2b): register-allocator cost of the first use of a "
             "callee-saved register, in spill-cost units per function entry "
             "(default 0 = stock)"));

unsigned RISCVXReg::getCSRFirstUseCost() { return XRegCSRCost; }

static cl::opt<unsigned> XRegUnJ(
    "riscv-xreg-unj", cl::Hidden, cl::init(0),
    cl::desc("xreg (H4a): allow unroll-and-jam of outer loops with a count "
             "of up to U (default 0 = stock); needs -enable-unroll-and-jam"));

unsigned RISCVXReg::getUnrollAndJamCount() { return XRegUnJ; }

static cl::opt<bool> XRegScalarRepl(
    "riscv-xreg-sr", cl::Hidden, cl::init(false),
    cl::desc("xreg (H4b): scalar replacement of loads across innermost-loop "
             "iterations"));

bool RISCVXReg::scalarRepl() { return XRegScalarRepl; }

static cl::opt<unsigned> XRegFullUnroll(
    "riscv-xreg-full-unroll", cl::Hidden, cl::init(0),
    cl::desc("xreg (H7): full-unroll size threshold for loops that unroll-"
             "and-jam won't tile (0 = stock)"));

unsigned RISCVXReg::getFullUnrollThreshold() { return XRegFullUnroll; }

static cl::opt<bool> XRegUnJAuto(
    "riscv-xreg-unj-auto", cl::Hidden, cl::init(false),
    cl::desc("xreg (H4c): unroll-and-jam outer loops with a count chosen from "
             "a register budget; needs -enable-unroll-and-jam"));
static cl::opt<unsigned> XRegUnJMax(
    "riscv-xreg-unj-max", cl::Hidden, cl::init(16),
    cl::desc("xreg (H4c): maximum unroll-and-jam count"));
static cl::opt<unsigned> XRegUnJBudget(
    "riscv-xreg-unj-budget", cl::Hidden, cl::init(75),
    cl::desc("xreg (H4c): register budget for unroll-and-jam, percent of N"));

static cl::opt<bool> XRegUnJTripAware(
    "riscv-xreg-unj-tripaware", cl::Hidden, cl::init(true),
    cl::desc("xreg (H4c.1): with -riscv-xreg-unj-auto, prefer factors that "
             "leave <= 1/8 of a known outer trip count to the remainder"));

bool RISCVXReg::unrollAndJamAuto() { return XRegUnJAuto; }
bool RISCVXReg::unrollAndJamTripAware() { return XRegUnJTripAware; }

static cl::opt<bool> XRegUnJShifted(
    "riscv-xreg-unj-shifted", cl::Hidden, cl::init(false),
    cl::desc("xreg (H4d): with -riscv-xreg-unj-auto, also jam nests whose "
             "inner loads differ by multiples of the outer stride (stencils)"));

bool RISCVXReg::unrollAndJamShifted() { return XRegUnJShifted; }

static cl::opt<unsigned> XRegUnJDimRet(
    "riscv-xreg-unj-dimret", cl::Hidden, cl::init(0),
    cl::desc("xreg (H4e): smallest unroll-and-jam factor reaching this percent "
             "of the shared-load savings of the budget factor (0 = off)"));

unsigned RISCVXReg::getUnrollAndJamDimRet() { return XRegUnJDimRet; }
unsigned RISCVXReg::getUnrollAndJamMax() { return XRegUnJMax; }
unsigned RISCVXReg::getUnrollAndJamBudgetPercent() { return XRegUnJBudget; }

bool RISCVXReg::emitTags() { return EmitXRegTags; }
bool RISCVXReg::pressureHint() { return XRegPressureHint; }

bool RISCVXReg::isExtendedGPR(MCRegister Reg, const MCRegisterInfo &MRI) {
  if (!Reg.isPhysical())
    return false;
  // x32..x255 and their _W/_H sub-registers all have HWEncoding >= 32 and are
  // the only integer-register-file members with such an encoding.
  return (Reg >= RISCV::X32 && Reg <= RISCV::X255) ||
         (Reg >= RISCV::X32_W && Reg <= RISCV::X255_W) ||
         (Reg >= RISCV::X32_H && Reg <= RISCV::X255_H);
}

bool RISCVXReg::needsPrefix(const MachineInstr &MI,
                            const MCRegisterInfo &MRI) {
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && !MO.isImplicit() && MO.getReg().isPhysical() &&
        isExtendedGPR(MO.getReg(), MRI))
      return true;
  return false;
}
