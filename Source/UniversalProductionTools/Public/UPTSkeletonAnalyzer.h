#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

struct FUPTSkeletonAnalysisData
{
    FString Report;
    TMap<FString, FName> BoneMappings;
    TMap<FString, float> Confidences;
    // 이름 근거 없이 위치·계층만으로 배정된 역할. 부분 메시(망토·머리 부착물)에서는
    // 남은 아무 본이나 집어 오므로(LeftFoot ← jaw) 결과를 그대로 믿으면 안 된다.
    TSet<FString> ForcedRoles;
    TArray<FName> TwistBones;
    TArray<FName> FingerBones;
    TArray<FName> FaceBones;
    float Readiness = 0.0f;
    bool bOverridesValid = true;
};

class FUPTSkeletonAnalyzer
{
public:
    static FString Analyze(USkeletalMesh* SkeletalMesh);
    static FString Analyze(USkeletalMesh* SkeletalMesh, const FString& OverrideJson);
    static FUPTSkeletonAnalysisData AnalyzeDetailed(USkeletalMesh* SkeletalMesh, const FString& OverrideJson);
};
