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
    if (Id == TEXT("extreme_close_up")) return LOCTEXT("SizeECU", "익스트림 클로즈업");
    if (Id == TEXT("close_up")) return LOCTEXT("SizeCU", "클로즈업");
    if (Id == TEXT("medium")) return LOCTEXT("SizeMedium", "미디엄");
    if (Id == TEXT("full")) return LOCTEXT("SizeFull", "풀샷");
    if (Id == TEXT("wide")) return LOCTEXT("SizeWide", "와이드");
    return FText::FromString(Id);
}

FText CameraAngleLabel(const FString& Id)
{
    if (Id == TEXT("low")) return LOCTEXT("AngleLow", "로우앵글");
    if (Id == TEXT("eye")) return LOCTEXT("AngleEye", "눈높이");
    if (Id == TEXT("high")) return LOCTEXT("AngleHigh", "하이앵글");
    if (Id == TEXT("overhead")) return LOCTEXT("AngleOverhead", "부감");
    if (Id == TEXT("dutch")) return LOCTEXT("AngleDutch", "더치(기울임)");
    return FText::FromString(Id);
}

FText CameraMotionLabel(const FString& Id)
{
    static const TMap<FString, FString> Labels = {
        { TEXT("static"), TEXT("고정") }, { TEXT("pan"), TEXT("팬") }, { TEXT("tilt"), TEXT("틸트") },
        { TEXT("dolly_in"), TEXT("돌리 인") }, { TEXT("dolly_out"), TEXT("돌리 아웃") }, { TEXT("zoom_in"), TEXT("줌 인") },
        { TEXT("zoom_out"), TEXT("줌 아웃") }, { TEXT("truck_left"), TEXT("트럭 왼쪽") }, { TEXT("truck_right"), TEXT("트럭 오른쪽") },
        { TEXT("pedestal"), TEXT("페데스탈(상승)") }, { TEXT("orbit"), TEXT("궤도") }, { TEXT("tracking"), TEXT("트래킹") },
        { TEXT("handheld"), TEXT("핸드헬드") } };
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
            Notes.Add(FString::Printf(TEXT("%s: %s '%s' → 유일한 등장인물 '%s'"), *ShotName, Field, *Value, *Labels[0]));
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
            Notes.Add(FString::Printf(TEXT("%s: subject 비어 있음 → '%s'"), *Shot.Name, *Labels[0]));
            Shot.Subject = Labels[0];
        }
        for (FString* Id : { &Shot.AnimationId, &Shot.LipSyncAnimationId })
        {
            if (!Id->IsEmpty() && !Animations.Contains(*Id)) { Notes.Add(FString::Printf(TEXT("%s: 없는 애니메이션 ID '%s' 제거"), *Shot.Name, **Id)); Id->Reset(); }
        }
        if (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId)) { Notes.Add(FString::Printf(TEXT("%s: 없는 오디오 ID '%s' 제거"), *Shot.Name, *Shot.AudioId)); Shot.AudioId.Reset(); }
        ClampField(Shot.DurationSeconds, 0.5f, 120.0f, 3.0f, Shot.Name, TEXT("duration_seconds"));
        ClampField(Shot.FocalLength, 8.0f, 200.0f, 50.0f, Shot.Name, TEXT("focal_length"));
        ClampField(Shot.DistanceCm, 30.0f, 10000.0f, 300.0f, Shot.Name, TEXT("distance_cm"));
        ClampField(Shot.HeightCm, -5000.0f, 5000.0f, 0.0f, Shot.Name, TEXT("height_cm"));
        ClampField(Shot.SideCm, -5000.0f, 5000.0f, 0.0f, Shot.Name, TEXT("side_cm"));
        ClampField(Shot.SubjectScreenX, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("subject_screen_x"));
        ClampField(Shot.SubjectScreenY, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("subject_screen_y"));
        ClampField(Shot.FocusHeightRatio, 0.0f, 1.0f, 0.5f, Shot.Name, TEXT("focus_height"));
        if (!AllowedMotions.Contains(Shot.CameraMotion)) { Notes.Add(FString::Printf(TEXT("%s: 알 수 없는 camera_motion '%s' → static"), *Shot.Name, *Shot.CameraMotion)); Shot.CameraMotion = TEXT("static"); }
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
            .ToolTipText(bWarn ? LOCTEXT("LowConfidenceTip", "분석 신뢰도가 낮은 샷입니다. 재생해 보고 '선택한 샷 수정'으로 구도를 손보세요.") : FText::GetEmpty())
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
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)[ MakeTabButton(0, LOCTEXT("CinematicTab", "시네마틱 만들기")) ]
            + SHorizontalBox::Slot().AutoWidth()[ MakeTabButton(1, LOCTEXT("AnimationTab", "애니메이션 범용화")) ]
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
                        return FText::Format(LOCTEXT("LocalModel", "모델: 로컬 {0} (API 키·크레딧 불필요)"), FText::FromString(Settings->VisionModel));
                    }
                    if (!UPTEndpoint::IsAllowed(Settings->ApiEndpoint) || !UPTEndpoint::IsAllowed(Settings->VisionApiEndpoint))
                    {
                        return FText::Format(LOCTEXT("EndpointInvalid", "모델 주소 오류: '{0}' / '{1}' (Editor Preferences > Universal Production Tools에서 확인)"),
                            FText::FromString(Settings->ApiEndpoint), FText::FromString(Settings->VisionApiEndpoint));
                    }
                    return bApiKeyReady
                        ? LOCTEXT("ApiReady", "API 키: 준비됨")
                        : FText::Format(LOCTEXT("ApiMissing", "API 키: 없음 (환경변수 {0} 설정 후 에디터 재시작)"), FText::FromString(GetDefault<UUPTSettings>()->ApiKeyEnvironmentVariable));
                })
                .ColorAndOpacity_Lambda([this] { return bApiKeyReady ? FSlateColor(FLinearColor(0.3f, 0.85f, 0.4f)) : FSlateColor(FLinearColor(1.0f, 0.4f, 0.35f)); })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    return ResolvedFFmpegPath.IsEmpty()
                        ? LOCTEXT("FFmpegMissing", "FFmpeg: 없음 (레퍼런스 영상 모드에 필요 — 설치 후 PATH 추가 또는 Editor Preferences > Universal Production Tools에서 경로 지정)")
                        : LOCTEXT("FFmpegReady", "FFmpeg: 준비됨");
                })
                .ColorAndOpacity_Lambda([this] { return ResolvedFFmpegPath.IsEmpty() ? FSlateColor(FLinearColor(1.0f, 0.4f, 0.35f)) : FSlateColor(FLinearColor(0.3f, 0.85f, 0.4f)); })
            ]
        ]

        // ① 등장인물
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)[ MakeStepHeader(LOCTEXT("Step1", "① 등장인물")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return GetCastText(); }) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("ChangeCast", "등장인물 다시 지정"))
                .ToolTipText(LOCTEXT("ChangeCastTip", "고정을 풀고 현재 레벨에서 선택한 Actor로 등장인물을 바꿉니다."))
                .Visibility_Lambda([this] { return bCastLocked ? EVisibility::Visible : EVisibility::Collapsed; })
                .OnClicked(this, &SUPTMainPanel::UnlockCast)
            ]
        ]

        // ② 소스
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step2", "② 무엇을 기반으로 만들까요?")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::ReferenceVideo; }, [this] { Source = EUPTCinematicSource::ReferenceVideo; }, LOCTEXT("SourceVideo", "레퍼런스 영상")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::PromptSearch; }, [this] { Source = EUPTCinematicSource::PromptSearch; }, LOCTEXT("SourceLibrary", "프롬프트로 레퍼런스 찾기 (라이브러리)")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
            [ MakeRadio([this] { return Source == EUPTCinematicSource::Script; }, [this] { Source = EUPTCinematicSource::Script; }, LOCTEXT("SourceScript", "텍스트 대본 (LLM)")) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ MakeRadio([this] { return Source == EUPTCinematicSource::ShotPlanJson; }, [this] { Source = EUPTCinematicSource::ShotPlanJson; }, LOCTEXT("SourceJson", "Shot Plan JSON 직접 입력 (API 불필요)")) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SVerticalBox)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::ShotPlanJson ? EVisibility::Visible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("JsonHelp", "Shot Plan JSON을 붙여넣고 '시네마틱 생성'을 누르세요. API와 FFmpeg 없이 동작합니다. 영상·대본으로 생성한 결과도 여기에 기록되므로 값을 고쳐 다시 생성할 수 있습니다.")) ]
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
                [ SNew(SButton).Text(LOCTEXT("SelectVideo", "영상 선택...")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::SelectReferenceVideo) ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                [ SNew(STextBlock).Text_Lambda([this] { return ReferenceVideoPath.IsEmpty() ? LOCTEXT("NoVideo", "선택된 영상 없음 (MP4 / MOV / M4V / AVI)") : FText::FromString(FPaths::GetCleanFilename(ReferenceVideoPath)); }) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [ SNew(STextBlock).Text(LOCTEXT("LinkLabel", "또는 영상 링크")) ]
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SAssignNew(ReferenceLinkInput, SEditableTextBox)
                    .HintText(LOCTEXT("LinkHint", "https://www.youtube.com/watch?v=... 또는 Pexels·Pixabay 영상 주소 (넣으면 위 파일 대신 링크를 씁니다)"))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
                [ SNew(STextBlock).Text(LOCTEXT("SectionLabel", "구간")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SBox).WidthOverride(110)
                    [ SAssignNew(ReferenceSectionInput, SEditableTextBox).HintText(LOCTEXT("SectionHint", "예: 1:20-2:05")) ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return bLinkRightsConfirmed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](const ECheckBoxState State) { bLinkRightsConfirmed = State == ECheckBoxState::Checked; })
                [
                    SNew(STextBlock).AutoWrapText(true)
                    .Text(LOCTEXT("LinkRights", "권리 확인: YouTube 등 다운로드를 허용하지 않는 사이트의 영상을 분석에 쓸 권리·허락이 있습니다. (저해상도로 임시로 받아 포즈 분석만 하고 영상은 바로 지웁니다. Pexels·Pixabay·Internet Archive·Wikimedia는 체크 없이 받습니다. 링크는 항상 로컬 포즈 분석으로 처리합니다.)"))
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                [ SNew(STextBlock).Text(LOCTEXT("MatchModeLabel", "분석 기준")) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
                [ MakeRadio([this] { return ReferenceMatchMode == TEXT("scene"); }, [this] { ReferenceMatchMode = TEXT("scene"); }, LOCTEXT("SceneMode", "장면 구도 (자막·로고 무시)")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ MakeRadio([this] { return ReferenceMatchMode == TEXT("edit"); }, [this] { ReferenceMatchMode = TEXT("edit"); }, LOCTEXT("EditMode", "편집 리듬 (줌·흔들림·컷 타이밍 유지)")) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                [ SNew(STextBlock).Text(LOCTEXT("AnalysisMethodLabel", "분석 방식")) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 20, 0)
                [ MakeRadio([this] { return ReferenceAnalysisMethod == EUPTReferenceAnalysisMethod::LocalPose; }, [this] { ReferenceAnalysisMethod = EUPTReferenceAnalysisMethod::LocalPose; }, LOCTEXT("LocalPoseMethod", "로컬 포즈 분석 (API 불필요, 권장)")) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ MakeRadio([this] { return ReferenceAnalysisMethod == EUPTReferenceAnalysisMethod::VisionModel; }, [this] { ReferenceAnalysisMethod = EUPTReferenceAnalysisMethod::VisionModel; }, LOCTEXT("VisionMethod", "Vision 모델 (설정된 LLM)")) ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [
            SNew(SVerticalBox)
            .Visibility_Lambda([this] { return Source == EUPTCinematicSource::PromptSearch ? EVisibility::Visible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(STextBlock).AutoWrapText(true)
                .Text(LOCTEXT("LibraryHelp", "레퍼런스 영상을 모아 둔 폴더에서, 적은 연출과 샷 구성(샷 크기·앵글·카메라 모션·인원)이 가장 비슷한 영상 구간을 찾아 레퍼런스로 씁니다. API 없이 로컬 포즈 분석으로 동작하며 폴더의 영상은 처음 한 번만 분석합니다."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [ SNew(STextBlock).Text(LOCTEXT("LibraryFolderLabel", "라이브러리 폴더")) ]
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SAssignNew(LibraryFolderInput, SEditableTextBox)
                    .Text(FText::FromString(GetDefault<UUPTSettings>()->ReferenceLibraryDirectory))
                    .HintText(LOCTEXT("LibraryFolderHint", "예: D:/RefVideos"))
                    .OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type)
                    {
                        UUPTSettings* Settings = GetMutableDefault<UUPTSettings>();
                        Settings->ReferenceLibraryDirectory = Text.ToString().TrimStartAndEnd();
                        Settings->SaveConfig();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
                [ SNew(SButton).Text(LOCTEXT("BrowseLibrary", "폴더 선택...")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::BrowseLibraryFolder) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SBox).HeightOverride(70)
                [ SAssignNew(LibraryPromptInput, SMultiLineEditableTextBox).HintText(LOCTEXT("LibraryPromptHint", "예: 두 사람이 투샷으로 서 있다가 어깨 너머로 대화하고, 한 사람 클로즈업으로 마무리")) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(LOCTEXT("SearchLibrary", "검색만 하기")).IsEnabled_Lambda([this] { return !IsGenerationInFlight(); }).OnClicked(this, &SUPTMainPanel::SearchLibraryOnly) ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
                [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("LibraryGenerateHint", "'시네마틱 생성'은 목록에서 고른 결과(고르지 않으면 1위)로 바로 만듭니다. 목록이 비었거나 폴더·프롬프트가 바뀌었으면 먼저 검색합니다.")) ]
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
                .HintText(LOCTEXT("ScriptHint", "예: 폐허가 된 신전. 주인공이 제단으로 걸어가고 여신이 나타난다. 와이드 샷으로 시작해 여신 얼굴 클로즈업으로 전환."))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 10, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
            [ SNew(STextBlock).Text(LOCTEXT("MediaLabel", "애니메이션·오디오 (선택)")) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("AddMedia", "Content Browser 선택 항목 추가")).OnClicked(this, &SUPTMainPanel::AddSelectedMedia) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("ClearMedia", "비우기")).OnClicked(this, &SUPTMainPanel::ClearMedia) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 4, 0, 0)
        [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return GetMediaText(); }) ]

        // ③ 생성
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step3", "③ 생성")) ]
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
                    .Text_Lambda([this] { return IsGenerationInFlight() ? LOCTEXT("Generating", "생성 중...") : LOCTEXT("Generate", "시네마틱 생성"); })
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
            [ SAssignNew(StatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("Ready", "등장인물과 레퍼런스 영상(또는 대본)을 준비한 뒤 생성하세요.")) ]
        ]

        // ④ 결과 검토
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ MakeStepHeader(LOCTEXT("Step4", "④ 결과 검토")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text_Lambda([this]
                {
                    const ULevelSequence* Sequence = LastCreatedSequence.Get();
                    return Sequence ? FText::FromString(Sequence->GetPathName()) : LOCTEXT("NoSequence", "아직 생성된 시네마틱이 없습니다.");
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("OpenSequence", "Sequencer에서 열기")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::OpenLastSequence) ]
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
                    + SHeaderRow::Column(ShotColumnName).DefaultLabel(LOCTEXT("ColName", "샷")).FillWidth(0.3f)
                    + SHeaderRow::Column(ShotColumnDuration).DefaultLabel(LOCTEXT("ColDuration", "길이")).FixedWidth(64.0f)
                    + SHeaderRow::Column(ShotColumnLens).DefaultLabel(LOCTEXT("ColLens", "렌즈")).FixedWidth(64.0f)
                    + SHeaderRow::Column(ShotColumnSubject).DefaultLabel(LOCTEXT("ColSubject", "피사체")).FillWidth(0.25f)
                    + SHeaderRow::Column(ShotColumnScreen).DefaultLabel(LOCTEXT("ColScreen", "화면 위치")).FixedWidth(90.0f)
                    + SHeaderRow::Column(ShotColumnMotion).DefaultLabel(LOCTEXT("ColMotion", "카메라 모션")).FillWidth(0.2f)
                    + SHeaderRow::Column(ShotColumnConfidence).DefaultLabel(LOCTEXT("ColConfidence", "신뢰도")).FixedWidth(72.0f)
                )
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("PlayShot", "선택 샷 재생")).ToolTipText(LOCTEXT("PlayShotTip", "샷 목록에서 행을 더블클릭해도 재생됩니다.")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::PlaySelectedShot) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("CaptureCamera", "수정한 카메라 구도 보존")).ToolTipText(LOCTEXT("CaptureCameraTip", "Sequencer에서 카메라를 옮긴 뒤 누르면 다음 생성 때 같은 구도가 유지됩니다.")).IsEnabled_Lambda([this] { return LastCreatedSequence.IsValid(); }).OnClicked(this, &SUPTMainPanel::CaptureCurrentShotCamera) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
            [ SNew(SButton).Text(LOCTEXT("AnalyzeComposition", "구도 점검")).ToolTipText(LOCTEXT("AnalyzeCompositionTip", "머리 잘림, 피사체 크기, 가림 여부를 샷별로 점검합니다.")).IsEnabled_Lambda([this] { return bHasValidPlan; }).OnClicked(this, &SUPTMainPanel::AnalyzeComposition) ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("OpenPoseDebug", "포즈 분석 이미지 열기"))
                .ToolTipText(LOCTEXT("OpenPoseDebugTip", "레퍼런스 영상에서 검출한 인물 박스·관절·조준점을 샷별 이미지로 확인합니다."))
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
                return FText::Format(LOCTEXT("ShotEditTitle", "선택한 샷 수정 — {0}번 {1}"), FText::AsNumber(CurrentShotIndex + 1),
                    FText::FromString(CurrentPlan.Shots.IsValidIndex(CurrentShotIndex) ? CurrentPlan.Shots[CurrentShotIndex].Name : FString()));
            })
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(STextBlock).AutoWrapText(true)
                    .Text(LOCTEXT("ShotEditHelp", "샷 목록에서 고른 샷만 고칩니다. 샷 크기·앵글을 바꾸면 렌즈·거리·높이를 레퍼런스 생성과 같은 식으로 다시 계산하고, 숫자 칸으로 미세 조정할 수 있습니다. '이 샷에 적용'을 누르면 같은 시퀀스를 그 자리에서 갱신하고 그 샷을 재생합니다."))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditSize", "샷 크기"), MakeOptionCombo(&ShotSizeOptions, [this] { return EditShotSize; }, [this](const FString& Id) { PreviewShotSize(Id); }, &ShotSizeLabel), 140.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditAngle", "앵글"), MakeOptionCombo(&CameraAngleOptions, [this] { return EditCameraAngle; }, [this](const FString& Id) { EditCameraAngle = Id; bEditFramingChanged = true; }, &CameraAngleLabel), 120.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditMotion", "카메라 모션"), MakeOptionCombo(&CameraMotionOptions, [this] { return EditCameraMotion; }, [this](const FString& Id) { EditCameraMotion = Id; }, &CameraMotionLabel), 130.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [
                        MakeLabeledField(LOCTEXT("EditSubject", "대상 인물"),
                            SAssignNew(SubjectCombo, SComboBox<TSharedPtr<FString>>)
                            .OptionsSource(&SubjectOptions)
                            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Option) { return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString())); })
                            .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Option, ESelectInfo::Type) { if (Option.IsValid()) EditSubject = *Option; })
                            [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(EditSubject.IsEmpty() ? TEXT("(없음)") : EditSubject); }) ],
                            160.0f)
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditDuration", "길이(초)"), MakeFloatSpin(0.1f, 120.0f, 0.1f, [this] { return EditDuration; }, [this](const float Value) { EditDuration = Value; }), 80.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditFocal", "렌즈(mm)"), MakeFloatSpin(8.0f, 200.0f, 1.0f, [this] { return EditFocalLength; }, [this](const float Value) { EditFocalLength = Value; }), 80.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [
                        MakeLabeledField(LOCTEXT("EditOverShoulder", "어깨 너머 앞사람"),
                            SAssignNew(OverShoulderCombo, SComboBox<TSharedPtr<FString>>)
                            .ToolTipText(LOCTEXT("EditOverShoulderTip", "고르면 카메라를 그 Actor의 어깨 뒤에 두고 주 피사체를 찍습니다. 거리·방위각 대신 앞사람 위치로 카메라를 계산하고, 렌즈로 샷 크기를 유지합니다."))
                            .OptionsSource(&OverShoulderOptions)
                            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Option) { return SNew(STextBlock).Text(FText::FromString(Option.IsValid() && !Option->IsEmpty() ? *Option : FString(TEXT("(일반 샷)")))); })
                            .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Option, ESelectInfo::Type) { if (Option.IsValid()) EditOverShoulderActor = *Option; })
                            [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(EditOverShoulderActor.IsEmpty() ? TEXT("(일반 샷)") : EditOverShoulderActor); }) ],
                            160.0f)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        MakeLabeledField(LOCTEXT("EditOverShoulderSide", "앞사람 위치"),
                            MakeOptionCombo(&OverShoulderSideOptions, [this] { return EditOverShoulderSide; }, [this](const FString& Id) { EditOverShoulderSide = Id; },
                                [](const FString& Id) { return Id == TEXT("left") ? LOCTEXT("OverShoulderLeft", "화면 왼쪽") : LOCTEXT("OverShoulderRight", "화면 오른쪽"); }),
                            100.0f)
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditScreenX", "화면 가로"), MakeFloatSpin(0.02f, 0.98f, 0.01f, [this] { return EditScreenX; }, [this](const float Value) { EditScreenX = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditScreenY", "화면 세로"), MakeFloatSpin(0.02f, 0.98f, 0.01f, [this] { return EditScreenY; }, [this](const float Value) { EditScreenY = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
                    [ MakeLabeledField(LOCTEXT("EditAzimuth", "방위각(°)"), MakeFloatSpin(-75.0f, 75.0f, 1.0f, [this] { return EditAzimuth; }, [this](const float Value) { EditAzimuth = Value; }), 70.0f) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ MakeLabeledField(LOCTEXT("EditDistance", "거리(cm)"), MakeFloatSpin(50.0f, 10000.0f, 5.0f, [this] { return EditDistance; }, [this](const float Value) { EditDistance = Value; }), 90.0f) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                    [
                        SNew(STextBlock).Text_Lambda([this]
                        {
                            const TWeakObjectPtr<UAnimSequenceBase>* Animation = Animations.Find(EditAnimationId);
                            const FString Name = EditAnimationId.IsEmpty() ? TEXT("없음") : (Animation && Animation->IsValid() ? Animation->Get()->GetName() : EditAnimationId);
                            return FText::Format(LOCTEXT("EditAnimation", "애니메이션: {0}"), FText::FromString(Name));
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(LOCTEXT("AssignAnimation", "Content Browser 선택 애니메이션 적용")).OnClicked(this, &SUPTMainPanel::AssignSelectedAnimationToShot) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("ClearAnimation", "애니메이션 빼기")).OnClicked(this, &SUPTMainPanel::ClearShotAnimation) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4, 6, 4, 4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("ApplyShotEdit", "이 샷에 적용하고 시퀀스 갱신"))
                        .IsEnabled_Lambda([this] { return !IsGenerationInFlight() && LastCreatedSequence.IsValid(); })
                        .OnClicked(this, &SUPTMainPanel::ApplyShotEdit)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("ResetShotEdit", "되돌리기(현재 값 다시 읽기)")).OnClicked_Lambda([this] { LoadShotEditor(); return FReply::Handled(); }) ]
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
        [
            SNew(SBox)
            .HeightOverride(120)
            [ SAssignNew(ReportOutput, SMultiLineEditableTextBox).IsReadOnly(true).HintText(LOCTEXT("ReportHint", "분석 결과와 오류 상세가 여기에 표시됩니다.")) ]
        ]

        // 고급
        + SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 8)
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(true)
            .AreaTitle(LOCTEXT("Advanced", "고급 설정 (역할 매핑)"))
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [ SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("RoleHelp", "역할 → Actor 매핑 — 영상 속 인물이 엉뚱한 Actor에 배정될 때만 입력하세요.")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(58)
                    [ SAssignNew(ReferenceRoleMappingInput, SMultiLineEditableTextBox).HintText(LOCTEXT("RoleHint", "예: {\"main_character\":\"Hero\",\"enemy\":\"Boss\"}")) ]
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
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 6)[ MakeStepHeader(LOCTEXT("AnimTitle", "다른 캐릭터의 애니메이션을 내 캐릭터에 맞게 변환")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 8)
        [
            SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("AnimGuide",
                "1. Content Browser에서 애니메이션을 적용할 대상 Skeletal Mesh를 선택합니다.\n"
                "2. 아래 버튼(또는 메시 우클릭 > 애니메이션 범용화)으로 창을 열고, 변환할 애니메이션을 골라 일괄 리타기팅합니다.\n"
                "MetaHuman · UE Mannequin · Mixamo 등 휴머노이드 본 구조는 자동으로 인식합니다."))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .ContentPadding(FMargin(28.0f, 8.0f))
                .OnClicked(this, &SUPTMainPanel::OpenRetargetWindowForSelection)
                [ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)).Text(LOCTEXT("OpenRetarget", "선택한 Skeletal Mesh로 범용화 창 열기")) ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
            [ SAssignNew(AnimationStatusText, STextBlock).AutoWrapText(true).Text(LOCTEXT("AnimReady", "준비됨")) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 8)
        [
            SNew(SExpandableArea)
            .InitiallyCollapsed(true)
            .AreaTitle(LOCTEXT("BoneFixTitle", "고급: 본 매핑 수동 보정 (자동 인식 실패 시)"))
            .BodyContent()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("BoneFixHelp",
                        "범용화 창에서 '본 구조 인식 실패'가 뜬 메시에만 사용합니다. Content Browser에서 그 메시를 선택해 분석하고, "
                        "틀린 본만 JSON으로 고쳐 저장하면 범용화 창이 자동으로 이 보정값을 사용합니다."))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(LOCTEXT("AnalyzeMesh", "선택 메시 분석")).OnClicked(this, &SUPTMainPanel::AnalyzeSelectedSkeleton) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("SaveOverride", "보정값 저장")).OnClicked(this, &SUPTMainPanel::SaveSkeletonOverride) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(80)
                    [ SAssignNew(SkeletonOverrideInput, SMultiLineEditableTextBox).HintText(LOCTEXT("OverrideHint", "틀린 본만 입력. 예: {\"Pelvis\":\"pelvis\",\"LeftHand\":\"hand_l\"}")) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4)
                [
                    SNew(SBox).HeightOverride(220)
                    [ SAssignNew(SkeletonReportOutput, SMultiLineEditableTextBox).IsReadOnly(true).HintText(LOCTEXT("SkeletonReportHint", "분석 결과가 여기에 표시됩니다.")) ]
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
    SetStatus(TEXT("등장인물 고정을 풀었습니다. 레벨에서 선택한 Actor가 등장인물로 반영됩니다."), FLinearColor::Green);
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
    if (Labels.IsEmpty()) return LOCTEXT("NoCast", "레벨 뷰포트에서 등장인물 Actor를 선택하세요 (여러 명은 Ctrl+클릭).");
    return FText::FromString(FString::Join(Labels, TEXT(", ")) + (bCastLocked
        ? TEXT("   — 마지막 생성에 사용 중 (고정됨)")
        : TEXT("   — 레벨 선택이 자동 반영됩니다")));
}

FReply SUPTMainPanel::SelectReferenceVideo()
{
    if (IsGenerationInFlight()) return FReply::Handled();
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform)
    {
        SetStatus(TEXT("파일 선택기를 사용할 수 없습니다."), FLinearColor::Red);
        return FReply::Handled();
    }
    TArray<FString> SelectedFiles;
    const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("레퍼런스 영상 선택"),
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
    SetStatus(TEXT("레퍼런스 영상을 선택했습니다. 원본 영상은 외부로 전송되지 않고 축소 프레임만 분석에 사용됩니다."), FLinearColor::Green);
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
        ? FString::Printf(TEXT("애니메이션·오디오 %d개를 추가했습니다."), Added)
        : TEXT("Content Browser에서 Animation Sequence/Montage 또는 Sound를 선택한 뒤 누르세요."),
        Added > 0 ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

FReply SUPTMainPanel::ClearMedia()
{
    Animations.Reset();
    Sounds.Reset();
    SetStatus(TEXT("애니메이션·오디오 목록을 비웠습니다."));
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
        ? LOCTEXT("NoMedia", "없음 — 캐릭터 동작이나 대사를 넣으려면 Content Browser에서 선택 후 추가하세요.")
        : FText::FromString(FString::Join(Labels, TEXT(", ")));
}

FReply SUPTMainPanel::GenerateCinematic()
{
    if (IsGenerationInFlight()) return FReply::Handled();
    RefreshEnvironmentStatus();
    RefreshCastFromSelection();
    if (SceneActors.IsEmpty())
    {
        SetStatus(TEXT("① 등장인물을 먼저 선택하세요. 레벨 뷰포트에서 캐릭터 Actor를 클릭하면 됩니다."), FLinearColor::Yellow);
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
            SetStatus(TEXT("② 찾고 싶은 연출을 글로 적어 주세요. 예: 투샷으로 시작해 어깨 너머로 대화, 클로즈업으로 마무리"), FLinearColor::Yellow);
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
            SetStatus(FString::Printf(TEXT("[2/4] 검색 결과 %d위 '%s' (%s)를 레퍼런스로 씁니다."), Item->Rank, *Item->Video, *Item->Segment));
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
            SetStatus(TEXT("② Shot Plan JSON 칸에 JSON을 붙여넣으세요."), FLinearColor::Yellow);
            return FReply::Handled();
        }
        return ApplyJsonAndRegenerate();
    }

    if (Source == EUPTCinematicSource::Script)
    {
        const FString Script = ScriptInput.IsValid() ? ScriptInput->GetText().ToString().TrimStartAndEnd() : FString();
        if (Script.IsEmpty())
        {
            SetStatus(TEXT("② 대본이나 연출 지시를 입력하세요."), FLinearColor::Yellow);
            return FReply::Handled();
        }
        if (!bApiKeyReady)
        {
            SetStatus(TEXT("API 키가 없어 LLM을 호출할 수 없습니다. 상단 안내에 따라 환경변수를 설정하세요."), FLinearColor::Red);
            return FReply::Handled();
        }
        bGenerationInFlight = true;
        SetStatus(TEXT("[1/3] LLM이 샷 구성을 만드는 중입니다..."));
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
        SetStatus(TEXT("[1/4] 링크 영상을 받아 로컬 포즈 분석 중입니다 (다운로드 → 컷 검출 → 인물·관절 검출). 영상 길이와 인터넷 속도에 따라 몇 분 걸릴 수 있습니다..."));
        FUPTPoseReferenceAnalyzer::AnalyzeLink(ReferenceLink, Section, bLinkRightsConfirmed, ReferenceMatchMode,
            FUPTPoseAnalysisComplete::CreateSP(this, &SUPTMainPanel::OnPoseAnalysisCompleted));
        return FReply::Handled();
    }
    if (!FUPTReferenceVideoProcessor::ValidateVideo(ReferenceVideoPath, Error))
    {
        SetStatus(ReferenceVideoPath.IsEmpty() ? TEXT("② '영상 선택...'으로 레퍼런스 영상을 고르세요.") : Error, FLinearColor::Yellow);
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
        SetStatus(TEXT("[1/4] 로컬 포즈 분석 중입니다 (컷 검출 → 인물·관절 검출). 영상 길이에 따라 수십 초~몇 분 걸립니다..."));
        FUPTPoseReferenceAnalyzer::Analyze(ReferenceVideoPath, ReferenceMatchMode, FUPTPoseAnalysisComplete::CreateSP(this, &SUPTMainPanel::OnPoseAnalysisCompleted));
        return FReply::Handled();
    }
    if (ResolvedFFmpegPath.IsEmpty())
    {
        SetStatus(TEXT("FFmpeg를 찾지 못했습니다. 상단 안내에 따라 설치하거나 경로를 지정하세요."), FLinearColor::Red);
        return FReply::Handled();
    }

    bGenerationInFlight = true;
    bHasReferencePlan = false;
    SetStatus(TEXT("[1/4] 영상에서 대표 프레임과 컷 경계를 추출하는 중입니다..."));
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
        SetStatus(TEXT("[2/4] 같은 영상의 이전 분석 결과를 재사용합니다."));
        ContinueReferencePipeline();
        return;
    }
    if (!bApiKeyReady)
    {
        FinishGeneration(TEXT("영상 분석에 필요한 API 키가 없습니다. 상단 안내에 따라 환경변수를 설정하세요."), FLinearColor::Red);
        return;
    }
    SetStatus(FString::Printf(TEXT("[2/4] %d개 컷 후보를 Vision 모델로 분석하는 중입니다..."), Result.ShotBoundaries.Num()));
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
        FinishGeneration(FString::Printf(TEXT("영상 분석 결과 형식이 올바르지 않습니다: %s"), *Error), FLinearColor::Red);
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
        FinishGeneration(FString::Printf(TEXT("포즈 분석 결과 형식이 올바르지 않습니다: %s"), *ParseError), FLinearColor::Red);
        return;
    }
    ParsedPlan.MatchMode = ReferenceMatchMode;
    CurrentReferencePlan = MoveTemp(ParsedPlan);
    bHasReferencePlan = true;
    LastPoseDebugDirectory = Result.DebugDirectory;
    LastReferencePlanFile = Result.PlanFile;
    UE_LOG(LogUPTPanel, Log, TEXT("Pose reference plan: %s (cache=%d)"), *Result.PlanFile, Result.bFromCache ? 1 : 0);
    SetStatus(Result.bFromCache ? TEXT("[2/4] 같은 영상의 포즈 분석 결과를 재사용합니다.") : TEXT("[2/4] 포즈 분석을 마쳤습니다."));
    ContinueReferencePipeline();
}

FReply SUPTMainPanel::BrowseLibraryFolder()
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform) return FReply::Handled();
    FString Folder;
    const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (DesktopPlatform->OpenDirectoryDialog(ParentWindow, TEXT("레퍼런스 영상 라이브러리 폴더"), GetDefault<UUPTSettings>()->ReferenceLibraryDirectory, Folder))
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
        SetStatus(TEXT("② 찾고 싶은 연출을 글로 적어 주세요."), FLinearColor::Yellow);
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
        SetStatus(TEXT("② '폴더 선택...'으로 레퍼런스 영상을 모아 둔 폴더를 지정하세요."), FLinearColor::Yellow);
        return;
    }
    bGenerationInFlight = true;
    PendingLibraryKey = GetLibraryQueryKey();
    SetStatus(TEXT("[1/4] 레퍼런스 라이브러리를 검색하는 중입니다. 처음이거나 영상을 새로 넣었다면 영상마다 포즈 분석을 하므로 몇 분 걸릴 수 있습니다..."));
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
        FinishGeneration(TEXT("라이브러리 검색 결과를 읽지 못했습니다."), FLinearColor::Red);
        return;
    }

    // 프롬프트를 어떤 샷 요구로 이해했는지 먼저 보여 준다(원하는 결과가 안 나오면 표현을 바꾸는 근거가 된다).
    LibraryReport = TEXT("[프롬프트 해석]\n");
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
            if (Request->TryGetBoolField(TEXT("ots"), bOverShoulder) && bOverShoulder) Parts.Add(TEXT("어깨 너머"));
            else if (Request->TryGetNumberField(TEXT("people"), People)) Parts.Add(FString::Printf(TEXT("%d명"), FMath::RoundToInt(People)));
            FString Text;
            Request->TryGetStringField(TEXT("text"), Text);
            LibraryReport += FString::Printf(TEXT("%d) \"%s\" → %s\n"), ++RequestCount, *Text, *FString::Join(Parts, TEXT(" / ")));
        }
    }
    if (RequestCount == 0) LibraryReport += TEXT("샷 크기·앵글·카메라 모션·인원 표현을 찾지 못해 파일 이름 키워드로만 찾았습니다.\n");

    LibraryReport += TEXT("\n[검색 결과]\n");
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
                Item->Segment = FString::Printf(TEXT("샷 %d~%d (%.1f~%.1f초)"), FMath::RoundToInt(StartShot), FMath::RoundToInt(EndShot), StartSeconds, EndSeconds);
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
            LibraryReport += FString::Printf(TEXT("%d위 %.0f점  %s  %s  맞춘 샷: %s\n"), Item->Rank, Item->Score, *Item->Video, *Item->Segment, *Item->Matches);
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
        FinishGeneration(TEXT("라이브러리에서 비슷한 구간을 찾지 못했습니다. 표현을 바꾸거나 영상을 더 넣어 보세요."), FLinearColor::Yellow);
        return;
    }
    if (!bContinueToGeneration)
    {
        FinishGeneration(FString::Printf(TEXT("검색 완료: %d개 결과. 목록에서 고른 뒤(기본 1위) '시네마틱 생성'을 누르세요."), LibraryResults.Num()), FLinearColor::Green);
        return;
    }
    FString LoadError;
    if (!LoadLibraryResult(LibraryResults[0], LoadError))
    {
        FinishGeneration(LoadError, FLinearColor::Red);
        return;
    }
    SetStatus(FString::Printf(TEXT("[2/4] 1위 '%s' (%s, %.0f점)를 레퍼런스로 씁니다."), *LibraryResults[0]->Video, *LibraryResults[0]->Segment, LibraryResults[0]->Score));
    ContinueReferencePipeline();
}

bool SUPTMainPanel::LoadLibraryResult(const TSharedPtr<FUPTLibraryResultItem>& Item, FString& OutError)
{
    FString Json;
    if (!Item.IsValid() || !FFileHelper::LoadFileToString(Json, *Item->ReferencePlanFile))
    {
        OutError = TEXT("검색 결과의 레퍼런스 구간 파일을 읽지 못했습니다. 다시 검색하세요.");
        return false;
    }
    FUPTReferencePlan Parsed;
    FString ParseError;
    if (!FUPTReferencePlanParser::Parse(Json, Parsed, ParseError))
    {
        OutError = FString::Printf(TEXT("레퍼런스 구간 형식이 올바르지 않습니다: %s"), *ParseError);
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
            [ SNew(SBox).WidthOverride(32)[ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d위"), Item->Rank))) ] ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [ SNew(SBox).WidthOverride(48)[ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%.0f점"), Item->Score))) ] ]
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
    SetStatus(TEXT("[3/4] 영상 속 인물을 등장인물에 배정하고 카메라를 계산하는 중입니다..."));
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
        PendingGenerationWarning += FString::Printf(TEXT(" [주의] 분석 신뢰도가 낮은 샷 %d개: %s — 재생해 보고 '선택한 샷 수정'으로 손보세요."),
            LowConfidenceShots.Num(), *FString::Join(LowConfidenceShots, TEXT(", ")));
    }
    if (EmptyShots > 0)
    {
        // 인물 없는 샷(원경·인서트·타이틀)은 첫 등장인물을 기준으로 영상의 샷 크기대로 찍는다.
        PendingGenerationWarning += FString::Printf(TEXT(" 인물이 없는 샷 %d개(원경·인서트 등)는 첫 등장인물 기준으로 배치했습니다."), EmptyShots);
    }

    // 구도 검증은 로컬 포즈 분석기가 만든 Reference Plan(영상 분석 결과나 라이브러리 검색 구간)이 있을 때만 할 수 있다.
    if (LastReferencePlanFile.IsEmpty() || !FPaths::FileExists(LastReferencePlanFile))
    {
        SetStatus(TEXT("[4/4] Level Sequence를 만드는 중입니다..."));
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
    SetStatus(FString::Printf(TEXT("[4/4] 카메라 구도를 레퍼런스와 비교하는 중입니다... (측정 1/%d)"), MaxCorrections + 1));
    TWeakPtr<SWidget> WeakPanel = AsShared();
    FUPTPoseReferenceAnalyzer::RefineShotCameras(CurrentPlan, SceneActors, ReferencePlanFile, MaxCorrections,
        FUPTPoseRefineProgress::CreateLambda([WeakPanel](const int32 Pass, const double Score)
        {
            const TSharedPtr<SWidget> Pinned = WeakPanel.Pin();
            if (!Pinned.IsValid()) return;
            static_cast<SUPTMainPanel*>(Pinned.Get())->SetStatus(FString::Printf(
                TEXT("[4/4] 카메라 구도 자동 보정 중... 측정 %d/%d, 현재 점수 %.0f"), Pass + 1, MaxCorrections + 1, Score));
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

            Panel->SetStatus(TEXT("[4/4] Level Sequence를 만드는 중입니다..."));
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
                Panel->FinishGeneration(CompletionMessage + TEXT(" (구도 검증 실패: ") + Result.Left(300) + TEXT(")"), FLinearColor::Yellow);
                return;
            }
            const FString Previous = Panel->ReportOutput.IsValid() ? Panel->ReportOutput->GetText().ToString() : FString();
            Panel->SetReport(ReportText + TEXT("\n") + Previous);
            Panel->FinishGeneration(FString::Printf(TEXT("%s — 레퍼런스 대비 구도 점수 %.0f / 100"), *CompletionMessage, Score),
                Score >= 70.0 ? FLinearColor::Green : FLinearColor::Yellow);
        }));
}

bool SUPTMainPanel::ApplyReferenceBlocking(FString& OutError)
{
    if (!bHasReferencePlan)
    {
        OutError = TEXT("영상 분석 결과가 없습니다.");
        return false;
    }
    TMap<FString, FString> ExplicitMappings;
    const FString MappingJson = ReferenceRoleMappingInput.IsValid() ? ReferenceRoleMappingInput->GetText().ToString().TrimStartAndEnd() : FString();
    if (!MappingJson.IsEmpty())
    {
        TSharedPtr<FJsonObject> MappingObject;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MappingJson), MappingObject) || !MappingObject.IsValid())
        {
            OutError = TEXT("고급 설정의 역할 매핑 JSON 형식이 올바르지 않습니다.");
            return false;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : MappingObject->Values)
        {
            FString ActorLabel;
            if (!Field.Value.IsValid() || !Field.Value->TryGetString(ActorLabel) || Field.Key.IsEmpty() || ActorLabel.IsEmpty())
            {
                OutError = FString::Printf(TEXT("역할 매핑 '%s'의 값은 비어 있지 않은 Actor 라벨 문자열이어야 합니다."), *Field.Key);
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
    SetStatus(TEXT("[2/3] 샷 구성을 검증하는 중입니다..."));

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
        FinishGeneration(FString::Printf(TEXT("LLM이 만든 샷 구성에 문제가 있습니다 — %s (②의 Shot Plan JSON을 고쳐 다시 생성할 수 있습니다)"), *Error), FLinearColor::Red);
        return;
    }
    SetStatus(TEXT("[3/3] Level Sequence를 만드는 중입니다..."));
    if (!CreateSequence(Error))
    {
        FinishGeneration(Error, FLinearColor::Red);
        return;
    }
    if (!RepairNotes.IsEmpty() && ReportOutput.IsValid())
    {
        SetReport(TEXT("[자동 보정]\n") + FString::Join(RepairNotes, TEXT("\n")) + TEXT("\n\n") + ReportOutput->GetText().ToString());
    }
    FinishGeneration(BuildCompletionMessage() + (RepairNotes.IsEmpty() ? FString() : FString::Printf(TEXT(" (LLM 결과 %d곳 자동 보정)"), RepairNotes.Num())), FLinearColor::Green);
}

bool SUPTMainPanel::ValidatePlanJson(const FString& JsonText, FString& OutError)
{
    FUPTCinematicPlan ParsedPlan;
    FString ParseError;
    if (!FUPTPlanParser::Parse(JsonText, ParsedPlan, ParseError))
    {
        SetReport(ParseError);
        UE_LOG(LogUPTPanel, Warning, TEXT("Shot Plan parse failed: %s"), *ParseError);
        OutError = FString::Printf(TEXT("Shot Plan JSON을 읽지 못했습니다: %s"), *ParseError);
        return false;
    }

    const FUPTPlanValidationResult Validation = FUPTPlanParser::Validate(ParsedPlan);
    TArray<FString> Problems = Validation.Errors;
    for (const FUPTCinematicShot& Shot : ParsedPlan.Shots)
    {
        if (!Shot.Subject.IsEmpty() && !SceneActors.Contains(Shot.Subject))
            Problems.Add(FString::Printf(TEXT("%s: subject '%s'가 등장인물에 없습니다."), *Shot.Name, *Shot.Subject));
        if (!Shot.LookAt.IsEmpty() && !SceneActors.Contains(Shot.LookAt))
            Problems.Add(FString::Printf(TEXT("%s: look_at '%s'가 등장인물에 없습니다."), *Shot.Name, *Shot.LookAt));
        if (!Shot.AnimationId.IsEmpty() && !Animations.Contains(Shot.AnimationId))
            Problems.Add(FString::Printf(TEXT("%s: animation_id '%s'가 애니메이션 목록에 없습니다."), *Shot.Name, *Shot.AnimationId));
        if (!Shot.LipSyncAnimationId.IsEmpty() && !Animations.Contains(Shot.LipSyncAnimationId))
            Problems.Add(FString::Printf(TEXT("%s: lip_sync_animation_id '%s'가 애니메이션 목록에 없습니다."), *Shot.Name, *Shot.LipSyncAnimationId));
        if (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId))
            Problems.Add(FString::Printf(TEXT("%s: audio_id '%s'가 오디오 목록에 없습니다."), *Shot.Name, *Shot.AudioId));
    }
    if (!Problems.IsEmpty())
    {
        SetReport(FString::Join(Problems, TEXT("\n")));
        UE_LOG(LogUPTPanel, Warning, TEXT("Shot Plan validation failed:\n%s"), *FString::Join(Problems, TEXT("\n")));
        OutError = Problems.Num() == 1
            ? FString::Printf(TEXT("검증 실패: %s"), *Problems[0])
            : FString::Printf(TEXT("검증 실패: %s 외 %d건 (아래 상세 참고)"), *Problems[0], Problems.Num() - 1);
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
        OutError = TEXT("생성할 Shot Plan이 없습니다.");
        return false;
    }
    const FUPTBlockingValidationResult BlockingValidation = FUPTBlockingValidator::Validate(CurrentPlan);
    if (!BlockingValidation.IsValid())
    {
        SetReport(BlockingValidation.ToText());
        OutError = TEXT("샷 구성 안전성 검증에 실패했습니다. 아래 상세를 확인하세요.");
        return false;
    }
    for (const FUPTCinematicShot& Shot : CurrentPlan.Shots)
    {
        if ((!Shot.Subject.IsEmpty() && !SceneActors.Contains(Shot.Subject)) || (!Shot.LookAt.IsEmpty() && !SceneActors.Contains(Shot.LookAt)) ||
            (!Shot.AnimationId.IsEmpty() && !Animations.Contains(Shot.AnimationId)) ||
            (!Shot.LipSyncAnimationId.IsEmpty() && !Animations.Contains(Shot.LipSyncAnimationId)) ||
            (!Shot.AudioId.IsEmpty() && !Sounds.Contains(Shot.AudioId)))
        {
            OutError = TEXT("등장인물이나 애니메이션·오디오 목록이 Shot Plan과 맞지 않습니다. 목록을 확인한 뒤 다시 생성하세요.");
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
    return FString::Printf(TEXT("완료: %s — %d개 샷. 샷 목록을 더블클릭하면 해당 구간을 재생합니다.%s"),
        Sequence ? *Sequence->GetName() : TEXT("Level Sequence"), CurrentPlan.Shots.Num(), *PendingGenerationWarning);
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
            Item->Duration = FString::Printf(TEXT("%.1f초"), Shot.DurationSeconds);
            Item->Lens = FString::Printf(TEXT("%.0fmm"), Shot.FocalLength);
            Item->Subject = Shot.Subject.IsEmpty() ? TEXT("(절대 좌표)") : Shot.Subject;
            Item->Screen = Shot.Subject.IsEmpty() ? TEXT("-") : FString::Printf(TEXT("%.2f, %.2f"), Shot.SubjectScreenX, Shot.SubjectScreenY);
            Item->Motion = Shot.CameraMotion.IsEmpty() ? TEXT("static") : Shot.CameraMotion;
            if (ShotConfidences.Num() == CurrentPlan.Shots.Num())
            {
                const float Confidence = ShotConfidences[Index];
                Item->Confidence = Confidence < 0.0f ? TEXT("인물 없음") : FString::Printf(TEXT("%.0f%%"), Confidence * 100.0f);
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
        SetStatus(TEXT("Content Browser에서 Animation Sequence 또는 Montage를 선택한 뒤 누르세요."), FLinearColor::Yellow);
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
        SetStatus(FString::Printf(TEXT("'%s'는 '%s'와 스켈레톤이 달라 그대로는 재생되지 않습니다. '애니메이션 범용화' 탭에서 리타기팅한 애니메이션을 고르세요."),
            *Animation->GetName(), *EditSubject), FLinearColor::Yellow);
        return FReply::Handled();
    }
    SetStatus(FString::Printf(TEXT("'%s'를 %d번 샷 애니메이션으로 골랐습니다. '이 샷에 적용하고 시퀀스 갱신'을 누르면 반영됩니다."), *Animation->GetName(), CurrentShotIndex + 1), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::ClearShotAnimation()
{
    EditAnimationId.Empty();
    SetStatus(TEXT("애니메이션을 뺐습니다. '이 샷에 적용하고 시퀀스 갱신'을 누르면 반영됩니다."));
    return FReply::Handled();
}

FReply SUPTMainPanel::ApplyShotEdit()
{
    if (IsGenerationInFlight() || !bHasValidPlan || !CurrentPlan.Shots.IsValidIndex(CurrentShotIndex)) return FReply::Handled();
    if (!LastCreatedSequence.IsValid())
    {
        SetStatus(TEXT("먼저 시네마틱을 생성하세요."), FLinearColor::Yellow);
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
        SetStatus(FString::Printf(TEXT("%d번 샷 수정을 반영하지 못해 되돌렸습니다: %s"), CurrentShotIndex + 1, *Error), FLinearColor::Red);
        return FReply::Handled();
    }
    if (JsonInput.IsValid()) JsonInput->SetText(FText::FromString(FUPTPlanParser::ToJson(CurrentPlan)));
    PlaySelectedShot();
    SetStatus(FString::Printf(TEXT("%d번 샷 수정을 반영해 '%s'를 그 자리에서 갱신하고 재생합니다."), CurrentShotIndex + 1, *LastCreatedSequence->GetName()), FLinearColor::Green);
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
        SetStatus(TEXT("먼저 시네마틱을 생성하세요."), FLinearColor::Yellow);
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
        SetStatus(TEXT("Sequencer가 아직 열리는 중입니다. 잠시 후 다시 재생하세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }

    ULevelSequenceEditorBlueprintLibrary::Pause();
    ULevelSequenceEditorBlueprintLibrary::SetSelectionRangeStart(StartFrame);
    ULevelSequenceEditorBlueprintLibrary::SetSelectionRangeEnd(EndFrame);
    ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(FMovieSceneSequencePlaybackParams(FFrameTime(StartFrame), EUpdatePositionMethod::Jump));
    ULevelSequenceEditorBlueprintLibrary::PlayTo(FMovieSceneSequencePlaybackParams(FFrameTime(EndFrame), EUpdatePositionMethod::Play));
    SetStatus(FString::Printf(TEXT("%d번 샷 '%s' 재생 중 (%d~%d 프레임)"), CurrentShotIndex + 1, *CurrentPlan.Shots[CurrentShotIndex].Name, StartFrame, EndFrame), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::CaptureCurrentShotCamera()
{
    ULevelSequence* Sequence = LastCreatedSequence.Get();
    if (!Sequence || !bHasValidPlan || !CurrentPlan.Shots.IsValidIndex(CurrentShotIndex))
    {
        SetStatus(TEXT("먼저 시네마틱을 생성하세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }
    if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
    {
        SetStatus(TEXT("생성된 시네마틱을 Sequencer에서 먼저 여세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }

    UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(Sequence->GetMovieScene()->GetCameraCutTrack());
    if (!CutTrack || !CutTrack->GetAllSections().IsValidIndex(CurrentShotIndex))
    {
        SetStatus(TEXT("선택한 샷의 Camera Cut을 찾지 못했습니다."), FLinearColor::Red);
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
        SetStatus(TEXT("카메라가 아직 평가되지 않았습니다. 샷을 한 번 재생한 뒤 다시 누르세요."), FLinearColor::Yellow);
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
    SetStatus(FString::Printf(TEXT("%d번 샷의 카메라 구도를 ②의 Shot Plan JSON에 보존했습니다. 'Shot Plan JSON 직접 입력'으로 다시 생성하면 반영됩니다."), CurrentShotIndex + 1), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::AnalyzeComposition()
{
    if (!bHasValidPlan)
    {
        SetStatus(TEXT("먼저 시네마틱을 생성하세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }
    SetReport(FUPTCompositionAnalyzer::Analyze(CurrentPlan, SceneActors));
    SetStatus(TEXT("샷별 구도 점검을 마쳤습니다. 아래 결과를 확인하세요."), FLinearColor::Green);
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
        SetAnimationStatus(TEXT("Content Browser에서 애니메이션을 적용할 대상 Skeletal Mesh를 먼저 선택하세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FUniversalProductionToolsModule::OpenAnimationRetargetWindow(Mesh);
    SetAnimationStatus(FString::Printf(TEXT("'%s'용 범용화 창을 열었습니다."), *Mesh->GetName()), FLinearColor::Green);
    return FReply::Handled();
}

FReply SUPTMainPanel::AnalyzeSelectedSkeleton()
{
    USkeletalMesh* Mesh = GetSelectedSkeletalMesh();
    if (!Mesh)
    {
        SetAnimationStatus(TEXT("Content Browser에서 보정할 Skeletal Mesh를 선택하세요."), FLinearColor::Yellow);
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
        SetAnimationStatus(TEXT("보정 JSON에 오류가 있습니다. 분석 결과의 오류를 확인하세요."), FLinearColor::Red);
    else
        SetAnimationStatus(FString::Printf(TEXT("'%s' 분석 완료 — 리타기팅 준비도 %.0f%%%s"), *Mesh->GetName(), LastSkeletonAnalysis.Readiness * 100.0f,
            LastSkeletonAnalysis.Readiness >= 0.7f ? TEXT(". 저장하면 범용화 창에서 사용할 수 있습니다.") : TEXT(". 70% 이상이 되도록 틀린 본을 보정하세요.")),
            LastSkeletonAnalysis.Readiness >= 0.7f ? FLinearColor::Green : FLinearColor::Yellow);
    return FReply::Handled();
}

FReply SUPTMainPanel::SaveSkeletonOverride()
{
    USkeletalMesh* Mesh = LastAnalyzedMesh.Get();
    if (!Mesh)
    {
        SetAnimationStatus(TEXT("먼저 '선택 메시 분석'을 누르세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }
    if (SkeletonOverrideInput->GetText().ToString().TrimStartAndEnd() != LastSkeletonOverrideJson)
    {
        SetAnimationStatus(TEXT("보정값이 분석 후 바뀌었습니다. '선택 메시 분석'을 다시 누른 뒤 저장하세요."), FLinearColor::Yellow);
        return FReply::Handled();
    }
    FString Error;
    UUPTSkeletonProfile* Profile = FUPTSkeletonProfileBuilder::CreateProfile(Mesh, LastSkeletonAnalysis, LastSkeletonOverrideJson, Error);
    if (!Profile)
    {
        SetAnimationStatus(Error, FLinearColor::Red);
        return FReply::Handled();
    }
    SetAnimationStatus(FString::Printf(TEXT("보정값을 저장했습니다(%s). 범용화 창이 '%s'에 자동 적용합니다. 에디터를 닫기 전에 에셋을 저장하세요."),
        *Profile->GetPathName(), *Mesh->GetName()), FLinearColor::Green);
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
