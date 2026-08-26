#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    // Handles stay legible on the panel with roughly this much clear time either side of a marker.
    constexpr float kTrackClearance = 0.05f;

    // First row carrying no marker within kTrackClearance, appending a row when every row is taken.
    // IgnoreIndex keeps a marker being moved from colliding with itself.
    int32 FindClearMarkerTrack(const UAnimSequence* Sequence, float Time, int32 IgnoreIndex)
    {
#if WITH_EDITORONLY_DATA
        const int32 TrackCount = FMath::Max(Sequence->AnimNotifyTracks.Num(), 1);

        for (int32 TrackIndex = 0; TrackIndex < TrackCount; ++TrackIndex)
        {
            bool bTaken = false;
            for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
            {
                const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
                if (Index == IgnoreIndex || Marker.TrackIndex != TrackIndex)
                {
                    continue;
                }

                if (FMath::Abs(Marker.Time - Time) <= kTrackClearance)
                {
                    bTaken = true;
                    break;
                }
            }

            if (!bTaken)
            {
                return TrackIndex;
            }
        }

        return TrackCount;
#else
        return 0;
#endif // WITH_EDITORONLY_DATA
    }

    // An unresolvable address is the most common spec error, so failures print what was there.
    FString DescribeMarkers(const UAnimSequence* Sequence)
    {
        TArray<FString> Lines;
        for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
        {
            const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
#if WITH_EDITORONLY_DATA
            Lines.Add(FString::Printf(TEXT("[%d] %s @ %.4f track %d"), Index, *Marker.MarkerName.ToString(), Marker.Time, Marker.TrackIndex));
#else
            Lines.Add(FString::Printf(TEXT("[%d] %s @ %.4f"), Index, *Marker.MarkerName.ToString(), Marker.Time));
#endif // WITH_EDITORONLY_DATA
        }

        return FString::Join(Lines, TEXT(", "));
    }

    class FAnimSyncMarkerWriter : public IAnimAssetWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("SyncMarkers");
        }

        virtual bool Apply(FAnimAssetEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: SyncMarkers must be an array of ops"), *Context.AssetPath);
                return false;
            }

            UAnimSequence* Sequence = AnimAssetEdit::RequireSequence(Context, GetSpecKey());
            if (!Sequence)
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: SyncMarkers carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                FString Op;
                if (!Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: sync marker entry has no Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Sequence, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
            }

            return true;
        }

    private:
        bool ApplyOp(FAnimAssetEditContext& Context, UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Desc, const FString& Op) const
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Sequence, Desc);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, Sequence, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Sequence, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown sync marker op '%s'. Expected Add, Modify or Remove"), *Context.AssetPath, *Op);
            return false;
        }

        bool ResolveTime(const FAnimAssetEditContext& Context, double Time) const
        {
            const float PlayLength = Context.AnimAsset->GetPlayLength();
            const bool bInRange = Time >= 0.0 && Time <= PlayLength;
            if (!bInRange)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: time %.4f is outside the sequence (0 .. %.4f)"), *Context.AssetPath, Time, PlayLength);
            }

            return bInRange;
        }

        bool ResolveMarkerIndex(const FAnimAssetEditContext& Context, const UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Desc, int32& OutIndex) const
        {
            const TArray<FAnimSyncMarker>& Markers = Sequence->AuthoredSyncMarkers;

            int32 Index = INDEX_NONE;
            if (Desc->TryGetNumberField(TEXT("Index"), Index))
            {
                if (!Markers.IsValidIndex(Index))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no sync marker at index %d. Present: %s"), *Context.AssetPath, Index, *DescribeMarkers(Sequence));
                    return false;
                }

                OutIndex = Index;
                return true;
            }

            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: sync marker op needs Name or Index to address a marker"), *Context.AssetPath);
                return false;
            }

            double At = 0.0;
            const bool bHasAt = Desc->TryGetNumberField(TEXT("At"), At);

            TArray<int32> Matches;
            for (int32 Candidate = 0; Candidate < Markers.Num(); ++Candidate)
            {
                const FAnimSyncMarker& Marker = Markers[Candidate];
                const bool bNameMatches = Marker.MarkerName.ToString() == Name;
                const bool bTimeMatches = !bHasAt || FMath::IsNearlyEqual(Marker.Time, static_cast<float>(At), AnimAssetEdit::kTimeTolerance);
                if (bNameMatches && bTimeMatches)
                {
                    Matches.Add(Candidate);
                }
            }

            if (Matches.Num() == 1)
            {
                OutIndex = Matches[0];
                return true;
            }

            if (Matches.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no sync marker matching '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeMarkers(Sequence));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' matches %d sync markers, narrow it with At. Present: %s"), *Context.AssetPath, *Name, Matches.Num(), *DescribeMarkers(Sequence));
            return false;
        }

        bool ApplyAdd(FAnimAssetEditContext& Context, UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name) || Name.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs Name"), *Context.AssetPath);
                return false;
            }

            double Time = 0.0;
            if (!Desc->TryGetNumberField(TEXT("Time"), Time))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs Time"), *Context.AssetPath);
                return false;
            }

            if (!ResolveTime(Context, Time))
            {
                return false;
            }

            AnimAssetEdit::FTrackRequest TrackRequest;
            if (!AnimAssetEdit::ReadTrackRequest(Context, Desc, TrackRequest))
            {
                return false;
            }

            const int32 Track = TrackRequest.bAuto ? FindClearMarkerTrack(Sequence, static_cast<float>(Time), INDEX_NONE) : TrackRequest.Index;

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add sync marker '%s' @ %.4f track %d"), *Sequence->GetName(), *Name, Time, Track);

            Sequence->Modify();
            AnimAssetEdit::AddMissingNotifyTracks(Sequence, Track);

            FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef();
            Marker.MarkerName = FName(*Name);
            Marker.Time = static_cast<float>(Time);
#if WITH_EDITORONLY_DATA
            Marker.TrackIndex = Track;
            Marker.Guid = FGuid::NewGuid();
#endif // WITH_EDITORONLY_DATA

            return true;
        }

        bool ApplyModify(FAnimAssetEditContext& Context, UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Desc) const
        {
            int32 Index = INDEX_NONE;
            if (!ResolveMarkerIndex(Context, Sequence, Desc, Index))
            {
                return false;
            }

            const bool bHasNewName = Desc->HasField(TEXT("NewName"));
            const bool bHasTime = Desc->HasField(TEXT("Time"));

            AnimAssetEdit::FTrackRequest TrackRequest;
            if (!AnimAssetEdit::ReadTrackRequest(Context, Desc, TrackRequest))
            {
                return false;
            }

            if (!bHasNewName && !bHasTime && !TrackRequest.bPresent)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Modify writes nothing. Expected NewName, Time or Track"), *Context.AssetPath);
                return false;
            }

            const FAnimSyncMarker& Current = Sequence->AuthoredSyncMarkers[Index];

            double Time = Current.Time;
            if (bHasTime)
            {
                Desc->TryGetNumberField(TEXT("Time"), Time);
                if (!ResolveTime(Context, Time))
                {
                    return false;
                }
            }

            FString NewName;
            if (bHasNewName && (!Desc->TryGetStringField(TEXT("NewName"), NewName) || NewName.IsEmpty()))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: NewName must be a non-empty string"), *Context.AssetPath);
                return false;
            }

#if WITH_EDITORONLY_DATA
            int32 Track = Current.TrackIndex;
#else
            int32 Track = 0;
#endif // WITH_EDITORONLY_DATA
            if (TrackRequest.bPresent)
            {
                Track = TrackRequest.bAuto ? FindClearMarkerTrack(Sequence, static_cast<float>(Time), Index) : TrackRequest.Index;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: modify sync marker '%s' @ %.4f track %d"), *Sequence->GetName(), *Current.MarkerName.ToString(), Time, Track);

            Sequence->Modify();
            AnimAssetEdit::AddMissingNotifyTracks(Sequence, Track);

            FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
            if (bHasNewName)
            {
                Marker.MarkerName = FName(*NewName);
            }

            Marker.Time = static_cast<float>(Time);
#if WITH_EDITORONLY_DATA
            Marker.TrackIndex = Track;
#endif // WITH_EDITORONLY_DATA

            return true;
        }

        bool ApplyRemove(FAnimAssetEditContext& Context, UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Desc) const
        {
            int32 Index = INDEX_NONE;
            if (!ResolveMarkerIndex(Context, Sequence, Desc, Index))
            {
                return false;
            }

            const FAnimSyncMarker& Marker = Sequence->AuthoredSyncMarkers[Index];
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove sync marker '%s' @ %.4f"), *Sequence->GetName(), *Marker.MarkerName.ToString(), Marker.Time);

            Sequence->Modify();
            Sequence->AuthoredSyncMarkers.RemoveAt(Index);
            return true;
        }
    };
}

TUniquePtr<IAnimAssetWriter> MakeAnimSyncMarkerWriter()
{
    return MakeUnique<FAnimSyncMarkerWriter>();
}
