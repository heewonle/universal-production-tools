#pragma once

#include "CoreMinimal.h"
#include "UPTSkeletonAnalyzer.h"

class USkeletalMesh;
class UUPTSkeletonProfile;

class FUPTSkeletonProfileBuilder
{
public:
    // 메시당 Profile은 하나만 유지한다. 이미 있으면 내용을 갱신해 수동 보정값이 중복 에셋으로 흩어지지 않게 한다.
    static UUPTSkeletonProfile* CreateProfile(
        USkeletalMesh* SkeletalMesh,
        const FUPTSkeletonAnalysisData& Analysis,
        const FString& OverrideJson,
        FString& OutError);
    static UUPTSkeletonProfile* FindProfileForMesh(const USkeletalMesh* SkeletalMesh);
    static FString FindManualOverride(const USkeletalMesh* SkeletalMesh);
};
