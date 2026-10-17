using UnrealBuildTool;

public class UniversalProductionTools : ModuleRules
{
    public UniversalProductionTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "ToolMenus", "UnrealEd",
            "AssetTools", "AssetRegistry", "ContentBrowser", "HTTP", "Json", "JsonUtilities", "ImageCore", "ImageWrapper",
            "LevelSequence", "LevelSequenceEditor", "SequencerScripting", "MovieScene", "MovieSceneTracks", "CinematicCamera",
            "Projects", "DeveloperSettings", "DesktopPlatform", "IKRig", "IKRigEditor",
            // 결과 애니메이션의 포즈를 시각마다 뽑아 품질을 재는 데 쓴다(UPTRetargetQualityReport).
            "AnimationBlueprintLibrary"
        });
    }
}
