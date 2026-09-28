#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.generated.h"

USTRUCT()
struct FUPTActorBlockingPlacement
{
    GENERATED_BODY()
    UPROPERTY() FString ActorLabel;
    UPROPERTY() FVector WorldLocation = FVector::ZeroVector;
    UPROPERTY() FRotator WorldRotation = FRotator::ZeroRotator;
};

USTRUCT()
struct FUPTCinematicShot
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() FString Description;
    UPROPERTY() FString Subject;
    UPROPERTY() FString LookAt;
    UPROPERTY() FString AnimationId;
    UPROPERTY() FString AudioId;
    UPROPERTY() FString Speaker;
    UPROPERTY() FString Dialogue;
    UPROPERTY() FString LipSyncAnimationId;
    UPROPERTY() float DurationSeconds = 3.0f;
    UPROPERTY() FVector CameraLocation = FVector::ZeroVector;
    UPROPERTY() FRotator CameraRotation = FRotator::ZeroRotator;
    UPROPERTY() float FocalLength = 50.0f;
    UPROPERTY() float DistanceCm = 300.0f;
    UPROPERTY() float HeightCm = 60.0f;
    UPROPERTY() float SideCm = 0.0f;
    UPROPERTY() FString CameraMotion;
    UPROPERTY() float CameraRollDegrees = 0.0f;
    UPROPERTY() float SubjectScreenX = 0.5f;
    UPROPERTY() float SubjectScreenY = 0.5f;
    UPROPERTY() float FocusHeightRatio = 0.5f;
    UPROPERTY() TArray<FUPTActorBlockingPlacement> ActorPlacements;
    // 어깨 너머(OTS) 샷: 카메라를 이 Actor의 어깨 뒤에 둔다(빈 문자열이면 일반 샷). 샷 수정·구도 보정이 이 배치를 유지하는 데 쓴다.
    UPROPERTY() FString OverShoulderActor;
    // 앞사람이 걸치는 화면 쪽("left"/"right")과 앞사람 몸 안쪽 윤곽이 놓일 화면 가로 위치.
    UPROPERTY() FString OverShoulderSide;
    UPROPERTY() float OverShoulderEdgeX = 0.85f;
};

USTRUCT()
struct FUPTCinematicPlan
{
    GENERATED_BODY()
    UPROPERTY() FString Title;
    UPROPERTY() int32 FrameRate = 30;
    UPROPERTY() float AspectRatio = 16.0f / 9.0f;
    UPROPERTY() TArray<FUPTCinematicShot> Shots;
};

struct FUPTPlanValidationResult
{
    bool bValid = false;
    TArray<FString> Errors;
    TArray<FString> Warnings;
};
