#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DeleteBlueprintNodeCommandlet.generated.h"

// Deletes Blueprint graph nodes by node id, the cleanup step after graph logic moves into C++. Deleting
// a node cuts every link it held, the surrounding graph is not rewired and its variables are not touched.
//   -run=DeleteBlueprintNode -nodes="GUID,GUID" -assets="/Game/A,/Game/B" [-apply]
// Contract: Docs/Migrate.md
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
    int32 DeleteBlueprintNodes(const FString& AssetPath, const TSet<FGuid>& NodeGuids, bool bApply, TSet<FGuid>& OutMatched, bool& OutSaveFailed) const;
};
