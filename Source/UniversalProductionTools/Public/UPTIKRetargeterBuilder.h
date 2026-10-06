#pragma once

#include "CoreMinimal.h"

class UIKRigDefinition;
class UIKRetargeter;
class UUPTSkeletonProfile;

class FUPTIKRetargeterBuilder
{
public:
    static bool ValidatePair(const UUPTSkeletonProfile* Profile, const UIKRigDefinition* IKRig, const TCHAR* PairLabel, FString& OutError);
    // 레스트 포즈 보정은 FUPTRestPoseAligner로 직접 계산한다. 엔진 AutoAlignBones는 쓰지 않는다
    // (비표준 Rig에서 어설션으로 에디터가 내려가고, 수치도 직접 계산 쪽이 더 낫다).
    static UIKRetargeter* CreateIKRetargeter(
        class USkeletalMesh* SourceMesh, UIKRigDefinition* SourceIKRig,
        class USkeletalMesh* TargetMesh, UIKRigDefinition* TargetIKRig,
        FString& OutError);
    static UIKRetargeter* CreateIKRetargeter(
        UUPTSkeletonProfile* SourceProfile, UIKRigDefinition* SourceIKRig,
        UUPTSkeletonProfile* TargetProfile, UIKRigDefinition* TargetIKRig,
        FString& OutError);
};
