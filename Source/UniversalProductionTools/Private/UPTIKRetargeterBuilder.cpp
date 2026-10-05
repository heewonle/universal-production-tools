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
    // 자체 분석으로 만든 Rig 경로다. 여기서는 레스트 포즈 자동 정렬을 쓰지 않는다(위 헤더 주석 참고).
    return CreateIKRetargeter(SourceMesh, SourceIKRig, TargetMesh, TargetIKRig, OutError, false);  // 폴백 경로
}

UIKRetargeter* FUPTIKRetargeterBuilder::CreateIKRetargeter(
    USkeletalMesh* SourceMesh, UIKRigDefinition* SourceIKRig,
    USkeletalMesh* TargetMesh, UIKRigDefinition* TargetIKRig,
    FString& OutError, bool bAlignRestPose)
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
    //
    // 엔진의 AutoAlignBones는 두 가지 함정이 있다.
    //  1) 빈 배열을 넘기면 "전부 정렬"로 해석한다(AutoAlignAllBones와 같아진다).
    //  2) 본이 속한 체인 중 원본에 짝이 없는 것이 하나라도 있으면
    //     "Bone should never be retargeted and not in a mapped chain" 어설션으로 에디터를 내린다
    //     (IKRetargeterPoseGenerator.cpp: GetChainNameForBone이 그 미매핑 체인을 돌려주기 때문).
    //     손목(hand_l)처럼 팔 체인의 끝이면서 손가락 체인의 시작이기도 한 본이 여기 걸린다.
    // 그래서 '속한 모든 체인이 매핑된 본'만 고른다. 매핑 여부는 이름 비교로 짐작하지 않고
    // 리타기터가 실제로 들고 있는 체인 매핑에서 읽는다. 체인 매핑은 op마다 따로 있고
    // 인자 없는 GetChainMapping()은 비어 있는 op의 것을 돌려줄 수 있으므로, op를 모두 훑어
    // 체인을 가진 매핑만 본다.
    TSet<FName> MappedChainNames;
    for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
    {
        const FRetargetChainMapping* OpMapping = Controller->GetChainMapping(Controller->GetOpName(OpIndex));
        if (!OpMapping || !OpMapping->HasAnyChains()) continue;
        for (const FName ChainName : OpMapping->GetChainNames(ERetargetSourceOrTarget::Target))
        {
            if (OpMapping->GetChainMappedTo(ChainName, ERetargetSourceOrTarget::Target) != NAME_None)
            {
                MappedChainNames.Add(ChainName);
            }
        }
    }

    if (!bAlignRestPose)
    {
        // 엔진 AutoAlignBones는 이 조합에서 어설션으로 에디터를 내린다. 같은 보정을 직접 계산해 넣는다.
        FUPTRestPoseAligner::AlignTargetToSource(Controller, SourceMesh, SourceIKRig, TargetMesh, TargetIKRig, MappedChainNames);
    }

    if (const UIKRigController* TargetRigController = bAlignRestPose ? UIKRigController::GetController(TargetIKRig) : nullptr)
    {
        const FReferenceSkeleton& TargetRef = TargetMesh->GetRefSkeleton();

        // 체인의 시작·끝만 정렬하면 중간 본(lowerarm 등)이 남아 팔이 31.9° 어긋난 채로 남는다.
        // 끝 본에서 시작 본까지 계층을 거슬러 올라가며 체인 전체를 모은다.
        auto CollectChainBones = [&TargetRef](const FBoneChain& Chain, TArray<FName>& OutBones)
        {
            const FName StartName = Chain.StartBone.BoneName;
            const FName EndName = Chain.EndBone.BoneName;
            if (StartName == NAME_None && EndName == NAME_None) return;
            if (StartName != NAME_None) OutBones.AddUnique(StartName);
            int32 Index = EndName != NAME_None ? TargetRef.FindBoneIndex(EndName) : INDEX_NONE;
            for (int32 Guard = 0; Index != INDEX_NONE && Guard < 64; ++Guard)
            {
                const FName BoneName = TargetRef.GetBoneName(Index);
                OutBones.AddUnique(BoneName);
                if (BoneName == StartName) break;
                Index = TargetRef.GetParentIndex(Index);
            }
        };

        // 리타기팅 루트(보통 pelvis)와 그 위쪽은 엔진이 자동 정렬을 지원하지 않는다
        // (AlignBone은 pelvis를 특수 처리하고, 루트 본은 어떤 체인에도 속하지 않아 어설션으로 이어진다).
        const FName RetargetRootBone = TargetRigController->GetRetargetRoot();

        TArray<FName> Candidates;
        TSet<FName> BlockedBones;
        for (const FBoneChain& Chain : TargetRigController->GetRetargetChains())
        {
            if (Chain.ChainName == TEXT("Root")) continue;
            TArray<FName> ChainBones;
            CollectChainBones(Chain, ChainBones);
            if (MappedChainNames.Contains(Chain.ChainName))
            {
                for (const FName BoneName : ChainBones) Candidates.AddUnique(BoneName);
            }
            else
            {
                // 짝 없는 체인에 걸친 본은 매핑된 체인에도 속해 있더라도 제외한다(위 2번).
                BlockedBones.Append(ChainBones);
            }
        }

        TArray<FName> BonesToAlign;
        for (const FName BoneName : Candidates)
        {
            if (BlockedBones.Contains(BoneName)) continue;
            if (BoneName == RetargetRootBone) continue;
            const int32 BoneIndex = TargetRef.FindBoneIndex(BoneName);
            if (BoneIndex == INDEX_NONE || TargetRef.GetParentIndex(BoneIndex) == INDEX_NONE) continue;
            BonesToAlign.Add(BoneName);
        }

        // 빈 배열은 "전부 정렬"이 되어 위험하므로 그때는 아예 부르지 않는다(위 1번).
        if (BonesToAlign.Num() > 0)
        {
            Controller->AutoAlignBones(BonesToAlign, ERetargetAutoAlignMethod::ChainToChain, ERetargetSourceOrTarget::Target);
        }
    }
    Controller->CleanAsset();
    Retargeter->MarkPackageDirty();
    return Retargeter;
}
