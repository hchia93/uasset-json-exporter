#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RedirectBlueprintPinCommandlet.generated.h"

// Moves a renamed delegate parameter's links from the stale output pin to the new one, then reconstructs
// the node to drop the stale pin. Only nodes carrying both pins are touched.
//   -run=RedirectBlueprintPin -OldPin="OldName" -NewPin="NewName" -assets="/Game/A,/Game/B" [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class URedirectBlueprintPinCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URedirectBlueprintPinCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns redirected node count for this Blueprint (0 = nothing to do).
    int32 RedirectBlueprintPins(const FString& AssetPath, FName OldPin, FName NewPin, bool bApply, bool& OutSaveFailed) const;
};
