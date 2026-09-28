#include "UPTSkeletonProfileBuilder.h"

#include "UPTSettings.h"
#include "UPTSkeletonProfile.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"

UUPTSkeletonProfile* FUPTSkeletonProfileBuilder::FindProfileForMesh(const USkeletalMesh* SkeletalMesh)
{
    if (!SkeletalMesh) return nullptr;
    FARFilter Filter;
    Filter.ClassPaths.Add(UUPTSkeletonProfile::StaticClass()->GetClassPathName());
    TArray<FAssetData> Assets;
    FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);
    const FSoftObjectPath MeshPath(SkeletalMesh);
    for (const FAssetData& Asset : Assets)
    {
        UUPTSkeletonProfile* Profile = Cast<UUPTSkeletonProfile>(Asset.GetAsset());
        if (Profile && Profile->SourceMesh.ToSoftObjectPath() == MeshPath) return Profile;
    }
    return nullptr;
}

FString FUPTSkeletonProfileBuilder::FindManualOverride(const USkeletalMesh* SkeletalMesh)
{
    const UUPTSkeletonProfile* Profile = FindProfileForMesh(SkeletalMesh);
    return Profile ? Profile->ManualOverrideJson.TrimStartAndEnd() : FString();
}

UUPTSkeletonProfile* FUPTSkeletonProfileBuilder::CreateProfile(
    USkeletalMesh* SkeletalMesh,
    const FUPTSkeletonAnalysisData& Analysis,
    const FString& OverrideJson,
    FString& OutError)
{
    if (!SkeletalMesh) { OutError = TEXT("분석된 Skeletal Mesh가 없습니다."); return nullptr; }
    if (!Analysis.bOverridesValid) { OutError = TEXT("수동 보정 JSON 오류를 수정한 뒤 다시 분석하세요."); return nullptr; }
    if (Analysis.BoneMappings.IsEmpty()) { OutError = TEXT("저장할 Semantic Bone Mapping이 없습니다."); return nullptr; }

    UUPTSkeletonProfile* Profile = FindProfileForMesh(SkeletalMesh);
    if (!Profile)
    {
        FString SafeName = ObjectTools::SanitizeObjectName(SkeletalMesh->GetName());
        if (SafeName.IsEmpty()) SafeName = TEXT("Skeleton");
        const FString BasePackage = GetDefault<UUPTSettings>()->DefaultSkeletonProfilePath / (TEXT("UPT_SKP_") + SafeName);
        FString PackageName, AssetName;
        FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(BasePackage, TEXT(""), PackageName, AssetName);

        UPackage* Package = CreatePackage(*PackageName);
        Profile = NewObject<UUPTSkeletonProfile>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (!Profile) { OutError = TEXT("Skeleton Profile 에셋을 생성하지 못했습니다."); return nullptr; }
        FAssetRegistryModule::AssetCreated(Profile);
    }

    Profile->Modify();
    Profile->SourceMesh = SkeletalMesh;
    Profile->BoneMappings.Reset();
    Profile->Confidences.Reset();
    for (const TPair<FString, FName>& Pair : Analysis.BoneMappings) Profile->BoneMappings.Add(FName(*Pair.Key), Pair.Value);
    for (const TPair<FString, float>& Pair : Analysis.Confidences) Profile->Confidences.Add(FName(*Pair.Key), Pair.Value);
    Profile->TwistBones = Analysis.TwistBones;
    Profile->FingerBones = Analysis.FingerBones;
    Profile->FaceBones = Analysis.FaceBones;
    Profile->RetargetReadiness = Analysis.Readiness;
    Profile->bOverridesValid = Analysis.bOverridesValid;
    Profile->ManualOverrideJson = OverrideJson.TrimStartAndEnd();
    Profile->MarkPackageDirty();
    return Profile;
}
