#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UAnimMontage;

// One montage, carried across every writer of a single target. The driver owns the asset, the cache
// rebuild and the save, each writer owns one facet, the same split the Blueprint side uses.
struct FMontageEditContext
{
    UAnimMontage* Montage = nullptr;
    FString AssetPath;
    bool bApply = false;

    int32 Ops = 0;
};

class IMontageWriter
{
public:
    virtual ~IMontageWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FMontageEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IMontageWriter> MakeMontageNotifyWriter();
