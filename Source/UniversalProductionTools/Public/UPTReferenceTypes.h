#pragma once

#include "CoreMinimal.h"

struct FUPTReferenceSubject
{
    FString Role;
    FVector2D ScreenPosition = FVector2D(0.5f, 0.5f);
    FVector2D ScreenSize = FVector2D(0.25f, 0.5f);
    int32 DepthOrder = 0;
};

/**
 * 분류한 카메라 모션의 **크기**. 레퍼런스 화면에서 재어 화면 비율로 들고 다닌다.
 *
 * 각도로 바꿔 두지 않는 이유: 레퍼런스 카메라의 화각을 모른다. 같은 화면 이동을 만들려면
 * **생성하는** 카메라의 화각으로 역산해야 하고 그 화각은 우리가 안다. 그래서 측정 단위를 유지한다.
 * 비어 있으면(bHasAmount=false) 시퀀스 빌더가 예전처럼 상수 폭을 쓴다(LLM 플랜, 구버전 JSON).
 */
struct FUPTCameraMotionAmount
{
    bool bHasAmount = false;
    float ScreenDX = 0.0f;          // 배경이 화면 가로로 이동한 비율 (+ = 오른쪽)
    float ScreenDY = 0.0f;          // 화면 세로 이동 비율 (+ = 아래)
    float BackgroundScale = 1.0f;   // 배경 배율 (줌)
    float SubjectScale = 0.0f;      // 인물 크기 비 (돌리). 0이면 추정값 없음
    float Path = 0.0f;              // 누적 이동 경로 길이 (핸드헬드 강도)
    int32 Steps = 1;                // 측정 구간 수. 각도 환산을 구간별로 나눠 더하는 데 쓴다
};

struct FUPTReferenceShot
{
    FString Name;
    FString SubjectRole;
    float StartSeconds = 0.0f;
    float EndSeconds = 0.0f;
    FString ShotSize;
    FString CameraAngle;
    FString CameraMotion;
    FUPTCameraMotionAmount MotionAmount;
    FVector2D SubjectScreenPosition = FVector2D(0.5f, 0.5f);
    FVector2D SubjectScreenSize = FVector2D(0.3f, 0.5f);
    FString Composition;
    float Confidence = 0.0f;
    TArray<FUPTReferenceSubject> Subjects;
    // 어깨 너머(OTS): 분석기가 화면 가장자리에 잘린 앞사람을 찾은 쪽("left"/"right", 없으면 빈 문자열)과 그 조각의 화면 가로 중심·폭.
    FString OverShoulderSide;
    float OverShoulderScreenX = 0.0f;
    float OverShoulderScreenWidth = 0.0f;
};

struct FUPTReferencePlan
{
    int32 SchemaVersion = 1;
    FString Title;
    FString MatchMode = TEXT("scene");
    FString VisionModel;
    FString AnalysisFingerprint;
    float AspectRatio = 16.0f / 9.0f;
    TArray<FUPTReferenceShot> Shots;
};
