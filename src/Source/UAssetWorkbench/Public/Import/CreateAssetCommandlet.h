#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "CreateAssetCommandlet.generated.h"

class FJsonObject;
class UFactory;

// Creates assets of any type from a JSON spec. FactoryProperties configures the factory before creation
// because some types are only valid once it is, Properties lands on the asset after. Entries are created
// in order, so a later one can reference an earlier one by path. Never overwrites.
//   UnrealEditor-Cmd.exe Project.uproject -run=CreateAsset -spec="C:/path/spec.json" -unattended
// Contract: Docs/Import.md
UCLASS()
class UCreateAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UCreateAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Skipped is not Created. Reporting an existing asset as created is how a caller ends up
    // believing a run did something it did not.
    enum class EOutcome : uint8
    {
        Created,
        Skipped,
        Failed
    };

    EOutcome CreateOne(const TSharedPtr<FJsonObject>& Entry) const;

    static UClass* ResolveAssetClass(const FString& ClassName);

    // Engine ships no lookup from asset class to factory, this walks every concrete factory once.
    static UFactory* ResolveFactory(UClass* AssetClass);
};
