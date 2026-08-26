#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DataAssetImportCommandlet.generated.h"

class FJsonObject;

// Writes properties into a DataAsset from a JSON spec, the inverse of DataAssetExport. Only the named
// properties are touched, everything else on the asset keeps its current value.
//   UnrealEditor-Cmd.exe Project.uproject -run=DataAssetImport -spec="C:/path/spec.json"
// Contract: Docs/Import.md
UCLASS()
class UDataAssetImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UDataAssetImportCommandlet();

    virtual int32 Main(const FString& Params) override;
};
