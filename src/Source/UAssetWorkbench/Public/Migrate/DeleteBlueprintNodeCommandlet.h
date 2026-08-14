#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DeleteBlueprintNodeCommandlet.generated.h"

// Deletes Blueprint graph nodes by node id, the cleanup step after graph logic moves into C++.
// Ids come straight from BlueprintEdGraphExport's -graphs output, so the selection is exact instead
// of name-matched: export first, read the ids off the nodes to cut, pass them here.
//
// Deleting a node cuts every link it held, the surrounding graph is not rewired. Nodes the schema
// refuses to delete (function entry, result) are reported and skipped. Ids matching nothing are
// reported at the end, a typo never passes silently.
//
// Blueprint variables left unreferenced by the deletion are not touched, remove them separately.
//
// One operation per commandlet. Generic: node ids are runtime args, nothing project-specific.
//
// Usage:
//   -run=DeleteBlueprintNode -nodes="GUID,GUID" -assets="/Game/A,/Game/B" [-apply]
//   Without -apply: dry run, reports only. With -apply: compile + save each modified Blueprint.
//
// Exit codes: 0 success, 1 bad args / unparseable id / no assets, 2 live editor conflict.
UCLASS()
class UDeleteBlueprintNodeCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UDeleteBlueprintNodeCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns deleted node count for this Blueprint (0 = nothing matched). Every id found lands in
    // OutMatched whether or not it was deletable, so the caller can report the ids that hit nothing.
    int32 DeleteBlueprintNodes(const FString& AssetPath, const TSet<FGuid>& NodeGuids, bool bApply, TSet<FGuid>& OutMatched) const;
};
