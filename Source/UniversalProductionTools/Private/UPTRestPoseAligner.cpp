#include "UPTRestPoseAligner.h"

#include "UPTSkeletonAnalyzer.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "RetargetEditor/IKRetargeterController.h"

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

/** 몸의 축. 한 축은 역할을 뿌리에서 끝까지 이은 한 줄기다. */
const TArray<TArray<FString>>& GetRoleAxes()
{
    static const TArray<TArray<FString>> Axes = {
        {TEXT("Pelvis"), TEXT("Spine"), TEXT("Neck"), TEXT("Head")},
        {TEXT("LeftClavicle"), TEXT("LeftUpperArm"), TEXT("LeftLowerArm"), TEXT("LeftHand")},
        {TEXT("RightClavicle"), TEXT("RightUpperArm"), TEXT("RightLowerArm"), TEXT("RightHand")},
        {TEXT("LeftThigh"), TEXT("LeftCalf"), TEXT("LeftFoot")},
        {TEXT("RightThigh"), TEXT("RightCalf"), TEXT("RightFoot")},
    };
    return Axes;
}

/** 정렬할 마디 목록(부모 역할 → 자식 역할). 축을 따라 이웃끼리만 잇는다.
 *
 * 갈라지는 마디(`Pelvis → Thigh`, `Spine → Clavicle`)는 **넣지 않는다.** 처음에는 넣고
 * 자식이 여럿인 본의 회전을 평균 냈는데, 펠비스는 척추와 양 허벅지 셋을 한 회전으로 떠안아
 * `pelvis→spine`이 중앙 3.4°로 남았다(다른 축의 마디는 전부 0.1° 이하였다).
 *
 * 갈라지는 방향은 **회전으로 고칠 수 있는 성질이 아니다.** 허벅지가 펠비스에서 어느 쪽으로
 * 뻗어 있는지는 골반 폭, 즉 **위치**의 문제다. 펠비스를 돌려 그 방향을 맞춰도 다리 자체의
 * 방향은 어차피 허벅지 본을 돌려 따로 맞춘다. 그래서 갈라지는 마디를 정렬에 넣으면
 * 몸통 방향만 희생되고 얻는 것이 없다. 각 본은 **자기가 속한 축의 다음 역할**만 본다.
 */
const TArray<TPair<FString, FString>>& GetRoleSegments()
{
    static const TArray<TPair<FString, FString>> Segments = []
    {
        TArray<TPair<FString, FString>> Built;
        for (const TArray<FString>& Axis : GetRoleAxes())
        {
            for (int32 Index = 0; Index + 1 < Axis.Num(); ++Index)
            {
                Built.Add({Axis[Index], Axis[Index + 1]});
            }
        }
        return Built;
    }();
    return Segments;
}

/** 역할이 없을 때 한 칸 건너뛸 대체 역할. 쇄골 없는 리그, 목 없는 리그가 실제로 있다. */
const TMap<FString, FString>& GetRoleFallbacks()
{
    static const TMap<FString, FString> Fallbacks = {
        {TEXT("LeftClavicle"), TEXT("LeftUpperArm")},
        {TEXT("RightClavicle"), TEXT("RightUpperArm")},
        {TEXT("Neck"), TEXT("Head")},
    };
    return Fallbacks;
}

/** 역할 → 본 이름. 이름 근거 없이 배정된 역할(ForcedRoles)은 쓰지 않는다.
 *
 * 부분 메시(망토·머리 부착물)에서는 남은 아무 본이나 집어 오므로(LeftFoot ← jaw)
 * 그 방향에 맞추면 몸이 통째로 틀어진다. 역할이 비면 대체 역할로 한 번 넘어간다.
 */
FName ResolveRole(const FUPTSkeletonAnalysisData& Analysis, const FString& Role)
{
    auto Pick = [&Analysis](const FString& Name) -> FName
    {
        if (Analysis.ForcedRoles.Contains(Name)) return NAME_None;
        const FName* Found = Analysis.BoneMappings.Find(Name);
        return Found ? *Found : NAME_None;
    };
    const FName Direct = Pick(Role);
    if (Direct != NAME_None) return Direct;
    if (const FString* Alternate = GetRoleFallbacks().Find(Role)) return Pick(*Alternate);
    return NAME_None;
}

/** 로컬 포즈를 컴포넌트 공간으로 합성한다. */
void BuildComponentPose(const FReferenceSkeleton& Ref, const TArray<FTransform>& Local, TArray<FTransform>& OutPose)
{
    OutPose.SetNum(Local.Num());
    for (int32 Index = 0; Index < Local.Num(); ++Index)
    {
        const int32 Parent = Ref.GetParentIndex(Index);
        OutPose[Index] = Parent == INDEX_NONE ? Local[Index] : Local[Index] * OutPose[Parent];
    }
}

int32 BoneDepth(const FReferenceSkeleton& Ref, int32 Index)
{
    int32 Depth = 0;
    for (int32 Walk = Ref.GetParentIndex(Index); Walk != INDEX_NONE; Walk = Ref.GetParentIndex(Walk)) ++Depth;
    return Depth;
}

/** 한 본이 맞춰야 할 마디들. 대상 자식 본과 그에 대응하는 원본 자식 본의 짝이다. */
struct FAlignTarget
{
    FName SourceBone = NAME_None;             // 이 본에 대응하는 원본 본
    TArray<TPair<FName, FName>> Children;     // (대상 자식, 원본 자식)
};
}

int32 FUPTRestPoseAligner::AlignTargetToSource(
    UIKRetargeterController* Controller,
    USkeletalMesh* SourceMesh,
    USkeletalMesh* TargetMesh)
{
    if (!Controller || !SourceMesh || !TargetMesh) return 0;
    if (CVarRestPoseAlign.GetValueOnAnyThread() == 0)
    {
        UE_LOG(LogUPTRestPose, Log, TEXT("UPT_RESTPOSE UPT.RestPoseAlign=0 이라 정렬을 건너뜁니다."));
        return 0;
    }

    const FUPTSkeletonAnalysisData SourceAnalysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, FString());
    const FUPTSkeletonAnalysisData TargetAnalysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(TargetMesh, FString());
    if (SourceAnalysis.BoneMappings.IsEmpty() || TargetAnalysis.BoneMappings.IsEmpty()) return 0;

    const FReferenceSkeleton& SourceRef = SourceMesh->GetRefSkeleton();
    const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();

    TArray<FTransform> SourcePose;
    BuildComponentPose(SourceRef, SourceRef.GetRefBonePose(), SourcePose);

    // 대상 쪽은 오프셋을 넣을 때마다 바뀌므로, 로컬 포즈를 복사해 두고 그 위에 누적한다.
    TArray<FTransform> TargetLocal = TargetRef.GetRefBonePose();
    TArray<FTransform> TargetPose;
    BuildComponentPose(TargetRef, TargetLocal, TargetPose);

    // 축만 따라가면 한 본에 마디는 하나씩 붙는다. 그래도 본 단위로 모아 두는 것은,
    // 역할 목록을 늘리다가 한 본에 둘이 걸리면 조용히 마지막 것만 적용되는 일을 막기 위해서다.
    TMap<FName, FAlignTarget> AlignTargets;
    int32 SegmentCount = 0;
    for (const TPair<FString, FString>& Segment : GetRoleSegments())
    {
        const FName TargetParent = ResolveRole(TargetAnalysis, Segment.Key);
        const FName TargetChild = ResolveRole(TargetAnalysis, Segment.Value);
        const FName SourceParent = ResolveRole(SourceAnalysis, Segment.Key);
        const FName SourceChild = ResolveRole(SourceAnalysis, Segment.Value);
        if (TargetParent == NAME_None || TargetChild == NAME_None) continue;
        if (SourceParent == NAME_None || SourceChild == NAME_None) continue;
        // 대체 역할로 넘어가다 보면 양끝이 같은 본이 될 수 있다(쇄골 없는 리그의 Clavicle→UpperArm).
        if (TargetParent == TargetChild || SourceParent == SourceChild) continue;

        FAlignTarget& Entry = AlignTargets.FindOrAdd(TargetParent);
        Entry.SourceBone = SourceParent;
        Entry.Children.Add({TargetChild, SourceChild});
        ++SegmentCount;
    }

    // 부모를 먼저 맞춰야 자식의 현재 방향이 제대로 나온다. 계층이 얕은 본부터 처리한다.
    TArray<FName> OrderedBones;
    AlignTargets.GetKeys(OrderedBones);
    OrderedBones.Sort([&TargetRef](const FName A, const FName B)
    {
        return BoneDepth(TargetRef, TargetRef.FindBoneIndex(A)) < BoneDepth(TargetRef, TargetRef.FindBoneIndex(B));
    });

    int32 AlignedCount = 0;
    for (const FName BoneName : OrderedBones)
    {
        const int32 BoneIndex = TargetRef.FindBoneIndex(BoneName);
        const FAlignTarget& Entry = AlignTargets[BoneName];
        const int32 SourceIndex = SourceRef.FindBoneIndex(Entry.SourceBone);
        if (BoneIndex == INDEX_NONE || SourceIndex == INDEX_NONE) continue;

        // 자식마다 필요한 회전을 구해 평균 낸다(지금 구성에서는 자식이 하나라 그대로 쓰인다).
        FQuat Align = FQuat::Identity;
        int32 Contributions = 0;
        for (const TPair<FName, FName>& Child : Entry.Children)
        {
            const int32 TargetChildIndex = TargetRef.FindBoneIndex(Child.Key);
            const int32 SourceChildIndex = SourceRef.FindBoneIndex(Child.Value);
            if (TargetChildIndex == INDEX_NONE || SourceChildIndex == INDEX_NONE) continue;

            const FVector Current = (TargetPose[TargetChildIndex].GetLocation() - TargetPose[BoneIndex].GetLocation()).GetSafeNormal();
            const FVector Desired = (SourcePose[SourceChildIndex].GetLocation() - SourcePose[SourceIndex].GetLocation()).GetSafeNormal();
            if (Current.IsNearlyZero() || Desired.IsNearlyZero()) continue;

            const FQuat Rotation = FQuat::FindBetweenNormals(Current, Desired);
            ++Contributions;
            Align = Contributions == 1 ? Rotation : FQuat::Slerp(Align, Rotation, 1.0f / Contributions);
        }
        if (Contributions == 0) continue;
        Align.Normalize();
        if (Align.GetAngle() < MinAlignRadians) continue;

        const int32 ParentIndex = TargetRef.GetParentIndex(BoneIndex);
        const FQuat ParentRotation = ParentIndex == INDEX_NONE ? FQuat::Identity : TargetPose[ParentIndex].GetRotation();
        const FQuat RefLocalRotation = TargetRef.GetRefBonePose()[BoneIndex].GetRotation();

        // 엔진은 리타기팅 포즈 오프셋을 LocalRotation = RefLocal * Delta 로 적용한다.
        // 컴포넌트 공간에서 Align만큼 돌리려면 부모 회전으로 감싸 로컬로 되돌려야 한다.
        const FQuat Delta = RefLocalRotation.Inverse() * ParentRotation.Inverse() * Align * ParentRotation * RefLocalRotation;
        Controller->SetRotationOffsetForRetargetPoseBone(BoneName, Delta.GetNormalized(), ERetargetSourceOrTarget::Target);

        TargetLocal[BoneIndex].SetRotation((RefLocalRotation * Delta).GetNormalized());
        BuildComponentPose(TargetRef, TargetLocal, TargetPose);
        ++AlignedCount;
    }

    UE_LOG(LogUPTRestPose, Log, TEXT("UPT_RESTPOSE '%s' 레스트 포즈 정렬(역할 그래프): 마디 %d개 / 본 %d개"),
        *TargetMesh->GetName(), SegmentCount, AlignedCount);
    return AlignedCount;
}
