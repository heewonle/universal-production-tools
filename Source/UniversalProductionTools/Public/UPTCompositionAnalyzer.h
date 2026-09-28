#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

class AActor;

class FUPTCompositionAnalyzer
{
public:
    static FString Analyze(
        const FUPTCinematicPlan& Plan,
        const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors);
};
