#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DataAssetImportCommandlet.generated.h"

class FJsonObject;

/*
 * Writes properties into a DataAsset from a JSON spec, the inverse of DataAssetExport.
 *
 * Property values take either form. A string goes through ImportText, which is what DataAssetExport
 * writes, so an exported asset round-trips. An object or array goes through the json converter, which
 * is what a hand-written spec wants for nested structs and arrays.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=DataAssetImport -spec="C:/path/spec.json"
 *
 * Spec:
 *   {
 *     "AssetPath": "/Game/Path/DA_Foo",
 *     "Properties": {
 *       "Scalar": "12.0",
 *       "Nested": [ { "Name": "A" }, { "Name": "B" } ]
 *     }
 *   }
 *
 * Only the named properties are touched, everything else on the asset keeps its current value.
 */
UCLASS()
class UDataAssetImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UDataAssetImportCommandlet();

    virtual int32 Main(const FString& Params) override;
};
