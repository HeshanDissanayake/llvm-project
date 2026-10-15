//===-- RISCVInstrInfoXReg.td - xreg research prefix -------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xpfx (research-only, LLVM-regsw docs/infra/xreg-encoding.md v0.1): supplies
// the upper 3 bits of the rd/rs1/rs2/rs3 register fields of the next
// instruction, giving access to x32..x255. Inserted by RISCVAsmPrinter in
// front of every instruction that uses one of those registers.
//
//   inst[31:19] = 0, [18:16] rs3h, [15:13] rs2h, [12:10] rs1h, [9:7] rdh,
//   [6:0] = 0001011 (custom-0)
//
//===----------------------------------------------------------------------===//

let Predicates = [HasVendorXReg], hasSideEffects = 1, mayLoad = 0, mayStore = 0,
    DecoderNamespace = "XReg" in
def XPFX : RVInst<(outs), (ins uimm3:$rdh, uimm3:$rs1h, uimm3:$rs2h, uimm3:$rs3h),
                  "xpfx", "$rdh, $rs1h, $rs2h, $rs3h", [], InstFormatOther>,
           Sched<[]> {
  bits<3> rdh;
  bits<3> rs1h;
  bits<3> rs2h;
  bits<3> rs3h;
  let Inst{31-19} = 0;
  let Inst{18-16} = rs3h;
  let Inst{15-13} = rs2h;
  let Inst{12-10} = rs1h;
  let Inst{9-7} = rdh;
  let Inst{6-0} = OPC_CUSTOM_0.Value;
}

// xrget / xrset (H13, research): register-indexed access.
//   xrget rd, rs1, imm   rd = x[(imm + x[rs1]) mod 256]   I-type, custom-2, 000
//   xrset rs2, rs1, imm  x[(imm + x[rs1]) mod 256] = rs2  S-type, custom-2, 001
// The block (imm..) lies above the allocatable registers (reserved), so the
// register allocator never sees it; the instructions keep their order through
// hasSideEffects.
let Predicates = [HasVendorXReg], hasSideEffects = 1, mayLoad = 0,
    mayStore = 0, DecoderNamespace = "XReg" in {
def XRGET : RVInstI<0b000, OPC_CUSTOM_2, (outs GPR:$rd),
                    (ins GPR:$rs1, uimm8:$imm12), "xrget", "$rd, $rs1, $imm12">,
            Sched<[]>;
def XRSET : RVInstS<0b001, OPC_CUSTOM_2, (outs),
                    (ins GPR:$rs2, GPR:$rs1, uimm8:$imm12), "xrset",
                    "$rs2, $rs1, $imm12">,
            Sched<[]>;
}

let Predicates = [HasVendorXReg, IsRV64] in {
def : Pat<(i64 (int_riscv_xreg_rget GPR:$idx, timm:$base)),
          (XRGET GPR:$idx, timm:$base)>;
def : Pat<(int_riscv_xreg_rset GPR:$val, GPR:$idx, timm:$base),
          (XRSET GPR:$val, GPR:$idx, timm:$base)>;
}
