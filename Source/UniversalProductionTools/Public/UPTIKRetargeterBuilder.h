#pragma once

#include "CoreMinimal.h"

class UIKRigDefinition;
class UIKRetargeter;
class UUPTSkeletonProfile;

class FUPTIKRetargeterBuilder
{
public:
    static bool ValidatePair(const UUPTSkeletonProfile* Profile, const UIKRigDefinition* IKRig, const TCHAR* PairLabel, FString& OutError);
    static UIKRetargeter* CreateIKRetargeter(
        class USkeletalMesh* SourceMesh, UIKRigDefinition* SourceIKRig,
        class USkeletalMesh* TargetMesh, UIKRigDefinition* TargetIKRig,
        FString& OutError);
    static UIKRetargeter* CreateIKRetargeter(
        UUPTSkeletonProfile* SourceProfile, UIKRigDefinition* SourceIKRig,
        UUPTSkeletonProfile* TargetProfile, UIKRigDefinition* TargetIKRig,
        FString& OutError);
};
