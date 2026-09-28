#pragma once

#include "CoreMinimal.h"
#include "UPTShotBoundaryDetector.h"

struct FUPTReferenceFrameResult
{
    FString SourceVideo;
    FString OutputDirectory;
    TArray<FString> FrameFiles;
    float SampleIntervalSeconds = 1.0f;
    int32 FrameWidth = 0;
    int32 FrameHeight = 0;
    float AspectRatio = 16.0f / 9.0f;
    FString SourceFingerprint;
    TArray<FUPTShotBoundary> ShotBoundaries;
};

DECLARE_DELEGATE_ThreeParams(FUPTReferenceFramesComplete, bool, const FUPTReferenceFrameResult&, const FString&);

class FUPTReferenceVideoProcessor
{
public:
    static bool ValidateVideo(const FString& VideoPath, FString& OutError);
    static void ExtractFrames(
        const FString& VideoPath,
        float SampleIntervalSeconds,
        int32 MaxFrames,
        FUPTReferenceFramesComplete Completion);
};
