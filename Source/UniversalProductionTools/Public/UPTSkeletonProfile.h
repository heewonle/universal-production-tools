#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UPTSkeletonProfile.generated.h"

class USkeletalMesh;

UCLASS(BlueprintType)
class UNIVERSALPRODUCTIONTOOLS_API UUPTSkeletonProfile : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Skeleton")
    TSoftObjectPtr<USkeletalMesh> SourceMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Mapping")
    TMap<FName, FName> BoneMappings;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Mapping")
    TMap<FName, float> Confidences;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Auxiliary")
    TArray<FName> TwistBones;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Auxiliary")
    TArray<FName> FingerBones;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Auxiliary")
    TArray<FName> FaceBones;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Validation", meta=(ClampMin="0.0", ClampMax="1.0"))
    float RetargetReadiness = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Validation")
    bool bOverridesValid = true;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Validation")
    FString ManualOverrideJson;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Validation")
    FString AnalyzerVersion = TEXT("2.1");
};
