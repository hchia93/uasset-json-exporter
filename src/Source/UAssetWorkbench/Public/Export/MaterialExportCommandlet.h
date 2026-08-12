#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "MaterialExportCommandlet.generated.h"

class UMaterialExpression;

/*
 * Exports Material node graph and MaterialInstance parameter overrides to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=MaterialExport -assets="/Game/Path/M_A,/Game/Path/MI_B"
 *
 * Accepts both UMaterial and UMaterialInstance assets.
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UMaterialExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UMaterialExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportMaterial(class UMaterial* Material) const;
    TSharedPtr<FJsonObject> ExportMaterialInstance(class UMaterialInstance* MaterialInstance) const;
    TSharedPtr<FJsonObject> ExportExpression(UMaterialExpression* Expression) const;

};
