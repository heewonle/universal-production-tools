#include "UPTBlockingValidator.h"

FUPTBlockingValidationResult FUPTBlockingValidator::Validate(const FUPTCinematicPlan& Plan)
{
    FUPTBlockingValidationResult Result;
    if (Plan.Shots.IsEmpty()) Result.Errors.Add(TEXT("Shot Plan이 비어 있습니다."));
    if (Plan.FrameRate < 1 || Plan.FrameRate > 120) Result.Errors.Add(TEXT("Frame Rate는 1~120 범위여야 합니다."));
    if (!FMath::IsFinite(Plan.AspectRatio) || Plan.AspectRatio < 0.25f || Plan.AspectRatio > 4.0f) Result.Errors.Add(TEXT("화면비는 0.25~4.0 범위여야 합니다."));
    const TSet<FString> AllowedMotions = { TEXT(""), TEXT("static"), TEXT("pan"), TEXT("tilt"), TEXT("dolly_in"), TEXT("dolly_out"), TEXT("zoom_in"), TEXT("zoom_out"), TEXT("truck_left"), TEXT("truck_right"), TEXT("pedestal"), TEXT("orbit"), TEXT("tracking"), TEXT("handheld") };
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        const FString Prefix = FString::Printf(TEXT("Shot %02d (%s)"), Index + 1, *Shot.Name);
        if (Shot.Name.IsEmpty()) Result.Errors.Add(Prefix + TEXT(": 이름이 비어 있습니다."));
        if (!FMath::IsFinite(Shot.DurationSeconds) || Shot.DurationSeconds < 0.1f || Shot.DurationSeconds > 120.0f) Result.Errors.Add(Prefix + TEXT(": 길이는 0.1~120초여야 합니다."));
        if (!FMath::IsFinite(Shot.FocalLength) || Shot.FocalLength < 8.0f || Shot.FocalLength > 200.0f) Result.Errors.Add(Prefix + TEXT(": 초점거리는 8~200mm여야 합니다."));
        if (!FMath::IsFinite(Shot.DistanceCm) || Shot.DistanceCm < 20.0f || Shot.DistanceCm > 10000.0f) Result.Errors.Add(Prefix + TEXT(": 카메라 거리가 안전 범위를 벗어났습니다."));
        if (Shot.Subject.IsEmpty() && Shot.CameraLocation.IsNearlyZero() && Shot.CameraRotation.IsNearlyZero())
            Result.Errors.Add(Prefix + TEXT(": Subject도 절대 카메라 Transform도 없어 카메라가 월드 원점에 생성됩니다. 등장인물 Actor를 선택하거나 camera_location을 지정하세요."));
        if (!AllowedMotions.Contains(Shot.CameraMotion)) Result.Errors.Add(Prefix + TEXT(": 지원하지 않는 Camera Motion입니다: ") + Shot.CameraMotion);
        if ((Shot.CameraMotion == TEXT("zoom_in") || Shot.CameraMotion == TEXT("zoom_out")) && Shot.FocalLength >= 190.0f) Result.Warnings.Add(Prefix + TEXT(": Zoom 렌즈 상한에 가까워 변화 폭이 작을 수 있습니다."));
        TSet<FString> PlacementActors;
        for (const FUPTActorBlockingPlacement& Placement : Shot.ActorPlacements)
        {
            if (Placement.ActorLabel.IsEmpty()) Result.Errors.Add(Prefix + TEXT(": Actor Placement 라벨이 비어 있습니다."));
            else if (PlacementActors.Contains(Placement.ActorLabel)) Result.Errors.Add(Prefix + TEXT(": 동일 Actor Placement가 중복됐습니다: ") + Placement.ActorLabel);
            PlacementActors.Add(Placement.ActorLabel);
            if (Placement.WorldLocation.ContainsNaN() || Placement.WorldRotation.ContainsNaN()) Result.Errors.Add(Prefix + TEXT(": Actor Placement Transform에 NaN이 있습니다."));
        }
        if (!Shot.Subject.IsEmpty() && Shot.LookAt.IsEmpty()) Result.Warnings.Add(Prefix + TEXT(": Subject는 있지만 LookAt이 비어 있습니다."));
    }
    return Result;
}

FString FUPTBlockingValidationResult::ToText() const
{
    FString Text;
    for (const FString& Error : Errors) Text += TEXT("[ERROR] ") + Error + TEXT("\n");
    for (const FString& Warning : Warnings) Text += TEXT("[WARN] ") + Warning + TEXT("\n");
    if (Errors.IsEmpty() && Warnings.IsEmpty()) Text = TEXT("[OK] Blocking Plan validation passed.");
    return Text;
}
