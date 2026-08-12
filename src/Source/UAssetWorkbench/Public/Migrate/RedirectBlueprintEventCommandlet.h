#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RedirectBlueprintEventCommandlet.generated.h"

// Redirects a Blueprint event override to a renamed C++ member, doing the Blueprint-side follow-up
// that CoreRedirects cannot. Renaming an interface BlueprintNativeEvent/BlueprintImplementableEvent
// (or a parent-class event) orphans the BP override into a dead custom event that no longer fires.
// This commandlet finds that orphan by its old name and rewires it to the new event, moving pin links.
//
// One operation per commandlet. Generic: the owner class / event names are runtime args, nothing project-specific.
//
// Usage:
//   -run=RedirectBlueprintEvent -OwnerClass="/Script/Module.NewOwnerClass"
//        -OldEvent="OldEventName" -NewEvent="NewEventName" -assets="/Game/A,/Game/B" [-apply]
//   Without -apply: dry run, reports only. With -apply: compile + save each modified Blueprint.
UCLASS()
class URedirectBlueprintEventCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URedirectBlueprintEventCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns redirected node count for this Blueprint (0 = nothing to do or not found).
    int32 RedirectBlueprintEvents(const FString& AssetPath, UClass* OwnerClass, FName OldEvent, FName NewEvent, bool bApply) const;
};
