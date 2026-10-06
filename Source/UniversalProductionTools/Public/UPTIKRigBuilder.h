#pragma once

#include "CoreMinimal.h"

class UIKRigDefinition;
class UUPTSkeletonProfile;

class FUPTIKRigBuilder
{
public:
    /**
     * 리타기팅에서 '몸'으로 다룰 본. 분석기가 Pelvis로 뽑은 본이 항상 높이를 들고 있지는 않다.
     *
     * Forest Golem은 계층이 `Root → CG → Pelvis`이고 **`Pelvis`의 로컬 이동이 (0,0,0)** 이다.
     * 높이는 `CG`가 든다. 그런 본을 리타기팅 루트로 잡으면 Pelvis Motion op이 이동을 써도
     * 몸이 움직이지 않는다. 그래서 레퍼런스 포즈의 로컬 이동이 거의 0인 동안 부모로 거슬러 올라간다.
     * UE 마네킹(`root → pelvis`, 로컬 z=95.9)이나 Mixamo(`Hips`가 최상위)는 그대로 유지된다.
     */
    static FName ResolveMotionBone(const struct FReferenceSkeleton& Ref, FName PelvisBone);

    // UE 5.7 Auto Characterizer를 우선 사용해 알려진 휴머노이드 구조를 이름 규칙과 무관하게 표준 체인으로 변환합니다.
    static UIKRigDefinition* CreateUniversalIKRig(class USkeletalMesh* Mesh, FString& OutError);
    static UIKRigDefinition* CreateIKRig(UUPTSkeletonProfile* Profile, FString& OutError);
};
