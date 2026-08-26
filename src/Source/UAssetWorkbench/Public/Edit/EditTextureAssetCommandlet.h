#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditTextureAssetCommandlet.generated.h"

class FJsonObject;
class UTexture;

// Writes texture build settings from a spec: LODGroup, CompressionSettings, SRGB, MipGenSettings and the
// rest of the accepted set. Spec shape matches the AuditTexture report Spec block, so an audit can be fed
// straight back. Dry run does everything except the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditTextureAsset -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditTextureAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditTextureAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, bool bApply, TSet<UTexture*>& OutTouched, int32& OutOps) const;
};
