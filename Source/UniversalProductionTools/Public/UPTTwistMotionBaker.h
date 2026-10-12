#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class USkeletalMesh;

/**
 * 리타기팅 결과에 트위스트 본의 회전을 옮겨 넣는다.
 *
 * 트위스트 본은 리타기팅에서 **통째로 사라진다.** 원본이 4~23° 구르는데 결과는 0.0~0.1°다.
 * 구조상 그렇다 — `lowerarm_twist_01_l`은 `lowerarm_l`에서 갈라져 `hand_l`로 가는 길에 없으므로
 * **체인 경로 밖의 가지**이고, FK 체인 리타기팅이 건드리지 않아 레퍼런스 포즈에 멈춘다.
 *
 * 처음에는 고치지 않고 기록만 했다. UE 관례에서 트위스트는 AnimBP의 보정 노드가 절차적으로
 * 구동하는 경우가 많아, 구워 넣으면 **두 번 적용**될 수 있기 때문이다. 그래서 먼저 재 봤다 —
 * "이 스켈레톤을 위해 만들어진 애니메이션들이 트위스트를 애니메이션하는가".
 *
 *   SKM_Manny              트위스트 본 26개, 원본 애니 34개 중 14개(41%)가 움직임, 최대 69.4°
 *   Retrieve_DefaultWoman  트위스트 본  8개, 원본 애니 40개 중 26개(65%)가 움직임, 최대 81.1°
 *   X_Bot / Forest Golem / Wolf   트위스트 본 없음
 *
 * 즉 이 파이프라인은 트위스트를 **애니메이션에 넣는** 쪽이고, 잃는 것은 진짜 손실이다.
 * (표본을 처음 6개만 봤을 때는 전부 0.0°로 나와 반대 결론이 났다. 40개로 넓혀야 드러났다.)
 *
 * 옮기는 방식: 레퍼런스 포즈 기준 **로컬 회전 델타**를 그대로 옮긴다.
 *
 *     TargetLocal = TargetRefLocal * (SourceRefLocal^-1 * SourceLocal)
 *
 * 레스트 포즈 정렬기와 같은 형태다(후위 곱 로컬 델타). 양쪽의 레스트 자세가 달라도 델타는
 * 비교 가능하다. 대응은 **트위스트 본이 매달린 역할**로 잡는다 — 이름은 스켈레톤마다 다르지만
 * (`lowerarm_twist_01_l` / `L_Forearm_Roll`) 어느 역할 아래에 있는지는 같다.
 */
class FUPTTwistMotionBaker
{
public:
    /** @return 키를 쓴 트위스트 본 수. 0이면 아무것도 하지 않았다. */
    static int32 Bake(
        UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
        UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh);
};
