#pragma once
#include "Modules/ModuleManager.h"

class FUniversalProductionToolsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    static void OpenAnimationRetargetWindow(class USkeletalMesh* TargetMesh);
private:
    void RegisterMenus();
    void RegisterSkeletalMeshContextMenu();
    TSharedRef<class SDockTab> SpawnMainTab(const class FSpawnTabArgs& Args);
};
