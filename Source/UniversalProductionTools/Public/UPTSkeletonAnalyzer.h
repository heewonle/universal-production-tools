#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

struct FUPTSkeletonAnalysisData
{
    FString Report;
    TMap<FString, FName> BoneMappings;
    TMap<FString, float> Confidences;
    // 이름 근거 없이 위치·계층만으로 배정된 역할. 부분 메시(망토·머리 부착물)에서는
    // 남은 아무 본이나 집어 오므로(LeftFoot ← jaw) 결과를 그대로 믿으면 안 된다.
    TSet<FString> ForcedRoles;
    TArray<FName> TwistBones;
    TArray<FName> FingerBones;
    TArray<FName> FaceBones;
    float Readiness = 0.0f;
    bool bOverridesValid = true;

    /**
     * 이 스켈레톤이 **직립 이족**인가. 휴머노이드 역할 그래프가 성립하는지를 기하로 본다.
     *
     * 이름만으로는 막을 수 없다. 늑대(사족보행)는 리거가 앞다리를 `L_UpperArm`·`L_Forearm`·
     * `L_Hand`로 이름 붙여 두어 **모든 역할이 신뢰도 1.00으로 매핑되고 A등급 0.9375로 통과한다.**
     * 준비도 70% 가드가 발동하지 않으니, 사람 애니메이션을 늑대에 옮겨도 조용히 진행된다.
     * 역할 이름은 리거의 선언일 뿐이고, 몸이 실제로 어떻게 서 있는지는 레퍼런스 포즈가 말해 준다.
     */
    bool bUprightBiped = true;
    // 손이 발보다 얼마나 위에 있는가(몸 높이 대비). 이족은 0.4 이상, 사족은 0에 가깝다.
    float HandAboveFootRatio = 0.0f;
    // 펠비스→목 방향의 수직 성분. 이족은 1에 가깝고, 사족은 몸통이 누워 있어 작다.
    float SpineUpright = 0.0f;
};

class FUPTSkeletonAnalyzer
{
public:
    static FString Analyze(USkeletalMesh* SkeletalMesh);
    static FString Analyze(USkeletalMesh* SkeletalMesh, const FString& OverrideJson);
    static FUPTSkeletonAnalysisData AnalyzeDetailed(USkeletalMesh* SkeletalMesh, const FString& OverrideJson);
};
