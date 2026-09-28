#pragma once

#include "CoreMinimal.h"
#include "UPTReferenceTypes.h"

class FUPTReferencePlanParser
{
public:
    static bool Parse(const FString& Json, FUPTReferencePlan& OutPlan, FString& OutError);
    static FString ToPreviewText(const FUPTReferencePlan& Plan);
    static bool ToJson(const FUPTReferencePlan& Plan, FString& OutJson);
};
