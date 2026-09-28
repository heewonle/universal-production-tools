#pragma once

#include "CoreMinimal.h"

struct FUPTRenderMatchReport
{
    double OverallScore = 0.0;
    FString Summary;
    int32 ShotCount = 0;
};

class FUPTRenderMatchReportParser
{
public:
    static bool Parse(const FString& JsonText, FUPTRenderMatchReport& OutReport, FString& OutError);
};
