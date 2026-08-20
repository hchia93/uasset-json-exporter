#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DuplicateAssetCommandlet.generated.h"

// Copies assets to new paths. The copy is independent, references inside it still point at whatever
// the original pointed at, nothing is retargeted.
//
// Useful as the first step of a risky edit: duplicate, operate on the copy, compare, then decide
// whether to repeat on the original.
//
// Refuses to overwrite. A destination that already exists is an error, so a re-run cannot silently
// discard a copy that was already worked on.
//
// One operation per commandlet. Generic: paths are runtime args, nothing project-specific.
//
// Usage:
//   -run=DuplicateAsset -pairs="/Game/A>/Game/B,/Game/C>/Game/Sub/D" [-apply]
//   Without -apply: dry run, reports only. With -apply: save each new asset.
//
// Exit codes: 0 success, 1 bad args / missing source / destination exists, 2 live editor conflict.
UCLASS()
class UDuplicateAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UDuplicateAssetCommandlet();

    virtual int32 Main(const FString& Params) override;
};
