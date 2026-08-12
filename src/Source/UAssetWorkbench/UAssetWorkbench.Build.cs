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
                "UnrealEd",
                "AssetRegistry",
                "Json",
                "MessageLog",
                "BlueprintGraph",
                "KismetCompiler",
                "UMG",
                "UMGEditor",
                "MovieScene",
                "Slate",
                "SlateCore",
                "Niagara",
                "NiagaraCore",
                "NiagaraEditor",
                "AIModule",
                "AnimGraph",
                "DirectoryWatcher",
                "Projects",
                "EditorSubsystem"
            }
        );
    }
}
