#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ReparentBlueprintCommandlet.generated.h"

// Reparents a Blueprint after a class rename, so the CoreRedirect covering the old name can be dropped.
//   -run=ReparentBlueprint -blueprints="/Game/Path/BP_X" -oldclass="AOldActor" -newclass="ANewActor"
// Contract: Docs/Migrate.md
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
