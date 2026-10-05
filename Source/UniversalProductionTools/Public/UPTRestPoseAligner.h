#pragma once

#include "CoreMinimal.h"

class UIKRetargeterController;
class UIKRigDefinition;
class USkeletalMesh;

/**
 * 레스트 포즈(A-Pose/T-Pose) 차이를 직접 계산해 리타기팅 포즈에 써 넣는다.
 *
 * 엔진의 UIKRetargeterController::AutoAlignBones는 같은 일을 하지만,
 * 매핑되지 않은 체인에 걸친 본 하나로 `checkf`가 걸려 에디터를 통째로 내린다
 * (IKRetargeterPoseGenerator.cpp:118). 자체 분석으로 만든 IK Rig에서 재현된다.
 * 여기서는 엔진 API를 거치지 않고 체인 방향에서 회전 오프셋을 직접 구한다.
 *
 * 방법: 매핑된 체인마다 원본/대상의 레퍼런스 포즈를 컴포넌트 공간으로 펴고,
 * 누적 길이 비율이 같은 자리의 마디 방향을 맞추는 회전을 본마다 누적한다.
 * 본 개수가 달라도(트위스트 본 유무 등) 길이 비율로 대응시키므로 그대로 맞는다.
 */
class FUPTRestPoseAligner
{
public:
    /** 정렬한 본 수를 돌려준다. */
    static int32 AlignTargetToSource(
        UIKRetargeterController* Controller,
        USkeletalMesh* SourceMesh, const UIKRigDefinition* SourceIKRig,
        USkeletalMesh* TargetMesh, const UIKRigDefinition* TargetIKRig,
        const TSet<FName>& MappedChainNames);
};
