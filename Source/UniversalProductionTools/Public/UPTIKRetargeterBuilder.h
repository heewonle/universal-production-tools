#pragma once

#include "CoreMinimal.h"

class UIKRigDefinition;
class UIKRetargeter;
class UUPTSkeletonProfile;

class FUPTIKRetargeterBuilder
{
public:
    static bool ValidatePair(const UUPTSkeletonProfile* Profile, const UIKRigDefinition* IKRig, const TCHAR* PairLabel, FString& OutError);
    // bAlignRestPose: 레스트 포즈(A/T-Pose) 차이를 엔진 AutoAlignBones로 보정할지.
    // 엔진 Auto Characterizer가 만든 Rig에서만 안전하다. 자체 분석으로 만든 Rig에 쓰면
    // IKRetargeterPoseGenerator의 어설션으로 에디터가 내려간다(Forest Golem에서 확인).
    static UIKRetargeter* CreateIKRetargeter(
        class USkeletalMesh* SourceMesh, UIKRigDefinition* SourceIKRig,
        class USkeletalMesh* TargetMesh, UIKRigDefinition* TargetIKRig,
        FString& OutError, bool bAlignRestPose = true);
    static UIKRetargeter* CreateIKRetargeter(
        UUPTSkeletonProfile* SourceProfile, UIKRigDefinition* SourceIKRig,
        UUPTSkeletonProfile* TargetProfile, UIKRigDefinition* TargetIKRig,
        FString& OutError);
};
