#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace AnimAssetEdit
{
    void AddMissingNotifyTracks(UAnimSequenceBase* AnimAsset, int32 TrackIndex)
    {
#if WITH_EDITORONLY_DATA
        while (!AnimAsset->AnimNotifyTracks.IsValidIndex(TrackIndex))
        {
            const FName TrackName = *FString::FromInt(AnimAsset->AnimNotifyTracks.Num() + 1);
            AnimAsset->AnimNotifyTracks.Add(FAnimNotifyTrack(TrackName, FLinearColor::White));
        }
#endif // WITH_EDITORONLY_DATA
    }

    bool ReadTrackRequest(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FTrackRequest& OutRequest)
    {
        const TSharedPtr<FJsonValue> Field = Desc->TryGetField(TEXT("Track"));
        if (!Field.IsValid())
        {
            return true;
        }

        OutRequest.bPresent = true;

        double Number = 0.0;
        if (Field->TryGetNumber(Number))
        {
            OutRequest.Index = static_cast<int32>(Number);
            if (OutRequest.Index < 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Track %d is negative"), *Context.AssetPath, OutRequest.Index);
                return false;
            }

            return true;
        }

        FString Text;
        if (!Field->TryGetString(Text))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Track must be a row index, a row name or \"auto\""), *Context.AssetPath);
            return false;
        }

        if (Text.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
        {
            OutRequest.bAuto = true;
            return true;
        }

#if WITH_EDITORONLY_DATA
        TArray<FString> Names;
        for (int32 Index = 0; Index < Context.AnimAsset->AnimNotifyTracks.Num(); ++Index)
        {
            const FName TrackName = Context.AnimAsset->AnimNotifyTracks[Index].TrackName;
            if (TrackName.ToString() == Text)
            {
                OutRequest.Index = Index;
                return true;
            }

            Names.Add(FString::Printf(TEXT("[%d] %s"), Index, *TrackName.ToString()));
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no notify track named '%s'. Present: %s"), *Context.AssetPath, *Text, *FString::Join(Names, TEXT(", ")));
#endif // WITH_EDITORONLY_DATA
        return false;
    }

    UAnimMontage* RequireMontage(const FAnimAssetEditContext& Context, const TCHAR* SpecKey)
    {
        UAnimMontage* Montage = Cast<UAnimMontage>(Context.AnimAsset);
        if (!Montage)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a montage, it carries no %s"), *Context.AssetPath, SpecKey);
        }

        return Montage;
    }

    UAnimSequence* RequireSequence(const FAnimAssetEditContext& Context, const TCHAR* SpecKey)
    {
        UAnimSequence* Sequence = Cast<UAnimSequence>(Context.AnimAsset);
        if (!Sequence)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an anim sequence, it carries no %s"), *Context.AssetPath, SpecKey);
        }

        return Sequence;
    }
}
