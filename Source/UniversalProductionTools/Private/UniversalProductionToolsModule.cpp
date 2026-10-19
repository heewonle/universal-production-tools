#include "UniversalProductionToolsModule.h"
#include "SUPTMainPanel.h"
#include "SUPTAnimationRetargetWindow.h"

#include "ContentBrowserMenuContexts.h"
#include "Engine/SkeletalMesh.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "UniversalProductionTools"
static const FName UPTMainTabName(TEXT("UniversalProductionTools.Main"));

void FUniversalProductionToolsModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(UPTMainTabName, FOnSpawnTab::CreateRaw(this, &FUniversalProductionToolsModule::SpawnMainTab))
        .SetDisplayName(LOCTEXT("TabTitle", "Universal Production Tools"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUniversalProductionToolsModule::RegisterMenus));
}

void FUniversalProductionToolsModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UPTMainTabName);
}

void FUniversalProductionToolsModule::RegisterMenus()
{
    FToolMenuOwnerScoped Owner(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("WindowLayout"));
    Section.AddMenuEntry(
        TEXT("OpenUniversalProductionTools"),
        LOCTEXT("MenuLabel", "Universal Production Tools"),
        LOCTEXT("MenuTooltip", "Opens the LLM cinematic generator and the skeleton analyzer."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(UPTMainTabName); })));

    RegisterSkeletalMeshContextMenu();
}

void FUniversalProductionToolsModule::RegisterSkeletalMeshContextMenu()
{
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh"));
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
    Section.AddDynamicEntry(TEXT("UPTUniversalAnimationRetarget"), FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
    {
        const UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
        if (!Context || Context->SelectedAssets.Num() != 1) return;

        const FAssetData TargetAsset = Context->SelectedAssets[0];
        InSection.AddMenuEntry(
            TEXT("UPTUniversalAnimationRetarget"),
            LOCTEXT("UniversalAnimationRetarget", "Universal Animation Retarget"),
            LOCTEXT("UniversalAnimationRetargetTooltip", "Select several animations and retarget them to this Skeletal Mesh in one batch."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([TargetAsset]
            {
                OpenAnimationRetargetWindow(Cast<USkeletalMesh>(TargetAsset.GetAsset()));
            })));
    }));
}

void FUniversalProductionToolsModule::OpenAnimationRetargetWindow(USkeletalMesh* TargetMesh)
{
    if (!TargetMesh) return;
    const TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("RetargetWindowTitle", "Universal Animation Retarget"))
        .ClientSize(FVector2D(920.0f, 680.0f))
        .SupportsMaximize(true)
        .SupportsMinimize(false);
    Window->SetContent(SNew(SUPTAnimationRetargetWindow).TargetMesh(TargetMesh).OwnerWindow(Window));
    FSlateApplication::Get().AddWindow(Window);
}

TSharedRef<SDockTab> FUniversalProductionToolsModule::SpawnMainTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SUPTMainPanel)];
}

IMPLEMENT_MODULE(FUniversalProductionToolsModule, UniversalProductionTools)
#undef LOCTEXT_NAMESPACE
