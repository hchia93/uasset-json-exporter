#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditAnimAssetCommandlet.generated.h"

class FJsonObject;
class UAnimSequenceBase;

// Edits an anim sequence or montage through one writer per facet, the way EditBlueprint splits a
// Blueprint. One target loads the asset once, runs every writer the spec names, rebuilds the notify
// cache and saves once. A writer that fails aborts the target, so an asset never lands half-edited.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditAnimAsset -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditAnimAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditAnimAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the montage unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UAnimSequenceBase*>& OutTouched, int32& OutOps) const;
};
