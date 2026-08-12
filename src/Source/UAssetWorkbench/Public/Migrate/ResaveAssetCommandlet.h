#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ResaveAssetCommandlet.generated.h"

// Force-resaves assets so load-time fixups bake into the package on disk. Use after a CoreRedirect
// (class/property) so the redirected reference is serialized under its new name and the redirect can
// eventually be dropped, or to flush any load-time upgrade. Blueprints are compiled before saving.
//
// One operation per commandlet. Generic: asset paths are runtime args, nothing project-specific.
//
// Usage:
//   -run=ResaveAsset -assets="/Game/A,/Game/Maps/B" [-nocompile]
//   -nocompile: skip the Blueprint compile step (plain reserialize only).
UCLASS()
class UResaveAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UResaveAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    bool ResaveAsset(const FString& AssetPath, bool bCompile) const;
};
