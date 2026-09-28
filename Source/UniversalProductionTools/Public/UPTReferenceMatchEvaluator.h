#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"
#include "UPTReferenceTypes.h"

struct FUPTReferenceMatchResult
{
    float Score = 0.0f;
    TArray<FString> Passed;
    TArray<FString> Warnings;
};

class FUPTReferenceMatchEvaluator
{
public:
    static FUPTReferenceMatchResult Evaluate(const FUPTReferencePlan& ReferencePlan, const FUPTCinematicPlan& CinematicPlan);
    static FString ToText(const FUPTReferenceMatchResult& Result);
};
