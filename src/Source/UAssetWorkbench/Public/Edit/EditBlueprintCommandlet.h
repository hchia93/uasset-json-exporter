#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditBlueprintCommandlet.generated.h"

class FJsonObject;

// Edits a Blueprint through one writer per facet, split the way the editor's Blueprint diff splits it.
// One target loads the asset once, runs every writer the spec names in a fixed order, then compiles
// and saves once. A writer that fails aborts the target, so a Blueprint never lands half-edited.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditBlueprint -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditBlueprintCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditBlueprintCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the Blueprint unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TMap<UBlueprint*, bool>& OutTouched, int32& OutOps) const;
};
