#include "Audit/AuditLevelTopologyCommandlet.h"

// Editor-only by design: loads level packages through the editor loader. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
    enum class ELevelRole : uint8
    {
        Standalone,
        PersistentHost,
        Sublevel
    };

    // Hosting wins over being hosted. Anything that streams sublevels in can be opened standalone to
    // drive them, and a copy of it living inside some other level does not take that away.
    ELevelRole ResolveRole(int32 StreamingCount, int32 ReferencedByCount)
    {
        if (StreamingCount > 0)
        {
            return ELevelRole::PersistentHost;
        }

        if (ReferencedByCount > 0)
        {
            return ELevelRole::Sublevel;
        }

        return ELevelRole::Standalone;
    }

    const TCHAR* ToRoleName(ELevelRole Role)
    {
        switch (Role)
        {
            case ELevelRole::Sublevel:       return TEXT("Sublevel");
            case ELevelRole::PersistentHost: return TEXT("PersistentHost");
            default:                         return TEXT("Standalone");
        }
    }
}

UAuditLevelTopologyCommandlet::UAuditLevelTopologyCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditLevelTopologyCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    const UAssetWorkbench::FLevelScanOptions Options = UAssetWorkbench::ParseLevelScanOptions(Params, TEXT("AuditLevelTopology"));

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().SearchAllAssets(/*bSynchronousSearch=*/ true);

    TArray<FName> LevelPackages;
    UAssetWorkbench::CollectLevelPackages(Options, LevelPackages);

    if (LevelPackages.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditLevelTopology: no levels found (levels=%d scandir=%s)"), Options.LevelPaths.Num(), *Options.ScanDir);
    }

    TArray<FLevelNode> Nodes;
    Nodes.Reserve(LevelPackages.Num());

    for (FName LevelPackage : LevelPackages)
    {
        FLevelNode Node;
        Node.LevelPath = LevelPackage.ToString();
        ReadStreamingLevels(LevelPackage, Node);
        Nodes.Add(MoveTemp(Node));
    }

    // Reverse the edges so each level knows who streams it in.
    TMap<FString, int32> IndexByPath;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        IndexByPath.Add(Nodes[Index].LevelPath, Index);
    }

    for (const FLevelNode& Host : Nodes)
    {
        for (const FString& Streamed : Host.StreamingLevels)
        {
            // A streamed level outside the scan scope has no node to annotate, the host still records it.
            if (const int32* TargetIndex = IndexByPath.Find(Streamed))
            {
                Nodes[*TargetIndex].ReferencedBy.AddUnique(Host.LevelPath);
            }
        }
    }

    if (!WriteReport(Options.ReportPath, Nodes))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditLevelTopology: complete. levels_scanned=%d report=%s"), Nodes.Num(), *Options.ReportPath);

    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

void UAuditLevelTopologyCommandlet::ReadStreamingLevels(FName LevelPackage, FLevelNode& OutNode) const
{
    UPackage* Package = LoadPackage(nullptr, *LevelPackage.ToString(), LOAD_None);
    if (!Package)
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load level package: %s"), *LevelPackage.ToString());
        return;
    }

    UWorld* World = UWorld::FindWorldInPackage(Package);
    if (!World)
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("No UWorld in package: %s"), *LevelPackage.ToString());
        return;
    }

    // Streaming entries are authored data, no component update needed to read them.
    for (ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming)
        {
            continue;
        }

        const FString StreamedPackage = Streaming->GetWorldAssetPackageName();
        if (!StreamedPackage.IsEmpty())
        {
            OutNode.StreamingLevels.AddUnique(StreamedPackage);
        }
    }
}

bool UAuditLevelTopologyCommandlet::WriteReport(const FString& ReportPath, const TArray<FLevelNode>& Nodes) const
{
    int32 StandaloneCount = 0;
    int32 HostCount = 0;
    int32 SublevelCount = 0;

    TArray<TSharedPtr<FJsonValue>> ResultsJson;
    ResultsJson.Reserve(Nodes.Num());

    for (const FLevelNode& Node : Nodes)
    {
        const ELevelRole Role = ResolveRole(Node.StreamingLevels.Num(), Node.ReferencedBy.Num());

        switch (Role)
        {
            case ELevelRole::Sublevel:       ++SublevelCount; break;
            case ELevelRole::PersistentHost: ++HostCount; break;
            default:                         ++StandaloneCount; break;
        }

        TArray<TSharedPtr<FJsonValue>> StreamingJson;
        for (const FString& Streamed : Node.StreamingLevels)
        {
            StreamingJson.Add(MakeShared<FJsonValueString>(Streamed));
        }

        TArray<TSharedPtr<FJsonValue>> ReferencedByJson;
        for (const FString& Host : Node.ReferencedBy)
        {
            ReferencedByJson.Add(MakeShared<FJsonValueString>(Host));
        }

        TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
        NodeJson->SetStringField(TEXT("level"), Node.LevelPath);
        NodeJson->SetStringField(TEXT("role"), ToRoleName(Role));
        NodeJson->SetArrayField(TEXT("streaming_levels"), StreamingJson);
        NodeJson->SetArrayField(TEXT("referenced_by"), ReferencedByJson);

        ResultsJson.Add(MakeShared<FJsonValueObject>(NodeJson));
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("phase"), TEXT("audit-level-topology"));
    Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
    Root->SetNumberField(TEXT("levels_scanned"), Nodes.Num());
    Root->SetNumberField(TEXT("standalone_count"), StandaloneCount);
    Root->SetNumberField(TEXT("persistent_host_count"), HostCount);
    Root->SetNumberField(TEXT("sublevel_count"), SublevelCount);
    Root->SetArrayField(TEXT("results"), ResultsJson);

    return UAssetWorkbench::SaveJsonToFile(Root, ReportPath);
}
