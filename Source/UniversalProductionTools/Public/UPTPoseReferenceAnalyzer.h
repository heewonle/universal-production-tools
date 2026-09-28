#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

class AActor;

DECLARE_DELEGATE_TwoParams(FUPTPoseVerifyComplete, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FUPTPoseRefineProgress, int32 /*Pass*/, double /*Score*/);
DECLARE_DELEGATE_ThreeParams(FUPTPoseRefineComplete, bool, const FUPTCinematicPlan&, const FString&);
// 성공 여부, 검색 결과 JSON(성공 시), 오류 메시지(실패 시)
DECLARE_DELEGATE_ThreeParams(FUPTLibrarySearchComplete, bool, const FString&, const FString&);

struct FUPTPoseAnalysisResult
{
    FString PlanJson;
    FString PlanFile;
    FString DebugDirectory;
    bool bFromCache = false;
};

DECLARE_DELEGATE_ThreeParams(FUPTPoseAnalysisComplete, bool, const FUPTPoseAnalysisResult&, const FString&);

// 레퍼런스 영상을 API 없이 로컬 포즈 분석기(Tools/upt_pose_reference_analyzer.py)로 분석해 Reference Plan JSON을 만든다.
class FUPTPoseReferenceAnalyzer
{
public:
    static bool Preflight(FString& OutError);
    static void Analyze(const FString& VideoPath, const FString& MatchMode, FUPTPoseAnalysisComplete Completion);

    // 영상 링크(Tools/upt_reference_link.py)를 받아 같은 포즈 분석을 한다. Section은 "1:20-2:05" 같은 선택 구간이다.
    // 다운로드를 허용하지 않는 사이트(YouTube 등)는 bRightsConfirmed일 때만 받고, 분석 뒤 영상은 남기지 않는다.
    static void AnalyzeLink(const FString& Url, const FString& Section, bool bRightsConfirmed, const FString& MatchMode, FUPTPoseAnalysisComplete Completion);

    // 레퍼런스 영상 라이브러리(Tools/upt_reference_library.py)를 색인(새로 넣었거나 바뀐 영상만 분석)한 뒤,
    // 프롬프트와 샷 구성이 가장 비슷한 영상 구간을 찾는다. 결과의 각 구간은 바로 쓸 수 있는 Reference Plan 파일로 저장된다.
    static void SearchLibrary(const FString& LibraryDirectory, const FString& Prompt, int32 ResultCount, FUPTLibrarySearchComplete Completion);
    static bool IsRunning();

    // 생성된 시네마틱의 샷별 카메라 시점을 캡처하고 같은 포즈 분석기로 재분석해 레퍼런스와의 구도 오차를 계산한다.
    // 성공 시 Completion의 문자열은 verify_report.json 내용, 실패 시 오류 메시지다.
    static void VerifyGeneratedCinematic(const FUPTCinematicPlan& Plan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
        const FString& ReferencePlanFile, FUPTPoseVerifyComplete Completion);
    static FString FormatVerifyReport(const FString& ReportJson, double& OutScore);

    // 검증(캡처→재측정) 결과로 샷별 카메라 거리·화면 목표 위치를 보정하고 다시 검증하는 과정을 최대 MaxCorrections회 반복한다.
    // 샷마다 가장 점수가 높았던 카메라만 채택하므로 결과가 처음보다 나빠지지 않는다. Level Sequence 없이 계획만으로 동작한다.
    // Completion: 성공 여부, 샷별 최고 구도를 모은 계획, 성공 시 최고 결과를 모은 검증 리포트 JSON(실패 시 오류 메시지).
    static void RefineShotCameras(const FUPTCinematicPlan& Plan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
        const FString& ReferencePlanFile, int32 MaxCorrections, FUPTPoseRefineProgress Progress, FUPTPoseRefineComplete Completion);
};
