; RUN: llc -mtriple=riscv64 -mattr=+m < %s | FileCheck %s --check-prefix=STOCK
; RUN: llc -mtriple=riscv64 -mattr=+m -riscv-xreg-csr-cost=4 < %s \
; RUN:   | FileCheck %s --check-prefix=COST

; xreg (H2b): -riscv-xreg-csr-cost=C prices the first use of a callee-saved
; register at C spill-cost units per function entry. A value that is live
; only across a cold call is then spilled around that call instead of
; being kept in s0, which would be saved/restored on every entry.

declare void @cold_fn() cold
; STOCK-LABEL: f:
; STOCK: sd s0,
; COST-LABEL: f:
; COST-NOT: sd s0,
; COST: ret
define i64 @f(i64 %x, i1 %c) nounwind {
entry:
  %y = mul i64 %x, %x
  br i1 %c, label %cold, label %done, !prof !0
cold:
  call void @cold_fn()
  br label %done
done:
  %r = add i64 %y, %x
  ret i64 %r
}
!0 = !{!"branch_weights", i32 1, i32 1000}
