# RUN: not llvm-mc -triple=riscv64 -mattr=+xregsw %s 2>&1 \
# RUN:     | FileCheck %s

# CHECK: :[[@LINE+1]]:13: error: operand must be a symbol with %lo/%pcrel_lo/%tprel_lo specifier or an integer in the range [-2048, 2047]
regsw_c x9, 2048(x30)

# CHECK: :[[@LINE+1]]:13: error: operand must be a symbol with %lo/%pcrel_lo/%tprel_lo specifier or an integer in the range [-2048, 2047]
regsw_c x9, -2049(x30)

# CHECK: :[[@LINE+1]]:11: error: too few operands for instruction
regsw_c x9

# CHECK: :[[@LINE+1]]:13: error: too few operands for instruction
regsw x5, x6

# CHECK: :[[@LINE+1]]:15: error: invalid operand for instruction
regsw x5, x6, 7
