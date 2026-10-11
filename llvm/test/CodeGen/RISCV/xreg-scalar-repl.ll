; RUN: opt -mtriple=riscv64 -passes='default<O3>' -riscv-xreg-sr \
; RUN:   -pass-remarks=riscv-xreg-scalar-repl -S < %s 2>%t.remarks | FileCheck %s
; RUN: FileCheck %s --check-prefix=REMARK < %t.remarks
; RUN: opt -mtriple=riscv64 -passes='default<O3>' -S < %s \
; RUN:   | FileCheck %s --check-prefix=STOCK

; xreg (H4b): scalar replacement of loads across innermost-loop iterations.
; jac: b[j] = a[j-1] + a[j] + a[j+1] needs only one load of a per iteration;
; the other two values rotate through registers (header phis seeded in the
; preheader). inplace: the store to a[j] may modify what is loaded, so it
; is left alone.

; REMARK: remark: {{.*}} replaced {{[12]}} loads with 2 rotating registers
; REMARK-NOT: remark:

; CHECK-LABEL: define {{.*}} @jac(
; CHECK: xreg.sr.init
; CHECK: phi i32 {{.*}}xreg.sr.init
; CHECK-LABEL: define {{.*}} @inplace(
; CHECK-NOT: xreg.sr
; CHECK: ret void

; STOCK-NOT: xreg.sr


; Function Attrs: nounwind
define dso_local void @jac(i32 noundef signext %n, ptr noalias noundef %b, ptr noalias noundef %a) {
entry:
  %n.addr = alloca i32, align 4
  %b.addr = alloca ptr, align 8
  %a.addr = alloca ptr, align 8
  %j = alloca i32, align 4
  store i32 %n, ptr %n.addr, align 4, !tbaa !10
  store ptr %b, ptr %b.addr, align 8, !tbaa !11
  store ptr %a, ptr %a.addr, align 8, !tbaa !11
  call void @llvm.lifetime.start.p0(ptr %j)
  store i32 1, ptr %j, align 4, !tbaa !10
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %0 = load i32, ptr %j, align 4, !tbaa !10
  %1 = load i32, ptr %n.addr, align 4, !tbaa !10
  %sub = sub nsw i32 %1, 1
  %cmp = icmp slt i32 %0, %sub
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  call void @llvm.lifetime.end.p0(ptr %j)
  br label %for.end

for.body:                                         ; preds = %for.cond
  %2 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %3 = load i32, ptr %j, align 4, !tbaa !10
  %sub1 = sub nsw i32 %3, 1
  %idxprom = sext i32 %sub1 to i64
  %arrayidx = getelementptr inbounds i32, ptr %2, i64 %idxprom
  %4 = load i32, ptr %arrayidx, align 4, !tbaa !10
  %5 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %6 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom2 = sext i32 %6 to i64
  %arrayidx3 = getelementptr inbounds i32, ptr %5, i64 %idxprom2
  %7 = load i32, ptr %arrayidx3, align 4, !tbaa !10
  %add = add nsw i32 %4, %7
  %8 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %9 = load i32, ptr %j, align 4, !tbaa !10
  %add4 = add nsw i32 %9, 1
  %idxprom5 = sext i32 %add4 to i64
  %arrayidx6 = getelementptr inbounds i32, ptr %8, i64 %idxprom5
  %10 = load i32, ptr %arrayidx6, align 4, !tbaa !10
  %add7 = add nsw i32 %add, %10
  %11 = load ptr, ptr %b.addr, align 8, !tbaa !11
  %12 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom8 = sext i32 %12 to i64
  %arrayidx9 = getelementptr inbounds i32, ptr %11, i64 %idxprom8
  store i32 %add7, ptr %arrayidx9, align 4, !tbaa !10
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %13 = load i32, ptr %j, align 4, !tbaa !10
  %inc = add nsw i32 %13, 1
  store i32 %inc, ptr %j, align 4, !tbaa !10
  br label %for.cond, !llvm.loop !14

for.end:                                          ; preds = %for.cond.cleanup
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none))

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none))

; Function Attrs: nounwind
define dso_local void @inplace(i32 noundef signext %n, ptr noundef %a) {
entry:
  %n.addr = alloca i32, align 4
  %a.addr = alloca ptr, align 8
  %j = alloca i32, align 4
  store i32 %n, ptr %n.addr, align 4, !tbaa !10
  store ptr %a, ptr %a.addr, align 8, !tbaa !11
  call void @llvm.lifetime.start.p0(ptr %j)
  store i32 1, ptr %j, align 4, !tbaa !10
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %0 = load i32, ptr %j, align 4, !tbaa !10
  %1 = load i32, ptr %n.addr, align 4, !tbaa !10
  %sub = sub nsw i32 %1, 1
  %cmp = icmp slt i32 %0, %sub
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  call void @llvm.lifetime.end.p0(ptr %j)
  br label %for.end

for.body:                                         ; preds = %for.cond
  %2 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %3 = load i32, ptr %j, align 4, !tbaa !10
  %sub1 = sub nsw i32 %3, 1
  %idxprom = sext i32 %sub1 to i64
  %arrayidx = getelementptr inbounds i32, ptr %2, i64 %idxprom
  %4 = load i32, ptr %arrayidx, align 4, !tbaa !10
  %5 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %6 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom2 = sext i32 %6 to i64
  %arrayidx3 = getelementptr inbounds i32, ptr %5, i64 %idxprom2
  %7 = load i32, ptr %arrayidx3, align 4, !tbaa !10
  %add = add nsw i32 %4, %7
  %8 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %9 = load i32, ptr %j, align 4, !tbaa !10
  %add4 = add nsw i32 %9, 1
  %idxprom5 = sext i32 %add4 to i64
  %arrayidx6 = getelementptr inbounds i32, ptr %8, i64 %idxprom5
  %10 = load i32, ptr %arrayidx6, align 4, !tbaa !10
  %add7 = add nsw i32 %add, %10
  %11 = load ptr, ptr %a.addr, align 8, !tbaa !11
  %12 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom8 = sext i32 %12 to i64
  %arrayidx9 = getelementptr inbounds i32, ptr %11, i64 %idxprom8
  store i32 %add7, ptr %arrayidx9, align 4, !tbaa !10
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %13 = load i32, ptr %j, align 4, !tbaa !10
  %inc = add nsw i32 %13, 1
  store i32 %inc, ptr %j, align 4, !tbaa !10
  br label %for.cond, !llvm.loop !16

for.end:                                          ; preds = %for.cond.cleanup
  ret void
}


!llvm.errno.tbaa = !{!5}

!0 = !{i32 1, !"target-abi", !"lp64d"}
!1 = !{i32 6, !"riscv-isa", !2}
!2 = !{!"rv64i2p1_m2p0_a2p1_f2p2_d2p2_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0"}
!3 = !{i32 8, !"SmallDataLimit", i32 0}
!4 = !{!"clang version 23.1.3 (https://github.com/llvm/llvm-project.git dfadbcb2f78629acc0bf5a616fba9a63aa4199f2)"}
!5 = !{!6, !7, i64 0}
!6 = !{!"__libc_errno", !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = !{!7, !7, i64 0}
!11 = !{!12, !12, i64 0}
!12 = !{!"p1 int", !13, i64 0}
!13 = !{!"any pointer", !8, i64 0}
!14 = distinct !{!14, !15}
!15 = !{!"llvm.loop.mustprogress"}
!16 = distinct !{!16, !15}
