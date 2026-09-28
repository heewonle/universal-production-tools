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
    // UE 5.7 AutoAlignAllBones는 비표준 Rig의 부분 매핑에서 내부 Assertion을 발생시킬 수 있다.
    // 체인 리타기팅은 안전하게 생성하고, 자세 보정은 비교 창에서 필요한 체인만 사용자가 적용한다.
    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();
    return Retargeter;
}
