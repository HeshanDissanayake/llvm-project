//===-- RISCVInstrInfoXRegsw.td - Register-bank switching --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the instructions of the 'XRegsw' (register-bank
// switching) research extension, as implemented by the regsw forks of Spike
// (riscv-isa-sim-dev) and CVA6.
//
// The integer register file has a second bank of 32 registers. regsw_c loads a
// 21-bit bank configuration that selects, for each of the next 7 instructions,
// the bank (0 or 1) of its rd, rs1 and rs2 operands:
//
//   cfg = rs1[4:0] << 16 | rs2[4:0] << 11 | imm[10:0]
//   slot k (k = 0..6): rd bank = cfg[20-3k], rs1 bank = cfg[19-3k],
//                      rs2 bank = cfg[18-3k]
//
// The rs1/rs2/imm fields of regsw_c are configuration bits, not register or
// memory operands; the assembly syntax mirrors the S-type store syntax used by
// the regsw binutils fork (e.g. "regsw_c x9, 0x7ff(x30)"). imm[11] is ignored
// by the hardware.
//
// Only the assembler/disassembler (MC layer) is supported; there is no codegen.
//
//===----------------------------------------------------------------------===//

// Major opcode 0b1101011 is reserved in the base opcode map; regsw uses it.
// The empty name keeps it out of the .insn opcode-name lookup.
def OPC_1101011 : RISCVOpcode<"", 0b1101011>;

let Predicates = [HasVendorXRegsw], DecoderNamespace = "XRegsw",
    hasSideEffects = 1, mayLoad = 0, mayStore = 0 in {

// regsw_c: STORE major opcode with the unused funct3 = 0b100.
def REGSW_C : RVInstS<0b100, OPC_STORE, (outs),
                      (ins GPR:$rs2, BasePtr:$rs1, simm12_lo:$imm12),
                      "regsw_c", "$rs2, ${imm12}(${rs1})">,
              Sched<[]>;

// regsw: R-type, funct7 = 0b0000001, funct3 = 0b000. Recognised by the Spike
// fork only (where it has no architectural effect); CVA6 does not decode it.
// rd is a field, not a destination register.
def REGSW : RVInstR<0b0000001, 0b000, OPC_1101011, (outs),
                    (ins GPR:$rd, GPR:$rs1, GPR:$rs2),
                    "regsw", "$rd, $rs1, $rs2">,
            Sched<[]>;

} // Predicates = [HasVendorXRegsw], DecoderNamespace = "XRegsw", ...

