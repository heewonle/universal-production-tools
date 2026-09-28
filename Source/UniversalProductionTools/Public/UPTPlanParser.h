#pragma once
#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

class FUPTPlanParser
{
public:
    static bool Parse(const FString& JsonText, FUPTCinematicPlan& OutPlan, FString& OutError);
    static FUPTPlanValidationResult Validate(const FUPTCinematicPlan& Plan);
    static FString ToPreviewText(const FUPTCinematicPlan& Plan);
    static FString ToJson(const FUPTCinematicPlan& Plan);
};
