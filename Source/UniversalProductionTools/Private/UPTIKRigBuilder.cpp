#include "UPTIKRigBuilder.h"

#include "UPTSettings.h"
#include "UPTSkeletonProfile.h"

#include "Engine/SkeletalMesh.h"
#include "ObjectTools.h"
#include "ReferenceSkeleton.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "RigEditor/IKRigAutoCharacterizer.h"
#include "RigEditor/IKRigAutoFBIK.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTIKRig, Log, All);

// IK Goal을 만들지 말지. 기본은 **끔**이다.
// 켜 보니 Manny 0.1/8.4 -> 5.3/11.8, X_Bot 0.1/7.1 -> 3.8/22.1로 각도가 나빠지고
// 접지도 같이 나빠졌다(발 높이 오차 0.0120 -> 0.0165, 접지 불일치 0.0417 -> 0.0625).
// 자세한 수치와 판단 근거는 Docs/RETARGET_QUALITY.md에 적어 두었다.
static TAutoConsoleVariable<int32> CVarGenerateIKGoals(
    TEXT("UPT.GenerateIKGoals"), 0,
    TEXT("1이면 IK Rig에 FBIK Goal과 Solver를 만든다. 0이면 FK와 펠비스 이동만 쓴다."));

// FBIK Solver 루트를 입력 포즈에 고정할지. 엔진 생성기는 Free로 두는데, 그러면 FBIK가
// Goal에 닿으려고 **펠비스까지 움직인다.** 우리는 펠비스를 Pelvis Motion op이 따로 맡으므로
// 두 보정이 서로를 밀어낸다. PinToInput이면 IK가 팔다리만 건드린다.
static TAutoConsoleVariable<int32> CVarIKPinRoot(
    TEXT("UPT.IKPinRoot"), 1,
    TEXT("1이면 FBIK Solver 루트를 PinToInput으로(팔다리만 푼다). 0이면 엔진 기본값 Free."));

namespace
{
struct FUPTChainSpec
{
    FName ChainName;
    FName StartRole;
    FName EndRole;
    bool bRequired;
};

bool ResolveBone(const UUPTSkeletonProfile* Profile, const FName Role, FName& OutBone)
{
    const FName* Bone = Profile ? Profile->BoneMappings.Find(Role) : nullptr;
    if (!Bone || Bone->IsNone()) return false;
    OutBone = *Bone;
    return true;
}

/** FBIK Solver의 루트 동작을 설정한다. 엔진 생성기가 Free로 두고 가므로 뒤에서 덮어쓴다. */
void ApplyIKRootBehavior(const UIKRigController* Controller)
{
    if (!Controller || CVarIKPinRoot.GetValueOnAnyThread() == 0) return;
    for (int32 SolverIndex = 0; SolverIndex < Controller->GetNumSolvers(); ++SolverIndex)
    {
        const FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
        if (!SolverStruct || SolverStruct->GetScriptStruct() != FIKRigFullBodyIKSolver::StaticStruct()) continue;
        FIKRigFullBodyIKSolver* Solver = static_cast<FIKRigFullBodyIKSolver*>(Controller->GetSolverAtIndex(SolverIndex));
        if (Solver) Solver->Settings.RootBehavior = EPBIKRootBehavior::PinToInput;
    }
}

/**
 * 폴백 Rig에 FBIK Goal과 Solver를 붙인다.
 *
 * 엔진의 `AutoGenerateFBIK`는 내부에서 Auto Characterize를 다시 돌리고 템플릿에 맞지 않으면
 * 그냥 돌아간다. 폴백 Rig은 애초에 그 판정에 실패해서 폴백으로 내려온 것이므로 쓸 수 없다.
 * 손·발 역할 본에 Goal을 만들고 같은 이름의 체인에 묶어 주는 일만 직접 한다.
 *
 * IK는 보조 수단이라 실패해도 Rig 생성을 막지 않는다. 없으면 FK로만 가게 된다.
 */
void AttachFallbackFBIK(const UIKRigController* Controller, const UUPTSkeletonProfile* Profile, const FName PelvisBone)
{
    if (!Controller || !Profile || CVarGenerateIKGoals.GetValueOnAnyThread() == 0) return;

    struct FGoalSpec { const TCHAR* GoalName; const TCHAR* Role; const TCHAR* ChainName; };
    static const FGoalSpec GoalSpecs[] = {
        {TEXT("LeftHand_Goal"), TEXT("LeftHand"), TEXT("LeftArm")},
        {TEXT("RightHand_Goal"), TEXT("RightHand"), TEXT("RightArm")},
        {TEXT("LeftFoot_Goal"), TEXT("LeftFoot"), TEXT("LeftLeg")},
        {TEXT("RightFoot_Goal"), TEXT("RightFoot"), TEXT("RightLeg")},
    };

    // Goal을 붙일 수 있는 체인이 하나도 없으면 Solver만 남아 쓸모가 없다. 먼저 센다.
    TArray<TPair<FName, FName>> Ready;  // (Goal 이름, 체인 이름)
    TArray<FName> GoalBones;
    for (const FGoalSpec& Spec : GoalSpecs)
    {
        FName Bone;
        if (!ResolveBone(Profile, FName(Spec.Role), Bone)) continue;
        const FBoneChain* Chain = Controller->GetRetargetChainByName(FName(Spec.ChainName));
        // Goal은 체인의 **끝 본**에 있어야 한다. 아니면 엔진이 경고하고 결과가 나빠진다.
        if (!Chain || Chain->EndBone.BoneName != Bone) continue;
        Ready.Add({FName(Spec.GoalName), FName(Spec.ChainName)});
        GoalBones.Add(Bone);
    }
    if (Ready.IsEmpty())
    {
        UE_LOG(LogUPTIKRig, Warning, TEXT("UPT_IKRIG 폴백 Rig에 붙일 IK Goal이 없습니다. FK로만 갑니다."));
        return;
    }

    const int32 SolverIndex = Controller->AddSolver(FIKRigFullBodyIKSolver::StaticStruct());
    if (SolverIndex == INDEX_NONE) return;
    if (!Controller->SetStartBone(PelvisBone, SolverIndex))
    {
        Controller->RemoveSolver(SolverIndex);
        UE_LOG(LogUPTIKRig, Warning, TEXT("UPT_IKRIG FBIK Solver 루트(%s) 설정에 실패해 IK를 붙이지 않습니다."), *PelvisBone.ToString());
        return;
    }

    int32 Attached = 0;
    for (int32 Index = 0; Index < Ready.Num(); ++Index)
    {
        const FName GoalName = Controller->AddNewGoal(Ready[Index].Key, GoalBones[Index]);
        if (GoalName.IsNone()) continue;
        if (!Controller->ConnectGoalToSolver(GoalName, SolverIndex)) continue;
        if (!Controller->SetRetargetChainGoal(Ready[Index].Value, GoalName)) continue;
        ++Attached;
    }

    // 엔진 생성기와 같은 설정. Free는 Goal에 닿기 위해 필요한 만큼만 펠비스를 움직이게 하고,
    // PullChainAlpha 0은 특히 팔에서 동작이 출렁이는 것을 가라앉힌다.
    if (FIKRigFullBodyIKSolver* Solver = static_cast<FIKRigFullBodyIKSolver*>(Controller->GetSolverAtIndex(SolverIndex)))
    {
        Solver->Settings.RootBehavior = EPBIKRootBehavior::Free;
        Solver->Settings.GlobalPullChainAlpha = 0.0f;
    }
    ApplyIKRootBehavior(Controller);
    UE_LOG(LogUPTIKRig, Log, TEXT("UPT_IKRIG 폴백 Rig에 IK Goal %d개를 붙였습니다."), Attached);
}
}

namespace
{
/**
 * 같은 이름의 IK Rig이 이미 있으면 **그것을 다시 쓴다.**
 *
 * 전에는 매번 새로 만들었고, 엔진이 이름 충돌을 피해 `..._1`, `..._2`를 붙였다.
 * 리타기팅을 몇 번만 돌려도 메시 하나에 Rig이 대여섯 개씩 쌓였다(21개까지 봤다).
 * 내용은 어차피 아래에서 처음부터 다시 채우므로, 비우고 재사용하면 된다. 참조도 끊기지 않는다.
 *
 * 체인뿐 아니라 **Goal과 Solver까지** 비운다. 체인만 비우면 새 체인에는 Goal이 붙지 않는데
 * Solver와 Goal은 남아 있어, 겉보기에는 IK가 갖춰진 Rig인데 **체인에 Goal이 없어 IK가
 * 통째로 건너뛰어지는** 상태가 된다. 또 FBIK 생성은 Solver를 무조건 하나 더 추가하므로
 * 비우지 않으면 돌릴 때마다 Solver가 쌓인다.
 */
UIKRigDefinition* AcquireIKRig(const FString& AssetName)
{
    const FString Folder = GetDefault<UUPTSettings>()->DefaultIKRigPath;
    const FString ObjectPath = Folder / AssetName + TEXT(".") + AssetName;
    if (UIKRigDefinition* Existing = LoadObject<UIKRigDefinition>(nullptr, *ObjectPath))
    {
        if (const UIKRigController* Controller = UIKRigController::GetController(Existing))
        {
            TArray<FName> ChainNames;
            for (const FBoneChain& Chain : Controller->GetRetargetChains()) ChainNames.Add(Chain.ChainName);
            for (const FName ChainName : ChainNames) Controller->RemoveRetargetChain(ChainName);

            TArray<FName> GoalNames;
            for (const UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
            {
                if (Goal) GoalNames.Add(Goal->GoalName);
            }
            for (const FName GoalName : GoalNames) Controller->RemoveGoal(GoalName);

            for (int32 SolverIndex = Controller->GetNumSolvers() - 1; SolverIndex >= 0; --SolverIndex)
            {
                Controller->RemoveSolver(SolverIndex);
            }
        }
        return Existing;
    }
    return UIKRigDefinitionFactory::CreateNewIKRigAsset(Folder, AssetName);
}
}

FName FUPTIKRigBuilder::ResolveMotionBone(const FReferenceSkeleton& Ref, const FName PelvisBone)
{
    int32 Index = Ref.FindBoneIndex(PelvisBone);
    for (int32 Guard = 0; Index != INDEX_NONE && Guard < 8; ++Guard)
    {
        if (Ref.GetRefBonePose()[Index].GetTranslation().SizeSquared() > 1.0) break;  // 1cm 이상이면 이 본이 높이를 든다
        const int32 Parent = Ref.GetParentIndex(Index);
        if (Parent == INDEX_NONE) break;                     // 스켈레톤 루트
        if (Ref.GetParentIndex(Parent) == INDEX_NONE) break;  // 부모가 루트면 더 올라가지 않는다
        Index = Parent;
    }
    return Index == INDEX_NONE ? PelvisBone : Ref.GetBoneName(Index);
}

UIKRigDefinition* FUPTIKRigBuilder::CreateUniversalIKRig(USkeletalMesh* Mesh, FString& OutError)
{
    if (!Mesh) { OutError = TEXT("Skeletal Mesh가 없습니다."); return nullptr; }

    FString SafeName = ObjectTools::SanitizeObjectName(Mesh->GetName());
    if (SafeName.IsEmpty()) SafeName = TEXT("Skeleton");
    UIKRigDefinition* IKRig = AcquireIKRig(FString(TEXT("IK_UPT_AUTO_")) + SafeName);
    if (!IKRig) { OutError = TEXT("범용 IK Rig 에셋을 생성하지 못했습니다."); return nullptr; }

    UIKRigController* Controller = UIKRigController::GetController(IKRig);
    if (!Controller || !Controller->SetSkeletalMesh(Mesh))
    { OutError = TEXT("범용 IK Rig에 Skeletal Mesh를 설정하지 못했습니다."); return nullptr; }

    // Auto Characterizer 구현체는 엔진 모듈 외부로 export되지 않으므로 공개 Controller API로 판정합니다.
    FAutoCharacterizeResults Results;
    Controller->AutoGenerateRetargetDefinition(Results);
    if (!Results.bUsedTemplate || !Controller->ApplyAutoGeneratedRetargetDefinition())
    {
        OutError = FString::Printf(
            TEXT("UE Auto Characterizer가 '%s' 구조를 알려진 휴머노이드로 판정하지 못했습니다(최고 일치도 %.0f%%)."),
            *Mesh->GetName(), Results.BestPercentageOfTemplateScore * 100.0f);
        return nullptr;
    }

    // Root 체인은 필수가 아니다. 엔진 Auto Characterizer도 Mixamo처럼 별도 루트 본이 없는 리그에서는
    // Root 체인을 만들지 않는다(X_Bot 확인). 필수로 걸어 두면 그런 리그가 엔진 Rig을 못 쓰고
    // 폴백으로 내려가고, 폴백은 `Hips → Hips` 체인을 만들어 리타기팅 루트를 FK 체인에 가둔다.
    const TArray<FName> RequiredChains = {
        TEXT("Spine"), TEXT("LeftArm"), TEXT("RightArm"), TEXT("LeftLeg"), TEXT("RightLeg")
    };
    for (const FName ChainName : RequiredChains)
    {
        if (!Controller->GetRetargetChainByName(ChainName))
        {
            OutError = FString::Printf(TEXT("자동 Characterize 결과에 필수 체인 %s가 없습니다."), *ChainName.ToString());
            return nullptr;
        }
    }

    Controller->SortRetargetChains();

    // 체인만 만들어 두면 **IK 패스가 통째로 비어 돈다.** 리타기터의 `Retarget IK Goals` op은
    // 체인에 Goal이 붙어 있는 것만 처리하고(IKChainsOp.cpp:290), 하나도 없으면 초기화에
    // 실패해 아무 일도 하지 않는다. IK op을 전부 꺼도 수치가 소수점 둘째 자리까지 그대로였던
    // 이유가 이것이다 — 지금까지의 품질은 전부 FK와 펠비스 이동에서 나온 것이었다.
    // 엔진 생성기는 Auto Characterize 템플릿을 다시 쓰므로 AUTO 경로에서만 쓸 수 있다.
    if (CVarGenerateIKGoals.GetValueOnAnyThread() != 0)
    {
    FAutoFBIKResults FBIKResults;
    Controller->AutoGenerateFBIK(FBIKResults);
    if (FBIKResults.Outcome != EAutoFBIKResult::AllOk)
    {
        UE_LOG(LogUPTIKRig, Warning,
            TEXT("UPT_IKRIG '%s' FBIK 자동 생성 결과 %d (없는 체인 %d개). IK 패스 없이 FK로만 갑니다."),
            *Mesh->GetName(), static_cast<int32>(FBIKResults.Outcome), FBIKResults.MissingChains.Num());
    }
    ApplyIKRootBehavior(Controller);
    }

    IKRig->MarkPackageDirty();
    return IKRig;
}

UIKRigDefinition* FUPTIKRigBuilder::CreateIKRig(UUPTSkeletonProfile* Profile, FString& OutError)
{
    if (!Profile) { OutError = TEXT("Skeleton Profile을 선택하세요."); return nullptr; }
    if (!Profile->bOverridesValid) { OutError = TEXT("Override 오류가 있는 Profile은 사용할 수 없습니다."); return nullptr; }
    if (Profile->RetargetReadiness < 0.70f) { OutError = TEXT("리타기팅 준비도가 70% 미만입니다. Mapping을 검수하세요."); return nullptr; }

    USkeletalMesh* Mesh = Profile->SourceMesh.LoadSynchronous();
    if (!Mesh) { OutError = TEXT("Profile의 Source Skeletal Mesh를 불러오지 못했습니다."); return nullptr; }
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();

    FName RetargetRoot;
    if (!ResolveBone(Profile, TEXT("Pelvis"), RetargetRoot)) { OutError = TEXT("필수 Pelvis Mapping이 없습니다."); return nullptr; }

    // Root 체인을 `Root → Pelvis`로 잡으면 **펠비스가 FK 체인 안에 들어간다.**
    // FK 체인 리타기팅은 회전만 옮기므로 펠비스 이동이 레스트 포즈 값으로 덮이고,
    // 그 결과 몸이 위아래로 전혀 움직이지 않아 발이 바닥에서 떴다.
    // 엔진 Auto Characterizer는 `root → root`로 잡고 펠비스는 Pelvis Motion op에 맡긴다.
    // 같은 메시(SKM_Manny)에 두 Rig을 각각 써서 확인했다 — 엔진 Rig은 진폭 11.2, 폴백은 0.00이었다.
    const TArray<FUPTChainSpec> Specs = {
        {TEXT("Root"), TEXT("Root"), TEXT("Root"), true},
        {TEXT("Spine"), TEXT("Spine"), TEXT("Head"), true},   // 끝 본은 아래에서 목 바로 앞까지로 줄인다
        {TEXT("Head"), TEXT("Head"), TEXT("Head"), false},
        {TEXT("LeftArm"), TEXT("LeftUpperArm"), TEXT("LeftHand"), true},
        {TEXT("RightArm"), TEXT("RightUpperArm"), TEXT("RightHand"), true},
        {TEXT("LeftLeg"), TEXT("LeftThigh"), TEXT("LeftFoot"), true},
        {TEXT("RightLeg"), TEXT("RightThigh"), TEXT("RightFoot"), true},
        {TEXT("LeftClavicle"), TEXT("LeftClavicle"), TEXT("LeftUpperArm"), false},
        {TEXT("RightClavicle"), TEXT("RightClavicle"), TEXT("RightUpperArm"), false},
        {TEXT("Neck"), TEXT("Neck"), TEXT("Head"), false}
    };

    struct FResolvedChain { FName Name; FName Start; FName End; };
    TArray<FResolvedChain> Chains;
    for (const FUPTChainSpec& Spec : Specs)
    {
        FName StartBone, EndBone;
        const bool bHasStart = ResolveBone(Profile, Spec.StartRole, StartBone);
        const bool bHasEnd = ResolveBone(Profile, Spec.EndRole, EndBone);
        if (!bHasStart || !bHasEnd)
        {
            if (Spec.bRequired) { OutError = FString::Printf(TEXT("필수 체인 %s의 Mapping이 부족합니다."), *Spec.ChainName.ToString()); return nullptr; }
            continue;
        }
        const int32 StartIndex = Ref.FindBoneIndex(StartBone);
        const int32 EndIndex = Ref.FindBoneIndex(EndBone);
        if (StartIndex == INDEX_NONE || EndIndex == INDEX_NONE)
        { OutError = FString::Printf(TEXT("체인 %s가 Source Mesh에 없는 본을 참조합니다."), *Spec.ChainName.ToString()); return nullptr; }
        if (StartIndex != EndIndex && !Ref.BoneIsChildOf(EndIndex, StartIndex))
        { OutError = FString::Printf(TEXT("체인 %s의 %s → %s 계층이 연결되지 않습니다."), *Spec.ChainName.ToString(), *StartBone.ToString(), *EndBone.ToString()); return nullptr; }
        Chains.Add({Spec.ChainName, StartBone, EndBone});
    }

    // Spine과 Neck의 끝을 '다음 체인 시작 본의 부모'까지로 줄인다.
    //
    // 전에는 둘 다 Head에서 끝나 **같은 본이 세 체인에 동시에 들어갔다**
    // (Spine: spine_01→head, Neck: neck_01→head, 그리고 Head 자체).
    // 엔진 Auto Characterizer는 `spine_01→spine_05`, `neck_01→neck_02`, `head→head`로 끊는다.
    // 한 본이 여러 체인에 걸치면 레스트 포즈 정렬에서도 제외해야 해서(엔진 어설션 회피),
    // 정렬이 닿지 않는 본이 늘어난다.
    auto TrimChainEnd = [&Ref, &Chains](const FName ChainName, const FName NextChainStart)
    {
        const int32 NextIndex = Ref.FindBoneIndex(NextChainStart);
        if (NextIndex == INDEX_NONE) return;
        const int32 ParentIndex = Ref.GetParentIndex(NextIndex);
        if (ParentIndex == INDEX_NONE) return;
        FResolvedChain* Chain = Chains.FindByPredicate([ChainName](const FResolvedChain& Candidate)
        {
            return Candidate.Name == ChainName;
        });
        if (!Chain) return;
        const int32 StartIndex = Ref.FindBoneIndex(Chain->Start);
        // 줄인 끝이 여전히 시작 본과 연결돼 있어야 한다.
        if (StartIndex == INDEX_NONE) return;
        if (ParentIndex != StartIndex && !Ref.BoneIsChildOf(ParentIndex, StartIndex)) return;
        Chain->End = Ref.GetBoneName(ParentIndex);
    };

    FName NeckBone, HeadBone;
    const bool bHasNeck = ResolveBone(Profile, TEXT("Neck"), NeckBone);
    const bool bHasHead = ResolveBone(Profile, TEXT("Head"), HeadBone);
    if (bHasNeck) TrimChainEnd(TEXT("Spine"), NeckBone);
    else if (bHasHead) TrimChainEnd(TEXT("Spine"), HeadBone);
    if (bHasHead) TrimChainEnd(TEXT("Neck"), HeadBone);

    FString SafeName = ObjectTools::SanitizeObjectName(Mesh->GetName());
    if (SafeName.IsEmpty()) SafeName = TEXT("Skeleton");
    UIKRigDefinition* IKRig = AcquireIKRig(FString(TEXT("IK_UPT_")) + SafeName);
    if (!IKRig) { OutError = TEXT("IK Rig 에셋을 생성하지 못했습니다."); return nullptr; }

    UIKRigController* Controller = UIKRigController::GetController(IKRig);
    if (!Controller || !Controller->SetSkeletalMesh(Mesh)) { OutError = TEXT("IK Rig에 Source Mesh를 설정하지 못했습니다."); return nullptr; }
    // 분석기의 Pelvis가 아니라 **실제로 높이를 드는 본**을 리타기팅 루트로 잡는다(위 헤더 주석 참고).
    const FName MotionBone = ResolveMotionBone(Mesh->GetRefSkeleton(), RetargetRoot);
    if (!Controller->SetRetargetRoot(MotionBone)) { OutError = TEXT("Retarget Root 설정에 실패했습니다."); return nullptr; }

    for (const FResolvedChain& Chain : Chains)
    {
        // 리타기팅 루트를 FK 체인에 넣으면 그 본의 이동이 회전 전용 리타기팅에 덮여 사라진다.
        // Mixamo처럼 루트와 펠비스가 같은 본이면 Root 체인이 바로 그 경우가 된다. 만들지 않는다.
        if (Chain.Start == MotionBone && Chain.End == MotionBone)
        {
            UE_LOG(LogTemp, Log, TEXT("UPT_IKRIG 체인 %s는 리타기팅 루트(%s) 하나뿐이라 만들지 않습니다."),
                *Chain.Name.ToString(), *MotionBone.ToString());
            continue;
        }
        const FName CreatedName = Controller->AddRetargetChain(Chain.Name, Chain.Start, Chain.End, NAME_None);
        if (CreatedName.IsNone()) { OutError = FString::Printf(TEXT("IK Rig 체인 %s 생성에 실패했습니다."), *Chain.Name.ToString()); return nullptr; }
    }
    Controller->SortRetargetChains();
    AttachFallbackFBIK(Controller, Profile, MotionBone);
    IKRig->MarkPackageDirty();
    return IKRig;
}

// 아래 블록은 **지금 켜면 컴파일되지 않는다.** `Profile->JointLimitPresets`와 `FUPTJointLimitPreset`은
// Skeleton Profile에서 이미 사라졌다. 즉 손·발 FBIK Goal과 팔꿈치·무릎 관절 제한은 한 번도 동작한 적이
// 없고, 그런데도 문서는 그것을 기능으로 적고 있었다(2026-10-07에 네 문서에서 바로잡았다).
// 지금 쓰이는 Goal 생성은 위 AttachFallbackFBIK이고 관절 제한은 들어 있지 않다.
// 되살릴 생각이면 Profile 쪽 필드를 먼저 복구해야 한다. 참고용으로만 남겨 둔다.
#if 0 // 컴파일 불가 (Profile에 JointLimitPresets 없음). 제거 대상 — 파일 정리 승인 후 블록 삭제
bool FUPTIKRigBuilder::AddFullBodyIK(UIKRigDefinition* IKRig, UUPTSkeletonProfile* Profile, FString& OutError)
{
    if (!IKRig || !Profile) { OutError = TEXT("IK Rig과 Skeleton Profile을 선택하세요."); return false; }
    UIKRigController* Controller = UIKRigController::GetController(IKRig);
    if (!Controller) { OutError = TEXT("IK Rig Controller를 가져오지 못했습니다."); return false; }
    if (Controller->GetNumSolvers() > 0) { OutError = TEXT("이미 Solver가 있는 IK Rig에는 자동 FBIK를 중복 추가하지 않습니다."); return false; }

    USkeletalMesh* Mesh = Profile->SourceMesh.LoadSynchronous();
    if (!Mesh || Controller->GetSkeletalMesh() != Mesh) { OutError = TEXT("IK Rig Preview Mesh와 Profile Source Mesh가 일치하지 않습니다."); return false; }

    FName Pelvis;
    if (!ResolveBone(Profile, TEXT("Pelvis"), Pelvis)) { OutError = TEXT("Pelvis Mapping이 없습니다."); return false; }

    struct FGoalSpec { FName GoalName; FName Role; FName ChainName; };
    const TArray<FGoalSpec> GoalSpecs = {
        {TEXT("LeftHand_Goal"), TEXT("LeftHand"), TEXT("LeftArm")},
        {TEXT("RightHand_Goal"), TEXT("RightHand"), TEXT("RightArm")},
        {TEXT("LeftFoot_Goal"), TEXT("LeftFoot"), TEXT("LeftLeg")},
        {TEXT("RightFoot_Goal"), TEXT("RightFoot"), TEXT("RightLeg")}
    };

    for (const FGoalSpec& Spec : GoalSpecs)
    {
        FName Bone;
        if (!ResolveBone(Profile, Spec.Role, Bone)) { OutError = FString::Printf(TEXT("FBIK 필수 Mapping %s가 없습니다."), *Spec.Role.ToString()); return false; }
        if (!Controller->GetRetargetChainByName(Spec.ChainName)) { OutError = FString::Printf(TEXT("FBIK 필수 체인 %s가 없습니다."), *Spec.ChainName.ToString()); return false; }
    }

    const int32 SolverIndex = Controller->AddSolver(FIKRigFullBodyIKSolver::StaticStruct());
    if (SolverIndex == INDEX_NONE) { OutError = TEXT("Full Body IK Solver를 추가하지 못했습니다."); return false; }
    if (!Controller->SetStartBone(Pelvis, SolverIndex)) { OutError = TEXT("Full Body IK Solver Root를 설정하지 못했습니다."); return false; }

    for (const FGoalSpec& Spec : GoalSpecs)
    {
        FName Bone;
        ResolveBone(Profile, Spec.Role, Bone);
        const FName GoalName = Controller->AddNewGoal(Spec.GoalName, Bone);
        if (GoalName.IsNone()) { OutError = FString::Printf(TEXT("Goal %s 생성에 실패했습니다."), *Spec.GoalName.ToString()); return false; }
        if (!Controller->ConnectGoalToSolver(GoalName, SolverIndex)) { OutError = FString::Printf(TEXT("Goal %s를 FBIK Solver에 연결하지 못했습니다."), *GoalName.ToString()); return false; }
        if (!Controller->SetRetargetChainGoal(Spec.ChainName, GoalName)) { OutError = FString::Printf(TEXT("Goal %s를 Retarget Chain에 연결하지 못했습니다."), *GoalName.ToString()); return false; }
    }

    FIKRigFullBodyIKSolver* Solver = static_cast<FIKRigFullBodyIKSolver*>(Controller->GetSolverAtIndex(SolverIndex));
    if (Solver)
    {
        Solver->Settings.RootBehavior = EPBIKRootBehavior::Free;
        Solver->Settings.GlobalPullChainAlpha = 0.0f;
    }

    for (const FName Bone : Profile->FingerBones) Controller->SetBoneExcluded(Bone, true);
    for (const FName Bone : Profile->FaceBones) Controller->SetBoneExcluded(Bone, true);
    for (const FUPTJointLimitPreset& Preset : Profile->JointLimitPresets)
    {
        if (!Preset.bApply) continue;
        if (Mesh->GetRefSkeleton().FindBoneIndex(Preset.Bone) == INDEX_NONE)
        { OutError = FString::Printf(TEXT("Joint Preset 본 %s가 Source Mesh에 없습니다."), *Preset.Bone.ToString()); return false; }
        const auto IsValidRange = [](const FVector2D& Range) { return Range.X >= -180.0f && Range.X <= 0.0f && Range.Y >= 0.0f && Range.Y <= 180.0f; };
        if ((Preset.bLimitX && !IsValidRange(Preset.XRange)) || (Preset.bLimitY && !IsValidRange(Preset.YRange)) || (Preset.bLimitZ && !IsValidRange(Preset.ZRange)))
        { OutError = FString::Printf(TEXT("Joint Preset %s의 Limit은 Min -180~0, Max 0~180 범위여야 합니다."), *Preset.SemanticRole.ToString()); return false; }
        Controller->AddBoneSetting(Preset.Bone, SolverIndex);
        FIKRigFBIKBoneSettings* Settings = reinterpret_cast<FIKRigFBIKBoneSettings*>(Controller->GetSettingsForBone(Preset.Bone, SolverIndex));
        if (!Settings) { OutError = FString::Printf(TEXT("Joint Preset %s 설정 생성에 실패했습니다."), *Preset.SemanticRole.ToString()); return false; }
        Settings->bUsePreferredAngles = !Preset.PreferredAngles.IsNearlyZero();
        Settings->PreferredAngles = Preset.PreferredAngles;
        if (Preset.bLimitX) { Settings->X = EPBIKLimitType::Limited; Settings->MinX = Preset.XRange.X; Settings->MaxX = Preset.XRange.Y; }
        if (Preset.bLimitY) { Settings->Y = EPBIKLimitType::Limited; Settings->MinY = Preset.YRange.X; Settings->MaxY = Preset.YRange.Y; }
        if (Preset.bLimitZ) { Settings->Z = EPBIKLimitType::Limited; Settings->MinZ = Preset.ZRange.X; Settings->MaxZ = Preset.ZRange.Y; }
    }
    IKRig->MarkPackageDirty();
    Controller->BroadcastNeedsReinitialized();
    return true;
}
#endif
