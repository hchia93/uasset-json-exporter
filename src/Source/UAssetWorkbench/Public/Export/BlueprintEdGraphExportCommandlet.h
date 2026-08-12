#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BlueprintEdGraphExportCommandlet.generated.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/*
 * Exports Blueprint structure + CDO state to JSON for external tooling consumption.
 *
 * Top-level fields (always emitted unless empty):
 *   ImplementedInterfaces[] / ImplementedInterfaceCount - BP "Implements Interface" list (Name, Path) (since 1.7.0)
 *   Variables / VariableCount / UserVariableCount       - generated class member properties
 *   Components[] (Name, Class, IsRoot, ParentName,      - SCS components with both:
 *       IsEditorOnly, PropertyOverrides, ResolvedProperties)
 *                                                          PropertyOverrides = delta vs component class CDO
 *                                                          ResolvedProperties = full resolved field dump (since 1.3.0)
 *   InheritedComponentOverrides[]                       - C++ default subobject deltas in BP CDO
 *   ActorCDOProperties[] / ActorCDOOverrides[]          - BP class CDO actor-level fields, full + delta vs parent (since 1.3.0)
 *   Graphs[]                                            - lean by default; -graphs expands to nodes/pins
 *   Referencers_Levels / Referencers_Other              - reverse asset deps split level vs non-level
 *   ReferencedAssets                                    - forward asset deps
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=BlueprintEdGraphExport -assets="/Game/Path/BP_A,/Game/Path/BP_B" [flags]
 *
 * Verbosity flags (default = lean, opt-in to expand):
 *   -graphs            include full graph node + pin details (otherwise only graph counts)
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UBlueprintEdGraphExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UBlueprintEdGraphExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    struct FExportOptions
    {
        bool bIncludeGraphs = false;
    };

    TSharedPtr<FJsonObject> ExportBlueprint(class UBlueprint* Blueprint, const FExportOptions& Options) const;
    TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph, const FExportOptions& Options) const;
    TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node) const;
    TSharedPtr<FJsonObject> ExportPin(const UEdGraphPin* Pin) const;

    // Compare Instance properties against its class CDO, output differences
    void ExportPropertyOverrides(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    // Compare Instance properties against an explicit Reference object, output differences
    void ExportPropertyOverridesCompare(UObject* Instance, UObject* Reference, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    // Dump every non-transient, non-deprecated, non-default-subobject property on Instance
    // with its resolved value. Lets external tools build a true field-by-field comparison
    // without needing to derive resolved values from delta entries.
    void ExportResolvedProperties(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    FExportOptions ParseExportOptions(const FString& Params) const;
};
