#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

struct FUPTBlockingValidationResult
{
    TArray<FString> Errors;
    TArray<FString> Warnings;
    bool IsValid() const { return Errors.IsEmpty(); }
    FString ToText() const;
};

class FUPTBlockingValidator
{
public:
    static FUPTBlockingValidationResult Validate(const FUPTCinematicPlan& Plan);
};
