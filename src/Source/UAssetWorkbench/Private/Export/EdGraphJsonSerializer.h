#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

struct FEdGraphJsonOptions
{
    // Off collapses a graph to Name / GraphType / NodeCount / HasLogic, the lean shape
    // BlueprintEdGraphExport ships without -graphs.
    bool bWithNodes = true;

    bool bRecurseSubGraphs = false;

    // Visited set already terminates shared and cyclic graphs. This only caps pathological nesting.
    int32 MaxSubGraphDepth = 8;

    bool bIncludeHiddenPins = false;
};

// One EdGraph to JSON, shared by the Blueprint, AnimBlueprint and Widget exporters.
// Scoped to one asset export, sub-graph recursion needs a visited set spanning every graph the asset reaches.
// A caller that exports some graph through its own structured path calls ExcludeGraph first.
class FEdGraphJsonSerializer
{
public:

    explicit FEdGraphJsonSerializer(const FEdGraphJsonOptions& InOptions);

    // GraphType is caller knowledge, one UEdGraph class backs an event graph and a function alike.
    // Recursed sub-graphs take their tag from the node that owns them.
    TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph, const TCHAR* GraphType);

    TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node, int32 SubGraphDepth = 0);

    TSharedPtr<FJsonObject> ExportPin(const UEdGraphPin* Pin) const;

    // Claim a graph the caller exports itself, the generic walk skips it.
    void ExcludeGraph(const UEdGraph* Graph);

private:

    TSharedPtr<FJsonObject> ExportGraphAtDepth(const UEdGraph* Graph, const TCHAR* GraphType, int32 SubGraphDepth);

    void AddSubGraphs(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj, int32 SubGraphDepth);

    FEdGraphJsonOptions m_Options;
    TSet<const UEdGraph*> m_VisitedGraphs;
};

namespace EdGraphJson
{
    // Per-class K2 node detail, plus the CallFunction and MacroInstance Args summary of
    // what an unlinked input pin actually passes.
    void AddK2NodeFields(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj);

    // Anim node asset, full settings, bindings and pin exposure, plus the reflection-only PropertyAccess path.
    // Not gated on UAnimGraphNode_Base. K2Node_PropertyAccess is a plain K2 node and that cast drops it.
    void AddAnimNodeFields(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj);

    // Dotted path of a K2Node_PropertyAccess, empty for any other node.
    FString GetPropertyAccessPath(const UEdGraphNode* Node);

    // One entry of the same Bindings map AddAnimNodeFields prints. A bound property is driven by the anim
    // instance and has no incoming wire, so a reader that follows links alone never sees it.
    bool GetPropertyBinding(const UEdGraphNode* Node, const FName PropertyName, FString& OutPath, FString& OutType);

    // The entry node owns the signature. Macros carry it on a UK2Node_Tunnel pair instead of a
    // FunctionEntry / FunctionResult pair. Overrides and interface impls have no UserDefinedPins, they
    // fall back to entry / result pins and report SignatureSource "Inherited".
    TSharedPtr<FJsonObject> ExportFunctionSignature(const UEdGraph* Graph, const UBlueprint* Blueprint, bool bInterfaceImplementation);
}
