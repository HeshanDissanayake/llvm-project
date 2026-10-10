# RUN: llvm-mc %s -triple=riscv32 -mattr=+xregsw -M no-aliases -show-encoding \
# RUN:     | FileCheck -check-prefixes=CHECK-ASM,CHECK-ASM-AND-OBJ %s
# RUN: llvm-mc %s -triple=riscv64 -mattr=+xregsw -M no-aliases -show-encoding \
# RUN:     | FileCheck -check-prefixes=CHECK-ASM,CHECK-ASM-AND-OBJ %s
# RUN: llvm-mc -filetype=obj -triple=riscv32 -mattr=+xregsw < %s \
# RUN:     | llvm-objdump --mattr=+xregsw -M no-aliases -d - \
# RUN:     | FileCheck -check-prefix=CHECK-ASM-AND-OBJ %s
# RUN: llvm-mc -filetype=obj -triple=riscv64 -mattr=+xregsw < %s \
# RUN:     | llvm-objdump --mattr=+xregsw -M no-aliases -d - \
# RUN:     | FileCheck -check-prefix=CHECK-ASM-AND-OBJ %s
# RUN: not llvm-mc -triple=riscv64 -show-encoding %s 2>&1 \
# RUN:     | FileCheck %s --check-prefix=CHECK-NO-EXT

# regsw_c rs2, imm(rs1): config = rs1 << 16 | rs2 << 11 | imm[10:0].
# Expected words cross-checked against GNU as ".insn s 0x23, 4, ...", the
# Spike fork (MATCH_REGSW_C 0x4023 / MASK 0x707f) and the regsw binutils fork.

# config 111100100111111111111
# CHECK-ASM-AND-OBJ: regsw_c s1, {{2047|0x7ff}}(t5)
# CHECK-ASM: encoding: [0xa3,0x4f,0x9f,0x7e]
# CHECK-NO-EXT: instruction requires the following: 'XRegsw' (Register-Bank Switching Instructions){{$}}
regsw_c x9, 0x7ff(x30)

# config 111111011000000000000
# CHECK-ASM-AND-OBJ: regsw_c s6, {{0|0x0}}(t6)
# CHECK-ASM: encoding: [0x23,0xc0,0x6f,0x01]
regsw_c x22, 0x0(x31)

# config 000110000100011111111
# CHECK-ASM-AND-OBJ: regsw_c ra, {{255|0xff}}(gp)
# CHECK-ASM: encoding: [0xa3,0xcf,0x11,0x0e]
regsw_c x1, 0xff(x3)

# all-zero config (MATCH_REGSW_C)
# CHECK-ASM-AND-OBJ: regsw_c zero, {{0|0x0}}(zero)
# CHECK-ASM: encoding: [0x23,0x40,0x00,0x00]
regsw_c x0, 0(x0)

# imm[11] (instruction bit 31) is encodable but ignored by the hardware
# CHECK-ASM-AND-OBJ: regsw_c t6, {{-1|-0x1}}(t6)
# CHECK-ASM: encoding: [0xa3,0xcf,0xff,0xff]
regsw_c x31, -1(x31)

# regsw rd, rs1, rs2 (Spike fork only; MATCH_REGSW 0x200006b / MASK 0xfe00707f)
# CHECK-ASM-AND-OBJ: regsw t0, t1, t2
# CHECK-ASM: encoding: [0xeb,0x02,0x73,0x02]
regsw x5, x6, x7

# CHECK-ASM-AND-OBJ: regsw zero, zero, zero
# CHECK-ASM: encoding: [0x6b,0x00,0x00,0x02]
regsw x0, x0, x0
