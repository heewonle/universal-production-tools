#include "UPTTwistMotionBaker.h"

#include "UPTSkeletonAnalyzer.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "ReferenceSkeleton.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTTwistBake, Log, All);

// 0이면 끔, 1이면 켬. 기본은 켜 둔다 — 측정으로 "이 파이프라인은 트위스트를 애니메이션에 넣는다"는
// 것을 확인했고(헤더 주석의 수치), 트위스트 본이 없는 대상에서는 아무 일도 하지 않는다.
static TAutoConsoleVariable<int32> CVarBakeTwistMotion(
    TEXT("UPT.BakeTwistMotion"), 1,
    TEXT("1이면 리타기팅 결과에 원본의 트위스트 회전을 옮겨 넣는다."));

namespace
{
const TCHAR* TwistTokens[] = {TEXT("twist"), TEXT("roll"), TEXT("torsion")};

bool IsTwistBone(const FName BoneName)
{
    const FString Lower = BoneName.ToString().ToLower();
    for (const TCHAR* Token : TwistTokens)
    {
        if (Lower.Contains(Token)) return true;
    }
    return false;
}

/** 트위스트 본 → 매달린 역할. 가장 가까운 조상 역할을 키로 쓴다. */
TMap<FString, FName> TwistByRole(const FReferenceSkeleton& Ref, const TMap<FString, FName>& Roles)
{
    TMap<FName, FString> BoneToRole;
    for (const TPair<FString, FName>& Pair : Roles) BoneToRole.Add(Pair.Value, Pair.Key);

    // 역할 아래에 트위스트 본이 여럿이면(twist_01, twist_02) **역할 본에 가장 가까운** 것을 쓴다.
    // 본 배열 순서로 고르면 안 된다 — Manny는 아래팔·정강이에서 배열 순서가 계층 순서와 달라
    // `_02`가 먼저 나온다. 측정기는 계층 거리로 고르는데 베이커가 배열 순서로 고르면
    // **쓴 본과 읽는 본이 달라져** 전달이 실패한 것처럼 보인다(여덟 중 넷이 그랬다).
    TMap<FString, TPair<int32, FName>> Nearest;
    for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
    {
        const FName BoneName = Ref.GetBoneName(Index);
        if (!IsTwistBone(BoneName)) continue;
        int32 Walk = Ref.GetParentIndex(Index);
        for (int32 Distance = 0; Walk != INDEX_NONE && Distance < 16; Walk = Ref.GetParentIndex(Walk), ++Distance)
        {
            const FString* Role = BoneToRole.Find(Ref.GetBoneName(Walk));
            if (!Role) continue;
            const TPair<int32, FName>* Existing = Nearest.Find(*Role);
            // 거리가 같으면 **이름이 작은 쪽**을 쓴다. Manny의 아래팔·정강이는 twist_01과
            // twist_02가 **둘 다 역할 본의 직접 자식**이라 거리가 동점이고, 동점 처리가
            // 측정기(이름 순)와 달라지면 또 쓴 본과 읽는 본이 어긋난다. 실제로 그래서
            // 여덟 중 넷만 전달됐다. 양쪽이 같은 규칙을 쓰는 것이 핵심이다.
            const bool bBetter = !Existing || Distance < Existing->Key
                || (Distance == Existing->Key && BoneName.ToString() < Existing->Value.ToString());
            if (bBetter) Nearest.Add(*Role, {Distance, BoneName});
            break;
        }
    }
    TMap<FString, FName> Out;
    for (const TPair<FString, TPair<int32, FName>>& Pair : Nearest) Out.Add(Pair.Key, Pair.Value.Value);
    return Out;
}

FQuat RefLocalRotation(const FReferenceSkeleton& Ref, const FName BoneName)
{
    const int32 Index = Ref.FindBoneIndex(BoneName);
    return Index == INDEX_NONE ? FQuat::Identity : Ref.GetRefBonePose()[Index].GetRotation();
}
}

int32 FUPTTwistMotionBaker::Bake(
    UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
    UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh)
{
    if (CVarBakeTwistMotion.GetValueOnAnyThread() == 0) return 0;
    if (!SourceAnimation || !SourceMesh || !TargetAnimation || !TargetMesh) return 0;

    const FReferenceSkeleton& SourceRef = SourceMesh->GetRefSkeleton();
    const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();
    const TMap<FString, FName> SourceTwists = TwistByRole(SourceRef,
        FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, FString()).BoneMappings);
    const TMap<FString, FName> TargetTwists = TwistByRole(TargetRef,
        FUPTSkeletonAnalyzer::AnalyzeDetailed(TargetMesh, FString()).BoneMappings);
    // 대상에 트위스트 본이 없으면(X_Bot·골렘·늑대) 할 일이 없다.
    if (SourceTwists.IsEmpty() || TargetTwists.IsEmpty()) return 0;

    const IAnimationDataModel* SourceModel = SourceAnimation->GetDataModel();
    IAnimationDataModel* TargetModel = TargetAnimation->GetDataModel();
    if (!SourceModel || !TargetModel) return 0;
    const int32 NumKeys = TargetModel->GetNumberOfKeys();
    const int32 SourceKeys = SourceModel->GetNumberOfKeys();
    if (NumKeys < 2 || SourceKeys < 2) return 0;

    IAnimationDataController& Controller = TargetAnimation->GetController();
    constexpr bool bShouldTransact = false;
    Controller.OpenBracket(NSLOCTEXT("UPT", "BakeTwistMotion", "Bake twist motion"), bShouldTransact);

    int32 Written = 0;
    for (const TPair<FString, FName>& Pair : TargetTwists)
    {
        const FName* SourceBone = SourceTwists.Find(Pair.Key);
        if (!SourceBone) continue;
        if (!SourceModel->IsValidBoneTrackName(*SourceBone)) continue;

        TArray<FTransform> SourceTrack;
        SourceModel->GetBoneTrackTransforms(*SourceBone, SourceTrack);
        if (SourceTrack.Num() < 2) continue;

        const FName TargetBone = Pair.Value;
        TArray<FTransform> TargetTrack;
        if (TargetModel->IsValidBoneTrackName(TargetBone))
        {
            TargetModel->GetBoneTrackTransforms(TargetBone, TargetTrack);
        }
        const FTransform TargetRefLocal = TargetRef.GetRefBonePose()[TargetRef.FindBoneIndex(TargetBone)];
        if (TargetTrack.Num() != NumKeys)
        {
            TArray<FTransform> Filled;
            Filled.Init(TargetRefLocal, NumKeys);
            for (int32 Key = 0; Key < TargetTrack.Num() && Key < NumKeys; ++Key) Filled[Key] = TargetTrack[Key];
            TargetTrack = MoveTemp(Filled);
        }

        const FQuat SourceRefInverse = RefLocalRotation(SourceRef, *SourceBone).Inverse();
        const FQuat TargetRefRotation = TargetRefLocal.GetRotation();

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
            // 레퍼런스 포즈 기준 델타를 그대로 옮긴다. 레스트 자세가 달라도 델타는 비교 가능하다.
            const FQuat Delta = SourceRefInverse * SourceTrack[FMath::Min(SourceKey, SourceTrack.Num() - 1)].GetRotation();
            RotationKeys.Add((TargetRefRotation * Delta).GetNormalized());
            // 트위스트는 회전만 건드린다. 이동·크기는 결과 그대로 둔다.
            PositionKeys.Add(TargetTrack[Key].GetTranslation());
            ScaleKeys.Add(TargetTrack[Key].GetScale3D());
        }

        Controller.AddBoneCurve(TargetBone, bShouldTransact);
        if (Controller.SetBoneTrackKeys(TargetBone, PositionKeys, RotationKeys, ScaleKeys, bShouldTransact)) ++Written;
    }
    Controller.CloseBracket(bShouldTransact);
    if (Written == 0) return 0;

    // 원시 키만 바꾸면 압축 데이터는 그대로라 재생·샘플링에 예전 값이 계속 나온다.
    // CacheDerivedDataForCurrentPlatform()은 재진입 루프에 빠지므로 표준 경로를 쓴다
    // (펠비스 베이커에서 에디터가 19.7GB까지 올라간 전례가 있다).
    TargetAnimation->PostEditChange();
    TargetAnimation->MarkPackageDirty();

    // 쓴 것이 실제로 들어갔는지 되읽어 확인한다. 펠비스 베이커에서 "원시 키는 바뀌었는데
    // 압축 데이터는 예전 값"이라 샘플링이 0을 돌려주는 일을 겪었다. 의도한 범위와 되읽은
    // 범위를 함께 남겨, 쓰기가 실패한 것인지 읽는 쪽이 다른 것을 보는 것인지 가린다.
    for (const TPair<FString, FName>& Pair : TargetTwists)
    {
        if (!SourceTwists.Contains(Pair.Key)) continue;
        TArray<FTransform> Readback;
        if (!TargetAnimation->GetDataModel()->IsValidBoneTrackName(Pair.Value)) continue;
        TargetAnimation->GetDataModel()->GetBoneTrackTransforms(Pair.Value, Readback);
        if (Readback.Num() < 2) continue;
        double Worst = 0.0;
        for (int32 Key = 1; Key < Readback.Num(); ++Key)
        {
            const double Dot = FMath::Abs(Readback[0].GetRotation() | Readback[Key].GetRotation());
            Worst = FMath::Max(Worst, FMath::RadiansToDegrees(2.0 * FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0))));
        }
        UE_LOG(LogUPTTwistBake, Log, TEXT("UPT_TWISTBAKE_CHECK %s / %s (%s): 되읽은 회전 범위 %.1f도"),
            *TargetAnimation->GetName(), *Pair.Key, *Pair.Value.ToString(), Worst);
    }

    UE_LOG(LogUPTTwistBake, Log, TEXT("UPT_TWISTBAKE %s: 트위스트 %d개에 회전 %d키 기록"),
        *TargetAnimation->GetName(), Written, NumKeys);
    return Written;
}
