#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

// One Blueprint, carried across every writer of a single target so they can see each other's work.
// NodesById is the reason this exists: the Graph writer registers nodes under the Id the spec gave
// them, and the Layout writer then addresses those same Ids instead of NodeGuids nobody knows yet.
struct FBlueprintEditContext
{
    UBlueprint* Blueprint = nullptr;
    FString AssetPath;
    bool bApply = false;

    UEdGraph* Graph = nullptr;
    TMap<FString, UEdGraphNode*> NodesById;

    // Layout is cosmetic. Everything else changes the class and has to go back through the compiler.
    bool bNeedsStructuralRecompile = false;
    int32 Ops = 0;
};

// Mirrors the engine's IDiffControl split: the driver owns the asset, each writer owns one facet.
class IBlueprintWriter
{
public:
    virtual ~IBlueprintWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IBlueprintWriter> MakeBlueprintComponentWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintVariableWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintDefaultsWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintGraphWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintLayoutWriter();

namespace BlueprintEdit
{
    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);

    // Pre-existing nodes answer to their 32-hex NodeGuid, the same one BlueprintEdGraphExport prints.
    void RegisterExistingNodes(UEdGraph* Graph, TMap<FString, UEdGraphNode*>& OutNodesById);

    // Exact PinName first, then PinFriendlyName, because the editor shows the friendly one and that
    // is what a spec author copies. "Return Value" and "ReturnValue" are the same pin.
    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName);

    // A wrong pin name is the most common spec error, so failures print what was actually there.
    FString DescribePins(UEdGraphNode* Node);
}
