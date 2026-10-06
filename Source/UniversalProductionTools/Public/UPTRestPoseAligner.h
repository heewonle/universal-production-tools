#pragma once

#include "CoreMinimal.h"

class UIKRetargeterController;
class USkeletalMesh;

/**
 * 레스트 포즈(A-Pose/T-Pose) 차이를 직접 계산해 리타기팅 포즈에 써 넣는다.
 *
 * 엔진의 UIKRetargeterController::AutoAlignBones는 같은 일을 하지만,
 * 매핑되지 않은 체인에 걸친 본 하나로 `checkf`가 걸려 에디터를 통째로 내린다
 * (IKRetargeterPoseGenerator.cpp:118). 자체 분석으로 만든 IK Rig에서 재현된다.
 *
 * 기준은 IK Rig의 체인이 아니라 **역할 그래프**(해부학적 인접 관계)다.
 * 체인을 따라 정렬하면 같은 캐릭터라도 Rig을 어떻게 끊었느냐에 따라 결과가 달라진다.
 * 실제로 체인을 엔진 관례대로 끊었더니 `Neck`·`Head`가 한 본짜리 체인이 되어 정렬에서
 * 통째로 빠지고 최악값이 10.7° → 31.6°로 튀었다. 뼈마디 방향은 스켈레톤의 성질이지
 * Rig 구성의 성질이 아니다. 분석기가 어떤 명명 규칙에서든 역할을 뽑아 주므로,
 * 역할 그래프를 쓰면 임의의 에셋에 같은 규칙이 그대로 적용된다.
 *
 * 방법: 두 메시의 역할→본 대응을 받아, 인접한 두 역할이 이루는 마디의 방향을
 * 원본과 같게 만드는 회전을 부모 쪽 본에 넣는다. 루트에 가까운 본부터 처리하고,
 * 자식이 여럿인 본(척추·펠비스)은 자식마다 필요한 회전을 평균 낸다.
 */
class FUPTRestPoseAligner
{
public:
    /** 정렬한 본 수를 돌려준다. */
    static int32 AlignTargetToSource(
        UIKRetargeterController* Controller,
        USkeletalMesh* SourceMesh,
        USkeletalMesh* TargetMesh);
};
