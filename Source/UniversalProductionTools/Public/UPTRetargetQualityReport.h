#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class USkeletalMesh;

/**
 * 리타기팅 결과가 원본 동작을 보존했는지 **제품 안에서** 잰다.
 *
 * 지금까지 이 측정은 개발용 파이썬 스위트에만 있었고 에셋 목록이 코드에 박혀 있었다.
 * 즉 **우리는 품질을 알지만 사용자는 모른다** — 50개를 일괄 변환하면 50개를 전부 눈으로
 * 봐야 한다. 측정 방법은 이미 다 있으므로, 같은 정의를 결과 애니메이션에 그대로 적용해
 * "어느 것을 봐야 하는지"를 돌려준다.
 *
 * 재는 것(정의는 Tools/eval/eval_retarget.py와 같다. 수치 일치는 UPT.QualityReport로 검증한다)
 *  - 뼈마디 방향 오차: 역할과 역할을 잇는 방향이 얼마나 같은가. 체형과 무관하다.
 *  - 움직임 변동분(MAD): 레스트 포즈 차이에서 오는 고정 오프셋을 빼고 **동작이 뭉개진 정도**만.
 *  - 발 높이 오차·접지 불일치: 각도와 **독립**인 디딤 지표. 발이 같은 시각에 같은 높이인가.
 *  - 손가락 마디 방향, 트위스트 전달 비: 손과 아래팔 스키닝이 살아 있는가.
 *
 * 판정은 숫자 하나로 하지 않는다. 각 지표에 따로 문턱을 두고, 넘은 지표의 이름을 사유로 남긴다.
 * 사용자가 보는 것은 "검토 필요 3/50"과 그 사유다.
 */
struct FUPTRetargetQualityRow
{
    FString Animation;
    FString SourceMesh;
    FString TargetMesh;

    /** 비교된 뼈마디 수. 0이면 역할이 부족해 재지 못한 것이다(판정하지 않는다). */
    int32 SegmentsCompared = 0;
    float SegmentMedianDeg = 0.0f;
    float WorstSegmentDeg = 0.0f;
    FString WorstSegment;
    float MotionDeviationWorstDeg = 0.0f;

    /** 다리 길이로 정규화한 값. 역할이 없으면 음수로 둔다(재지 못함). */
    float FootClearanceError = -1.0f;
    float ContactMismatch = -1.0f;

    int32 FingerSegmentsCompared = 0;
    float FingerMedianDeg = 0.0f;
    float FingerWorstDeg = 0.0f;

    int32 TwistCompared = 0;
    int32 TwistDropped = 0;
    float TwistRatioWorstError = 0.0f;

    bool bMeasured = false;
    bool bNeedsReview = false;
    /** 검토가 필요한 이유. 어떤 지표가 어떤 값으로 문턱을 넘었는지 그대로 적는다. */
    TArray<FString> Reasons;
};

struct FUPTRetargetQualityThresholds
{
    float WorstSegmentDeg = 15.0f;
    float MotionDeviationDeg = 6.0f;
    float FootClearanceError = 0.06f;
    float ContactMismatch = 0.25f;
    float FingerWorstDeg = 25.0f;
    float TwistRatioError = 0.5f;
};

class FUPTRetargetQualityReport
{
public:
    /** 한 쌍을 잰다. 역할 매핑이 부족하면 bMeasured=false로 돌려준다(틀린 숫자를 보여주지 않는다). */
    static FUPTRetargetQualityRow Measure(
        UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
        UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh,
        const FUPTRetargetQualityThresholds& Thresholds = FUPTRetargetQualityThresholds());

    /** 표를 JSON으로 저장한다. @return 저장한 경로(실패하면 빈 문자열). */
    static FString Save(const TArray<FUPTRetargetQualityRow>& Rows, const FString& Directory);

    /** 패널에 보여 줄 한 줄 요약. */
    static FString Summarize(const TArray<FUPTRetargetQualityRow>& Rows);
};
