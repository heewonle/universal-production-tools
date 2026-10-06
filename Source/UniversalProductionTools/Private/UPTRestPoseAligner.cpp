#include "UPTRestPoseAligner.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTRestPose, Log, All);

// 정렬을 껐다 켜며 결과를 비교하기 위한 스위치. 각도는 좋아지는데 접지가 나빠지는 식의
// 맞교환이 있는지 확인하려면 같은 빌드에서 양쪽을 재야 한다.
static TAutoConsoleVariable<int32> CVarRestPoseAlign(
    TEXT("UPT.RestPoseAlign"), 1,
    TEXT("1이면 자체 분석 Rig에 레스트 포즈 정렬을 적용한다. 0이면 건너뛴다(비교용)."));

namespace
{
// 본 하나를 정렬했다고 보기에 너무 작은 각도. 떨림만 만들고 의미가 없다.
constexpr double MinAlignRadians = 0.5 * PI / 180.0;

/** 체인의 끝 본에서 시작 본까지 계층을 거슬러 올라가 본 목록을 만든다(시작 → 끝 순서로 돌려준다). */
TArray<FName> CollectChainBones(const FReferenceSkeleton& Ref, const FBoneChain& Chain)
{
    TArray<FName> Reversed;
    const FName StartName = Chain.StartBone.BoneName;
    const FName EndName = Chain.EndBone.BoneName;
    int32 Index = EndName != NAME_None ? Ref.FindBoneIndex(EndName) : Ref.FindBoneIndex(StartName);
    for (int32 Guard = 0; Index != INDEX_NONE && Guard < 64; ++Guard)
    {
        const FName BoneName = Ref.GetBoneName(Index);
        Reversed.Add(BoneName);
        if (BoneName == StartName) break;
        Index = Ref.GetParentIndex(Index);
    }
    Algo::Reverse(Reversed);
    // 시작 본에 닿지 못했으면(계층이 끊겼으면) 체인으로 쓰지 않는다.
    if (StartName != NAME_None && (Reversed.IsEmpty() || Reversed[0] != StartName)) return {};
    return Reversed;
}

/** 레퍼런스 포즈를 컴포넌트 공간으로 합성한다. */
void BuildComponentPose(const FReferenceSkeleton& Ref, TArray<FTransform>& OutPose)
{
    const TArray<FTransform>& Local = Ref.GetRefBonePose();
    OutPose.SetNum(Local.Num());
    for (int32 Index = 0; Index < Local.Num(); ++Index)
    {
        const int32 Parent = Ref.GetParentIndex(Index);
        OutPose[Index] = Parent == INDEX_NONE ? Local[Index] : Local[Index] * OutPose[Parent];
    }
}

/** 체인을 따라가며 마디마다 (누적 길이 비율, 방향 단위벡터)를 만든다. */
struct FChainSegment
{
    double Param = 0.0;      // 체인 시작에서 이 마디 시작까지의 누적 길이 비율
    FVector Direction = FVector::ZeroVector;
};

TArray<FChainSegment> BuildSegments(const FReferenceSkeleton& Ref, const TArray<FTransform>& Pose, const TArray<FName>& Bones)
{
    TArray<FChainSegment> Segments;
    TArray<double> Lengths;
    double Total = 0.0;
    for (int32 Index = 0; Index + 1 < Bones.Num(); ++Index)
    {
        const int32 A = Ref.FindBoneIndex(Bones[Index]);
        const int32 B = Ref.FindBoneIndex(Bones[Index + 1]);
        if (A == INDEX_NONE || B == INDEX_NONE) return {};
        const FVector Delta = Pose[B].GetLocation() - Pose[A].GetLocation();
        const double Length = Delta.Size();
        if (Length <= KINDA_SMALL_NUMBER) return {};
        Segments.Add({0.0, Delta / Length});
        Lengths.Add(Length);
        Total += Length;
    }
    if (Segments.IsEmpty() || Total <= KINDA_SMALL_NUMBER) return {};

    double Walked = 0.0;
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        Segments[Index].Param = Walked / Total;
        Walked += Lengths[Index];
    }
    return Segments;
}

/** 마디의 중점 비율. 어느 마디끼리 대응하는지 따질 때의 대표 위치다. */
double SegmentMidpoint(const TArray<FChainSegment>& Segments, int32 Index)
{
    const double Start = Segments[Index].Param;
    const double End = Index + 1 < Segments.Num() ? Segments[Index + 1].Param : 1.0;
    return 0.5 * (Start + End);
}

/** 대상 마디의 중점에 가장 가까운 원본 마디의 방향.
 *
 * 처음에는 "대상 마디의 **시작** 비율 이하인 마지막 원본 마디"를 집었다. 그 한 끗 때문에
 * UE4 마네킹 원본 → 골렘에서 팔이 32° 어긋났다. 골렘의 아래팔 마디는 0.519에서 시작하는데
 * 원본의 아래팔 마디는 0.529에서 시작한다. 0.519 <= 0.529라 **아래팔이 위팔 방향에 맞춰졌고**,
 * 그 차이가 곧 팔꿈치 각도였다.
 * 양옆을 섞는 보간도 시도했지만 더 나빴다(관절이 굽어 있으면 어느 쪽도 아닌 방향이 나온다).
 * 섞지 않고 **중점끼리 가장 가까운 마디 하나**를 고르는 것이 맞다.
 */
FVector DirectionAtMidpoint(const TArray<FChainSegment>& Segments, double TargetMidpoint)
{
    int32 Best = 0;
    double BestDistance = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        const double Distance = FMath::Abs(SegmentMidpoint(Segments, Index) - TargetMidpoint);
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Index;
        }
    }
    return Segments[Best].Direction;
}
}

int32 FUPTRestPoseAligner::AlignTargetToSource(
    UIKRetargeterController* Controller,
    USkeletalMesh* SourceMesh, const UIKRigDefinition* SourceIKRig,
    USkeletalMesh* TargetMesh, const UIKRigDefinition* TargetIKRig,
    const TSet<FName>& MappedChainNames)
{
    if (!Controller || !SourceMesh || !TargetMesh || !SourceIKRig || !TargetIKRig) return 0;
    if (CVarRestPoseAlign.GetValueOnAnyThread() == 0)
    {
        UE_LOG(LogUPTRestPose, Log, TEXT("UPT_RESTPOSE UPT.RestPoseAlign=0 이라 정렬을 건너뜁니다."));
        return 0;
    }
    const UIKRigController* SourceRig = UIKRigController::GetController(SourceIKRig);
    const UIKRigController* TargetRig = UIKRigController::GetController(TargetIKRig);
    if (!SourceRig || !TargetRig) return 0;

    const FReferenceSkeleton& SourceRef = SourceMesh->GetRefSkeleton();
    const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();

    TArray<FTransform> SourcePose;
    BuildComponentPose(SourceRef, SourcePose);

    // 대상 쪽은 오프셋을 넣을 때마다 바뀌므로, 로컬 포즈를 복사해 두고 그 위에 누적한다.
    TArray<FTransform> TargetLocal = TargetRef.GetRefBonePose();
    TArray<FTransform> TargetPose;
    TargetPose.SetNum(TargetLocal.Num());
    auto RefreshTargetPose = [&]()
    {
        for (int32 Index = 0; Index < TargetLocal.Num(); ++Index)
        {
            const int32 Parent = TargetRef.GetParentIndex(Index);
            TargetPose[Index] = Parent == INDEX_NONE ? TargetLocal[Index] : TargetLocal[Index] * TargetPose[Parent];
        }
    };
    RefreshTargetPose();

    TMap<FName, const FBoneChain*> SourceChains;
    for (const FBoneChain& Chain : SourceRig->GetRetargetChains()) SourceChains.Add(Chain.ChainName, &Chain);

    const FName RetargetRootBone = TargetRig->GetRetargetRoot();
    int32 AlignedCount = 0;

    for (const FBoneChain& TargetChain : TargetRig->GetRetargetChains())
    {
        if (!MappedChainNames.Contains(TargetChain.ChainName)) continue;
        // Root 체인은 펠비스 배치용이라 방향을 맞출 대상이 아니다.
        if (TargetChain.ChainName == TEXT("Root")) continue;
        const FBoneChain* const* SourceChainPtr = SourceChains.Find(TargetChain.ChainName);
        if (!SourceChainPtr || !*SourceChainPtr) continue;

        const TArray<FName> TargetBones = CollectChainBones(TargetRef, TargetChain);
        const TArray<FName> SourceBones = CollectChainBones(SourceRef, **SourceChainPtr);
        if (TargetBones.Num() < 2 || SourceBones.Num() < 2) continue;

        const TArray<FChainSegment> SourceSegments = BuildSegments(SourceRef, SourcePose, SourceBones);
        if (SourceSegments.IsEmpty()) continue;

        // 대상 체인은 본을 하나 돌릴 때마다 아래쪽 위치가 바뀌므로, 마디마다 다시 계산한다.
        for (int32 Index = 0; Index + 1 < TargetBones.Num(); ++Index)
        {
            const FName BoneName = TargetBones[Index];
            if (BoneName == RetargetRootBone) continue;

            const TArray<FChainSegment> TargetSegments = BuildSegments(TargetRef, TargetPose, TargetBones);
            if (!TargetSegments.IsValidIndex(Index)) break;

            const FVector Current = TargetSegments[Index].Direction;
            const FVector Desired = DirectionAtMidpoint(SourceSegments, SegmentMidpoint(TargetSegments, Index));
            const FQuat Align = FQuat::FindBetweenNormals(Current, Desired);
            if (Align.GetAngle() < MinAlignRadians) continue;

            const int32 BoneIndex = TargetRef.FindBoneIndex(BoneName);
            if (BoneIndex == INDEX_NONE) continue;
            const int32 ParentIndex = TargetRef.GetParentIndex(BoneIndex);
            const FQuat ParentRotation = ParentIndex == INDEX_NONE ? FQuat::Identity : TargetPose[ParentIndex].GetRotation();
            const FQuat RefLocalRotation = TargetRef.GetRefBonePose()[BoneIndex].GetRotation();

            // 엔진은 리타기팅 포즈 오프셋을 LocalRotation = RefLocal * Delta 로 적용한다.
            // 컴포넌트 공간에서 Align만큼 돌리려면 부모 회전으로 감싸 로컬로 되돌려야 한다.
            const FQuat Delta = RefLocalRotation.Inverse() * ParentRotation.Inverse() * Align * ParentRotation * RefLocalRotation;
            Controller->SetRotationOffsetForRetargetPoseBone(BoneName, Delta.GetNormalized(), ERetargetSourceOrTarget::Target);

            TargetLocal[BoneIndex].SetRotation((RefLocalRotation * Delta).GetNormalized());
            RefreshTargetPose();
            ++AlignedCount;
        }
    }

    UE_LOG(LogUPTRestPose, Log, TEXT("UPT_RESTPOSE '%s' 레스트 포즈 직접 정렬: 본 %d개"), *TargetMesh->GetName(), AlignedCount);
    return AlignedCount;
}
