#include "UPTPelvisMotionBaker.h"

#include "UPTSkeletonAnalyzer.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "ReferenceSkeleton.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTPelvisBake, Log, All);

namespace
{
// 0이면 끄고, 1이면 이동이 비어 있을 때만, 2면 이미 이동이 있어도 덮어쓴다(비교용).
TAutoConsoleVariable<int32> CVarBakePelvisMotion(
    TEXT("UPT.BakePelvisMotion"), 1,
    TEXT("리타기팅 결과의 펠비스 이동이 비어 있으면 직접 구워 넣는다. 0=끔, 1=빈 경우만, 2=항상"));

// 이동이 "없다"고 보는 기준(cm). 압축 오차 정도는 없는 것으로 친다.
constexpr double EmptyMotionThreshold = 0.05;

/** 역할 'Pelvis'에 해당하는 본 이름. 메시마다 명명 규칙이 달라 분석기를 쓴다. */
FName FindPelvisBone(USkeletalMesh* Mesh)
{
    if (!Mesh) return NAME_None;
    const FUPTSkeletonAnalysisData Analysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(Mesh, FString());
    if (const FName* Found = Analysis.BoneMappings.Find(TEXT("Pelvis")))
    {
        return *Found;
    }
    return NAME_None;
}

/**
 * 실제로 '몸 높이'를 드는 본을 고른다.
 *
 * 분석기가 Pelvis로 뽑은 본이 항상 높이를 들고 있지는 않다. Forest Golem은 계층이
 * `Root → CG → Pelvis`이고 **`Pelvis`의 로컬 이동이 (0,0,0)** 이다. 높이는 `CG`가 든다.
 * 실제로 리타기터도 `CG`의 회전을 구동하고 `Pelvis`는 건드리지 않는다.
 * 그런 본에 이동을 써 봐야 몸은 움직이지 않는다.
 *
 * 그래서 레퍼런스 포즈에서 로컬 이동이 거의 0인 동안 부모로 거슬러 올라간다.
 * UE 마네킹(`root → pelvis`, 로컬 z=95.9)이나 Mixamo(`Hips`가 최상위)는 그대로 유지된다.
 */
FName ResolveMotionBone(const FReferenceSkeleton& Ref, const FName PelvisBone)
{
    int32 Index = Ref.FindBoneIndex(PelvisBone);
    for (int32 Guard = 0; Index != INDEX_NONE && Guard < 8; ++Guard)
    {
        const FVector LocalOffset = Ref.GetRefBonePose()[Index].GetTranslation();
        if (LocalOffset.SizeSquared() > 1.0) break;  // 1cm 이상이면 이 본이 높이를 든다
        const int32 Parent = Ref.GetParentIndex(Index);
        if (Parent == INDEX_NONE) break;                       // 스켈레톤 루트까지 왔다
        if (Ref.GetParentIndex(Parent) == INDEX_NONE) break;    // 부모가 루트면 더 올라가지 않는다
        Index = Parent;
    }
    return Index == INDEX_NONE ? PelvisBone : Ref.GetBoneName(Index);
}

/** 본에서 루트까지의 사슬(루트 → … → 본 순서). */
TArray<FName> BuildAncestorChain(const FReferenceSkeleton& Ref, const FName BoneName)
{
    TArray<FName> Chain;
    for (int32 Index = Ref.FindBoneIndex(BoneName); Index != INDEX_NONE; Index = Ref.GetParentIndex(Index))
    {
        Chain.Add(Ref.GetBoneName(Index));
        if (Chain.Num() > 64) break;
    }
    Algo::Reverse(Chain);
    return Chain;
}

/** 애니메이션에서 사슬의 각 본의 로컬 트랜스폼을 키별로 읽는다. 트랙이 없으면 레퍼런스 포즈로 채운다. */
bool ReadChainTracks(const UAnimSequence* Animation, const FReferenceSkeleton& Ref,
                     const TArray<FName>& Chain, int32 NumKeys, TArray<TArray<FTransform>>& OutTracks)
{
    const IAnimationDataModel* Model = Animation ? Animation->GetDataModel() : nullptr;
    if (!Model) return false;

    OutTracks.Reset();
    for (const FName BoneName : Chain)
    {
        TArray<FTransform> Track;
        if (Model->IsValidBoneTrackName(BoneName))
        {
            Model->GetBoneTrackTransforms(BoneName, Track);
        }
        if (Track.Num() != NumKeys)
        {
            // 트랙이 없거나 키 수가 다르면 레퍼런스 포즈를 그대로 쓴다(움직이지 않는 본).
            const int32 BoneIndex = Ref.FindBoneIndex(BoneName);
            if (BoneIndex == INDEX_NONE) return false;
            const FTransform RefLocal = Ref.GetRefBonePose()[BoneIndex];
            TArray<FTransform> Filled;
            Filled.Init(RefLocal, NumKeys);
            for (int32 Key = 0; Key < Track.Num() && Key < NumKeys; ++Key) Filled[Key] = Track[Key];
            Track = MoveTemp(Filled);
        }
        OutTracks.Add(MoveTemp(Track));
    }
    return OutTracks.Num() == Chain.Num();
}

/** 사슬을 합성해 Key 시점의 컴포넌트 공간 트랜스폼을 만든다. Depth개만 합성한다. */
FTransform ComposeChain(const TArray<TArray<FTransform>>& Tracks, int32 Key, int32 Depth)
{
    FTransform Result = FTransform::Identity;
    for (int32 Index = 0; Index < Depth && Index < Tracks.Num(); ++Index)
    {
        Result = Tracks[Index][Key] * Result;
    }
    return Result;
}

/** 레퍼런스 포즈에서의 컴포넌트 공간 위치.
 *
 * UE의 합성 규칙은 `자식 컴포넌트 = 자식 로컬 * 부모 컴포넌트`다. 루트에서 잎 방향으로
 * 누적해야 한다. 거꾸로 곱하면 높이가 0 근처로 나와 비율이 통째로 무너진다(실제로 겪었다).
 */
FVector RefComponentLocation(const FReferenceSkeleton& Ref, const FName BoneName)
{
    TArray<int32> Indices;
    for (int32 Index = Ref.FindBoneIndex(BoneName); Index != INDEX_NONE; Index = Ref.GetParentIndex(Index))
    {
        Indices.Add(Index);
        if (Indices.Num() > 64) break;
    }
    Algo::Reverse(Indices);

    FTransform Result = FTransform::Identity;
    for (const int32 Index : Indices)
    {
        Result = Ref.GetRefBonePose()[Index] * Result;
    }
    return Result.GetLocation();
}

double TrackMotionRange(const TArray<FTransform>& Track)
{
    if (Track.Num() < 2) return 0.0;
    FVector Min = Track[0].GetLocation();
    FVector Max = Min;
    for (const FTransform& Transform : Track)
    {
        const FVector Location = Transform.GetLocation();
        Min = Min.ComponentMin(Location);
        Max = Max.ComponentMax(Location);
    }
    return (Max - Min).GetMax();
}
}

bool FUPTPelvisMotionBaker::BakeIfMissing(
    UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
    UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh)
{
    const int32 Mode = CVarBakePelvisMotion.GetValueOnAnyThread();
    if (Mode == 0 || !SourceAnimation || !SourceMesh || !TargetAnimation || !TargetMesh) return false;

    const FName SourcePelvis = FindPelvisBone(SourceMesh);
    const FName TargetPelvis = FindPelvisBone(TargetMesh);
    if (SourcePelvis == NAME_None || TargetPelvis == NAME_None)
    {
        UE_LOG(LogUPTPelvisBake, Warning, TEXT("UPT_PELVISBAKE 펠비스 역할을 찾지 못해 건너뜁니다: %s -> %s"),
            *SourceMesh->GetName(), *TargetMesh->GetName());
        return false;
    }

    IAnimationDataModel* TargetModel = TargetAnimation->GetDataModel();
    const IAnimationDataModel* SourceModel = SourceAnimation->GetDataModel();
    if (!TargetModel || !SourceModel) return false;

    const int32 NumKeys = TargetModel->GetNumberOfKeys();
    if (NumKeys < 2 || SourceModel->GetNumberOfKeys() < 2) return false;

    const FReferenceSkeleton& SourceRef = SourceMesh->GetRefSkeleton();
    const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();

    // 대상은 '높이를 드는 본'에 써야 몸이 실제로 움직인다.
    const FName TargetMotionBone = ResolveMotionBone(TargetRef, TargetPelvis);
    if (TargetMotionBone != TargetPelvis)
    {
        UE_LOG(LogUPTPelvisBake, Log, TEXT("UPT_PELVISBAKE '%s'의 로컬 이동이 0이라 부모 '%s'에 씁니다."),
            *TargetPelvis.ToString(), *TargetMotionBone.ToString());
    }

    const TArray<FName> SourceChain = BuildAncestorChain(SourceRef, SourcePelvis);
    const TArray<FName> TargetChain = BuildAncestorChain(TargetRef, TargetMotionBone);
    if (SourceChain.IsEmpty() || TargetChain.IsEmpty()) return false;

    TArray<TArray<FTransform>> TargetTracks;
    if (!ReadChainTracks(TargetAnimation, TargetRef, TargetChain, NumKeys, TargetTracks)) return false;

    // 이미 이동이 들어 있으면 엔진 op이 제 일을 한 것이다. 건드리지 않는다.
    const TArray<FTransform>& ExistingPelvisTrack = TargetTracks.Last();
    if (Mode == 1 && TrackMotionRange(ExistingPelvisTrack) > EmptyMotionThreshold) return false;

    const int32 SourceKeys = SourceModel->GetNumberOfKeys();
    TArray<TArray<FTransform>> SourceTracks;
    if (!ReadChainTracks(SourceAnimation, SourceRef, SourceChain, SourceKeys, SourceTracks)) return false;

    const double SourceRefHeight = RefComponentLocation(SourceRef, SourcePelvis).Z;
    const double TargetRefHeight = RefComponentLocation(TargetRef, TargetMotionBone).Z;
    if (FMath::Abs(SourceRefHeight) < KINDA_SMALL_NUMBER)
    {
        UE_LOG(LogUPTPelvisBake, Warning, TEXT("UPT_PELVISBAKE 원본 펠비스가 바닥 높이라 비율을 낼 수 없습니다: %s"), *SourceMesh->GetName());
        return false;
    }
    const double HeightRatio = TargetRefHeight / SourceRefHeight;

    TArray<FVector> PositionKeys;
    TArray<FQuat> RotationKeys;
    TArray<FVector> ScaleKeys;
    PositionKeys.Reserve(NumKeys);
    RotationKeys.Reserve(NumKeys);
    ScaleKeys.Reserve(NumKeys);

    for (int32 Key = 0; Key < NumKeys; ++Key)
    {
        // 키 수가 다르면 비율로 대응시킨다(보통은 같다).
        const int32 SourceKey = SourceKeys == NumKeys
            ? Key
            : FMath::Clamp(FMath::RoundToInt(static_cast<double>(Key) * (SourceKeys - 1) / (NumKeys - 1)), 0, SourceKeys - 1);

        const FVector SourcePosition = ComposeChain(SourceTracks, SourceKey, SourceTracks.Num()).GetLocation();
        const FVector DesiredComponent = SourcePosition * HeightRatio;

        // 부모까지만 합성해 원하는 컴포넌트 위치를 로컬로 되돌린다.
        const FTransform ParentComponent = ComposeChain(TargetTracks, Key, TargetTracks.Num() - 1);
        PositionKeys.Add(ParentComponent.InverseTransformPosition(DesiredComponent));
        RotationKeys.Add(ExistingPelvisTrack[Key].GetRotation());
        ScaleKeys.Add(ExistingPelvisTrack[Key].GetScale3D());
    }

    IAnimationDataController& Controller = TargetAnimation->GetController();
    constexpr bool bShouldTransact = false;
    Controller.OpenBracket(NSLOCTEXT("UPT", "BakePelvisMotion", "Bake pelvis motion"), bShouldTransact);
    Controller.AddBoneCurve(TargetMotionBone, bShouldTransact);
    const bool bWritten = Controller.SetBoneTrackKeys(TargetMotionBone, PositionKeys, RotationKeys, ScaleKeys, bShouldTransact);
    Controller.CloseBracket(bShouldTransact);

    // 원시 키만 바꾸면 압축 데이터는 그대로라, 재생·샘플링에는 예전 값(0)이 계속 나온다.
    // 실제로 raw에는 값이 들어갔는데 get_bone_pose_for_time은 0을 돌려줬다.
    // CacheDerivedDataForCurrentPlatform()을 직접 부르면 재진입 루프에 빠져 메모리가 폭주한다
    // (에디터가 19.7GB까지 올라가 응답이 멎었다). 표준 경로인 PostEditChange로 한 번만 돌린다.
    TargetAnimation->PostEditChange();
    TargetAnimation->MarkPackageDirty();

    UE_LOG(LogUPTPelvisBake, Log, TEXT("UPT_PELVISBAKE %s: 펠비스 '%s' 이동 %d키 기록(높이비 %.3f) %s"),
        *TargetAnimation->GetName(), *TargetMotionBone.ToString(), NumKeys, HeightRatio,
        bWritten ? TEXT("성공") : TEXT("실패"));
    return bWritten;
}
