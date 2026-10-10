# xreg research extension (256 integer registers via the xpfx prefix).
# RUN: llvm-mc %s -triple=riscv64 -mattr=+xreg -M no-aliases -show-encoding \
# RUN:     | FileCheck -check-prefix=CHECK-ASM %s
# RUN: not llvm-mc -triple=riscv64 %s 2>&1 | FileCheck -check-prefix=CHECK-NOEXT %s

# xpfx rdh, rs1h, rs2h, rs3h: custom-0, payload in inst[18:7].
# CHECK-ASM: xpfx 1, 2, 3, 0
# CHECK-ASM: encoding: [0x8b,0x68,0x00,0x00]
# CHECK-NOEXT: error: instruction requires the following: 'XReg' (Research: 256 Integer Registers via xpfx Prefix)
xpfx 1, 2, 3, 0

# CHECK-ASM: xpfx 7, 7, 7, 7
# CHECK-ASM: encoding: [0x8b,0xff,0x07,0x00]
xpfx 7, 7, 7, 7

# x32..x255 are accepted with +xreg; the encoding holds the low 5 bits (the
# prefix supplies the rest).
# CHECK-ASM: add x40, x72, x255
# CHECK-ASM: encoding: [0x33,0x04,0xf4,0x01]
# CHECK-NOEXT: error: invalid operand for instruction
add x40, x72, x255
