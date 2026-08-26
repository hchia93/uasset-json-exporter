#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_EditablePinBase.h"

namespace
{
    const TCHAR* kTypeWords = TEXT("bool, byte, enum, int, int64, float, real, double, string, name, text, object, class, softobject, softclass, struct");

    bool ResolvePinCategory(const FString& Type, FName& OutCategory, FName& OutSubCategory)
    {
        OutSubCategory = NAME_None;

        static const TMap<FString, FName> Simple =
        {
            { TEXT("bool"),        UEdGraphSchema_K2::PC_Boolean },
            { TEXT("byte"),        UEdGraphSchema_K2::PC_Byte },
            { TEXT("enum"),        UEdGraphSchema_K2::PC_Byte },
            { TEXT("int"),         UEdGraphSchema_K2::PC_Int },
            { TEXT("int64"),       UEdGraphSchema_K2::PC_Int64 },
            { TEXT("string"),      UEdGraphSchema_K2::PC_String },
            { TEXT("name"),        UEdGraphSchema_K2::PC_Name },
            { TEXT("text"),        UEdGraphSchema_K2::PC_Text },
            { TEXT("object"),      UEdGraphSchema_K2::PC_Object },
            { TEXT("class"),       UEdGraphSchema_K2::PC_Class },
            { TEXT("softobject"),  UEdGraphSchema_K2::PC_SoftObject },
            { TEXT("softclass"),   UEdGraphSchema_K2::PC_SoftClass },
            { TEXT("struct"),      UEdGraphSchema_K2::PC_Struct },
        };

        if (const FName* Found = Simple.Find(Type))
        {
            OutCategory = *Found;
            return true;
        }

        // Floats are PC_Real plus a width subcategory, "float" alone would land on a wildcard width.
        if (Type == TEXT("float") || Type == TEXT("real"))
        {
            OutCategory = UEdGraphSchema_K2::PC_Real;
            OutSubCategory = UEdGraphSchema_K2::PC_Float;
            return true;
        }

        if (Type == TEXT("double"))
        {
            OutCategory = UEdGraphSchema_K2::PC_Real;
            OutSubCategory = UEdGraphSchema_K2::PC_Double;
            return true;
        }

        return false;
    }

    bool ResolveContainerType(const FString& Container, EPinContainerType& OutContainer)
    {
        if (Container.IsEmpty() || Container == TEXT("None"))
        {
            OutContainer = EPinContainerType::None;
            return true;
        }
        if (Container == TEXT("Array"))
        {
            OutContainer = EPinContainerType::Array;
            return true;
        }
        if (Container == TEXT("Set"))
        {
            OutContainer = EPinContainerType::Set;
            return true;
        }
        if (Container == TEXT("Map"))
        {
            OutContainer = EPinContainerType::Map;
            return true;
        }

        return false;
    }

    TSharedPtr<FUserPinInfo>* FindUserPin(UK2Node_EditablePinBase* Node, const FName PinName)
    {
        return Node->UserDefinedPins.FindByPredicate([PinName](const TSharedPtr<FUserPinInfo>& PinInfo)
        {
            return PinInfo.IsValid() && PinInfo->PinName == PinName;
        });
    }

    FString DescribeUserPins(UK2Node_EditablePinBase* Node)
    {
        TArray<FString> Names;
        for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
        {
            if (PinInfo.IsValid())
            {
                Names.Add(PinInfo->PinName.ToString());
            }
        }

        return FString::Join(Names, TEXT(", "));
    }

    // UserDefinedPins order is the parameter order the compiler emits and the order the editor lists.
    void SortUserPinsToSpecOrder(UK2Node_EditablePinBase* Node, const TArray<BlueprintEdit::FSignatureEntry>& Entries)
    {
        TArray<TSharedPtr<FUserPinInfo>> Ordered;
        Ordered.Reserve(Node->UserDefinedPins.Num());

        for (const BlueprintEdit::FSignatureEntry& Entry : Entries)
        {
            if (TSharedPtr<FUserPinInfo>* Found = FindUserPin(Node, Entry.Name))
            {
                Ordered.Add(*Found);
            }
        }

        for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
        {
            if (!Ordered.Contains(PinInfo))
            {
                Ordered.Add(PinInfo);
            }
        }

        Node->UserDefinedPins = MoveTemp(Ordered);
    }

    // Every machine hanging off a graph, then every machine nested inside one of its states. A transition's
    // bound graph is a rule graph and never holds a machine.
    void CollectMachineNodes(const UEdGraph* Graph, TArray<UAnimGraphNode_StateMachineBase*>& OutMachineNodes)
    {
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
            if (!MachineNode || !MachineNode->EditorStateMachineGraph)
            {
                continue;
            }

            OutMachineNodes.AddUnique(MachineNode);

            for (UEdGraphNode* Inner : MachineNode->EditorStateMachineGraph->Nodes)
            {
                if (const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Inner))
                {
                    CollectMachineNodes(State->GetBoundGraph(), OutMachineNodes);
                }
            }
        }
    }

    void CollectMachineNodes(const UBlueprint* Blueprint, TArray<UAnimGraphNode_StateMachineBase*>& OutMachineNodes)
    {
        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            CollectMachineNodes(Graph, OutMachineNodes);
        }

        for (const UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            CollectMachineNodes(Graph, OutMachineNodes);
        }
    }

    FString DescribeMachines(const TArray<UAnimationStateMachineGraph*>& Machines)
    {
        TArray<FString> Names;
        for (const UAnimationStateMachineGraph* Machine : Machines)
        {
            Names.Add(Machine->GetName());
        }

        return FString::Join(Names, TEXT(", "));
    }
}

namespace BlueprintEdit
{
    bool ResolvePinType(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdGraphPinType& OutType)
    {
        FString Type;
        if (!Desc->TryGetStringField(TEXT("Type"), Type))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: entry needs a Type. Accepted: %s"), *Context.AssetPath, kTypeWords);
            return false;
        }

        FName Category;
        FName SubCategory;
        if (!ResolvePinCategory(Type, Category, SubCategory))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Type '%s'. Accepted: %s"), *Context.AssetPath, *Type, kTypeWords);
            return false;
        }

        OutType.PinCategory = Category;
        OutType.PinSubCategory = SubCategory;

        const bool bNeedsSubObject = Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
            || Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
            || Category == UEdGraphSchema_K2::PC_Struct || Type == TEXT("enum");

        FString SubType;
        if (Desc->TryGetStringField(TEXT("SubType"), SubType))
        {
            UObject* Resolved = LoadObject<UObject>(nullptr, *SubType);
            if (!Resolved)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve SubType '%s'"), *Context.AssetPath, *SubType);
                return false;
            }
            OutType.PinSubCategoryObject = Resolved;
        }
        else if (bNeedsSubObject)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Type '%s' needs a SubType"), *Context.AssetPath, *Type);
            return false;
        }

        FString Container;
        Desc->TryGetStringField(TEXT("Container"), Container);

        EPinContainerType ContainerType = EPinContainerType::None;
        if (!ResolveContainerType(Container, ContainerType))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Container '%s'. Accepted: None, Array, Set, Map"), *Context.AssetPath, *Container);
            return false;
        }
        OutType.ContainerType = ContainerType;

        FString ValueType;
        if (Desc->TryGetStringField(TEXT("ValueType"), ValueType))
        {
            FName ValueCategory;
            FName ValueSubCategory;
            if (!ResolvePinCategory(ValueType, ValueCategory, ValueSubCategory))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown ValueType '%s'. Accepted: %s"), *Context.AssetPath, *ValueType, kTypeWords);
                return false;
            }

            OutType.PinValueType.TerminalCategory = ValueCategory;
            OutType.PinValueType.TerminalSubCategory = ValueSubCategory;

            FString ValueSubType;
            if (Desc->TryGetStringField(TEXT("ValueSubType"), ValueSubType))
            {
                UObject* ResolvedValue = LoadObject<UObject>(nullptr, *ValueSubType);
                if (!ResolvedValue)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve ValueSubType '%s'"), *Context.AssetPath, *ValueSubType);
                    return false;
                }
                OutType.PinValueType.TerminalSubCategoryObject = ResolvedValue;
            }
        }

        return true;
    }

    bool ResolvePinTypeOverrides(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdGraphPinType& InOutType, bool& bOutTouched)
    {
        const bool bHasType = Desc->HasTypedField<EJson::String>(TEXT("Type"));
        const bool bHasSubType = Desc->HasTypedField<EJson::String>(TEXT("SubType"));
        const bool bHasContainer = Desc->HasTypedField<EJson::String>(TEXT("Container"));
        const bool bHasValueType = Desc->HasTypedField<EJson::String>(TEXT("ValueType"));

        bOutTouched = bHasType || bHasSubType || bHasContainer || bHasValueType;
        if (!bOutTouched)
        {
            return true;
        }

        if (bHasType)
        {
            FEdGraphPinType Resolved;
            if (!ResolvePinType(Context, Desc, Resolved))
            {
                return false;
            }

            // ResolvePinType reads a whole type, so the keys the spec left out have to be put back.
            if (!bHasContainer)
            {
                Resolved.ContainerType = InOutType.ContainerType;
            }
            if (!bHasValueType)
            {
                Resolved.PinValueType = InOutType.PinValueType;
            }

            InOutType = Resolved;
            return true;
        }

        if (bHasSubType || bHasValueType)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: SubType and ValueType only mean something next to a Type"), *Context.AssetPath);
            return false;
        }

        FString Container;
        Desc->TryGetStringField(TEXT("Container"), Container);

        EPinContainerType ContainerType = EPinContainerType::None;
        if (!ResolveContainerType(Container, ContainerType))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Container '%s'. Accepted: None, Array, Set, Map"), *Context.AssetPath, *Container);
            return false;
        }

        InOutType.ContainerType = ContainerType;
        return true;
    }

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
    {
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        // Last resort covers the sub graphs: a state machine, and the anim graph inside each of its states.
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    // Sub graphs carry the nodes a state machine hides: its states, and the anim graph inside each state.
    // Guids are unique across the asset, so one flat map addresses every depth.
    void RegisterExistingNodes(UEdGraph* Graph, TMap<FString, UEdGraphNode*>& OutNodesById)
    {
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                OutNodesById.Add(Node->NodeGuid.ToString(EGuidFormats::Digits), Node);
            }
        }

        for (UEdGraph* SubGraph : Graph->SubGraphs)
        {
            RegisterExistingNodes(SubGraph, OutNodesById);
        }
    }

    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName)
    {
        if (!Node)
        {
            return nullptr;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName.ToString() == PinName)
            {
                return Pin;
            }
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && !Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString() == PinName)
            {
                return Pin;
            }
        }

        return nullptr;
    }

    FString DescribePins(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return TEXT("<node not found>");
        }

        TArray<FString> Names;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin)
            {
                Names.Add(Pin->PinName.ToString());
            }
        }

        return FString::Join(Names, TEXT(", "));
    }

    bool ReadSignatureEntries(const FBlueprintEditContext& Context, const TArray<TSharedPtr<FJsonValue>>& Items, const TCHAR* Label, TArray<FSignatureEntry>& OutEntries)
    {
        for (const TSharedPtr<FJsonValue>& Value : Items)
        {
            const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
            FString Name;
            if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s entry needs a Name"), *Context.AssetPath, Label);
                return false;
            }

            FSignatureEntry Entry;
            Entry.Name = FName(*Name);
            if (!ResolvePinType(Context, Desc, Entry.Type))
            {
                return false;
            }

            bool bIsReference = false;
            if (Desc->TryGetBoolField(TEXT("IsReference"), bIsReference))
            {
                Entry.Type.bIsReference = bIsReference;
            }

            Entry.bHasDefault = Desc->TryGetStringField(TEXT("Default"), Entry.Default);

            if (OutEntries.ContainsByPredicate([&Entry](const FSignatureEntry& Other) { return Other.Name == Entry.Name; }))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s lists '%s' twice"), *Context.AssetPath, Label, *Name);
                return false;
            }

            OutEntries.Add(MoveTemp(Entry));
        }

        return true;
    }

    bool ApplyPinShape(const FBlueprintEditContext& Context, UK2Node_EditablePinBase* Node, const TArray<FSignatureEntry>& Entries, EEdGraphPinDirection Direction)
    {
        Node->Modify();

        TArray<FName> Doomed;
        for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
        {
            if (!PinInfo.IsValid())
            {
                continue;
            }

            const bool bWanted = Entries.ContainsByPredicate([&PinInfo](const FSignatureEntry& Entry) { return Entry.Name == PinInfo->PinName; });
            if (!bWanted)
            {
                Doomed.Add(PinInfo->PinName);
            }
        }

        for (const FName& PinName : Doomed)
        {
            Node->RemoveUserDefinedPinByName(PinName);
        }

        for (const FSignatureEntry& Entry : Entries)
        {
            // A by-ref or container input is declared const to match the native signature, the editor
            // conforms the same way behind its checkbox.
            FEdGraphPinType PinType = Entry.Type;
            if (!PinType.bIsConst && Node->ShouldUseConstRefParams())
            {
                PinType.bIsConst = PinType.IsArray() || PinType.bIsReference;
            }

            TSharedPtr<FUserPinInfo>* Existing = FindUserPin(Node, Entry.Name);
            if (!Existing)
            {
                FText Refusal;
                if (!Node->CanCreateUserDefinedPin(PinType, Direction, Refusal))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: pin '%s' refused: %s"), *Context.AssetPath, *Entry.Name.ToString(), *Refusal.ToString());
                    return false;
                }

                if (!Node->CreateUserDefinedPin(Entry.Name, PinType, Direction, /* bUseUniqueName */ false))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: could not create pin '%s'"), *Context.AssetPath, *Entry.Name.ToString());
                    return false;
                }

                continue;
            }

            if ((*Existing)->PinType != PinType)
            {
                (*Existing)->PinType = PinType;
                // A default that parsed under the old type is unreadable under the new one.
                (*Existing)->PinDefaultValue.Reset();
            }
        }

        SortUserPinsToSpecOrder(Node, Entries);
        return true;
    }

    bool ApplyPinDefaults(const FBlueprintEditContext& Context, UK2Node_EditablePinBase* Node, const TArray<FSignatureEntry>& Entries)
    {
        for (const FSignatureEntry& Entry : Entries)
        {
            if (!Entry.bHasDefault)
            {
                continue;
            }

            TSharedPtr<FUserPinInfo>* Existing = FindUserPin(Node, Entry.Name);
            if (!Existing)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: pin '%s' is gone before its Default landed. Present: %s"), *Context.AssetPath, *Entry.Name.ToString(), *DescribeUserPins(Node));
                return false;
            }

            if (!Node->ModifyUserDefinedPinDefaultValue(*Existing, Entry.Default))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: pin '%s' rejected Default '%s'"), *Context.AssetPath, *Entry.Name.ToString(), *Entry.Default);
                return false;
            }
        }

        return true;
    }

    void ReconstructTerminator(UEdGraphNode* Node)
    {
        const bool bPreviousOrphanSaving = Node->bDisableOrphanPinSaving;
        Node->bDisableOrphanPinSaving = true;
        Node->ReconstructNode();
        Node->bDisableOrphanPinSaving = bPreviousOrphanSaving;
    }

    void CollectMachines(const UBlueprint* Blueprint, TArray<UAnimationStateMachineGraph*>& OutMachines)
    {
        TArray<UAnimGraphNode_StateMachineBase*> MachineNodes;
        CollectMachineNodes(Blueprint, MachineNodes);

        for (const UAnimGraphNode_StateMachineBase* MachineNode : MachineNodes)
        {
            OutMachines.AddUnique(MachineNode->EditorStateMachineGraph);
        }
    }

    UAnimationStateMachineGraph* FindMachine(const FBlueprintEditContext& Context, const FString& MachineName)
    {
        TArray<UAnimationStateMachineGraph*> Machines;
        CollectMachines(Context.Blueprint, Machines);

        for (UAnimationStateMachineGraph* Machine : Machines)
        {
            if (Machine->GetName() == MachineName)
            {
                return Machine;
            }
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no state machine named '%s'. Present: %s"), *Context.AssetPath, *MachineName, *DescribeMachines(Machines));
        return nullptr;
    }

    UAnimGraphNode_StateMachineBase* FindMachineNode(const UBlueprint* Blueprint, const UEdGraph* MachineGraph)
    {
        TArray<UAnimGraphNode_StateMachineBase*> MachineNodes;
        CollectMachineNodes(Blueprint, MachineNodes);

        for (UAnimGraphNode_StateMachineBase* MachineNode : MachineNodes)
        {
            if (MachineNode->EditorStateMachineGraph == MachineGraph)
            {
                return MachineNode;
            }
        }

        return nullptr;
    }

    UAnimStateNodeBase* FindState(const UAnimationStateMachineGraph* MachineGraph, const FString& StateName)
    {
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node);
            if (State && !State->IsA<UAnimStateTransitionNode>() && State->GetStateName() == StateName)
            {
                return State;
            }
        }

        return nullptr;
    }

    FString DescribeStates(const UAnimationStateMachineGraph* MachineGraph)
    {
        TArray<FString> Names;
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node);
            if (State && !State->IsA<UAnimStateTransitionNode>())
            {
                Names.Add(State->GetStateName());
            }
        }

        return FString::Join(Names, TEXT(", "));
    }

    void CollectTransitions(const UAnimationStateMachineGraph* MachineGraph, const FString& FromName, const FString& ToName, TArray<UAnimStateTransitionNode*>& OutTransitions)
    {
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
            if (!Transition)
            {
                continue;
            }

            const UAnimStateNodeBase* PrevState = Transition->GetPreviousState();
            const UAnimStateNodeBase* NextState = Transition->GetNextState();
            if (PrevState && NextState && PrevState->GetStateName() == FromName && NextState->GetStateName() == ToName)
            {
                OutTransitions.Add(Transition);
            }
        }
    }

    FString DescribeTransitions(const UAnimationStateMachineGraph* MachineGraph)
    {
        TArray<FString> Pairs;
        for (UEdGraphNode* Node : MachineGraph->Nodes)
        {
            UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
            if (!Transition)
            {
                continue;
            }

            const UAnimStateNodeBase* PrevState = Transition->GetPreviousState();
            const UAnimStateNodeBase* NextState = Transition->GetNextState();
            const FString FromName = PrevState ? PrevState->GetStateName() : TEXT("(none)");
            const FString ToName = NextState ? NextState->GetStateName() : TEXT("(none)");
            Pairs.Add(FString::Printf(TEXT("%s -> %s"), *FromName, *ToName));
        }

        return FString::Join(Pairs, TEXT(", "));
    }

    UAnimStateTransitionNode* ResolveTransition(const FBlueprintEditContext& Context, const UAnimationStateMachineGraph* MachineGraph, const FString& FromName, const FString& ToName, const TSharedPtr<FJsonObject>& Desc)
    {
        TArray<UAnimStateTransitionNode*> Matches;
        CollectTransitions(MachineGraph, FromName, ToName, Matches);
        if (Matches.Num() == 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s has no transition from '%s' to '%s'. Present: %s"), *Context.AssetPath, *MachineGraph->GetName(), *FromName, *ToName, *DescribeTransitions(MachineGraph));
            return nullptr;
        }

        int32 Index = 0;
        const bool bHasIndex = Desc->TryGetNumberField(TEXT("Index"), Index);
        if (Matches.Num() > 1 && !bHasIndex)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d transitions connect '%s' to '%s', add an Index between 0 and %d"), *Context.AssetPath, Matches.Num(), *FromName, *ToName, Matches.Num() - 1);
            return nullptr;
        }

        if (!Matches.IsValidIndex(Index))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Index %d is out of range, '%s' to '%s' has %d transition(s)"), *Context.AssetPath, Index, *FromName, *ToName, Matches.Num());
            return nullptr;
        }

        return Matches[Index];
    }
}
