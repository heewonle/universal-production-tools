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
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/FloorConstraintOp.h"
#include "Retargeter/RetargetOps/IKChainsOp.h"
#include "Retargeter/RetargetOps/RunIKRigOp.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTIKRetargeter, Log, All);

// 손·발에만 남은 오차가 IK Goal 쪽에서 오는지 확인하기 위한 진단 스위치.
// 1로 두면 이름에 IK가 들어간 op을 모두 끈다(접지가 나빠지므로 측정용으로만 쓴다).
static TAutoConsoleVariable<int32> CVarDisableIKOps(
    TEXT("UPT.DisableIKOps"), 0,
    TEXT("1이면 리타기터의 IK 관련 op을 모두 끈다(진단용)."));

// 발을 바닥에 붙이는 op을 스택에 넣을지. 기본은 **끔**이다.
// 이 op은 IK Goal의 위치를 보정하므로 IK Goal이 있어야 의미가 있다(UPT.GenerateIKGoals 1).
static TAutoConsoleVariable<int32> CVarFloorConstraint(
    TEXT("UPT.FloorConstraint"), 0,
    TEXT("1이면 다리 체인에 Floor Constraint op을 붙인다(IK Goal이 있을 때만 효과가 있다)."));

// 본 개수가 같은 체인의 FK 회전을 길이 비율 보간 대신 1:1로 옮긴다(아래 ApplyOneToOneForEqualChains 참고).
// 기본은 **끔**이다. 각도는 완벽해지는데(최악 8.4° -> 0.1°) 접지가 나빠진다
// (접지 불일치 0.0417 -> 0.0625, 발 높이 오차 0.0120 -> 0.0181). 각도를 정확히 복사하면
// 뼈 길이가 다른 몸에서는 발이 닿는 높이가 달라지기 때문이다. 길이 정규화가 그걸 메워 주고 있었다.
static TAutoConsoleVariable<int32> CVarFKOneToOne(
    TEXT("UPT.FKOneToOne"), 0,
    TEXT("1이면 원본·대상 체인의 본 개수가 같을 때 FK 회전 모드를 One to One으로 바꾼다(각도↑ 접지↓)."));

namespace
{
    // Root 체인은 필수가 아니다. 엔진 Auto Characterizer도 Mixamo처럼 별도 루트 본이 없는 리그에서는
    // Root 체인을 만들지 않는다(X_Bot 확인). 필수로 걸어 두면 그런 리그가 엔진 Rig을 못 쓰고
    // 폴백으로 내려가고, 폴백은 `Hips → Hips` 체인을 만들어 리타기팅 루트를 FK 체인에 가둔다.
const TArray<FName> RequiredChains = {
    TEXT("Spine"), TEXT("LeftArm"), TEXT("RightArm"), TEXT("LeftLeg"), TEXT("RightLeg")
};

/** 체인에 든 본 개수. 끝 본에서 시작 본까지 계층을 거슬러 올라가 센다. 계층이 끊기면 0. */
int32 CountChainBones(const FReferenceSkeleton& Ref, const FBoneChain& Chain)
{
    const FName StartName = Chain.StartBone.BoneName;
    const FName EndName = Chain.EndBone.BoneName;
    int32 Index = EndName != NAME_None ? Ref.FindBoneIndex(EndName) : Ref.FindBoneIndex(StartName);
    for (int32 Count = 1; Index != INDEX_NONE && Count <= 64; ++Count)
    {
        if (Ref.GetBoneName(Index) == StartName) return Count;
        Index = Ref.GetParentIndex(Index);
    }
    return 0;
}

TMap<FName, int32> BuildChainLengths(const USkeletalMesh* Mesh, const UIKRigDefinition* IKRig)
{
    TMap<FName, int32> Lengths;
    const UIKRigController* RigController = IKRig ? UIKRigController::GetController(IKRig) : nullptr;
    if (!Mesh || !RigController) return Lengths;
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    for (const FBoneChain& Chain : RigController->GetRetargetChains())
    {
        Lengths.Add(Chain.ChainName, CountChainBones(Ref, Chain));
    }
    return Lengths;
}

/**
 * 다리 체인에 Floor Constraint op을 붙인다.
 *
 * 이 op은 IK Goal의 높이를 바닥 기준으로 보정한다. 체인에 Goal이 없으면 아무 일도 하지 않는다.
 * 기본값은 모든 체인이 `EnableFloorConstraint = false`라, 넣기만 해서는 효과가 없다.
 * 다리 체인만 켠다. 손은 바닥에 붙일 이유가 없다.
 *
 * 부모는 `Retarget IK Goals`가 아니라 **`Run IK Rig`**이다(FloorConstraintOp.cpp:209).
 * 이름만 보고 IK Goals에 붙였더니 정리 단계에서 부모 타입이 맞지 않아 op이 통째로 사라지고
 * "Retarget ops were lost during cleaning" ensure로 **에디터가 멈췄다.**
 */
void AttachFloorConstraint(UIKRetargeterController* Controller)
{
    if (CVarFloorConstraint.GetValueOnAnyThread() == 0 || !Controller) return;

    FName ParentOpName = NAME_None;
    for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
    {
        const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(OpIndex);
        if (Op && Op->GetType() == FIKRetargetRunIKRigOp::StaticStruct())
        {
            ParentOpName = Controller->GetOpName(OpIndex);
            break;
        }
    }
    if (ParentOpName == NAME_None)
    {
        UE_LOG(LogUPTIKRetargeter, Warning, TEXT("UPT_RETARGETER Run IK Rig op이 없어 Floor Constraint를 붙이지 못했습니다."));
        return;
    }

    // 다시 쓰는 에셋에는 이미 붙어 있을 수 있다. 두 번 넣으면 보정이 두 번 걸린다.
    int32 FloorOpIndex = INDEX_NONE;
    for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
    {
        const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(OpIndex);
        if (Op && Op->GetType() == FIKRetargetFloorConstraintOp::StaticStruct())
        {
            FloorOpIndex = OpIndex;
            break;
        }
    }
    if (FloorOpIndex == INDEX_NONE)
    {
        FloorOpIndex = Controller->AddRetargetOp(FIKRetargetFloorConstraintOp::StaticStruct(), ParentOpName);
    }
    if (FloorOpIndex == INDEX_NONE)
    {
        UE_LOG(LogUPTIKRetargeter, Warning, TEXT("UPT_RETARGETER Floor Constraint op을 추가하지 못했습니다."));
        return;
    }

    FIKRetargetOpBase* FloorOpBase = Controller->GetRetargetOpByIndex(FloorOpIndex);
    if (!FloorOpBase) return;
    FIKRetargetFloorConstraintOp* FloorOp = static_cast<FIKRetargetFloorConstraintOp*>(FloorOpBase);

    int32 Enabled = 0;
    for (FFloorConstraintChainSettings& ChainSettings : FloorOp->Settings.ChainsToAffect)
    {
        const FString ChainName = ChainSettings.TargetChainName.ToString();
        if (!ChainName.Contains(TEXT("Leg"))) continue;
        ChainSettings.EnableFloorConstraint = true;
        ++Enabled;
    }
    UE_LOG(LogUPTIKRetargeter, Log, TEXT("UPT_RETARGETER Floor Constraint를 다리 체인 %d개에 켰습니다(전체 %d개)."),
        Enabled, FloorOp->Settings.ChainsToAffect.Num());
}

/**
 * 본 개수가 같은 체인의 FK 회전 모드를 One to One으로 바꾼다.
 *
 * 역할 축 정렬을 넣은 뒤 남은 오차가 `calf→foot` 5.7°, `lowerarm→hand` 2.1°였고 나머지 마디는
 * 전부 0.1° 이하였다. IK Goal이 걸린 본이라 IK 쪽을 의심했지만, IK op을 모두 꺼도 수치가
 * **소수점 둘째 자리까지 그대로**였다. 그래서 IK는 아니었다.
 *
 * 남은 마디를 다시 보면 규칙이 정확하다. **0.1°인 마디는 부모 본이 체인의 첫 본이고
 * (`thigh`, `upperarm`, `neck`), 오차가 있는 마디는 그렇지 않다**(`calf`, `lowerarm`).
 * FK Chains op의 기본 회전 모드가 `Interpolated`이기 때문이다. 양쪽 체인을 길이로 정규화한 뒤
 * 같은 비율 위치의 회전을 **이웃 본 사이에서 보간**해 가져온다. 첫 본은 비율 0이라 정확히 오지만,
 * 두 번째 본부터는 허벅지:정강이 길이 비가 다르면 **원본의 다른 본 쪽으로 밀린 값**이 온다.
 * 내가 정렬기에서 직접 겪었던 "마디 한 칸 밀림"과 같은 종류이고, 여기서는 엔진의 의도된 동작이다.
 *
 * 본 개수가 같으면 비율로 찾을 이유가 없다. `One to One`은 i번째를 i번째로 옮긴다.
 * 개수가 다른 체인(트위스트 본 유무 등)은 비율 보간이 여전히 맞으므로 건드리지 않는다.
 */
void ApplyOneToOneForEqualChains(UIKRetargeterController* Controller,
    const USkeletalMesh* SourceMesh, const UIKRigDefinition* SourceIKRig,
    const USkeletalMesh* TargetMesh, const UIKRigDefinition* TargetIKRig)
{
    if (CVarFKOneToOne.GetValueOnAnyThread() == 0 || !Controller) return;
    const TMap<FName, int32> SourceLengths = BuildChainLengths(SourceMesh, SourceIKRig);
    const TMap<FName, int32> TargetLengths = BuildChainLengths(TargetMesh, TargetIKRig);
    if (SourceLengths.IsEmpty() || TargetLengths.IsEmpty()) return;

    for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
    {
        UIKRetargetFKChainsController* FKController =
            Cast<UIKRetargetFKChainsController>(Controller->GetOpController(OpIndex));
        if (!FKController) continue;

        const FName OpName = Controller->GetOpName(OpIndex);
        FIKRetargetFKChainsOpSettings Settings = FKController->GetSettings();
        int32 Changed = 0;
        for (FRetargetFKChainSettings& ChainSettings : Settings.ChainsToRetarget)
        {
            const FName SourceChain = Controller->GetSourceChain(ChainSettings.TargetChainName, OpName);
            if (SourceChain == NAME_None) continue;
            const int32* SourceCount = SourceLengths.Find(SourceChain);
            const int32* TargetCount = TargetLengths.Find(ChainSettings.TargetChainName);
            if (!SourceCount || !TargetCount || *SourceCount == 0 || *SourceCount != *TargetCount) continue;
            ChainSettings.RotationMode = EFKChainRotationMode::OneToOne;
            ++Changed;
        }
        if (Changed > 0)
        {
            FKController->SetSettings(Settings);
            UE_LOG(LogUPTIKRetargeter, Log,
                TEXT("UPT_RETARGETER '%s': 본 개수가 같은 체인 %d개를 One to One 회전으로 바꿨습니다."),
                *OpName.ToString(), Changed);
        }
    }
}
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

    // 역할 축 정렬 뒤 남은 오차가 **손과 발에만** 몰려 있다(발 5.7°, 손 2.1°, 나머지 0.1° 이하).
    // 둘 다 IK Goal이 걸린 본이라, IK 쪽이 원인인지 확인할 수단이 필요하다. 진단용 스위치다.
    ApplyOneToOneForEqualChains(Controller, SourceMesh, SourceIKRig, TargetMesh, TargetIKRig);
    AttachFloorConstraint(Controller);

    if (CVarDisableIKOps.GetValueOnAnyThread() != 0)
    {
        for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
        {
            const FString OpName = Controller->GetOpName(OpIndex).ToString();
            if (!OpName.Contains(TEXT("IK"))) continue;
            Controller->SetRetargetOpEnabled(OpIndex, false);
            UE_LOG(LogUPTIKRetargeter, Log, TEXT("UPT_RETARGETER op '%s' 을 껐습니다(진단)."), *OpName);
        }
    }

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
