#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditMaterialAssetCommandlet.generated.h"

class FJsonObject;
class FMaterialUpdateContext;
class UMaterial;
class UMaterialInstanceConstant;

// Writes material settings from a spec: usage flags, blend mode, domain, shading model and two sided on a
// Material, parent and parameter overrides on a MaterialInstanceConstant. Spec shape matches the
// AuditMaterial report Spec block. Node graph edits are out of scope. Dry run skips only the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditMaterialAsset -spec="C:/path/spec.json" [-apply]
// See Docs/Edit.md.
UCLASS()
class UEditMaterialAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditMaterialAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, FMaterialUpdateContext& UpdateContext, TSet<UObject*>& OutTouched, int32& OutOps) const;

    bool ApplyMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Properties, FMaterialUpdateContext& UpdateContext, int32& OutOps) const;
    bool ApplyMaterialInstance(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const;
};
