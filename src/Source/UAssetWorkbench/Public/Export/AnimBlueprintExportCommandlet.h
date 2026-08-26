#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AnimBlueprintExportCommandlet.generated.h"

class FEdGraphJsonSerializer;
class UAnimationStateMachineGraph;
class UAnimBlueprint;
class UAnimGraphNode_StateMachineBase;
class UEdGraph;
class UEdGraphNode;

// Exports AnimBlueprint structure to JSON: EdGraph, state machines with their states and transitions.
//   UnrealEditor-Cmd.exe Project.uproject -run=AnimBlueprintExport -assets="/Game/Path/ABP_A,/Game/Path/ABP_B"
// Contract: Docs/Export.md
UCLASS()
class UAnimBlueprintExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAnimBlueprintExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // One state machine node, plus the machine and state that reach it when it is nested inside a state.
    struct FStateMachineEntry
    {
        UAnimGraphNode_StateMachineBase* Node = nullptr;
        FString ParentMachine;
        FString ParentState;
    };

    TSharedPtr<FJsonObject> ExportAnimBlueprint(UAnimBlueprint* AnimBP) const;

    // Every state machine in Graph, then every one nested in a state below it. Each machine graph is
    // claimed on the serializer, otherwise the generic graph walk emits its nodes a second time.
    void CollectStateMachines(const UEdGraph* Graph, const FString& ParentMachine, const FString& ParentState, FEdGraphJsonSerializer& Serializer, TArray<FStateMachineEntry>& OutEntries) const;

    // AnimGraph / AnimLayer / Function. OutInterface names the anim layer interface a layer conforms to, if any.
    const TCHAR* ClassifyFunctionGraph(const UAnimBlueprint* AnimBP, const UEdGraph* Graph, const UClass*& OutInterface) const;

    // Layer output group, carried on the graph's root node.
    FName FindLayerGroup(const UEdGraph* Graph) const;

    // Event hooks bound on one node, merged into OutBindings.
    void CollectNodeEventBindings(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& OutBindings) const;

    // A state's hooks, gathered from the state node and the result node in its bound graph.
    TSharedPtr<FJsonObject> ExportStateEventBindings(const class UAnimStateNode* StateNode) const;

    // A transition's hooks, gathered from the transition node and the result node in its rule graph.
    TSharedPtr<FJsonObject> ExportTransitionEventBindings(const class UAnimStateTransitionNode* TransNode) const;

    // Names the shape that gates a transition, the full rule graph is exported beside it.
    TSharedPtr<FJsonObject> ExportTransitionRuleSummary(const UEdGraph* RuleGraph) const;

    TSharedPtr<FJsonObject> ExportStateMachine(UAnimationStateMachineGraph* SMGraph, FEdGraphJsonSerializer& Serializer) const;

};
