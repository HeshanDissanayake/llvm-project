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

#include "llvm/MC/MCRegister.h"

namespace llvm {
class MachineInstr;
class MCRegisterInfo;

namespace RISCVXReg {

/// Number of allocatable integer registers x0..x(N-1) (-riscv-num-gprs).
/// 32 means stock behaviour: x32..x255 are reserved.
unsigned getNumGPRs();

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
} // namespace llvm

#endif
