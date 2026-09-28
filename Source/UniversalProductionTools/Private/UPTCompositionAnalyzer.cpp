#include "UPTCompositionAnalyzer.h"

#include "UPTFraming.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString FUPTCompositionAnalyzer::Analyze(
    const FUPTCinematicPlan& Plan,
    const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FString Report = TEXT("Composition Risk Report\n\n");
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        const TWeakObjectPtr<AActor>* SubjectPtr = SceneActors.Find(Shot.Subject);
        AActor* Subject = SubjectPtr ? SubjectPtr->Get() : nullptr;
        int32 Risk = 0;
        TArray<FString> Reasons;
        FString Metrics;
        if (!Subject)
        {
            Risk = 35;
            Reasons.Add(TEXT("absolute camera: 피사체 화면 점유율 분석 생략"));
        }
        else
        {
            // Sequence 생성과 같은 카메라 해석을 사용해 실제로 만들어질 구도를 검사한다.
            FVector CameraLocation;
            FRotator CameraRotation;
            UPTFraming::ResolveShotCamera(World, Shot, Plan.AspectRatio, SceneActors, CameraLocation, CameraRotation);
            const TWeakObjectPtr<AActor>* LookAtPtr = SceneActors.Find(Shot.LookAt);
            AActor* LookAt = LookAtPtr && LookAtPtr->IsValid() ? LookAtPtr->Get() : Subject;
            const FVector Focus = UPTFraming::GetFocusPoint(LookAt, Shot.FocusHeightRatio);

            FVector2D HeadScreen;
            FVector2D FeetScreen;
            const bool bHeadVisible = UPTFraming::ProjectToScreen(CameraLocation, CameraRotation, UPTFraming::GetFocusPoint(Subject, 1.0f), Shot.FocalLength, Plan.AspectRatio, HeadScreen);
            const bool bFeetVisible = UPTFraming::ProjectToScreen(CameraLocation, CameraRotation, UPTFraming::GetFocusPoint(Subject, 0.0f), Shot.FocalLength, Plan.AspectRatio, FeetScreen);
            if (!bHeadVisible || !bFeetVisible)
            {
                Risk += 60;
                Reasons.Add(TEXT("피사체가 카메라 뒤쪽에 있음"));
            }
            else
            {
                const float Coverage = FeetScreen.Y - HeadScreen.Y;
                Metrics = FString::Printf(TEXT("전신 세로 점유 %.0f%%, 머리 끝 y=%.2f"), Coverage * 100.0f, HeadScreen.Y);
                if (HeadScreen.Y < 0.0f && Shot.FocusHeightRatio < 0.8f) { Risk += 45; Reasons.Add(TEXT("머리가 프레임 위로 잘림")); }
                else if (HeadScreen.Y >= 0.0f && HeadScreen.Y < 0.03f) { Risk += 15; Reasons.Add(TEXT("헤드룸 부족")); }
                if (Coverage < 0.08f) { Risk += 25; Reasons.Add(TEXT("피사체가 지나치게 작음")); }
                if (HeadScreen.X < -0.05f || HeadScreen.X > 1.05f) { Risk += 30; Reasons.Add(TEXT("피사체가 화면 좌우 밖으로 벗어남")); }
            }

            if (World)
            {
                FCollisionQueryParams Params(SCENE_QUERY_STAT(UPTCompositionOcclusion), true, Subject);
                if (LookAt != Subject) Params.AddIgnoredActor(LookAt);
                FHitResult Hit;
                if (World->LineTraceSingleByChannel(Hit, CameraLocation, Focus, ECC_Visibility, Params))
                {
                    Risk += 40;
                    Reasons.Add(FString::Printf(TEXT("Visibility 차폐: %s"), Hit.GetActor() ? *Hit.GetActor()->GetActorLabel() : TEXT("geometry")));
                }
            }
            if (FMath::Abs(Shot.SideCm) > Shot.DistanceCm * 0.8f) { Risk += 15; Reasons.Add(TEXT("과도한 측면 오프셋")); }
        }
        Risk = FMath::Clamp(Risk, 0, 100);
        const TCHAR* Grade = Risk >= 60 ? TEXT("HIGH") : Risk >= 30 ? TEXT("CHECK") : TEXT("LOW");
        Report += FString::Printf(TEXT("%02d. %s | %s %d/100%s%s\n    %s\n"), Index + 1, *Shot.Name, Grade, Risk,
            Metrics.IsEmpty() ? TEXT("") : TEXT(" | "), *Metrics,
            Reasons.IsEmpty() ? TEXT("자동 검사상 특이사항 없음") : *FString::Join(Reasons, TEXT(", ")));
    }
    return Report;
}
