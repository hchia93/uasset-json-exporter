#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AnimBlueprintExportCommandlet.generated.h"

class UAnimationStateMachineGraph;
class UAnimGraphNode_Base;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/*
 * Exports AnimBlueprint structure (EdGraph, StateMachines with states/transitions) to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=AnimBlueprintExport -assets="/Game/Path/ABP_A,/Game/Path/ABP_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UAnimBlueprintExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAnimBlueprintExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportAnimBlueprint(class UAnimBlueprint* AnimBP) const;
    TSharedPtr<FJsonObject> ExportStateMachine(UAnimationStateMachineGraph* SMGraph) const;

    // EdGraph export
    TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph) const;
    TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node) const;
    TSharedPtr<FJsonObject> ExportPin(const UEdGraphPin* Pin) const;

    // Anim node struct fields that differ from the struct defaults, null when everything is default
    TSharedPtr<FJsonObject> ExportAnimNodeSettings(const UAnimGraphNode_Base* AnimNode) const;

    // Property Access node ships behind a private header in a Developer plugin, reflection is the only route in
    void AddPropertyAccessPath(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj) const;

};
