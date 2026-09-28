#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserDelegates.h"
#include "Widgets/SCompoundWidget.h"

class USkeletalMesh;
class UIKRetargeter;
class SWindow;

class SUPTAnimationRetargetWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUPTAnimationRetargetWindow) {}
        SLATE_ARGUMENT(USkeletalMesh*, TargetMesh)
        SLATE_ARGUMENT(TSharedPtr<SWindow>, OwnerWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply RetargetSelectedAnimations();
    FReply OpenComparisonRetargeters();
    FReply ValidatePipeline();
    FReply OpenGeneratedAssets();
    FText GetSelectionSummary() const;
    void SetStatus(const FString& Message, const FLinearColor& Color);

    TWeakObjectPtr<USkeletalMesh> TargetMesh;
    TWeakPtr<SWindow> OwnerWindow;
    TArray<TWeakObjectPtr<UIKRetargeter>> LastRetargeters;
    TArray<TWeakObjectPtr<UObject>> LastGeneratedAssets;
    TArray<FString> LastSourceAnimationNames;
    FGetCurrentSelectionDelegate GetCurrentSelectionDelegate;
    TSharedPtr<class STextBlock> SelectionSummaryText;
    TSharedPtr<class STextBlock> StatusText;
};
