#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

struct FUPTSkeletonAnalysisData
{
    FString Report;
    TMap<FString, FName> BoneMappings;
    TMap<FString, float> Confidences;
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
