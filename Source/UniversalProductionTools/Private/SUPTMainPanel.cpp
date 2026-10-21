#include "SUPTMainPanel.h"

#include "UniversalProductionToolsModule.h"
#include "UPTBlockingValidator.h"
#include "UPTCompositionAnalyzer.h"
#include "UPTEndpoint.h"
#include "UPTFraming.h"
#include "UPTLLMClient.h"
#include "UPTPlanParser.h"
#include "UPTPoseReferenceAnalyzer.h"
#include "UPTReferenceBlockingSolver.h"
#include "UPTReferencePlanParser.h"
#include "UPTSequenceBuilder.h"
#include "UPTSettings.h"
#include "UPTSkeletonProfile.h"
#include "UPTSkeletonProfileBuilder.h"

#include "Animation/AnimSequenceBase.h"
#include "Camera/CameraActor.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "IDesktopPlatform.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Misc/FileHelper.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlayer.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SUPTMainPanel"

DEFINE_LOG_CATEGORY_STATIC(LogUPTPanel, Log, All);

namespace
{
const FName ShotColumnIndex(TEXT("Index"));
const FName ShotColumnName(TEXT("Name"));
const FName ShotColumnDuration(TEXT("Duration"));
const FName ShotColumnLens(TEXT("Lens"));
const FName ShotColumnSubject(TEXT("Subject"));
const FName ShotColumnScreen(TEXT("Screen"));
const FName ShotColumnMotion(TEXT("Motion"));
const FName ShotColumnConfidence(TEXT("Confidence"));

FText ShotSizeLabel(const FString& Id)
{
    if (Id == TEXT("extreme_close_up")) return LOCTEXT("SizeECU", "Extreme close-up");
    if (Id == TEXT("close_up")) return LOCTEXT("SizeCU", "Close-up");
    if (Id == TEXT("medium")) return LOCTEXT("SizeMedium", "Medium");
    if (Id == TEXT("full")) return LOCTEXT("SizeFull", "Full shot");
    if (Id == TEXT("wide")) return LOCTEXT("SizeWide", "Wide");
    return FText::FromString(Id);
}

FText CameraAngleLabel(const FString& Id)
{
    if (Id == TEXT("low")) return LOCTEXT("AngleLow", "Low angle");
    if (Id == TEXT("eye")) return LOCTEXT("AngleEye", "Eye level");
    if (Id == TEXT("high")) return LOCTEXT("AngleHigh", "High angle");
    if (Id == TEXT("overhead")) return LOCTEXT("AngleOverhead", "Overhead");
    if (Id == TEXT("dutch")) return LOCTEXT("AngleDutch", "Dutch (tilted)");
    return FText::FromString(Id);
}

FText CameraMotionLabel(const FString& Id)
{
    static const TMap<FString, FString> Labels = {
        { TEXT("static"), LOCTEXT("MotionStatic", "Static").ToString() }, { TEXT("pan"), LOCTEXT("MotionPan", "Pan").ToString() }, { TEXT("tilt"), LOCTEXT("MotionTilt", "Tilt").ToString() },
        { TEXT("dolly_in"), LOCTEXT("MotionDollyIn", "Dolly in").ToString() }, { TEXT("dolly_out"), LOCTEXT("MotionDollyOut", "Dolly out").ToString() }, { TEXT("zoom_in"), LOCTEXT("MotionZoomIn", "Zoom in").ToString() },
        { TEXT("zoom_out"), LOCTEXT("MotionZoomOut", "Zoom out").ToString() }, { TEXT("truck_left"), LOCTEXT("MotionTruckLeft", "Truck left").ToString() }, { TEXT("truck_right"), LOCTEXT("MotionTruckRight", "Truck right").ToString() },
        { TEXT("pedestal"), LOCTEXT("MotionPedestalUp", "Pedestal (up)").ToString() }, { TEXT("orbit"), LOCTEXT("MotionOrbit", "Orbit").ToString() }, { TEXT("tracking"), LOCTEXT("MotionTracking", "Tracking").ToString() },
        { TEXT("handheld"), LOCTEXT("MotionHandheld", "Handheld").ToString() } };
    const FString* Label = Labels.Find(Id);
    return FText::FromString(Label ? *Label : Id);
}

TSharedRef<SWidget> MakeOptionCombo(const TArray<TSharedPtr<FString>>* Options, TFunction<FString()> GetCurrent,
    TFunction<void(const FString&)> OnPicked, TFunction<FText(const FString&)> ToLabel)
{
    return SNew(SComboBox<TSharedPtr<FString>>)
        .OptionsSource(Options)
        .OnGenerateWidget_Lambda([ToLabel](TSharedPtr<FString> Option) { return SNew(STextBlock).Text(ToLabel(Option.IsValid() ? *Option : FString())); })
        .OnSelectionChanged_Lambda([OnPicked](TSharedPtr<FString> Option, ESelectInfo::Type) { if (Option.IsValid()) OnPicked(*Option); })
        [ SNew(STextBlock).Text_Lambda([GetCurrent, ToLabel] { return ToLabel(GetCurrent()); }) ];
}

TSharedRef<SWidget> MakeFloatSpin(const float MinValue, const float MaxValue, const float Delta, TFunction<float()> GetValue, TFunction<void(float)> SetValue)
{
    return SNew(SSpinBox<float>)
        .MinValue(MinValue).MaxValue(MaxValue).Delta(Delta)
        .Value_Lambda([GetValue] { return GetValue(); })
        .OnValueChanged_Lambda([SetValue](const float Value) { SetValue(Value); })
        .OnValueCommitted_Lambda([SetValue](const float Value, ETextCommit::Type) { SetValue(Value); });
}

TSharedRef<SWidget> MakeLabeledField(const FText& Label, const TSharedRef<SWidget>& Content, const float Width = 120.0f)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[ SNew(STextBlock).Text(Label) ]
        + SHorizontalBox::Slot().AutoWidth()[ SNew(SBox).WidthOverride(Width)[ Content ] ];
}

FString GetReferenceCacheFilename(const FString& MatchMode)
{
    return MatchMode == TEXT("edit") ? TEXT("reference_plan_edit.json") : TEXT("reference_plan_scene.json");
}

FString BuildReferenceAnalysisFingerprint(const FUPTReferenceFrameResult& Frames, const UUPTSettings* Settings, const FString& MatchMode)
{
    return FString::Printf(TEXT("v1|%s|%s|%s|%.3f|%d|%.3f|%.3f|%d|%dx%d"),
        *Frames.SourceFingerprint, *MatchMode, *Settings->VisionModel,
        Frames.SampleIntervalSeconds, Settings->MaxVisionFrames, Settings->ShotChangeThreshold,
        Settings->MinimumShotSeconds, Frames.FrameFiles.Num(), Frames.FrameWidth, Frames.FrameHeight);
}

FString ResolveFFmpegPath(const FString& Setting)
{
    const FString Trimmed = Setting.TrimStartAndEnd();
    if (Trimmed.IsEmpty()) return FString();
    if (Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT("\\")))
    {
        return FPaths::FileExists(Trimmed) ? FPaths::ConvertRelativePathToFull(Trimmed) : FString();
    }
    TArray<FString> PathEntries;
    FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(PathEntries, TEXT(";"), true);
    for (FString PathEntry : PathEntries)
    {
        PathEntry.TrimQuotesInline();
        const FString Candidate = FPaths::Combine(PathEntry, Trimmed);
        if (FPaths::FileExists(Candidate)) return Candidate;
    }
    return FString();
}

USkeletalMesh* GetSelectedSkeletalMesh()
{
    FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Assets;
    ContentBrowser.Get().GetSelectedAssets(Assets);
    for (const FAssetData& Asset : Assets)
    {
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset())) return Mesh;
    }
    return nullptr;
}

TSharedRef<SWidget> MakeStepHeader(const FText& Title)
{
    return SNew(STextBlock).Text(Title).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12));
}

TSharedRef<SWidget> MakeRadio(TFunction<bool()> IsChecked, TFunction<void()> OnChecked, const FText& Label)
{
    return SNew(SCheckBox)
        .Style(FAppStyle::Get(), "RadioButton")
        .IsChecked_Lambda([IsChecked] { return IsChecked() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([OnChecked](const ECheckBoxState State) { if (State == ECheckBoxState::Checked) OnChecked(); })
        [ SNew(STextBlock).Text(Label) ];
}

// 작은 로컬 모델이 자주 내는 실수(없는 Actor 이름, "none" 같은 미디어 ID, 범위 밖 숫자)를 검증 전에 보정한다.
// 안전 범위 안으로만 옮기며, 무엇을 고쳤는지는 결과 칸에 그대로 보여 준다.
TArray<FString> RepairModelPlan(FUPTCinematicPlan& Plan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const TMap<FString, TWeakObjectPtr<UAnimSequenceBase>>& Animations, const TMap<FString, TWeakObjectPtr<USoundBase>>& Sounds)
{
    TArray<FString> Notes;
    TArray<FString> Labels;
    for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
    {
        if (Pair.Value.IsValid()) Labels.Add(Pair.Key);
    }
    auto ResolveLabel = [&Labels, &Notes, &SceneActors](const FString& Value, const FString& ShotName, const TCHAR* Field) -> FString
    {
        if (Value.IsEmpty() || SceneActors.Contains(Value)) return Value;
        for (const FString& Label : Labels)
        {
            if (Label.Equals(Value, ESearchCase::IgnoreCase) || Label.Contains(Value, ESearchCase::IgnoreCase) || Value.Contains(Label, ESearchCase::IgnoreCase))
            {
                Notes.Add(FString::Printf(TEXT("%s: %s '%s' → '%s'"), *ShotName, Field, *Value, *Label));
                return Label;
            }
        }
        if (Labels.Num() == 1)
        {
            Notes.Add(FString::Format(*LOCTEXT("FixSoleCast", "{0}: {1} '{2}' -> the only cast member '{3}'").ToString(), {ShotName, Field, Value, Labels[0]}));
            return Labels[0];
        }
        return Value;
    };
    auto ClampField = [&Notes](float& Value, const float Min, const float Max, const float Fallback, const FString& ShotName, const TCHAR* Field)
    {
        const float Repaired = FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
        if (!FMath::IsNearlyEqual(Repaired, Value))
        {
            Notes.Add(FString::Printf(TEXT("%s: %s %.2f → %.2f"), *ShotName, Field, Value, Repaired));
            Value = Repaired;
        }
    };
    const TSet<FString> AllowedMotions = { TEXT(""), TEXT("static"), TEXT("pan"), TEXT("tilt"), TEXT("dolly_in"), TEXT("dolly_out"), TEXT("zoom_in"), TEXT("zoom_out"), TEXT("truck_left"), TEXT("truck_right"), TEXT("pedestal"), TEXT("orbit"), TEXT("tracking"), TEXT("handheld") };

    if (Plan.FrameRate < 1 || Plan.FrameRate > 120) { Notes.Add(FString::Printf(TEXT("frame_rate %d → 30"), Plan.FrameRate)); Plan.FrameRate = 30; }
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        FUPTCinematicShot& Shot = Plan.Shots[Index];
        if (Shot.Name.TrimStartAndEnd().IsEmpty()) Shot.Name = FString::Printf(TEXT("Shot_%02d"), Index + 1);
        Shot.Subject = ResolveLabel(Shot.Subject, Shot.Name, TEXT("subject"));
        Shot.LookAt = ResolveLabel(Shot.LookAt, Shot.Name, TEXT("look_at"));
        if (Shot.Subject.IsEmpty() && Shot.CameraLocation.IsNearlyZero() && !Labels.IsEmpty())
        {
            Notes.Add(FString::Format(*LOCTEXT("FixEmptySubject", "{0}: subject was empty -> '{1}'").ToString(), {Shot.Name, Labels[0]}));
            Shot.Subject = Labels[0];
        }
        for (FString* Id : { &Shot.AnimationId, &Shot.LipSyncAnimationId })
        {
            if (!Id->IsEmpty() && !Animations.Contains(*Id)) { Notes.Add(FString::Format(*LOCTEXT("FixDropAnimation", "{0}: removed unknown animation id '{1}'").ToString(), {Shot.Name, *Id})); Id->Reset(); }
        }
        if (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId)) { Notes.Add(FString::Format(*LOCTEXT("FixDropAudio", "{0}: removed unknown audio id '{1}'").ToString(), {Shot.Name, Shot.AudioId})); Shot.AudioId.Reset(); }
        ClampField(Shot.DurationSeconds, 0.5f, 120.0f, 3.0f, Shot.Name, TEXT("duration_seconds"));
        ClampField(Shot.FocalLength, 8.0f, 200.0f, 50.0f, Shot.Name, TEXT("focal_length"));
        ClampField(Shot.DistanceCm, 30.0f, 10000.0f, 300.0f, Shot.Name, TEXT("distance_cm"));
        ClampField(Shot.HeightCm, -5000.0f, 5000.0f, 0.0f, Shot.Name, TEXT("height_cm"));
        ClampField(Shot.SideCm, -5000.0f, 5000.0f, 0.0f, Shot.Name, TEXT("side_cm"));
        ClampField(Shot.SubjectScreenX, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("subject_screen_x"));
        ClampField(Shot.SubjectScreenY, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("subject_screen_y"));
        ClampField(Shot.FocusHeightRatio, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("focus_height"));
        if (!AllowedMotions.Contains(Shot.CameraMotion)) { Notes.Add(FString::Format(*LOCTEXT("FixUnknownMotion", "{0}: unknown camera_motion '{1}' -> static").ToString(), {Shot.Name, Shot.CameraMotion})); Shot.CameraMotion = TEXT("static"); }
    }
    return Notes;
}

class SUPTShotRow : public SMultiColumnTableRow<TSharedPtr<FUPTShotListItem>>
{
public:
    SLATE_BEGIN_ARGS(SUPTShotRow) {}
        SLATE_ARGUMENT(TSharedPtr<FUPTShotListItem>, Item)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
    {
        Item = InArgs._Item;
        FSuperRowType::Construct(FSuperRowType::FArguments(), OwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
    {
        FString Value;
        bool bWarn = false;
        if (Item.IsValid())
        {
            if (Column == ShotColumnIndex) Value = FString::FromInt(Item->Index + 1);
            else if (Column == ShotColumnName) Value = Item->Name;
            else if (Column == ShotColumnDuration) Value = Item->Duration;
            else if (Column == ShotColumnLens) Value = Item->Lens;
            else if (Column == ShotColumnSubject) Value = Item->Subject;
            else if (Column == ShotColumnScreen) Value = Item->Screen;
            else if (Column == ShotColumnMotion) Value = Item->Motion;
            else if (Column == ShotColumnConfidence) { Value = Item->Confidence; bWarn = Item->bLowConfidence; }
        }
        return SNew(SBox).Padding(FMargin(4.0f, 2.0f))
        [
            SNew(STextBlock)
            .Text(FText::FromString(Value))
            .ColorAndOpacity(bWarn ? FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f)) : FSlateColor::UseForeground())
            .ToolTipText(bWarn ? LOCTEXT("LowConfidenceTip", "Analysis confidence is low for this shot. Play it back and adjust the framing with 'Edit selected shot'.") : FText::GetEmpty())
        ];
    }

private:
    TSharedPtr<FUPTShotListItem> Item;
};
}

SUPTMainPanel::~SUPTMainPanel()
{
    USelection::SelectionChangedEvent.RemoveAll(this);
}

void SUPTMainPanel::Construct(const FArguments& InArgs)
{
    LLMClient = MakeShared<FUPTLLMClient>();
    InitShotEditorOptions();
    RefreshEnvironmentStatus();

    auto MakeTabButton = [this](const int32 TabIndex, const FText& Label)
    {
        return SNew(SCheckBox)
            .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([this, TabIndex] { return ActiveTab == TabIndex ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this, TabIndex](ECheckBoxState) { ActiveTab = TabIndex; })
            [
                SNew(SBox).Padding(FMargin(16.0f, 6.0f))
                [ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11)) ]
            ];
    };

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(12, 10, 12, 6)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)[ MakeTabButton(0, LOCTEXT("CinematicTab", "Create cinematic")) ]
            + SHorizontalBox::Slot().AutoWidth()[ MakeTabButton(1, LOCTEXT("AnimationTab", "Universal animation retarget")) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(12, 0)[ SNew(SSeparator) ]
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SWidgetSwitcher)
            .WidgetIndex_Lambda([this] { return ActiveTab; })
            + SWidgetSwitcher::Slot()[ BuildCinematicTab() ]
            + SWidgetSwitcher::Slot()[ BuildAnimationTab() ]
        ]
    ];

    USelection::SelectionChangedEvent.AddSP(this, &SUPTMainPanel::OnEditorSelectionChanged);
    RefreshCastFromSelection();
}

TSharedRef<SWidget> SUPTMainPanel::BuildCinematicTab()
{
    return SNew(SScrollBox)
    + SScrollBox::Slot().Padding(12, 8)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 24, 0)
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
                    if (UPTEndpoint::IsLocal(Settings->ApiEndpoint) && UPTEndpoint::IsLocal(Settings->VisionApiEndpoint))
                    {
                        return FText::Format(LOCTEXT("LocalModel", "Model: local {0} (no API key or credits needed)"), FText::FromString(Settings->VisionModel));
                    }
                    if (!UPTEndpoint::IsAllowed(Settings->ApiEndpoint) || !UPTEndpoint::IsAllowed(Settings->VisionApiEndpoint))
                    {
                        return FText::Format(LOCTEXT("EndpointInvalid", "Invalid model address: '{0}' / '{1}' (check Editor Preferences > Universal Production Tools)"),
                            FText::FromString(Settings->ApiEndpoint), FText::FromString(Settings->VisionApiEndpoint));
                    }
                    return bApiKeyReady
                        ? LOCTEXT("ApiReady", "API key: ready")
                        : FText::Format(LOCTEXT("ApiMissing", "API key: missing (set the environment variable {0} and restart the editor)"), FText::FromString(GetDefault<UUPTSettings>()->ApiKeyEnvironmentVariable));
                })
                .ColorAndOpacity_Lambda([this] { return bApiKeyReady ? FSlateColor(FLinearColor(0.3f, 0.85f, 0.4f)) : FSlateColor(FLinearColor(1.0f, 0.4f, 0.35f)); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    return ResolvedFFmpegPath.IsEmpty()
                        ? LOCTEXT("FFmpegMissing", "FFmpeg: missing (required for reference-video mode - install it and add it to PATH, or set the path in Editor Preferences > Universal Production Tools)")
                        : LOCTEXT("FFmpegReady", "FFmpeg: ready");
                })
                .ColorAndOpacity_Lambda([this] { return ResolvedFFmpegPath.IsEmpty() ? FSlateColor(FLinearColor(1.0f, 0.4f, 0.35f)) : FSlateColor(FLinearColor(0.3f, 0.85f, 0.4f)); })
            ]
        ]

        // ① 등장인물
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)[ MakeStepHeader(LOCTEXT("Step1", "(1) Cast")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return GetCastText(); }) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("ChangeCast", "Reassign cast"))
                .ToolTipText(LOCTEXT("ChangeCastTip", "Unpins the cast and replaces it with the Actors currently selected in the level."))
                .Visibility_Lambda([this] { return bCastLocked ? EVisibility::Visible : EVisibility::Collapsed; })
                .OnClicked(this, &SUPTMainPanel::UnlockCast)
            ]
        ]

        // ② 소스
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step2", "(2) What should it be built from?")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::ReferenceVideo; }, [this] { Source = EUPTCinematicSource::ReferenceVideo; }, LOCTEXT("SourceVideo", "Reference video")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::PromptSearch; }, [this] { Source = EUPTCinematicSource::PromptSearch; }, LOCTEXT("SourceLibrary", "Find a reference by prompt (library)")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::Script; }, [this] { Source = EUPTCinematicSource::Script; }, LOCTEXT("SourceScript", "Text script (LLM)")) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ MakeRadio([this] { return Source == EUPTCinematicSource::ShotPlanJson; }, [this] { Source = EUPTCinematicSource::ShotPlanJson; }, LOCTEXT("SourceJson", "Paste Shot Plan JSON (no API needed)")) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SVerticalBox)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::ShotPlanJson ? EVisibility::Visible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("JsonHelp", "Paste a Shot Plan JSON and press 'Generate cinematic'. Works without an API or FFmpeg. Results generated from a video or a script are recorded here as well, so you can edit the values and generate again.")) ]
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SBox).HeightOverride(200)[ SAssignNew(JsonInput, SMultiLineEditableTextBox).HintText(LOCTEXT("JsonHint", "{\"title\":\"...\",\"frame_rate\":30,\"shots\":[...]}")) ] ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SVerticalBox)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::ReferenceVideo ? EVisibility::Visible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                [ SNew(SButton).Text(LOCTEXT("SelectVideo", "Choose video...")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::SelectReferenceVideo) ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                [ SNew(STextBlock).Text_Lambda([this] { return ReferenceVideoPath.IsEmpty() ? LOCTEXT("NoVideo", "No video selected (MP4 / MOV / M4V / AVI)") : FText::FromString(FPaths::GetCleanFilename(ReferenceVideoPath)); }) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [ SNew(STextBlock).Text(LOCTEXT("LinkLabel", "or a video link")) ]
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SAssignNew(ReferenceLinkInput, SEditableTextBox)
                    .HintText(LOCTEXT("LinkHint", "https://www.youtube.com/watch?v=... or a Pexels/Pixabay video address (a link takes precedence over the file above)"))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
                [ SNew(STextBlock).Text(LOCTEXT("SectionLabel", "Section")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SBox).WidthOverride(110)
                    [ SAssignNew(ReferenceSectionInput, SEditableTextBox).HintText(LOCTEXT("SectionHint", "e.g. 1:20-2:05")) ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return bLinkRightsConfirmed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](const ECheckBoxState State) { bLinkRightsConfirmed = State == ECheckBoxState::Checked; })
                [
                    SNew(STextBlock).AutoWrapText(true)
                    .Text(LOCTEXT("LinkRights", "Rights confirmed: I have the right or permission to use this video for analysis, including sites such as YouTube that do not allow downloading. (A low-resolution copy is fetched temporarily, used only for pose analysis, and deleted right away. Pexels, Pixabay, Internet Archive and Wikimedia are fetched without this checkbox. Links are always handled by local pose analysis.)"))
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                [ SNew(STextBlock).Text(LOCTEXT("MatchModeLabel", "Match on")) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
                [ MakeRadio([this] { return ReferenceMatchMode == TEXT("scene"); }, [this] { ReferenceMatchMode = TEXT("scene"); }, LOCTEXT("SceneMode", "Scene composition (ignore subtitles and logos)")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ MakeRadio([this] { return ReferenceMatchMode == TEXT("edit"); }, [this] { ReferenceMatchMode = TEXT("edit"); }, LOCTEXT("EditMode", "Edit rhythm (keep zoom, shake and cut timing)")) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                [ SNew(STextBlock).Text(LOCTEXT("AnalysisMethodLabel", "Analysis method")) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
                [ MakeRadio([this] { return ReferenceAnalysisMethod == EUPTReferenceAnalysisMethod::LocalPose; }, [this] { ReferenceAnalysisMethod = EUPTReferenceAnalysisMethod::LocalPose; }, LOCTEXT("LocalPoseMethod", "Local pose analysis (no API needed, recommended)")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ MakeRadio([this] { return ReferenceAnalysisMethod == EUPTReferenceAnalysisMethod::VisionModel; }, [this] { ReferenceAnalysisMethod = EUPTReferenceAnalysisMethod::VisionModel; }, LOCTEXT("VisionMethod", "Vision model (the configured LLM)")) ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SVerticalBox)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::PromptSearch ? EVisibility::Visible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(STextBlock).AutoWrapText(true)
                .Text(LOCTEXT("LibraryHelp", "Searches a folder of reference videos for the segment whose staging and shot structure (shot size, angle, camera motion, number of people) is closest to what you described, and uses it as the reference. Runs on local pose analysis without an API, and each video in the folder is analyzed only once."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [ SNew(STextBlock).Text(LOCTEXT("LibraryFolderLabel", "Library folder")) ]
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SAssignNew(LibraryFolderInput, SEditableTextBox)
                    .Text(FText::FromString(GetDefault<UUPTSettings>()->ReferenceLibraryDirectory))
                    .HintText(LOCTEXT("LibraryFolderHint", "e.g. D:/RefVideos"))
                    .OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type)
                    {
                        UUPTSettings* Settings = GetMutableDefault<UUPTSettings>();
                        Settings->ReferenceLibraryDirectory = Text.ToString().TrimStartAndEnd();
                        Settings->SaveConfig();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
                [ SNew(SButton).Text(LOCTEXT("BrowseLibrary", "Choose folder...")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::BrowseLibraryFolder) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SBox).HeightOverride(70)
                [ SAssignNew(LibraryPromptInput, SMultiLineEditableTextBox).HintText(LOCTEXT("LibraryPromptHint", "e.g. two people stand in a two-shot, talk over the shoulder, and it ends on a close-up of one of them")) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(LOCTEXT("SearchLibrary", "Search only")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::SearchLibraryOnly) ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
                [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("LibraryGenerateHint", "'Generate cinematic' uses the result picked in the list (the top one if nothing is picked). If the list is empty or the folder or prompt changed, it searches first.")) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SBox).HeightOverride(130)
                [
                    SAssignNew(LibraryResultView, SListView<TSharedPtr<FUPTLibraryResultItem>>)
                    .ListItemsSource(&LibraryResults)
                    .SelectionMode(ESelectionMode::Single)
                    .OnGenerateRow(this, &SUPTMainPanel::GenerateLibraryRow)
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SBox)
            .HeightOverride(120)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::Script ? EVisibility::Visible : EVisibility::Collapsed; })
            [
                SAssignNew(ScriptInput, SMultiLineEditableTextBox)
                .HintText(LOCTEXT("ScriptHint", "e.g. A ruined temple. The hero walks to the altar and the goddess appears. Open on a wide shot and cut to a close-up of her face."))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 10, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
            [ SNew(STextBlock).Text(LOCTEXT("MediaLabel", "Animation and audio (optional)")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("AddMedia", "Add Content Browser selection")).OnClicked(this, &SUPTMainPanel::AddSelectedMedia) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("ClearMedia", "Clear")).OnClicked(this, &SUPTMainPanel::ClearMedia) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return GetMediaText(); }) ]

        // ③ 생성
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step3", "(3) Generate")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .ContentPadding(FMargin(28.0f, 8.0f))
                .IsEnabled_Lambda([this] { return !IsGenerationInFlight(); })
                .OnClicked(this, &SUPTMainPanel::GenerateCinematic)
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
                    .Text_Lambda([this] { return IsGenerationInFlight() ? LOCTEXT("Generating", "Generating...") : LOCTEXT("Generate", "Generate cinematic"); })
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
            [ SAssignNew(StatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("Ready", "Set up the cast and a reference video (or a script), then generate.")) ]
        ]

        // ④ 결과 검토
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step4", "(4) Review")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text_Lambda([this]
                {
                    const ULevelSequence* Sequence = LastCreatedSequence.Get();
                    return Sequence ? FText::FromString(Sequence->GetPathName()) : LOCTEXT("NoSequence", "No cinematic has been generated yet.");
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("OpenSequence", "Open in Sequencer")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::OpenLastSequence) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SBox)
            .HeightOverride(180)
            [
                SAssignNew(ShotListView, SListView<TSharedPtr<FUPTShotListItem>>)
                .ListItemsSource(&ShotItems)
                .SelectionMode(ESelectionMode::Single)
                .OnGenerateRow(this, &SUPTMainPanel::GenerateShotRow)
                .OnSelectionChanged(this, &SUPTMainPanel::OnShotSelectionChanged)
                .OnMouseButtonDoubleClick(this, &SUPTMainPanel::OnShotDoubleClicked)
                .HeaderRow
                (
                    SNew(SHeaderRow)
                    + SHeaderRow::Column(ShotColumnIndex).DefaultLabel(LOCTEXT("ColIndex", "#")).FixedWidth(32.0f)
                    + SHeaderRow::Column(ShotColumnName).DefaultLabel(LOCTEXT("ColName", "Shot")).FillWidth(0.3f)
                    + SHeaderRow::Column(ShotColumnDuration).DefaultLabel(LOCTEXT("ColDuration", "Length")).FixedWidth(64.0f)
                    + SHeaderRow::Column(ShotColumnLens).DefaultLabel(LOCTEXT("ColLens", "Lens")).FixedWidth(64.0f)
                    + SHeaderRow::Column(ShotColumnSubject).DefaultLabel(LOCTEXT("ColSubject", "Subject")).FillWidth(0.25f)
                    + SHeaderRow::Column(ShotColumnScreen).DefaultLabel(LOCTEXT("ColScreen", "Screen position")).FixedWidth(90.0f)
                    + SHeaderRow::Column(ShotColumnMotion).DefaultLabel(LOCTEXT("ColMotion", "Camera motion")).FillWidth(0.2f)
                    + SHeaderRow::Column(ShotColumnConfidence).DefaultLabel(LOCTEXT("ColConfidence", "Confidence")).FixedWidth(72.0f)
                )
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("PlayShot", "Play selected shot")).ToolTipText(LOCTEXT("PlayShotTip", "Double-clicking a row in the shot list plays it as well.")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::PlaySelectedShot) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("CaptureCamera", "Keep my camera adjustments")).ToolTipText(LOCTEXT("CaptureCameraTip", "Press this after moving a camera in Sequencer and the same framing is kept on the next generation.")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::CaptureCurrentShotCamera) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("AnalyzeComposition", "Check framing")).ToolTipText(LOCTEXT("AnalyzeCompositionTip", "Checks each shot for head cropping, subject size and occlusion.")).IsEnabled_Lambda([this] { return bHasValidPlan; }).OnClicked(this, &SUPTMainPanel::AnalyzeComposition) ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("OpenPoseDebug", "Open pose analysis images"))
                .ToolTipText(LOCTEXT("OpenPoseDebugTip", "Shows the detected person boxes, joints and aim points from the reference video as one image per shot."))
                .Visibility_Lambda([this] { return LastPoseDebugDirectory.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
                .OnClicked(this, &SUPTMainPanel::OpenPoseDebugFolder)
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 6)
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(false)
            .Visibility_Lambda([this] { return bHasValidPlan && CurrentPlan.Shots.IsValidIndex(CurrentShotIndex) ? EVisibility::Visible : EVisibility::Collapsed; })
            .AreaTitle_Lambda([this]
            {
                return FText::Format(LOCTEXT("ShotEditTitle", "Edit selected shot - #{0} {1}"), FText::AsNumber(CurrentShotIndex + 1),
                    FText::FromString(CurrentPlan.Shots.IsValidIndex(CurrentShotIndex) ? CurrentPlan.Shots[CurrentShotIndex].Name : FString()));
            })
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(STextBlock).AutoWrapText(true)
                    .Text(LOCTEXT("ShotEditHelp", "Edits only the shot picked in the list. Changing the shot size or angle recomputes the lens, distance and height the same way reference generation does, and the number fields let you fine-tune. 'Apply to this shot' updates the same sequence in place and plays that shot."))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditSize", "Shot size"), MakeOptionCombo(&ShotSizeOptions, [this] { return EditShotSize; }, [this](const FString& Id) { PreviewShotSize(Id); }, &ShotSizeLabel), 140.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditAngle", "Angle"), MakeOptionCombo(&CameraAngleOptions, [this] { return EditCameraAngle; }, [this](const FString& Id) { EditCameraAngle = Id; bEditFramingChanged = true; }, &CameraAngleLabel), 120.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditMotion", "Camera motion"), MakeOptionCombo(&CameraMotionOptions, [this] { return EditCameraMotion; }, [this](const FString& Id) { EditCameraMotion = Id; }, &CameraMotionLabel), 130.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [
                        MakeLabeledField(LOCTEXT("EditSubject", "Subject"),
                            SAssignNew(SubjectCombo, SComboBox<TSharedPtr<FString>>)
                            .OptionsSource(&SubjectOptions)
                            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Option) { return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString())); })
                            .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Option, ESelectInfo::Type) { if (Option.IsValid()) EditSubject = *Option; })
                            [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(EditSubject.IsEmpty() ? LOCTEXT("ValueNone", "(none)").ToString() : EditSubject); }) ],
                            160.0f)
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditDuration", "Length (s)"), MakeFloatSpin(0.1f, 120.0f, 0.1f, [this] { return EditDuration; }, [this](const float Value) { EditDuration = Value; }), 80.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditFocal", "Lens (mm)"), MakeFloatSpin(8.0f, 200.0f, 1.0f, [this] { return EditFocalLength; }, [this](const float Value) { EditFocalLength = Value; }), 80.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [
                        MakeLabeledField(LOCTEXT("EditOverShoulder", "Over-the-shoulder foreground"),
                            SAssignNew(OverShoulderCombo, SComboBox<TSharedPtr<FString>>)
                            .ToolTipText(LOCTEXT("EditOverShoulderTip", "Puts the camera behind that Actor's shoulder and shoots the main subject. The camera is computed from the foreground Actor's position instead of distance and azimuth, and the lens keeps the shot size."))
                            .OptionsSource(&OverShoulderOptions)
                            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Option) { return SNew(STextBlock).Text(FText::FromString(Option.IsValid() && !Option->IsEmpty() ? *Option : FString(LOCTEXT("ValuePlainShot", "(plain shot)").ToString()))); })
                            .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Option, ESelectInfo::Type) { if (Option.IsValid()) EditOverShoulderActor = *Option; })
                            [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(EditOverShoulderActor.IsEmpty() ? LOCTEXT("ValuePlainShot", "(plain shot)").ToString() : EditOverShoulderActor); }) ],
                            160.0f)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        MakeLabeledField(LOCTEXT("EditOverShoulderSide", "Foreground side"),
                            MakeOptionCombo(&OverShoulderSideOptions, [this] { return EditOverShoulderSide; }, [this](const FString& Id) { EditOverShoulderSide = Id; },
                                [](const FString& Id) { return Id == TEXT("left") ? LOCTEXT("OverShoulderLeft", "Screen left") : LOCTEXT("OverShoulderRight", "Screen right"); }),
                            100.0f)
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditScreenX", "Screen X"), MakeFloatSpin(0.02f, 0.98f, 0.01f, [this] { return EditScreenX; }, [this](const float Value) { EditScreenX = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditScreenY", "Screen Y"), MakeFloatSpin(0.02f, 0.98f, 0.01f, [this] { return EditScreenY; }, [this](const float Value) { EditScreenY = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditAzimuth", "Azimuth (deg)"), MakeFloatSpin(-75.0f, 75.0f, 1.0f, [this] { return EditAzimuth; }, [this](const float Value) { EditAzimuth = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditDistance", "Distance (cm)"), MakeFloatSpin(50.0f, 10000.0f, 5.0f, [this] { return EditDistance; }, [this](const float Value) { EditDistance = Value; }), 90.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                    [
                        SNew(STextBlock).Text_Lambda([this]
                        {
                            const TWeakObjectPtr<UAnimSequenceBase>* Animation = Animations.Find(EditAnimationId);
                            const FString Name = EditAnimationId.IsEmpty() ? LOCTEXT("ValueNoneShort", "None").ToString() : (Animation && Animation->IsValid() ? Animation->Get()->GetName() : EditAnimationId);
                            return FText::Format(LOCTEXT("EditAnimation", "Animation: {0}"), FText::FromString(Name));
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(LOCTEXT("AssignAnimation", "Apply Content Browser animation")).OnClicked(this, &SUPTMainPanel::AssignSelectedAnimationToShot) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("ClearAnimation", "Remove animation")).OnClicked(this, &SUPTMainPanel::ClearShotAnimation) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4, 6, 4, 4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("ApplyShotEdit", "Apply to this shot and update the sequence"))
                        .IsEnabled_Lambda([this] { return !IsGenerationInFlight() && LastCreatedSequence.IsValid(); })
                        .OnClicked(this, &SUPTMainPanel::ApplyShotEdit)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("ResetShotEdit", "Revert (re-read current values)")).OnClicked_Lambda([this] { LoadShotEditor(); return FReply::Handled(); }) ]
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
        [
            SNew(SBox)
            .HeightOverride(120)
            [ SAssignNew(ReportOutput, SMultiLineEditableTextBox).IsReadOnly(true).HintText(LOCTEXT("ReportHint", "Analysis results and error details appear here.")) ]
        ]

        // 고급
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 8)
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(true)
            .AreaTitle(LOCTEXT("Advanced", "Advanced (role mapping)"))
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("RoleHelp", "Role to Actor mapping - fill this in only when a person in the video is assigned to the wrong Actor.")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(58)
                    [ SAssignNew(ReferenceRoleMappingInput, SMultiLineEditableTextBox).HintText(LOCTEXT("RoleHint", "e.g. {\"main_character\":\"Hero\",\"enemy\":\"Boss\"}")) ]
                ]
            ]
        ]
    ];
}

TSharedRef<SWidget> SUPTMainPanel::BuildAnimationTab()
{
    return SNew(SScrollBox)
    + SScrollBox::Slot().Padding(12, 8)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 6)[ MakeStepHeader(LOCTEXT("AnimTitle", "Convert another character's animation to fit mine")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 8)
        [
            SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("AnimGuide",
                "1. In the Content Browser, select the Skeletal Mesh the animations should be applied to.\n"
                "2. Open the window with the button below (or right-click the mesh > Universal Animation Retarget), pick the animations and retarget them in one batch.\n"
                "Humanoid bone structures such as MetaHuman, UE Mannequin and Mixamo are recognized automatically."))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .ContentPadding(FMargin(28.0f, 8.0f))
                .OnClicked(this, &SUPTMainPanel::OpenRetargetWindowForSelection)
                [ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)).Text(LOCTEXT("OpenRetarget", "Open the retarget window for the selected Skeletal Mesh")) ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
            [ SAssignNew(AnimationStatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("AnimReady", "Ready")) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 8)
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(true)
            .AreaTitle(LOCTEXT("BoneFixTitle", "Advanced: manual bone mapping (when auto-detection fails)"))
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("BoneFixHelp",
                        "Use this only for a mesh the retarget window reported as 'bone structure not recognized'. Select that mesh in the Content Browser and analyze it, "
                        "then fix only the wrong bones as JSON and save - the retarget window picks the correction up automatically."))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(LOCTEXT("AnalyzeMesh", "Analyze selected mesh")).OnClicked(this, &SUPTMainPanel::AnalyzeSelectedSkeleton) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("SaveOverride", "Save correction")).OnClicked(this, &SUPTMainPanel::SaveSkeletonOverride) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(80)
                    [ SAssignNew(SkeletonOverrideInput, SMultiLineEditableTextBox).HintText(LOCTEXT("OverrideHint", "Enter only the wrong bones. e.g. {\"Pelvis\":\"pelvis\",\"LeftHand\":\"hand_l\"}")) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(220)
                    [ SAssignNew(SkeletonReportOutput, SMultiLineEditableTextBox).IsReadOnly(true).HintText(LOCTEXT("SkeletonReportHint", "Analysis results appear here.")) ]
                ]
            ]
        ]
    ];
}

void SUPTMainPanel::RefreshEnvironmentStatus()
{
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    bApiKeyReady = UPTEndpoint::IsAllowed(Settings->ApiEndpoint) && UPTEndpoint::IsAllowed(Settings->VisionApiEndpoint)
        && !UPTEndpoint::ResolveApiKey(Settings->ApiKeyEnvironmentVariable, Settings->ApiEndpoint).IsEmpty()
        && !UPTEndpoint::ResolveApiKey(Settings->ApiKeyEnvironmentVariable, Settings->VisionApiEndpoint).IsEmpty();
    ResolvedFFmpegPath = ResolveFFmpegPath(Settings->FFmpegExecutablePath);
}

void SUPTMainPanel::OnEditorSelectionChanged(UObject* NewSelection)
{
    if (!GEditor || NewSelection != GEditor->GetSelectedActors()) return;
    RefreshCastFromSelection();
}

void SUPTMainPanel::RefreshCastFromSelection()
{
    if (bCastLocked || !GEditor || !GEditor->GetSelectedActors()) return;
    TMap<FString, TWeakObjectPtr<AActor>> NewCast;
    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        // Sequencer에서 카메라나 시퀀스를 클릭해도 등장인물이 바뀌지 않도록 제외한다.
        if (!IsValid(Actor) || Actor->IsA<ACameraActor>() || Actor->IsA<ALevelSequenceActor>()) continue;
        NewCast.Add(Actor->GetActorLabel(), Actor);
    }
    if (!NewCast.IsEmpty()) SceneActors = MoveTemp(NewCast);
}

FReply SUPTMainPanel::UnlockCast()
{
    bCastLocked = false;
    RefreshCastFromSelection();
    SetStatus(LOCTEXT("CastUnpinned", "The cast is no longer pinned. Actors selected in the level become the cast.").ToString(), FLinearColor::Green);
    return FReply::Handled();
}

FText SUPTMainPanel::GetCastText() const
{
    TArray<FString> Labels;
    for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
    {
        if (Pair.Value.IsValid()) Labels.Add(Pair.Key);
    }
    Labels.Sort();
    if (Labels.IsEmpty()) return LOCTEXT("NoCast", "Select the cast Actors in the level viewport (Ctrl+click for several).");
    return FText::FromString(FString::Join(Labels, TEXT(", ")) + (bCastLocked
        ? LOCTEXT("CastPinned", "   - in use by the last generation (pinned)").ToString()
        : LOCTEXT("CastFollowsSelection", "   - follows the level selection").ToString()));
}

FReply SUPTMainPanel::SelectReferenceVideo()
{
    if (IsGenerationInFlight()) return FReply::Handled();
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform)
    {
        SetStatus(LOCTEXT("NoFilePicker", "The file picker is not available.").ToString(), FLinearColor::Red);
        return FReply::Handled();
    }
    TArray<FString> SelectedFiles;
    const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (!DesktopPlatform->OpenFileDialog(ParentWindow, LOCTEXT("ChooseReferenceVideo", "Choose a reference video").ToString(),
        ReferenceVideoPath.IsEmpty() ? FPaths::ProjectDir() : FPaths::GetPath(ReferenceVideoPath), TEXT(""),
        TEXT("Video Files (*.mp4;*.mov;*.m4v;*.avi)|*.mp4;*.mov;*.m4v;*.avi"), EFileDialogFlags::None, SelectedFiles) || SelectedFiles.IsEmpty())
    {
        return FReply::Handled();
    }

    FString Error;
    if (!FUPTReferenceVideoProcessor::ValidateVideo(SelectedFiles[0], Error))
    {
        SetStatus(Error, FLinearColor::Red);
        return FReply::Handled();
    }
    ReferenceVideoPath = FPaths::ConvertRelativePathToFull(SelectedFiles[0]);
    ReferenceFrameResult = FUPTReferenceFrameResult();
    CurrentReferencePlan = FUPTReferencePlan();
    bHasReferencePlan = false;
    SetStatus(LOCTEXT("VideoChosen", "Reference video selected. The original video is never sent anywhere; only downscaled frames are analyzed.").ToString(), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::AddSelectedMedia()
{
    FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Assets;
    ContentBrowser.Get().GetSelectedAssets(Assets);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

    int32 Added = 0;
    for (const FAssetData& Asset : Assets)
    {
        UObject* Object = Asset.GetAsset();
        if (UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Object))
        {
            bool bExists = false;
            for (const TPair<FString, TWeakObjectPtr<UAnimSequenceBase>>& Pair : Animations) bExists |= Pair.Value.Get() == Animation;
            if (bExists) continue;
            Animations.Add(FString::Printf(TEXT("anim_%d"), Animations.Num() + 1), Animation);
            ++Added;
        }
        else if (USoundBase* Sound = Cast<USoundBase>(Object))
        {
            bool bExists = false;
            for (const TPair<FString, TWeakObjectPtr<USoundBase>>& Pair : Sounds) bExists |= Pair.Value.Get() == Sound;
            if (bExists) continue;
            Sounds.Add(FString::Printf(TEXT("audio_%d"), Sounds.Num() + 1), Sound);
            ++Added;
        }
    }
    SetStatus(Added > 0
        ? FString::Format(*LOCTEXT("MediaAdded", "Added {0} animation and audio assets.").ToString(), {Added})
        : LOCTEXT("MediaPickFirst", "Select an Animation Sequence/Montage or a Sound in the Content Browser first.").ToString(),
        Added > 0 ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

FReply SUPTMainPanel::ClearMedia()
{
    Animations.Reset();
    Sounds.Reset();
    SetStatus(LOCTEXT("MediaCleared", "Cleared the animation and audio list.").ToString());
    return FReply::Handled();
}

FText SUPTMainPanel::GetMediaText() const
{
    TArray<FString> Labels;
    for (const TPair<FString, TWeakObjectPtr<UAnimSequenceBase>>& Pair : Animations)
    {
        if (const UAnimSequenceBase* Animation = Pair.Value.Get()) Labels.Add(FString::Printf(TEXT("%s (%s)"), *Animation->GetName(), *Pair.Key));
    }
    for (const TPair<FString, TWeakObjectPtr<USoundBase>>& Pair : Sounds)
    {
        if (const USoundBase* Sound = Pair.Value.Get()) Labels.Add(FString::Printf(TEXT("%s (%s)"), *Sound->GetName(), *Pair.Key));
    }
    Labels.Sort();
    return Labels.IsEmpty()
        ? LOCTEXT("NoMedia", "None - to add character motion or dialogue, select assets in the Content Browser and add them.")
        : FText::FromString(FString::Join(Labels, TEXT(", ")));
}

FReply SUPTMainPanel::GenerateCinematic()
{
    if (IsGenerationInFlight()) return FReply::Handled();
    RefreshEnvironmentStatus();
    RefreshCastFromSelection();
    if (SceneActors.IsEmpty())
    {
        SetStatus(LOCTEXT("NeedCast", "(1) Pick the cast first - click the character Actors in the level viewport.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    SetReport(FString());
    LastReferencePlanFile.Empty();
    ShotConfidences.Reset();
    PendingGenerationWarning.Reset();

    if (Source == EUPTCinematicSource::PromptSearch)
    {
        const FString Prompt = LibraryPromptInput.IsValid() ? LibraryPromptInput->GetText().ToString().TrimStartAndEnd() : FString();
        if (Prompt.IsEmpty())
        {
            SetStatus(LOCTEXT("NeedPromptLong", "(2) Describe the staging you are looking for. e.g. open on a two-shot, talk over the shoulder, end on a close-up").ToString(), FLinearColor::Yellow);
            return FReply::Handled();
        }
        // 같은 폴더·프롬프트로 이미 검색했다면 목록에서 고른 결과(고르지 않았으면 1위)로 바로 만든다.
        if (!LibraryResults.IsEmpty() && LibraryResultsKey == GetLibraryQueryKey())
        {
            const TArray<TSharedPtr<FUPTLibraryResultItem>> Selected = LibraryResultView.IsValid() ? LibraryResultView->GetSelectedItems() : TArray<TSharedPtr<FUPTLibraryResultItem>>();
            const TSharedPtr<FUPTLibraryResultItem> Item = Selected.IsEmpty() ? LibraryResults[0] : Selected[0];
            bGenerationInFlight = true;
            FString LoadError;
            if (!LoadLibraryResult(Item, LoadError))
            {
                FinishGeneration(LoadError, FLinearColor::Red);
                return FReply::Handled();
            }
            SetStatus(FString::Format(*LOCTEXT("ProgUseResult", "[2/4] Using search result #{0} '{1}' ({2}) as the reference.").ToString(), {Item->Rank, Item->Video, Item->Segment}));
            ContinueReferencePipeline();
            return FReply::Handled();
        }
        StartLibrarySearch(true);
        return FReply::Handled();
    }

    if (Source == EUPTCinematicSource::ShotPlanJson)
    {
        if (!JsonInput.IsValid() || JsonInput->GetText().IsEmpty())
        {
            SetStatus(LOCTEXT("NeedJson", "(2) Paste a JSON into the Shot Plan JSON box.").ToString(), FLinearColor::Yellow);
            return FReply::Handled();
        }
        return ApplyJsonAndRegenerate();
    }

    if (Source == EUPTCinematicSource::Script)
    {
        const FString Script = ScriptInput.IsValid() ? ScriptInput->GetText().ToString().TrimStartAndEnd() : FString();
        if (Script.IsEmpty())
        {
            SetStatus(LOCTEXT("NeedScript", "(2) Enter a script or staging directions.").ToString(), FLinearColor::Yellow);
            return FReply::Handled();
        }
        if (!bApiKeyReady)
        {
            SetStatus(LOCTEXT("NoApiKey", "No API key, so the LLM cannot be called. Set the environment variable as described above.").ToString(), FLinearColor::Red);
            return FReply::Handled();
        }
        bGenerationInFlight = true;
        SetStatus(LOCTEXT("ProgLlmPlan", "[1/3] The LLM is building the shot plan...").ToString());
        LLMClient->GeneratePlan(Script, BuildSceneContext(), BuildAssetContext(), FUPTLLMComplete::CreateSP(this, &SUPTMainPanel::OnScriptPlanReceived));
        return FReply::Handled();
    }

    FString Error;
    const FString ReferenceLink = ReferenceLinkInput.IsValid() ? ReferenceLinkInput->GetText().ToString().TrimStartAndEnd() : FString();
    if (!ReferenceLink.IsEmpty())
    {
        // 링크는 영상을 받아 로컬 포즈 분석으로만 처리한다(Vision 모델은 프레임 파일이 필요하고, 권리 확인 영상은 분석 뒤 지우기 때문).
        if (!FUPTPoseReferenceAnalyzer::Preflight(Error))
        {
            SetStatus(Error, FLinearColor::Red);
            return FReply::Handled();
        }
        const FString Section = ReferenceSectionInput.IsValid() ? ReferenceSectionInput->GetText().ToString().TrimStartAndEnd() : FString();
        bGenerationInFlight = true;
        bHasReferencePlan = false;
        SetStatus(LOCTEXT("ProgLinkPose", "[1/4] Fetching the linked video and running local pose analysis (download -> cut detection -> person and joint detection). This can take a few minutes depending on the video length and your connection...").ToString());
        FUPTPoseReferenceAnalyzer::AnalyzeLink(ReferenceLink, Section, bLinkRightsConfirmed, ReferenceMatchMode,
            FUPTPoseAnalysisComplete::CreateSP(this, &SUPTMainPanel::OnPoseAnalysisCompleted));
        return FReply::Handled();
    }
    if (!FUPTReferenceVideoProcessor::ValidateVideo(ReferenceVideoPath, Error))
    {
        SetStatus(ReferenceVideoPath.IsEmpty() ? LOCTEXT("NeedVideo", "(2) Pick a reference video with 'Choose video...'.").ToString() : Error, FLinearColor::Yellow);
        return FReply::Handled();
    }
    if (ReferenceAnalysisMethod == EUPTReferenceAnalysisMethod::LocalPose)
    {
        if (!FUPTPoseReferenceAnalyzer::Preflight(Error))
        {
            SetStatus(Error, FLinearColor::Red);
            return FReply::Handled();
        }
        bGenerationInFlight = true;
        bHasReferencePlan = false;
        SetStatus(LOCTEXT("ProgLocalPose", "[1/4] Running local pose analysis (cut detection -> person and joint detection). Takes tens of seconds to a few minutes depending on the video length...").ToString());
        FUPTPoseReferenceAnalyzer::Analyze(ReferenceVideoPath, ReferenceMatchMode, FUPTPoseAnalysisComplete::CreateSP(this, &SUPTMainPanel::OnPoseAnalysisCompleted));
        return FReply::Handled();
    }
    if (ResolvedFFmpegPath.IsEmpty())
    {
        SetStatus(LOCTEXT("NoFFmpeg", "FFmpeg was not found. Install it or set its path as described above.").ToString(), FLinearColor::Red);
        return FReply::Handled();
    }

    bGenerationInFlight = true;
    bHasReferencePlan = false;
    SetStatus(LOCTEXT("ProgExtractFrames", "[1/4] Extracting representative frames and cut boundaries from the video...").ToString());
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    FUPTReferenceVideoProcessor::ExtractFrames(ReferenceVideoPath, Settings->ReferenceFrameIntervalSeconds, Settings->MaxReferenceFrames,
        FUPTReferenceFramesComplete::CreateSP(this, &SUPTMainPanel::OnReferenceFramesExtracted));
    return FReply::Handled();
}

void SUPTMainPanel::OnReferenceFramesExtracted(const bool bSuccess, const FUPTReferenceFrameResult& Result, const FString& Error)
{
    if (!bSuccess)
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }
    ReferenceFrameResult = Result;
    if (TryLoadCachedReferenceAnalysis())
    {
        SetStatus(LOCTEXT("ProgReuseAnalysis", "[2/4] Reusing the previous analysis of the same video.").ToString());
        ContinueReferencePipeline();
        return;
    }
    if (!bApiKeyReady)
    {
        FinishGeneration(LOCTEXT("NoVisionKey", "The API key needed for video analysis is missing. Set the environment variable as described above.").ToString(), FLinearColor::Red);
        return;
    }
    SetStatus(FString::Format(*LOCTEXT("ProgVisionAnalyze", "[2/4] Analyzing {0} candidate cuts with the vision model...").ToString(), {Result.ShotBoundaries.Num()}));
    LLMClient->AnalyzeReferenceFrames(ReferenceFrameResult, ReferenceMatchMode,
        FUPTLLMComplete::CreateSP(this, &SUPTMainPanel::OnReferenceAnalysisCompleted));
}

void SUPTMainPanel::OnReferenceAnalysisCompleted(const bool bSuccess, const FString& Result)
{
    if (!bSuccess)
    {
        FinishGeneration(Result, FLinearColor::Red);
        return;
    }
    FString Error;
    FUPTReferencePlan ParsedPlan;
    if (!FUPTReferencePlanParser::Parse(Result, ParsedPlan, Error))
    {
        SetReport(Result);
        FinishGeneration(FString::Format(*LOCTEXT("ErrVideoAnalysisFormat", "The video analysis result is not in the expected format: {0}").ToString(), {Error}), FLinearColor::Red);
        return;
    }

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    CurrentReferencePlan = MoveTemp(ParsedPlan);
    CurrentReferencePlan.MatchMode = ReferenceMatchMode;
    CurrentReferencePlan.VisionModel = Settings->VisionModel;
    CurrentReferencePlan.AnalysisFingerprint = BuildReferenceAnalysisFingerprint(ReferenceFrameResult, Settings, ReferenceMatchMode);
    CurrentReferencePlan.AspectRatio = ReferenceFrameResult.AspectRatio;
    bHasReferencePlan = true;

    FString CacheJson;
    if (!ReferenceFrameResult.OutputDirectory.IsEmpty() && FUPTReferencePlanParser::ToJson(CurrentReferencePlan, CacheJson))
    {
        FFileHelper::SaveStringToFile(CacheJson, *FPaths::Combine(ReferenceFrameResult.OutputDirectory, GetReferenceCacheFilename(ReferenceMatchMode)),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    ContinueReferencePipeline();
}

void SUPTMainPanel::OnPoseAnalysisCompleted(const bool bSuccess, const FUPTPoseAnalysisResult& Result, const FString& Error)
{
    if (!bSuccess)
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }
    FString ParseError;
    FUPTReferencePlan ParsedPlan;
    if (!FUPTReferencePlanParser::Parse(Result.PlanJson, ParsedPlan, ParseError))
    {
        SetReport(Result.PlanJson.Left(4000));
        FinishGeneration(FString::Format(*LOCTEXT("ErrPoseAnalysisFormat", "The pose analysis result is not in the expected format: {0}").ToString(), {ParseError}), FLinearColor::Red);
        return;
    }
    ParsedPlan.MatchMode = ReferenceMatchMode;
    CurrentReferencePlan = MoveTemp(ParsedPlan);
    bHasReferencePlan = true;
    LastPoseDebugDirectory = Result.DebugDirectory;
    LastReferencePlanFile = Result.PlanFile;
    UE_LOG(LogUPTPanel, Log, TEXT("Pose reference plan: %s (cache=%d)"), *Result.PlanFile, Result.bFromCache ? 1 : 0);
    SetStatus(Result.bFromCache ? LOCTEXT("ProgReusePose", "[2/4] Reusing the pose analysis of the same video.").ToString() : LOCTEXT("ProgPoseDone", "[2/4] Pose analysis finished.").ToString());
    ContinueReferencePipeline();
}

FReply SUPTMainPanel::BrowseLibraryFolder()
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform) return FReply::Handled();
    FString Folder;
    const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (DesktopPlatform->OpenDirectoryDialog(ParentWindow, LOCTEXT("LibraryFolderTitle", "Reference video library folder").ToString(), GetDefault<UUPTSettings>()->ReferenceLibraryDirectory, Folder))
    {
        UUPTSettings* Settings = GetMutableDefault<UUPTSettings>();
        Settings->ReferenceLibraryDirectory = Folder;
        Settings->SaveConfig();
        if (LibraryFolderInput.IsValid()) LibraryFolderInput->SetText(FText::FromString(Folder));
    }
    return FReply::Handled();
}

FReply SUPTMainPanel::SearchLibraryOnly()
{
    if (!IsGenerationInFlight()) StartLibrarySearch(false);
    return FReply::Handled();
}

FString SUPTMainPanel::GetLibraryQueryKey() const
{
    const FString Prompt = LibraryPromptInput.IsValid() ? LibraryPromptInput->GetText().ToString().TrimStartAndEnd() : FString();
    return GetDefault<UUPTSettings>()->ReferenceLibraryDirectory.TrimStartAndEnd() + TEXT("|") + Prompt;
}

void SUPTMainPanel::StartLibrarySearch(const bool bContinueToGeneration)
{
    const FString Prompt = LibraryPromptInput.IsValid() ? LibraryPromptInput->GetText().ToString().TrimStartAndEnd() : FString();
    if (Prompt.IsEmpty())
    {
        SetStatus(LOCTEXT("NeedPrompt", "(2) Describe the staging you are looking for.").ToString(), FLinearColor::Yellow);
        return;
    }
    // 폴더 칸에 입력만 하고 Enter를 누르지 않았어도 지금 보이는 경로로 검색한다.
    if (LibraryFolderInput.IsValid())
    {
        const FString Typed = LibraryFolderInput->GetText().ToString().TrimStartAndEnd();
        UUPTSettings* Settings = GetMutableDefault<UUPTSettings>();
        if (Typed != Settings->ReferenceLibraryDirectory)
        {
            Settings->ReferenceLibraryDirectory = Typed;
            Settings->SaveConfig();
        }
    }
    const FString Library = GetDefault<UUPTSettings>()->ReferenceLibraryDirectory.TrimStartAndEnd();
    if (Library.IsEmpty())
    {
        SetStatus(LOCTEXT("NeedLibraryFolder", "(2) Point 'Choose folder...' at the folder that holds your reference videos.").ToString(), FLinearColor::Yellow);
        return;
    }
    bGenerationInFlight = true;
    PendingLibraryKey = GetLibraryQueryKey();
    SetStatus(LOCTEXT("ProgLibrarySearch", "[1/4] Searching the reference library. The first run, or a run after adding videos, analyzes each video and can take a few minutes...").ToString());
    FUPTPoseReferenceAnalyzer::SearchLibrary(Library, Prompt, GetDefault<UUPTSettings>()->ReferenceLibraryResultCount,
        FUPTLibrarySearchComplete::CreateSP(this, &SUPTMainPanel::OnLibrarySearchCompleted, bContinueToGeneration));
}

void SUPTMainPanel::OnLibrarySearchCompleted(const bool bSuccess, const FString& ResultJson, const FString& Error, const bool bContinueToGeneration)
{
    LibraryResults.Reset();
    LibraryResultsKey.Reset();
    LibraryReport.Reset();
    if (LibraryResultView.IsValid()) LibraryResultView->RequestListRefresh();
    if (!bSuccess)
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultJson), Root) || !Root.IsValid())
    {
        FinishGeneration(LOCTEXT("ErrLibraryResult", "Could not read the library search result.").ToString(), FLinearColor::Red);
        return;
    }

    // 프롬프트를 어떤 샷 요구로 이해했는지 먼저 보여 준다(원하는 결과가 안 나오면 표현을 바꾸는 근거가 된다).
    LibraryReport = LOCTEXT("ReportPromptRead", "[Prompt as read]\n").ToString();
    const TSharedPtr<FJsonObject>* Query = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* QueryShots = nullptr;
    int32 RequestCount = 0;
    if (Root->TryGetObjectField(TEXT("query"), Query) && Query && (*Query)->TryGetArrayField(TEXT("shots"), QueryShots) && QueryShots)
    {
        for (const TSharedPtr<FJsonValue>& Value : *QueryShots)
        {
            const TSharedPtr<FJsonObject> Request = Value->AsObject();
            if (!Request.IsValid()) continue;
            TArray<FString> Parts;
            for (const TCHAR* Key : { TEXT("size"), TEXT("angle"), TEXT("motion") })
            {
                FString Part;
                if (Request->TryGetStringField(Key, Part)) Parts.Add(Part);
            }
            double People = 0.0;
            bool bOverShoulder = false;
            if (Request->TryGetBoolField(TEXT("ots"), bOverShoulder) && bOverShoulder) Parts.Add(LOCTEXT("OverShoulderWord", "over the shoulder").ToString());
            else if (Request->TryGetNumberField(TEXT("people"), People)) Parts.Add(FString::Format(*LOCTEXT("PeopleCount", "{0} people").ToString(), {FMath::RoundToInt(People)}));
            FString Text;
            Request->TryGetStringField(TEXT("text"), Text);
            LibraryReport += FString::Printf(TEXT("%d) \"%s\" → %s\n"), ++RequestCount, *Text, *FString::Join(Parts, TEXT(" / ")));
        }
    }
    if (RequestCount == 0) LibraryReport += LOCTEXT("ReportKeywordOnly", "No shot size, angle, camera motion or head count was recognized, so the search used file-name keywords only.\n").ToString();

    LibraryReport += LOCTEXT("ReportResults", "\n[Search results]\n").ToString();
    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    if (Root->TryGetArrayField(TEXT("results"), Results) && Results)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Results)
        {
            const TSharedPtr<FJsonObject> Result = Value->AsObject();
            if (!Result.IsValid()) continue;
            TSharedPtr<FUPTLibraryResultItem> Item = MakeShared<FUPTLibraryResultItem>();
            Item->Rank = LibraryResults.Num() + 1;
            Result->TryGetStringField(TEXT("video"), Item->Video);
            Result->TryGetNumberField(TEXT("score"), Item->Score);
            Result->TryGetStringField(TEXT("reference_plan"), Item->ReferencePlanFile);
            const TSharedPtr<FJsonObject>* Segment = nullptr;
            if (Result->TryGetObjectField(TEXT("segment"), Segment) && Segment)
            {
                double StartShot = 0, EndShot = 0, StartSeconds = 0, EndSeconds = 0;
                (*Segment)->TryGetNumberField(TEXT("start_shot"), StartShot);
                (*Segment)->TryGetNumberField(TEXT("end_shot"), EndShot);
                (*Segment)->TryGetNumberField(TEXT("start_seconds"), StartSeconds);
                (*Segment)->TryGetNumberField(TEXT("end_seconds"), EndSeconds);
                Item->Segment = FString::Format(*LOCTEXT("ShotRange", "shots {0}-{1} ({2}-{3}s)").ToString(), {FMath::RoundToInt(StartShot), FMath::RoundToInt(EndShot), FString::Printf(TEXT("%.1f"), StartSeconds), FString::Printf(TEXT("%.1f"), EndSeconds)});
            }
            const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
            TArray<FString> MatchTexts;
            if (Result->TryGetArrayField(TEXT("matches"), Matches) && Matches)
            {
                for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
                {
                    const TSharedPtr<FJsonObject> Match = MatchValue->AsObject();
                    FString ShotName, ShotInfo;
                    if (Match.IsValid() && Match->TryGetStringField(TEXT("shot"), ShotName) && Match->TryGetStringField(TEXT("shot_info"), ShotInfo))
                    {
                        MatchTexts.Add(FString::Printf(TEXT("%s(%s)"), *ShotName, *ShotInfo));
                    }
                }
            }
            Item->Matches = FString::Join(MatchTexts, TEXT(", "));
            LibraryReport += FString::Format(*LOCTEXT("ReportRow", "#{0} {1} pts  {2}  {3}  matched shots: {4}\n").ToString(), {Item->Rank, FString::Printf(TEXT("%.0f"), Item->Score), Item->Video, Item->Segment, Item->Matches});
            LibraryResults.Add(Item);
        }
    }
    LibraryResultsKey = PendingLibraryKey;
    if (LibraryResultView.IsValid())
    {
        LibraryResultView->RequestListRefresh();
        if (!LibraryResults.IsEmpty()) LibraryResultView->SetSelection(LibraryResults[0]);
    }
    SetReport(LibraryReport);

    if (LibraryResults.IsEmpty())
    {
        FinishGeneration(LOCTEXT("LibraryNoMatch", "No similar segment was found in the library. Try different wording, or add more videos.").ToString(), FLinearColor::Yellow);
        return;
    }
    if (!bContinueToGeneration)
    {
        FinishGeneration(FString::Format(*LOCTEXT("LibrarySearchDone", "Search finished: {0} results. Pick one from the list (the top one by default) and press 'Generate cinematic'.").ToString(), {LibraryResults.Num()}), FLinearColor::Green);
        return;
    }
    FString LoadError;
    if (!LoadLibraryResult(LibraryResults[0], LoadError))
    {
        FinishGeneration(LoadError, FLinearColor::Red);
        return;
    }
    SetStatus(FString::Format(*LOCTEXT("ProgUseTopResult", "[2/4] Using the top result '{0}' ({1}, {2} pts) as the reference.").ToString(), {LibraryResults[0]->Video, LibraryResults[0]->Segment, FString::Printf(TEXT("%.0f"), LibraryResults[0]->Score)}));
    ContinueReferencePipeline();
}

bool SUPTMainPanel::LoadLibraryResult(const TSharedPtr<FUPTLibraryResultItem>& Item, FString& OutError)
{
    FString Json;
    if (!Item.IsValid() || !FFileHelper::LoadFileToString(Json, *Item->ReferencePlanFile))
    {
        OutError = LOCTEXT("ErrSegmentFile", "Could not read the reference segment file from the search result. Search again.").ToString();
        return false;
    }
    FUPTReferencePlan Parsed;
    FString ParseError;
    if (!FUPTReferencePlanParser::Parse(Json, Parsed, ParseError))
    {
        OutError = FString::Format(*LOCTEXT("ErrSegmentFormat", "The reference segment is not in the expected format: {0}").ToString(), {ParseError});
        return false;
    }
    Parsed.MatchMode = TEXT("scene");
    CurrentReferencePlan = MoveTemp(Parsed);
    bHasReferencePlan = true;
    LastReferencePlanFile = Item->ReferencePlanFile;
    LastPoseDebugDirectory = FPaths::GetPath(Item->ReferencePlanFile);
    return true;
}

TSharedRef<ITableRow> SUPTMainPanel::GenerateLibraryRow(TSharedPtr<FUPTLibraryResultItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(STableRow<TSharedPtr<FUPTLibraryResultItem>>, OwnerTable)
        .Padding(FMargin(4.0f, 2.0f))
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [ SNew(SBox).WidthOverride(32)[ SNew(STextBlock).Text(FText::FromString(FString::Format(*LOCTEXT("RankLabel", "#{0}").ToString(), {Item->Rank}))) ] ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [ SNew(SBox).WidthOverride(48)[ SNew(STextBlock).Text(FText::FromString(FString::Format(*LOCTEXT("ScoreLabel", "{0} pts").ToString(), {FString::Printf(TEXT("%.0f"), Item->Score)}))) ] ]
            + SHorizontalBox::Slot().FillWidth(0.3f).Padding(0, 0, 8, 0)
            [ SNew(STextBlock).Text(FText::FromString(Item->Video)).ToolTipText(FText::FromString(Item->ReferencePlanFile)) ]
            + SHorizontalBox::Slot().FillWidth(0.25f).Padding(0, 0, 8, 0)
            [ SNew(STextBlock).Text(FText::FromString(Item->Segment)) ]
            + SHorizontalBox::Slot().FillWidth(0.45f)
            [ SNew(STextBlock).Text(FText::FromString(Item->Matches)).ToolTipText(FText::FromString(Item->Matches)) ]
        ];
}

FReply SUPTMainPanel::OpenPoseDebugFolder()
{
    if (!LastPoseDebugDirectory.IsEmpty()) FPlatformProcess::ExploreFolder(*LastPoseDebugDirectory);
    return FReply::Handled();
}

bool SUPTMainPanel::TryLoadCachedReferenceAnalysis()
{
    if (ReferenceFrameResult.OutputDirectory.IsEmpty()) return false;
    FString CachedJson;
    if (!FFileHelper::LoadFileToString(CachedJson, *FPaths::Combine(ReferenceFrameResult.OutputDirectory, GetReferenceCacheFilename(ReferenceMatchMode)))) return false;

    FString Error;
    FUPTReferencePlan ParsedPlan;
    if (!FUPTReferencePlanParser::Parse(CachedJson, ParsedPlan, Error) || ParsedPlan.MatchMode != ReferenceMatchMode) return false;
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    if (!ParsedPlan.VisionModel.IsEmpty() && ParsedPlan.VisionModel != Settings->VisionModel) return false;
    if (!ParsedPlan.AnalysisFingerprint.IsEmpty()
        && ParsedPlan.AnalysisFingerprint != BuildReferenceAnalysisFingerprint(ReferenceFrameResult, Settings, ReferenceMatchMode)) return false;

    ParsedPlan.AspectRatio = ReferenceFrameResult.AspectRatio;
    CurrentReferencePlan = MoveTemp(ParsedPlan);
    bHasReferencePlan = true;
    return true;
}

void SUPTMainPanel::ContinueReferencePipeline()
{
    SetStatus(LOCTEXT("ProgAssignCast", "[3/4] Assigning the people in the video to the cast and computing cameras...").ToString());
    FString Error;
    if (!ApplyReferenceBlocking(Error))
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }

    // 신뢰도로 생성을 막지 않는다. 영상 종류(합성·실사·저해상도)마다 신뢰도 분포가 달라 한 기준으로 막으면 정상 장면도 멈추기 때문이다.
    // 대신 신뢰도가 낮은 샷을 완료 메시지와 샷 목록(신뢰도 열)에 표시해 '선택한 샷 수정'으로 그 샷만 손보게 한다.
    const float WarningThreshold = GetDefault<UUPTSettings>()->MinimumAutoGenerationConfidence;
    TArray<FString> LowConfidenceShots;
    int32 EmptyShots = 0;
    for (const FUPTReferenceShot& Shot : CurrentReferencePlan.Shots)
    {
        if (Shot.SubjectRole.IsEmpty())
        {
            ++EmptyShots;
            continue;
        }
        if (Shot.Confidence < WarningThreshold) LowConfidenceShots.Add(FString::Printf(TEXT("%s(%.0f%%)"), *Shot.Name, Shot.Confidence * 100.0f));
    }
    PendingGenerationWarning.Reset();
    if (!LowConfidenceShots.IsEmpty())
    {
        PendingGenerationWarning += FString::Format(*LOCTEXT("WarnLowConfidence", " [note] {0} shots have low analysis confidence: {1} - play them back and adjust with 'Edit selected shot'.").ToString(), {LowConfidenceShots.Num(), FString::Join(LowConfidenceShots, TEXT(", "))});
    }
    if (EmptyShots > 0)
    {
        // 인물 없는 샷(원경·인서트·타이틀)은 첫 등장인물을 기준으로 영상의 샷 크기대로 찍는다.
        PendingGenerationWarning += FString::Format(*LOCTEXT("NotePersonless", " {0} shots without a person (distant views, inserts) were placed relative to the first cast member.").ToString(), {EmptyShots});
    }

    // 구도 검증은 로컬 포즈 분석기가 만든 Reference Plan(영상 분석 결과나 라이브러리 검색 구간)이 있을 때만 할 수 있다.
    if (LastReferencePlanFile.IsEmpty() || !FPaths::FileExists(LastReferencePlanFile))
    {
        SetStatus(LOCTEXT("ProgBuildSequence", "[4/4] Building the Level Sequence...").ToString());
        if (!CreateSequence(Error))
        {
            FinishGeneration(Error, FLinearColor::Red);
            return;
        }
        FinishGeneration(BuildCompletionMessage(), FLinearColor::Green);
        return;
    }

    // 로컬 포즈 분석으로 만든 계획은 샷별 카메라 시점을 캡처해 같은 분석기로 레퍼런스와 비교하고,
    // 인물 크기·위치 오차만큼 카메라를 옮겨 다시 재는 과정을 반복한 뒤 샷마다 가장 잘 맞은 구도로 Level Sequence를 만든다.
    constexpr int32 MaxCorrections = 2;
    const FString ReferencePlanFile = LastReferencePlanFile;
    // 보정 중 선택이 바뀌어도 등장인물이 흔들리지 않도록 먼저 고정한다.
    const bool bWasCastLocked = bCastLocked;
    bCastLocked = true;
    SetStatus(FString::Format(*LOCTEXT("ProgCompareFraming", "[4/4] Comparing the camera framing against the reference... (measurement 1/{0})").ToString(), {MaxCorrections + 1}));
    TWeakPtr<SWidget> WeakPanel = AsShared();
    FUPTPoseReferenceAnalyzer::RefineShotCameras(CurrentPlan, SceneActors, ReferencePlanFile, MaxCorrections,
        FUPTPoseRefineProgress::CreateLambda([WeakPanel](const int32 Pass, const double Score)
        {
            const TSharedPtr<SWidget> Pinned = WeakPanel.Pin();
            if (!Pinned.IsValid()) return;
            static_cast<SUPTMainPanel*>(Pinned.Get())->SetStatus(FString::Format(
                *LOCTEXT("ProgRefineFraming", "[4/4] Refining the camera framing... measurement {0}/{1}, current score {2}").ToString(),
                {Pass + 1, MaxCorrections + 1, FString::Printf(TEXT("%.0f"), Score)}));
        }),
        FUPTPoseRefineComplete::CreateLambda([WeakPanel, bWasCastLocked](const bool bSuccess, const FUPTCinematicPlan& BestPlan, const FString& Result)
        {
            const TSharedPtr<SWidget> Pinned = WeakPanel.Pin();
            if (!Pinned.IsValid()) return;
            SUPTMainPanel* Panel = static_cast<SUPTMainPanel*>(Pinned.Get());
            double Score = -1.0;
            FString ReportText;
            if (bSuccess)
            {
                Panel->CurrentPlan = BestPlan;
                if (Panel->JsonInput.IsValid()) Panel->JsonInput->SetText(FText::FromString(FUPTPlanParser::ToJson(Panel->CurrentPlan)));
                ReportText = FUPTPoseReferenceAnalyzer::FormatVerifyReport(Result, Score);
                UE_LOG(LogUPTPanel, Log, TEXT("%s"), *ReportText);
            }
            else
            {
                // 검증을 못 해도 계산한 카메라로 시네마틱은 만든다.
                UE_LOG(LogUPTPanel, Warning, TEXT("Render verification failed: %s"), *Result);
            }

            Panel->SetStatus(LOCTEXT("ProgBuildSequence", "[4/4] Building the Level Sequence...").ToString());
            FString Error;
            if (!Panel->CreateSequence(Error))
            {
                Panel->bCastLocked = bWasCastLocked;
                Panel->FinishGeneration(Error, FLinearColor::Red);
                return;
            }
            const FString CompletionMessage = Panel->BuildCompletionMessage();
            if (!bSuccess)
            {
                Panel->FinishGeneration(CompletionMessage + LOCTEXT("ErrFramingCheck", " (framing check failed: ").ToString() + Result.Left(300) + TEXT(")"), FLinearColor::Yellow);
                return;
            }
            const FString Previous = Panel->ReportOutput.IsValid() ? Panel->ReportOutput->GetText().ToString() : FString();
            Panel->SetReport(ReportText + TEXT("\n") + Previous);
            Panel->FinishGeneration(FString::Format(*LOCTEXT("FramingScore", "{0} - framing score against the reference {1} / 100").ToString(), {CompletionMessage, FString::Printf(TEXT("%.0f"), Score)}),
                Score >= 70.0 ? FLinearColor::Green : FLinearColor::Yellow);
        }));
}

bool SUPTMainPanel::ApplyReferenceBlocking(FString& OutError)
{
    if (!bHasReferencePlan)
    {
        OutError = LOCTEXT("ErrNoVideoAnalysis", "There is no video analysis result.").ToString();
        return false;
    }
    TMap<FString, FString> ExplicitMappings;
    const FString MappingJson = ReferenceRoleMappingInput.IsValid() ? ReferenceRoleMappingInput->GetText().ToString().TrimStartAndEnd() : FString();
    if (!MappingJson.IsEmpty())
    {
        TSharedPtr<FJsonObject> MappingObject;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MappingJson), MappingObject) || !MappingObject.IsValid())
        {
            OutError = LOCTEXT("ErrRoleMappingJson", "The role mapping JSON in the advanced settings is not valid.").ToString();
            return false;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : MappingObject->Values)
        {
            FString ActorLabel;
            if (!Field.Value.IsValid() || !Field.Value->TryGetString(ActorLabel) || Field.Key.IsEmpty() || ActorLabel.IsEmpty())
            {
                OutError = FString::Format(*LOCTEXT("ErrRoleMappingValue", "The value of role mapping '{0}' must be a non-empty Actor label string.").ToString(), {Field.Key});
                return false;
            }
            ExplicitMappings.Add(Field.Key, ActorLabel);
        }
    }

    FString Report;
    FUPTCinematicPlan BlockingPlan;
    if (!FUPTReferenceBlockingSolver::BuildPlan(CurrentReferencePlan, SceneActors, ExplicitMappings, BlockingPlan, Report))
    {
        OutError = Report;
        return false;
    }
    CurrentPlan = MoveTemp(BlockingPlan);
    bHasValidPlan = true;
    ShotConfidences.Reset();
    for (const FUPTReferenceShot& ReferenceShot : CurrentReferencePlan.Shots)
    {
        ShotConfidences.Add(ReferenceShot.SubjectRole.IsEmpty() ? -1.0f : ReferenceShot.Confidence);
    }
    CurrentShotIndex = 0;
    if (JsonInput.IsValid()) JsonInput->SetText(FText::FromString(FUPTPlanParser::ToJson(CurrentPlan)));
    // 라이브러리 검색으로 고른 구간이면 프롬프트 해석·검색 순위를 함께 남긴다.
    const FString Prefix = Source == EUPTCinematicSource::PromptSearch && !LibraryReport.IsEmpty() ? LibraryReport + TEXT("\n") : FString();
    SetReport(Prefix + Report + TEXT("\n\n") + FUPTReferencePlanParser::ToPreviewText(CurrentReferencePlan));
    RefreshShotList();
    return true;
}

void SUPTMainPanel::OnScriptPlanReceived(const bool bSuccess, const FString& Result)
{
    if (!bSuccess)
    {
        FinishGeneration(Result, FLinearColor::Red);
        return;
    }
    UE_LOG(LogUPTPanel, Log, TEXT("LLM shot plan response:\n%s"), *Result.Left(8000));
    SetStatus(LOCTEXT("ProgValidatePlan", "[2/3] Validating the shot plan...").ToString());

    FString PlanJson = Result;
    TArray<FString> RepairNotes;
    FUPTCinematicPlan ModelPlan;
    FString ParseError;
    if (FUPTPlanParser::Parse(Result, ModelPlan, ParseError))
    {
        RepairNotes = RepairModelPlan(ModelPlan, SceneActors, Animations, Sounds);
        PlanJson = FUPTPlanParser::ToJson(ModelPlan);
        for (const FString& Note : RepairNotes) UE_LOG(LogUPTPanel, Log, TEXT("LLM plan auto-repair: %s"), *Note);
    }
    if (JsonInput.IsValid()) JsonInput->SetText(FText::FromString(PlanJson));

    FString Error;
    if (!ValidatePlanJson(PlanJson, Error))
    {
        Source = EUPTCinematicSource::ShotPlanJson;
        FinishGeneration(FString::Format(*LOCTEXT("ErrLlmPlan", "There is a problem with the shot plan the LLM produced - {0} (you can fix the Shot Plan JSON in (2) and generate again)").ToString(), {Error}), FLinearColor::Red);
        return;
    }
    SetStatus(LOCTEXT("ProgBuildSequence3", "[3/3] Building the Level Sequence...").ToString());
    if (!CreateSequence(Error))
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }
    if (!RepairNotes.IsEmpty() && ReportOutput.IsValid())
    {
        SetReport(LOCTEXT("ReportAutoFix", "[Automatic corrections]\n").ToString() + FString::Join(RepairNotes, TEXT("\n")) + TEXT("\n\n") + ReportOutput->GetText().ToString());
    }
    FinishGeneration(BuildCompletionMessage() + (RepairNotes.IsEmpty() ? FString() : FString::Format(*LOCTEXT("NoteAutoFix", " ({0} automatic corrections to the LLM result)").ToString(), {RepairNotes.Num()})), FLinearColor::Green);
}

bool SUPTMainPanel::ValidatePlanJson(const FString& JsonText, FString& OutError)
{
    FUPTCinematicPlan ParsedPlan;
    FString ParseError;
    if (!FUPTPlanParser::Parse(JsonText, ParsedPlan, ParseError))
    {
        SetReport(ParseError);
        UE_LOG(LogUPTPanel, Warning, TEXT("Shot Plan parse failed: %s"), *ParseError);
        OutError = FString::Format(*LOCTEXT("ErrShotPlanJson", "Could not read the Shot Plan JSON: {0}").ToString(), {ParseError});
        return false;
    }

    const FUPTPlanValidationResult Validation = FUPTPlanParser::Validate(ParsedPlan);
    TArray<FString> Problems = Validation.Errors;
    for (const FUPTCinematicShot& Shot : ParsedPlan.Shots)
    {
        if (!Shot.Subject.IsEmpty() && !SceneActors.Contains(Shot.Subject))
            Problems.Add(FString::Format(*LOCTEXT("ErrSubjectMissing", "{0}: subject '{1}' is not in the cast.").ToString(), {Shot.Name, Shot.Subject}));
        if (!Shot.LookAt.IsEmpty() && !SceneActors.Contains(Shot.LookAt))
            Problems.Add(FString::Format(*LOCTEXT("ErrLookAtMissing", "{0}: look_at '{1}' is not in the cast.").ToString(), {Shot.Name, Shot.LookAt}));
        if (!Shot.AnimationId.IsEmpty() && !Animations.Contains(Shot.AnimationId))
            Problems.Add(FString::Format(*LOCTEXT("ErrAnimationMissing", "{0}: animation_id '{1}' is not in the animation list.").ToString(), {Shot.Name, Shot.AnimationId}));
        if (!Shot.LipSyncAnimationId.IsEmpty() && !Animations.Contains(Shot.LipSyncAnimationId))
            Problems.Add(FString::Format(*LOCTEXT("ErrLipSyncMissing", "{0}: lip_sync_animation_id '{1}' is not in the animation list.").ToString(), {Shot.Name, Shot.LipSyncAnimationId}));
        if (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId))
            Problems.Add(FString::Format(*LOCTEXT("ErrAudioMissing", "{0}: audio_id '{1}' is not in the audio list.").ToString(), {Shot.Name, Shot.AudioId}));
    }
    if (!Problems.IsEmpty())
    {
        SetReport(FString::Join(Problems, TEXT("\n")));
        UE_LOG(LogUPTPanel, Warning, TEXT("Shot Plan validation failed:\n%s"), *FString::Join(Problems, TEXT("\n")));
        OutError = Problems.Num() == 1
            ? FString::Format(*LOCTEXT("ErrValidation", "Validation failed: {0}").ToString(), {Problems[0]})
            : FString::Format(*LOCTEXT("ErrValidationMore", "Validation failed: {0} and {1} more (see the details below)").ToString(), {Problems[0], Problems.Num() - 1});
        return false;
    }

    CurrentPlan = MoveTemp(ParsedPlan);
    bHasValidPlan = true;
    CurrentShotIndex = 0;
    FString Preview = FUPTPlanParser::ToPreviewText(CurrentPlan);
    for (const FString& Warning : Validation.Warnings) Preview += TEXT("\n[WARN] ") + Warning;
    SetReport(Preview);
    RefreshShotList();
    return true;
}

bool SUPTMainPanel::CreateSequence(FString& OutError, const bool bReuseExistingSequence)
{
    if (!bHasValidPlan)
    {
        OutError = LOCTEXT("ErrNoShotPlan", "There is no Shot Plan to generate from.").ToString();
        return false;
    }
    const FUPTBlockingValidationResult BlockingValidation = FUPTBlockingValidator::Validate(CurrentPlan);
    if (!BlockingValidation.IsValid())
    {
        SetReport(BlockingValidation.ToText());
        OutError = LOCTEXT("ErrSafetyCheck", "The shot plan failed the safety check. See the details below.").ToString();
        return false;
    }
    for (const FUPTCinematicShot& Shot : CurrentPlan.Shots)
    {
        if ((!Shot.Subject.IsEmpty() && !SceneActors.Contains(Shot.Subject)) || (!Shot.LookAt.IsEmpty() && !SceneActors.Contains(Shot.LookAt)) ||
            (!Shot.AnimationId.IsEmpty() && !Animations.Contains(Shot.AnimationId)) ||
            (!Shot.LipSyncAnimationId.IsEmpty() && !Animations.Contains(Shot.LipSyncAnimationId)) ||
            (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId)))
        {
            OutError = LOCTEXT("ErrPlanMismatch", "The cast or the animation/audio list does not match the Shot Plan. Check the lists and generate again.").ToString();
            return false;
        }
    }

    // 생성 중 Sequencer가 열리면서 선택이 바뀌어도 등장인물 목록이 흔들리지 않도록 먼저 고정한다.
    const bool bWasCastLocked = bCastLocked;
    bCastLocked = true;
    ULevelSequence* Sequence = FUPTSequenceBuilder::Build(CurrentPlan, SceneActors, Animations, Sounds, OutError,
        bReuseExistingSequence ? LastCreatedSequence.Get() : nullptr);
    if (!Sequence)
    {
        bCastLocked = bWasCastLocked;
        return false;
    }
    LastCreatedSequence = Sequence;
    bCastLocked = true;
    if (!bReuseExistingSequence) CurrentShotIndex = 0;
    RefreshShotList();
    if (GEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
    return true;
}

void SUPTMainPanel::FinishGeneration(const FString& Message, const FSlateColor& Color)
{
    bGenerationInFlight = false;
    SetStatus(Message, Color);
}

FString SUPTMainPanel::BuildCompletionMessage() const
{
    const ULevelSequence* Sequence = LastCreatedSequence.Get();
    return FString::Format(*LOCTEXT("GenerateDone", "Done: {0} - {1} shots. Double-click a row in the shot list to play that section.{2}").ToString(), {Sequence ? *Sequence->GetName() : TEXT("Level Sequence"), CurrentPlan.Shots.Num(), PendingGenerationWarning});
}

void SUPTMainPanel::RefreshShotList()
{
    ShotItems.Reset();
    if (bHasValidPlan)
    {
        for (int32 Index = 0; Index < CurrentPlan.Shots.Num(); ++Index)
        {
            const FUPTCinematicShot& Shot = CurrentPlan.Shots[Index];
            TSharedPtr<FUPTShotListItem> Item = MakeShared<FUPTShotListItem>();
            Item->Index = Index;
            Item->Name = Shot.Name;
            Item->Duration = FString::Format(*LOCTEXT("SecondsValue", "{0}s").ToString(), {FString::Printf(TEXT("%.1f"), Shot.DurationSeconds)});
            Item->Lens = FString::Printf(TEXT("%.0fmm"), Shot.FocalLength);
            Item->Subject = Shot.Subject.IsEmpty() ? LOCTEXT("AbsoluteCoords", "(absolute)").ToString() : Shot.Subject;
            Item->Screen = Shot.Subject.IsEmpty() ? TEXT("-") : FString::Printf(TEXT("%.2f, %.2f"), Shot.SubjectScreenX, Shot.SubjectScreenY);
            Item->Motion = Shot.CameraMotion.IsEmpty() ? TEXT("static") : Shot.CameraMotion;
            if (ShotConfidences.Num() == CurrentPlan.Shots.Num())
            {
                const float Confidence = ShotConfidences[Index];
                Item->Confidence = Confidence < 0.0f ? LOCTEXT("NoPerson", "no person").ToString() : FString::Printf(TEXT("%.0f%%"), Confidence * 100.0f);
                Item->bLowConfidence = Confidence >= 0.0f && Confidence < GetDefault<UUPTSettings>()->MinimumAutoGenerationConfidence;
            }
            else
            {
                Item->Confidence = TEXT("-");
            }
            ShotItems.Add(Item);
        }
    }
    if (ShotListView.IsValid())
    {
        ShotListView->RequestListRefresh();
        if (ShotItems.IsValidIndex(CurrentShotIndex)) ShotListView->SetSelection(ShotItems[CurrentShotIndex], ESelectInfo::Direct);
    }
    LoadShotEditor();
}

TSharedRef<ITableRow> SUPTMainPanel::GenerateShotRow(TSharedPtr<FUPTShotListItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SUPTShotRow, OwnerTable).Item(Item);
}

void SUPTMainPanel::OnShotSelectionChanged(TSharedPtr<FUPTShotListItem> Item, ESelectInfo::Type SelectInfo)
{
    if (!Item.IsValid()) return;
    const bool bChanged = CurrentShotIndex != Item->Index;
    CurrentShotIndex = Item->Index;
    if (bChanged) LoadShotEditor();
}

void SUPTMainPanel::InitShotEditorOptions()
{
    for (const TCHAR* Id : { TEXT("extreme_close_up"), TEXT("close_up"), TEXT("medium"), TEXT("full"), TEXT("wide") }) ShotSizeOptions.Add(MakeShared<FString>(Id));
    for (const TCHAR* Id : { TEXT("low"), TEXT("eye"), TEXT("high"), TEXT("overhead"), TEXT("dutch") }) CameraAngleOptions.Add(MakeShared<FString>(Id));
    for (const TCHAR* Id : { TEXT("static"), TEXT("pan"), TEXT("tilt"), TEXT("dolly_in"), TEXT("dolly_out"), TEXT("zoom_in"), TEXT("zoom_out"),
        TEXT("truck_left"), TEXT("truck_right"), TEXT("pedestal"), TEXT("orbit"), TEXT("tracking"), TEXT("handheld") })
    {
        CameraMotionOptions.Add(MakeShared<FString>(Id));
    }
    OverShoulderSideOptions.Add(MakeShared<FString>(TEXT("right")));
    OverShoulderSideOptions.Add(MakeShared<FString>(TEXT("left")));
}

void SUPTMainPanel::RebuildSubjectOptions()
{
    SubjectOptions.Reset();
    TArray<FString> Labels;
    SceneActors.GetKeys(Labels);
    Labels.Sort();
    for (const FString& Label : Labels) SubjectOptions.Add(MakeShared<FString>(Label));
    if (SubjectCombo.IsValid()) SubjectCombo->RefreshOptions();
    OverShoulderOptions.Reset();
    OverShoulderOptions.Add(MakeShared<FString>());
    for (const FString& Label : Labels) OverShoulderOptions.Add(MakeShared<FString>(Label));
    if (OverShoulderCombo.IsValid()) OverShoulderCombo->RefreshOptions();
}

float SUPTMainPanel::GetSubjectHeightCm(const FString& ActorLabel) const
{
    const TWeakObjectPtr<AActor>* ActorPtr = SceneActors.Find(ActorLabel);
    const FBox Bounds = UPTFraming::GetSubjectBounds(ActorPtr ? ActorPtr->Get() : nullptr);
    return Bounds.IsValid ? FMath::Max(30.0f, static_cast<float>(Bounds.GetSize().Z)) : 180.0f;
}

void SUPTMainPanel::LoadShotEditor()
{
    RebuildSubjectOptions();
    bEditFramingChanged = false;
    if (!bHasValidPlan || !CurrentPlan.Shots.IsValidIndex(CurrentShotIndex)) return;
    const FUPTCinematicShot& Shot = CurrentPlan.Shots[CurrentShotIndex];
    EditShotSize = FUPTReferenceBlockingSolver::InferShotSize(Shot);
    EditCameraAngle = FUPTReferenceBlockingSolver::InferCameraAngle(Shot);
    EditCameraMotion = Shot.CameraMotion.IsEmpty() ? TEXT("static") : Shot.CameraMotion;
    EditSubject = Shot.Subject;
    EditOverShoulderActor = Shot.OverShoulderActor;
    EditOverShoulderSide = Shot.OverShoulderSide == TEXT("left") ? TEXT("left") : TEXT("right");
    EditAnimationId = Shot.AnimationId;
    EditScreenX = Shot.SubjectScreenX;
    EditScreenY = Shot.SubjectScreenY;
    EditDistance = FMath::Sqrt(FMath::Square(Shot.DistanceCm) + FMath::Square(Shot.SideCm));
    EditAzimuth = FMath::RadiansToDegrees(FMath::Atan2(Shot.SideCm, Shot.DistanceCm));
    EditFocalLength = Shot.FocalLength;
    EditFocusHeight = Shot.FocusHeightRatio;
    EditDuration = Shot.DurationSeconds;
}

void SUPTMainPanel::PreviewShotSize(const FString& ShotSize)
{
    // 샷 크기를 고르면 렌즈·조준 높이·거리를 표 기준으로 바로 채워 숫자 칸에서 확인·미세 조정할 수 있게 한다.
    EditShotSize = ShotSize;
    bEditFramingChanged = true;
    FUPTCinematicShot Preview;
    Preview.SubjectScreenX = EditScreenX;
    FUPTReferenceBlockingSolver::ApplyShotFraming(Preview, ShotSize, EditCameraAngle, GetSubjectHeightCm(EditSubject),
        FUPTReferenceBlockingSolver::DefaultScreenHeightFraction(ShotSize), CurrentPlan.AspectRatio);
    EditFocalLength = Preview.FocalLength;
    EditFocusHeight = Preview.FocusHeightRatio;
    EditDistance = Preview.DistanceCm;
}

FReply SUPTMainPanel::AssignSelectedAnimationToShot()
{
    FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Assets;
    ContentBrowser.Get().GetSelectedAssets(Assets);
    UAnimSequenceBase* Animation = nullptr;
    for (const FAssetData& Asset : Assets)
    {
        Animation = Cast<UAnimSequenceBase>(Asset.GetAsset());
        if (Animation) break;
    }
    if (!Animation)
    {
        SetStatus(LOCTEXT("PickAnimationFirst", "Select an Animation Sequence or Montage in the Content Browser first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FString Id;
    for (const TPair<FString, TWeakObjectPtr<UAnimSequenceBase>>& Pair : Animations)
    {
        if (Pair.Value.Get() == Animation) { Id = Pair.Key; break; }
    }
    if (Id.IsEmpty())
    {
        int32 Number = Animations.Num() + 1;
        do { Id = FString::Printf(TEXT("anim_%d"), Number++); } while (Animations.Contains(Id));
        Animations.Add(Id, Animation);
    }
    EditAnimationId = Id;

    // 스켈레톤이 다르면 적용 시 오류가 나므로 미리 알려 준다.
    const TWeakObjectPtr<AActor>* SubjectPtr = SceneActors.Find(EditSubject);
    const USkeletalMeshComponent* Mesh = SubjectPtr && SubjectPtr->IsValid() ? SubjectPtr->Get()->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
    if (Mesh && Mesh->GetSkeletalMeshAsset() && Animation->GetSkeleton() != Mesh->GetSkeletalMeshAsset()->GetSkeleton())
    {
        SetStatus(FString::Format(*LOCTEXT("ErrSkeletonMismatch", "'{0}' has a different skeleton from '{1}' and will not play as-is. Pick an animation retargeted in the 'Universal animation retarget' tab.").ToString(), {Animation->GetName(), EditSubject}), FLinearColor::Yellow);
        return FReply::Handled();
    }
    SetStatus(FString::Format(*LOCTEXT("AnimationPicked", "'{0}' is set as the animation for shot #{1}. Press 'Apply to this shot and update the sequence' to apply it.").ToString(), {Animation->GetName(), CurrentShotIndex + 1}), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::ClearShotAnimation()
{
    EditAnimationId.Empty();
    SetStatus(LOCTEXT("AnimationRemoved", "The animation was removed. Press 'Apply to this shot and update the sequence' to apply it.").ToString());
    return FReply::Handled();
}

FReply SUPTMainPanel::ApplyShotEdit()
{
    if (IsGenerationInFlight() || !bHasValidPlan || !CurrentPlan.Shots.IsValidIndex(CurrentShotIndex)) return FReply::Handled();
    if (!LastCreatedSequence.IsValid())
    {
        SetStatus(LOCTEXT("GenerateFirst", "Generate a cinematic first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FUPTCinematicShot& Shot = CurrentPlan.Shots[CurrentShotIndex];
    const FUPTCinematicShot Before = Shot;

    if (!EditSubject.IsEmpty() && SceneActors.Contains(EditSubject))
    {
        if (Shot.LookAt.IsEmpty() || Shot.LookAt == Shot.Subject) Shot.LookAt = EditSubject;
        Shot.Subject = EditSubject;
    }
    Shot.DurationSeconds = FMath::Clamp(EditDuration, 0.1f, 120.0f);
    Shot.FocalLength = FMath::Clamp(EditFocalLength, 8.0f, 200.0f);
    Shot.FocusHeightRatio = FMath::Clamp(EditFocusHeight, 0.0f, 1.0f);
    Shot.SubjectScreenX = FMath::Clamp(EditScreenX, 0.02f, 0.98f);
    Shot.SubjectScreenY = FMath::Clamp(EditScreenY, 0.02f, 0.98f);
    Shot.CameraMotion = EditCameraMotion;
    Shot.AnimationId = EditAnimationId;
    // 방위각은 인물 정면 기준으로 카메라를 돌린 각도다. 뒤쪽으로는 가지 않도록 ±75°로 제한한다.
    const float Radius = FMath::Clamp(EditDistance, 50.0f, 10000.0f);
    const float Azimuth = FMath::DegreesToRadians(FMath::Clamp(EditAzimuth, -75.0f, 75.0f));
    Shot.DistanceCm = FMath::Max(30.0f, Radius * FMath::Cos(Azimuth));
    Shot.SideCm = Radius * FMath::Sin(Azimuth);
    if (bEditFramingChanged)
    {
        Shot.HeightCm = FUPTReferenceBlockingSolver::ComputeCameraHeight(EditCameraAngle, Radius, GetSubjectHeightCm(Shot.Subject), Shot.FocusHeightRatio);
        Shot.CameraRollDegrees = EditCameraAngle == TEXT("dutch") ? (Shot.SubjectScreenX < 0.5f ? -10.0f : 10.0f) : 0.0f;
    }
    else
    {
        // 크기·앵글을 안 바꿨으면 거리 변화에 비례해 높이만 옮겨 기존 앵글을 유지한다.
        const float OldRadius = FMath::Max(1.0f, FMath::Sqrt(FMath::Square(Before.DistanceCm) + FMath::Square(Before.SideCm)));
        Shot.HeightCm = FMath::Clamp(Before.HeightCm * Radius / OldRadius, -5000.0f, 5000.0f);
    }

    // 어깨 너머 앞사람을 지정하면 위 거리·방위각 대신 앞사람 어깨 뒤로 카메라를 다시 계산한다(렌즈로 샷 크기를 유지).
    if (!EditOverShoulderActor.IsEmpty() && EditOverShoulderActor != Shot.Subject && SceneActors.Contains(EditOverShoulderActor))
    {
        if (Before.OverShoulderActor != EditOverShoulderActor || Before.OverShoulderSide != EditOverShoulderSide)
        {
            Shot.OverShoulderEdgeX = EditOverShoulderSide == TEXT("left") ? 0.15f : 0.85f;
            // 다른 앞사람으로 바꾸면 이전 앞사람을 이 샷에서 옮겨 둔 배치는 버린다.
            if (!Before.OverShoulderActor.IsEmpty())
            {
                Shot.ActorPlacements.RemoveAll([&Before](const FUPTActorBlockingPlacement& Placement) { return Placement.ActorLabel == Before.OverShoulderActor; });
            }
        }
        Shot.OverShoulderActor = EditOverShoulderActor;
        Shot.OverShoulderSide = EditOverShoulderSide;
        FString OverShoulderNote;
        if (!FUPTReferenceBlockingSolver::ApplyOverShoulder(Shot, SceneActors.FindRef(Shot.Subject).Get(), SceneActors.FindRef(Shot.OverShoulderActor).Get(),
            CurrentPlan.AspectRatio, OverShoulderNote))
        {
            Shot = Before;
            SetStatus(OverShoulderNote, FLinearColor::Yellow);
            return FReply::Handled();
        }
    }
    else
    {
        Shot.OverShoulderActor.Empty();
        Shot.OverShoulderSide.Empty();
    }

    FString Error;
    if (!CreateSequence(Error, true))
    {
        Shot = Before;
        RefreshShotList();
        SetStatus(FString::Format(*LOCTEXT("ErrShotEdit", "The edit to shot #{0} could not be applied and was reverted: {1}").ToString(), {CurrentShotIndex + 1, Error}), FLinearColor::Red);
        return FReply::Handled();
    }
    if (JsonInput.IsValid()) JsonInput->SetText(FText::FromString(FUPTPlanParser::ToJson(CurrentPlan)));
    PlaySelectedShot();
    SetStatus(FString::Format(*LOCTEXT("ShotEditApplied", "Applied the edit to shot #{0}, updated '{1}' in place and is playing it.").ToString(), {CurrentShotIndex + 1, LastCreatedSequence->GetName()}), FLinearColor::Green);
    return FReply::Handled();
}

void SUPTMainPanel::OnShotDoubleClicked(TSharedPtr<FUPTShotListItem> Item)
{
    if (!Item.IsValid()) return;
    CurrentShotIndex = Item->Index;
    PlaySelectedShot();
}

FReply SUPTMainPanel::OpenLastSequence()
{
    if (ULevelSequence* Sequence = LastCreatedSequence.Get())
    {
        if (GEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
    }
    return FReply::Handled();
}

FReply SUPTMainPanel::PlaySelectedShot()
{
    ULevelSequence* Sequence = LastCreatedSequence.Get();
    if (!bHasValidPlan || CurrentPlan.Shots.IsEmpty() || !Sequence)
    {
        SetStatus(LOCTEXT("GenerateFirst", "Generate a cinematic first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }

    CurrentShotIndex = FMath::Clamp(CurrentShotIndex, 0, CurrentPlan.Shots.Num() - 1);
    int32 StartFrame = 0;
    for (int32 Index = 0; Index < CurrentShotIndex; ++Index)
    {
        StartFrame += FMath::Max(1, FMath::RoundToInt(CurrentPlan.Shots[Index].DurationSeconds * CurrentPlan.FrameRate));
    }
    const int32 EndFrame = StartFrame + FMath::Max(1, FMath::RoundToInt(CurrentPlan.Shots[CurrentShotIndex].DurationSeconds * CurrentPlan.FrameRate));

    if (GEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
    if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
    {
        SetStatus(LOCTEXT("SequencerOpening", "Sequencer is still opening. Try playing again in a moment.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }

    ULevelSequenceEditorBlueprintLibrary::Pause();
    ULevelSequenceEditorBlueprintLibrary::SetSelectionRangeStart(StartFrame);
    ULevelSequenceEditorBlueprintLibrary::SetSelectionRangeEnd(EndFrame);
    ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(FMovieSceneSequencePlaybackParams(FFrameTime(StartFrame), EUpdatePositionMethod::Jump));
    ULevelSequenceEditorBlueprintLibrary::PlayTo(FMovieSceneSequencePlaybackParams(FFrameTime(EndFrame), EUpdatePositionMethod::Play));
    SetStatus(FString::Format(*LOCTEXT("PlayingShot", "Playing shot #{0} '{1}' (frames {2}-{3})").ToString(), {CurrentShotIndex + 1, CurrentPlan.Shots[CurrentShotIndex].Name, StartFrame, EndFrame}), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::CaptureCurrentShotCamera()
{
    ULevelSequence* Sequence = LastCreatedSequence.Get();
    if (!Sequence || !bHasValidPlan || !CurrentPlan.Shots.IsValidIndex(CurrentShotIndex))
    {
        SetStatus(LOCTEXT("GenerateFirst", "Generate a cinematic first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
    {
        SetStatus(LOCTEXT("OpenInSequencerFirst", "Open the generated cinematic in Sequencer first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }

    UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(Sequence->GetMovieScene()->GetCameraCutTrack());
    if (!CutTrack || !CutTrack->GetAllSections().IsValidIndex(CurrentShotIndex))
    {
        SetStatus(LOCTEXT("ErrNoCameraCut", "Could not find the Camera Cut for the selected shot.").ToString(), FLinearColor::Red);
        return FReply::Handled();
    }
    UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(CutTrack->GetAllSections()[CurrentShotIndex]);
    if (!CutSection) return FReply::Handled();
    ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(
        FMovieSceneSequencePlaybackParams(FFrameTime(CutSection->GetInclusiveStartFrame()), EUpdatePositionMethod::Jump));
    ACineCameraActor* Camera = nullptr;
    for (UObject* Object : ULevelSequenceEditorBlueprintLibrary::GetBoundObjects(CutSection->GetCameraBindingID()))
    {
        Camera = Cast<ACineCameraActor>(Object);
        if (Camera) break;
    }
    if (!Camera)
    {
        SetStatus(LOCTEXT("ErrCameraNotEvaluated", "The camera has not been evaluated yet. Play the shot once and try again.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }

    FUPTCinematicShot& Shot = CurrentPlan.Shots[CurrentShotIndex];
    Shot.CameraLocation = Camera->GetActorLocation();
    Shot.CameraRotation = Camera->GetActorRotation();
    Shot.FocalLength = Camera->GetCineCameraComponent()->CurrentFocalLength;
    Shot.CameraRollDegrees = Shot.CameraRotation.Roll;
    if (const TWeakObjectPtr<AActor>* SubjectPtr = SceneActors.Find(Shot.Subject))
    {
        if (AActor* Subject = SubjectPtr->Get())
        {
            UPTFraming::DecomposeCameraOffset(Subject, Shot.FocusHeightRatio, Shot.CameraLocation, Shot.DistanceCm, Shot.SideCm, Shot.HeightCm);
            // 재생성 시 같은 구도가 나오도록 LookAt 조준점이 현재 화면 어디에 있는지도 보존한다.
            const TWeakObjectPtr<AActor>* LookAtPtr = SceneActors.Find(Shot.LookAt);
            const AActor* LookAt = LookAtPtr && LookAtPtr->IsValid() ? LookAtPtr->Get() : Subject;
            FRotator RotationWithoutRoll = Shot.CameraRotation;
            RotationWithoutRoll.Roll = 0.0f;
            FVector2D Screen;
            if (UPTFraming::ProjectToScreen(Shot.CameraLocation, RotationWithoutRoll, UPTFraming::GetFocusPoint(LookAt, Shot.FocusHeightRatio),
                Shot.FocalLength, CurrentPlan.AspectRatio, Screen))
            {
                Shot.SubjectScreenX = FMath::Clamp(static_cast<float>(Screen.X), 0.0f, 1.0f);
                Shot.SubjectScreenY = FMath::Clamp(static_cast<float>(Screen.Y), 0.0f, 1.0f);
            }
        }
    }
    if (JsonInput.IsValid()) JsonInput->SetText(FText::FromString(FUPTPlanParser::ToJson(CurrentPlan)));
    RefreshShotList();
    SetStatus(FString::Format(*LOCTEXT("CameraCaptured", "The camera framing of shot #{0} was written into the Shot Plan JSON in (2). Generate again with 'Paste Shot Plan JSON' to use it.").ToString(), {CurrentShotIndex + 1}), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::AnalyzeComposition()
{
    if (!bHasValidPlan)
    {
        SetStatus(LOCTEXT("GenerateFirst", "Generate a cinematic first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    SetReport(FUPTCompositionAnalyzer::Analyze(CurrentPlan, SceneActors));
    SetStatus(LOCTEXT("FramingCheckDone", "The per-shot framing check finished. See the results below.").ToString(), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::ApplyJsonAndRegenerate()
{
    if (IsGenerationInFlight() || !JsonInput.IsValid()) return FReply::Handled();
    RefreshCastFromSelection();
    FString Error;
    if (!ValidatePlanJson(JsonInput->GetText().ToString(), Error) || !CreateSequence(Error))
    {
        SetStatus(Error, FLinearColor::Red);
        return FReply::Handled();
    }
    SetStatus(BuildCompletionMessage(), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::OpenRetargetWindowForSelection()
{
    USkeletalMesh* Mesh = GetSelectedSkeletalMesh();
    if (!Mesh)
    {
        SetAnimationStatus(LOCTEXT("PickTargetMeshFirst", "Select the target Skeletal Mesh for the animations in the Content Browser first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FUniversalProductionToolsModule::OpenAnimationRetargetWindow(Mesh);
    SetAnimationStatus(FString::Format(*LOCTEXT("RetargetWindowOpened", "Opened the retarget window for '{0}'.").ToString(), {Mesh->GetName()}), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::AnalyzeSelectedSkeleton()
{
    USkeletalMesh* Mesh = GetSelectedSkeletalMesh();
    if (!Mesh)
    {
        SetAnimationStatus(LOCTEXT("PickMeshToCorrect", "Select the Skeletal Mesh to correct in the Content Browser.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FString OverrideJson = SkeletonOverrideInput->GetText().ToString().TrimStartAndEnd();
    if (Mesh != LastAnalyzedMesh.Get())
    {
        // 다른 메시로 바뀌면 이전 입력 대신 이 메시에 저장된 보정값을 불러온다.
        OverrideJson = FUPTSkeletonProfileBuilder::FindManualOverride(Mesh);
        SkeletonOverrideInput->SetText(FText::FromString(OverrideJson));
    }
    LastSkeletonAnalysis = FUPTSkeletonAnalyzer::AnalyzeDetailed(Mesh, OverrideJson);
    LastAnalyzedMesh = Mesh;
    LastSkeletonOverrideJson = OverrideJson;
    SkeletonReportOutput->SetText(FText::FromString(LastSkeletonAnalysis.Report));
    if (!LastSkeletonAnalysis.bOverridesValid)
        SetAnimationStatus(LOCTEXT("ErrOverrideJson", "The correction JSON has an error. Check the errors in the analysis result.").ToString(), FLinearColor::Red);
    else
        SetAnimationStatus(FString::Format(*LOCTEXT("AnalysisDone", "'{0}' analyzed - retarget readiness {1}%{2}").ToString(), {Mesh->GetName(), FString::Printf(TEXT("%.0f"), LastSkeletonAnalysis.Readiness * 100.0f), LastSkeletonAnalysis.Readiness >= 0.7f ? LOCTEXT("SaveToUse", ". Save it and the retarget window will use it.").ToString() : LOCTEXT("FixBonesTo70", ". Correct the wrong bones until it reaches 70% or more.").ToString()}),
            LastSkeletonAnalysis.Readiness >= 0.7f ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

FReply SUPTMainPanel::SaveSkeletonOverride()
{
    USkeletalMesh* Mesh = LastAnalyzedMesh.Get();
    if (!Mesh)
    {
        SetAnimationStatus(LOCTEXT("AnalyzeFirst", "Press 'Analyze selected mesh' first.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    if (SkeletonOverrideInput->GetText().ToString().TrimStartAndEnd() != LastSkeletonOverrideJson)
    {
        SetAnimationStatus(LOCTEXT("OverrideChanged", "The correction changed after the analysis. Press 'Analyze selected mesh' again, then save.").ToString(), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FString Error;
    UUPTSkeletonProfile* Profile = FUPTSkeletonProfileBuilder::CreateProfile(Mesh, LastSkeletonAnalysis, LastSkeletonOverrideJson, Error);
    if (!Profile)
    {
        SetAnimationStatus(Error, FLinearColor::Red);
        return FReply::Handled();
    }
    SetAnimationStatus(FString::Format(*LOCTEXT("OverrideSaved", "Correction saved ({0}). The retarget window applies it to '{1}' automatically. Save the asset before closing the editor.").ToString(), {Profile->GetPathName(), Mesh->GetName()}), FLinearColor::Green);
    return FReply::Handled();
}

void SUPTMainPanel::SetStatus(const FString& Message, const FSlateColor& Color)
{
    if (!StatusText.IsValid()) return;
    StatusText->SetText(FText::FromString(Message));
    StatusText->SetColorAndOpacity(Color);
}

void SUPTMainPanel::SetAnimationStatus(const FString& Message, const FSlateColor& Color)
{
    if (!AnimationStatusText.IsValid()) return;
    AnimationStatusText->SetText(FText::FromString(Message));
    AnimationStatusText->SetColorAndOpacity(Color);
}

void SUPTMainPanel::SetReport(const FString& Text)
{
    if (ReportOutput.IsValid()) ReportOutput->SetText(FText::FromString(Text));
}

FString SUPTMainPanel::BuildSceneContext() const
{
    if (SceneActors.IsEmpty()) return TEXT("none");
    TArray<FString> Lines;
    for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
    {
        const AActor* Actor = Pair.Value.Get();
        if (!Actor) continue;
        const FVector Location = Actor->GetActorLocation();
        Lines.Add(FString::Printf(TEXT("- label=%s; class=%s; location=(%.1f,%.1f,%.1f)"), *Pair.Key, *Actor->GetClass()->GetName(), Location.X, Location.Y, Location.Z));
    }
    Lines.Sort();
    return FString::Join(Lines, TEXT("\n"));
}

FString SUPTMainPanel::BuildAssetContext() const
{
    TArray<FString> Lines;
    for (const TPair<FString, TWeakObjectPtr<UAnimSequenceBase>>& Pair : Animations)
    {
        const UAnimSequenceBase* Animation = Pair.Value.Get();
        if (Animation) Lines.Add(FString::Printf(TEXT("- id=%s; type=animation; name=%s; duration=%.2fs"), *Pair.Key, *Animation->GetName(), Animation->GetPlayLength()));
    }
    for (const TPair<FString, TWeakObjectPtr<USoundBase>>& Pair : Sounds)
    {
        const USoundBase* Sound = Pair.Value.Get();
        if (Sound) Lines.Add(FString::Printf(TEXT("- id=%s; type=audio; name=%s; duration=%.2fs"), *Pair.Key, *Sound->GetName(), Sound->GetDuration()));
    }
    Lines.Sort();
    return Lines.IsEmpty() ? TEXT("none") : FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
