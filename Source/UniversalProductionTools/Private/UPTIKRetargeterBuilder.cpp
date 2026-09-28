#include "UPTIKRetargeterBuilder.h"

#include "UPTSettings.h"
#include "UPTSkeletonProfile.h"

#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "ObjectTools.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"

namespace
{
const TArray<FName> RequiredChains = {
    TEXT("Root"), TEXT("Spine"), TEXT("LeftArm"), TEXT("RightArm"), TEXT("LeftLeg"), TEXT("RightLeg")
};
}

bool FUPTIKRetargeterBuilder::ValidatePair(const UUPTSkeletonProfile* Profile, const UIKRigDefinition* IKRig, const TCHAR* PairLabel, FString& OutError)
{
    if (!Profile || !IKRig)
    {
        OutError = FString::Printf(TEXT("%s Profile과 IK Rig을 함께 지정하세요."), PairLabel);
        return false;
    }
    USkeletalMesh* ProfileMesh = Profile->SourceMesh.LoadSynchronous();
    const UIKRigController* RigController = UIKRigController::GetController(IKRig);
    if (!ProfileMesh || !RigController)
    {
        OutError = FString::Printf(TEXT("%s Profile 또는 IK Rig을 불러오지 못했습니다."), PairLabel);
        return false;
    }
    if (RigController->GetSkeletalMesh() != ProfileMesh)
    {
        OutError = FString::Printf(TEXT("%s IK Rig의 Preview Mesh가 Profile Source Mesh와 다릅니다."), PairLabel);
        return false;
    }
    for (const FName ChainName : RequiredChains)
    {
        if (!RigController->GetRetargetChainByName(ChainName))
        {
            OutError = FString::Printf(TEXT("%s IK Rig에 필수 체인 %s가 없습니다."), PairLabel, *ChainName.ToString());
            return false;
        }
    }
    return true;
}

UIKRetargeter* FUPTIKRetargeterBuilder::CreateIKRetargeter(
    UUPTSkeletonProfile* SourceProfile, UIKRigDefinition* SourceIKRig,
    UUPTSkeletonProfile* TargetProfile, UIKRigDefinition* TargetIKRig,
    FString& OutError)
{
    if (!ValidatePair(SourceProfile, SourceIKRig, TEXT("Source"), OutError) ||
        !ValidatePair(TargetProfile, TargetIKRig, TEXT("Target"), OutError)) return nullptr;
    if (SourceProfile == TargetProfile || SourceIKRig == TargetIKRig)
    {
        OutError = TEXT("Source와 Target은 서로 다른 Profile과 IK Rig이어야 합니다.");
        return nullptr;
    }
    USkeletalMesh* SourceMesh = SourceProfile->SourceMesh.LoadSynchronous();
    USkeletalMesh* TargetMesh = TargetProfile->SourceMesh.LoadSynchronous();
    return CreateIKRetargeter(SourceMesh, SourceIKRig, TargetMesh, TargetIKRig, OutError);
}

UIKRetargeter* FUPTIKRetargeterBuilder::CreateIKRetargeter(
    USkeletalMesh* SourceMesh, UIKRigDefinition* SourceIKRig,
    USkeletalMesh* TargetMesh, UIKRigDefinition* TargetIKRig,
    FString& OutError)
{
    if (!SourceMesh || !TargetMesh || !SourceIKRig || !TargetIKRig)
    { OutError = TEXT("Source/Target Mesh와 IK Rig을 모두 지정하세요."); return nullptr; }
    if (SourceMesh == TargetMesh)
    {
        OutError = TEXT("Source와 Target Profile이 같은 Skeletal Mesh를 참조합니다.");
        return nullptr;
    }

    FString SourceName = ObjectTools::SanitizeObjectName(SourceMesh->GetName());
    FString TargetName = ObjectTools::SanitizeObjectName(TargetMesh->GetName());
    if (SourceName.IsEmpty()) SourceName = TEXT("Source");
    if (TargetName.IsEmpty()) TargetName = TEXT("Target");

    FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    FString PackageName;
    FString AssetName;
    const FString BasePackageName = GetDefault<UUPTSettings>()->DefaultRetargeterPath /
        FString::Printf(TEXT("RTG_UPT_%s_to_%s"), *SourceName, *TargetName);
    AssetTools.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), PackageName, AssetName);
    FString PackagePath;
    PackageName.Split(TEXT("/"), &PackagePath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

    UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UIKRetargeter::StaticClass(), Factory));
    if (!Retargeter)
    {
        OutError = TEXT("IK Retargeter 에셋을 생성하지 못했습니다.");
        return nullptr;
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        OutError = TEXT("IK Retargeter Controller를 가져오지 못했습니다.");
        return nullptr;
    }

    Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceIKRig);
    Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetIKRig);
    Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
    Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
    Controller->AddDefaultOps();
    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceIKRig);
    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetIKRig);
    Controller->AutoMapChains(EAutoMapChainType::Exact, true);

    // 레스트 포즈(A-Pose/T-Pose) 차이를 보정하지 않으면 동작은 그대로 옮겨져도 자세가 통째로 틀어진다.
    // 측정해 보니 Synty→Manny에서 팔이 항상 52.9° 어긋났다(다리는 2.2°). 원인은 이 보정을 건너뛴 것이었다.
    // AutoAlignAllBones는 비표준 Rig의 부분 매핑에서 내부 Assertion을 낼 수 있으므로 쓰지 않는다.
    // 대신 실제로 매핑된 체인의 본만 골라 AutoAlignBones를 부른다(팔 52.9° → 0.1°로 줄었고 어설션도 없었다).
    if (const UIKRigController* TargetRigController = UIKRigController::GetController(TargetIKRig))
    {
        // 체인의 시작·끝만 정렬하면 중간 본(lowerarm 등)이 남아 팔이 31.9° 어긋난 채로 남는다.
        // 끝 본에서 시작 본까지 계층을 거슬러 올라가며 체인 전체를 모은다.
        const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();
        TArray<FName> BonesToAlign;
        for (const FBoneChain& Chain : TargetRigController->GetRetargetChains())
        {
            const FName StartName = Chain.StartBone.BoneName;
            const FName EndName = Chain.EndBone.BoneName;
            if (StartName == NAME_None && EndName == NAME_None) continue;
            if (StartName != NAME_None) BonesToAlign.AddUnique(StartName);
            int32 Index = EndName != NAME_None ? TargetRef.FindBoneIndex(EndName) : INDEX_NONE;
            for (int32 Guard = 0; Index != INDEX_NONE && Guard < 64; ++Guard)
            {
                const FName BoneName = TargetRef.GetBoneName(Index);
                BonesToAlign.AddUnique(BoneName);
                if (BoneName == StartName) break;
                Index = TargetRef.GetParentIndex(Index);
            }
        }
        if (BonesToAlign.Num() > 0)
        {
            Controller->AutoAlignBones(BonesToAlign, ERetargetAutoAlignMethod::ChainToChain, ERetargetSourceOrTarget::Target);
        }
    }
    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();
    return Retargeter;
}
