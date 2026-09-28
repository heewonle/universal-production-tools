#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"
#include "UPTReferenceTypes.h"

class AActor;

// 어깨 너머(OTS) 카메라 계산 입력(월드 좌표). Actor 없이 계산할 수 있게 위치·키만 받는다.
struct FUPTOverShoulderSetup
{
    FVector AnchorFocus = FVector::ZeroVector;    // 주 피사체 조준점
    FVector AnchorHead = FVector::ZeroVector;     // 가림 검사용 주 피사체 머리
    FVector ForegroundBase = FVector::ZeroVector; // 앞사람 발 중심
    float ForegroundHeightCm = 180.0f;
    bool bForegroundOnRight = true;               // 앞사람이 화면 오른쪽에 걸친다
    float ForegroundEdgeX = 0.85f;                // 앞사람 몸 안쪽 윤곽이 놓일 화면 가로 위치
    float SubjectScreenX = 0.5f;
    float SubjectScreenY = 0.45f;
    float BaseFocalLength = 50.0f;                // BaseDistanceCm 거리에서 원하는 샷 크기가 되는 렌즈
    float BaseDistanceCm = 300.0f;
    float AspectRatio = 16.0f / 9.0f;
};

class FUPTReferenceBlockingSolver
{
public:
    // 어깨 너머(OTS): 앞사람 어깨 뒤에서 주 피사체를 가리지 않고 보는 카메라 위치와, 주 피사체 크기를 유지하는 렌즈를 찾는다.
    static bool SolveOverShoulderCamera(const FUPTOverShoulderSetup& Setup, FVector& OutCamera, float& OutFocalLength);
    // Shot.Subject를 주 피사체, Shot.OverShoulderActor를 앞사람으로 보고 거리·측면·높이·렌즈를 덮어쓴다(이 샷에서 옮겨 둔 앞사람 위치 반영).
    // 실패하면 Shot을 바꾸지 않고 OutNote에 이유를 남긴다.
    static bool ApplyOverShoulder(FUPTCinematicShot& Shot, const AActor* Anchor, const AActor* Foreground, float AspectRatio, FString& OutNote);

    static bool BuildPlan(const FUPTReferencePlan& ReferencePlan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
        const TMap<FString, FString>& ExplicitRoleMappings, FUPTCinematicPlan& OutPlan, FString& OutReport);

    // 샷 크기·앵글 표로 렌즈, 조준 높이, 카메라 거리·높이·롤을 정한다(측면 오프셋은 0으로 초기화).
    // 레퍼런스 블로킹과 패널의 '선택한 샷 수정'이 같은 식을 쓰도록 한곳에 둔다.
    // ScreenHeightFraction: 샷 크기가 뜻하는 신체 부분(클로즈업이면 머리~가슴)이 화면 세로에서 차지하는 비율.
    static void ApplyShotFraming(FUPTCinematicShot& Shot, const FString& ShotSize, const FString& CameraAngle, float SubjectHeightCm,
        float ScreenHeightFraction, float AspectRatio);
    // 앵글과 카메라까지의 수평 거리로 조준점 대비 카메라 높이를 구한다(dutch·eye는 0).
    static float ComputeCameraHeight(const FString& CameraAngle, float HorizontalDistanceCm, float SubjectHeightCm, float FocusHeightRatio);
    // 샷 크기 프리셋을 직접 고를 때 쓰는 기본 화면 점유율.
    static float DefaultScreenHeightFraction(const FString& ShotSize);
    // 이미 계산된 샷 값에서 샷 크기·앵글 이름을 거꾸로 추정한다(샷 수정 화면의 초기값).
    static FString InferShotSize(const FUPTCinematicShot& Shot);
    static FString InferCameraAngle(const FUPTCinematicShot& Shot);
};
