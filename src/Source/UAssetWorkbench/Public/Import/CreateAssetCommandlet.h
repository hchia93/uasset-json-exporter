#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "CreateAssetCommandlet.generated.h"

class FJsonObject;
class UFactory;

/*
 * Creates assets of any type from a JSON spec.
 *
 * Two property blocks, because some types are only valid if their factory was configured first.
 * FactoryProperties is applied to the factory before creation (DataTable needs Struct, Blueprint needs
 * ParentClass plus bSkipClassPicker, Texture2D needs Width and Height or it silently yields nothing).
 * Properties is applied to the asset after creation.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=CreateAsset -spec="C:/path/spec.json" -unattended
 *
 * Spec:
 *   {
 *     "Assets": [
 *       {
 *         "PackagePath": "/Game/Path",
 *         "AssetName": "DT_Foo",
 *         "Class": "/Script/Engine.DataTable",
 *         "FactoryProperties": { "Struct": "/Script/MyModule.MyRow" },
 *         "Properties": {}
 *       }
 *     ]
 *   }
 *
 * Entries are created in order, so a later one can reference an earlier one by its resulting path.
 * An existing asset at the same path is left alone and reported, this never overwrites.
 *
 * -unattended is required. FMessageDialog does not check for commandlet mode, some paths would block.
 */
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
