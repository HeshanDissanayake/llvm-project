# RUN: not llvm-mc -triple=riscv64 -mattr=+xreg %s 2>&1 | FileCheck %s

# CHECK: :[[@LINE+1]]:6: error: immediate must be an integer in the range [0, 7]
xpfx 8, 0, 0, 0
# CHECK: :[[@LINE+1]]:13: error: too few operands for instruction
xpfx 1, 2, 3
# CHECK: :[[@LINE+1]]:5: error: invalid operand for instruction
add x256, x1, x2
