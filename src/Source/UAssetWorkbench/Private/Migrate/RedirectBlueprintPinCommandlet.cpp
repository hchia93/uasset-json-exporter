#include "Migrate/RedirectBlueprintPinCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/SavePackage.h"

URedirectBlueprintPinCommandlet::URedirectBlueprintPinCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URedirectBlueprintPinCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString OldPinStr;
    FString NewPinStr;
    FParse::Value(*Params, TEXT("OldPin="), OldPinStr);
    FParse::Value(*Params, TEXT("NewPin="), NewPinStr);
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (OldPinStr.IsEmpty() || NewPinStr.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RedirectBlueprintPin -OldPin=\"Old\" -NewPin=\"New\" -assets=\"/Game/A,/Game/B\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("No -assets specified."));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const FName OldPin(*OldPinStr);
    const FName NewPin(*NewPinStr);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RedirectBlueprintPin: '%s' -> '%s' over %d asset(s) %s"),
        *OldPinStr, *NewPinStr, AssetPaths.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    int32 TotalRedirected = 0;
    int32 BlueprintsChanged = 0;
    for (const FString& AssetPath : AssetPaths)
    {
        const int32 Redirected = RedirectBlueprintPins(AssetPath, OldPin, NewPin, bApply);
        if (Redirected > 0)
        {
            ++BlueprintsChanged;
            TotalRedirected += Redirected;
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Redirected %d node(s) across %d/%d blueprint(s) %s"),
        TotalRedirected, BlueprintsChanged, AssetPaths.Num(), bApply ? TEXT("(saved)") : TEXT("(dry run, not saved)"));
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

int32 URedirectBlueprintPinCommandlet::RedirectBlueprintPins(const FString& AssetPath, FName OldPin, FName NewPin, bool bApply) const
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

    int32 Redirected = 0;

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            // Only the stale-pin case: the node carries BOTH the old and new output pin.
            UEdGraphPin* OldPinPtr = Node->FindPin(OldPin, EGPD_Output);
            UEdGraphPin* NewPinPtr = Node->FindPin(NewPin, EGPD_Output);
            if (!OldPinPtr || !NewPinPtr)
            {
                continue;
            }

            const int32 LinkCount = OldPinPtr->LinkedTo.Num();
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s: node '%s' has stale pin '%s' (%d link(s)) + new pin '%s'"),
                *Blueprint->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *OldPin.ToString(), LinkCount, *NewPin.ToString());

            if (bApply)
            {
                const UEdGraphSchema* Schema = Graph->GetSchema();
                Schema->MovePinLinks(*OldPinPtr, *NewPinPtr);
                Node->ReconstructNode();
            }
            ++Redirected;
        }
    }

    if (Redirected == 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("%s: no node carrying both '%s' and '%s' output pins."), *Blueprint->GetName(), *OldPin.ToString(), *NewPin.ToString());
        return 0;
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d node(s) staged (dry run, not compiled/saved)."), *Blueprint->GetName(), Redirected);
        return Redirected;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const bool bSaved = UAssetWorkbench::CompileAndSavePackage(Blueprint);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d node(s) %s"), *Blueprint->GetName(), Redirected, bSaved ? TEXT("compiled + SAVED") : TEXT("compiled, SAVE FAILED"));

    return Redirected;
}
