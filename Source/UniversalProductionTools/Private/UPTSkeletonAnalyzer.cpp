#include "UPTSkeletonAnalyzer.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
enum class EBodyZone : uint8
{
    Root,
    Pelvis,
    CenterUpper,
    Head,
    Arm,
    Hand,
    Leg,
    Foot
};

struct FRoleSpec
{
    FString Role;
    TArray<FString> Tokens;
    FString ParentRole;
    EBodyZone Zone;
    int32 Side = 0; // -1 left, +1 right, 0 center
    bool bRequired = true;
};

struct FRoleMatch
{
    int32 BoneIndex = INDEX_NONE;
    float Confidence = 0.0f;
    float NameScore = 0.0f;
    float SpatialScore = 0.0f;
    float HierarchyScore = 0.0f;
    bool bManual = false;
};

FString Normalize(FString Name)
{
    Name.ToLowerInline();
    Name.ReplaceInline(TEXT("mixamorig:"), TEXT(""));
    Name.ReplaceInline(TEXT("bip001"), TEXT(""));
    Name.ReplaceInline(TEXT("."), TEXT("_"));
    Name.ReplaceInline(TEXT("-"), TEXT("_"));
    Name.ReplaceInline(TEXT(" "), TEXT("_"));
    return Name;
}

float GetNameScore(const FString& BoneName, const TArray<FString>& Tokens)
{
    const FString Normalized = Normalize(BoneName);
    float Best = 0.0f;
    for (const FString& TokenValue : Tokens)
    {
        const FString Token = Normalize(TokenValue);
        if (Normalized == Token) Best = FMath::Max(Best, 0.70f);
        else if (Normalized.EndsWith(TEXT("_") + Token) || Normalized.StartsWith(Token + TEXT("_"))) Best = FMath::Max(Best, 0.64f);
        else if (Normalized.Contains(Token)) Best = FMath::Max(Best, 0.56f);
    }
    return Best;
}

TArray<FTransform> BuildComponentPose(const FReferenceSkeleton& Ref)
{
    const TArray<FTransform>& LocalPose = Ref.GetRefBonePose();
    TArray<FTransform> ComponentPose;
    ComponentPose.SetNum(LocalPose.Num());
    for (int32 Index = 0; Index < LocalPose.Num(); ++Index)
    {
        const int32 ParentIndex = Ref.GetParentIndex(Index);
        ComponentPose[Index] = ParentIndex == INDEX_NONE ? LocalPose[Index] : LocalPose[Index] * ComponentPose[ParentIndex];
    }
    return ComponentPose;
}

int32 GetDescendantDistance(const FReferenceSkeleton& Ref, int32 DescendantIndex, int32 AncestorIndex)
{
    int32 Distance = 0;
    for (int32 Index = DescendantIndex; Index != INDEX_NONE && Index != AncestorIndex; Index = Ref.GetParentIndex(Index))
    {
        ++Distance;
    }
    int32 Check = DescendantIndex;
    for (int32 Step = 0; Step < Distance && Check != INDEX_NONE; ++Step) Check = Ref.GetParentIndex(Check);
    return Check == AncestorIndex ? Distance : INDEX_NONE;
}

float GetAxisValue(const FVector& Position, int32 Axis)
{
    return Axis == 0 ? Position.X : Position.Y;
}

float GetSpatialScore(
    EBodyZone Zone,
    int32 Side,
    const FVector& Position,
    const FBox& Bounds,
    int32 LateralAxis,
    float LeftSign)
{
    const FVector Size = Bounds.GetSize();
    const float Height = FMath::Max(Size.Z, 1.0f);
    const float NormalizedZ = FMath::Clamp((Position.Z - Bounds.Min.Z) / Height, 0.0f, 1.0f);
    const float HalfWidth = FMath::Max((LateralAxis == 0 ? Size.X : Size.Y) * 0.5f, 1.0f);
    const float Lateral = (GetAxisValue(Position, LateralAxis) - GetAxisValue(Bounds.GetCenter(), LateralAxis)) / HalfWidth;

    float Score = 0.0f;
    switch (Zone)
    {
    case EBodyZone::Root:        Score = NormalizedZ < 0.35f ? 0.20f : 0.02f; break;
    case EBodyZone::Pelvis:      Score = 0.20f * FMath::Clamp(1.0f - FMath::Abs(NormalizedZ - 0.42f) / 0.32f, 0.0f, 1.0f); break;
    case EBodyZone::CenterUpper: Score = (NormalizedZ > 0.45f && FMath::Abs(Lateral) < 0.30f) ? 0.20f : 0.04f; break;
    case EBodyZone::Head:        Score = NormalizedZ > 0.82f ? 0.20f : NormalizedZ * 0.12f; break;
    case EBodyZone::Arm:         Score = (NormalizedZ > 0.45f && FMath::Abs(Lateral) > 0.18f) ? 0.18f : 0.03f; break;
    case EBodyZone::Hand:        Score = (NormalizedZ > 0.35f && FMath::Abs(Lateral) > 0.55f) ? 0.20f : 0.03f; break;
    case EBodyZone::Leg:         Score = (NormalizedZ < 0.55f && FMath::Abs(Lateral) > 0.04f) ? 0.18f : 0.03f; break;
    case EBodyZone::Foot:        Score = NormalizedZ < 0.18f ? 0.20f : 0.02f; break;
    }

    if (Side != 0)
    {
        const float ExpectedSign = Side < 0 ? LeftSign : -LeftSign;
        Score += FMath::Sign(Lateral) == FMath::Sign(ExpectedSign) ? 0.08f : -0.12f;
    }
    else if (FMath::Abs(Lateral) < 0.20f)
    {
        Score += 0.05f;
    }
    return FMath::Clamp(Score, 0.0f, 0.28f);
}

bool IsLikelyLeftName(const FString& Name)
{
    const FString N = Normalize(Name);
    return N.Contains(TEXT("left")) || N.StartsWith(TEXT("l_")) || N.EndsWith(TEXT("_l")) ||
        N.Contains(TEXT("riglarm")) || N.Contains(TEXT("riglleg")) || N.Contains(TEXT("_larm")) || N.Contains(TEXT("_lleg"));
}

TArray<FRoleSpec> GetHumanoidSpecs()
{
    return {
        {TEXT("Root"), {TEXT("root"), TEXT("reference"), TEXT("rootnode")}, TEXT(""), EBodyZone::Root, 0, true},
        {TEXT("Pelvis"), {TEXT("pelvis"), TEXT("hips"), TEXT("hip"), TEXT("rigpelvis")}, TEXT("Root"), EBodyZone::Pelvis, 0, true},
        {TEXT("Spine"), {TEXT("spine"), TEXT("chest"), TEXT("torso"), TEXT("rigspine1")}, TEXT("Pelvis"), EBodyZone::CenterUpper, 0, true},
        {TEXT("Neck"), {TEXT("neck"), TEXT("rigneck1")}, TEXT("Spine"), EBodyZone::CenterUpper, 0, false},
        {TEXT("Head"), {TEXT("head"), TEXT("righead")}, TEXT("Spine"), EBodyZone::Head, 0, true},
        {TEXT("LeftClavicle"), {TEXT("clavicle_l"), TEXT("leftshoulder"), TEXT("shoulder_l"), TEXT("l_clavicle"), TEXT("riglarmcollarbone")}, TEXT("Spine"), EBodyZone::Arm, -1, false},
        {TEXT("LeftUpperArm"), {TEXT("upperarm_l"), TEXT("leftarm"), TEXT("l_upperarm"), TEXT("riglarm1")}, TEXT("Spine"), EBodyZone::Arm, -1, true},
        {TEXT("LeftLowerArm"), {TEXT("lowerarm_l"), TEXT("leftforearm"), TEXT("forearm_l"), TEXT("l_forearm"), TEXT("riglarm2")}, TEXT("LeftUpperArm"), EBodyZone::Arm, -1, true},
        {TEXT("LeftHand"), {TEXT("hand_l"), TEXT("lefthand"), TEXT("l_hand"), TEXT("riglarmpalm")}, TEXT("LeftLowerArm"), EBodyZone::Hand, -1, true},
        {TEXT("RightClavicle"), {TEXT("clavicle_r"), TEXT("rightshoulder"), TEXT("shoulder_r"), TEXT("r_clavicle"), TEXT("rigrarmcollarbone")}, TEXT("Spine"), EBodyZone::Arm, 1, false},
        {TEXT("RightUpperArm"), {TEXT("upperarm_r"), TEXT("rightarm"), TEXT("r_upperarm"), TEXT("rigrarm1")}, TEXT("Spine"), EBodyZone::Arm, 1, true},
        {TEXT("RightLowerArm"), {TEXT("lowerarm_r"), TEXT("rightforearm"), TEXT("forearm_r"), TEXT("r_forearm"), TEXT("rigrarm2")}, TEXT("RightUpperArm"), EBodyZone::Arm, 1, true},
        {TEXT("RightHand"), {TEXT("hand_r"), TEXT("righthand"), TEXT("r_hand"), TEXT("rigrarmpalm")}, TEXT("RightLowerArm"), EBodyZone::Hand, 1, true},
        {TEXT("LeftThigh"), {TEXT("thigh_l"), TEXT("leftupleg"), TEXT("l_thigh"), TEXT("riglleg1")}, TEXT("Pelvis"), EBodyZone::Leg, -1, true},
        {TEXT("LeftCalf"), {TEXT("calf_l"), TEXT("leftleg"), TEXT("shin_l"), TEXT("l_calf"), TEXT("riglleg2")}, TEXT("LeftThigh"), EBodyZone::Leg, -1, true},
        {TEXT("LeftFoot"), {TEXT("foot_l"), TEXT("leftfoot"), TEXT("l_foot"), TEXT("ankle_l"), TEXT("rigllegankle"), TEXT("rigllegfoot1")}, TEXT("LeftCalf"), EBodyZone::Foot, -1, true},
        {TEXT("RightThigh"), {TEXT("thigh_r"), TEXT("rightupleg"), TEXT("r_thigh"), TEXT("rigrleg1")}, TEXT("Pelvis"), EBodyZone::Leg, 1, true},
        {TEXT("RightCalf"), {TEXT("calf_r"), TEXT("rightleg"), TEXT("shin_r"), TEXT("r_calf"), TEXT("rigrleg2")}, TEXT("RightThigh"), EBodyZone::Leg, 1, true},
        {TEXT("RightFoot"), {TEXT("foot_r"), TEXT("rightfoot"), TEXT("r_foot"), TEXT("ankle_r"), TEXT("rigrlegankle"), TEXT("rigrlegfoot1")}, TEXT("RightCalf"), EBodyZone::Foot, 1, true}
    };
}

bool ContainsAny(const FString& Name, const TArray<FString>& Tokens)
{
    const FString N = Normalize(Name);
    for (const FString& Token : Tokens)
    {
        if (N.Contains(Token)) return true;
    }
    return false;
}
}

FString FUPTSkeletonAnalyzer::Analyze(USkeletalMesh* SkeletalMesh)
{
    return Analyze(SkeletalMesh, FString());
}

FString FUPTSkeletonAnalyzer::Analyze(USkeletalMesh* SkeletalMesh, const FString& OverrideJson)
{
    return AnalyzeDetailed(SkeletalMesh, OverrideJson).Report;
}

FUPTSkeletonAnalysisData FUPTSkeletonAnalyzer::AnalyzeDetailed(USkeletalMesh* SkeletalMesh, const FString& OverrideJson)
{
    FUPTSkeletonAnalysisData Result;
    if (!SkeletalMesh) { Result.Report = TEXT("Skeletal Mesh를 선택하세요."); return Result; }
    const FReferenceSkeleton& Ref = SkeletalMesh->GetRefSkeleton();
    if (Ref.GetNum() == 0) { Result.Report = TEXT("선택한 Skeletal Mesh에 본이 없습니다."); return Result; }

    const TArray<FTransform> ComponentPose = BuildComponentPose(Ref);
    FBox Bounds(ForceInit);
    for (const FTransform& Transform : ComponentPose) Bounds += Transform.GetLocation();
    const FVector Size = Bounds.GetSize();
    const int32 LateralAxis = Size.Y >= Size.X ? 1 : 0;

    float LeftCoordinateSum = 0.0f;
    int32 LeftCoordinateCount = 0;
    for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
    {
        if (IsLikelyLeftName(Ref.GetBoneName(Index).ToString()))
        {
            LeftCoordinateSum += GetAxisValue(ComponentPose[Index].GetLocation() - Bounds.GetCenter(), LateralAxis);
            ++LeftCoordinateCount;
        }
    }
    const float LeftSign = LeftCoordinateCount > 0 ? (LeftCoordinateSum >= 0.0f ? 1.0f : -1.0f) : -1.0f;

    const TArray<FRoleSpec> Specs = GetHumanoidSpecs();
    TMap<FString, FRoleMatch> Matches;
    TSet<int32> UsedBones;

    for (const FRoleSpec& Spec : Specs)
    {
        FRoleMatch Best;
        const FRoleMatch* ParentMatch = Spec.ParentRole.IsEmpty() ? nullptr : Matches.Find(Spec.ParentRole);
        for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
        {
            if (UsedBones.Contains(BoneIndex)) continue;
            const float NameScore = GetNameScore(Ref.GetBoneName(BoneIndex).ToString(), Spec.Tokens);
            const float SpatialScore = GetSpatialScore(Spec.Zone, Spec.Side, ComponentPose[BoneIndex].GetLocation(), Bounds, LateralAxis, LeftSign);
            float HierarchyScore = 0.0f;
            if (Spec.Role == TEXT("Root") && Ref.GetParentIndex(BoneIndex) == INDEX_NONE) HierarchyScore = 0.50f;
            else if (ParentMatch && ParentMatch->BoneIndex != INDEX_NONE)
            {
                // 이름이 전혀 없는 커스텀 Rig도 몸의 연결 구조만으로 판정할 수 있도록
                // 부모 역할 아래의 연속 체인에 높은 가중치를 둡니다.
                if (Ref.BoneIsChildOf(BoneIndex, ParentMatch->BoneIndex))
                {
                    const int32 Distance = GetDescendantDistance(Ref, BoneIndex, ParentMatch->BoneIndex);
                    HierarchyScore = FMath::Clamp(0.44f - FMath::Max(0, Distance - 1) * 0.05f, 0.20f, 0.44f);
                }
                else HierarchyScore = -0.18f;
            }
            if (Spec.Role == TEXT("Pelvis"))
            {
                TArray<int32> Children;
                if (Ref.GetDirectChildBones(BoneIndex, Children) >= 3) HierarchyScore += 0.08f;
            }

            const float Total = FMath::Clamp(NameScore + SpatialScore + HierarchyScore, 0.0f, 1.0f);
            if (Total > Best.Confidence)
            {
                Best = {BoneIndex, Total, NameScore, SpatialScore, HierarchyScore, false};
            }
        }

        // Spatial-only guesses remain visible but deliberately low-confidence.
        // Clavicle/Neck은 생략 가능한 중간 본이다. 이름 근거 없이 공간만으로 강제 배정하면
        // 실제 UpperArm/Head를 먼저 소비하므로 비표준 구조에서는 비워 둔다.
        const bool bOptionalWithoutNameEvidence = !Spec.bRequired && Best.NameScore <= 0.0f;
        if (Best.Confidence >= 0.20f && !bOptionalWithoutNameEvidence)
        {
            Matches.Add(Spec.Role, Best);
            UsedBones.Add(Best.BoneIndex);
        }
        else
        {
            Matches.Add(Spec.Role, FRoleMatch());
        }
    }

    TArray<FString> OverrideDiagnostics;
    if (!OverrideJson.TrimStartAndEnd().IsEmpty())
    {
        TSharedPtr<FJsonObject> OverrideObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OverrideJson);
        if (!FJsonSerializer::Deserialize(Reader, OverrideObject) || !OverrideObject.IsValid())
        {
            OverrideDiagnostics.Add(TEXT("수동 매핑 JSON 형식이 올바르지 않아 자동 결과만 사용했습니다."));
            Result.bOverridesValid = false;
        }
        else
        {
            TSet<int32> ManualBones;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : OverrideObject->Values)
            {
                if (!Matches.Contains(Pair.Key))
                {
                    OverrideDiagnostics.Add(FString::Printf(TEXT("지원하지 않는 역할: %s"), *Pair.Key));
                    Result.bOverridesValid = false;
                    continue;
                }
                FString BoneName;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(BoneName))
                {
                    OverrideDiagnostics.Add(FString::Printf(TEXT("%s 값은 본 이름 문자열이어야 합니다."), *Pair.Key));
                    Result.bOverridesValid = false;
                    continue;
                }
                int32 BoneIndex = Ref.FindBoneIndex(FName(*BoneName));
                if (BoneIndex == INDEX_NONE)
                {
                    OverrideDiagnostics.Add(FString::Printf(TEXT("%s: 존재하지 않는 본 '%s'"), *Pair.Key, *BoneName));
                    Result.bOverridesValid = false;
                    continue;
                }
                if (ManualBones.Contains(BoneIndex))
                {
                    OverrideDiagnostics.Add(FString::Printf(TEXT("%s: 본 '%s'가 다른 수동 역할에 이미 배정되었습니다."), *Pair.Key, *BoneName));
                    Result.bOverridesValid = false;
                    continue;
                }
                for (TPair<FString, FRoleMatch>& Existing : Matches)
                {
                    if (Existing.Key == Pair.Key || Existing.Value.BoneIndex != BoneIndex) continue;
                    if (Existing.Value.bManual)
                    {
                        OverrideDiagnostics.Add(FString::Printf(TEXT("%s: 본 '%s'가 수동 역할 %s에 이미 배정되었습니다."), *Pair.Key, *BoneName, *Existing.Key));
                        Result.bOverridesValid = false;
                        BoneIndex = INDEX_NONE;
                        break;
                    }
                    OverrideDiagnostics.Add(FString::Printf(TEXT("%s 수동 지정으로 자동 역할 %s의 중복 후보를 해제했습니다."), *Pair.Key, *Existing.Key));
                    Existing.Value = FRoleMatch();
                }
                if (BoneIndex == INDEX_NONE) continue;
                ManualBones.Add(BoneIndex);
                Matches[Pair.Key] = {BoneIndex, 1.0f, 0.0f, 0.0f, 0.0f, true};
            }
        }
    }

    TArray<FString> TwistBones;
    TArray<FString> FingerBones;
    TArray<FString> FaceBones;
    for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
    {
        const FString BoneName = Ref.GetBoneName(BoneIndex).ToString();
        if (ContainsAny(BoneName, {TEXT("twist"), TEXT("roll"), TEXT("torsion")})) TwistBones.Add(BoneName);
        if (ContainsAny(BoneName, {TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky"), TEXT("little")})) FingerBones.Add(BoneName);
        if (ContainsAny(BoneName, {TEXT("jaw"), TEXT("eye"), TEXT("brow"), TEXT("lid"), TEXT("lip"), TEXT("mouth"), TEXT("cheek"), TEXT("nose"), TEXT("tongue")})) FaceBones.Add(BoneName);
    }

    int32 RequiredCount = 0;
    int32 ReadyCount = 0;
    int32 HighConfidenceCount = 0;
    for (const FRoleSpec& Spec : Specs)
    {
        if (!Spec.bRequired) continue;
        ++RequiredCount;
        const FRoleMatch& Match = Matches.FindChecked(Spec.Role);
        if (Match.Confidence >= 0.55f) ++ReadyCount;
        if (Match.Confidence >= 0.80f) ++HighConfidenceCount;
    }

    const float Readiness = RequiredCount > 0 ? static_cast<float>(ReadyCount) / RequiredCount : 0.0f;
    Result.Readiness = Readiness;
    const FString Tier = Readiness >= 0.90f ? TEXT("A - 자동 매핑 후보") : Readiness >= 0.70f ? TEXT("B - 검수 필요") : TEXT("C - 수동 매핑 필요");

    FString Report = FString::Printf(
        TEXT("Mesh: %s\nBones: %d\nBounds: %.1f x %.1f x %.1f cm\nLateral axis: %s / inferred left sign: %s\nTopology: Humanoid candidate\nRetarget readiness: %.0f%% (%s)\nHigh-confidence required roles: %d/%d\n\n"),
        *SkeletalMesh->GetName(), Ref.GetNum(), Size.X, Size.Y, Size.Z,
        LateralAxis == 0 ? TEXT("X") : TEXT("Y"), LeftSign > 0 ? TEXT("positive") : TEXT("negative"),
        Readiness * 100.0f, *Tier, HighConfidenceCount, RequiredCount);

    Report += TEXT("Semantic mapping\n");
    for (const FRoleSpec& Spec : Specs)
    {
        const FRoleMatch& Match = Matches.FindChecked(Spec.Role);
        const FString BoneName = Match.BoneIndex == INDEX_NONE ? TEXT("<not found>") : Ref.GetBoneName(Match.BoneIndex).ToString();
        if (Match.BoneIndex != INDEX_NONE)
        {
            Result.BoneMappings.Add(Spec.Role, Ref.GetBoneName(Match.BoneIndex));
            Result.Confidences.Add(Spec.Role, Match.Confidence);
        }
        const TCHAR* State = Match.bManual ? TEXT("MANUAL") : Match.Confidence >= 0.80f ? TEXT("OK") : Match.Confidence >= 0.55f ? TEXT("CHECK") : TEXT("LOW");
        Report += FString::Printf(TEXT("%-16s %-28s %3.0f%%  %-6s  [name %.0f / spatial %.0f / hierarchy %.0f]\n"),
            *Spec.Role, *BoneName, Match.Confidence * 100.0f, State,
            Match.NameScore * 100.0f, Match.SpatialScore * 100.0f, Match.HierarchyScore * 100.0f);
    }

    Report += FString::Printf(TEXT("\nAuxiliary bones\nTwist/Roll: %d\nFinger: %d\nFace: %d\n"), TwistBones.Num(), FingerBones.Num(), FaceBones.Num());
    if (!TwistBones.IsEmpty()) Report += TEXT("- Twist: ") + FString::Join(TwistBones, TEXT(", ")) + TEXT("\n");
    if (!FingerBones.IsEmpty()) Report += TEXT("- Finger: ") + FString::Join(FingerBones, TEXT(", ")) + TEXT("\n");
    if (!FaceBones.IsEmpty()) Report += TEXT("- Face: ") + FString::Join(FaceBones, TEXT(", ")) + TEXT("\n");

    Report += TEXT("\nDiagnostics\n");
    if (Readiness >= 0.90f) Report += TEXT("- IK Rig 체인 초안 생성에 사용할 수 있는 수준입니다. 생성 전 CHECK/LOW 항목을 검수하세요.\n");
    else Report += TEXT("- 핵심 본 매핑 신뢰도가 부족합니다. IK Rig 자동 생성 전에 수동 Semantic Mapping이 필요합니다.\n");
    if (LeftCoordinateCount == 0) Report += TEXT("- 좌측 이름 표식을 찾지 못해 UE 관례의 음수 좌우축을 가정했습니다.\n");
    for (const FString& Diagnostic : OverrideDiagnostics) Report += TEXT("- Override: ") + Diagnostic + TEXT("\n");
    Report += TEXT("- 이 분석은 원본 에셋을 변경하지 않습니다. 보조 본 분류는 이름 기반이며 IK 체인 생성 대상과 분리됩니다.");
    for (const FString& Name : TwistBones) Result.TwistBones.Add(FName(*Name));
    for (const FString& Name : FingerBones) Result.FingerBones.Add(FName(*Name));
    for (const FString& Name : FaceBones) Result.FaceBones.Add(FName(*Name));
    Result.Report = MoveTemp(Report);
    return Result;
}
