#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RedirectBlueprintEventCommandlet.generated.h"

// Rewires a Blueprint event override orphaned by a C++ rename, the Blueprint-side follow-up CoreRedirects
// cannot do. Finds the dead custom event by its old name and moves its pin links onto the new event.
//   -run=RedirectBlueprintEvent -OwnerClass= -OldEvent= -NewEvent= -assets="/Game/A,/Game/B" [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class URedirectBlueprintEventCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URedirectBlueprintEventCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns redirected node count for this Blueprint (0 = nothing to do or not found).
    int32 RedirectBlueprintEvents(const FString& AssetPath, UClass* OwnerClass, FName OldEvent, FName NewEvent, bool bApply, bool& OutSaveFailed) const;
};
