#include "UPTFraming.h"

#include "CollisionQueryParams.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarUPTBodyCurveFocus(
    TEXT("upt.Framing.BodyCurveFocus"),
    1,
    TEXT("1: aim point XY follows skeletal bones near the focus height (better close-ups when the head is ahead of the torso). 0: mesh bounds center."));

namespace UPTFraming
{
float GetSensorHeightMm(const float AspectRatio)
{
    return SensorWidthMm / FMath::Clamp(AspectRatio, 0.25f, 4.0f);
}

float GetFieldOfViewDegrees(const float SensorSizeMm, const float FocalLengthMm)
{
    return FMath::RadiansToDegrees(2.0f * FMath::Atan(SensorSizeMm / (2.0f * FMath::Max(1.0f, FocalLengthMm))));
}

FBox GetSubjectBounds(const AActor* Actor)
{
    if (!Actor) return FBox(ForceInit);
    // 캐릭터에 붙은 무기·카메라 붐·트리거 볼륨이 Actor 전체 Bounds를 부풀려 조준점이 몸 밖으로 벗어나므로, 몸체(Skeletal Mesh) Bounds를 우선 사용한다.
    if (const USkeletalMeshComponent* Mesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
    {
        const FBox MeshBounds = Mesh->Bounds.GetBox();
        if (MeshBounds.IsValid && MeshBounds.GetSize().Z > KINDA_SMALL_NUMBER) return MeshBounds;
    }
    return Actor->GetComponentsBoundingBox(true);
}

FVector GetFocusPoint(const AActor* Actor, const float HeightRatio)
{
    if (!Actor) return FVector::ZeroVector;
    const FBox Bounds = GetSubjectBounds(Actor);
    if (!Bounds.IsValid) return Actor->GetActorLocation();
    const FVector Center = Bounds.GetCenter();
    FVector Focus(Center.X, Center.Y, FMath::Lerp(Bounds.Min.Z, Bounds.Max.Z, FMath::Clamp(HeightRatio, 0.0f, 1.0f)));

    // 메시 Bounds 중심은 몸통 기둥이라 머리가 앞으로 나온 캐릭터의 클로즈업에서 조준점이 어긋난다.
    // 조준 높이 부근(키의 ±6%) 본들의 가로 위치 중앙값을 쓰고, 팔처럼 몸통에서 멀리 벌어진 본은 제외한다.
    const USkeletalMeshComponent* Mesh = CVarUPTBodyCurveFocus.GetValueOnGameThread() != 0 ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
    if (Mesh)
    {
        const float BodyHeight = Bounds.GetSize().Z;
        const float Band = FMath::Max(6.0f, BodyHeight * 0.06f);
        const float LateralLimit = BodyHeight * 0.15f;
        TArray<double> Xs;
        TArray<double> Ys;
        for (int32 BoneIndex = 0; BoneIndex < Mesh->GetNumBones(); ++BoneIndex)
        {
            const FVector BoneLocation = Mesh->GetBoneLocation(Mesh->GetBoneName(BoneIndex));
            if (FMath::Abs(BoneLocation.Z - Focus.Z) > Band || FVector::Dist2D(BoneLocation, Center) > LateralLimit) continue;
            Xs.Add(BoneLocation.X);
            Ys.Add(BoneLocation.Y);
        }
        if (Xs.Num() >= 2)
        {
            Xs.Sort();
            Ys.Sort();
            Focus.X = Xs[Xs.Num() / 2];
            Focus.Y = Ys[Ys.Num() / 2];
        }
    }
    return Focus;
}

void GetSubjectBasis(const AActor* Actor, FVector& OutForward, FVector& OutRight)
{
    OutForward = Actor ? Actor->GetActorForwardVector() : FVector::ForwardVector;
    // Skeletal Mesh 에셋은 +Y가 정면이다. Character는 Mesh를 -90도 돌려 두므로 두 경우 모두 Mesh +Y가 실제 정면이 된다.
    if (const USkeletalMeshComponent* Mesh = Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr)
    {
        OutForward = Mesh->GetRightVector();
    }
    OutForward.Z = 0.0f;
    if (!OutForward.Normalize()) OutForward = FVector::ForwardVector;
    OutRight = FVector::CrossProduct(FVector::UpVector, OutForward);
}

FVector ComputeDesiredCameraLocation(const AActor* Subject, const float HeightRatio, const float DistanceCm, const float SideCm, const float HeightCm)
{
    FVector Forward, Right;
    GetSubjectBasis(Subject, Forward, Right);
    return GetFocusPoint(Subject, HeightRatio) + Forward * DistanceCm + Right * SideCm + FVector::UpVector * HeightCm;
}

bool ProjectToScreen(const FVector& CameraLocation, const FRotator& CameraRotation, const FVector& WorldPoint,
    const float FocalLength, const float AspectRatio, FVector2D& OutScreen)
{
    const FVector Local = CameraRotation.UnrotateVector(WorldPoint - CameraLocation);
    if (Local.X <= KINDA_SMALL_NUMBER) return false;
    const float TanHalfHorizontal = FMath::Tan(FMath::DegreesToRadians(GetFieldOfViewDegrees(SensorWidthMm, FocalLength)) * 0.5f);
    const float TanHalfVertical = FMath::Tan(FMath::DegreesToRadians(GetFieldOfViewDegrees(GetSensorHeightMm(AspectRatio), FocalLength)) * 0.5f);
    OutScreen.X = 0.5f + 0.5f * (Local.Y / Local.X) / TanHalfHorizontal;
    OutScreen.Y = 0.5f - 0.5f * (Local.Z / Local.X) / TanHalfVertical;
    return true;
}

FRotator ComputeFramingRotation(const FVector& CameraLocation, const FVector& TargetPoint, const float ScreenX, const float ScreenY,
    const float FocalLength, const float AspectRatio, const float RollDegrees)
{
    FRotator Rotation = (TargetPoint - CameraLocation).Rotation();
    const float TanHalfHorizontal = FMath::Tan(FMath::DegreesToRadians(GetFieldOfViewDegrees(SensorWidthMm, FocalLength)) * 0.5f);
    const float TanHalfVertical = FMath::Tan(FMath::DegreesToRadians(GetFieldOfViewDegrees(GetSensorHeightMm(AspectRatio), FocalLength)) * 0.5f);
    const FVector2D Desired(FMath::Clamp(ScreenX, 0.02f, 0.98f), FMath::Clamp(ScreenY, 0.02f, 0.98f));

    // Yaw/Pitch 오프셋은 피치가 클수록 근사 오차가 생기므로 실제 투영 결과로 몇 번 보정한다.
    for (int32 Iteration = 0; Iteration < 6; ++Iteration)
    {
        FVector2D Actual;
        if (!ProjectToScreen(CameraLocation, Rotation, TargetPoint, FocalLength, AspectRatio, Actual)) break;
        const float NdcErrorX = (Desired.X - Actual.X) * 2.0f;
        const float NdcErrorUp = (Actual.Y - Desired.Y) * 2.0f;
        if (FMath::Abs(NdcErrorX) < 0.001f && FMath::Abs(NdcErrorUp) < 0.001f) break;
        Rotation.Yaw -= FMath::RadiansToDegrees(FMath::Atan(NdcErrorX * TanHalfHorizontal));
        Rotation.Pitch -= FMath::RadiansToDegrees(FMath::Atan(NdcErrorUp * TanHalfVertical));
    }
    Rotation.Roll += RollDegrees;
    return Rotation;
}

void DecomposeCameraOffset(const AActor* Subject, const float HeightRatio, const FVector& CameraLocation,
    float& OutDistanceCm, float& OutSideCm, float& OutHeightCm)
{
    FVector Forward, Right;
    GetSubjectBasis(Subject, Forward, Right);
    const FVector Delta = CameraLocation - GetFocusPoint(Subject, HeightRatio);
    OutDistanceCm = FVector::DotProduct(Delta, Forward);
    OutSideCm = FVector::DotProduct(Delta, Right);
    OutHeightCm = Delta.Z;
}

void ResolveShotCamera(UWorld* World, const FUPTCinematicShot& Shot, const float AspectRatio,
    const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors, FVector& OutLocation, FRotator& OutRotation)
{
    const TWeakObjectPtr<AActor>* SubjectPtr = SceneActors.Find(Shot.Subject);
    AActor* Subject = SubjectPtr ? SubjectPtr->Get() : nullptr;
    if (!Subject)
    {
        OutLocation = Shot.CameraLocation;
        OutRotation = Shot.CameraRotation;
        return;
    }

    const TWeakObjectPtr<AActor>* LookAtPtr = SceneActors.Find(Shot.LookAt);
    AActor* LookAt = LookAtPtr && LookAtPtr->IsValid() ? LookAtPtr->Get() : Subject;
    const FVector TargetPoint = GetFocusPoint(LookAt, Shot.FocusHeightRatio);
    const FVector DesiredLocation = ComputeDesiredCameraLocation(Subject, Shot.FocusHeightRatio, Shot.DistanceCm, Shot.SideCm, Shot.HeightCm);
    OutLocation = DesiredLocation;

    if (World)
    {
        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UPTCameraPlacement), false);
        for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
        {
            if (Pair.Value.IsValid()) QueryParams.AddIgnoredActor(Pair.Value.Get());
        }
        FHitResult Hit;
        if (World->SweepSingleByChannel(Hit, TargetPoint, DesiredLocation, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(20.0f), QueryParams))
        {
            OutLocation = Hit.Location + (TargetPoint - Hit.Location).GetSafeNormal() * 30.0f;
        }
    }
    OutRotation = ComputeFramingRotation(OutLocation, TargetPoint, Shot.SubjectScreenX, Shot.SubjectScreenY, Shot.FocalLength, AspectRatio, Shot.CameraRollDegrees);
}
}
