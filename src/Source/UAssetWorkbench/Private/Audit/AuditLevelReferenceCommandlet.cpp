#include "Audit/AuditLevelReferenceCommandlet.h"

// Editor-only by design: drives the editor Asset Registry. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

UAuditLevelReferenceCommandlet::UAuditLevelReferenceCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditLevelReferenceCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditLevelReference commandlet starting..."));

    const UAssetWorkbench::FLevelScanOptions Options = UAssetWorkbench::ParseLevelScanOptions(Params, TEXT("AuditLevelReference"));

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // Force a synchronous full scan, dependency edges and on-disk existence must be current.
    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditLevelReference: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(/*bSynchronousSearch=*/ true);

    TArray<FName> LevelPackages;
    UAssetWorkbench::CollectLevelPackages(Options, LevelPackages);

    if (LevelPackages.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditLevelReference: no levels to audit (levels=%d scandir=%s)"), Options.LevelPaths.Num(), *Options.ScanDir);
    }

    TArray<FLevelResult> Results;
    int32 TotalBroken = 0;

    for (FName LevelPackage : LevelPackages)
    {
        FLevelResult Result;
        Result.LevelPath = LevelPackage.ToString();
        AuditLevel(LevelPackage, AssetRegistry, Result);

        if (Result.BrokenRefs.Num() > 0)
        {
            TotalBroken += Result.BrokenRefs.Num();
            Results.Add(MoveTemp(Result));
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditLevelReference: %s has %d broken ref(s)"), *Results.Last().LevelPath, Results.Last().BrokenRefs.Num());
            for (const FString& Ref : Results.Last().BrokenRefs)
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("    missing: %s"), *Ref);
            }
        }
    }

    if (!WriteReport(Options.ReportPath, Results, LevelPackages.Num()))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditLevelReference: complete. levels_scanned=%d levels_with_broken_refs=%d total_broken=%d"), LevelPackages.Num(), Results.Num(), TotalBroken);

    return ToExitCode(TotalBroken > 0 ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}



void UAuditLevelReferenceCommandlet::AuditLevel(FName LevelPackage, IAssetRegistry& AssetRegistry, FLevelResult& OutResult) const
{
    TArray<FName> Dependencies;
    AssetRegistry.GetDependencies(LevelPackage, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);

    const FString LevelStr = LevelPackage.ToString();

    for (FName Dependency : Dependencies)
    {
        const FString DepStr = Dependency.ToString();

        // /Script/* are code modules, not on-disk packages. Self-edge is not a ref.
        if (DepStr.StartsWith(TEXT("/Script/")) || DepStr == LevelStr)
        {
            continue;
        }
        if (!FPackageName::IsValidLongPackageName(DepStr))
        {
            continue;
        }

        if (!FPackageName::DoesPackageExist(DepStr))
        {
            OutResult.BrokenRefs.AddUnique(DepStr);
        }
    }
}

bool UAuditLevelReferenceCommandlet::WriteReport(const FString& ReportPath, const TArray<FLevelResult>& Results, int32 LevelsScanned) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("phase"), TEXT("audit-level-reference"));
    Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
    Root->SetNumberField(TEXT("levels_scanned"), LevelsScanned);
    Root->SetNumberField(TEXT("levels_with_broken_refs"), Results.Num());

    int32 TotalBroken = 0;
    TArray<TSharedPtr<FJsonValue>> ResultsJson;
    for (const FLevelResult& Result : Results)
    {
        TotalBroken += Result.BrokenRefs.Num();

        TSharedRef<FJsonObject> LevelJson = MakeShared<FJsonObject>();
        LevelJson->SetStringField(TEXT("level"), Result.LevelPath);
        LevelJson->SetNumberField(TEXT("broken_count"), Result.BrokenRefs.Num());

        TArray<TSharedPtr<FJsonValue>> RefsJson;
        for (const FString& Ref : Result.BrokenRefs)
        {
            RefsJson.Add(MakeShared<FJsonValueString>(Ref));
        }
        LevelJson->SetArrayField(TEXT("broken_refs"), RefsJson);

        ResultsJson.Add(MakeShared<FJsonValueObject>(LevelJson));
    }

    Root->SetNumberField(TEXT("total_broken_refs"), TotalBroken);
    Root->SetArrayField(TEXT("results"), ResultsJson);

    FString OutString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("AuditLevelReference: failed to serialize report JSON"));
        return false;
    }

    const FString ReportDir = FPaths::GetPath(ReportPath);
    if (!IFileManager::Get().DirectoryExists(*ReportDir))
    {
        IFileManager::Get().MakeDirectory(*ReportDir, /*Tree=*/ true);
    }

    if (!FFileHelper::SaveStringToFile(OutString, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("AuditLevelReference: failed to write report: %s"), *ReportPath);
        return false;
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditLevelReference: report written: %s"), *ReportPath);
    return true;
}
