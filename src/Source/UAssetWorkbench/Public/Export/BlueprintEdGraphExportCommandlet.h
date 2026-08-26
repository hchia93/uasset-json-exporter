#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BlueprintEdGraphExportCommandlet.generated.h"

class UBlueprint;
class UEdGraph;

// Exports Blueprint structure and CDO state to JSON: class identity, variables, event dispatchers,
// timelines, SCS components with their overrides, CDO deltas, graphs, and both reference directions.
// Graphs are lean by default, -graphs expands them to nodes and pins.
//   UnrealEditor-Cmd.exe Project.uproject -run=BlueprintEdGraphExport -assets="/Game/Path/BP_A" [-graphs]
// Contract: Docs/Export.md
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

    TSharedPtr<FJsonObject> ExportBlueprint(UBlueprint* Blueprint, const FExportOptions& Options) const;

    // Compare Instance properties against its class CDO, output differences
    void ExportPropertyOverrides(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    // Compare Instance properties against an explicit Reference object, output differences
    void ExportPropertyOverridesCompare(UObject* Instance, UObject* Reference, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    // Every non-transient, non-deprecated, non-default-subobject property with its resolved value, so a
    // reader compares field by field instead of deriving values back out of the delta entries.
    void ExportResolvedProperties(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const;

    FExportOptions ParseExportOptions(const FString& Params) const;
};
