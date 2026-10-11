; RUN: opt -mtriple=riscv64 -passes='default<O3>' -enable-unroll-and-jam \
; RUN:   -riscv-xreg-unj-auto -riscv-num-gprs=256 \
; RUN:   -pass-remarks=loop-unroll-and-jam -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=N256
; RUN: opt -mtriple=riscv64 -passes='default<O3>' -enable-unroll-and-jam \
; RUN:   -riscv-xreg-unj-auto -riscv-num-gprs=32 \
; RUN:   -pass-remarks=loop-unroll-and-jam -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=N32
; RUN: opt -mtriple=riscv64 -passes='default<O3>' -enable-unroll-and-jam \
; RUN:   -pass-remarks=loop-unroll-and-jam -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=STOCK --allow-empty

; xreg (H4c): -riscv-xreg-unj-auto picks the unroll-and-jam count from a
; register budget (TTI::getUnrollAndJamCount): larger with more registers.
;   for i, for k, for j: C[i][j] += A[i][k] * B[k][j]
; N256: unroll and jammed loop by a factor of 16
; N32: unroll and jammed loop by a factor of 3
; STOCK-NOT: unroll and jammed


@A = dso_local global [64 x [64 x i32]] zeroinitializer, align 4
@B = dso_local global [64 x [64 x i32]] zeroinitializer, align 4
@C = dso_local global [64 x [64 x i32]] zeroinitializer, align 4

; Function Attrs: nounwind
define dso_local void @mm() {
entry:
  %i = alloca i32, align 4
  %cleanup.dest.slot = alloca i32, align 4
  %k = alloca i32, align 4
  %j = alloca i32, align 4
  call void @llvm.lifetime.start.p0(ptr %i)
  store i32 0, ptr %i, align 4, !tbaa !10
  br label %for.cond

for.cond:                                         ; preds = %for.inc22, %entry
  %0 = load i32, ptr %i, align 4, !tbaa !10
  %cmp = icmp slt i32 %0, 64
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  store i32 2, ptr %cleanup.dest.slot, align 4
  call void @llvm.lifetime.end.p0(ptr %i)
  br label %for.end24

for.body:                                         ; preds = %for.cond
  call void @llvm.lifetime.start.p0(ptr %k)
  store i32 0, ptr %k, align 4, !tbaa !10
  br label %for.cond1

for.cond1:                                        ; preds = %for.inc19, %for.body
  %1 = load i32, ptr %k, align 4, !tbaa !10
  %cmp2 = icmp slt i32 %1, 64
  br i1 %cmp2, label %for.body4, label %for.cond.cleanup3

for.cond.cleanup3:                                ; preds = %for.cond1
  store i32 5, ptr %cleanup.dest.slot, align 4
  call void @llvm.lifetime.end.p0(ptr %k)
  br label %for.end21

for.body4:                                        ; preds = %for.cond1
  call void @llvm.lifetime.start.p0(ptr %j)
  store i32 0, ptr %j, align 4, !tbaa !10
  br label %for.cond5

for.cond5:                                        ; preds = %for.inc, %for.body4
  %2 = load i32, ptr %j, align 4, !tbaa !10
  %cmp6 = icmp slt i32 %2, 64
  br i1 %cmp6, label %for.body8, label %for.cond.cleanup7

for.cond.cleanup7:                                ; preds = %for.cond5
  store i32 8, ptr %cleanup.dest.slot, align 4
  call void @llvm.lifetime.end.p0(ptr %j)
  br label %for.end

for.body8:                                        ; preds = %for.cond5
  %3 = load i32, ptr %i, align 4, !tbaa !10
  %idxprom = sext i32 %3 to i64
  %arrayidx = getelementptr inbounds [64 x [64 x i32]], ptr @A, i64 0, i64 %idxprom
  %4 = load i32, ptr %k, align 4, !tbaa !10
  %idxprom9 = sext i32 %4 to i64
  %arrayidx10 = getelementptr inbounds [64 x i32], ptr %arrayidx, i64 0, i64 %idxprom9
  %5 = load i32, ptr %arrayidx10, align 4, !tbaa !10
  %6 = load i32, ptr %k, align 4, !tbaa !10
  %idxprom11 = sext i32 %6 to i64
  %arrayidx12 = getelementptr inbounds [64 x [64 x i32]], ptr @B, i64 0, i64 %idxprom11
  %7 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom13 = sext i32 %7 to i64
  %arrayidx14 = getelementptr inbounds [64 x i32], ptr %arrayidx12, i64 0, i64 %idxprom13
  %8 = load i32, ptr %arrayidx14, align 4, !tbaa !10
  %mul = mul nsw i32 %5, %8
  %9 = load i32, ptr %i, align 4, !tbaa !10
  %idxprom15 = sext i32 %9 to i64
  %arrayidx16 = getelementptr inbounds [64 x [64 x i32]], ptr @C, i64 0, i64 %idxprom15
  %10 = load i32, ptr %j, align 4, !tbaa !10
  %idxprom17 = sext i32 %10 to i64
  %arrayidx18 = getelementptr inbounds [64 x i32], ptr %arrayidx16, i64 0, i64 %idxprom17
  %11 = load i32, ptr %arrayidx18, align 4, !tbaa !10
  %add = add nsw i32 %11, %mul
  store i32 %add, ptr %arrayidx18, align 4, !tbaa !10
  br label %for.inc

for.inc:                                          ; preds = %for.body8
  %12 = load i32, ptr %j, align 4, !tbaa !10
  %inc = add nsw i32 %12, 1
  store i32 %inc, ptr %j, align 4, !tbaa !10
  br label %for.cond5, !llvm.loop !11

for.end:                                          ; preds = %for.cond.cleanup7
  br label %for.inc19

for.inc19:                                        ; preds = %for.end
  %13 = load i32, ptr %k, align 4, !tbaa !10
  %inc20 = add nsw i32 %13, 1
  store i32 %inc20, ptr %k, align 4, !tbaa !10
  br label %for.cond1, !llvm.loop !13

for.end21:                                        ; preds = %for.cond.cleanup3
  br label %for.inc22

for.inc22:                                        ; preds = %for.end21
  %14 = load i32, ptr %i, align 4, !tbaa !10
  %inc23 = add nsw i32 %14, 1
  store i32 %inc23, ptr %i, align 4, !tbaa !10
  br label %for.cond, !llvm.loop !14

for.end24:                                        ; preds = %for.cond.cleanup
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
!4 = !{!"clang version 23.1.3 (https://github.com/llvm/llvm-project.git fbbc610707f96cd87dc60d64dddb9e16f7480009)"}
!5 = !{!6, !7, i64 0}
!6 = !{!"__libc_errno", !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = !{!7, !7, i64 0}
!11 = distinct !{!11, !12}
!12 = !{!"llvm.loop.mustprogress"}
!13 = distinct !{!13, !12}
!14 = distinct !{!14, !12}
