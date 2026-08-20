#include "Edit/BlueprintWriter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"

namespace BlueprintEdit
{
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

        return nullptr;
    }

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
}
