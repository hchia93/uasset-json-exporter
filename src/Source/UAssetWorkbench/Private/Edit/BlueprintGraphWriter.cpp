#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MathExpression.h"
#include "K2Node_MultiGate.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
    // Gate, DoOnce, FlipFlop and the loops are macro graphs in StandardMacros, not native K2Nodes.
    const TCHAR* kStandardMacros = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");

    // "Package.Asset:GraphName", the same form the editor keybind config uses.
    UEdGraph* ResolveMacroGraph(const FString& MacroPath)
    {
        FString LibraryPath;
        FString GraphName;
        if (!MacroPath.Split(TEXT(":"), &LibraryPath, &GraphName))
        {
            return nullptr;
        }

        UBlueprint* Library = LoadObject<UBlueprint>(nullptr, *LibraryPath);
        if (!Library)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Library->MacroGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    // A fresh Select has a wildcard index, which the editor resolves when the user drags a wire in.
    // Nothing drags a wire here, so the index type is set outright and the node rebuilds off it.
    // Enum mode derives the option pins from the enum entries, so it ignores OptionCount.
    bool ConfigureSelect(UK2Node_Select* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        UEdGraphPin* IndexPin = Node->GetIndexPin();
        if (!IndexPin)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: no index pin"));
            return false;
        }

        FString EnumPath;
        if (Desc->TryGetStringField(TEXT("Enum"), EnumPath))
        {
            UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
            if (!Enum)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: cannot load enum '%s'"), *EnumPath);
                return false;
            }

            // PSC_Index survives from the wildcard pin and would keep enum mode from being recognised.
            IndexPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            IndexPin->PinType.PinSubCategory = NAME_None;
            IndexPin->PinType.PinSubCategoryObject = Enum;
            Node->ChangePinType(IndexPin);
            return true;
        }

        int32 OptionCount = 0;
        if (!Desc->TryGetNumberField(TEXT("OptionCount"), OptionCount))
        {
            return true;
        }

        if (OptionCount < 2)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: OptionCount %d is below the two the node always has"), OptionCount);
            return false;
        }

        // Two options is a ternary on a bool, more than two indexes by integer.
        IndexPin->PinType.PinCategory = OptionCount == 2 ? UEdGraphSchema_K2::PC_Boolean : UEdGraphSchema_K2::PC_Int;
        IndexPin->PinType.PinSubCategory = NAME_None;
        IndexPin->PinType.PinSubCategoryObject = nullptr;
        Node->ChangePinType(IndexPin);

        TArray<UEdGraphPin*> OptionPins;
        Node->GetOptionPins(OptionPins);
        while (OptionPins.Num() < OptionCount && Node->CanAddPin())
        {
            Node->AddInputPin();
            Node->GetOptionPins(OptionPins);
        }

        if (OptionPins.Num() != OptionCount)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: wanted %d option pins, node settled at %d"), OptionCount, OptionPins.Num());
            return false;
        }

        return true;
    }

    // Component name on the Blueprint, else a class path. Static and library calls use the latter.
    UClass* ResolveCallTargetClass(UBlueprint* Blueprint, const FString& Target)
    {
        if (Target.IsEmpty())
        {
            return Blueprint->GeneratedClass;
        }

        if (Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == Target)
                {
                    return Node->ComponentTemplate->GetClass();
                }
            }
        }

        // Native subobject on the CDO, which is where C++ components live.
        if (UClass* GeneratedClass = Blueprint->GeneratedClass)
        {
            if (UObject* CDO = GeneratedClass->GetDefaultObject())
            {
                if (FObjectProperty* ObjProp = FindFProperty<FObjectProperty>(GeneratedClass, FName(*Target)))
                {
                    if (UObject* SubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO)))
                    {
                        return SubObj->GetClass();
                    }
                }
            }
        }

        return LoadClass<UObject>(nullptr, *Target);
    }

    void PlaceNode(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        int32 PosX = 0;
        int32 PosY = 0;
        Desc->TryGetNumberField(TEXT("PosX"), PosX);
        Desc->TryGetNumberField(TEXT("PosY"), PosY);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
    }

    class FBlueprintGraphWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Graph");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* SectionObject = nullptr;
            if (!Section->TryGetObject(SectionObject))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Graph must be an object"), *Context.AssetPath);
                return false;
            }

            const TSharedPtr<FJsonObject>& Entry = *SectionObject;

            FString GraphName = TEXT("EventGraph");
            Entry->TryGetStringField(TEXT("Name"), GraphName);

            Context.Graph = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
            if (!Context.Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named '%s'"), *Context.AssetPath, *GraphName);
                return false;
            }

            BlueprintEdit::RegisterExistingNodes(Context.Graph, Context.NodesById);

            // Fixed order: create, set defaults, unlink, link. Unlink before Links so one pass can
            // re-route an existing exec chain instead of leaving it doubly connected.
            if (!ApplyNodes(Context, Entry) || !ApplyPinDefaults(Context, Entry) || !ApplyUnlink(Context, Entry) || !ApplyLinks(Context, Entry))
            {
                return false;
            }

            if (Context.bApply)
            {
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        bool ApplyNodes(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Nodes"), Nodes))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Id;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Id"), Id))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node entry needs an Id"), *Context.AssetPath);
                    return false;
                }

                FString Type;
                Desc->TryGetStringField(TEXT("Type"), Type);
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + node %s (%s)"), *Context.Blueprint->GetName(), *Id, *Type);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                UEdGraphNode* Node = SpawnNode(Context, Desc);
                if (!Node)
                {
                    return false;
                }

                Context.NodesById.Add(Id, Node);
            }

            return true;
        }

        bool ApplyPinDefaults(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* PinDefaults = nullptr;
            if (!Entry->TryGetArrayField(TEXT("PinDefaults"), PinDefaults))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *PinDefaults)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                FString PinName;
                FString PinValue;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetStringField(TEXT("Pin"), PinName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: PinDefaults entry needs Node and Pin"), *Context.AssetPath);
                    return false;
                }
                Desc->TryGetStringField(TEXT("Value"), PinValue);

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: %s.%s = %s"), *Context.Blueprint->GetName(), *NodeId, *PinName, *PinValue);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                UEdGraphPin* Pin = Found ? BlueprintEdit::FindPin(*Found, PinName) : nullptr;
                if (!Pin)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *NodeId, *PinName,
                        *BlueprintEdit::DescribePins(Found ? *Found : nullptr));
                    return false;
                }

                const UEdGraphSchema* Schema = Context.Graph->GetSchema();
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
                {
                    UObject* AsObject = LoadObject<UObject>(nullptr, *PinValue);
                    Schema->TrySetDefaultObject(*Pin, AsObject);
                }
                else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
                {
                    Schema->TrySetDefaultText(*Pin, FText::FromString(PinValue));
                }
                else
                {
                    Schema->TrySetDefaultValue(*Pin, PinValue);
                }
            }

            return true;
        }

        bool ApplyUnlink(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Unlink = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Unlink"), Unlink))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Unlink)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                FString PinName;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetStringField(TEXT("Pin"), PinName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Unlink entry needs Node and Pin"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: unlink %s.%s"), *Context.Blueprint->GetName(), *NodeId, *PinName);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                UEdGraphPin* Pin = Found ? BlueprintEdit::FindPin(*Found, PinName) : nullptr;
                if (!Pin)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *NodeId, *PinName,
                        *BlueprintEdit::DescribePins(Found ? *Found : nullptr));
                    return false;
                }

                Pin->BreakAllPinLinks();
            }

            return true;
        }

        bool ApplyLinks(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Links"), Links))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Links)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString FromNode;
                FString FromPin;
                FString ToNode;
                FString ToPin;
                const bool bComplete = Desc.IsValid()
                    && Desc->TryGetStringField(TEXT("FromNode"), FromNode) && Desc->TryGetStringField(TEXT("FromPin"), FromPin)
                    && Desc->TryGetStringField(TEXT("ToNode"), ToNode) && Desc->TryGetStringField(TEXT("ToPin"), ToPin);
                if (!bComplete)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Links entry needs FromNode/FromPin/ToNode/ToPin"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: link %s.%s -> %s.%s"), *Context.Blueprint->GetName(), *FromNode, *FromPin, *ToNode, *ToPin);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                UEdGraphNode** FoundFrom = Context.NodesById.Find(FromNode);
                UEdGraphNode** FoundTo = Context.NodesById.Find(ToNode);
                UEdGraphPin* A = FoundFrom ? BlueprintEdit::FindPin(*FoundFrom, FromPin) : nullptr;
                UEdGraphPin* B = FoundTo ? BlueprintEdit::FindPin(*FoundTo, ToPin) : nullptr;
                if (!A || !B)
                {
                    UEdGraphNode* BadNode = !A ? (FoundFrom ? *FoundFrom : nullptr) : (FoundTo ? *FoundTo : nullptr);
                    const FString& BadId = !A ? FromNode : ToNode;
                    const FString& BadPin = !A ? FromPin : ToPin;
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *BadId, *BadPin,
                        *BlueprintEdit::DescribePins(BadNode));
                    return false;
                }

                // Schema validates and inserts conversion nodes, same as dragging a wire in the editor.
                if (!Context.Graph->GetSchema()->TryCreateConnection(A, B))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: schema rejected %s.%s -> %s.%s"), *Context.AssetPath, *FromNode, *FromPin, *ToNode, *ToPin);
                    return false;
                }
            }

            return true;
        }

        UEdGraphNode* SpawnNode(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Type;
            if (!Desc->TryGetStringField(TEXT("Type"), Type))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Node entry has no Type"));
                return nullptr;
            }

            UBlueprint* Blueprint = Context.Blueprint;
            UEdGraph* Graph = Context.Graph;
            UEdGraphNode* Result = nullptr;

            if (Type == TEXT("CallFunction"))
            {
                FString FunctionName;
                if (!Desc->TryGetStringField(TEXT("Function"), FunctionName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction needs a Function"));
                    return nullptr;
                }

                FString Target;
                Desc->TryGetStringField(TEXT("Target"), Target);
                UClass* TargetClass = ResolveCallTargetClass(Blueprint, Target);
                if (!TargetClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction cannot resolve Target '%s'"), *Target);
                    return nullptr;
                }

                UFunction* Function = TargetClass->FindFunctionByName(FName(*FunctionName));
                if (!Function)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction: '%s' has no function '%s'"), *TargetClass->GetName(), *FunctionName);
                    return nullptr;
                }

                UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
                Node->SetFromFunction(Function);
                Result = Node;
            }
            else if (Type == TEXT("VariableGet") || Type == TEXT("VariableSet"))
            {
                FString VariableName;
                if (!Desc->TryGetStringField(TEXT("Variable"), VariableName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s needs a Variable"), *Type);
                    return nullptr;
                }

                if (Type == TEXT("VariableGet"))
                {
                    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
                    Node->VariableReference.SetSelfMember(FName(*VariableName));
                    Result = Node;
                }
                else
                {
                    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
                    Node->VariableReference.SetSelfMember(FName(*VariableName));
                    Result = Node;
                }
            }
            else if (Type == TEXT("Branch"))
            {
                Result = NewObject<UK2Node_IfThenElse>(Graph);
            }
            else if (Type == TEXT("Sequence"))
            {
                Result = NewObject<UK2Node_ExecutionSequence>(Graph);
            }
            else if (Type == TEXT("MacroInstance"))
            {
                FString MacroPath;
                if (!Desc->TryGetStringField(TEXT("Macro"), MacroPath))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("MacroInstance needs a Macro"));
                    return nullptr;
                }

                UEdGraph* MacroGraph = ResolveMacroGraph(MacroPath);
                if (!MacroGraph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Cannot resolve macro graph '%s'"), *MacroPath);
                    return nullptr;
                }

                UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
                Node->SetMacroGraph(MacroGraph);
                Result = Node;
            }
            else if (Type == TEXT("MultiGate"))
            {
                Result = NewObject<UK2Node_MultiGate>(Graph);
            }
            else if (Type == TEXT("Select"))
            {
                Result = NewObject<UK2Node_Select>(Graph);
            }
            else if (Type == TEXT("MathExpression"))
            {
                FString Expression;
                Desc->TryGetStringField(TEXT("Expression"), Expression);
                UK2Node_MathExpression* Node = NewObject<UK2Node_MathExpression>(Graph);
                Node->Expression = Expression;
                Result = Node;
            }
            else if (Type == TEXT("CustomEvent"))
            {
                FString EventName;
                if (!Desc->TryGetStringField(TEXT("EventName"), EventName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CustomEvent needs an EventName"));
                    return nullptr;
                }

                UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
                Node->CustomFunctionName = FName(*EventName);
                Result = Node;
            }
            else if (Type == TEXT("Self"))
            {
                Result = NewObject<UK2Node_Self>(Graph);
            }
            else if (UEdGraph* StandardMacro = ResolveMacroGraph(FString::Printf(TEXT("%s:%s"), kStandardMacros, *Type)))
            {
                // Anything left that names a StandardMacros graph: Gate, DoOnce, ForEachLoop, "Do N".
                UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
                Node->SetMacroGraph(StandardMacro);
                Result = Node;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown node Type '%s'"), *Type);
                return nullptr;
            }

            PlaceNode(Result, Desc);
            Graph->AddNode(Result, /* bFromUI */ false, /* bSelectNewNode */ false);
            Result->CreateNewGuid();
            Result->PostPlacedNewNode();
            Result->AllocateDefaultPins();

            // Select derives its option pins from the index type, which only exists once defaults are in.
            if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Result))
            {
                if (!ConfigureSelect(SelectNode, Desc))
                {
                    return nullptr;
                }
            }

            // Expression parses into pins on reconstruct only, PostPlacedNewNode just makes the bound graph.
            if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Result))
            {
                MathNode->ReconstructNode();
            }

            // A Sequence allocates two outputs, further ones are the editor's Add pin button.
            if (UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(Result))
            {
                int32 OutputCount = 0;
                if (Desc->TryGetNumberField(TEXT("OutputCount"), OutputCount))
                {
                    if (OutputCount < 2)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Sequence: OutputCount %d is below the two the node always has"), OutputCount);
                        return nullptr;
                    }

                    while (SequenceNode->GetThenPinGivenIndex(OutputCount - 1) == nullptr)
                    {
                        SequenceNode->AddInputPin();
                    }
                }
            }

            return Result;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintGraphWriter()
{
    return MakeUnique<FBlueprintGraphWriter>();
}
