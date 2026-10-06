#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

class AActor;
class UWorld;

// 카메라 프레이밍 공통 규약 (SequenceBuilder, CompositionAnalyzer, Shot 카메라 보존이 모두 이 규약을 사용한다)
// - distance_cm: 피사체 정면(+Forward) 방향으로 카메라를 배치한다.
// - side_cm: 피사체 기준 오른쪽(+Right)으로 이동한다. 카메라 화면에서는 왼쪽에 해당한다.
// - height_cm: 초점 지점 기준 위쪽으로 이동한다.
// - focus_height: 피사체 Bounds 높이 비율(0=발, 1=머리 끝)에서 조준할 지점.
// - subject_screen_x/y: LookAt 초점 지점이 놓일 화면 정규화 좌표(0,0=좌상단).
namespace UPTFraming
{
constexpr float SensorWidthMm = 36.0f;

float GetSensorHeightMm(float AspectRatio);
float GetFieldOfViewDegrees(float SensorSizeMm, float FocalLengthMm);

/**
 * 화면에서 잰 이동 비율을 카메라 회전각(도)으로 되돌린다.
 *
 * 정지한 배경이 화면에서 ScreenDelta(프레임 폭/높이 비율)만큼 움직였다면 회전각 α는
 * tan α = ScreenDelta · 2 · tan(화각/2) 이다. 레퍼런스 카메라의 화각은 몰라도 되고,
 * **같은 화면 이동을 만드는** 우리 카메라의 화각만 있으면 된다.
 *
 * Steps는 측정 구간 수다. 측정값은 구간별 이동량의 **합**이고 tan은 볼록하므로
 * Σtan(αᵢ) < tan(Σαᵢ)다. 한 번에 환산하면 정답 18° 패닝이 15.96°로 나왔다(-11%).
 * 구간마다 환산해 더하면 16.37°(-9%)가 된다.
 */
float ScreenDeltaToRotationDegrees(float ScreenDelta, float FocalLengthMm, float SensorSizeMm, int32 Steps);
FBox GetSubjectBounds(const AActor* Actor);
FVector GetFocusPoint(const AActor* Actor, float HeightRatio);
void GetSubjectBasis(const AActor* Actor, FVector& OutForward, FVector& OutRight);
FVector ComputeDesiredCameraLocation(const AActor* Subject, float HeightRatio, float DistanceCm, float SideCm, float HeightCm);
FRotator ComputeFramingRotation(const FVector& CameraLocation, const FVector& TargetPoint, float ScreenX, float ScreenY,
    float FocalLength, float AspectRatio, float RollDegrees);
bool ProjectToScreen(const FVector& CameraLocation, const FRotator& CameraRotation, const FVector& WorldPoint,
    float FocalLength, float AspectRatio, FVector2D& OutScreen);
void DecomposeCameraOffset(const AActor* Subject, float HeightRatio, const FVector& CameraLocation,
    float& OutDistanceCm, float& OutSideCm, float& OutHeightCm);
void ResolveShotCamera(UWorld* World, const FUPTCinematicShot& Shot, float AspectRatio,
    const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors, FVector& OutLocation, FRotator& OutRotation);
}
