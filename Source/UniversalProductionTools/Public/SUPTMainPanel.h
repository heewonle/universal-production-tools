#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"
#include "UPTReferenceVideoProcessor.h"
#include "UPTReferenceTypes.h"
#include "UPTSkeletonAnalyzer.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FUPTLLMClient;
class SMultiLineEditableTextBox;
class SEditableTextBox;
class STextBlock;
class AActor;
class UAnimSequenceBase;
class USoundBase;
class USkeletalMesh;
class ULevelSequence;

struct FUPTShotListItem
{
    int32 Index = 0;
    FString Name;
    FString Duration;
    FString Lens;
    FString Subject;
    FString Screen;
    FString Motion;
    FString Confidence;
    bool bLowConfidence = false;
};

// 레퍼런스 라이브러리 검색 결과 한 줄(영상 구간)
struct FUPTLibraryResultItem
{
    int32 Rank = 0;
    FString Video;
    double Score = 0.0;
    FString Segment;
    FString Matches;
    FString ReferencePlanFile;
};

enum class EUPTCinematicSource : uint8 { ReferenceVideo, PromptSearch, Script, ShotPlanJson };
enum class EUPTReferenceAnalysisMethod : uint8 { LocalPose, VisionModel };
struct FUPTPoseAnalysisResult;

class SUPTMainPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUPTMainPanel) {}
    SLATE_END_ARGS()

    virtual ~SUPTMainPanel() override;
    void Construct(const FArguments& InArgs);

private:
    TSharedRef<SWidget> BuildCinematicTab();
    TSharedRef<SWidget> BuildAnimationTab();
    TSharedRef<ITableRow> GenerateShotRow(TSharedPtr<FUPTShotListItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void OnShotSelectionChanged(TSharedPtr<FUPTShotListItem> Item, ESelectInfo::Type SelectInfo);
    void OnShotDoubleClicked(TSharedPtr<FUPTShotListItem> Item);
    void RefreshShotList();
    void RefreshEnvironmentStatus();

    // ① 등장인물
    void OnEditorSelectionChanged(UObject* NewSelection);
    void RefreshCastFromSelection();
    FReply UnlockCast();
    FText GetCastText() const;

    // ② 소스
    FReply SelectReferenceVideo();
    FReply AddSelectedMedia();
    FReply ClearMedia();
    FText GetMediaText() const;

    // ② 프롬프트로 레퍼런스 찾기
    FReply BrowseLibraryFolder();
    FReply SearchLibraryOnly();
    void StartLibrarySearch(bool bContinueToGeneration);
    void OnLibrarySearchCompleted(bool bSuccess, const FString& ResultJson, const FString& Error, bool bContinueToGeneration);
    bool LoadLibraryResult(const TSharedPtr<FUPTLibraryResultItem>& Item, FString& OutError);
    TSharedRef<ITableRow> GenerateLibraryRow(TSharedPtr<FUPTLibraryResultItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
    FString GetLibraryQueryKey() const;

    // ④ 선택한 샷 수정: 만든 뒤 샷 하나의 구도·모션·길이·인물·애니메이션을 고치고 같은 시퀀스를 그 자리에서 갱신한다.
    void InitShotEditorOptions();
    void RebuildSubjectOptions();
    void LoadShotEditor();
    void PreviewShotSize(const FString& ShotSize);
    float GetSubjectHeightCm(const FString& ActorLabel) const;
    FReply ApplyShotEdit();
    FReply AssignSelectedAnimationToShot();
    FReply ClearShotAnimation();

    // ③ 생성
    FReply GenerateCinematic();
    bool IsGenerationInFlight() const { return bGenerationInFlight; }
    void OnReferenceFramesExtracted(bool bSuccess, const FUPTReferenceFrameResult& Result, const FString& Error);
    void OnReferenceAnalysisCompleted(bool bSuccess, const FString& Result);
    void OnPoseAnalysisCompleted(bool bSuccess, const FUPTPoseAnalysisResult& Result, const FString& Error);
    FReply OpenPoseDebugFolder();
    bool TryLoadCachedReferenceAnalysis();
    void ContinueReferencePipeline();
    bool ApplyReferenceBlocking(FString& OutError);
    void OnScriptPlanReceived(bool bSuccess, const FString& Result);
    bool ValidatePlanJson(const FString& JsonText, FString& OutError);
    bool CreateSequence(FString& OutError, bool bReuseExistingSequence = false);
    void FinishGeneration(const FString& Message, const FSlateColor& Color);
    FString BuildCompletionMessage() const;

    // ④ 결과 검토
    FReply OpenLastSequence();
    FReply PlaySelectedShot();
    FReply CaptureCurrentShotCamera();
    FReply AnalyzeComposition();
    FReply ApplyJsonAndRegenerate();

    // 애니메이션 범용화 탭
    FReply OpenRetargetWindowForSelection();
    FReply AnalyzeSelectedSkeleton();
    FReply SaveSkeletonOverride();

    void SetStatus(const FString& Message, const FSlateColor& Color = FSlateColor::UseForeground());
    void SetAnimationStatus(const FString& Message, const FSlateColor& Color = FSlateColor::UseForeground());
    void SetReport(const FString& Text);
    FString BuildSceneContext() const;
    FString BuildAssetContext() const;

    TSharedPtr<FUPTLLMClient> LLMClient;
    TSharedPtr<SMultiLineEditableTextBox> ScriptInput;
    TSharedPtr<SMultiLineEditableTextBox> JsonInput;
    TSharedPtr<SMultiLineEditableTextBox> ReportOutput;
    TSharedPtr<SMultiLineEditableTextBox> ReferenceRoleMappingInput;
    TSharedPtr<SMultiLineEditableTextBox> SkeletonOverrideInput;
    TSharedPtr<SMultiLineEditableTextBox> SkeletonReportOutput;
    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> AnimationStatusText;
    TSharedPtr<SListView<TSharedPtr<FUPTShotListItem>>> ShotListView;
    TArray<TSharedPtr<FUPTShotListItem>> ShotItems;

    int32 ActiveTab = 0;
    EUPTCinematicSource Source = EUPTCinematicSource::ReferenceVideo;
    bool bApiKeyReady = false;
    FString ResolvedFFmpegPath;
    bool bCastLocked = false;
    bool bGenerationInFlight = false;

    FUPTCinematicPlan CurrentPlan;
    bool bHasValidPlan = false;
    int32 CurrentShotIndex = 0;
    TMap<FString, TWeakObjectPtr<AActor>> SceneActors;
    TMap<FString, TWeakObjectPtr<UAnimSequenceBase>> Animations;
    TMap<FString, TWeakObjectPtr<USoundBase>> Sounds;
    TWeakObjectPtr<ULevelSequence> LastCreatedSequence;

    FString ReferenceVideoPath;
    FUPTReferenceFrameResult ReferenceFrameResult;
    FUPTReferencePlan CurrentReferencePlan;
    bool bHasReferencePlan = false;
    FString ReferenceMatchMode = TEXT("scene");
    EUPTReferenceAnalysisMethod ReferenceAnalysisMethod = EUPTReferenceAnalysisMethod::LocalPose;
    FString LastPoseDebugDirectory;
    // 로컬 포즈 분석기가 만든 Reference Plan 파일(영상 분석 결과 또는 라이브러리 검색 구간). 비어 있으면 구도 검증을 건너뛴다.
    FString LastReferencePlanFile;

    // ② 레퍼런스 영상: 파일 대신 영상 링크(유튜브·Pexels 등)를 쓸 때
    TSharedPtr<SEditableTextBox> ReferenceLinkInput;
    TSharedPtr<SEditableTextBox> ReferenceSectionInput;
    bool bLinkRightsConfirmed = false;

    TSharedPtr<SEditableTextBox> LibraryFolderInput;
    TSharedPtr<SMultiLineEditableTextBox> LibraryPromptInput;
    TSharedPtr<SListView<TSharedPtr<FUPTLibraryResultItem>>> LibraryResultView;
    TArray<TSharedPtr<FUPTLibraryResultItem>> LibraryResults;
    FString LibraryResultsKey;
    FString PendingLibraryKey;
    FString LibraryReport;

    // 레퍼런스 분석 신뢰도(샷 순서대로, 인물 없는 샷은 -1). 레퍼런스로 만든 계획일 때만 채운다.
    TArray<float> ShotConfidences;
    FString PendingGenerationWarning;

    TArray<TSharedPtr<FString>> ShotSizeOptions;
    TArray<TSharedPtr<FString>> CameraAngleOptions;
    TArray<TSharedPtr<FString>> CameraMotionOptions;
    TArray<TSharedPtr<FString>> SubjectOptions;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> SubjectCombo;
    FString EditShotSize;
    FString EditCameraAngle;
    FString EditCameraMotion;
    FString EditSubject;
    FString EditAnimationId;
    float EditScreenX = 0.5f;
    float EditScreenY = 0.5f;
    float EditAzimuth = 0.0f;
    float EditDistance = 300.0f;
    float EditFocalLength = 50.0f;
    float EditFocusHeight = 0.5f;
    float EditDuration = 3.0f;
    bool bEditFramingChanged = false;
    // 어깨 너머(OTS): 카메라를 둘 앞사람 Actor(빈 문자열이면 일반 샷)와 앞사람이 걸칠 화면 쪽.
    TArray<TSharedPtr<FString>> OverShoulderOptions;
    TArray<TSharedPtr<FString>> OverShoulderSideOptions;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> OverShoulderCombo;
    FString EditOverShoulderActor;
    FString EditOverShoulderSide = TEXT("right");

    TWeakObjectPtr<USkeletalMesh> LastAnalyzedMesh;
    FUPTSkeletonAnalysisData LastSkeletonAnalysis;
    FString LastSkeletonOverrideJson;
};
