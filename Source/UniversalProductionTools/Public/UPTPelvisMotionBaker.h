#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class USkeletalMesh;

/**
 * 리타기팅 결과에 펠비스(골반) 이동을 직접 구워 넣는다.
 *
 * 엔진의 Pelvis Motion op은 일부 조합에서 **아예 실행되지 않는다**. 정적 오프셋을 넣어도
 * 결과가 꿈쩍하지 않고, 다른 op을 전부 꺼도 같으며, 플러그인을 거치지 않은 엔진 기본 API에서도
 * 똑같이 재현된다. 그 결과 대상 펠비스의 이동 키가 세 축 모두 0이 되어, 원본이 몸을 낮추는
 * 구간에서 다리가 그 차이를 떠안고 **발이 바닥에서 뜬다**(다리 길이의 최대 24%).
 *
 * 여기서는 op이 하려던 계산을 그대로 다시 해서 트랙에 써 넣는다(PelvisMotionOp.cpp 기준).
 *
 *     normalized = 원본 펠비스 컴포넌트 위치 / 원본 레퍼런스 펠비스 높이
 *     대상 펠비스 컴포넌트 위치 = normalized * 대상 레퍼런스 펠비스 높이
 *
 * 이미 이동이 들어 있는 결과(엔진 op이 정상 동작한 조합)는 건드리지 않는다.
 */
class FUPTPelvisMotionBaker
{
public:
    /**
     * @return 키를 새로 쓴 경우 true. 이미 이동이 있거나 조건을 못 갖춰 건너뛰면 false.
     */
    static bool BakeIfMissing(
        UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
        UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh);
};
