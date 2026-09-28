#pragma once

#include "CoreMinimal.h"

class UIKRigDefinition;
class UUPTSkeletonProfile;

class FUPTIKRigBuilder
{
public:
    // UE 5.7 Auto Characterizer를 우선 사용해 알려진 휴머노이드 구조를 이름 규칙과 무관하게 표준 체인으로 변환합니다.
    static UIKRigDefinition* CreateUniversalIKRig(class USkeletalMesh* Mesh, FString& OutError);
    static UIKRigDefinition* CreateIKRig(UUPTSkeletonProfile* Profile, FString& OutError);
};
