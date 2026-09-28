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
            "Projects", "DeveloperSettings", "DesktopPlatform", "IKRig", "IKRigEditor"
        });
    }
}
