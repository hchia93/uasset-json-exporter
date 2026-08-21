#include "Edit/MontageWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectIterator.h"

namespace
{
    // Trigger times carry an offset of about KINDA_SMALL_NUMBER, so a time copied out of an export
    // never compares equal to the authored one. A millisecond apart is still the same notify.
    constexpr float kTimeTolerance = 1.0e-3f;

    UClass* ResolveNotifyClass(const FString& ClassPath)
    {
        if (UClass* Found = LoadClass<UObject>(nullptr, *ClassPath))
        {
            return Found;
        }

        // A Blueprint notify named by its asset path resolves only through the generated class.
        if (!ClassPath.Contains(TEXT(".")))
        {
            FString AssetName;
            ClassPath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            const FString GeneratedPath = FString::Printf(TEXT("%s.%s_C"), *ClassPath, *AssetName);
            if (UClass* Generated = LoadClass<UObject>(nullptr, *GeneratedPath))
            {
                return Generated;
            }
        }

        // Bare name fallback, so a spec can say "AnimNotify_PlaySound" without the /Script prefix.
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            const bool bIsNotify = Candidate->IsChildOf(UAnimNotify::StaticClass());
            const bool bIsNotifyState = Candidate->IsChildOf(UAnimNotifyState::StaticClass());
            if ((bIsNotify || bIsNotifyState) && Candidate->GetName() == ClassPath)
            {
                return Candidate;
            }
        }

        return nullptr;
    }

    // The name the notify panel stores on creation, not GetNotifyName(), which PlaySound and friends
    // override to display a parameter value instead of anything that would address the notify again.
    FString MakeDefaultNotifyName(const UClass* NotifyClass)
    {
        FString Name = NotifyClass->GetName();
        Name.ReplaceInline(TEXT("AnimNotify_"), TEXT(""), ESearchCase::CaseSensitive);
        Name.ReplaceInline(TEXT("AnimNotifyState_"), TEXT(""), ESearchCase::CaseSensitive);
        return Name;
    }

    // The panel builds its rows from AnimNotifyTracks, so a notify on a track the montage never
    // declared lands nowhere visible until RefreshCacheData patches the index back to 0.
    void AddMissingNotifyTracks(UAnimMontage* Montage, int32 TrackIndex)
    {
#if WITH_EDITORONLY_DATA
        while (!Montage->AnimNotifyTracks.IsValidIndex(TrackIndex))
        {
            const FName TrackName = *FString::FromInt(Montage->AnimNotifyTracks.Num() + 1);
            Montage->AnimNotifyTracks.Add(FAnimNotifyTrack(TrackName, FLinearColor::White));
        }
#endif // WITH_EDITORONLY_DATA
    }

    // An unresolvable address is the most common spec error, so failures print what was there.
    FString DescribeNotifies(const UAnimMontage* Montage)
    {
        TArray<FString> Lines;
        for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
        {
            const FAnimNotifyEvent& Event = Montage->Notifies[Index];
            Lines.Add(FString::Printf(TEXT("[%d] %s @ %.4f track %d"), Index, *Event.NotifyName.ToString(), Event.GetTriggerTime(), Event.TrackIndex));
        }

        return FString::Join(Lines, TEXT(", "));
    }

    UObject* GetNotifyObject(const FAnimNotifyEvent& Event)
    {
        if (Event.NotifyStateClass)
        {
            return Event.NotifyStateClass;
        }

        return Event.Notify;
    }

    class FMontageNotifyWriter : public IMontageWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Notifies");
        }

        virtual bool Apply(FMontageEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Notifies must be an array of ops"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Notifies carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                FString Op;
                if (!Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: notify entry has no Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
            }

            return true;
        }

    private:
        bool ApplyOp(FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Op) const
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Desc);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown notify op '%s'. Expected Add, Modify or Remove"), *Context.AssetPath, *Op);
            return false;
        }

        bool ResolveTime(const FMontageEditContext& Context, double Time) const
        {
            const float PlayLength = Context.Montage->GetPlayLength();
            const bool bInRange = Time >= 0.0 && Time <= PlayLength;
            if (!bInRange)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: time %.4f is outside the montage (0 .. %.4f)"), *Context.AssetPath, Time, PlayLength);
            }

            return bInRange;
        }

        bool ResolveNotifyIndex(const FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc, int32& OutIndex) const
        {
            const TArray<FAnimNotifyEvent>& Notifies = Context.Montage->Notifies;

            int32 Index = INDEX_NONE;
            if (Desc->TryGetNumberField(TEXT("Index"), Index))
            {
                if (!Notifies.IsValidIndex(Index))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no notify at index %d. Present: %s"), *Context.AssetPath, Index, *DescribeNotifies(Context.Montage));
                    return false;
                }

                OutIndex = Index;
                return true;
            }

            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: notify op needs Name or Index to address a notify"), *Context.AssetPath);
                return false;
            }

            double At = 0.0;
            const bool bHasAt = Desc->TryGetNumberField(TEXT("At"), At);
            int32 Track = 0;
            const bool bHasTrack = Desc->TryGetNumberField(TEXT("Track"), Track);

            TArray<int32> Matches;
            for (int32 Candidate = 0; Candidate < Notifies.Num(); ++Candidate)
            {
                const FAnimNotifyEvent& Event = Notifies[Candidate];
                const bool bNameMatches = Event.NotifyName.ToString() == Name;
                const bool bTimeMatches = !bHasAt || FMath::IsNearlyEqual(Event.GetTriggerTime(), static_cast<float>(At), kTimeTolerance);
                const bool bTrackMatches = !bHasTrack || Event.TrackIndex == Track;
                if (bNameMatches && bTimeMatches && bTrackMatches)
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
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no notify matching '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeNotifies(Context.Montage));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' matches %d notifies, narrow it with At or Track. Present: %s"), *Context.AssetPath, *Name, Matches.Num(), *DescribeNotifies(Context.Montage));
            return false;
        }

        bool ApplyParameters(const FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FAnimNotifyEvent& Event) const
        {
            const TSharedPtr<FJsonObject>* Parameters = nullptr;
            if (!Desc->TryGetObjectField(TEXT("Parameters"), Parameters))
            {
                return true;
            }

            UObject* NotifyObject = GetNotifyObject(Event);
            if (!NotifyObject)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a named notify, it carries no parameters"), *Context.AssetPath, *Event.NotifyName.ToString());
                return false;
            }

            NotifyObject->Modify();

            int32 Failures = 0;
            UAssetWorkbench::ApplyProperties(NotifyObject, *Parameters, Failures);
            if (Failures > 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d parameter(s) failed on '%s' in %s"), Failures, *Event.NotifyName.ToString(), *Context.AssetPath);
                return false;
            }

            return true;
        }

        // Trigger time, duration and track move together: the end handle of a state notify is its own
        // linkable element, and both handles carry an offset that only holds for the time they were
        // computed at.
        void SetTimeAndTrack(UAnimMontage* Montage, FAnimNotifyEvent& Event, float TriggerTime, int32 TrackIndex) const
        {
            const float EventDuration = Event.GetDuration();

            Event.Link(Montage, TriggerTime, Event.GetSlotIndex());
            Event.RefreshTriggerOffset(Montage->CalculateOffsetForNotify(Event.GetTime()));

            if (EventDuration > 0.0f)
            {
                Event.EndLink.Link(Montage, Event.GetTime() + EventDuration, Event.GetSlotIndex());
                Event.RefreshEndTriggerOffset(Montage->CalculateOffsetForNotify(Event.EndLink.GetTime()));
            }
            else
            {
                Event.EndTriggerTimeOffset = 0.0f;
            }

            Event.TrackIndex = TrackIndex;
        }

        bool ApplyAdd(FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            double TriggerTime = 0.0;
            if (!Desc->TryGetNumberField(TEXT("TriggerTime"), TriggerTime))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs TriggerTime"), *Context.AssetPath);
                return false;
            }

            if (!ResolveTime(Context, TriggerTime))
            {
                return false;
            }

            UClass* NotifyClass = nullptr;
            FString ClassPath;
            if (Desc->TryGetStringField(TEXT("Class"), ClassPath))
            {
                NotifyClass = ResolveNotifyClass(ClassPath);
                if (!NotifyClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve notify class '%s'"), *Context.AssetPath, *ClassPath);
                    return false;
                }

                const bool bIsNotify = NotifyClass->IsChildOf(UAnimNotify::StaticClass());
                const bool bIsNotifyState = NotifyClass->IsChildOf(UAnimNotifyState::StaticClass());
                if (!bIsNotify && !bIsNotifyState)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is neither an AnimNotify nor an AnimNotifyState"), *Context.AssetPath, *ClassPath);
                    return false;
                }
            }

            FString Name;
            if (!Desc->TryGetStringField(TEXT("Name"), Name))
            {
                if (!NotifyClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs Class, or Name for a named notify"), *Context.AssetPath);
                    return false;
                }

                Name = MakeDefaultNotifyName(NotifyClass);
            }

            const bool bIsState = NotifyClass && NotifyClass->IsChildOf(UAnimNotifyState::StaticClass());
            double Duration = 0.0;
            const bool bHasDuration = Desc->TryGetNumberField(TEXT("Duration"), Duration);
            if (bIsState && !bHasDuration)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a state notify and needs Duration"), *Context.AssetPath, *Name);
                return false;
            }

            if (!bIsState && bHasDuration)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not a state notify, it has no Duration"), *Context.AssetPath, *Name);
                return false;
            }

            if (bIsState && !ResolveTime(Context, TriggerTime + Duration))
            {
                return false;
            }

            int32 Track = 0;
            Desc->TryGetNumberField(TEXT("Track"), Track);
            if (Track < 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Track %d is negative"), *Context.AssetPath, Track);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add '%s' @ %.4f track %d"), *Context.Montage->GetName(), *Name, TriggerTime, Track);

            if (!Context.bApply)
            {
                return true;
            }

            UAnimMontage* Montage = Context.Montage;
            Montage->Modify();
            AddMissingNotifyTracks(Montage, Track);

            const int32 NewIndex = Montage->Notifies.Add(FAnimNotifyEvent());
            FAnimNotifyEvent& NewEvent = Montage->Notifies[NewIndex];
            NewEvent.NotifyName = FName(*Name);
#if WITH_EDITORONLY_DATA
            NewEvent.Guid = FGuid::NewGuid();
#endif // WITH_EDITORONLY_DATA

            if (NotifyClass)
            {
                UObject* NotifyObject = NewObject<UObject>(Montage, NotifyClass, NAME_None, RF_Transactional);
                NewEvent.NotifyStateClass = Cast<UAnimNotifyState>(NotifyObject);
                NewEvent.Notify = Cast<UAnimNotify>(NotifyObject);

                if (NewEvent.NotifyStateClass)
                {
                    NewEvent.SetDuration(static_cast<float>(Duration));
                    NewEvent.TriggerWeightThreshold = NewEvent.NotifyStateClass->GetDefaultTriggerWeightThreshold();
                }
                else
                {
                    NewEvent.TriggerWeightThreshold = NewEvent.Notify->GetDefaultTriggerWeightThreshold();
                }
            }

            SetTimeAndTrack(Montage, NewEvent, static_cast<float>(TriggerTime), Track);

#if WITH_EDITOR
            // Same hook the panel gives a fresh notify, before the spec's own parameters land.
            if (NewEvent.NotifyStateClass)
            {
                NewEvent.NotifyStateClass->OnAnimNotifyCreatedInEditor(NewEvent);
            }
            else if (NewEvent.Notify)
            {
                NewEvent.Notify->OnAnimNotifyCreatedInEditor(NewEvent);
            }
#endif // WITH_EDITOR

            return ApplyParameters(Context, Desc, NewEvent);
        }

        bool ApplyModify(FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            int32 Index = INDEX_NONE;
            if (!ResolveNotifyIndex(Context, Desc, Index))
            {
                return false;
            }

            const bool bHasNewName = Desc->HasField(TEXT("NewName"));
            const bool bHasTriggerTime = Desc->HasField(TEXT("TriggerTime"));
            const bool bHasDuration = Desc->HasField(TEXT("Duration"));
            const bool bHasTrack = Desc->HasField(TEXT("Track"));
            const bool bHasParameters = Desc->HasField(TEXT("Parameters"));
            if (!bHasNewName && !bHasTriggerTime && !bHasDuration && !bHasParameters)
            {
                // Track alone is a move between rows, which is a real edit. Nothing at all is a spec bug.
                if (!bHasTrack)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Modify writes nothing. Expected NewName, TriggerTime, Duration, Track or Parameters"), *Context.AssetPath);
                    return false;
                }
            }

            UAnimMontage* Montage = Context.Montage;
            const FAnimNotifyEvent& Current = Montage->Notifies[Index];
            const bool bIsState = Current.NotifyStateClass != nullptr;

            // Authored time, not GetTriggerTime(): re-linking at the trigger time would fold the
            // snap offset into the notify and walk it forward on every params-only edit.
            double TriggerTime = Current.GetTime();
            if (bHasTriggerTime)
            {
                Desc->TryGetNumberField(TEXT("TriggerTime"), TriggerTime);
                if (!ResolveTime(Context, TriggerTime))
                {
                    return false;
                }
            }

            double Duration = Current.GetDuration();
            if (bHasDuration)
            {
                if (!bIsState)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not a state notify, it has no Duration"), *Context.AssetPath, *Current.NotifyName.ToString());
                    return false;
                }

                Desc->TryGetNumberField(TEXT("Duration"), Duration);
                if (!ResolveTime(Context, TriggerTime + Duration))
                {
                    return false;
                }
            }

            int32 Track = Current.TrackIndex;
            if (bHasTrack)
            {
                Desc->TryGetNumberField(TEXT("Track"), Track);
                if (Track < 0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Track %d is negative"), *Context.AssetPath, Track);
                    return false;
                }
            }

            FString NewName;
            if (bHasNewName && !Desc->TryGetStringField(TEXT("NewName"), NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: NewName must be a string"), *Context.AssetPath);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: modify '%s' @ %.4f track %d"), *Montage->GetName(), *Current.NotifyName.ToString(), TriggerTime, Track);

            if (!Context.bApply)
            {
                return true;
            }

            Montage->Modify();
            AddMissingNotifyTracks(Montage, Track);

            FAnimNotifyEvent& Event = Montage->Notifies[Index];
            if (bHasNewName)
            {
                Event.NotifyName = FName(*NewName);
            }

            if (bHasDuration)
            {
                Event.SetDuration(static_cast<float>(Duration));
            }

            // Links stay untouched when the spec only rewrites parameters or the name.
            if (bHasTriggerTime || bHasDuration || bHasTrack)
            {
                SetTimeAndTrack(Montage, Event, static_cast<float>(TriggerTime), Track);
            }

            return ApplyParameters(Context, Desc, Event);
        }

        bool ApplyRemove(FMontageEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            int32 Index = INDEX_NONE;
            if (!ResolveNotifyIndex(Context, Desc, Index))
            {
                return false;
            }

            UAnimMontage* Montage = Context.Montage;
            const FAnimNotifyEvent& Event = Montage->Notifies[Index];
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove '%s' @ %.4f track %d"), *Montage->GetName(), *Event.NotifyName.ToString(), Event.GetTriggerTime(), Event.TrackIndex);

            if (!Context.bApply)
            {
                return true;
            }

            Montage->Modify();
            Montage->Notifies.RemoveAt(Index);
            return true;
        }
    };
}

TUniquePtr<IMontageWriter> MakeMontageNotifyWriter()
{
    return MakeUnique<FMontageNotifyWriter>();
}
