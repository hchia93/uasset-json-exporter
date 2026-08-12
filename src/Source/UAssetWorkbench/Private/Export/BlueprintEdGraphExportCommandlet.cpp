#include "Export/BlueprintEdGraphExportCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

UBlueprintEdGraphExportCommandlet::UBlueprintEdGraphExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UBlueprintEdGraphExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - BlueprintEdGraphExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);
    FExportOptions Options = ParseExportOptions(Params);

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Options: IncludeGraphs=%s"),
        Options.bIncludeGraphs ? TEXT("true") : TEXT("false"));

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/BP_A,/Game/Path/BP_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
        if (!Blueprint)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load Blueprint: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportBlueprint(Blueprint, Options);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export Blueprint: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetWorkbench::GetExportPath(AssetPath);
        if (UAssetWorkbench::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d blueprints exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UBlueprintEdGraphExportCommandlet::ExportBlueprint(UBlueprint* Blueprint, const FExportOptions& Options) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("BlueprintEdGraph"));
    Root->SetStringField(TEXT("Blueprint"), Blueprint->GetName());
    Root->SetStringField(TEXT("AssetPath"), Blueprint->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Parent class
    if (Blueprint->ParentClass)
    {
        Root->SetStringField(TEXT("ParentClass"), Blueprint->ParentClass->GetName());
    }

    // Implemented interfaces (BP "Implements Interface" list). Answers "who implements interface X"
    // directly, without -graphs node scraping for the overriding event titles.
    TArray<TSharedPtr<FJsonValue>> InterfacesArray;
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = InterfaceDesc.Interface.Get();
        if (!InterfaceClass)
        {
            continue;
        }

        TSharedPtr<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
        IfaceObj->SetStringField(TEXT("Name"), InterfaceClass->GetName());
        IfaceObj->SetStringField(TEXT("Path"), InterfaceClass->GetPathName());
        InterfacesArray.Add(MakeShared<FJsonValueObject>(IfaceObj));
    }
    Root->SetArrayField(TEXT("ImplementedInterfaces"), InterfacesArray);
    Root->SetNumberField(TEXT("ImplementedInterfaceCount"), InterfacesArray.Num());

    // Build a set of SCS component variable names so we can mark auto-generated variables.
    // Each SCS component registers a property of the same name as the variable on the generated class.
    TSet<FString> SCSComponentNames;
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode)
            {
                SCSComponentNames.Add(SCSNode->GetVariableName().ToString());
            }
        }
    }

    // Variables (member properties on generated class)
    TArray<TSharedPtr<FJsonValue>> VariablesArray;
    int32 UserVariableCount = 0;
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        UObject* CDO = GeneratedClass->GetDefaultObject();
        for (TFieldIterator<FProperty> PropIt(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            const FString PropName = Property->GetName();
            VarObj->SetStringField(TEXT("Name"), PropName);
            VarObj->SetStringField(TEXT("Type"), Property->GetCPPType());

            const bool bIsAutoGen = SCSComponentNames.Contains(PropName);
            VarObj->SetBoolField(TEXT("IsAutoGenerated"), bIsAutoGen);
            if (!bIsAutoGen)
            {
                ++UserVariableCount;
            }

            if (CDO)
            {
                FString DefaultValue;
                Property->ExportTextItem_Direct(DefaultValue, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
                if (!DefaultValue.IsEmpty())
                {
                    VarObj->SetStringField(TEXT("Default"), DefaultValue);
                }
            }

            VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
        }
    }
    Root->SetArrayField(TEXT("Variables"), VariablesArray);
    Root->SetNumberField(TEXT("VariableCount"), VariablesArray.Num());
    Root->SetNumberField(TEXT("UserVariableCount"), UserVariableCount);

    // Components (from SimpleConstructionScript) with hierarchy info + property overrides
    // Each entry includes ParentName / IsRoot so consumers can identify root component cheaply
    // without re-traversing the tree.
    TArray<TSharedPtr<FJsonValue>> ComponentsArray;
    FString RootComponentName;
    FString RootComponentClass;
    int32 NonEditorComponentCount = 0;

    if (Blueprint->SimpleConstructionScript)
    {
        const TArray<USCS_Node*>& RootNodes = Blueprint->SimpleConstructionScript->GetRootNodes();

        // Build parent lookup map by walking the tree from roots
        TMap<USCS_Node*, USCS_Node*> ParentMap;
        TArray<USCS_Node*> Stack = RootNodes;
        TSet<USCS_Node*> Visited;
        while (Stack.Num() > 0)
        {
            USCS_Node* Cur = Stack.Pop();
            if (!Cur || Visited.Contains(Cur))
            {
                continue;
            }
            Visited.Add(Cur);
            for (USCS_Node* Child : Cur->GetChildNodes())
            {
                if (Child)
                {
                    ParentMap.Add(Child, Cur);
                    Stack.Push(Child);
                }
            }
        }

        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (!SCSNode || !SCSNode->ComponentTemplate)
            {
                continue;
            }

            TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
            const FString CompName = SCSNode->GetVariableName().ToString();
            const FString CompClass = SCSNode->ComponentTemplate->GetClass()->GetName();
            CompObj->SetStringField(TEXT("Name"), CompName);
            CompObj->SetStringField(TEXT("Class"), CompClass);

            const bool bIsRoot = RootNodes.Contains(SCSNode);
            CompObj->SetBoolField(TEXT("IsRoot"), bIsRoot);

            if (USCS_Node** ParentPtr = ParentMap.Find(SCSNode))
            {
                if (*ParentPtr)
                {
                    CompObj->SetStringField(TEXT("ParentName"), (*ParentPtr)->GetVariableName().ToString());
                }
            }

            const bool bIsEditorOnly = SCSNode->ComponentTemplate->IsEditorOnly();
            CompObj->SetBoolField(TEXT("IsEditorOnly"), bIsEditorOnly);
            if (!bIsEditorOnly)
            {
                ++NonEditorComponentCount;
            }

            if (bIsRoot && RootComponentName.IsEmpty())
            {
                RootComponentName = CompName;
                RootComponentClass = CompClass;
            }

            TArray<TSharedPtr<FJsonValue>> OverridesArray;
            ExportPropertyOverrides(SCSNode->ComponentTemplate, OverridesArray);
            if (OverridesArray.Num() > 0)
            {
                CompObj->SetArrayField(TEXT("PropertyOverrides"), OverridesArray);
            }

            TArray<TSharedPtr<FJsonValue>> ResolvedArray;
            ExportResolvedProperties(SCSNode->ComponentTemplate, ResolvedArray);
            if (ResolvedArray.Num() > 0)
            {
                CompObj->SetArrayField(TEXT("ResolvedProperties"), ResolvedArray);
            }

            ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
        }
    }
    Root->SetArrayField(TEXT("Components"), ComponentsArray);
    Root->SetNumberField(TEXT("ComponentCount"), ComponentsArray.Num());
    Root->SetNumberField(TEXT("NonEditorComponentCount"), NonEditorComponentCount);
    if (!RootComponentName.IsEmpty())
    {
        Root->SetStringField(TEXT("RootComponentName"), RootComponentName);
        Root->SetStringField(TEXT("RootComponentClass"), RootComponentClass);
    }

    // Inherited component property overrides (C++ default subobjects modified in Blueprint CDO)
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        UObject* CDO = GeneratedClass->GetDefaultObject();
        UClass* ParentClass = GeneratedClass->GetSuperClass();
        UObject* ParentCDO = ParentClass ? ParentClass->GetDefaultObject() : nullptr;

        if (CDO && ParentCDO)
        {
            TArray<TSharedPtr<FJsonValue>> InheritedOverridesArray;

            for (TFieldIterator<FObjectProperty> PropIt(ParentClass); PropIt; ++PropIt)
            {
                FObjectProperty* ObjProp = *PropIt;
                if (!ObjProp || ObjProp->HasAnyPropertyFlags(CPF_Transient))
                {
                    continue;
                }

                UObject* ChildSubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO));
                UObject* ParentSubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(ParentCDO));

                if (!ChildSubObj || !ParentSubObj || ChildSubObj->GetClass() != ParentSubObj->GetClass())
                {
                    continue;
                }
                if (!ChildSubObj->IsDefaultSubobject())
                {
                    continue;
                }

                TArray<TSharedPtr<FJsonValue>> SubObjOverrides;
                ExportPropertyOverridesCompare(ChildSubObj, ParentSubObj, SubObjOverrides);
                if (SubObjOverrides.Num() > 0)
                {
                    TSharedPtr<FJsonObject> InheritedObj = MakeShared<FJsonObject>();
                    InheritedObj->SetStringField(TEXT("Name"), ObjProp->GetName());
                    InheritedObj->SetStringField(TEXT("Class"), ChildSubObj->GetClass()->GetName());
                    InheritedObj->SetArrayField(TEXT("PropertyOverrides"), SubObjOverrides);
                    InheritedOverridesArray.Add(MakeShared<FJsonValueObject>(InheritedObj));
                }
            }

            if (InheritedOverridesArray.Num() > 0)
            {
                Root->SetArrayField(TEXT("InheritedComponentOverrides"), InheritedOverridesArray);
            }
        }
    }

    // Actor-level CDO: full resolved properties (Tags, bHidden, replication flags, etc.)
    // and the deltas vs the parent class CDO. Lets external tools see what the BP author
    // tweaked at the actor level (independent of components).
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        if (UObject* CDO = GeneratedClass->GetDefaultObject())
        {
            TArray<TSharedPtr<FJsonValue>> ActorResolvedArray;
            ExportResolvedProperties(CDO, ActorResolvedArray);
            if (ActorResolvedArray.Num() > 0)
            {
                Root->SetArrayField(TEXT("ActorCDOProperties"), ActorResolvedArray);
            }

            UClass* ParentClass = GeneratedClass->GetSuperClass();
            UObject* ParentCDO = ParentClass ? ParentClass->GetDefaultObject() : nullptr;
            if (ParentCDO)
            {
                TArray<TSharedPtr<FJsonValue>> ActorOverridesArray;
                ExportPropertyOverridesCompare(CDO, ParentCDO, ActorOverridesArray);
                if (ActorOverridesArray.Num() > 0)
                {
                    Root->SetArrayField(TEXT("ActorCDOOverrides"), ActorOverridesArray);
                }
            }
        }
    }

    // Graphs (EventGraphs + FunctionGraphs)
    // Default: emit Name + GraphType + NodeCount + HasLogic only. Pass -graphs to include full nodes/pins.
    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    int32 EventGraphNodeTotal = 0;
    int32 FunctionGraphNodeTotal = 0;
    bool bHasAnyLogic = false;

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        TSharedPtr<FJsonObject> GraphObj = ExportGraph(Graph, Options);
        if (GraphObj.IsValid())
        {
            GraphObj->SetStringField(TEXT("GraphType"), TEXT("EventGraph"));
            int32 NodeCount = 0;
            GraphObj->TryGetNumberField(TEXT("NodeCount"), NodeCount);
            EventGraphNodeTotal += NodeCount;
            bool bGraphLogic = false;
            GraphObj->TryGetBoolField(TEXT("HasLogic"), bGraphLogic);
            bHasAnyLogic = bHasAnyLogic || bGraphLogic;
            GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        }
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        TSharedPtr<FJsonObject> GraphObj = ExportGraph(Graph, Options);
        if (GraphObj.IsValid())
        {
            GraphObj->SetStringField(TEXT("GraphType"), TEXT("Function"));
            int32 NodeCount = 0;
            GraphObj->TryGetNumberField(TEXT("NodeCount"), NodeCount);
            FunctionGraphNodeTotal += NodeCount;
            bool bGraphLogic = false;
            GraphObj->TryGetBoolField(TEXT("HasLogic"), bGraphLogic);
            bHasAnyLogic = bHasAnyLogic || bGraphLogic;
            GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        }
    }

    Root->SetArrayField(TEXT("Graphs"), GraphsArray);
    Root->SetNumberField(TEXT("EventGraphNodeTotal"), EventGraphNodeTotal);
    Root->SetNumberField(TEXT("FunctionGraphNodeTotal"), FunctionGraphNodeTotal);
    Root->SetNumberField(TEXT("FunctionGraphCount"), Blueprint->FunctionGraphs.Num());
    Root->SetBoolField(TEXT("HasAnyGraphLogic"), bHasAnyLogic);

    // Referenced assets via AssetRegistry
    // Split into Levels (umap) and Other to support quick "level-only references" filtering.
    TArray<TSharedPtr<FJsonValue>> LevelRefsArray;
    TArray<TSharedPtr<FJsonValue>> OtherRefsArray;
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        // Reverse dependencies: assets that reference this Blueprint's package
        // Default FDependencyQuery() = include hard + soft references, no flag filtering.
        TArray<FName> Referencers;
        AssetRegistry.GetReferencers(
            Blueprint->GetOutermost()->GetFName(),
            Referencers,
            UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& RefName : Referencers)
        {
            FString RefPath = RefName.ToString();

            if (RefPath.StartsWith(TEXT("/Script/")) || RefPath.StartsWith(TEXT("/Engine/")))
            {
                continue;
            }

            TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
            RefObj->SetStringField(TEXT("PackageName"), RefPath);

            FString AssetClassName;
            TArray<FAssetData> AssetDataList;
            AssetRegistry.GetAssetsByPackageName(RefName, AssetDataList, true);
            if (AssetDataList.Num() > 0)
            {
                AssetClassName = AssetDataList[0].AssetClassPath.GetAssetName().ToString();
                RefObj->SetStringField(TEXT("AssetClass"), AssetClassName);
            }

            const bool bIsLevel = (AssetClassName == TEXT("World"))
                || RefPath.EndsWith(TEXT(".umap"))
                || RefPath.Contains(TEXT("/Maps/"));
            if (bIsLevel)
            {
                LevelRefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
            }
            else
            {
                OtherRefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
            }
        }
    }
    Root->SetArrayField(TEXT("Referencers_Levels"), LevelRefsArray);
    Root->SetArrayField(TEXT("Referencers_Other"), OtherRefsArray);
    Root->SetNumberField(TEXT("Referencers_LevelCount"), LevelRefsArray.Num());
    Root->SetNumberField(TEXT("Referencers_OtherCount"), OtherRefsArray.Num());

    // Forward dependencies: assets this Blueprint references (mesh, material, etc.)
    TArray<TSharedPtr<FJsonValue>> RefsArray;
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(Blueprint->GetOutermost()->GetFName(), Dependencies);

        for (const FName& DepName : Dependencies)
        {
            FString DepPath = DepName.ToString();

            if (DepPath.StartsWith(TEXT("/Script/")) || DepPath.StartsWith(TEXT("/Engine/")))
            {
                continue;
            }

            TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
            RefObj->SetStringField(TEXT("PackageName"), DepPath);

            TArray<FAssetData> AssetDataList;
            AssetRegistry.GetAssetsByPackageName(DepName, AssetDataList, true);
            if (AssetDataList.Num() > 0)
            {
                RefObj->SetStringField(TEXT("AssetClass"), AssetDataList[0].AssetClassPath.GetAssetName().ToString());
            }

            RefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
        }
    }
    Root->SetArrayField(TEXT("ReferencedAssets"), RefsArray);

    return Root;
}

TSharedPtr<FJsonObject> UBlueprintEdGraphExportCommandlet::ExportGraph(const UEdGraph* Graph, const FExportOptions& Options) const
{
    if (!Graph)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
    GraphObj->SetStringField(TEXT("Name"), Graph->GetName());
    GraphObj->SetNumberField(TEXT("NodeCount"), Graph->Nodes.Num());

    // bHasLogic = true iff any node has at least one pin with LinkedTo > 0.
    // Distinguishes a pure-stub graph (e.g. disabled placeholder events, lone FunctionEntry)
    // from a graph with actual user logic. Lets the lean output stand alone for candidate
    // filtering without forcing -graphs.
    bool bHasLogic = false;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->LinkedTo.Num() > 0)
            {
                bHasLogic = true;
                break;
            }
        }
        if (bHasLogic)
        {
            break;
        }
    }
    GraphObj->SetBoolField(TEXT("HasLogic"), bHasLogic);

    if (!Options.bIncludeGraphs)
    {
        return GraphObj;
    }

    TArray<TSharedPtr<FJsonValue>> NodesArray;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        TSharedPtr<FJsonObject> NodeObj = ExportNode(Node);
        if (NodeObj.IsValid())
        {
            NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
    }
    GraphObj->SetArrayField(TEXT("Nodes"), NodesArray);

    return GraphObj;
}

TSharedPtr<FJsonObject> UBlueprintEdGraphExportCommandlet::ExportNode(const UEdGraphNode* Node) const
{
    if (!Node)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

    NodeObj->SetStringField(TEXT("NodeId"), Node->NodeGuid.ToString());
    NodeObj->SetStringField(TEXT("Class"), Node->GetClass()->GetName());
    NodeObj->SetStringField(TEXT("Title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    NodeObj->SetNumberField(TEXT("PosX"), Node->NodePosX);
    NodeObj->SetNumberField(TEXT("PosY"), Node->NodePosY);

    if (!Node->NodeComment.IsEmpty())
    {
        NodeObj->SetStringField(TEXT("Comment"), Node->NodeComment);
    }

    if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
    {
        FName VarName = VarNode->VariableReference.GetMemberName();
        if (!VarName.IsNone())
        {
            NodeObj->SetStringField(TEXT("VariableName"), VarName.ToString());
        }
        if (UClass* VarOwner = VarNode->VariableReference.GetMemberParentClass())
        {
            NodeObj->SetStringField(TEXT("VariableOwner"), VarOwner->GetName());
        }
    }

    if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
    {
        if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
        {
            NodeObj->SetStringField(TEXT("MacroName"), MacroGraph->GetName());
            if (UPackage* MacroPkg = MacroGraph->GetOutermost())
            {
                NodeObj->SetStringField(TEXT("MacroPackage"), MacroPkg->GetName());
            }
        }
    }

    if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
    {
        FName FunctionName = CallNode->FunctionReference.GetMemberName();
        if (!FunctionName.IsNone())
        {
            NodeObj->SetStringField(TEXT("FunctionName"), FunctionName.ToString());
        }

        UClass* MemberParent = CallNode->FunctionReference.GetMemberParentClass();
        if (MemberParent)
        {
            NodeObj->SetStringField(TEXT("FunctionOwner"), MemberParent->GetName());
        }
    }

    if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
    {
        if (CastNode->TargetType)
        {
            NodeObj->SetStringField(TEXT("CastTarget"), CastNode->TargetType->GetPathName());
        }
    }

    if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
    {
        FName EventName = EventNode->EventReference.GetMemberName();
        if (!EventName.IsNone())
        {
            NodeObj->SetStringField(TEXT("EventName"), EventName.ToString());
        }
    }

    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->bHidden)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PinObj = ExportPin(Pin);
        if (PinObj.IsValid())
        {
            PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
        }
    }
    NodeObj->SetArrayField(TEXT("Pins"), PinsArray);

    return NodeObj;
}

TSharedPtr<FJsonObject> UBlueprintEdGraphExportCommandlet::ExportPin(const UEdGraphPin* Pin) const
{
    if (!Pin)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();

    PinObj->SetStringField(TEXT("Name"), Pin->PinName.ToString());
    PinObj->SetStringField(TEXT("Direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
    PinObj->SetStringField(TEXT("Type"), Pin->PinType.PinCategory.ToString());

    if (Pin->PinType.PinSubCategoryObject.IsValid())
    {
        PinObj->SetStringField(TEXT("SubType"), Pin->PinType.PinSubCategoryObject->GetName());
    }

    if (!Pin->DefaultValue.IsEmpty())
    {
        PinObj->SetStringField(TEXT("Default"), Pin->DefaultValue);
    }

    if (!Pin->DefaultTextValue.IsEmpty())
    {
        PinObj->SetStringField(TEXT("DefaultText"), Pin->DefaultTextValue.ToString());
    }

    if (Pin->DefaultObject)
    {
        PinObj->SetStringField(TEXT("DefaultObject"), Pin->DefaultObject->GetPathName());
    }

    if (Pin->LinkedTo.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> LinksArray;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (LinkedPin && LinkedPin->GetOwningNode())
            {
                TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
                LinkObj->SetStringField(TEXT("NodeId"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
                LinkObj->SetStringField(TEXT("NodeTitle"), LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                LinkObj->SetStringField(TEXT("PinName"), LinkedPin->PinName.ToString());
                LinksArray.Add(MakeShared<FJsonValueObject>(LinkObj));
            }
        }
        PinObj->SetArrayField(TEXT("LinkedTo"), LinksArray);
    }

    return PinObj;
}

void UBlueprintEdGraphExportCommandlet::ExportPropertyOverrides(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance)
    {
        return;
    }

    UClass* ObjClass = Instance->GetClass();
    UObject* ClassCDO = ObjClass->GetDefaultObject();
    if (!ClassCDO || ClassCDO == Instance)
    {
        return;
    }

    ExportPropertyOverridesCompare(Instance, ClassCDO, OutArray);
}

void UBlueprintEdGraphExportCommandlet::ExportPropertyOverridesCompare(UObject* Instance, UObject* Reference, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance || !Reference)
    {
        return;
    }

    for (TFieldIterator<FProperty> PropIt(Instance->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        // When Reference is a base-class CDO (e.g. AActor CDO vs a BP-class CDO), the
        // iterator surfaces properties owned by Instance subclasses. Reading those off
        // Reference triggers a hard assertion in ContainerPtrToValuePtr. Guard here.
        UClass* PropOwner = Prop->GetOwner<UClass>();
        if (PropOwner && !Reference->GetClass()->IsChildOf(PropOwner))
        {
            continue;
        }

        // Skip default-subobject pointers (nested components handled separately via InheritedComponentOverrides).
        // External asset references (StaticMesh, Material, etc.) are kept and serialized as PathName.
        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
        {
            UObject* CurObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Instance));
            UObject* RefObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Reference));
            const bool bIsSubobjectRef = (CurObj && CurObj->IsDefaultSubobject()) || (RefObj && RefObj->IsDefaultSubobject());
            if (bIsSubobjectRef)
            {
                continue;
            }
        }

        FString CurrentValue;
        Prop->ExportTextItem_Direct(CurrentValue, Prop->ContainerPtrToValuePtr<void>(Instance), nullptr, Instance, PPF_None);

        FString DefaultValue;
        Prop->ExportTextItem_Direct(DefaultValue, Prop->ContainerPtrToValuePtr<void>(Reference), nullptr, Reference, PPF_None);

        if (CurrentValue != DefaultValue)
        {
            TSharedPtr<FJsonObject> OverrideObj = MakeShared<FJsonObject>();
            OverrideObj->SetStringField(TEXT("Name"), Prop->GetName());
            OverrideObj->SetStringField(TEXT("Type"), Prop->GetCPPType());
            OverrideObj->SetStringField(TEXT("Default"), DefaultValue);
            OverrideObj->SetStringField(TEXT("Value"), CurrentValue);
            OutArray.Add(MakeShared<FJsonValueObject>(OverrideObj));
        }
    }
}

void UBlueprintEdGraphExportCommandlet::ExportResolvedProperties(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance)
    {
        return;
    }

    for (TFieldIterator<FProperty> PropIt(Instance->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        // Skip default-subobject pointers (component nesting noise, external asset refs are kept).
        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
        {
            UObject* CurObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Instance));
            if (CurObj && CurObj->IsDefaultSubobject())
            {
                continue;
            }
        }

        FString CurrentValue;
        Prop->ExportTextItem_Direct(CurrentValue, Prop->ContainerPtrToValuePtr<void>(Instance), nullptr, Instance, PPF_None);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("Name"), Prop->GetName());
        Entry->SetStringField(TEXT("Type"), Prop->GetCPPType());
        Entry->SetStringField(TEXT("Value"), CurrentValue);
        OutArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
}

UBlueprintEdGraphExportCommandlet::FExportOptions UBlueprintEdGraphExportCommandlet::ParseExportOptions(const FString& Params) const
{
    FExportOptions Options;
    Options.bIncludeGraphs = FParse::Param(*Params, TEXT("graphs"));
    return Options;
}
