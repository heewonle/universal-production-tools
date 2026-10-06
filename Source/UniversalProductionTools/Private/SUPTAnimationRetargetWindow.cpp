#include "SUPTAnimationRetargetWindow.h"

#include "UPTIKRetargeterBuilder.h"
#include "UPTIKRigBuilder.h"
#include "UPTPelvisMotionBaker.h"
#include "UPTSkeletonAnalyzer.h"
#include "UPTSettings.h"
#include "UPTSkeletonProfileBuilder.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "IContentBrowserSingleton.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UPTAnimationRetargetWindow"

namespace
{
const TCHAR* ManualFixHint = TEXT("Universal Production Tools > 애니메이션 범용화 탭 > '고급: 본 매핑 수동 보정'에서 이 메시를 보정하세요.");

USkeletalMesh* ResolveSourceMesh(USkeleton* Skeleton)
{
    if (!Skeleton) return nullptr;
    if (USkeletalMesh* PreviewMesh = Skeleton->GetPreviewMesh(true)) return PreviewMesh;

    // 일부 외부 애니메이션 팩은 Skeleton Preview Mesh가 비어 있다. 동일 Skeleton을 쓰는 메시를 역검색한다.
    FARFilter Filter;
    Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    TArray<FAssetData> MeshAssets;
    FAssetRegistryModule::GetRegistry().GetAssets(Filter, MeshAssets);
    for (const FAssetData& MeshAsset : MeshAssets)
    {
        USkeletalMesh* Candidate = Cast<USkeletalMesh>(MeshAsset.GetAsset());
        if (Candidate && Candidate->GetSkeleton() == Skeleton) return Candidate;
    }
    return nullptr;
}

// 엔진의 DuplicateAndRetarget 편의 함수는 출력 폴더를 받지 않아 결과가 FNameDuplicationRule의
// 기본값인 /Game 루트에 떨어진다. 설정의 폴더로 떨어지도록 컨텍스트를 직접 채워 돌린다.
TArray<FAssetData> RetargetIntoSettingsFolder(
    const TArray<FAssetData>& AssetsToRetarget, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh,
    UIKRetargeter* Retargeter, const FString& Suffix)
{
    FIKRetargetBatchOperationContext Context;
    for (const FAssetData& Asset : AssetsToRetarget)
    {
        if (UObject* Object = Asset.GetAsset()) Context.AssetsToRetarget.Add(Object);
    }
    Context.SourceMesh = SourceMesh;
    Context.TargetMesh = TargetMesh;
    Context.IKRetargetAsset = Retargeter;
    Context.NameRule.Suffix = Suffix;
    Context.bIncludeReferencedAssets = false;
    // 덮어쓰지 않으면 다시 돌릴 때마다 `..._1`, `..._2`가 쌓인다. 같은 조합의 결과는 하나만 둔다.
    Context.bOverwriteExistingFiles = true;

    FString Folder = GetDefault<UUPTSettings>()->DefaultRetargetedAnimationPath;
    if (!Folder.IsEmpty()) Context.NameRule.FolderPath = Folder;

    UIKRetargetBatchOperation* BatchOperation = NewObject<UIKRetargetBatchOperation>();
    BatchOperation->AddToRoot();
    BatchOperation->RunRetarget(Context);
    BatchOperation->RemoveFromRoot();

    // 덮어쓰기라 이름이 `<원본 이름><접미사>`로 정해진다. 폴더를 비교하는 대신 바로 찾는다
    // (전에는 생성 전후를 비교해 찾았는데, 그 결과 배열의 순서가 입력과 달라 엉뚱한 쌍이 manifest에 들어갔다).
    TArray<FAssetData> Results;
    for (const FAssetData& SourceAsset : AssetsToRetarget)
    {
        const FString Expected = SourceAsset.AssetName.ToString() + Suffix;
        if (UObject* Created = LoadObject<UObject>(nullptr, *(Context.NameRule.FolderPath / Expected + TEXT(".") + Expected)))
        {
            Results.Add(FAssetData(Created));
        }
    }

    // 엔진 Pelvis Motion op이 아예 돌지 않는 조합이 있다(펠비스 이동 키가 세 축 모두 0으로 나온다).
    // 그 경우에만 같은 계산을 직접 해서 채워 넣는다. 이름으로 원본-결과를 짝짓는다.
    for (const FAssetData& SourceAsset : AssetsToRetarget)
    {
        UAnimSequence* SourceSequence = Cast<UAnimSequence>(SourceAsset.GetAsset());
        if (!SourceSequence) continue;
        const FString SourceName = SourceAsset.AssetName.ToString();
        const FAssetData* Match = Results.FindByPredicate([&SourceName](const FAssetData& Candidate)
        {
            return Candidate.AssetName.ToString().StartsWith(SourceName, ESearchCase::CaseSensitive);
        });
        if (!Match) continue;
        if (UAnimSequence* TargetSequence = Cast<UAnimSequence>(Match->GetAsset()))
        {
            FUPTPelvisMotionBaker::BakeIfMissing(SourceSequence, SourceMesh, TargetSequence, TargetMesh);
        }
    }
    return Results;
}

// 같은 메시로 '엔진 Characterize Rig'과 '자체 분석 Rig'을 비교하기 위한 스위치.
// 메시가 다르면 비교가 오염되므로, 같은 메시에 폴백 경로를 강제할 수단이 필요하다.
static TAutoConsoleVariable<int32> CVarForceFallbackRig(
    TEXT("UPT.ForceFallbackRig"), 0,
    TEXT("1이면 엔진 Auto Characterizer가 성공해도 자체 분석 Rig을 쓴다(비교용)."));

bool BuildRetargeter(USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UIKRetargeter*& OutRetargeter, FString& OutError)
{
    if (!SourceMesh || !TargetMesh) { OutError = TEXT("Source 또는 Target Skeletal Mesh가 없습니다."); return false; }

    const FString SourceOverride = FUPTSkeletonProfileBuilder::FindManualOverride(SourceMesh);
    const FString TargetOverride = FUPTSkeletonProfileBuilder::FindManualOverride(TargetMesh);
    const bool bHasManualOverride = !SourceOverride.IsEmpty() || !TargetOverride.IsEmpty()
        || CVarForceFallbackRig.GetValueOnAnyThread() != 0;

    // 알려진 UE/MetaHuman/Mixamo 휴머노이드는 엔진 Auto Characterizer가 가장 안정적이다. 사용자가 저장한 수동 보정값이 있으면 그것을 우선한다.
    FString AutoSourceError = TEXT("수동 보정값 우선");
    FString AutoTargetError = AutoSourceError;
    if (!bHasManualOverride)
    {
        AutoSourceError.Reset();
        AutoTargetError.Reset();
        UIKRigDefinition* AutoSourceRig = FUPTIKRigBuilder::CreateUniversalIKRig(SourceMesh, AutoSourceError);
        UIKRigDefinition* AutoTargetRig = FUPTIKRigBuilder::CreateUniversalIKRig(TargetMesh, AutoTargetError);
        if (AutoSourceRig && AutoTargetRig)
        {
            OutRetargeter = FUPTIKRetargeterBuilder::CreateIKRetargeter(SourceMesh, AutoSourceRig, TargetMesh, AutoTargetRig, OutError);
            if (OutRetargeter) return true;
        }
    }

    // 알려지지 않은 구조나 수동 보정된 메시는 이름+계층+공간 Semantic Analyzer로 처리한다.
    const FUPTSkeletonAnalysisData SourceAnalysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, SourceOverride);
    const FUPTSkeletonAnalysisData TargetAnalysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(TargetMesh, TargetOverride);
    if (!SourceAnalysis.bOverridesValid || SourceAnalysis.Readiness < 0.70f)
    {
        OutError = FString::Printf(TEXT("원본 '%s' 본 구조 인식 실패 (엔진 인식: %s / 보조 분석 준비도 %.0f%%). %s"),
            *SourceMesh->GetName(), *AutoSourceError, SourceAnalysis.Readiness * 100.0f, ManualFixHint);
        return false;
    }
    if (!TargetAnalysis.bOverridesValid || TargetAnalysis.Readiness < 0.70f)
    {
        OutError = FString::Printf(TEXT("대상 '%s' 본 구조 인식 실패 (엔진 인식: %s / 보조 분석 준비도 %.0f%%). %s"),
            *TargetMesh->GetName(), *AutoTargetError, TargetAnalysis.Readiness * 100.0f, ManualFixHint);
        return false;
    }

    // 쪽마다 더 나은 Rig을 고르는(한쪽은 엔진 Rig, 다른 쪽은 폴백 Rig) 방식도 만들어 재 봤는데
    // 전면적으로 나빠졌다(골렘 5.6° → 31.0°, X_Bot 3.8° → 31.1°). 두 Rig의 체인 **이름 규칙**이
    // 달라져 AutoMapChains(Exact)가 거의 아무것도 짝짓지 못하기 때문이다.
    // 한쪽이 폴백이면 양쪽 다 폴백으로 가는 지금 구조가 맞다.
    UUPTSkeletonProfile* SourceProfile = FUPTSkeletonProfileBuilder::CreateProfile(SourceMesh, SourceAnalysis, SourceOverride, OutError);
    if (!SourceProfile) return false;
    UIKRigDefinition* SourceRig = FUPTIKRigBuilder::CreateIKRig(SourceProfile, OutError);
    if (!SourceRig) return false;
    UUPTSkeletonProfile* TargetProfile = FUPTSkeletonProfileBuilder::CreateProfile(TargetMesh, TargetAnalysis, TargetOverride, OutError);
    if (!TargetProfile) return false;
    UIKRigDefinition* TargetRig = FUPTIKRigBuilder::CreateIKRig(TargetProfile, OutError);
    if (!TargetRig) return false;

    OutRetargeter = FUPTIKRetargeterBuilder::CreateIKRetargeter(SourceProfile, SourceRig, TargetProfile, TargetRig, OutError);
    return OutRetargeter != nullptr;
}
}

void SUPTAnimationRetargetWindow::Construct(const FArguments& InArgs)
{
    TargetMesh = InArgs._TargetMesh;
    OwnerWindow = InArgs._OwnerWindow;

    FAssetPickerConfig PickerConfig;
    PickerConfig.SelectionMode = ESelectionMode::Multi;
    PickerConfig.Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
    PickerConfig.Filter.bRecursiveClasses = true;
    PickerConfig.GetCurrentSelectionDelegates.Add(&GetCurrentSelectionDelegate);
    PickerConfig.InitialAssetViewType = EAssetViewType::Column;
    PickerConfig.bAllowNullSelection = false;
    PickerConfig.OnAssetSelected = FOnAssetSelected::CreateLambda([this](const FAssetData&)
    {
        if (SelectionSummaryText) SelectionSummaryText->SetText(GetSelectionSummary());
    });

    FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    const TSharedRef<SWidget> Picker = ContentBrowser.Get().CreateAssetPicker(PickerConfig);

    ChildSlot
    [
        SNew(SBorder).Padding(10.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
                .Text(FText::Format(LOCTEXT("TargetMesh", "대상 메시: {0}"), FText::FromString(TargetMesh.IsValid() ? TargetMesh->GetName() : TEXT("없음"))))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
            [ SNew(STextBlock).Text(LOCTEXT("Guide", "① 변환할 애니메이션 선택 (Ctrl/Shift로 여러 개)  →  ② 사전 점검  →  ③ 일괄 리타기팅  →  ④ 원본/대상 비교")) ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [ Picker ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
            [ SAssignNew(SelectionSummaryText, STextBlock).Text(LOCTEXT("NoSelection", "선택된 애니메이션 없음")) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
            [ SAssignNew(StatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("Ready", "준비됨")) ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 8, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Close", "닫기")).OnClicked_Lambda([this] { if (OwnerWindow.IsValid()) OwnerWindow.Pin()->RequestDestroyWindow(); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Validate", "② 사전 점검")).OnClicked(this, &SUPTAnimationRetargetWindow::ValidatePipeline) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Retarget", "③ 선택 애니메이션 일괄 리타기팅")).OnClicked(this, &SUPTAnimationRetargetWindow::RetargetSelectedAnimations) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("OpenResults", "생성 결과 열기")).IsEnabled_Lambda([this] { return !LastGeneratedAssets.IsEmpty(); }).OnClicked(this, &SUPTAnimationRetargetWindow::OpenGeneratedAssets) ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Compare", "④ 원본/대상 비교"))
                    .ToolTipText(LOCTEXT("CompareTooltip", "마지막 리타기팅에 사용한 IK Retargeter를 열어 원본과 대상 동작을 나란히 비교합니다."))
                    .IsEnabled_Lambda([this] { return !LastRetargeters.IsEmpty(); })
                    .OnClicked(this, &SUPTAnimationRetargetWindow::OpenComparisonRetargeters)
                ]
            ]
        ]
    ];
}

FText SUPTAnimationRetargetWindow::GetSelectionSummary() const
{
    const int32 Count = GetCurrentSelectionDelegate.IsBound() ? GetCurrentSelectionDelegate.Execute().Num() : 0;
    return Count > 0 ? FText::Format(LOCTEXT("SelectionCount", "{0}개 애니메이션 선택됨"), Count) : LOCTEXT("NoSelection2", "선택된 애니메이션 없음");
}

FReply SUPTAnimationRetargetWindow::ValidatePipeline()
{
    USkeletalMesh* Target = TargetMesh.Get();
    if (!Target || !Target->GetSkeleton())
    { SetStatus(TEXT("점검 실패: 대상 Skeletal Mesh 또는 Skeleton이 없습니다."), FLinearColor::Red); return FReply::Handled(); }
    if (!GetCurrentSelectionDelegate.IsBound() || GetCurrentSelectionDelegate.Execute().IsEmpty())
    { SetStatus(TEXT("점검 실패: 변환할 Animation Sequence를 하나 이상 선택하세요."), FLinearColor::Yellow); return FReply::Handled(); }

    int32 DirectCount = 0;
    TSet<USkeletalMesh*> SourceMeshes;
    TArray<FString> Problems;
    for (const FAssetData& Data : GetCurrentSelectionDelegate.Execute())
    {
        UAnimationAsset* Animation = Cast<UAnimationAsset>(Data.GetAsset());
        USkeleton* Skeleton = Animation ? Animation->GetSkeleton() : nullptr;
        if (!Skeleton) { Problems.Add(Data.AssetName.ToString() + TEXT(": Skeleton 없음")); continue; }
        if (Skeleton == Target->GetSkeleton()) { ++DirectCount; continue; }
        USkeletalMesh* SourceMesh = ResolveSourceMesh(Skeleton);
        if (!SourceMesh) { Problems.Add(Data.AssetName.ToString() + TEXT(": 원본 캐릭터 메시를 찾지 못함")); continue; }
        if (SourceMeshes.Contains(SourceMesh)) continue;
        SourceMeshes.Add(SourceMesh);
        const FUPTSkeletonAnalysisData Analysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, FUPTSkeletonProfileBuilder::FindManualOverride(SourceMesh));
        if (!Analysis.bOverridesValid || Analysis.Readiness < 0.70f)
        {
            Problems.Add(FString::Printf(TEXT("%s: 보조 분석 준비도 %.0f%% — 엔진 자동 인식이 실패하면 수동 보정이 필요합니다"), *SourceMesh->GetName(), Analysis.Readiness * 100.0f));
        }
    }

    if (!Problems.IsEmpty())
    {
        SetStatus(TEXT("확인 필요: ") + FString::Join(Problems, TEXT(" | ")), FLinearColor::Yellow);
    }
    else
    {
        SetStatus(FString::Printf(TEXT("점검 통과: 그대로 사용 가능 %d개, 변환이 필요한 원본 캐릭터 %d종."), DirectCount, SourceMeshes.Num()), FLinearColor::Green);
    }
    return FReply::Handled();
}

FReply SUPTAnimationRetargetWindow::OpenGeneratedAssets()
{
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem) { SetStatus(TEXT("Asset Editor Subsystem을 사용할 수 없습니다."), FLinearColor::Red); return FReply::Handled(); }
    int32 Opened = 0;
    for (const TWeakObjectPtr<UObject>& Generated : LastGeneratedAssets)
    {
        if (UObject* Asset = Generated.Get()) { AssetEditorSubsystem->OpenEditorForAsset(Asset); ++Opened; }
    }
    SetStatus(Opened > 0 ? FString::Printf(TEXT("생성 결과 %d개를 열었습니다."), Opened) : TEXT("열 수 있는 생성 결과가 없습니다."), Opened > 0 ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

void SUPTAnimationRetargetWindow::SetStatus(const FString& Message, const FLinearColor& Color)
{
    if (StatusText) { StatusText->SetText(FText::FromString(Message)); StatusText->SetColorAndOpacity(Color); }
}

FReply SUPTAnimationRetargetWindow::RetargetSelectedAnimations()
{
    USkeletalMesh* Target = TargetMesh.Get();
    if (!Target || !GetCurrentSelectionDelegate.IsBound()) { SetStatus(TEXT("대상 메시 또는 애니메이션 선택기를 사용할 수 없습니다."), FLinearColor::Red); return FReply::Handled(); }

    const TArray<FAssetData> Selection = GetCurrentSelectionDelegate.Execute();
    if (Selection.IsEmpty()) { SetStatus(TEXT("변환할 Animation Sequence를 하나 이상 선택하세요."), FLinearColor::Yellow); return FReply::Handled(); }

    LastRetargeters.Reset();
    LastGeneratedAssets.Reset();
    LastSourceAnimationNames.Reset();
    TMap<USkeletalMesh*, TArray<FAssetData>> AssetsBySourceMesh;
    TArray<FString> Skipped;
    int32 DirectlyCompatible = 0;
    for (const FAssetData& AssetData : Selection)
    {
        UAnimationAsset* Animation = Cast<UAnimationAsset>(AssetData.GetAsset());
        USkeleton* Skeleton = Animation ? Animation->GetSkeleton() : nullptr;
        if (Skeleton && Skeleton == Target->GetSkeleton()) { ++DirectlyCompatible; continue; }
        USkeletalMesh* SourceMesh = ResolveSourceMesh(Skeleton);
        if (!SourceMesh) { Skipped.Add(AssetData.AssetName.ToString()); continue; }
        AssetsBySourceMesh.FindOrAdd(SourceMesh).Add(AssetData);
    }

    int32 CreatedCount = 0;
    TArray<FString> Errors;
    for (const TPair<USkeletalMesh*, TArray<FAssetData>>& Pair : AssetsBySourceMesh)
    {
        UIKRetargeter* Retargeter = nullptr;
        FString Error;
        if (!BuildRetargeter(Pair.Key, Target, Retargeter, Error)) { Errors.Add(Error); continue; }
        LastRetargeters.Add(Retargeter);
        for (const FAssetData& SourceAsset : Pair.Value) LastSourceAnimationNames.Add(SourceAsset.AssetName.ToString());

        const FString Suffix = TEXT("_") + Target->GetName();
        const TArray<FAssetData> Results = RetargetIntoSettingsFolder(Pair.Value, Pair.Key, Target, Retargeter, Suffix);
        CreatedCount += Results.Num();
        for (const FAssetData& Result : Results) if (UObject* Asset = Result.GetAsset()) LastGeneratedAssets.Add(Asset);
    }

    FString Summary = FString::Printf(TEXT("'%s'용 %d개 생성, 그대로 사용 가능 %d개"), *Target->GetName(), CreatedCount, DirectlyCompatible);
    if (!Skipped.IsEmpty()) Summary += FString::Printf(TEXT(", 원본 메시를 찾지 못해 건너뜀 %d개"), Skipped.Num());
    if (!Errors.IsEmpty())
    {
        SetStatus(Summary + TEXT("\n실패: ") + FString::Join(Errors, TEXT("\n")), FLinearColor::Red);
    }
    else
    {
        SetStatus(TEXT("완료: ") + Summary + TEXT(". '④ 원본/대상 비교'로 동작을 확인하세요."), FLinearColor::Green);
    }
    return FReply::Handled();
}

FReply SUPTAnimationRetargetWindow::OpenComparisonRetargeters()
{
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem)
    {
        SetStatus(TEXT("Asset Editor Subsystem을 사용할 수 없습니다."), FLinearColor::Red);
        return FReply::Handled();
    }

    int32 OpenedCount = 0;
    for (const TWeakObjectPtr<UIKRetargeter>& Retargeter : LastRetargeters)
    {
        if (UIKRetargeter* Asset = Retargeter.Get())
        {
            AssetEditorSubsystem->OpenEditorForAsset(Asset);
            ++OpenedCount;
        }
    }

    if (OpenedCount == 0)
    {
        SetStatus(TEXT("비교할 결과가 없습니다. 먼저 일괄 리타기팅을 실행하세요."), FLinearColor::Yellow);
    }
    else
    {
        SetStatus(FString::Printf(TEXT("비교 창 %d개를 열었습니다. IK Retargeter의 Asset Browser에서 원본 애니메이션(%s)을 재생하세요."),
            OpenedCount, *FString::Join(LastSourceAnimationNames, TEXT(", "))), FLinearColor::Green);
    }
    return FReply::Handled();
}

namespace
{
DEFINE_LOG_CATEGORY_STATIC(LogUPTRetarget, Log, All);

// 패널을 거치지 않고 같은 리타기팅 경로를 돌린다. 결과 품질을 스크립트로 재기 위한 진입점이다.
// 사용: UPT.RetargetE2E <대상 메시 경로> <애니메이션 경로...> [-out=결과.json]
void RunRetargetE2E(const TArray<FString>& Args, UWorld* World)
{
    if (Args.Num() < 2)
    {
        UE_LOG(LogUPTRetarget, Error, TEXT("UPT_RETARGET 사용법: UPT.RetargetE2E <대상 메시> <애니메이션...> [-out=경로]"));
        return;
    }

    USkeletalMesh* Target = LoadObject<USkeletalMesh>(nullptr, *Args[0]);
    if (!Target)
    {
        UE_LOG(LogUPTRetarget, Error, TEXT("UPT_RETARGET 대상 메시를 열지 못했습니다: %s"), *Args[0]);
        return;
    }

    FString OutputFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("retarget_manifest.json"));
    TArray<FAssetData> Selection;
    for (int32 Index = 1; Index < Args.Num(); ++Index)
    {
        if (Args[Index].StartsWith(TEXT("-out="))) { OutputFile = Args[Index].RightChop(5); continue; }
        UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *Args[Index]);
        if (!Animation) { UE_LOG(LogUPTRetarget, Warning, TEXT("UPT_RETARGET 애니메이션을 열지 못했습니다: %s"), *Args[Index]); continue; }
        Selection.Add(FAssetData(Animation));
    }
    if (Selection.IsEmpty())
    {
        UE_LOG(LogUPTRetarget, Error, TEXT("UPT_RETARGET 변환할 애니메이션이 없습니다."));
        return;
    }

    TMap<USkeletalMesh*, TArray<FAssetData>> AssetsBySourceMesh;
    for (const FAssetData& AssetData : Selection)
    {
        UAnimationAsset* Animation = Cast<UAnimationAsset>(AssetData.GetAsset());
        USkeleton* Skeleton = Animation ? Animation->GetSkeleton() : nullptr;
        if (Skeleton && Skeleton == Target->GetSkeleton())
        {
            UE_LOG(LogUPTRetarget, Log, TEXT("UPT_RETARGET %s 는 대상과 같은 Skeleton이라 변환하지 않습니다."), *AssetData.AssetName.ToString());
            continue;
        }
        USkeletalMesh* SourceMesh = ResolveSourceMesh(Skeleton);
        if (!SourceMesh) { UE_LOG(LogUPTRetarget, Warning, TEXT("UPT_RETARGET %s 의 원본 메시를 찾지 못했습니다."), *AssetData.AssetName.ToString()); continue; }
        AssetsBySourceMesh.FindOrAdd(SourceMesh).Add(AssetData);
    }

    TArray<TSharedPtr<FJsonValue>> Pairs;
    for (const TPair<USkeletalMesh*, TArray<FAssetData>>& Group : AssetsBySourceMesh)
    {
        UIKRetargeter* Retargeter = nullptr;
        FString Error;
        if (!BuildRetargeter(Group.Key, Target, Retargeter, Error))
        {
            UE_LOG(LogUPTRetarget, Error, TEXT("UPT_RETARGET 리타기터 생성 실패: %s"), *Error);
            continue;
        }
        const FString Suffix = TEXT("_") + Target->GetName();
        const TArray<FAssetData> Results = RetargetIntoSettingsFolder(Group.Value, Group.Key, Target, Retargeter, Suffix);
        UE_LOG(LogUPTRetarget, Log, TEXT("UPT_RETARGET %s -> %s : %d개 생성"),
            *Group.Key->GetName(), *Target->GetName(), Results.Num());

        // 결과는 폴더 비교로 찾으므로 순서가 입력과 같다는 보장이 없다(이름 충돌 시 번호도 붙는다).
        // 인덱스로 짝지으면 엉뚱한 쌍이 manifest에 들어가므로 이름으로 맞춘다.
        for (const FAssetData& SourceAsset : Group.Value)
        {
            const FString SourceName = SourceAsset.AssetName.ToString();
            const FAssetData* Match = Results.FindByPredicate([&SourceName](const FAssetData& Candidate)
            {
                return Candidate.AssetName.ToString().StartsWith(SourceName, ESearchCase::CaseSensitive);
            });
            if (!Match)
            {
                UE_LOG(LogUPTRetarget, Warning, TEXT("UPT_RETARGET %s 의 결과를 찾지 못해 manifest에서 뺍니다."), *SourceName);
                continue;
            }
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("source_animation"), SourceAsset.GetObjectPathString());
            Row->SetStringField(TEXT("retargeted_animation"), Match->GetObjectPathString());
            Row->SetStringField(TEXT("source_mesh"), Group.Key->GetPathName());
            Row->SetStringField(TEXT("target_mesh"), Target->GetPathName());
            Pairs.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("pairs"), Pairs);
    FString Text;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text, *OutputFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogUPTRetarget, Log, TEXT("UPT_RETARGET_DONE %d쌍 → %s"), Pairs.Num(), *OutputFile);
}

FAutoConsoleCommandWithWorldAndArgs GUPTRetargetE2ECommand(
    TEXT("UPT.RetargetE2E"),
    TEXT("Runs the retarget pipeline without the panel: <TargetMeshPath> <AnimPath...> [-out=manifest.json]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunRetargetE2E));
}

#undef LOCTEXT_NAMESPACE
