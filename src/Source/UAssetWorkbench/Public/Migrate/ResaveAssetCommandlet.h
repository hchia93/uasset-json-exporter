#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ResaveAssetCommandlet.generated.h"

// Force-resaves assets so load-time fixups bake into the package, which is what lets a CoreRedirect be
// dropped afterwards. Blueprints are compiled before saving unless -nocompile says otherwise.
//   -run=ResaveAsset -assets="/Game/A,/Game/Maps/B" [-nocompile]
// Contract: Docs/Migrate.md
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
