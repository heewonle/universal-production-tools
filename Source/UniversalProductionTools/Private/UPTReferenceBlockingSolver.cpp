#include "UPTReferenceBlockingSolver.h"

#include "UPTFraming.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

namespace
{
float FocalLengthForShotSize(const FString& ShotSize)
{
    if (ShotSize == TEXT("extreme_close_up")) return 85.0f;
    if (ShotSize == TEXT("close_up")) return 65.0f;
    if (ShotSize == TEXT("medium")) return 50.0f;
    if (ShotSize == TEXT("full")) return 40.0f;
    return 28.0f;
}

// 샷 크기별로 화면 세로에 담기는 신체 높이 비율
float VisibleBodyRatioForShotSize(const FString& ShotSize)
{
    if (ShotSize == TEXT("extreme_close_up")) return 0.12f;
    if (ShotSize == TEXT("close_up")) return 0.22f;
    if (ShotSize == TEXT("medium")) return 0.5f;
    return 1.0f;
}

// 샷 크기별 조준 높이 (0=발, 1=머리 끝)
float FocusHeightRatioForShotSize(const FString& ShotSize)
{
    if (ShotSize == TEXT("extreme_close_up")) return 0.92f;
    if (ShotSize == TEXT("close_up")) return 0.88f;
    if (ShotSize == TEXT("medium")) return 0.72f;
    return 0.5f;
}

FVector SnapPlacementToGround(AActor* Actor, const FVector& DesiredLocation, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors)
{
    if (!Actor || !Actor->GetWorld()) return DesiredLocation;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(UPTActorGroundSnap), false);
    for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
    {
        if (Pair.Value.IsValid()) Params.AddIgnoredActor(Pair.Value.Get());
    }
    FHitResult Hit;
    const FVector Start = DesiredLocation + FVector::UpVector * 1000.0f;
    const FVector End = DesiredLocation - FVector::UpVector * 5000.0f;
    if (!Actor->GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params)) return DesiredLocation;
    const FBox Bounds = Actor->GetComponentsBoundingBox(true);
    const float PivotToFeet = Bounds.IsValid ? FMath::Max(0.0f, Actor->GetActorLocation().Z - Bounds.Min.Z) : 0.0f;
    FVector Grounded = DesiredLocation;
    Grounded.Z = Hit.Location.Z + PivotToFeet;
    return Grounded;
}

// 선분 P0→P1이 발 중심 Base, 키 BodyHeight로 서 있는 인물(몸통 굵기의 세로 기둥)을 지나가는지 검사한다. 기둥 반지름은 키의 14%(약 25cm)다.
bool SegmentHitsCapsule(const FVector& P0, const FVector& P1, const FVector& Base, const float BodyHeight)
{
    const float Radius = FMath::Max(15.0f, BodyHeight * 0.14f);
    const FVector2D Axis(Base.X, Base.Y);
    const FVector2D A(P0.X, P0.Y);
    const FVector2D AB = FVector2D(P1.X, P1.Y) - A;
    const double LengthSq = AB.SizeSquared();
    const double T = LengthSq > KINDA_SMALL_NUMBER ? FMath::Clamp(FVector2D::DotProduct(Axis - A, AB) / LengthSq, 0.0, 1.0) : 0.0;
    if (FVector2D::Distance(A + AB * T, Axis) > Radius) return false;
    const double Z = FMath::Lerp(P0.Z, P1.Z, T);
    return Z >= Base.Z && Z <= Base.Z + BodyHeight;
}

bool SegmentHitsBody(const FVector& P0, const FVector& P1, const AActor* Body)
{
    const FBox Bounds = UPTFraming::GetSubjectBounds(Body);
    if (!Bounds.IsValid) return false;
    const FVector Center = Bounds.GetCenter();
    return SegmentHitsCapsule(P0, P1, FVector(Center.X, Center.Y, Bounds.Min.Z), static_cast<float>(Bounds.GetSize().Z));
}

struct FStagedSubject
{
    AActor* Actor = nullptr;
    float ScreenX = 0.5f;
    float ScreenHeightRatio = 1.0f; // 레퍼런스에서 이 인물의 화면 크기 / 기준 인물의 화면 크기
};

// 등장인물이 서 있는 자리는 그대로 두고, 기준 인물 주위로 카메라 방위각을 돌려 가며 가장 알맞은 각도를 고른다.
// 비용: 다른 인물의 화면 가로 위치·크기 비율 차이 + 누군가에게 가려짐(큰 벌점) + 원래 각도에서 벗어난 정도.
void SolveStagedCameraAzimuth(FUPTCinematicShot& Shot, AActor* Anchor, const float AspectRatio, const TArray<FStagedSubject>& Others,
    const TArray<AActor*>& Occluders, TArray<FString>& OutNotes)
{
    const float Radius = FMath::Max(30.0f, FMath::Sqrt(FMath::Square(Shot.DistanceCm) + FMath::Square(Shot.SideCm)));
    const float BaseAngle = FMath::RadiansToDegrees(FMath::Atan2(Shot.SideCm, Shot.DistanceCm));
    const FVector AnchorFocus = UPTFraming::GetFocusPoint(Anchor, Shot.FocusHeightRatio);
    const FBox AnchorBounds = UPTFraming::GetSubjectBounds(Anchor);
    const float AnchorHeight = AnchorBounds.IsValid ? static_cast<float>(AnchorBounds.GetSize().Z) : 180.0f;
    const TArray<FVector> AnchorPoints = { AnchorFocus, UPTFraming::GetFocusPoint(Anchor, 0.9f), UPTFraming::GetFocusPoint(Anchor, 0.65f) };

    double BestCost = TNumericLimits<double>::Max();
    float BestAngle = BaseAngle;
    bool bBestOccluded = false;
    for (float Angle = -75.0f; Angle <= 75.0f + KINDA_SMALL_NUMBER; Angle += 2.5f)
    {
        const float Radians = FMath::DegreesToRadians(Angle);
        const FVector Camera = UPTFraming::ComputeDesiredCameraLocation(Anchor, Shot.FocusHeightRatio, Radius * FMath::Cos(Radians), Radius * FMath::Sin(Radians), Shot.HeightCm);
        const FRotator Rotation = UPTFraming::ComputeFramingRotation(Camera, AnchorFocus, Shot.SubjectScreenX, Shot.SubjectScreenY, Shot.FocalLength, AspectRatio, 0.0f);
        double Cost = 0.3 * FMath::Square((Angle - BaseAngle) / 75.0f);

        bool bOccluded = false;
        for (const AActor* Occluder : Occluders)
        {
            for (const FVector& Point : AnchorPoints)
            {
                bOccluded |= SegmentHitsBody(Camera, Point, Occluder);
            }
        }
        if (bOccluded) Cost += 10.0;

        const double AnchorDepth = FVector::DotProduct(AnchorFocus - Camera, Rotation.Vector());
        for (const FStagedSubject& Other : Others)
        {
            const FVector OtherMid = UPTFraming::GetFocusPoint(Other.Actor, 0.5f);
            FVector2D Screen;
            if (!UPTFraming::ProjectToScreen(Camera, Rotation, OtherMid, Shot.FocalLength, AspectRatio, Screen))
            {
                Cost += 5.0;
                continue;
            }
            Cost += 4.0 * FMath::Square(Screen.X - Other.ScreenX);
            const FBox OtherBounds = UPTFraming::GetSubjectBounds(Other.Actor);
            const double OtherHeight = OtherBounds.IsValid ? OtherBounds.GetSize().Z : AnchorHeight;
            const double OtherDepth = FVector::DotProduct(OtherMid - Camera, Rotation.Vector());
            if (AnchorDepth > 1.0 && OtherDepth > 1.0 && Other.ScreenHeightRatio > 0.01f)
            {
                const double Predicted = (OtherHeight / OtherDepth) / (AnchorHeight / AnchorDepth);
                Cost += 0.5 * FMath::Square(FMath::Loge(Predicted / Other.ScreenHeightRatio));
            }
            // 다른 인물이 기준 인물 뒤에 완전히 숨으면 투샷이 성립하지 않는다(어깨 너머 정도는 허용해 벌점을 작게 둔다).
            if (SegmentHitsBody(Camera, OtherMid, Anchor)) Cost += 2.0;
        }
        if (Cost < BestCost)
        {
            BestCost = Cost;
            BestAngle = Angle;
            bBestOccluded = bOccluded;
        }
    }

    const float BestRadians = FMath::DegreesToRadians(BestAngle);
    Shot.DistanceCm = FMath::Max(30.0f, Radius * FMath::Cos(BestRadians));
    Shot.SideCm = Radius * FMath::Sin(BestRadians);
    if (!FMath::IsNearlyEqual(BestAngle, BaseAngle, 1.0f) || bBestOccluded)
    {
        OutNotes.Add(FString::Printf(TEXT("%s: 카메라 방위각 %+.0f°%s"), *Shot.Name, BestAngle,
            bBestOccluded ? TEXT(" (가리지 않는 각도를 찾지 못함 — 등장인물 위치 확인 필요)") : TEXT("")));
    }
}

// Mesh 정면이 Actor +X와 다를 수 있으므로(Skeletal Mesh Actor 등) 실제 정면이 Target을 보도록 Actor Yaw를 보정한다.
FRotator FacingRotation(const AActor* Actor, const FVector& FromLocation, const FVector& TargetLocation)
{
    FVector Forward, Right;
    UPTFraming::GetSubjectBasis(Actor, Forward, Right);
    const double MeshYawOffset = FRotator::NormalizeAxis(Forward.Rotation().Yaw - Actor->GetActorRotation().Yaw);
    return FRotator(0.0, (TargetLocation - FromLocation).Rotation().Yaw - MeshYawOffset, 0.0);
}
}

bool FUPTReferenceBlockingSolver::SolveOverShoulderCamera(const FUPTOverShoulderSetup& Setup, FVector& OutCamera, float& OutFocalLength)
{
    // 주 피사체에서 앞사람 쪽 수평 방향. 카메라는 앞사람보다 바깥(뒤)에서 주 피사체를 향한다.
    FVector Toward = Setup.ForegroundBase - Setup.AnchorFocus;
    Toward.Z = 0.0;
    const double Gap = Toward.Size();
    if (Gap < 30.0) return false;
    Toward /= Gap;
    const FVector Lateral = FVector::CrossProduct(FVector::UpVector, Toward);
    const float BodyHeight = FMath::Max(30.0f, Setup.ForegroundHeightCm);
    const float BodyRadius = FMath::Max(15.0f, BodyHeight * 0.14f);
    const FVector Shoulder = Setup.ForegroundBase + FVector(0.0, 0.0, BodyHeight * 0.82f);
    const FVector ForegroundHead = Setup.ForegroundBase + FVector(0.0, 0.0, BodyHeight * 0.95f);
    const float EdgeTarget = FMath::Clamp(Setup.ForegroundEdgeX, 0.02f, 0.98f);
    // 가로만 맞추면 안 된다. 주 피사체를 화면 아래쪽에 두는 샷에서는 카메라가 위를 향하고,
    // 카메라보다 낮은 앞사람이 통째로 화면 밖 아래로 빠져 어깨 너머로 보이지 않는다.
    constexpr double ShoulderMinScreenY = 0.45;   // 어깨가 이보다 위면 앞사람이 화면을 가린다
    constexpr double ShoulderMaxScreenY = 1.05;   // 이보다 아래면 어깨가 프레임 밖이다
    constexpr double HeadMaxScreenY = 0.85;       // 앞사람 머리가 보여야 '어깨 너머'로 읽힌다
    constexpr double ShoulderTargetScreenY = 0.85;

    double BestCost = TNumericLimits<double>::Max();
    double BestEdgeError = 1.0;
    // 카메라 높이는 앞사람 키의 65~105% 사이에서 함께 찾는다(고정하면 세로 조건을 만족할 자리가 없다).
    for (float HeightRatio = 0.65f; HeightRatio <= 1.05f + KINDA_SMALL_NUMBER; HeightRatio += 0.05f)
    for (float Back = 20.0f; Back <= 200.0f + KINDA_SMALL_NUMBER; Back += 10.0f)
    {
        for (float Offset = -150.0f; Offset <= 150.0f + KINDA_SMALL_NUMBER; Offset += 5.0f)
        {
            FVector Camera = Setup.ForegroundBase + Toward * Back + Lateral * Offset;
            Camera.Z = Setup.ForegroundBase.Z + BodyHeight * HeightRatio;
            if (FVector::Dist2D(Camera, Setup.ForegroundBase) < BodyRadius + 10.0f) continue;
            // 주 피사체가 가려지면 어깨 너머 샷이 아니다.
            if (SegmentHitsCapsule(Camera, Setup.AnchorFocus, Setup.ForegroundBase, BodyHeight)
                || SegmentHitsCapsule(Camera, Setup.AnchorHead, Setup.ForegroundBase, BodyHeight))
            {
                continue;
            }
            // 주 피사체까지 거리가 달라진 만큼 렌즈를 바꿔 레퍼런스 샷 크기를 유지한다.
            const float DesiredFocal = Setup.BaseFocalLength * static_cast<float>(FVector::Dist(Camera, Setup.AnchorFocus)) / FMath::Max(30.0f, Setup.BaseDistanceCm);
            const float Focal = FMath::Clamp(DesiredFocal, 12.0f, 150.0f);
            const FRotator Rotation = UPTFraming::ComputeFramingRotation(Camera, Setup.AnchorFocus, Setup.SubjectScreenX, Setup.SubjectScreenY, Focal, Setup.AspectRatio, 0.0f);
            const FVector CameraRight = FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y);
            FVector2D Edge, Center, Head;
            if (!UPTFraming::ProjectToScreen(Camera, Rotation, Shoulder + CameraRight * (Setup.bForegroundOnRight ? -BodyRadius : BodyRadius), Focal, Setup.AspectRatio, Edge)
                || !UPTFraming::ProjectToScreen(Camera, Rotation, Shoulder, Focal, Setup.AspectRatio, Center)
                || !UPTFraming::ProjectToScreen(Camera, Rotation, ForegroundHead, Focal, Setup.AspectRatio, Head))
            {
                continue;
            }
            // 앞사람 몸 중심은 요청한 쪽 바깥에 있어야 한다(반대쪽에 걸치면 제외).
            if (Setup.bForegroundOnRight ? Center.X < Edge.X : Center.X > Edge.X) continue;
            // 앞사람이 세로로도 프레임 안에 있어야 한다.
            if (Center.Y < ShoulderMinScreenY || Center.Y > ShoulderMaxScreenY) continue;
            if (Head.Y > HeadMaxScreenY) continue;
            const double EdgeError = FMath::Abs(Edge.X - EdgeTarget);
            double Cost = 20.0 * FMath::Square(EdgeError) + 8.0 * FMath::Square(Center.Y - ShoulderTargetScreenY)
                + 0.2 * FMath::Square((Back - 60.0) / 100.0);
            if (!FMath::IsNearlyEqual(Focal, DesiredFocal, 0.5f)) Cost += 1.0;
            if (Cost < BestCost)
            {
                BestCost = Cost;
                BestEdgeError = EdgeError;
                OutCamera = Camera;
                OutFocalLength = Focal;
            }
        }
    }
    return BestCost < TNumericLimits<double>::Max() && BestEdgeError <= 0.08;
}

bool FUPTReferenceBlockingSolver::ApplyOverShoulder(FUPTCinematicShot& Shot, const AActor* Anchor, const AActor* Foreground, const float AspectRatio, FString& OutNote)
{
    if (!Anchor || !Foreground || Anchor == Foreground)
    {
        OutNote = FString::Printf(TEXT("%s: 어깨 너머 앞사람은 주 피사체와 다른 Actor여야 합니다."), *Shot.Name);
        return false;
    }
    const FBox ForegroundBounds = UPTFraming::GetSubjectBounds(Foreground);
    FVector Shift = FVector::ZeroVector;
    for (const FUPTActorBlockingPlacement& Placement : Shot.ActorPlacements)
    {
        if (Placement.ActorLabel == Shot.OverShoulderActor) Shift = Placement.WorldLocation - Foreground->GetActorLocation();
    }

    FUPTOverShoulderSetup Setup;
    Setup.AnchorFocus = UPTFraming::GetFocusPoint(Anchor, Shot.FocusHeightRatio);
    Setup.AnchorHead = UPTFraming::GetFocusPoint(Anchor, 0.9f);
    Setup.ForegroundBase = Shift + (ForegroundBounds.IsValid
        ? FVector(ForegroundBounds.GetCenter().X, ForegroundBounds.GetCenter().Y, ForegroundBounds.Min.Z)
        : Foreground->GetActorLocation());
    Setup.ForegroundHeightCm = ForegroundBounds.IsValid ? static_cast<float>(ForegroundBounds.GetSize().Z) : 180.0f;
    Setup.bForegroundOnRight = Shot.OverShoulderSide != TEXT("left");
    Setup.ForegroundEdgeX = Shot.OverShoulderEdgeX;
    Setup.SubjectScreenX = Shot.SubjectScreenX;
    Setup.SubjectScreenY = Shot.SubjectScreenY;
    Setup.BaseFocalLength = Shot.FocalLength;
    Setup.BaseDistanceCm = static_cast<float>(FVector(Shot.DistanceCm, Shot.SideCm, Shot.HeightCm).Size());
    Setup.AspectRatio = AspectRatio;

    FVector Camera = FVector::ZeroVector;
    float Focal = Shot.FocalLength;
    if (!SolveOverShoulderCamera(Setup, Camera, Focal))
    {
        OutNote = FString::Printf(TEXT("%s: '%s' 어깨 너머에서 주 피사체를 가리지 않고 앞사람을 화면 가장자리에 걸치는 카메라 자리를 찾지 못해 일반 구도로 둡니다."),
            *Shot.Name, *Shot.OverShoulderActor);
        return false;
    }
    float OffsetDistance = 0.0f, OffsetSide = 0.0f, OffsetHeight = 0.0f;
    UPTFraming::DecomposeCameraOffset(Anchor, Shot.FocusHeightRatio, Camera, OffsetDistance, OffsetSide, OffsetHeight);
    if (OffsetDistance < 30.0f)
    {
        OutNote = FString::Printf(TEXT("%s: '%s'이(가) 주 피사체 정면 쪽에 있지 않아 어깨 너머 카메라가 피사체 뒤로 갑니다. 앞사람을 주 피사체 앞에 세우세요."),
            *Shot.Name, *Shot.OverShoulderActor);
        return false;
    }
    Shot.DistanceCm = OffsetDistance;
    Shot.SideCm = OffsetSide;
    Shot.HeightCm = OffsetHeight;
    Shot.FocalLength = Focal;
    Shot.CameraRollDegrees = 0.0f;
    OutNote = FString::Printf(TEXT("%s: '%s' 어깨 너머(앞사람 화면 %s, 렌즈 %.0fmm)"), *Shot.Name, *Shot.OverShoulderActor,
        Setup.bForegroundOnRight ? TEXT("오른쪽") : TEXT("왼쪽"), Focal);
    return true;
}

void FUPTReferenceBlockingSolver::ApplyShotFraming(FUPTCinematicShot& Shot, const FString& ShotSize, const FString& CameraAngle, const float SubjectHeightCm,
    const float ScreenHeightFraction, const float AspectRatio)
{
    // 화면 세로 점유율과 렌즈 화각으로 거리를 역산한다: 거리 = 보이는 신체 높이 × 초점거리 / (센서 높이 × 점유율)
    const float BodyHeightCm = FMath::Max(30.0f, SubjectHeightCm);
    Shot.FocalLength = FocalLengthForShotSize(ShotSize);
    Shot.FocusHeightRatio = FocusHeightRatioForShotSize(ShotSize);
    const float Fraction = FMath::Clamp(ScreenHeightFraction, 0.05f, 1.0f);
    const float VisibleHeightCm = BodyHeightCm * VisibleBodyRatioForShotSize(ShotSize);
    Shot.DistanceCm = FMath::Clamp(VisibleHeightCm * Shot.FocalLength / (UPTFraming::GetSensorHeightMm(AspectRatio) * Fraction), 50.0f, 10000.0f);
    Shot.SideCm = 0.0f;
    Shot.HeightCm = ComputeCameraHeight(CameraAngle, Shot.DistanceCm, BodyHeightCm, Shot.FocusHeightRatio);
    Shot.CameraRollDegrees = CameraAngle == TEXT("dutch") ? (Shot.SubjectScreenX < 0.5f ? -10.0f : 10.0f) : 0.0f;
}

float FUPTReferenceBlockingSolver::ComputeCameraHeight(const FString& CameraAngle, const float HorizontalDistanceCm, const float SubjectHeightCm, const float FocusHeightRatio)
{
    float Height = 0.0f;
    if (CameraAngle == TEXT("low"))
    {
        // 바닥 아래로 내려가지 않도록 조준점 높이에서 20cm 위까지만 낮춘다.
        const float FocusAboveFeetCm = FMath::Max(30.0f, SubjectHeightCm) * FocusHeightRatio;
        Height = -FMath::Min(HorizontalDistanceCm * FMath::Tan(FMath::DegreesToRadians(15.0f)), FMath::Max(0.0f, FocusAboveFeetCm - 20.0f));
    }
    else if (CameraAngle == TEXT("high")) Height = HorizontalDistanceCm * FMath::Tan(FMath::DegreesToRadians(25.0f));
    else if (CameraAngle == TEXT("overhead")) Height = HorizontalDistanceCm * FMath::Tan(FMath::DegreesToRadians(60.0f));
    return FMath::Clamp(Height, -5000.0f, 5000.0f);
}

float FUPTReferenceBlockingSolver::DefaultScreenHeightFraction(const FString& ShotSize)
{
    if (ShotSize == TEXT("extreme_close_up")) return 1.0f;
    if (ShotSize == TEXT("close_up") || ShotSize == TEXT("medium")) return 0.95f;
    if (ShotSize == TEXT("full")) return 0.85f;
    return 0.35f;
}

FString FUPTReferenceBlockingSolver::InferShotSize(const FUPTCinematicShot& Shot)
{
    if (Shot.FocusHeightRatio >= 0.9f) return TEXT("extreme_close_up");
    if (Shot.FocusHeightRatio >= 0.8f) return TEXT("close_up");
    if (Shot.FocusHeightRatio >= 0.6f) return TEXT("medium");
    return Shot.FocalLength <= 32.0f ? TEXT("wide") : TEXT("full");
}

FString FUPTReferenceBlockingSolver::InferCameraAngle(const FUPTCinematicShot& Shot)
{
    if (!FMath::IsNearlyZero(Shot.CameraRollDegrees, 1.0f)) return TEXT("dutch");
    const float Radius = FMath::Max(1.0f, FMath::Sqrt(FMath::Square(Shot.DistanceCm) + FMath::Square(Shot.SideCm)));
    const float Degrees = FMath::RadiansToDegrees(FMath::Atan2(Shot.HeightCm, Radius));
    if (Degrees > 45.0f) return TEXT("overhead");
    if (Degrees > 12.0f) return TEXT("high");
    if (Degrees < -5.0f) return TEXT("low");
    return TEXT("eye");
}

bool FUPTReferenceBlockingSolver::BuildPlan(const FUPTReferencePlan& ReferencePlan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const TMap<FString, FString>& ExplicitRoleMappings, FUPTCinematicPlan& OutPlan, FString& OutReport)
{
    if (ReferencePlan.Shots.IsEmpty()) { OutReport = TEXT("변환할 Reference Shot이 없습니다."); return false; }
    TArray<FString> ActorLabels;
    SceneActors.GetKeys(ActorLabels);
    ActorLabels.Sort();
    if (ActorLabels.IsEmpty())
    {
        OutReport = TEXT("레퍼런스 기반 생성에는 레벨에서 등장인물 Actor를 1명 이상 선택해야 합니다. 선택 없이 생성하면 모든 카메라가 월드 원점에 놓입니다.");
        return false;
    }

    TMap<FString, FString> RoleToActor;
    TArray<FString> MappingLines;
    for (const TPair<FString, FString>& Mapping : ExplicitRoleMappings)
    {
        if (!SceneActors.Contains(Mapping.Value))
        {
            OutReport = FString::Printf(TEXT("역할 '%s'에 지정한 Actor '%s'가 현재 선택 Actor 목록에 없습니다."), *Mapping.Key, *Mapping.Value);
            return false;
        }
        RoleToActor.Add(Mapping.Key, Mapping.Value);
        MappingLines.Add(FString::Printf(TEXT("%s -> %s (수동)"), *Mapping.Key, *Mapping.Value));
    }
    TSet<FString> UsedActorLabels;
    for (const TPair<FString, FString>& Mapping : RoleToActor) UsedActorLabels.Add(Mapping.Value);

    TArray<FString> RequiredRoles;
    auto AddRequiredRole = [&RequiredRoles](const FString& Role)
    {
        if (!Role.IsEmpty() && !RequiredRoles.Contains(Role)) RequiredRoles.Add(Role);
    };
    for (const FUPTReferenceShot& ReferenceShot : ReferencePlan.Shots)
    {
        AddRequiredRole(ReferenceShot.SubjectRole);
        for (const FUPTReferenceSubject& Subject : ReferenceShot.Subjects) AddRequiredRole(Subject.Role);
    }

    int32 ReusedRoles = 0;
    for (const FString& Role : RequiredRoles)
    {
        if (RoleToActor.Contains(Role)) continue;

        FString MatchedActor;
        for (const FString& ActorLabel : ActorLabels)
        {
            if (!UsedActorLabels.Contains(ActorLabel) && ActorLabel.Equals(Role, ESearchCase::IgnoreCase))
            { MatchedActor = ActorLabel; break; }
        }
        if (MatchedActor.IsEmpty())
        {
            for (const FString& ActorLabel : ActorLabels)
            {
                if (!UsedActorLabels.Contains(ActorLabel)
                    && (ActorLabel.Contains(Role, ESearchCase::IgnoreCase) || Role.Contains(ActorLabel, ESearchCase::IgnoreCase)))
                { MatchedActor = ActorLabel; break; }
            }
        }
        if (MatchedActor.IsEmpty())
        {
            for (const FString& ActorLabel : ActorLabels)
            {
                if (!UsedActorLabels.Contains(ActorLabel)) { MatchedActor = ActorLabel; break; }
            }
        }
        if (MatchedActor.IsEmpty())
        {
            // 영상 속 인물보다 선택한 Actor가 적으면 생성을 멈추지 않고 이미 배정한 Actor를 다시 쓴다.
            // 같은 샷에 함께 나오는 두 인물이 같은 Actor가 되면 두 번째 인물은 배치·구도 계산에서 빠진다.
            MatchedActor = ActorLabels[ReusedRoles % ActorLabels.Num()];
            ++ReusedRoles;
            RoleToActor.Add(Role, MatchedActor);
            MappingLines.Add(FString::Printf(TEXT("%s -> %s (Actor 부족으로 재사용)"), *Role, *MatchedActor));
            continue;
        }
        RoleToActor.Add(Role, MatchedActor);
        UsedActorLabels.Add(MatchedActor);
        MappingLines.Add(FString::Printf(TEXT("%s -> %s (자동)"), *Role, *MatchedActor));
    }

    OutPlan = FUPTCinematicPlan();
    OutPlan.Title = ReferencePlan.Title.IsEmpty() ? TEXT("ReferenceBlocking") : ReferencePlan.Title + TEXT("_Blocking");
    OutPlan.FrameRate = 30;
    OutPlan.AspectRatio = FMath::Clamp(ReferencePlan.AspectRatio, 0.25f, 4.0f);
    auto ResolveRole = [&](const FString& Role) -> FString
    {
        if (Role.IsEmpty()) return FString();
        if (const FString* Existing = RoleToActor.Find(Role)) return *Existing;
        return FString();
    };
    const FString DefaultSubject = !RequiredRoles.IsEmpty() && RoleToActor.Contains(RequiredRoles[0]) ? RoleToActor[RequiredRoles[0]] : ActorLabels[0];
    int32 DefaultSubjectShots = 0;
    TArray<FString> SolveNotes;
    TArray<FString> OverShoulderNotes;
    // 어깨 너머 샷의 앞사람 후보 순위: 영상 전체에서 많이 나온 역할.
    TMap<FString, int32> RoleShotCounts;
    for (const FUPTReferenceShot& ReferenceShot : ReferencePlan.Shots)
    {
        for (const FUPTReferenceSubject& Subject : ReferenceShot.Subjects) RoleShotCounts.FindOrAdd(Subject.Role) += 1;
    }

    for (const FUPTReferenceShot& ReferenceShot : ReferencePlan.Shots)
    {
        FUPTCinematicShot Shot;
        Shot.Name = ReferenceShot.Name;
        Shot.Description = FString::Printf(TEXT("Reference: %s / %s / %s / %s (confidence %.0f%%)"), *ReferenceShot.ShotSize, *ReferenceShot.CameraAngle, *ReferenceShot.CameraMotion, *ReferenceShot.Composition, ReferenceShot.Confidence * 100.0f);
        Shot.DurationSeconds = FMath::Max(0.1f, ReferenceShot.EndSeconds - ReferenceShot.StartSeconds);
        Shot.Subject = ResolveRole(ReferenceShot.SubjectRole);
        if (Shot.Subject.IsEmpty())
        {
            Shot.Subject = DefaultSubject;
            ++DefaultSubjectShots;
        }
        Shot.LookAt = Shot.Subject;

        AActor* AnchorActor = SceneActors.FindRef(Shot.Subject).Get();
        const FBox AnchorBounds = UPTFraming::GetSubjectBounds(AnchorActor);
        const float SubjectHeightCm = AnchorBounds.IsValid ? FMath::Max(30.0f, static_cast<float>(AnchorBounds.GetSize().Z)) : 180.0f;

        Shot.SubjectScreenX = FMath::Clamp(static_cast<float>(ReferenceShot.SubjectScreenPosition.X), 0.05f, 0.95f);
        Shot.SubjectScreenY = FMath::Clamp(static_cast<float>(ReferenceShot.SubjectScreenPosition.Y), 0.05f, 0.95f);
        ApplyShotFraming(Shot, ReferenceShot.ShotSize, ReferenceShot.CameraAngle, SubjectHeightCm, ReferenceShot.SubjectScreenSize.Y, OutPlan.AspectRatio);
        Shot.CameraMotion = ReferenceShot.CameraMotion;

        float AnchorScreenHeight = ReferenceShot.SubjectScreenSize.Y;
        for (const FUPTReferenceSubject& ReferenceSubject : ReferenceShot.Subjects)
        {
            if (ReferenceSubject.Role == ReferenceShot.SubjectRole) { AnchorScreenHeight = ReferenceSubject.ScreenSize.Y; break; }
        }

        // 등장인물이 이미 서로 가까이 서 있으면(연출해 둔 장면) 액터를 옮기지 않고 카메라 방위각으로 레퍼런스 배치를 맞춘다.
        // 혼자 나오는 샷도 주변 인물에게 가리지 않도록 같은 방식으로 각도를 고른다.
        bool bCameraSolved = false;
        if (AnchorActor && !ReferenceShot.OverShoulderSide.IsEmpty())
        {
            // 레퍼런스가 어깨 너머 샷이면, 이 샷 주 피사체가 아닌 인물 중 영상에 가장 많이 나온 인물(없으면 가장 가까운 Actor)을 앞사람으로 세운다.
            AActor* ForegroundActor = nullptr;
            FString ForegroundLabel;
            int32 BestRoleCount = 0;
            for (const TPair<FString, int32>& RoleCount : RoleShotCounts)
            {
                const FString CandidateLabel = ResolveRole(RoleCount.Key);
                AActor* Candidate = SceneActors.FindRef(CandidateLabel).Get();
                if (Candidate && Candidate != AnchorActor && RoleCount.Value > BestRoleCount)
                {
                    ForegroundActor = Candidate;
                    ForegroundLabel = CandidateLabel;
                    BestRoleCount = RoleCount.Value;
                }
            }
            if (!ForegroundActor)
            {
                double NearestDistance = TNumericLimits<double>::Max();
                for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
                {
                    AActor* Candidate = Pair.Value.Get();
                    if (!Candidate || Candidate == AnchorActor) continue;
                    const double CandidateDistance = FVector::Dist2D(Candidate->GetActorLocation(), AnchorActor->GetActorLocation());
                    if (CandidateDistance < NearestDistance)
                    {
                        NearestDistance = CandidateDistance;
                        ForegroundActor = Candidate;
                        ForegroundLabel = Pair.Key;
                    }
                }
            }
            if (!ForegroundActor)
            {
                OverShoulderNotes.Add(FString::Printf(TEXT("%s: 레퍼런스는 어깨 너머 샷이지만 앞사람으로 세울 다른 Actor가 없어 일반 구도로 만듭니다"), *Shot.Name));
            }
            else
            {
                Shot.OverShoulderActor = ForegroundLabel;
                Shot.OverShoulderSide = ReferenceShot.OverShoulderSide;
                const float HalfWidth = ReferenceShot.OverShoulderScreenWidth * 0.5f;
                Shot.OverShoulderEdgeX = ReferenceShot.OverShoulderSide == TEXT("left")
                    ? FMath::Clamp(ReferenceShot.OverShoulderScreenX + HalfWidth, 0.02f, 0.45f)
                    : FMath::Clamp(ReferenceShot.OverShoulderScreenX - HalfWidth, 0.55f, 0.98f);
                // 앞사람이 주 피사체 정면 쪽 4m 안에 서 있지 않으면 이 샷에서만 정면 1.5m로 옮긴다.
                FVector AnchorForward, AnchorRight;
                UPTFraming::GetSubjectBasis(AnchorActor, AnchorForward, AnchorRight);
                const FVector ToForeground = ForegroundActor->GetActorLocation() - AnchorActor->GetActorLocation();
                const double Ahead = FVector::DotProduct(ToForeground, AnchorForward);
                if (Ahead < 60.0 || ToForeground.Size2D() > 400.0 || FMath::Abs(FVector::DotProduct(ToForeground, AnchorRight)) > Ahead)
                {
                    FUPTActorBlockingPlacement ForegroundPlacement;
                    ForegroundPlacement.ActorLabel = ForegroundLabel;
                    ForegroundPlacement.WorldLocation = SnapPlacementToGround(ForegroundActor, AnchorActor->GetActorLocation() + AnchorForward * 150.0f, SceneActors);
                    ForegroundPlacement.WorldRotation = FacingRotation(ForegroundActor, ForegroundPlacement.WorldLocation, AnchorActor->GetActorLocation());
                    Shot.ActorPlacements.Add(MoveTemp(ForegroundPlacement));
                }
                FString OverShoulderNote;
                bCameraSolved = ApplyOverShoulder(Shot, AnchorActor, ForegroundActor, OutPlan.AspectRatio, OverShoulderNote);
                OverShoulderNotes.Add(OverShoulderNote);
                if (!bCameraSolved)
                {
                    Shot.OverShoulderActor.Empty();
                    Shot.OverShoulderSide.Empty();
                    Shot.ActorPlacements.Reset();
                }
            }
        }
        if (AnchorActor && !bCameraSolved)
        {
            constexpr float StagingRadiusCm = 800.0f;
            const FVector AnchorLocation = AnchorActor->GetActorLocation();
            TArray<AActor*> Occluders;
            for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
            {
                AActor* Other = Pair.Value.Get();
                if (Other && Other != AnchorActor && FVector::Dist2D(Other->GetActorLocation(), AnchorLocation) <= StagingRadiusCm * 2.0f) Occluders.Add(Other);
            }
            TArray<FStagedSubject> Others;
            bool bAllStaged = true;
            for (const FUPTReferenceSubject& ReferenceSubject : ReferenceShot.Subjects)
            {
                AActor* OtherActor = ReferenceSubject.Role == ReferenceShot.SubjectRole ? nullptr : SceneActors.FindRef(ResolveRole(ReferenceSubject.Role)).Get();
                if (!OtherActor || OtherActor == AnchorActor) continue;
                if (FVector::Dist2D(OtherActor->GetActorLocation(), AnchorLocation) > StagingRadiusCm) { bAllStaged = false; break; }
                FStagedSubject Staged;
                Staged.Actor = OtherActor;
                Staged.ScreenX = ReferenceSubject.ScreenPosition.X;
                Staged.ScreenHeightRatio = AnchorScreenHeight > 0.01f ? ReferenceSubject.ScreenSize.Y / AnchorScreenHeight : 1.0f;
                Others.Add(Staged);
            }
            if (bAllStaged && (!Others.IsEmpty() || !Occluders.IsEmpty()))
            {
                SolveStagedCameraAzimuth(Shot, AnchorActor, OutPlan.AspectRatio, Others, Occluders, SolveNotes);
                bCameraSolved = true;
            }
        }

        if (AnchorActor && ReferenceShot.Subjects.Num() > 1 && !bCameraSolved)
        {
            const FVector AnchorLocation = AnchorActor->GetActorLocation();
            FVector Forward, Right;
            UPTFraming::GetSubjectBasis(AnchorActor, Forward, Right);
            const float TanHalfHorizontal = UPTFraming::SensorWidthMm * 0.5f / FMath::Max(1.0f, Shot.FocalLength);
            TArray<FVector> AcceptedLocations;
            TArray<float> AcceptedRadii;
            TSet<FString> PlacedLabels;
            for (const FUPTReferenceSubject& ReferenceSubject : ReferenceShot.Subjects)
            {
                const FString ActorLabel = ResolveRole(ReferenceSubject.Role);
                AActor* BlockingActor = SceneActors.FindRef(ActorLabel).Get();
                if (!BlockingActor || PlacedLabels.Contains(ActorLabel)) continue;
                PlacedLabels.Add(ActorLabel);
                FUPTActorBlockingPlacement Placement;
                Placement.ActorLabel = ActorLabel;
                // 카메라가 Anchor 정면에 있으므로 화면 오른쪽은 Anchor의 왼쪽(-Right), 화면 안쪽 깊이는 -Forward 방향이다.
                // 화면 가로 차이 Δx는 피사체 거리에서 Δx × 2 × 거리 × tan(가로 화각/2)만큼의 실제 간격이다.
                const float HorizontalOffset = (ReferenceSubject.ScreenPosition.X - ReferenceShot.SubjectScreenPosition.X) * 2.0f * Shot.DistanceCm * TanHalfHorizontal;
                // 키가 비슷하다고 보면 화면에서 작을수록 멀리 있다: 깊이 = 기준 거리 × (기준 인물 크기 / 이 인물 크기).
                const float SizeRatio = AnchorScreenHeight > 0.01f && ReferenceSubject.ScreenSize.Y > 0.01f ? AnchorScreenHeight / ReferenceSubject.ScreenSize.Y : 1.0f;
                const float DepthOffset = FMath::Clamp(Shot.DistanceCm * (SizeRatio - 1.0f), -0.7f * Shot.DistanceCm, 3.0f * Shot.DistanceCm);
                FVector DesiredLocation = AnchorLocation - Right * HorizontalOffset - Forward * DepthOffset;
                DesiredLocation = SnapPlacementToGround(BlockingActor, DesiredLocation, SceneActors);
                const FBox ActorBounds = BlockingActor->GetComponentsBoundingBox(true);
                const float Radius = ActorBounds.IsValid ? FMath::Max(35.0f, ActorBounds.GetExtent().Size2D()) : 50.0f;
                for (int32 Attempt = 0; Attempt < 6; ++Attempt)
                {
                    bool bOverlaps = false;
                    for (int32 ExistingIndex = 0; ExistingIndex < AcceptedLocations.Num(); ++ExistingIndex)
                    {
                        if (FVector::Dist2D(DesiredLocation, AcceptedLocations[ExistingIndex]) < Radius + AcceptedRadii[ExistingIndex])
                        { bOverlaps = true; break; }
                    }
                    if (!bOverlaps) break;
                    DesiredLocation += Right * (Radius * (Attempt % 2 == 0 ? 1.0f : -1.0f));
                }
                Placement.WorldLocation = DesiredLocation;
                Placement.WorldRotation = FVector::Dist2D(DesiredLocation, AnchorLocation) < 10.0f
                    ? BlockingActor->GetActorRotation()
                    : FacingRotation(BlockingActor, DesiredLocation, AnchorLocation);
                AcceptedLocations.Add(Placement.WorldLocation);
                AcceptedRadii.Add(Radius);
                Shot.ActorPlacements.Add(MoveTemp(Placement));
            }
        }
        OutPlan.Shots.Add(MoveTemp(Shot));
    }

    OutReport = FString::Printf(TEXT("역할 자동 매핑: %s. 생성 전 Shot Review에서 카메라를 확인하세요."), *FString::Join(MappingLines, TEXT(", ")));
    if (!SolveNotes.IsEmpty()) OutReport += TEXT("\n[카메라 각도 보정] ") + FString::Join(SolveNotes, TEXT(" / "));
    if (!OverShoulderNotes.IsEmpty()) OutReport += TEXT("\n[어깨 너머] ") + FString::Join(OverShoulderNotes, TEXT(" / "));
    if (ReusedRoles > 0)
    {
        OutReport += FString::Printf(TEXT("\n[WARN] 영상 속 인물 역할 %d개에 배정할 Actor가 부족해 같은 Actor를 재사용했습니다. 인물마다 다른 Actor를 쓰려면 Actor를 더 선택하세요."), ReusedRoles);
    }
    if (DefaultSubjectShots > 0)
    {
        OutReport += FString::Printf(TEXT("\n[WARN] 주 피사체 역할이 없는 %d개 Shot은 '%s'를 기준으로 촬영합니다."), DefaultSubjectShots, *DefaultSubject);
    }
    return true;
}
