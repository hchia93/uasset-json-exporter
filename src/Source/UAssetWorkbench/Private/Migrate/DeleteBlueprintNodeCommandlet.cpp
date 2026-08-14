#include "Migrate/DeleteBlueprintNodeCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Parse.h"

UDeleteBlueprintNodeCommandlet::UDeleteBlueprintNodeCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UDeleteBlueprintNodeCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    const TArray<FString> NodeIdStrings = UAssetWorkbench::ParsePathList(Params, TEXT("-nodes="));
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (NodeIdStrings.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=DeleteBlueprintNode -nodes=\"GUID,GUID\" -assets=\"/Game/A,/Game/B\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSet<FGuid> NodeGuids;
    for (const FString& NodeIdString : NodeIdStrings)
    {
        FGuid NodeGuid;
        if (!FGuid::Parse(NodeIdString, NodeGuid))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Not a node id: '%s'. Copy NodeId verbatim out of the BlueprintEdGraphExport -graphs output."), *NodeIdString);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
        NodeGuids.Add(NodeGuid);
    }

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("No -assets specified."));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("DeleteBlueprintNode: %d node id(s) over %d asset(s) %s"),
        NodeGuids.Num(), AssetPaths.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<FGuid> Matched;
    int32 TotalDeleted = 0;
    int32 BlueprintsChanged = 0;
    for (const FString& AssetPath : AssetPaths)
    {
        const int32 Deleted = DeleteBlueprintNodes(AssetPath, NodeGuids, bApply, Matched);
        if (Deleted > 0)
        {
            ++BlueprintsChanged;
            TotalDeleted += Deleted;
        }
    }

    TArray<FString> Unmatched;
    for (const FGuid& NodeGuid : NodeGuids)
    {
        if (!Matched.Contains(NodeGuid))
        {
            Unmatched.Add(NodeGuid.ToString());
        }
    }

    if (!Unmatched.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("%d node id(s) matched nothing: %s"), Unmatched.Num(), *FString::Join(Unmatched, TEXT(", ")));
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Deleted %d node(s) across %d/%d blueprint(s) %s"),
        TotalDeleted, BlueprintsChanged, AssetPaths.Num(), bApply ? TEXT("(saved)") : TEXT("(dry run, not saved)"));
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

int32 UDeleteBlueprintNodeCommandlet::DeleteBlueprintNodes(const FString& AssetPath, const TSet<FGuid>& NodeGuids, bool bApply, TSet<FGuid>& OutMatched) const
{
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load Blueprint: %s"), *AssetPath);
        return 0;
    }

    TArray<UEdGraph*> Graphs;
    Graphs.Append(Blueprint->UbergraphPages);
    Graphs.Append(Blueprint->FunctionGraphs);

    // Collect first, removal mutates the node list.
    TArray<UEdGraphNode*> Doomed;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || !NodeGuids.Contains(Node->NodeGuid))
            {
                continue;
            }

            OutMatched.Add(Node->NodeGuid);

            if (!Node->CanUserDeleteNode())
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("  %s: '%s' (%s) is not user-deletable, skipped."),
                    *Blueprint->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Node->NodeGuid.ToString());
                continue;
            }

            Doomed.Add(Node);
        }
    }

    if (Doomed.IsEmpty())
    {
        return 0;
    }

    for (UEdGraphNode* Node : Doomed)
    {
        int32 LinkCount = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            LinkCount += Pin ? Pin->LinkedTo.Num() : 0;
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s: '%s' (%s) %d link(s) cut"),
            *Blueprint->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Node->NodeGuid.ToString(), LinkCount);

        if (bApply)
        {
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
        }
    }

    const int32 Deleted = Doomed.Num();

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d node(s) staged (dry run, not compiled/saved)."), *Blueprint->GetName(), Deleted);
        return Deleted;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const bool bSaved = UAssetWorkbench::CompileAndSavePackage(Blueprint);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d node(s) %s"), *Blueprint->GetName(), Deleted, bSaved ? TEXT("compiled + SAVED") : TEXT("compiled, SAVE FAILED"));

    return Deleted;
}
