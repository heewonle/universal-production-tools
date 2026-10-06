#include "UPTIKRetargeterBuilder.h"

#include "UPTSettings.h"
#include "UPTRestPoseAligner.h"
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
    // Root 체인은 필수가 아니다. 엔진 Auto Characterizer도 Mixamo처럼 별도 루트 본이 없는 리그에서는
    // Root 체인을 만들지 않는다(X_Bot 확인). 필수로 걸어 두면 그런 리그가 엔진 Rig을 못 쓰고
    // 폴백으로 내려가고, 폴백은 `Hips → Hips` 체인을 만들어 리타기팅 루트를 FK 체인에 가둔다.
const TArray<FName> RequiredChains = {
    TEXT("Spine"), TEXT("LeftArm"), TEXT("RightArm"), TEXT("LeftLeg"), TEXT("RightLeg")
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

    // IK Rig과 같은 이유로 **같은 이름이 있으면 다시 쓴다.** 매번 새로 만들면 `..._1`, `..._2`가 쌓인다.
    const FString Folder = GetDefault<UUPTSettings>()->DefaultRetargeterPath;
    const FString AssetName = FString::Printf(TEXT("RTG_UPT_%s_to_%s"), *SourceName, *TargetName);
    UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, *(Folder / AssetName + TEXT(".") + AssetName));
    const bool bReused = Retargeter != nullptr;
    if (!Retargeter)
    {
        FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
        Retargeter = Cast<UIKRetargeter>(AssetTools.Get().CreateAsset(AssetName, Folder, UIKRetargeter::StaticClass(), Factory));
    }
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
    // IKRetargetFactory가 이미 기본 op 스택을 넣어 둔다. 여기서 또 부르면 스택이 두 벌이 되고
    // (Pelvis Motion / Pelvis Motion_0 ...) 펠비스 보정이 두 번 적용돼 발이 바닥에서 뜬다.
    // 오차가 다리 길이 차이에 비례해, Manny(+6%)에서는 거의 안 보이고 골렘(+55%)에서 크게 드러났다.
    // 싱글턴 op만 "Op not added" 경고를 내고 걸러지므로 로그만으로는 알기 어려웠다.
    if (Controller->GetNumRetargetOps() == 0)
    {
        Controller->AddDefaultOps();
    }
    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceIKRig);
    Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetIKRig);
    Controller->AutoMapChains(EAutoMapChainType::Exact, true);

    // 레스트 포즈(A-Pose/T-Pose) 차이를 보정하지 않으면 동작은 그대로 옮겨져도 자세가 통째로 틀어진다.
    // 측정해 보니 Synty→Manny에서 팔이 항상 52.9° 어긋났다(다리는 2.2°).
    //
    // 엔진의 AutoAlignBones는 쓰지 않는다. 함정이 둘 있고 둘 다 겪었다.
    //  1) 빈 배열을 넘기면 "전부 정렬"로 해석한다(AutoAlignAllBones와 같아진다).
    //  2) 본이 속한 체인 중 원본에 짝이 없는 것이 하나라도 있으면
    //     "Bone should never be retargeted and not in a mapped chain" 어설션으로 **에디터를 내린다**
    //     (IKRetargeterPoseGenerator.cpp:118). 자체 분석으로 만든 Rig에서 실제로 세 번 겪었다.
    // 같은 보정을 직접 계산하는 쪽이 수치도 더 낫다(Manny 쌍 4.7°/11.5° → 3.3°/8.7°).
    //
    // 정렬 기준은 이 리타기터의 체인 구성이 아니라 **역할 그래프**다. 체인을 따라 정렬했더니
    // 체인을 어디서 끊느냐에 결과가 따라붙었고(한 본짜리 Neck 체인에서 10.7° → 31.6°),
    // 같은 스켈레톤인데도 Rig을 만든 방식에 따라 수치가 달라졌다. 그래서 Rig을 넘기지 않는다.
    //
    // 다시 쓰는 경우 지난번 정렬 오프셋이 남아 있다. 이번에 맞추지 않는 본의 값이 섞이지 않도록 비운다.
    if (bReused)
    {
        Controller->ResetRetargetPose(Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target),
            TArray<FName>(), ERetargetSourceOrTarget::Target);
    }

    FUPTRestPoseAligner::AlignTargetToSource(Controller, SourceMesh, TargetMesh);

    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();
    return Retargeter;
}
