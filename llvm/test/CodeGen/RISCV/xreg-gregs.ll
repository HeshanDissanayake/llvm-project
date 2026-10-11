; RUN: opt -mtriple=riscv64 -passes=riscv-xreg-gregs -riscv-num-gprs=224 \
; RUN:   -riscv-xreg-gregs=g,h:8 -S < %s | FileCheck %s
; RUN: opt -mtriple=riscv64 -passes=riscv-xreg-gregs -riscv-num-gprs=224 \
; RUN:   -riscv-xreg-gregs=g,h:8 < %s | llc -mtriple=riscv64 -mattr=+xreg \
; RUN:   -riscv-num-gprs=224 -riscv-xreg-gregs=g,h:8 \
; RUN:   | FileCheck %s --check-prefix=ASM
; RUN: not opt -mtriple=riscv64 -passes=riscv-xreg-gregs -riscv-num-gprs=224 \
; RUN:   -riscv-xreg-gregs=a -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERR
; RUN: not opt -mtriple=riscv64 -passes=riscv-xreg-gregs -riscv-num-gprs=256 \
; RUN:   -riscv-xreg-gregs=g -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERRN

; xreg (H6): -riscv-xreg-gregs keeps listed globals in x255, x254, ...
; Loads become register reads, stores register writes; main seeds the
; registers from memory (h isn't defined here: width from ":8").

@g = global i32 7
@a = global i32 0
declare void @f()

; CHECK-LABEL: define i32 @use(
; CHECK: call i64 @llvm.read_register.i64(metadata [[G:![0-9]+]])
; CHECK: call void @llvm.write_register.i64(metadata [[G]], i64
; CHECK-NOT: load i32, ptr @g
; CHECK-NOT: store i32 {{.*}} @g
define i32 @use(i32 %x) {
  %v = load i32, ptr @g
  %s = add i32 %v, %x
  store i32 %s, ptr @g
  call void @f()
  %w = load i32, ptr @g
  ret i32 %w
}

; The read after the call must stay after the call (x255 may change).
; ASM-LABEL: use:
; ASM: call f
; ASM: mv a0, x255

; CHECK-LABEL: define i32 @main(
; CHECK: load i32, ptr @g
; CHECK: call void @llvm.write_register.i64(metadata [[G]]
; CHECK: load i8, ptr @h
; CHECK: call void @llvm.write_register.i64(metadata [[H:![0-9]+]]
define i32 @main() {
  ret i32 0
}

@b = global ptr @a
; ERR: global 'a' has a use other than a simple load/store
; ERRN: must not be allocatable

; CHECK: [[G]] = !{!"x255"}
; CHECK: [[H]] = !{!"x254"}
