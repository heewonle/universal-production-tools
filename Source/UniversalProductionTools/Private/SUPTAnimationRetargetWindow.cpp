#include "SUPTAnimationRetargetWindow.h"

#include "UPTIKRetargeterBuilder.h"
#include "UPTRetargetQualityReport.h"
#include "UPTIKRigBuilder.h"
#include "UPTPelvisMotionBaker.h"
#include "UPTTwistMotionBaker.h"
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
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UPTAnimationRetargetWindow"

// 일괄 변환과 그 품질 점검이 같은 기록으로 남도록 파일 전체에서 쓰는 로그 카테고리다.
DEFINE_LOG_CATEGORY_STATIC(LogUPTRetarget, Log, All);

namespace
{
// 파일 범위의 상수로 두면 정적 초기화 때 번역이 준비돼 있지 않을 수 있다. 쓰는 시점에 만든다.
FString ManualFixHint()
{
    return LOCTEXT("ManualFixHintText", "Correct this mesh in Universal Production Tools > "
        "Universal animation retarget tab > 'Advanced: manual bone mapping'.").ToString();
}

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

/**
 * 만들어 둔 에셋을 디스크에 저장한다.
 *
 * 리타기팅 결과는 메모리에만 있고 더티 표시만 된다. 에디터 도구로서는 흔한 동작이지만,
 * **일괄 변환 50개를 만들고 저장 없이 닫으면 전부 사라진다.** 품질 리포트를 별도 세션에서
 * 돌렸을 때 35쌍을 하나도 열지 못해 알았다 — 디스크에 있었던 적이 없었다.
 *
 * 리비전 컨트롤은 건드리지 않는다(체크아웃 없이 로컬 저장만). 읽기 전용 파일은 그대로 두고
 * 실패로 센다 — 사용자의 소스 컨트롤 상태를 도구가 임의로 바꾸지 않는다.
 *
 * @return 저장한 패키지 수. OutFailed에는 저장하지 못한 에셋 이름이 담긴다.
 */
int32 SaveAssetsToDisk(const TArray<UObject*>& Assets, TArray<FString>& OutFailed)
{
    if (!GetDefault<UUPTSettings>()->bSaveGeneratedAssets) return 0;

    TSet<UPackage*> Packages;
    for (UObject* Asset : Assets)
    {
        if (UPackage* Package = Asset ? Asset->GetOutermost() : nullptr)
        {
            if (Package->IsDirty()) Packages.Add(Package);
        }
    }

    int32 Saved = 0;
    for (UPackage* Package : Packages)
    {
        const FString FileName = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError | SAVE_KeepDirty;  // 실패해도 더티를 유지해 사용자가 다시 저장할 수 있다
        const FSavePackageResultStruct Result = UPackage::Save(Package, nullptr, *FileName, Args);
        if (Result.IsSuccessful())
        {
            Package->SetDirtyFlag(false);
            ++Saved;
        }
        else
        {
            OutFailed.Add(FPackageName::GetShortName(Package->GetName()));
        }
    }
    return Saved;
}

// 엔진의 DuplicateAndRetarget 편의 함수는 출력 폴더를 받지 않아 결과가 FNameDuplicationRule의
// 기본값인 /Game 루트에 떨어진다. 설정의 폴더로 떨어지도록 컨텍스트를 직접 채워 돌린다.
TArray<FAssetData> RetargetIntoSettingsFolder(
    const TArray<FAssetData>& AssetsToRetarget, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh,
    UIKRetargeter* Retargeter, const FString& Suffix,
    // 저장 결과를 패널이 사용자에게 보여 줄 수 있도록 돌려준다. 콘솔 경로는 쓰지 않는다.
    int32* OutSavedCount = nullptr, TArray<FString>* OutSaveFailures = nullptr)
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
            // 트위스트 본은 체인 경로 밖의 가지라 FK 체인 리타기팅이 건드리지 않고 멈춘다.
            // 대상에 트위스트 본이 없으면 아무 일도 하지 않는다.
            FUPTTwistMotionBaker::Bake(SourceSequence, SourceMesh, TargetSequence, TargetMesh);
        }
    }

    // 베이커가 키를 쓴 **뒤에** 저장한다. 먼저 저장하면 베이킹 전 상태가 디스크에 남는다.
    // 리타기터와 그 IK Rig도 같이 저장한다. 이것들이 없으면 다음 세션에서 결과를 열어도
    // 'IK Retargeter 열기'로 비교할 수 없다.
    TArray<UObject*> ToSave;
    for (const FAssetData& Result : Results) if (UObject* Asset = Result.GetAsset()) ToSave.Add(Asset);
    if (Retargeter)
    {
        ToSave.Add(Retargeter);
        if (UIKRigDefinition* SourceRig = Retargeter->GetIKRigWriteable(ERetargetSourceOrTarget::Source)) ToSave.Add(SourceRig);
        if (UIKRigDefinition* TargetRig = Retargeter->GetIKRigWriteable(ERetargetSourceOrTarget::Target)) ToSave.Add(TargetRig);
    }
    TArray<FString> Failed;
    const int32 Saved = SaveAssetsToDisk(ToSave, Failed);
    if (Saved > 0 || !Failed.IsEmpty())
    {
        UE_LOG(LogUPTRetarget, Log, TEXT("UPT_RETARGET_SAVE %d개 저장, 실패 %d개%s"),
            Saved, Failed.Num(), Failed.IsEmpty() ? TEXT("") : *(TEXT(": ") + FString::Join(Failed, TEXT(", "))));
    }
    if (OutSavedCount) *OutSavedCount += Saved;
    if (OutSaveFailures) OutSaveFailures->Append(Failed);
    return Results;
}

// 같은 메시로 '엔진 Characterize Rig'과 '자체 분석 Rig'을 비교하기 위한 스위치.
// 메시가 다르면 비교가 오염되므로, 같은 메시에 폴백 경로를 강제할 수단이 필요하다.
static TAutoConsoleVariable<int32> CVarForceFallbackRig(
    TEXT("UPT.ForceFallbackRig"), 0,
    TEXT("1이면 엔진 Auto Characterizer가 성공해도 자체 분석 Rig을 쓴다(비교용)."));

bool BuildRetargeter(USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UIKRetargeter*& OutRetargeter, FString& OutError)
{
    if (!SourceMesh || !TargetMesh) { OutError = LOCTEXT("ErrNoMesh", "No source or target Skeletal Mesh.").ToString(); return false; }

    const FString SourceOverride = FUPTSkeletonProfileBuilder::FindManualOverride(SourceMesh);
    const FString TargetOverride = FUPTSkeletonProfileBuilder::FindManualOverride(TargetMesh);
    const bool bHasManualOverride = !SourceOverride.IsEmpty() || !TargetOverride.IsEmpty()
        || CVarForceFallbackRig.GetValueOnAnyThread() != 0;

    // 알려진 UE/MetaHuman/Mixamo 휴머노이드는 엔진 Auto Characterizer가 가장 안정적이다. 사용자가 저장한 수동 보정값이 있으면 그것을 우선한다.
    FString AutoSourceError = LOCTEXT("ManualOverrideFirst", "manual correction takes precedence").ToString();
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
        OutError = FString::Format(*LOCTEXT("ErrSourceUnrecognized",
            "Source '{0}': bone structure not recognized (engine: {1} / fallback readiness {2}%). {3}").ToString(),
            {SourceMesh->GetName(), AutoSourceError, FMath::RoundToInt(SourceAnalysis.Readiness * 100.0f), ManualFixHint()});
        return false;
    }
    if (!TargetAnalysis.bOverridesValid || TargetAnalysis.Readiness < 0.70f)
    {
        OutError = FString::Format(*LOCTEXT("ErrTargetUnrecognized",
            "Target '{0}': bone structure not recognized (engine: {1} / fallback readiness {2}%). {3}").ToString(),
            {TargetMesh->GetName(), AutoTargetError, FMath::RoundToInt(TargetAnalysis.Readiness * 100.0f), ManualFixHint()});
        return false;
    }

    // 준비도만으로는 사족보행을 막지 못한다. 늑대는 리거가 앞다리를 UpperArm·Forearm·Hand로
    // 이름 붙여 두어 **모든 역할이 신뢰도 1.00으로 매핑되고 준비도 94%로 통과한다.**
    // 역할 이름은 리거의 선언이고, 몸이 어떻게 서 있는지는 레퍼런스 포즈가 말해 준다.
    //
    // 막아야 하는 것은 **자세를 섞는 것**이고, 사족보행 자체가 아니다. 처음에는 "직립 이족이
    // 아니면 거부"로 넣었는데 그건 과했다 — 늑대의 역할 축(펠비스→척추→목→머리,
    // 쇄골→위팔→아래팔→손, 허벅지→정강이→발)은 사람과 **글자 그대로 같다.** 늑대 → 늑대 새끼
    // 리타기팅은 같은 자세끼리라 성립하고, 실제로 재 보면 사람 쌍과 같은 수준으로 나온다.
    // 망가지는 것은 사람 → 늑대처럼 **자세가 다른 쌍**이다.
    if (SourceAnalysis.bUprightBiped != TargetAnalysis.bUprightBiped)
    {
        const auto Describe = [](const FUPTSkeletonAnalysisData& Analysis) { return Analysis.bUprightBiped ? LOCTEXT("PostureUpright", "upright biped").ToString() : LOCTEXT("PostureQuadruped", "quadruped or non-upright").ToString(); };
        OutError = FString::Format(*LOCTEXT("ErrPostureMismatch",
            "The pair has different postures: source '{0}' is {1} (hand height {2}), target '{3}' is {4} (hand height {5}). "
            "The role names map on both sides, but when the postures differ the same role does a different job and the result breaks.").ToString(),
            {SourceMesh->GetName(), Describe(SourceAnalysis), FString::Printf(TEXT("%.2f"), SourceAnalysis.HandAboveFootRatio),
             TargetMesh->GetName(), Describe(TargetAnalysis), FString::Printf(TEXT("%.2f"), TargetAnalysis.HandAboveFootRatio)});
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
                .Text(FText::Format(LOCTEXT("TargetMesh", "Target mesh: {0}"), FText::FromString(TargetMesh.IsValid() ? TargetMesh->GetName() : TEXT("없음"))))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
            [ SNew(STextBlock).Text(LOCTEXT("Guide", "(1) Pick animations (Ctrl/Shift for several)  ->  (2) Pre-flight check  ->  (3) Batch retarget  ->  (4) Compare source and target")) ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [ Picker ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
            [ SAssignNew(SelectionSummaryText, STextBlock).Text(LOCTEXT("NoSelection", "No animation selected")) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
            [ SAssignNew(StatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("Ready", "Set up the cast and a reference video (or a script), then generate.")) ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 8, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Close", "Close")).OnClicked_Lambda([this] { if (OwnerWindow.IsValid()) OwnerWindow.Pin()->RequestDestroyWindow(); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Validate", "(2) Pre-flight check")).OnClicked(this, &SUPTAnimationRetargetWindow::ValidatePipeline) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("Retarget", "(3) Batch retarget the selected animations")).OnClicked(this, &SUPTAnimationRetargetWindow::RetargetSelectedAnimations) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("OpenResults", "Open generated assets")).IsEnabled_Lambda([this] { return !LastGeneratedAssets.IsEmpty(); }).OnClicked(this, &SUPTAnimationRetargetWindow::OpenGeneratedAssets) ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Compare", "(4) Compare source and target"))
                    .ToolTipText(LOCTEXT("CompareTooltip", "Opens the IK Retargeter used by the last retarget so you can compare source and target motion side by side."))
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
    return Count > 0 ? FText::Format(LOCTEXT("SelectionCount", "{0} animations selected"), Count) : LOCTEXT("NoSelection2", "No animation selected");
}

FReply SUPTAnimationRetargetWindow::ValidatePipeline()
{
    USkeletalMesh* Target = TargetMesh.Get();
    if (!Target || !Target->GetSkeleton())
    { SetStatus(LOCTEXT("CheckNoMesh", "Check failed: no target Skeletal Mesh or Skeleton.").ToString(), FLinearColor::Red); return FReply::Handled(); }
    if (!GetCurrentSelectionDelegate.IsBound() || GetCurrentSelectionDelegate.Execute().IsEmpty())
    { SetStatus(LOCTEXT("CheckNoSelection", "Check failed: select at least one Animation Sequence to convert.").ToString(), FLinearColor::Yellow); return FReply::Handled(); }

    int32 DirectCount = 0;
    TSet<USkeletalMesh*> SourceMeshes;
    TArray<FString> Problems;
    for (const FAssetData& Data : GetCurrentSelectionDelegate.Execute())
    {
        UAnimationAsset* Animation = Cast<UAnimationAsset>(Data.GetAsset());
        USkeleton* Skeleton = Animation ? Animation->GetSkeleton() : nullptr;
        if (!Skeleton) { Problems.Add(Data.AssetName.ToString() + LOCTEXT("CheckNoSkeleton", ": no Skeleton").ToString()); continue; }
        if (Skeleton == Target->GetSkeleton()) { ++DirectCount; continue; }
        USkeletalMesh* SourceMesh = ResolveSourceMesh(Skeleton);
        if (!SourceMesh) { Problems.Add(Data.AssetName.ToString() + LOCTEXT("CheckNoSourceMesh", ": source character mesh not found").ToString()); continue; }
        if (SourceMeshes.Contains(SourceMesh)) continue;
        SourceMeshes.Add(SourceMesh);
        const FUPTSkeletonAnalysisData Analysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, FUPTSkeletonProfileBuilder::FindManualOverride(SourceMesh));
        if (!Analysis.bOverridesValid || Analysis.Readiness < 0.70f)
        {
            Problems.Add(FString::Format(*LOCTEXT("CheckLowReadiness", "{0}: fallback readiness {1}% - a manual correction is needed if engine auto-detection fails").ToString(), {SourceMesh->GetName(), FMath::RoundToInt(Analysis.Readiness * 100.0f)}));
        }
        // 자세는 여기서 검사하지 않는다. 이 사전 검사는 **원본 메시만** 보는데, 자세는
        // 원본과 대상을 견줘야 뜻이 생긴다(사족 → 사족은 정상이다). 실제 가드는 BuildRetargeter에 있다.
    }

    if (!Problems.IsEmpty())
    {
        SetStatus(LOCTEXT("CheckNeedsAttention", "Needs attention: ").ToString() + FString::Join(Problems, TEXT(" | ")), FLinearColor::Yellow);
    }
    else
    {
        SetStatus(FString::Format(*LOCTEXT("CheckPassed", "Check passed: {0} usable as-is, {1} source characters need conversion.").ToString(), {DirectCount, SourceMeshes.Num()}), FLinearColor::Green);
    }
    return FReply::Handled();
}

FReply SUPTAnimationRetargetWindow::OpenGeneratedAssets()
{
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem) { SetStatus(LOCTEXT("NoAssetEditorSubsystem", "The Asset Editor Subsystem is not available.").ToString(), FLinearColor::Red); return FReply::Handled(); }
    int32 Opened = 0;
    for (const TWeakObjectPtr<UObject>& Generated : LastGeneratedAssets)
    {
        if (UObject* Asset = Generated.Get()) { AssetEditorSubsystem->OpenEditorForAsset(Asset); ++Opened; }
    }
    SetStatus(Opened > 0 ? FString::Format(*LOCTEXT("OpenedResults", "Opened {0} generated assets.").ToString(), {Opened}) : LOCTEXT("NoResultsToOpen", "There are no generated assets to open.").ToString(), Opened > 0 ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

void SUPTAnimationRetargetWindow::SetStatus(const FString& Message, const FLinearColor& Color)
{
    if (StatusText) { StatusText->SetText(FText::FromString(Message)); StatusText->SetColorAndOpacity(Color); }
}

FReply SUPTAnimationRetargetWindow::RetargetSelectedAnimations()
{
    USkeletalMesh* Target = TargetMesh.Get();
    if (!Target || !GetCurrentSelectionDelegate.IsBound()) { SetStatus(LOCTEXT("NoTargetOrPicker", "The target mesh or the animation picker is not available.").ToString(), FLinearColor::Red); return FReply::Handled(); }

    const TArray<FAssetData> Selection = GetCurrentSelectionDelegate.Execute();
    if (Selection.IsEmpty()) { SetStatus(LOCTEXT("SelectAtLeastOne", "Select at least one Animation Sequence to convert.").ToString(), FLinearColor::Yellow); return FReply::Handled(); }

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
    int32 SavedCount = 0;
    TArray<FString> Errors;
    TArray<FString> SaveFailures;
    TArray<FUPTRetargetQualityRow> QualityRows;
    for (const TPair<USkeletalMesh*, TArray<FAssetData>>& Pair : AssetsBySourceMesh)
    {
        UIKRetargeter* Retargeter = nullptr;
        FString Error;
        if (!BuildRetargeter(Pair.Key, Target, Retargeter, Error)) { Errors.Add(Error); continue; }
        LastRetargeters.Add(Retargeter);
        for (const FAssetData& SourceAsset : Pair.Value) LastSourceAnimationNames.Add(SourceAsset.AssetName.ToString());

        const FString Suffix = TEXT("_") + Target->GetName();
        const TArray<FAssetData> Results = RetargetIntoSettingsFolder(
            Pair.Value, Pair.Key, Target, Retargeter, Suffix, &SavedCount, &SaveFailures);
        CreatedCount += Results.Num();
        for (const FAssetData& Result : Results) if (UObject* Asset = Result.GetAsset()) LastGeneratedAssets.Add(Asset);

        // 변환한 자리에서 바로 잰다. 50개를 만들어 놓고 "눈으로 보세요"로 끝내면
        // 품질을 아는 쪽은 우리뿐이고 쓰는 쪽은 모른다.
        // 결과는 이름으로 짝짓는다 — 폴더 순서는 입력 순서와 같다는 보장이 없다(E2E와 같은 이유).
        for (const FAssetData& SourceAsset : Pair.Value)
        {
            const FString SourceName = SourceAsset.AssetName.ToString();
            const FAssetData* Match = Results.FindByPredicate([&SourceName](const FAssetData& Candidate)
            {
                return Candidate.AssetName.ToString().StartsWith(SourceName, ESearchCase::CaseSensitive);
            });
            if (!Match) continue;
            UAnimSequence* SourceAnimation = Cast<UAnimSequence>(SourceAsset.GetAsset());
            UAnimSequence* TargetAnimation = Cast<UAnimSequence>(Match->GetAsset());
            if (!SourceAnimation || !TargetAnimation) continue;
            QualityRows.Add(FUPTRetargetQualityReport::Measure(SourceAnimation, Pair.Key, TargetAnimation, Target));
        }
    }

    FString Summary = FString::Format(*LOCTEXT("RetargetSummary", "'{0}': {1} created, {2} usable as-is").ToString(), {Target->GetName(), CreatedCount, DirectlyCompatible});
    if (!Skipped.IsEmpty()) Summary += FString::Format(*LOCTEXT("RetargetSkipped", ", {0} skipped because the source mesh was not found").ToString(), {Skipped.Num()});
    // 저장했는지 아닌지를 분명히 적는다. 저장되지 않은 결과는 에디터를 닫으면 사라진다.
    if (!GetDefault<UUPTSettings>()->bSaveGeneratedAssets)
    {
        Summary += LOCTEXT("NotSaved", "\nNot saved ('Save Generated Assets' is off in the settings) - save them yourself before closing the editor.").ToString();
    }
    else if (!SaveFailures.IsEmpty())
    {
        Summary += FString::Format(*LOCTEXT("SavedWithFailures", "\nSaved {0} to disk, {1} failed: {2}").ToString(),
            {SavedCount, SaveFailures.Num(), FString::Join(SaveFailures, TEXT(", "))});
    }
    else
    {
        Summary += FString::Format(*LOCTEXT("SavedCount", "\nSaved {0} to disk").ToString(), {SavedCount});
    }
    if (!Errors.IsEmpty())
    {
        SetStatus(Summary + LOCTEXT("FailurePrefix", "\nFailed: ").ToString() + FString::Join(Errors, TEXT("\n")), FLinearColor::Red);
        return FReply::Handled();
    }

    const FString ReportPath = FUPTRetargetQualityReport::Save(QualityRows,
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools")));
    FString Quality = FUPTRetargetQualityReport::Summarize(QualityRows);

    // 검토가 필요한 것만, 사유와 함께 올린다. 전부 통과면 그 사실만 적는다.
    TArray<FString> Review;
    for (const FUPTRetargetQualityRow& Row : QualityRows)
    {
        if (!Row.bNeedsReview) continue;
        if (Review.Num() < 5) Review.Add(FString::Printf(TEXT("  · %s — %s"), *Row.Animation, *FString::Join(Row.Reasons, TEXT(", "))));
        UE_LOG(LogUPTRetarget, Warning, TEXT("UPT_QUALITY 검토 필요: %s — %s"), *Row.Animation, *FString::Join(Row.Reasons, TEXT(", ")));
    }
    if (!Review.IsEmpty())
    {
        Quality += TEXT("\n") + FString::Join(Review, TEXT("\n"));
        const int32 Hidden = QualityRows.FilterByPredicate([](const FUPTRetargetQualityRow& Row) { return Row.bNeedsReview; }).Num() - Review.Num();
        if (Hidden > 0) Quality += FString::Format(*LOCTEXT("ReviewMore", "\n  and {0} more (the full list is in the report file)").ToString(), {Hidden});
    }
    if (!ReportPath.IsEmpty()) Quality += LOCTEXT("ReportPrefix", "\nReport: ").ToString() + ReportPath;

    const bool bAllClear = !QualityRows.ContainsByPredicate([](const FUPTRetargetQualityRow& Row) { return Row.bNeedsReview; });
    SetStatus(LOCTEXT("DonePrefix", "Done: ").ToString() + Summary + TEXT("\n") + Quality, bAllClear ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

FReply SUPTAnimationRetargetWindow::OpenComparisonRetargeters()
{
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!AssetEditorSubsystem)
    {
        SetStatus(LOCTEXT("NoAssetEditorSubsystem", "The Asset Editor Subsystem is not available.").ToString(), FLinearColor::Red);
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
        SetStatus(LOCTEXT("NothingToCompare", "There is nothing to compare. Run a batch retarget first.").ToString(), FLinearColor::Yellow);
    }
    else
    {
        SetStatus(FString::Format(*LOCTEXT("OpenedComparison",
            "Opened {0} comparison windows. Play the source animation ({1}) from the IK Retargeter's Asset Browser.").ToString(),
            {OpenedCount, FString::Join(LastSourceAnimationNames, TEXT(", "))}), FLinearColor::Green);
    }
    return FReply::Handled();
}

namespace
{

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
