#pragma once

#include "CoreMinimal.h"

struct FUPTReferenceSubject
{
    FString Role;
    FVector2D ScreenPosition = FVector2D(0.5f, 0.5f);
    FVector2D ScreenSize = FVector2D(0.25f, 0.5f);
    int32 DepthOrder = 0;
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
