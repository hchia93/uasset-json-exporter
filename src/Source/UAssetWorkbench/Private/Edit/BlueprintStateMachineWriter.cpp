#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "AlphaBlend.h"
#include "AnimationStateMachineGraph.h"
#include "Animation/AnimTypes.h"
#include "Animation/BlendProfile.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "UObject/UnrealType.h"

namespace
{
    UAnimStateEntryNode* FindEntry(const UAnimationStateMachineGraph* MachineGraph)
    {
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            if (UAnimStateEntryNode* Entry = Cast<UAnimStateEntryNode>(Node))
            {
                return Entry;
            }
        }

        return nullptr;
    }

    UAnimStateNodeBase* FindEntryTarget(const UAnimationStateMachineGraph* MachineGraph)
    {
        const UAnimStateEntryNode* Entry = FindEntry(MachineGraph);
        const UEdGraphPin* EntryPin = Entry ? Entry->GetOutputPin() : nullptr;
        if (!EntryPin || EntryPin->LinkedTo.Num() == 0 || !EntryPin->LinkedTo[0])
        {
            return nullptr;
        }

        return Cast<UAnimStateNodeBase>(EntryPin->LinkedTo[0]->GetOwningNode());
    }

    const TCHAR* DescribeStateKind(const UAnimStateNodeBase* State)
    {
        if (State->IsA<UAnimStateConduitNode>())
        {
            return TEXT("conduit");
        }

        if (State->IsA<UAnimStateAliasNode>())
        {
            return TEXT("alias");
        }

        return TEXT("state");
    }

    void CollectAttachedTransitions(const UAnimStateNodeBase* State, TArray<UAnimStateTransitionNode*>& OutTransitions)
    {
        for (UEdGraphNode* Node : State->GetGraph()->Nodes)
        {
            UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
            if (!Transition)
            {
                continue;
            }

            if (Transition->GetPreviousState() == State || Transition->GetNextState() == State)
            {
                OutTransitions.AddUnique(Transition);
            }
        }
    }

    FString DescribeEnumerators(const UEnum* Enum)
    {
        TArray<FString> Names;
        for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
        {
            Names.Add(Enum->GetNameStringByIndex(Index));
        }

        return FString::Join(Names, TEXT(", "));
    }

    // Takes the enumerator name the exporter prints. Prefix lets a namespaced enum also answer to the bare
    // word, so LogicType reads as Inertialization instead of TLT_Inertialization.
    bool FindEnumerator(const UEnum* Enum, const FString& Value, const TCHAR* Prefix, int64& OutValue)
    {
        for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
        {
            const FString Name = Enum->GetNameStringByIndex(Index);
            const bool bPrefixed = Prefix && Name == FString(Prefix) + Value;
            if (Name == Value || bPrefixed)
            {
                OutValue = Enum->GetValueByIndex(Index);
                return true;
            }
        }

        return false;
    }

    // "None" empties the slot, any other value has to load or the op is a spec error.
    template<typename TAsset>
    bool ResolveOptionalAsset(const FString& Path, TAsset*& OutAsset)
    {
        if (Path.IsEmpty() || Path == TEXT("None"))
        {
            OutAsset = nullptr;
            return true;
        }

        OutAsset = LoadObject<TAsset>(nullptr, *Path);
        return OutAsset != nullptr;
    }

    UAnimStateTransitionNode* FindSharedGroupMember(const UBlueprint* Blueprint, const FString& ShareName, bool bRules, const UAnimStateTransitionNode* Exclude)
    {
        TArray<UAnimStateNodeBase*> StateNodes;
        FBlueprintEditorUtils::GetAllNodesOfClassEx<UAnimStateNodeBase>(Blueprint, StateNodes);

        for (UAnimStateNodeBase* StateNode : StateNodes)
        {
            UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(StateNode);
            if (!Transition || Transition == Exclude)
            {
                continue;
            }

            const FString& Existing = bRules ? Transition->SharedRulesName : Transition->SharedCrossfadeName;
            if (Existing == ShareName)
            {
                return Transition;
            }
        }

        return nullptr;
    }

    // Mirrors FAnimTransitionNodeDetails::AssignUniqueColorsToAllSharedNodes. The badge colour is stored on
    // the node and no editor is around to recompute it.
    void AssignSharedRuleColors(const UEdGraph* MachineGraph)
    {
        TArray<UEdGraph*> Sources;
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
            if (!Transition || !Transition->bSharedRules)
            {
                continue;
            }

            const int32 ColorIndex = Sources.AddUnique(Transition->BoundGraph) + 1;
            const float Red = (ColorIndex & 1) ? 1.0f : 0.15f;
            const float Green = (ColorIndex & 2) ? 1.0f : 0.15f;
            const float Blue = (ColorIndex & 4) ? 1.0f : 0.15f;
            Transition->SharedColor = FLinearColor(Red, Green, Blue, 1.0f);
        }
    }

    void PlaceStateMachineNode(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        double PosX = 0.0;
        double PosY = 0.0;
        Desc->TryGetNumberField(TEXT("PosX"), PosX);
        Desc->TryGetNumberField(TEXT("PosY"), PosY);
        Node->NodePosX = static_cast<int32>(PosX);
        Node->NodePosY = static_cast<int32>(PosY);
    }

    // Placed the way the editor places one. The machine node and each state build their own sub graph
    // inside PostPlacedNewNode, so a sub graph is never assembled here by hand.
    void FinalizeStateMachineNode(UEdGraph* Graph, UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        PlaceStateMachineNode(Node, Desc);
        // State and transition nodes are not K2Nodes, the editor never heals this flag on them
        Node->SetFlags(RF_Transactional);
        Graph->AddNode(Node, /* bFromUI */ false, /* bSelectNewNode */ false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
    }

    class FBlueprintStateMachineWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("StateMachines");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: StateMachines must be an array of ops"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: StateMachines entry needs an Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;
            }

            return true;
        }

    private:
        bool ApplyOp(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Op)
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Desc);
            }

            if (Op == TEXT("AddState"))
            {
                return ApplyAddState(Context, Desc);
            }

            if (Op == TEXT("AddConduit"))
            {
                return ApplyAddConduit(Context, Desc);
            }

            if (Op == TEXT("AddAlias"))
            {
                return ApplyAddAlias(Context, Desc);
            }

            if (Op == TEXT("AddTransition"))
            {
                return ApplyAddTransition(Context, Desc);
            }

            if (Op == TEXT("ModifyState"))
            {
                return ApplyModifyState(Context, Desc);
            }

            if (Op == TEXT("ModifyTransition"))
            {
                return ApplyModifyTransition(Context, Desc);
            }

            if (Op == TEXT("RenameState"))
            {
                return ApplyRenameState(Context, Desc);
            }

            if (Op == TEXT("RemoveState"))
            {
                return ApplyRemoveState(Context, Desc);
            }

            if (Op == TEXT("RemoveTransition"))
            {
                return ApplyRemoveTransition(Context, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown state machine op. Expected Add, AddState, AddConduit, AddAlias, AddTransition, ModifyState, ModifyTransition, RenameState, RemoveState or RemoveTransition"), *Context.AssetPath);
            return false;
        }

        // Registers the node under the spec Id so later writers address it the way Graph does.
        void Register(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, UEdGraphNode* Node) const
        {
            FString Id;
            if (Desc->TryGetStringField(TEXT("Id"), Id))
            {
                Context.NodesById.Add(Id, Node);
            }
        }

        // Machines this run built win over the loaded ones, a spec that just named a machine means that one.
        UAnimationStateMachineGraph* ResolveMachine(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const TCHAR* Label)
        {
            FString MachineName;
            if (!Desc->TryGetStringField(TEXT("Machine"), MachineName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s needs a Machine"), *Context.AssetPath, Label);
                return nullptr;
            }

            if (UAnimationStateMachineGraph** Staged = m_NewMachines.Find(MachineName))
            {
                return *Staged;
            }

            return BlueprintEdit::FindMachine(Context, MachineName);
        }

        UAnimStateNodeBase* ResolveState(const FBlueprintEditContext& Context, const UAnimationStateMachineGraph* MachineGraph, const FString& StateName)
        {
            if (UAnimStateNodeBase** Staged = m_NewStates.Find(MakeStateKey(MachineGraph, StateName)))
            {
                return *Staged;
            }

            UAnimStateNodeBase* State = BlueprintEdit::FindState(MachineGraph, StateName);
            if (!State)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s has no state named '%s'. Present: %s"), *Context.AssetPath, *MachineGraph->GetName(), *StateName, *BlueprintEdit::DescribeStates(MachineGraph));
            }

            return State;
        }

        static FString MakeStateKey(const UEdGraph* MachineGraph, const FString& StateName)
        {
            return MachineGraph->GetName() + TEXT("|") + StateName;
        }

        bool ApplyAdd(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs a Name"), *Context.AssetPath);
                return false;
            }

            FString GraphName = TEXT("AnimGraph");
            Desc->TryGetStringField(TEXT("Graph"), GraphName);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + state machine %s in %s"), *Context.Blueprint->GetName(), *Name, *GraphName);

            UEdGraph* HostGraph = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
            if (!HostGraph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named %s"), *Context.AssetPath, *GraphName);
                return false;
            }

            UAnimGraphNode_StateMachine* MachineNode = NewObject<UAnimGraphNode_StateMachine>(HostGraph);
            FinalizeStateMachineNode(HostGraph, MachineNode, Desc);

            if (!MachineNode->EditorStateMachineGraph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: state machine %s came up without its graph"), *Context.AssetPath, *Name);
                return false;
            }

            FBlueprintEditorUtils::RenameGraph(MachineNode->EditorStateMachineGraph, *Name);
            m_NewMachines.Add(Name, MachineNode->EditorStateMachineGraph);
            Register(Context, Desc, MachineNode);
            return true;
        }

        bool ApplyAddState(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddState needs a Name"), *Context.AssetPath);
                return false;
            }

            bool bEntry = false;
            Desc->TryGetBoolField(TEXT("Entry"), bEntry);

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("AddState"));
            if (!MachineGraph)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + state %s in %s"), *Context.Blueprint->GetName(), *Name, *MachineGraph->GetName());

            UAnimStateNode* StateNode = NewObject<UAnimStateNode>(MachineGraph);
            FinalizeStateMachineNode(MachineGraph, StateNode, Desc);

            if (UEdGraph* BoundGraph = StateNode->GetBoundGraph())
            {
                FBlueprintEditorUtils::RenameGraph(BoundGraph, *Name);
            }

            if (bEntry && !ConnectEntry(Context, MachineGraph, StateNode))
            {
                return false;
            }

            m_NewStates.Add(MakeStateKey(MachineGraph, Name), StateNode);
            Register(Context, Desc, StateNode);
            return true;
        }

        bool ConnectEntry(const FBlueprintEditContext& Context, UAnimationStateMachineGraph* MachineGraph, UAnimStateNode* StateNode) const
        {
            UAnimStateEntryNode* Entry = FindEntry(MachineGraph);
            UEdGraphPin* EntryPin = Entry ? Entry->GetOutputPin() : nullptr;
            UEdGraphPin* StatePin = StateNode->GetInputPin();
            if (!EntryPin || !StatePin)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: state machine has no entry node to wire a state to"), *Context.AssetPath);
                return false;
            }

            // Entry only ever points at one state, so a later Entry state replaces the earlier one.
            EntryPin->BreakAllPinLinks();
            EntryPin->MakeLinkTo(StatePin);
            return true;
        }

        bool ApplyAddConduit(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddConduit needs a Name"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("AddConduit"));
            if (!MachineGraph)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + conduit %s in %s"), *Context.Blueprint->GetName(), *Name, *MachineGraph->GetName());

            UAnimStateConduitNode* ConduitNode = NewObject<UAnimStateConduitNode>(MachineGraph);
            FinalizeStateMachineNode(MachineGraph, ConduitNode, Desc);

            if (UEdGraph* BoundGraph = ConduitNode->GetBoundGraph())
            {
                FBlueprintEditorUtils::RenameGraph(BoundGraph, *Name);
            }

            m_NewStates.Add(MakeStateKey(MachineGraph, ConduitNode->GetStateName()), ConduitNode);
            Register(Context, Desc, ConduitNode);
            return true;
        }

        bool ApplyAddAlias(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddAlias needs a Name"), *Context.AssetPath);
                return false;
            }

            bool bGlobal = false;
            Desc->TryGetBoolField(TEXT("Global"), bGlobal);

            const TArray<TSharedPtr<FJsonValue>>* StateNames = nullptr;
            const bool bHasStates = Desc->TryGetArrayField(TEXT("States"), StateNames);
            if (bGlobal == bHasStates)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddAlias takes either States or Global, not both and not neither"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("AddAlias"));
            if (!MachineGraph)
            {
                return false;
            }

            // Resolved before the node exists so a mistyped state leaves nothing half built behind.
            TArray<UAnimStateNodeBase*> Aliased;
            if (bHasStates && !ResolveAliasedStates(Context, MachineGraph, *StateNames, Aliased))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + alias %s in %s"), *Context.Blueprint->GetName(), *Name, *MachineGraph->GetName());

            UAnimStateAliasNode* AliasNode = NewObject<UAnimStateAliasNode>(MachineGraph);

            // An alias keeps its name on itself instead of a bound graph, and PostPlacedNewNode inside
            // Finalize is what runs it past the other names in the machine.
            AliasNode->StateAliasName = Name;
            FinalizeStateMachineNode(MachineGraph, AliasNode, Desc);

            AliasNode->bGlobalAlias = bGlobal;
            for (UAnimStateNodeBase* Target : Aliased)
            {
                AliasNode->GetAliasedStates().Add(Target);
            }

            m_NewStates.Add(MakeStateKey(MachineGraph, AliasNode->GetStateName()), AliasNode);
            Register(Context, Desc, AliasNode);
            return true;
        }

        bool ResolveAliasedStates(const FBlueprintEditContext& Context, const UAnimationStateMachineGraph* MachineGraph, const TArray<TSharedPtr<FJsonValue>>& Items, TArray<UAnimStateNodeBase*>& OutStates)
        {
            for (const TSharedPtr<FJsonValue>& Value : Items)
            {
                FString StateName;
                if (!Value->TryGetString(StateName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddAlias States takes state names"), *Context.AssetPath);
                    return false;
                }

                UAnimStateNodeBase* State = ResolveState(Context, MachineGraph, StateName);
                if (!State)
                {
                    return false;
                }

                OutStates.AddUnique(State);
            }

            return true;
        }

        bool ApplyAddTransition(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString FromName;
            FString ToName;
            if (!Desc->TryGetStringField(TEXT("From"), FromName) || !Desc->TryGetStringField(TEXT("To"), ToName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddTransition needs From and To"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("AddTransition"));
            if (!MachineGraph)
            {
                return false;
            }

            UAnimStateNodeBase* FromState = ResolveState(Context, MachineGraph, FromName);
            UAnimStateNodeBase* ToState = ResolveState(Context, MachineGraph, ToName);
            if (!FromState || !ToState)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + transition %s to %s in %s"), *Context.Blueprint->GetName(), *FromName, *ToName, *MachineGraph->GetName());

            UAnimStateTransitionNode* TransitionNode = NewObject<UAnimStateTransitionNode>(MachineGraph);
            FinalizeStateMachineNode(MachineGraph, TransitionNode, Desc);
            TransitionNode->CreateConnections(FromState, ToState);

            // Graph addresses a rule graph by Name later and the engine calls every one of them Transition,
            // so stamp the pair it belongs to.
            if (TransitionNode->BoundGraph)
            {
                const FName RuleGraphName = FBlueprintEditorUtils::FindUniqueKismetName(Context.Blueprint, FString::Printf(TEXT("%s_to_%s"), *FromName, *ToName));
                FBlueprintEditorUtils::RenameGraph(TransitionNode->BoundGraph, RuleGraphName.ToString());
            }

            if (!ApplyTransitionKeys(Context, TransitionNode, Desc))
            {
                return false;
            }

            Register(Context, Desc, TransitionNode);
            return true;
        }

        bool ApplyModifyTransition(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString FromName;
            FString ToName;
            if (!Desc->TryGetStringField(TEXT("From"), FromName) || !Desc->TryGetStringField(TEXT("To"), ToName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ModifyTransition needs From and To"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("ModifyTransition"));
            if (!MachineGraph)
            {
                return false;
            }

            if (!ResolveState(Context, MachineGraph, FromName) || !ResolveState(Context, MachineGraph, ToName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: ~ transition %s to %s in %s"), *Context.Blueprint->GetName(), *FromName, *ToName, *MachineGraph->GetName());

            UAnimStateTransitionNode* TransitionNode = BlueprintEdit::ResolveTransition(Context, MachineGraph, FromName, ToName, Desc);
            if (!TransitionNode || !ApplyTransitionKeys(Context, TransitionNode, Desc))
            {
                return false;
            }

            Register(Context, Desc, TransitionNode);
            return true;
        }

        bool ApplyRemoveTransition(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString FromName;
            FString ToName;
            if (!Desc->TryGetStringField(TEXT("From"), FromName) || !Desc->TryGetStringField(TEXT("To"), ToName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: RemoveTransition needs From and To"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("RemoveTransition"));
            if (!MachineGraph)
            {
                return false;
            }

            if (!ResolveState(Context, MachineGraph, FromName) || !ResolveState(Context, MachineGraph, ToName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: - transition %s to %s in %s"), *Context.Blueprint->GetName(), *FromName, *ToName, *MachineGraph->GetName());

            UAnimStateTransitionNode* TransitionNode = BlueprintEdit::ResolveTransition(Context, MachineGraph, FromName, ToName, Desc);
            if (!TransitionNode)
            {
                return false;
            }

            // DestroyNode keeps a rule graph the rest of a shared group still points at, so one member can go
            // without taking the group's rules with it.
            FBlueprintEditorUtils::RemoveNode(Context.Blueprint, TransitionNode, /* bDontRecompile */ true);
            return true;
        }

        bool ApplyModifyState(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ModifyState needs a Name"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("ModifyState"));
            if (!MachineGraph)
            {
                return false;
            }

            UAnimStateNodeBase* State = ResolveState(Context, MachineGraph, Name);
            if (!State)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: ~ state %s in %s"), *Context.Blueprint->GetName(), *Name, *MachineGraph->GetName());

            if (!ApplyStateKeys(Context, State, Desc))
            {
                return false;
            }

            Register(Context, Desc, State);
            return true;
        }

        bool ApplyRenameState(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            FString NewName;
            if (!Desc->TryGetStringField(TEXT("Name"), Name) || !Desc->TryGetStringField(TEXT("NewName"), NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: RenameState needs Name and NewName"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("RenameState"));
            if (!MachineGraph)
            {
                return false;
            }

            UAnimStateNodeBase* State = ResolveState(Context, MachineGraph, Name);
            if (!State)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: rename state %s to %s in %s"), *Context.Blueprint->GetName(), *Name, *NewName, *MachineGraph->GetName());

            // The node's own validator, which also refuses the anim layer names a state may not shadow. The
            // rename box silently uniquifies, a spec says the name it wants and hears about a clash instead.
            const TSharedPtr<INameValidatorInterface> Validator = State->MakeNameValidator();
            const EValidatorResult Verdict = Validator->IsValid(NewName, /* bOriginal */ false);
            if (Verdict != EValidatorResult::Ok && Verdict != EValidatorResult::ExistingName)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot rename '%s' to '%s'. %s Present: %s"), *Context.AssetPath, *Name, *NewName, *INameValidatorInterface::GetErrorString(NewName, Verdict), *BlueprintEdit::DescribeStates(MachineGraph));
                return false;
            }

            State->Modify();
            State->OnRenameNode(NewName);

            m_NewStates.Remove(MakeStateKey(MachineGraph, Name));
            m_NewStates.Add(MakeStateKey(MachineGraph, State->GetStateName()), State);
            Register(Context, Desc, State);
            return true;
        }

        bool ApplyRemoveState(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: RemoveState needs a Name"), *Context.AssetPath);
                return false;
            }

            UAnimationStateMachineGraph* MachineGraph = ResolveMachine(Context, Desc, TEXT("RemoveState"));
            if (!MachineGraph)
            {
                return false;
            }

            UAnimStateNodeBase* State = ResolveState(Context, MachineGraph, Name);
            if (!State)
            {
                return false;
            }

            bool bForce = false;
            Desc->TryGetBoolField(TEXT("Force"), bForce);

            if (FindEntryTarget(MachineGraph) == State)
            {
                if (!bForce)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is the entry state of %s. Point the entry elsewhere first, or pass Force to leave the machine without one"), *Context.AssetPath, *Name, *MachineGraph->GetName());
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s: %s is left with no entry state"), *Context.AssetPath, *MachineGraph->GetName());
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: - state %s in %s"), *Context.Blueprint->GetName(), *Name, *MachineGraph->GetName());

            // A transition survives losing an endpoint and the compiler reaches transitions through states, so
            // the leftover is inert rather than reported. Take them out with the state.
            TArray<UAnimStateTransitionNode*> Attached;
            CollectAttachedTransitions(State, Attached);
            for (UAnimStateTransitionNode* Transition : Attached)
            {
                FBlueprintEditorUtils::RemoveNode(Context.Blueprint, Transition, /* bDontRecompile */ true);
            }

            FBlueprintEditorUtils::RemoveNode(Context.Blueprint, State, /* bDontRecompile */ true);
            m_NewStates.Remove(MakeStateKey(MachineGraph, Name));
            return true;
        }

        // Only keys the spec carries are touched, key names mirror what AnimBlueprintExport prints under
        // States[].
        bool ApplyStateKeys(const FBlueprintEditContext& Context, UAnimStateNodeBase* State, const TSharedPtr<FJsonObject>& Desc) const
        {
            State->Modify();

            FString StateTypeName;
            if (Desc->TryGetStringField(TEXT("StateType"), StateTypeName) && !ApplyStateType(Context, State, StateTypeName))
            {
                return false;
            }

            bool bAlwaysReset = false;
            if (Desc->TryGetBoolField(TEXT("bAlwaysResetOnEntry"), bAlwaysReset))
            {
                UAnimStateNode* StateNode = Cast<UAnimStateNode>(State);
                if (!StateNode)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: bAlwaysResetOnEntry belongs to a state, '%s' is a %s"), *Context.AssetPath, *State->GetStateName(), DescribeStateKind(State));
                    return false;
                }

                StateNode->bAlwaysResetOnEntry = bAlwaysReset;
            }

            return ApplyNotifyBindings(Context, State, Desc) && ApplyFunctionBindings(Context, State, Desc);
        }

        // A state builds its contents when it is placed and the editor offers no way to turn one kind into
        // another, so a StateType that disagrees with what is there is a spec error, not a conversion.
        bool ApplyStateType(const FBlueprintEditContext& Context, UAnimStateNodeBase* State, const FString& StateTypeName) const
        {
            const UEnum* TypeEnum = StaticEnum<EAnimStateType>();
            int64 TypeValue = 0;
            if (!FindEnumerator(TypeEnum, StateTypeName, TEXT("AST_"), TypeValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no StateType named '%s'. Enumerators: %s"), *Context.AssetPath, *StateTypeName, *DescribeEnumerators(TypeEnum));
                return false;
            }

            const UAnimStateNode* StateNode = Cast<UAnimStateNode>(State);
            if (!StateNode || StateNode->StateType.GetValue() != static_cast<EAnimStateType>(TypeValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a %s and cannot become %s, a state's type is fixed once it exists"), *Context.AssetPath, *State->GetStateName(), DescribeStateKind(State), *StateTypeName);
                return false;
            }

            return true;
        }

        // The state Details panel edits <Event>.NotifyName and nothing else, a name-only custom notify is the
        // whole binding. Walking the class keeps the accepted keys the ones the exporter prints.
        bool ApplyNotifyBindings(const FBlueprintEditContext& Context, UAnimStateNodeBase* State, const TSharedPtr<FJsonObject>& Desc) const
        {
            UAnimStateNode* StateNode = Cast<UAnimStateNode>(State);

            for (TFieldIterator<FStructProperty> It(UAnimStateNode::StaticClass()); It; ++It)
            {
                FStructProperty* Property = *It;
                if (Property->Struct != FAnimNotifyEvent::StaticStruct())
                {
                    continue;
                }

                const TSharedPtr<FJsonValue> Value = Desc->TryGetField(Property->GetName());
                if (!Value.IsValid())
                {
                    continue;
                }

                if (!StateNode)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s belongs to a state, '%s' is a %s"), *Context.AssetPath, *Property->GetName(), *State->GetStateName(), DescribeStateKind(State));
                    return false;
                }

                FName NotifyName;
                if (!ReadBindingName(Context, Value, TEXT("Name"), Property->GetName(), NotifyName))
                {
                    return false;
                }

                Property->ContainerPtrToValuePtr<FAnimNotifyEvent>(StateNode)->NotifyName = NotifyName;
            }

            return true;
        }

        // A state's function hooks sit on the result node inside its bound graph, which is where the Details
        // panel pulls them in from as well.
        bool ApplyFunctionBindings(const FBlueprintEditContext& Context, UAnimStateNodeBase* State, const TSharedPtr<FJsonObject>& Desc) const
        {
            UAnimGraphNode_StateResult* ResultNode = nullptr;

            for (TFieldIterator<FStructProperty> It(UAnimGraphNode_StateResult::StaticClass()); It; ++It)
            {
                FStructProperty* Property = *It;
                if (Property->Struct != FMemberReference::StaticStruct())
                {
                    continue;
                }

                const TSharedPtr<FJsonValue> Value = Desc->TryGetField(Property->GetName());
                if (!Value.IsValid())
                {
                    continue;
                }

                if (!ResultNode && !ResolveResultNode(Context, State, Property->GetName(), ResultNode))
                {
                    return false;
                }

                FName FunctionName;
                if (!ReadBindingName(Context, Value, TEXT("Function"), Property->GetName(), FunctionName))
                {
                    return false;
                }

                FMemberReference* Reference = Property->ContainerPtrToValuePtr<FMemberReference>(ResultNode);
                *Reference = FMemberReference();
                if (!FunctionName.IsNone())
                {
                    Reference->SetSelfMember(FunctionName);
                }
            }

            return true;
        }

        bool ResolveResultNode(const FBlueprintEditContext& Context, UAnimStateNodeBase* State, const FString& Label, UAnimGraphNode_StateResult*& OutResultNode) const
        {
            UAnimStateNode* StateNode = Cast<UAnimStateNode>(State);
            OutResultNode = StateNode ? StateNode->GetResultNodeInsideState() : nullptr;
            if (!OutResultNode)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s binds on a state result node, '%s' is a %s and has none"), *Context.AssetPath, *Label, *State->GetStateName(), DescribeStateKind(State));
                return false;
            }

            OutResultNode->Modify();
            return true;
        }

        // { "<Key>": "X" } sets the binding, "None" clears it, same shape SharedRules takes.
        bool ReadBindingName(const FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Value, const TCHAR* Key, const FString& Label, FName& OutName) const
        {
            FString Literal;
            if (Value->TryGetString(Literal))
            {
                if (Literal != TEXT("None"))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s takes { \"%s\": ... } or \"None\", got '%s'"), *Context.AssetPath, *Label, Key, *Literal);
                    return false;
                }

                OutName = NAME_None;
                return true;
            }

            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Inner;
            if (!Value->TryGetObject(Object) || !(*Object)->TryGetStringField(Key, Inner))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s takes { \"%s\": ... } or \"None\""), *Context.AssetPath, *Label, Key);
                return false;
            }

            OutName = FName(*Inner);
            return true;
        }

        // Shared with AddTransition so one op can create and fully configure a transition. Only keys the spec
        // carries are touched, key names mirror what AnimBlueprintExport prints.
        bool ApplyTransitionKeys(const FBlueprintEditContext& Context, UAnimStateTransitionNode* Node, const TSharedPtr<FJsonObject>& Desc) const
        {
            Node->Modify();

            double Crossfade = 0.0;
            if (Desc->TryGetNumberField(TEXT("Blend"), Crossfade) || Desc->TryGetNumberField(TEXT("CrossfadeDuration"), Crossfade))
            {
                Node->CrossfadeDuration = static_cast<float>(Crossfade);
            }

            FString BlendModeName;
            if (Desc->TryGetStringField(TEXT("BlendMode"), BlendModeName))
            {
                const UEnum* BlendModeEnum = StaticEnum<EAlphaBlendOption>();
                int64 BlendModeValue = 0;
                if (!FindEnumerator(BlendModeEnum, BlendModeName, nullptr, BlendModeValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no BlendMode named '%s'. Enumerators: %s"), *Context.AssetPath, *BlendModeName, *DescribeEnumerators(BlendModeEnum));
                    return false;
                }

                Node->BlendMode = static_cast<EAlphaBlendOption>(BlendModeValue);
            }

            FString CurvePath;
            if (Desc->TryGetStringField(TEXT("CustomBlendCurve"), CurvePath))
            {
                UCurveFloat* Curve = nullptr;
                if (!ResolveOptionalAsset(CurvePath, Curve))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: CustomBlendCurve '%s' did not load"), *Context.AssetPath, *CurvePath);
                    return false;
                }

                Node->CustomBlendCurve = Curve;
            }

            FString ProfilePath;
            if (Desc->TryGetStringField(TEXT("BlendProfile"), ProfilePath))
            {
                UBlendProfile* Profile = nullptr;
                if (!ResolveOptionalAsset(ProfilePath, Profile))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: BlendProfile '%s' did not load"), *Context.AssetPath, *ProfilePath);
                    return false;
                }

                Node->BlendProfileWrapper.SetSkeletonBlendProfile(Profile);
            }

            int32 Priority = 0;
            if (Desc->TryGetNumberField(TEXT("PriorityOrder"), Priority))
            {
                Node->PriorityOrder = Priority;
            }

            bool bBidirectional = false;
            if (Desc->TryGetBoolField(TEXT("Bidirectional"), bBidirectional))
            {
                Node->Bidirectional = bBidirectional;
            }

            bool bAutomaticRule = false;
            if (Desc->TryGetBoolField(TEXT("bAutomaticRuleBasedOnSequencePlayerInState"), bAutomaticRule))
            {
                Node->bAutomaticRuleBasedOnSequencePlayerInState = bAutomaticRule;
            }

            double TriggerTime = 0.0;
            if (Desc->TryGetNumberField(TEXT("AutomaticRuleTriggerTime"), TriggerTime))
            {
                Node->AutomaticRuleTriggerTime = static_cast<float>(TriggerTime);
            }

            double ReentryTime = 0.0;
            if (Desc->TryGetNumberField(TEXT("MinTimeBeforeReentry"), ReentryTime))
            {
                Node->MinTimeBeforeReentry = static_cast<float>(ReentryTime);
            }

            bool bSelfInertialization = false;
            if (Desc->TryGetBoolField(TEXT("bAllowInertializationForSelfTransitions"), bSelfInertialization))
            {
                Node->bAllowInertializationForSelfTransitions = bSelfInertialization;
            }

            FString SyncGroupName;
            if (Desc->TryGetStringField(TEXT("SyncGroupNameToRequireValidMarkersRule"), SyncGroupName))
            {
                Node->SyncGroupNameToRequireValidMarkersRule = FName(*SyncGroupName);
            }

            bool bDisabled = false;
            if (Desc->TryGetBoolField(TEXT("bDisabled"), bDisabled))
            {
                Node->bDisabled = bDisabled;
            }

            FString LogicTypeName;
            if (Desc->TryGetStringField(TEXT("LogicType"), LogicTypeName) && !ApplyLogicType(Context, Node, LogicTypeName))
            {
                return false;
            }

            const TSharedPtr<FJsonValue> SharedRules = Desc->TryGetField(TEXT("SharedRules"));
            if (SharedRules.IsValid() && !ApplyShared(Context, Node, SharedRules, /* bRules */ true))
            {
                return false;
            }

            const TSharedPtr<FJsonValue> SharedCrossfade = Desc->TryGetField(TEXT("SharedCrossfade"));
            if (SharedCrossfade.IsValid() && !ApplyShared(Context, Node, SharedCrossfade, /* bRules */ false))
            {
                return false;
            }

            return ApplyTransitionNotifyBindings(Context, Node, Desc);
        }

        // Same shape a state's notifications take: the Details panel edits <Event>.NotifyName and nothing else.
        // Walking the class keeps the accepted keys the ones the exporter prints under Events.
        bool ApplyTransitionNotifyBindings(const FBlueprintEditContext& Context, UAnimStateTransitionNode* Node, const TSharedPtr<FJsonObject>& Desc) const
        {
            for (TFieldIterator<FStructProperty> It(UAnimStateTransitionNode::StaticClass()); It; ++It)
            {
                FStructProperty* Property = *It;
                if (Property->Struct != FAnimNotifyEvent::StaticStruct())
                {
                    continue;
                }

                const TSharedPtr<FJsonValue> Value = Desc->TryGetField(Property->GetName());
                if (!Value.IsValid())
                {
                    continue;
                }

                FName NotifyName;
                if (!ReadBindingName(Context, Value, TEXT("Name"), Property->GetName(), NotifyName))
                {
                    return false;
                }

                Property->ContainerPtrToValuePtr<FAnimNotifyEvent>(Node)->NotifyName = NotifyName;
            }

            return true;
        }

        // The Details panel drives the custom graph off the property change, both ways. Notifying an unchanged
        // LogicType would take the engine's else branch and delete the graph it just made.
        bool ApplyLogicType(const FBlueprintEditContext& Context, UAnimStateTransitionNode* Node, const FString& LogicTypeName) const
        {
            const UEnum* LogicEnum = StaticEnum<ETransitionLogicType::Type>();
            int64 LogicValue = 0;
            if (!FindEnumerator(LogicEnum, LogicTypeName, TEXT("TLT_"), LogicValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no LogicType named '%s'. Enumerators: %s"), *Context.AssetPath, *LogicTypeName, *DescribeEnumerators(LogicEnum));
                return false;
            }

            const ETransitionLogicType::Type Resolved = static_cast<ETransitionLogicType::Type>(LogicValue);
            if (Node->LogicType == Resolved)
            {
                return true;
            }

            Node->LogicType = Resolved;

            FProperty* LogicProperty = UAnimStateTransitionNode::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UAnimStateTransitionNode, LogicType));
            FPropertyChangedEvent PropertyEvent(LogicProperty);
            Node->PostEditChangeProperty(PropertyEvent);
            return true;
        }

        // { "Name": "X" } joins the group and creates it when nobody carries the name yet, "None" leaves it.
        bool ApplyShared(const FBlueprintEditContext& Context, UAnimStateTransitionNode* Node, const TSharedPtr<FJsonValue>& Value, bool bRules) const
        {
            const TCHAR* Label = bRules ? TEXT("SharedRules") : TEXT("SharedCrossfade");

            FString Literal;
            if (Value->TryGetString(Literal))
            {
                if (Literal != TEXT("None"))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s takes { \"Name\": ... } or \"None\", got '%s'"), *Context.AssetPath, Label, *Literal);
                    return false;
                }

                if (bRules)
                {
                    Node->UnshareRules();
                }
                else
                {
                    Node->UnshareCrossade();
                }

                AssignSharedRuleColors(Node->GetGraph());
                return true;
            }

            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString ShareName;
            if (!Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("Name"), ShareName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s takes { \"Name\": ... } or \"None\""), *Context.AssetPath, Label);
                return false;
            }

            UAnimStateTransitionNode* GroupMember = FindSharedGroupMember(Context.Blueprint, ShareName, bRules, Node);
            if (bRules)
            {
                if (GroupMember)
                {
                    Node->UseSharedRules(GroupMember);
                }
                else
                {
                    Node->MakeRulesShareable(ShareName);
                }
            }
            else
            {
                if (GroupMember)
                {
                    Node->UseSharedCrossfade(GroupMember);
                }
                else
                {
                    Node->MakeCrossfadeShareable(ShareName);
                }
            }

            AssignSharedRuleColors(Node->GetGraph());
            return true;
        }

        TMap<FString, UAnimationStateMachineGraph*> m_NewMachines;
        TMap<FString, UAnimStateNodeBase*> m_NewStates;
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintStateMachineWriter()
{
    return MakeUnique<FBlueprintStateMachineWriter>();
}
