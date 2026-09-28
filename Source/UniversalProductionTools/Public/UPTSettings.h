#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UPTSettings.generated.h"

class UMovieGraphConfig;

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Universal Production Tools"))
class UNIVERSALPRODUCTIONTOOLS_API UUPTSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UUPTSettings();
    UPROPERTY(Config, EditAnywhere, Category="LLM", meta=(DisplayName="API Endpoint")) FString ApiEndpoint;
    UPROPERTY(Config, EditAnywhere, Category="LLM") FString Model;
    UPROPERTY(Config, EditAnywhere, Category="LLM", meta=(DisplayName="API Key Environment Variable")) FString ApiKeyEnvironmentVariable;
    UPROPERTY(Config, EditAnywhere, Category="Cinematic") FString DefaultSequencePath;
    UPROPERTY(Config, EditAnywhere, Category="Cinematic", meta=(ClampMin="1", ClampMax="120")) int32 DefaultFrameRate = 30;
    UPROPERTY(Config, EditAnywhere, Category="Skeleton") FString DefaultSkeletonProfilePath;
    UPROPERTY(Config, EditAnywhere, Category="Skeleton") FString DefaultIKRigPath;
    UPROPERTY(Config, EditAnywhere, Category="Skeleton") FString DefaultRetargeterPath;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(DisplayName="FFmpeg Executable")) FString FFmpegExecutablePath;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="0.1", ClampMax="10.0")) float ReferenceFrameIntervalSeconds = 1.0f;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="1", ClampMax="300")) int32 MaxReferenceFrames = 120;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="0.01", ClampMax="1.0")) float ShotChangeThreshold = 0.22f;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="0.1", ClampMax="10.0")) float MinimumShotSeconds = 0.5f;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video") FString VisionApiEndpoint;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video") FString VisionModel;
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="1", ClampMax="20")) int32 MaxVisionFrames = 20;
    // 생성을 멈추는 기준이 아니라 샷별 경고 기준이다. 이보다 낮은 샷은 샷 목록·결과 칸에 표시해 '선택한 샷 수정'으로 손보게 한다.
    // (실사 저해상도 영상은 관절 점수 한계로 신뢰도가 0.6 안팎에 머물러, 합성 영상 기준으로 생성을 막으면 대부분 멈췄다.)
    UPROPERTY(Config, EditAnywhere, Category="Reference Video", meta=(ClampMin="0.0", ClampMax="1.0", DisplayName="Low Confidence Warning Threshold", ToolTip="이보다 분석 신뢰도가 낮은 샷은 생성은 하되 샷 목록에 경고로 표시합니다.")) float MinimumAutoGenerationConfidence = 0.4f;
    UPROPERTY(Config, EditAnywhere, Category="Pose Analyzer", meta=(DisplayName="Analyzer Python Executable")) FString PoseAnalyzerPythonPath = TEXT("ThirdParty/UPTAnalyzer/.venv/Scripts/python.exe");
    UPROPERTY(Config, EditAnywhere, Category="Pose Analyzer", meta=(DisplayName="Analyzer Script")) FString PoseAnalyzerScript = TEXT("Plugins/UniversalProductionTools/Tools/upt_pose_reference_analyzer.py");
    UPROPERTY(Config, EditAnywhere, Category="Pose Analyzer", meta=(DisplayName="Model (lightweight / balanced / performance)")) FString PoseAnalyzerModel = TEXT("balanced");
    UPROPERTY(Config, EditAnywhere, Category="Pose Analyzer", meta=(ClampMin="1", ClampMax="12")) int32 PoseSamplesPerShot = 4;
    UPROPERTY(Config, EditAnywhere, Category="Reference Library", meta=(DisplayName="Reference Video Library Folder")) FString ReferenceLibraryDirectory;
    UPROPERTY(Config, EditAnywhere, Category="Reference Library", meta=(DisplayName="Library Search Script")) FString ReferenceLibraryScript = TEXT("Plugins/UniversalProductionTools/Tools/upt_reference_library.py");
    UPROPERTY(Config, EditAnywhere, Category="Reference Library", meta=(ClampMin="1", ClampMax="20")) int32 ReferenceLibraryResultCount = 5;
};
