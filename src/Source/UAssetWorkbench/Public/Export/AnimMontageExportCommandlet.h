#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AnimMontageExportCommandlet.generated.h"

struct FAnimNotifyEvent;

/*
 * Exports AnimMontage structure (sections, slots, ANS/AN placement with properties) to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=AnimMontageExport -assets="/Game/Path/AM_A,/Game/Path/AM_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UAnimMontageExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAnimMontageExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportMontage(class UAnimMontage* Montage) const;
    TSharedPtr<FJsonObject> ExportNotify(const FAnimNotifyEvent& NotifyEvent) const;
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const;

};
