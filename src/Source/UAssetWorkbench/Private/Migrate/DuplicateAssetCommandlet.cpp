#include "Migrate/DuplicateAssetCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

UDuplicateAssetCommandlet::UDuplicateAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UDuplicateAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - DuplicateAsset"), UASSET_WORKBENCH_VERSION_STRING);

    const TArray<FString> Pairs = UAssetWorkbench::ParsePathList(Params, TEXT("-pairs="));
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (Pairs.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=DuplicateAsset -pairs=\"/Game/A>/Game/B\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("DuplicateAsset: %d pair(s) %s"), Pairs.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    int32 Duplicated = 0;
    for (const FString& Pair : Pairs)
    {
        FString SourcePath;
        FString DestPath;
        if (!Pair.Split(TEXT(">"), &SourcePath, &DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Malformed pair '%s', expected Source>Destination"), *Pair);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        SourcePath.TrimStartAndEndInline();
        DestPath.TrimStartAndEndInline();

        UObject* Source = LoadObject<UObject>(nullptr, *SourcePath);
        if (!Source)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to load source: %s"), *SourcePath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        const FString DestPackagePath = FPackageName::GetLongPackagePath(DestPath);
        const FString DestAssetName = FPackageName::GetShortName(DestPath);

        // Overwriting would discard whatever was already done to an earlier copy.
        if (FPackageName::DoesPackageExist(DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Destination already exists: %s"), *DestPath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s -> %s"), *SourcePath, *DestPath);
        ++Duplicated;

        if (!bApply)
        {
            continue;
        }

        UObject* Copy = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, Source);
        if (!Copy)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("DuplicateAsset failed for %s"), *SourcePath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        if (!UAssetWorkbench::CompileAndSavePackage(Copy, /* bCompileBlueprint */ true))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save package for %s"), *DestPath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d asset(s) duplicated %s"), Duplicated, bApply ? TEXT("(saved)") : TEXT("(dry run, not saved)"));
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
