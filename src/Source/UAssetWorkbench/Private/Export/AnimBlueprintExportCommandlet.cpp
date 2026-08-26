#include "Export/AnimBlueprintExportCommandlet.h"
#include "Export/EdGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/BlendProfile.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "K2Node_AnimGetter.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

UAnimBlueprintExportCommandlet::UAnimBlueprintExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAnimBlueprintExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - AnimBlueprintExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/ABP_A,/Game/Path/ABP_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
        if (!AnimBP)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load AnimBlueprint: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportAnimBlueprint(AnimBP);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export AnimBlueprint: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d anim blueprints exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UAnimBlueprintExportCommandlet::ExportAnimBlueprint(UAnimBlueprint* AnimBP) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("AnimBlueprint"));
    Root->SetStringField(TEXT("AnimBlueprintName"), AnimBP->GetName());
    Root->SetStringField(TEXT("AssetPath"), AnimBP->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    if (AnimBP->ParentClass)
    {
        Root->SetStringField(TEXT("ParentClass"), AnimBP->ParentClass->GetName());
        Root->SetStringField(TEXT("ParentClassPath"), AnimBP->ParentClass->GetPathName());
    }

    if (AnimBP->TargetSkeleton)
    {
        Root->SetStringField(TEXT("TargetSkeleton"), AnimBP->TargetSkeleton->GetPathName());
    }

    // An anim layer interface declares the layer graphs this blueprint has to supply, a plain BP interface does not
    TArray<TSharedPtr<FJsonValue>> InterfacesArray;
    for (const FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
    {
        const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
        if (!InterfaceClass)
        {
            continue;
        }

        TSharedPtr<FJsonObject> InterfaceObj = MakeShared<FJsonObject>();
        InterfaceObj->SetStringField(TEXT("Name"), InterfaceClass->GetName());
        InterfaceObj->SetStringField(TEXT("Path"), InterfaceClass->GetPathName());
        InterfaceObj->SetBoolField(TEXT("IsAnimLayerInterface"), InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()));
        InterfacesArray.Add(MakeShared<FJsonValueObject>(InterfaceObj));
    }
    Root->SetArrayField(TEXT("ImplementedInterfaces"), InterfacesArray);

    FEdGraphJsonOptions GraphOptions;
    GraphOptions.bRecurseSubGraphs = true;
    FEdGraphJsonSerializer Serializer(GraphOptions);

    // Must run before the graph pass below, that is what claims every machine graph on the serializer.
    TArray<FStateMachineEntry> StateMachineEntries;

    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        CollectStateMachines(Graph, FString(), FString(), Serializer, StateMachineEntries);
    }

    for (UEdGraph* Graph : AnimBP->UbergraphPages)
    {
        CollectStateMachines(Graph, FString(), FString(), Serializer, StateMachineEntries);
    }

    // EdGraphs (EventGraph + AnimGraph + anim layers + plain functions)
    TArray<TSharedPtr<FJsonValue>> GraphsArray;

    for (UEdGraph* Graph : AnimBP->UbergraphPages)
    {
        TSharedPtr<FJsonObject> GraphObj = Serializer.ExportGraph(Graph, TEXT("EventGraph"));
        if (GraphObj.IsValid())
        {
            GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        }
    }

    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        const UClass* LayerInterface = nullptr;
        const TCHAR* GraphType = ClassifyFunctionGraph(AnimBP, Graph, LayerInterface);

        TSharedPtr<FJsonObject> GraphObj = Serializer.ExportGraph(Graph, GraphType);
        if (!GraphObj.IsValid())
        {
            continue;
        }

        if (FCString::Strcmp(GraphType, TEXT("AnimLayer")) == 0)
        {
            if (LayerInterface)
            {
                GraphObj->SetStringField(TEXT("Interface"), LayerInterface->GetName());
            }

            const FName LayerGroup = FindLayerGroup(Graph);
            if (!LayerGroup.IsNone())
            {
                GraphObj->SetStringField(TEXT("LayerGroup"), LayerGroup.ToString());
            }
        }

        // Only a plain K2 function has a signature to read, an anim graph or layer answers to its pose root.
        if (FCString::Strcmp(GraphType, TEXT("Function")) == 0)
        {
            TSharedPtr<FJsonObject> Signature = EdGraphJson::ExportFunctionSignature(Graph, AnimBP, false);
            if (Signature.IsValid())
            {
                GraphObj->SetObjectField(TEXT("Signature"), Signature);
            }
        }

        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
    }

    Root->SetArrayField(TEXT("Graphs"), GraphsArray);

    // StateMachines, every machine reached above, exported structurally
    TArray<TSharedPtr<FJsonValue>> StateMachinesArray;

    for (const FStateMachineEntry& Entry : StateMachineEntries)
    {
        TSharedPtr<FJsonObject> SMObj = ExportStateMachine(Entry.Node->EditorStateMachineGraph, Serializer);
        if (!SMObj.IsValid())
        {
            continue;
        }

        SMObj->SetStringField(TEXT("StateMachineName"), Entry.Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

        if (!Entry.ParentMachine.IsEmpty())
        {
            TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
            ParentObj->SetStringField(TEXT("Machine"), Entry.ParentMachine);
            ParentObj->SetStringField(TEXT("State"), Entry.ParentState);
            SMObj->SetObjectField(TEXT("Parent"), ParentObj);
        }

        StateMachinesArray.Add(MakeShared<FJsonValueObject>(SMObj));
    }

    Root->SetArrayField(TEXT("StateMachines"), StateMachinesArray);

    return Root;
}

void UAnimBlueprintExportCommandlet::CollectStateMachines(const UEdGraph* Graph, const FString& ParentMachine, const FString& ParentState, FEdGraphJsonSerializer& Serializer, TArray<FStateMachineEntry>& OutEntries) const
{
    if (!Graph)
    {
        return;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
        if (!SMNode || !SMNode->EditorStateMachineGraph)
        {
            continue;
        }

        FStateMachineEntry& Entry = OutEntries.AddDefaulted_GetRef();
        Entry.Node = SMNode;
        Entry.ParentMachine = ParentMachine;
        Entry.ParentState = ParentState;

        Serializer.ExcludeGraph(SMNode->EditorStateMachineGraph);

        const FString MachineName = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        for (UEdGraphNode* Inner : SMNode->EditorStateMachineGraph->Nodes)
        {
            const UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Inner);
            if (StateNode)
            {
                CollectStateMachines(StateNode->GetBoundGraph(), MachineName, StateNode->GetStateName(), Serializer, OutEntries);
            }
        }
    }
}

const TCHAR* UAnimBlueprintExportCommandlet::ClassifyFunctionGraph(const UAnimBlueprint* AnimBP, const UEdGraph* Graph, const UClass*& OutInterface) const
{
    OutInterface = nullptr;

    if (!Graph)
    {
        return TEXT("Function");
    }

    if (Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
    {
        return TEXT("AnimGraph");
    }

    // An interface layer has to conform to the interface signature and group, name the source
    for (const FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
    {
        const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
        const bool bIsLayerInterface = InterfaceClass && InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass());
        if (bIsLayerInterface && InterfaceClass->FindFunctionByName(Graph->GetFName()))
        {
            OutInterface = InterfaceClass;
            return TEXT("AnimLayer");
        }
    }

    // The compiler stamps this on every anim layer it emits, a plain K2 function graph carries none
    if (const UClass* SkeletonClass = AnimBP->SkeletonGeneratedClass)
    {
        const UFunction* Function = SkeletonClass->FindFunctionByName(Graph->GetFName());
        if (Function && Function->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
        {
            return TEXT("AnimLayer");
        }
    }

    // Uncompiled blueprints have no skeleton function to read, the graph class still separates the two
    if (Graph->IsA(UAnimationGraph::StaticClass()))
    {
        return TEXT("AnimLayer");
    }

    return TEXT("Function");
}

FName UAnimBlueprintExportCommandlet::FindLayerGroup(const UEdGraph* Graph) const
{
    if (!Graph)
    {
        return NAME_None;
    }

    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (const UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
        {
            return RootNode->Node.GetGroup();
        }
    }

    return NAME_None;
}

// Event hooks live in FMemberReference and in FAnimNotifyEvent, the older custom-event form. Both are
// walked by reflection, so a UE version that renames or adds a hook exports without touching this code.
void UAnimBlueprintExportCommandlet::CollectNodeEventBindings(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& OutBindings) const
{
    if (!Node)
    {
        return;
    }

    for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It)
    {
        const FStructProperty* Property = *It;
        if (Property->Struct == FMemberReference::StaticStruct())
        {
            const FMemberReference* Reference = Property->ContainerPtrToValuePtr<FMemberReference>(Node);
            if (Reference && !Reference->GetMemberName().IsNone())
            {
                OutBindings->SetStringField(Property->GetName(), Reference->GetMemberName().ToString());
            }
            continue;
        }

        if (Property->Struct == FAnimNotifyEvent::StaticStruct())
        {
            const FAnimNotifyEvent* Event = Property->ContainerPtrToValuePtr<FAnimNotifyEvent>(Node);
            if (Event && !Event->NotifyName.IsNone())
            {
                OutBindings->SetStringField(Property->GetName(), Event->NotifyName.ToString());
            }
        }
    }
}

// A state's function hooks sit on the result node inside its bound graph, not on the state node, which
// carries only the older notify-event form. Both are folded into one Events block.
TSharedPtr<FJsonObject> UAnimBlueprintExportCommandlet::ExportStateEventBindings(const UAnimStateNode* StateNode) const
{
    TSharedPtr<FJsonObject> Bindings = MakeShared<FJsonObject>();
    CollectNodeEventBindings(StateNode, Bindings);

    if (StateNode && StateNode->BoundGraph)
    {
        for (const UEdGraphNode* Inner : StateNode->BoundGraph->Nodes)
        {
            if (Inner && Inner->IsA(UAnimGraphNode_StateResult::StaticClass()))
            {
                CollectNodeEventBindings(Inner, Bindings);
            }
        }
    }

    return Bindings->Values.Num() > 0 ? Bindings : nullptr;
}

// Same split as a state, the transition node holds the notify-event form and the result node inside
// the rule graph holds the function references.
TSharedPtr<FJsonObject> UAnimBlueprintExportCommandlet::ExportTransitionEventBindings(const UAnimStateTransitionNode* TransNode) const
{
    TSharedPtr<FJsonObject> Bindings = MakeShared<FJsonObject>();
    CollectNodeEventBindings(TransNode, Bindings);

    if (TransNode && TransNode->BoundGraph)
    {
        for (const UEdGraphNode* Inner : TransNode->BoundGraph->Nodes)
        {
            if (Inner && Inner->IsA(UAnimGraphNode_TransitionResult::StaticClass()))
            {
                CollectNodeEventBindings(Inner, Bindings);
            }
        }
    }

    return Bindings->Values.Num() > 0 ? Bindings : nullptr;
}

// Best effort, only the shapes a reader meets constantly. Anything else reports Custom and sends
// the reader to the rule graph exported beside this.
TSharedPtr<FJsonObject> UAnimBlueprintExportCommandlet::ExportTransitionRuleSummary(const UEdGraph* RuleGraph) const
{
    if (!RuleGraph)
    {
        return nullptr;
    }

    const UEdGraphNode* ResultNode = nullptr;
    const UEdGraphPin* ResultPin = nullptr;

    for (const UEdGraphNode* Node : RuleGraph->Nodes)
    {
        if (Node && Node->IsA(UAnimGraphNode_TransitionResult::StaticClass()))
        {
            ResultNode = Node;
            ResultPin = Node->FindPin(TEXT("bCanEnterTransition"), EGPD_Input);
            break;
        }
    }

    if (!ResultPin)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();

    // A binding drives the property every frame and leaves the pin unwired, so it has to be read before the
    // shapes below, which all start from what the pin is linked to.
    FString BoundPath;
    FString BoundType;
    if (EdGraphJson::GetPropertyBinding(ResultNode, TEXT("bCanEnterTransition"), BoundPath, BoundType))
    {
        Summary->SetStringField(TEXT("Bound"), BoundPath);
        Summary->SetStringField(TEXT("Type"), BoundType);
        return Summary;
    }

    if (ResultPin->LinkedTo.Num() == 0)
    {
        Summary->SetStringField(TEXT("Constant"), ResultPin->DefaultValue);
        return Summary;
    }

    const bool bSingleDriver = ResultPin->LinkedTo.Num() == 1 && ResultPin->LinkedTo[0] != nullptr;
    if (!bSingleDriver)
    {
        Summary->SetBoolField(TEXT("Custom"), true);
        return Summary;
    }

    const UEdGraphNode* Driver = ResultPin->LinkedTo[0]->GetOwningNode();
    const UK2Node_AnimGetter* Getter = Cast<UK2Node_AnimGetter>(Driver);
    FString PropertyAccess = Getter ? FString() : EdGraphJson::GetPropertyAccessPath(Driver);
    const UK2Node_CallFunction* Compare = nullptr;

    // A getter or a property access reads a float far more often than a bool, so it usually reaches
    // the result pin through a comparison node rather than directly.
    if (!Getter && PropertyAccess.IsEmpty())
    {
        if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Driver))
        {
            for (const UEdGraphPin* Pin : CallNode->Pins)
            {
                const bool bIsSingleLinkedInput = Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0] != nullptr;
                if (!bIsSingleLinkedInput)
                {
                    continue;
                }

                const UEdGraphNode* Source = Pin->LinkedTo[0]->GetOwningNode();
                Getter = Cast<UK2Node_AnimGetter>(Source);
                if (Getter)
                {
                    Compare = CallNode;
                    break;
                }

                PropertyAccess = EdGraphJson::GetPropertyAccessPath(Source);
                if (!PropertyAccess.IsEmpty())
                {
                    Compare = CallNode;
                    break;
                }
            }
        }
    }

    if (Getter)
    {
        Summary->SetStringField(TEXT("Getter"), Getter->GetFunctionName().ToString());

        if (Getter->SourceStateNode)
        {
            Summary->SetStringField(TEXT("State"), Getter->SourceStateNode->GetStateName());
        }

        if (Getter->SourceNode)
        {
            Summary->SetStringField(TEXT("Machine"), Getter->SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        }
    }
    else if (!PropertyAccess.IsEmpty())
    {
        Summary->SetStringField(TEXT("PropertyAccess"), PropertyAccess);
    }

    if (Getter || !PropertyAccess.IsEmpty())
    {
        if (Compare)
        {
            Summary->SetStringField(TEXT("Compare"), Compare->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

            for (const UEdGraphPin* Pin : Compare->Pins)
            {
                const bool bIsLiteralInput = Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0 && !Pin->DefaultValue.IsEmpty();
                if (bIsLiteralInput)
                {
                    Summary->SetStringField(TEXT("Threshold"), Pin->DefaultValue);
                    break;
                }
            }
        }

        return Summary;
    }

    if (const UK2Node_VariableGet* VariableGet = Cast<UK2Node_VariableGet>(Driver))
    {
        Summary->SetStringField(TEXT("Variable"), VariableGet->GetVarName().ToString());
        return Summary;
    }

    Summary->SetBoolField(TEXT("Custom"), true);
    return Summary;
}

TSharedPtr<FJsonObject> UAnimBlueprintExportCommandlet::ExportStateMachine(UAnimationStateMachineGraph* SMGraph, FEdGraphJsonSerializer& Serializer) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    // States
    TArray<TSharedPtr<FJsonValue>> StatesArray;

    // Transitions
    TArray<TSharedPtr<FJsonValue>> TransitionsArray;

    int32 ConduitCount = 0;
    int32 AliasCount = 0;

    // The entry is a node like any other, but what it points at is the one thing a reader always wants.
    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        const UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node);
        const UEdGraphPin* EntryPin = EntryNode ? EntryNode->GetOutputPin() : nullptr;
        if (EntryPin && EntryPin->LinkedTo.Num() > 0 && EntryPin->LinkedTo[0])
        {
            if (const UAnimStateNodeBase* EntryState = Cast<UAnimStateNodeBase>(EntryPin->LinkedTo[0]->GetOwningNode()))
            {
                Obj->SetStringField(TEXT("EntryState"), EntryState->GetStateName());
            }
        }
    }

    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
        {
            TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
            StateObj->SetStringField(TEXT("StateName"), StateNode->GetStateName());
            StateObj->SetStringField(TEXT("NodeId"), StateNode->NodeGuid.ToString());
            StateObj->SetStringField(TEXT("StateType"), StaticEnum<EAnimStateType>()->GetNameStringByValue(static_cast<int64>(StateNode->StateType.GetValue())));

            if (StateNode->bAlwaysResetOnEntry)
            {
                StateObj->SetBoolField(TEXT("bAlwaysResetOnEntry"), true);
            }

            if (!StateNode->NodeComment.IsEmpty())
            {
                StateObj->SetStringField(TEXT("Comment"), StateNode->NodeComment);
            }

            if (const TSharedPtr<FJsonObject> Events = ExportStateEventBindings(StateNode))
            {
                StateObj->SetObjectField(TEXT("Events"), Events);
            }

            // Export the state's bound graph (contains the animation logic)
            if (StateNode->BoundGraph)
            {
                TSharedPtr<FJsonObject> BoundGraphObj = Serializer.ExportGraph(StateNode->BoundGraph, TEXT("State"));
                if (BoundGraphObj.IsValid())
                {
                    StateObj->SetObjectField(TEXT("BoundGraph"), BoundGraphObj);
                }
            }

            StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
        }
        else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
        {
            TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
            StateObj->SetStringField(TEXT("StateName"), ConduitNode->GetStateName());
            StateObj->SetStringField(TEXT("NodeId"), ConduitNode->NodeGuid.ToString());
            StateObj->SetStringField(TEXT("StateType"), TEXT("Conduit"));

            if (!ConduitNode->NodeComment.IsEmpty())
            {
                StateObj->SetStringField(TEXT("Comment"), ConduitNode->NodeComment);
            }

            // A conduit's bound graph carries transition rules, not a pose
            TSharedPtr<FJsonObject> BoundGraphObj = Serializer.ExportGraph(ConduitNode->GetBoundGraph(), TEXT("Conduit"));
            if (BoundGraphObj.IsValid())
            {
                StateObj->SetObjectField(TEXT("BoundGraph"), BoundGraphObj);
            }

            StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
            ConduitCount++;
        }
        else if (UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(Node))
        {
            TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
            StateObj->SetStringField(TEXT("StateName"), AliasNode->GetStateName());
            StateObj->SetStringField(TEXT("NodeId"), AliasNode->NodeGuid.ToString());
            StateObj->SetStringField(TEXT("StateType"), TEXT("Alias"));

            if (!AliasNode->NodeComment.IsEmpty())
            {
                StateObj->SetStringField(TEXT("Comment"), AliasNode->NodeComment);
            }

            if (AliasNode->bGlobalAlias)
            {
                StateObj->SetBoolField(TEXT("bGlobalAlias"), true);
            }
            else
            {
                TArray<FString> AliasedNames;
                for (const TWeakObjectPtr<UAnimStateNodeBase>& Aliased : AliasNode->GetAliasedStates())
                {
                    if (const UAnimStateNodeBase* AliasedState = Aliased.Get())
                    {
                        AliasedNames.Add(AliasedState->GetStateName());
                    }
                }

                // Source is a TSet, sort so two exports of an untouched asset stay comparable
                AliasedNames.Sort();

                TArray<TSharedPtr<FJsonValue>> AliasedArray;
                for (const FString& AliasedName : AliasedNames)
                {
                    AliasedArray.Add(MakeShared<FJsonValueString>(AliasedName));
                }

                StateObj->SetArrayField(TEXT("AliasedStates"), AliasedArray);
            }

            StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
            AliasCount++;
        }
        else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
        {
            TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
            TransObj->SetStringField(TEXT("NodeId"), TransNode->NodeGuid.ToString());

            UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
            UAnimStateNodeBase* NextState = TransNode->GetNextState();

            if (PrevState)
            {
                TransObj->SetStringField(TEXT("FromState"), PrevState->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            }
            if (NextState)
            {
                TransObj->SetStringField(TEXT("ToState"), NextState->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
            }

            TransObj->SetNumberField(TEXT("CrossfadeDuration"), TransNode->CrossfadeDuration);
            TransObj->SetStringField(TEXT("BlendMode"), StaticEnum<EAlphaBlendOption>()->GetNameStringByValue(static_cast<int64>(TransNode->BlendMode)));
            TransObj->SetNumberField(TEXT("PriorityOrder"), TransNode->PriorityOrder);
            TransObj->SetStringField(TEXT("LogicType"), StaticEnum<ETransitionLogicType::Type>()->GetNameStringByValue(static_cast<int64>(TransNode->LogicType.GetValue())));

            // Everything below is off by default, emitting it unconditionally would bury the one transition
            // that actually carries a non-standard setting
            if (TransNode->bAutomaticRuleBasedOnSequencePlayerInState)
            {
                TransObj->SetBoolField(TEXT("bAutomaticRuleBasedOnSequencePlayerInState"), true);
                TransObj->SetNumberField(TEXT("AutomaticRuleTriggerTime"), TransNode->AutomaticRuleTriggerTime);
            }

            if (TransNode->bDisabled)
            {
                TransObj->SetBoolField(TEXT("bDisabled"), true);
            }

            if (TransNode->Bidirectional)
            {
                TransObj->SetBoolField(TEXT("Bidirectional"), true);
            }

            if (TransNode->MinTimeBeforeReentry >= 0.0f)
            {
                TransObj->SetNumberField(TEXT("MinTimeBeforeReentry"), TransNode->MinTimeBeforeReentry);
            }

            if (TransNode->bAllowInertializationForSelfTransitions)
            {
                TransObj->SetBoolField(TEXT("bAllowInertializationForSelfTransitions"), true);
            }

            if (!TransNode->SyncGroupNameToRequireValidMarkersRule.IsNone())
            {
                TransObj->SetStringField(TEXT("SyncGroupNameToRequireValidMarkersRule"), TransNode->SyncGroupNameToRequireValidMarkersRule.ToString());
            }

            if (TransNode->bSharedRules)
            {
                TransObj->SetBoolField(TEXT("bSharedRules"), true);
                TransObj->SetStringField(TEXT("SharedRulesName"), TransNode->SharedRulesName);
                TransObj->SetStringField(TEXT("SharedRulesGuid"), TransNode->SharedRulesGuid.ToString());
            }

            if (TransNode->bSharedCrossfade)
            {
                TransObj->SetBoolField(TEXT("SharedCrossfade"), true);
                TransObj->SetStringField(TEXT("SharedCrossfadeName"), TransNode->SharedCrossfadeName);
                TransObj->SetStringField(TEXT("SharedCrossfadeGuid"), TransNode->SharedCrossfadeGuid.ToString());
            }

            if (const UBlendProfile* BlendProfile = TransNode->BlendProfileWrapper.GetBlendProfile())
            {
                TransObj->SetStringField(TEXT("BlendProfile"), BlendProfile->GetPathName());
                TransObj->SetStringField(TEXT("BlendProfileMode"), StaticEnum<EBlendProfileMode>()->GetNameStringByValue(static_cast<int64>(BlendProfile->GetMode())));
            }

            if (TransNode->CustomBlendCurve)
            {
                TransObj->SetStringField(TEXT("CustomBlendCurve"), TransNode->CustomBlendCurve->GetPathName());
            }

            // Custom blend logic lives in its own pose graph, without it a Custom LogicType reads as a bare rule
            if (TransNode->CustomTransitionGraph)
            {
                TSharedPtr<FJsonObject> CustomGraph = Serializer.ExportGraph(TransNode->CustomTransitionGraph, TEXT("CustomTransition"));
                if (CustomGraph.IsValid())
                {
                    TransObj->SetObjectField(TEXT("CustomTransitionGraph"), CustomGraph);
                }
            }

            // Export transition rule graph
            if (const TSharedPtr<FJsonObject> TransEvents = ExportTransitionEventBindings(TransNode))
            {
                TransObj->SetObjectField(TEXT("Events"), TransEvents);
            }

            if (TransNode->BoundGraph)
            {
                TSharedPtr<FJsonObject> RuleGraph = Serializer.ExportGraph(TransNode->BoundGraph, TEXT("TransitionRule"));
                if (RuleGraph.IsValid())
                {
                    TransObj->SetObjectField(TEXT("TransitionRule"), RuleGraph);
                }

                if (const TSharedPtr<FJsonObject> RuleSummary = ExportTransitionRuleSummary(TransNode->BoundGraph))
                {
                    TransObj->SetObjectField(TEXT("RuleSummary"), RuleSummary);
                }
            }

            TransitionsArray.Add(MakeShared<FJsonValueObject>(TransObj));
        }
    }

    Obj->SetArrayField(TEXT("States"), StatesArray);
    Obj->SetArrayField(TEXT("Transitions"), TransitionsArray);
    Obj->SetNumberField(TEXT("StateCount"), StatesArray.Num());
    Obj->SetNumberField(TEXT("TransitionCount"), TransitionsArray.Num());
    Obj->SetNumberField(TEXT("ConduitCount"), ConduitCount);
    Obj->SetNumberField(TEXT("AliasCount"), AliasCount);

    return Obj;
}

