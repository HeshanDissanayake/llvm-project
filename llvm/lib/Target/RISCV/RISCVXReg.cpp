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
