#include "UPTReferenceMatchEvaluator.h"

FUPTReferenceMatchResult FUPTReferenceMatchEvaluator::Evaluate(const FUPTReferencePlan& ReferencePlan, const FUPTCinematicPlan& CinematicPlan)
{
    FUPTReferenceMatchResult Result;
    float Earned = 0.0f;
    float Available = 0.0f;
    auto Check = [&Result, &Earned, &Available](const bool bPassed, const float Weight, const FString& PassText, const FString& WarningText)
    {
        Available += Weight;
        if (bPassed) { Earned += Weight; Result.Passed.Add(PassText); }
        else Result.Warnings.Add(WarningText);
    };

    Check(!ReferencePlan.Shots.IsEmpty() && ReferencePlan.Shots.Num() == CinematicPlan.Shots.Num(), 20.0f,
        TEXT("Shot 개수가 일치합니다."), TEXT("Shot 개수가 레퍼런스와 다릅니다."));
    Check(FMath::IsNearlyEqual(ReferencePlan.AspectRatio, CinematicPlan.AspectRatio, 0.01f), 15.0f,
        TEXT("화면비가 보존됐습니다."), TEXT("Filmback 화면비가 레퍼런스와 다릅니다."));

    const int32 Count = FMath::Min(ReferencePlan.Shots.Num(), CinematicPlan.Shots.Num());
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FUPTReferenceShot& ReferenceShot = ReferencePlan.Shots[Index];
        const FUPTCinematicShot& Shot = CinematicPlan.Shots[Index];
        const FString Prefix = FString::Printf(TEXT("Shot %02d"), Index + 1);
        const float ReferenceDuration = ReferenceShot.EndSeconds - ReferenceShot.StartSeconds;
        Check(FMath::IsNearlyEqual(ReferenceDuration, Shot.DurationSeconds, 0.05f), 10.0f / FMath::Max(1, Count),
            Prefix + TEXT(": 길이 보존"), Prefix + TEXT(": 길이 불일치"));
        Check(ReferenceShot.CameraMotion.IsEmpty() || ReferenceShot.CameraMotion == Shot.CameraMotion, 15.0f / FMath::Max(1, Count),
            Prefix + TEXT(": 카메라 모션 보존"), Prefix + TEXT(": 카메라 모션 누락"));
        Check(ReferenceShot.SubjectRole.IsEmpty() || !Shot.Subject.IsEmpty(), 15.0f / FMath::Max(1, Count),
            Prefix + TEXT(": 주 피사체 매핑"), Prefix + TEXT(": 주 피사체 역할이 Actor에 매핑되지 않음"));
        if (ReferenceShot.Subjects.Num() > 1)
        {
            Check(Shot.ActorPlacements.Num() >= ReferenceShot.Subjects.Num(), 20.0f / FMath::Max(1, Count),
                Prefix + TEXT(": 복수 인물 배치 보존"), Prefix + TEXT(": 복수 인물 배치 일부 누락"));
        }
    }

    float ConfidenceSum = 0.0f;
    for (const FUPTReferenceShot& Shot : ReferencePlan.Shots) ConfidenceSum += Shot.Confidence;
    const float AverageConfidence = ReferencePlan.Shots.IsEmpty() ? 0.0f : ConfidenceSum / ReferencePlan.Shots.Num();
    Check(AverageConfidence >= 0.55f, 15.0f, FString::Printf(TEXT("Vision 평균 신뢰도 %.0f%%"), AverageConfidence * 100.0f),
        FString::Printf(TEXT("Vision 평균 신뢰도가 낮습니다(%.0f%%). 수동 검토가 필요합니다."), AverageConfidence * 100.0f));
    Result.Score = Available > 0.0f ? FMath::Clamp(Earned / Available * 100.0f, 0.0f, 100.0f) : 0.0f;
    return Result;
}

FString FUPTReferenceMatchEvaluator::ToText(const FUPTReferenceMatchResult& Result)
{
    FString Text = FString::Printf(TEXT("Reference Match Score: %.0f / 100\n\n"), Result.Score);
    for (const FString& Item : Result.Passed) Text += TEXT("[OK] ") + Item + TEXT("\n");
    for (const FString& Item : Result.Warnings) Text += TEXT("[WARN] ") + Item + TEXT("\n");
    if (Result.Score < 70.0f) Text += TEXT("\n자동 생성 전 역할 매핑과 Shot 설정을 수정하는 것을 권장합니다.");
    return Text;
}
