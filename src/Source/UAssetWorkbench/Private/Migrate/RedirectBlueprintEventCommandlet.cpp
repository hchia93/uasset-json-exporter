#include "Migrate/RedirectBlueprintEventCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/SavePackage.h"

URedirectBlueprintEventCommandlet::URedirectBlueprintEventCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URedirectBlueprintEventCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString OwnerClassPath;
    FString OldEventStr;
    FString NewEventStr;
    FParse::Value(*Params, TEXT("OwnerClass="), OwnerClassPath);
    FParse::Value(*Params, TEXT("OldEvent="), OldEventStr);
    FParse::Value(*Params, TEXT("NewEvent="), NewEventStr);
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (OwnerClassPath.IsEmpty() || OldEventStr.IsEmpty() || NewEventStr.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RedirectBlueprintEvent -OwnerClass=\"/Script/Module.OwnerClass\" -OldEvent=\"Old\" -NewEvent=\"New\" -assets=\"/Game/A,/Game/B\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("No -assets specified."));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UClass* OwnerClass = FindObject<UClass>(nullptr, *OwnerClassPath);
    if (!OwnerClass)
    {
        OwnerClass = LoadObject<UClass>(nullptr, *OwnerClassPath);
    }
    if (!OwnerClass)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Owner class not found: %s"), *OwnerClassPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const FName OldEvent(*OldEventStr);
    const FName NewEvent(*NewEventStr);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RedirectBlueprintEvent: %s '%s' -> '%s' over %d asset(s) %s"),
        *OwnerClass->GetName(), *OldEventStr, *NewEventStr, AssetPaths.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    int32 TotalRedirected = 0;
    int32 BlueprintsChanged = 0;
    for (const FString& AssetPath : AssetPaths)
    {
        const int32 Redirected = RedirectBlueprintEvents(AssetPath, OwnerClass, OldEvent, NewEvent, bApply);
        if (Redirected > 0)
        {
            ++BlueprintsChanged;
            TotalRedirected += Redirected;
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Redirected %d event(s) across %d/%d blueprint(s) %s"),
        TotalRedirected, BlueprintsChanged, AssetPaths.Num(), bApply ? TEXT("(saved)") : TEXT("(dry run, not saved)"));
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

int32 URedirectBlueprintEventCommandlet::RedirectBlueprintEvents(const FString& AssetPath, UClass* OwnerClass, FName OldEvent, FName NewEvent, bool bApply) const
{
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load Blueprint: %s"), *AssetPath);
        return 0;
    }

    int32 Redirected = 0;
    const FString OldEventStr = OldEvent.ToString();

    // UE renames the orphaned override to "<OldEvent>_N" (uniquing suffix) when it demotes it to a custom event.
    auto MatchesOldName = [&OldEventStr](const FString& Name) -> bool
    {
        if (Name == OldEventStr)
        {
            return true;
        }
        if (Name.StartsWith(OldEventStr + TEXT("_")))
        {
            const FString Suffix = Name.RightChop(OldEventStr.Len() + 1);
            return !Suffix.IsEmpty() && Suffix.IsNumeric();
        }
        return false;
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph)
        {
            continue;
        }

        // Collect first, the node list is mutated below.
        // The orphaned override surfaces either as a converted custom event (CustomFunctionName)
        // or as a still-bound event node with a dangling EventReference to the old name.
        TArray<UEdGraphNode*> Orphans;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                const FString Name = CustomEvent->CustomFunctionName.ToString();
                UE_LOG(LogUAssetWorkbenchMigrator, Verbose, TEXT("    [scan] %s CustomEvent '%s'"), *Blueprint->GetName(), *Name);
                if (MatchesOldName(Name))
                {
                    Orphans.Add(Node);
                }
            }
            else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                const FString MemberName = EventNode->EventReference.GetMemberName().ToString();
                UE_LOG(LogUAssetWorkbenchMigrator, Verbose, TEXT("    [scan] %s Event '%s'"), *Blueprint->GetName(), *MemberName);
                if (MatchesOldName(MemberName))
                {
                    Orphans.Add(Node);
                }
            }
        }

        for (UEdGraphNode* Old : Orphans)
        {
            UK2Node_Event* NewNode = NewObject<UK2Node_Event>(Graph);
            NewNode->EventReference.SetExternalMember(NewEvent, OwnerClass);
            NewNode->bOverrideFunction = true;
            NewNode->NodePosX = Old->NodePosX;
            NewNode->NodePosY = Old->NodePosY;
            NewNode->SetFlags(RF_Transactional);
            Graph->AddNode(NewNode, false, false);
            NewNode->CreateNewGuid();
            NewNode->PostPlacedNewNode();
            NewNode->AllocateDefaultPins();

            const UEdGraphSchema* Schema = Graph->GetSchema();

            // Pair output pins by role: one exec, then data params by order. Skip the delegate output pin.
            UEdGraphPin* OldExec = nullptr;
            UEdGraphPin* NewExec = nullptr;
            TArray<UEdGraphPin*> OldData;
            TArray<UEdGraphPin*> NewData;

            for (UEdGraphPin* Pin : Old->Pins)
            {
                if (Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    OldExec = Pin;
                }
                else if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
                {
                    OldData.Add(Pin);
                }
            }
            for (UEdGraphPin* Pin : NewNode->Pins)
            {
                if (Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    NewExec = Pin;
                }
                else if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
                {
                    NewData.Add(Pin);
                }
            }

            int32 MovedGroups = 0;
            if (OldExec && NewExec)
            {
                Schema->MovePinLinks(*OldExec, *NewExec);
                ++MovedGroups;
            }
            const int32 DataCount = FMath::Min(OldData.Num(), NewData.Num());
            for (int32 Index = 0; Index < DataCount; ++Index)
            {
                Schema->MovePinLinks(*OldData[Index], *NewData[Index]);
                ++MovedGroups;
            }

            Graph->RemoveNode(Old);
            ++Redirected;

            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s: '%s' -> event '%s' (%d pin group(s) moved, old data pins=%d new data pins=%d)"),
                *Blueprint->GetName(), *OldEvent.ToString(), *NewEvent.ToString(), MovedGroups, OldData.Num(), NewData.Num());
        }
    }

    if (Redirected == 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("%s: no orphaned event '%s' found."), *Blueprint->GetName(), *OldEvent.ToString());
        return 0;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d redirect(s) staged (dry run, not compiled/saved)."), *Blueprint->GetName(), Redirected);
        return Redirected;
    }

    const bool bSaved = UAssetWorkbench::CompileAndSavePackage(Blueprint);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("%s: %d redirect(s) %s"), *Blueprint->GetName(), Redirected, bSaved ? TEXT("compiled + SAVED") : TEXT("compiled, SAVE FAILED"));

    return Redirected;
}
