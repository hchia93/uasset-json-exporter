#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RedirectBlueprintPinCommandlet.generated.h"

// Redirects a renamed output pin's links on Blueprint nodes, doing the Blueprint-side follow-up
// that CoreRedirects cannot. Renaming a delegate/event parameter (e.g. Progress -> Signal) leaves the
// bound-event handler with a stale old-named output pin still holding links, plus a fresh new-named pin.
// This commandlet finds nodes carrying BOTH pins, moves the links old -> new, and reconstructs the node
// to drop the stale pin.
//
// One operation per commandlet. Generic: pin names are runtime args, nothing project-specific.
//
// Usage:
//   -run=RedirectBlueprintPin -OldPin="OldName" -NewPin="NewName" -assets="/Game/A,/Game/B" [-apply]
//   Without -apply: dry run, reports only. With -apply: compile + save each modified Blueprint.
UCLASS()
class URedirectBlueprintPinCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URedirectBlueprintPinCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns redirected node count for this Blueprint (0 = nothing to do).
    int32 RedirectBlueprintPins(const FString& AssetPath, FName OldPin, FName NewPin, bool bApply) const;
};
