; RUN: opt -mtriple=riscv64 -passes='default<O3>' -enable-unroll-and-jam \
; RUN:   -riscv-xreg-unj-auto -riscv-xreg-unj-max=8 -riscv-xreg-unj-shifted \
; RUN:   -riscv-num-gprs=256 -pass-remarks=loop-unroll-and-jam \
; RUN:   -disable-output < %s 2>&1 | FileCheck %s --check-prefix=SHIFT
; RUN: opt -mtriple=riscv64 -passes='default<O3>' -enable-unroll-and-jam \
; RUN:   -riscv-xreg-unj-auto -riscv-xreg-unj-max=8 -riscv-num-gprs=256 \
; RUN:   -pass-remarks=loop-unroll-and-jam -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NOSHIFT --allow-empty

; xreg (H4d): stencil rows A[i-1][j], A[i][j], A[i+1][j] are shared between
; copies jammed along i (shifted reuse), so -riscv-xreg-unj-shifted jams the
; i loop; without it there are no loop-invariant loads and nothing is jammed.
; SHIFT: unroll and jammed loop by a factor of {{[2-8]}}
; NOSHIFT-NOT: unroll and jammed


@A = dso_local global [64 x [64 x i32]] zeroinitializer, align 4
@B = dso_local global [64 x [64 x i32]] zeroinitializer, align 4

; Function Attrs: nounwind
define dso_local void @stencil() {
entry:
  %i = alloca i32, align 4
  %cleanup.dest.slot = alloca i32, align 4
  %j = alloca i32, align 4
  call void @llvm.lifetime.start.p0(ptr %i)
  store i32 1, ptr %i, align 4, !tbaa !10
  br label %for.cond

for.cond:                                         ; preds = %for.inc21, %entry
  %0 = load i32, ptr %i, align 4, !tbaa !10
  %cmp = icmp slt i32 %0, 63
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  store i32 2, ptr %cleanup.dest.slot, align 4
  call void @llvm.lifetime.end.p0(ptr %i)
  br label %for.end23

for.body:                                         ; preds = %for.cond
  call void @llvm.lifetime.start.p0(ptr %j)
  store i32 1, ptr %j, align 4, !tbaa !10
  br label %for.cond1

for.cond1:                                        ; preds = %for.inc, %for.body
  %1 = load i32, ptr %j, align 4, !tbaa !10
  %cmp2 = icmp slt i32 %1, 63
  br i1 %cmp2, label %for.body4, label %for.cond.cleanup3

for.cond.cleanup3:                                ; preds = %for.cond1
  store i32 5, ptr %cleanup.dest.slot, align 4
  call void @llvm.lifetime.end.p0(ptr %j)
  br label %for.end

for.body4:                                        ; preds = %for.cond1
  %2 = load i32, ptr %i, align 4, !tbaa !10
  %sub = sub nsw i32 %2, 1
  %idxprom = sext i32 %sub to i64
  %arrayidx = getelementptr inbounds [64 x [64 x i32]], ptr @A, i64 0, i64 %idxprom
  %3 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom5 = sext i32 %3 to i64
  %arrayidx6 = getelementptr inbounds [64 x i32], ptr %arrayidx, i64 0, i64 %idxprom5
  %4 = load i32, ptr %arrayidx6, align 4, !tbaa !10
  %5 = load i32, ptr %i, align 4, !tbaa !10
  %idxprom7 = sext i32 %5 to i64
  %arrayidx8 = getelementptr inbounds [64 x [64 x i32]], ptr @A, i64 0, i64 %idxprom7
  %6 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom9 = sext i32 %6 to i64
  %arrayidx10 = getelementptr inbounds [64 x i32], ptr %arrayidx8, i64 0, i64 %idxprom9
  %7 = load i32, ptr %arrayidx10, align 4, !tbaa !10
  %add = add nsw i32 %4, %7
  %8 = load i32, ptr %i, align 4, !tbaa !10
  %add11 = add nsw i32 %8, 1
  %idxprom12 = sext i32 %add11 to i64
  %arrayidx13 = getelementptr inbounds [64 x [64 x i32]], ptr @A, i64 0, i64 %idxprom12
  %9 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom14 = sext i32 %9 to i64
  %arrayidx15 = getelementptr inbounds [64 x i32], ptr %arrayidx13, i64 0, i64 %idxprom14
  %10 = load i32, ptr %arrayidx15, align 4, !tbaa !10
  %add16 = add nsw i32 %add, %10
  %11 = load i32, ptr %i, align 4, !tbaa !10
  %idxprom17 = sext i32 %11 to i64
  %arrayidx18 = getelementptr inbounds [64 x [64 x i32]], ptr @B, i64 0, i64 %idxprom17
  %12 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom19 = sext i32 %12 to i64
  %arrayidx20 = getelementptr inbounds [64 x i32], ptr %arrayidx18, i64 0, i64 %idxprom19
  store i32 %add16, ptr %arrayidx20, align 4, !tbaa !10
  br label %for.inc

for.inc:                                          ; preds = %for.body4
  %13 = load i32, ptr %j, align 4, !tbaa !10
  %inc = add nsw i32 %13, 1
  store i32 %inc, ptr %j, align 4, !tbaa !10
  br label %for.cond1, !llvm.loop !11

for.end:                                          ; preds = %for.cond.cleanup3
  br label %for.inc21

for.inc21:                                        ; preds = %for.end
  %14 = load i32, ptr %i, align 4, !tbaa !10
  %inc22 = add nsw i32 %14, 1
  store i32 %inc22, ptr %i, align 4, !tbaa !10
  br label %for.cond, !llvm.loop !13

for.end23:                                        ; preds = %for.cond.cleanup
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none))

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none))


!llvm.errno.tbaa = !{!5}

!0 = !{i32 1, !"target-abi", !"lp64d"}
!1 = !{i32 6, !"riscv-isa", !2}
!2 = !{!"rv64i2p1_m2p0_a2p1_f2p2_d2p2_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0"}
!3 = !{i32 8, !"SmallDataLimit", i32 0}
!4 = !{!"clang version 23.1.3 (https://github.com/llvm/llvm-project.git 4effbed6443d78adb2e85fcd6af66f78884d5fce)"}
!5 = !{!6, !7, i64 0}
!6 = !{!"__libc_errno", !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = !{!7, !7, i64 0}
!11 = distinct !{!11, !12}
!12 = !{!"llvm.loop.mustprogress"}
!13 = distinct !{!13, !12}
