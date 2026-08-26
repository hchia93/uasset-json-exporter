#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AnimAssetExportCommandlet.generated.h"

struct FAnimNotifyEvent;

class UAnimMontage;
class UAnimSequence;
class UAnimSequenceBase;

// Exports an anim asset to JSON. Both kinds carry playback settings, curves and notify tracks, a montage
// adds sections, slot tracks and blend settings, a sequence adds root motion, additive and compression
// settings plus its sync markers.
//   UnrealEditor-Cmd.exe Project.uproject -run=AnimAssetExport -assets="/Game/Path/AM_A,/Game/Path/AM_B"
// Contract: Docs/Export.md
UCLASS()
class UAnimAssetExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAnimAssetExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportAnimAsset(UAnimSequenceBase* AnimAsset) const;
    void ExportSequenceSettings(const UAnimSequence* Sequence, const TSharedRef<FJsonObject>& Root) const;
    void ExportMontageBlend(const UAnimMontage* Montage, const TSharedRef<FJsonObject>& Root) const;
    void ExportCurves(const UAnimSequenceBase* AnimAsset, const TSharedRef<FJsonObject>& Root) const;
    void ExportNotifyTracks(const UAnimSequenceBase* AnimAsset, const TSharedRef<FJsonObject>& Root) const;
    TSharedPtr<FJsonObject> ExportNotify(const FAnimNotifyEvent& NotifyEvent) const;
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const;

};
