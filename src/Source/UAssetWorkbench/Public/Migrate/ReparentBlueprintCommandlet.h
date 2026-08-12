#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ReparentBlueprintCommandlet.generated.h"

// Reparents a Blueprint after a class rename. Fixes BP parent class references that were hardcoded
// to the old class name before CoreRedirect was added, allowing the redirect to be safely removed later.
// Only modifies the specified blueprints, no side effects on other assets.
//
// Usage:
//   -run=ReparentBlueprint -blueprints="/Game/Path/BP_X" -oldclass="AOldClass" -newclass="ANewClass"
//   -blueprints: comma-separated list of BP package paths to reparent
//   -oldclass: full class name to search for (e.g., "AMyOldActor")
//   -newclass: full class name to reparent to (e.g., "AMyNewActor")
UCLASS()
class UReparentBlueprintCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UReparentBlueprintCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    bool ReparentBlueprint(const FString& BlueprintPath, const FString& OldClassName, const FString& NewClassName) const;
};
