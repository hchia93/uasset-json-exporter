using UnrealBuildTool;

public class UAssetWorkbench : ModuleRules
{
    public UAssetWorkbench(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "UnrealEd",
                "AssetRegistry",
                "AssetTools",
                "MediaAssets",
                "Json",
                "JsonUtilities",
                "MessageLog",
                "BlueprintGraph",
                "KismetCompiler",
                "UMG",
                "UMGEditor",
                "MovieScene",
                "Slate",
                "SlateCore",
                "MaterialEditor",
                "RenderCore",
                "RHI",
                "Niagara",
                "NiagaraCore",
                "NiagaraEditor",
                "AIModule",
                "AnimGraph",
                "DirectoryWatcher",
                "Projects",
                "EditorSubsystem",
                "SubobjectDataInterface",
                "TargetPlatform",
                "TextureUtilitiesCommon"
            }
        );
    }
}
