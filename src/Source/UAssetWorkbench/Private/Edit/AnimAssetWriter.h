#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UAnimMontage;
class UAnimSequence;
class UAnimSequenceBase;

// One anim asset, carried across every writer of a single target. The driver owns the asset, the cache
// rebuild and the save, each writer owns one facet, the same split the Blueprint side uses.
struct FAnimAssetEditContext
{
    UAnimSequenceBase* AnimAsset = nullptr;
    FString AssetPath;

    int32 Ops = 0;
};

class IAnimAssetWriter
{
public:
    virtual ~IAnimAssetWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FAnimAssetEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IAnimAssetWriter> MakeAnimNotifyWriter();
TUniquePtr<IAnimAssetWriter> MakeAnimCurveWriter();
TUniquePtr<IAnimAssetWriter> MakeAnimSyncMarkerWriter();
TUniquePtr<IAnimAssetWriter> MakeAnimMontageSectionWriter();
TUniquePtr<IAnimAssetWriter> MakeAnimMontageSlotWriter();

namespace AnimAssetEdit
{
    // Authored times carry a snap offset of about KINDA_SMALL_NUMBER, so a time copied out of an export
    // never compares equal to the authored one. A millisecond apart is still the same element.
    constexpr float kTimeTolerance = 1.0e-3f;

    // The panel builds its rows from AnimNotifyTracks, and sync markers share those rows with notifies,
    // so an element on a row the asset never declared lands nowhere visible until the cache patches it.
    void AddMissingNotifyTracks(UAnimSequenceBase* AnimAsset, int32 TrackIndex);

    // Track takes a row index, a row name from NotifyTracks, or "auto" for the caller to place it.
    struct FTrackRequest
    {
        bool bPresent = false;
        bool bAuto = false;
        int32 Index = 0;
    };

    bool ReadTrackRequest(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FTrackRequest& OutRequest);

    // Montage composition on a plain sequence is a spec bug, not a no-op.
    UAnimMontage* RequireMontage(const FAnimAssetEditContext& Context, const TCHAR* SpecKey);

    // Sync markers live on UAnimSequence, a montage samples them from the sequences it plays.
    UAnimSequence* RequireSequence(const FAnimAssetEditContext& Context, const TCHAR* SpecKey);
}
