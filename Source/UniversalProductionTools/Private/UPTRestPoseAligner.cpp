#include "UPTRestPoseAligner.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTRestPose, Log, All);

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

/** 길이 비율 Param에 해당하는 마디의 방향. 본 개수가 달라도 같은 자리끼리 맞추기 위한 것. */
FVector DirectionAtParam(const TArray<FChainSegment>& Segments, double Param)
{
    int32 Best = 0;
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        if (Segments[Index].Param <= Param + KINDA_SMALL_NUMBER) Best = Index;
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
            const FVector Desired = DirectionAtParam(SourceSegments, TargetSegments[Index].Param);
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
