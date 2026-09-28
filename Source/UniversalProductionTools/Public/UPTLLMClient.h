#pragma once

#include "CoreMinimal.h"
#include "UPTReferenceVideoProcessor.h"

DECLARE_DELEGATE_TwoParams(FUPTLLMComplete, bool, const FString&);

class FUPTLLMClient : public TSharedFromThis<FUPTLLMClient>
{
public:
    void GeneratePlan(const FString& Script, const FString& SceneContext, const FString& AssetContext, FUPTLLMComplete Completion);
    void AnalyzeReferenceFrames(const FUPTReferenceFrameResult& Frames, const FString& MatchMode, FUPTLLMComplete Completion);
    bool IsRequestInFlight() const { return bRequestInFlight; }

private:
    void SendRequest(const FString& Url, const FString& ApiKey, const TSharedRef<class FJsonObject>& Body, bool bChatCompletions,
        const FString& ServiceLabel, FUPTLLMComplete Completion);

    bool bRequestInFlight = false;
};
