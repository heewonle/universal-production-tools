#pragma once

#include "CoreMinimal.h"

struct FUPTShotBoundary
{
    int32 FrameIndex = 0;
    float TimeSeconds = 0.0f;
    float ChangeScore = 0.0f;
};

class FUPTShotBoundaryDetector
{
public:
    static bool Detect(
        const TArray<FString>& FrameFiles,
        float SampleIntervalSeconds,
        float ChangeThreshold,
        float MinimumShotSeconds,
        TArray<FUPTShotBoundary>& OutBoundaries,
        FString& OutError);
};
