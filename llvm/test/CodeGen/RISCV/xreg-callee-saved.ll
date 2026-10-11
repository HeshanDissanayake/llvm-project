; RUN: llc -mtriple=riscv64 -mattr=+m,+xreg -riscv-num-gprs=64 < %s \
; RUN:   | FileCheck %s --check-prefix=CALLER
; RUN: llc -mtriple=riscv64 -mattr=+m,+xreg -riscv-num-gprs=64 \
; RUN:   -riscv-xreg-callee-saved=16 < %s | FileCheck %s --check-prefix=CALLEE
; RUN: not --crash llc -mtriple=riscv64 -mattr=+m,+xreg -riscv-num-gprs=40 \
; RUN:   -riscv-xreg-callee-saved=16 < %s 2>&1 | FileCheck %s --check-prefix=ERR

; xreg (H2): -riscv-xreg-callee-saved=K makes x(N-K)..x(N-1) callee-saved.

; With all extras caller-saved, clobbering x63 or x47 needs no save.
; CALLER-LABEL: clobber:
; CALLER-NOT: sd x63
; CALLER-NOT: sd x47
; CALLER: ret
; With K=16 at N=64, x48..x63 are callee-saved: x63 is saved and restored,
; x47 (still caller-saved) is not.
; CALLEE-LABEL: clobber:
; CALLEE: sd x63, {{[0-9]+}}(sp)
; CALLEE-NOT: sd x47
; CALLEE: ld x63, {{[0-9]+}}(sp)
; CALLEE: ret
define void @clobber() nounwind {
  call void asm sideeffect "", "~{x63},~{x47}"()
  ret void
}

; A caller keeps a value across a call in a callee-saved extra register.
; CALLEE-LABEL: across:
; CALLEE: call g
; CALLEE-NOT: ld {{.*}}(sp)
; CALLEE: add a0, a0, x{{(4[89]|5[0-9]|6[0-3])}}
declare i64 @g(i64)
define i64 @across(i64 %x) nounwind {
  %y = mul i64 %x, %x
  %r = call i64 @g(i64 %x)
  %s = add i64 %r, %y
  ret i64 %s
}

; ERR: -riscv-xreg-callee-saved must be <= riscv-num-gprs - 32
