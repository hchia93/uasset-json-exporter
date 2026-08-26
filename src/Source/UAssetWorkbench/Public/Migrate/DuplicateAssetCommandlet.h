#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DuplicateAssetCommandlet.generated.h"

// Copies assets to new paths. The copy is independent, references inside it still point at whatever the
// original pointed at. Refuses to overwrite, so a re-run cannot discard a copy already worked on.
//   -run=DuplicateAsset -pairs="/Game/A>/Game/B,/Game/C>/Game/Sub/D" [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class UDuplicateAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UDuplicateAssetCommandlet();

    virtual int32 Main(const FString& Params) override;
};
