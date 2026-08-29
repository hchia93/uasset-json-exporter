#include "Migrate/ResaveAssetCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

UResaveAssetCommandlet::UResaveAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UResaveAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    const bool bCompile = !FParse::Param(*Params, TEXT("nocompile"));

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=ResaveAsset -assets=\"/Game/A,/Game/Maps/B\" [-nocompile]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("ResaveAsset: %d asset(s), compile=%s"), AssetPaths.Num(), bCompile ? TEXT("true") : TEXT("false"));

    int32 Saved = 0;
    for (const FString& AssetPath : AssetPaths)
    {
        if (ResaveAsset(AssetPath, bCompile))
        {
            ++Saved;
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Resaved %d/%d asset(s)."), Saved, AssetPaths.Num());
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UResaveAssetCommandlet::ResaveAsset(const FString& AssetPath, bool bCompile) const
{
    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load: %s"), *AssetPath);
        return false;
    }

    Asset->GetOutermost()->FullyLoad();

    const bool bSaved = UAssetWorkbench::CompileAndSavePackage(Asset, bCompile);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s: %s"), *Asset->GetOutermost()->GetName(), bSaved ? TEXT("resaved") : TEXT("SAVE FAILED"));
    return bSaved;
}
