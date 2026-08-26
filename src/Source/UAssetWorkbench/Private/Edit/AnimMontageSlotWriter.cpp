#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    FString DescribeSlots(const UAnimMontage* Montage)
    {
        TArray<FString> Lines;
        for (int32 Index = 0; Index < Montage->SlotAnimTracks.Num(); ++Index)
        {
            const FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks[Index];
            Lines.Add(FString::Printf(TEXT("[%d] %s (%d segment(s))"), Index, *SlotTrack.SlotName.ToString(), SlotTrack.AnimTrack.AnimSegments.Num()));
        }

        return FString::Join(Lines, TEXT(", "));
    }

    FString DescribeSegments(const FSlotAnimationTrack& SlotTrack)
    {
        TArray<FString> Lines;
        for (int32 Index = 0; Index < SlotTrack.AnimTrack.AnimSegments.Num(); ++Index)
        {
            const FAnimSegment& Segment = SlotTrack.AnimTrack.AnimSegments[Index];
            const UAnimSequenceBase* Reference = Segment.GetAnimReference();
            Lines.Add(FString::Printf(TEXT("[%d] %s @ %.4f"), Index, Reference ? *Reference->GetName() : TEXT("None"), Segment.StartPos));
        }

        return FString::Join(Lines, TEXT(", "));
    }

    class FAnimMontageSlotWriter : public IAnimAssetWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Slots");
        }

        virtual bool Apply(FAnimAssetEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Slots must be an array of ops"), *Context.AssetPath);
                return false;
            }

            UAnimMontage* Montage = AnimAssetEdit::RequireMontage(Context, GetSpecKey());
            if (!Montage)
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Slots carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                FString Op;
                if (!Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: slot entry has no Op"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Montage, Desc, Op))
                {
                    return false;
                }

                ++Context.Ops;
            }

            Recompose(Montage);
            return true;
        }

    private:
        // What the montage editor runs after any segment edit. Ops address segments by array position,
        // so this runs once the target is done, never between its ops.
        void Recompose(UAnimMontage* Montage) const
        {
            for (FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
            {
                SlotTrack.AnimTrack.SortAnimSegments();
            }

            Montage->UpdateLinkableElements();

            const float NewLength = Montage->CalculateSequenceLength();
            const bool bLengthMoved = !FMath::IsNearlyEqual(NewLength, Montage->GetPlayLength(), UE_KINDA_SMALL_NUMBER);
            if (NewLength > 0.0f && bLengthMoved)
            {
                // UAnimMontage::SetCompositeLength is not exported from a MinimalAPI class, this is its body.
                const FFrameTime LengthInFrames = Montage->GetDataModel()->GetFrameRate().AsFrameTime(NewLength);
                Montage->GetController().SetNumberOfFrames(LengthInFrames.RoundToFrame(), false);
            }

            for (FAnimNotifyEvent& Event : Montage->Notifies)
            {
                Event.ConditionalRelink();
            }

            for (FCompositeSection& Section : Montage->CompositeSections)
            {
                Section.ConditionalRelink();
            }
        }

        bool ApplyOp(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc, const FString& Op)
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Montage, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Montage, Desc);
            }

            if (Op == TEXT("AddSegment"))
            {
                return ApplyAddSegment(Context, Montage, Desc);
            }

            if (Op == TEXT("RemoveSegment"))
            {
                return ApplyRemoveSegment(Context, Montage, Desc);
            }

            if (Op == TEXT("ModifySegment"))
            {
                return ApplyModifySegment(Context, Montage, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown slot op '%s'. Expected Add, Remove, AddSegment, RemoveSegment or ModifySegment"), *Context.AssetPath, *Op);
            return false;
        }

        bool ReadSlotName(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FString& OutName) const
        {
            if (!Desc->TryGetStringField(TEXT("SlotName"), OutName) || OutName.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: slot op needs SlotName"), *Context.AssetPath);
                return false;
            }

            return true;
        }

        bool RequireSlot(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const FString& SlotName) const
        {
            const int32 Count = CountSlots(Montage, SlotName);
            if (Count > 1)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s carries several slots named '%s', a spec cannot address one of them"), *Context.AssetPath, *SlotName);
                return false;
            }

            if (Count == 1)
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no slot named '%s'. Present: %s"), *Context.AssetPath, *SlotName, *DescribeSlots(Montage));
            return false;
        }

        int32 CountSlots(const UAnimMontage* Montage, const FString& SlotName) const
        {
            int32 Count = 0;
            for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
            {
                if (SlotTrack.SlotName.ToString() == SlotName)
                {
                    ++Count;
                }
            }

            return Count;
        }

        int32 FindSlotIndex(const UAnimMontage* Montage, const FString& SlotName) const
        {
            return Montage->SlotAnimTracks.IndexOfByPredicate([&SlotName](const FSlotAnimationTrack& SlotTrack)
            {
                return SlotTrack.SlotName.ToString() == SlotName;
            });
        }

        bool ResolveSegmentIndex(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const FString& SlotName, const TSharedPtr<FJsonObject>& Desc, int32& OutIndex) const
        {
            const int32 SlotIndex = FindSlotIndex(Montage, SlotName);
            const int32 Count = Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments.Num();

            int32 Index = INDEX_NONE;
            if (Desc->TryGetNumberField(TEXT("Index"), Index))
            {
                if (Index < 0 || Index >= Count)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: slot '%s' has no segment at index %d, it holds %d"), *Context.AssetPath, *SlotName, Index, Count);
                    return false;
                }

                OutIndex = Index;
                return true;
            }

            double StartPos = 0.0;
            if (!Desc->TryGetNumberField(TEXT("StartPos"), StartPos))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: segment op needs Index or StartPos to address a segment"), *Context.AssetPath);
                return false;
            }

            const FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks[SlotIndex];
            const TArray<FAnimSegment>& Segments = SlotTrack.AnimTrack.AnimSegments;

            TArray<int32> Matches;
            for (int32 Candidate = 0; Candidate < Segments.Num(); ++Candidate)
            {
                if (FMath::IsNearlyEqual(Segments[Candidate].StartPos, static_cast<float>(StartPos), AnimAssetEdit::kTimeTolerance))
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
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: slot '%s' has no segment starting at %.4f. Present: %s"), *Context.AssetPath, *SlotName, StartPos, *DescribeSegments(SlotTrack));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d segments of slot '%s' start at %.4f, address one with Index"), *Context.AssetPath, Matches.Num(), *SlotName, StartPos);
            return false;
        }

        UAnimSequenceBase* ResolveSequence(const FAnimAssetEditContext& Context, const UAnimMontage* Montage, const FString& SlotName, const FString& SequencePath) const
        {
            UAnimSequenceBase* Sequence = LoadObject<UAnimSequenceBase>(nullptr, *SequencePath);
            if (!Sequence)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot load animation '%s'"), *Context.AssetPath, *SequencePath);
                return nullptr;
            }

            if (Sequence->GetSkeleton() != Montage->GetSkeleton())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is built on a different skeleton"), *Context.AssetPath, *SequencePath);
                return nullptr;
            }

            const int32 SlotIndex = FindSlotIndex(Montage, SlotName);
            if (Montage->SlotAnimTracks.IsValidIndex(SlotIndex))
            {
                FText Reason;
                if (!Montage->SlotAnimTracks[SlotIndex].AnimTrack.IsValidToAdd(Sequence, &Reason))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s refuses '%s': %s"), *Context.AssetPath, *SequencePath, *Reason.ToString());
                    return nullptr;
                }
            }

            return Sequence;
        }

        bool ApplyAdd(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString SlotName;
            if (!ReadSlotName(Context, Desc, SlotName))
            {
                return false;
            }

            if (FindSlotIndex(Montage, SlotName) != INDEX_NONE)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already carries a slot named '%s'"), *Context.AssetPath, *SlotName);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add slot '%s'"), *Montage->GetName(), *SlotName);

            Montage->Modify();
            Montage->AddSlot(FName(*SlotName));
            return true;
        }

        bool ApplyRemove(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString SlotName;
            if (!ReadSlotName(Context, Desc, SlotName) || !RequireSlot(Context, Montage, SlotName))
            {
                return false;
            }

            // A montage with no slot plays nothing, the editor keeps the last one for the same reason.
            if (Montage->SlotAnimTracks.Num() <= 1)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is the only slot, a montage cannot lose its last one"), *Context.AssetPath, *SlotName);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove slot '%s'"), *Montage->GetName(), *SlotName);

            Montage->Modify();
            Montage->SlotAnimTracks.RemoveAt(FindSlotIndex(Montage, SlotName));
            return true;
        }

        bool ApplyAddSegment(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString SlotName;
            if (!ReadSlotName(Context, Desc, SlotName) || !RequireSlot(Context, Montage, SlotName))
            {
                return false;
            }

            FString SequencePath;
            if (!Desc->TryGetStringField(TEXT("Sequence"), SequencePath) || SequencePath.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddSegment needs Sequence"), *Context.AssetPath);
                return false;
            }

            UAnimSequenceBase* Sequence = ResolveSequence(Context, Montage, SlotName, SequencePath);
            if (!Sequence)
            {
                return false;
            }

            double StartPos = 0.0;
            if (!Desc->TryGetNumberField(TEXT("StartPos"), StartPos))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddSegment needs StartPos"), *Context.AssetPath);
                return false;
            }

            if (StartPos < 0.0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: StartPos %.4f is negative"), *Context.AssetPath, StartPos);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: add segment '%s' to slot '%s' @ %.4f"), *Montage->GetName(), *Sequence->GetName(), *SlotName, StartPos);

            Montage->Modify();

            FAnimSegment NewSegment;
            NewSegment.SetAnimReference(Sequence, true);
            NewSegment.StartPos = static_cast<float>(StartPos);

            if (!ApplySegmentFields(Context, Desc, NewSegment))
            {
                return false;
            }

            Montage->SlotAnimTracks[FindSlotIndex(Montage, SlotName)].AnimTrack.AnimSegments.Add(NewSegment);
            return true;
        }

        bool ApplyRemoveSegment(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc)
        {
            FString SlotName;
            int32 SegmentIndex = INDEX_NONE;
            if (!ReadSlotName(Context, Desc, SlotName) || !RequireSlot(Context, Montage, SlotName))
            {
                return false;
            }

            if (!ResolveSegmentIndex(Context, Montage, SlotName, Desc, SegmentIndex))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove segment [%d] from slot '%s'"), *Montage->GetName(), SegmentIndex, *SlotName);

            Montage->Modify();
            Montage->SlotAnimTracks[FindSlotIndex(Montage, SlotName)].AnimTrack.AnimSegments.RemoveAt(SegmentIndex);
            return true;
        }

        bool ApplyModifySegment(FAnimAssetEditContext& Context, UAnimMontage* Montage, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString SlotName;
            int32 SegmentIndex = INDEX_NONE;
            if (!ReadSlotName(Context, Desc, SlotName) || !RequireSlot(Context, Montage, SlotName))
            {
                return false;
            }

            if (!ResolveSegmentIndex(Context, Montage, SlotName, Desc, SegmentIndex))
            {
                return false;
            }

            FString SequencePath;
            const bool bHasSequence = Desc->TryGetStringField(TEXT("Sequence"), SequencePath);
            UAnimSequenceBase* Sequence = nullptr;
            if (bHasSequence)
            {
                Sequence = ResolveSequence(Context, Montage, SlotName, SequencePath);
                if (!Sequence)
                {
                    return false;
                }
            }

            // StartPos doubles as the address, so it only moves a segment addressed by Index.
            const bool bAddressedByIndex = Desc->HasField(TEXT("Index"));
            const bool bHasStartPos = bAddressedByIndex && Desc->HasField(TEXT("StartPos"));
            const bool bHasAnimStartTime = Desc->HasField(TEXT("AnimStartTime"));
            const bool bHasAnimEndTime = Desc->HasField(TEXT("AnimEndTime"));
            const bool bHasPlayRate = Desc->HasField(TEXT("PlayRate"));
            const bool bHasLoopingCount = Desc->HasField(TEXT("LoopingCount"));
            const bool bWritesTimes = bHasStartPos || bHasAnimStartTime || bHasAnimEndTime || bHasPlayRate || bHasLoopingCount;
            if (!bHasSequence && !bWritesTimes)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ModifySegment writes nothing. Expected Sequence, StartPos, AnimStartTime, AnimEndTime, PlayRate or LoopingCount"), *Context.AssetPath);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: modify segment [%d] of slot '%s'"), *Montage->GetName(), SegmentIndex, *SlotName);

            Montage->Modify();

            FAnimSegment& Segment = Montage->SlotAnimTracks[FindSlotIndex(Montage, SlotName)].AnimTrack.AnimSegments[SegmentIndex];
            if (bHasSequence)
            {
                Segment.SetAnimReference(Sequence, false);
            }

            if (bHasStartPos)
            {
                double StartPos = Segment.StartPos;
                Desc->TryGetNumberField(TEXT("StartPos"), StartPos);
                if (StartPos < 0.0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: StartPos %.4f is negative"), *Context.AssetPath, StartPos);
                    return false;
                }

                Segment.StartPos = static_cast<float>(StartPos);
            }

            return ApplySegmentFields(Context, Desc, Segment);
        }

        // The four playback fields, shared by AddSegment and ModifySegment. Names mirror what
        // AnimAssetExport prints for a segment, minus the Anim prefix on the rate.
        bool ApplySegmentFields(const FAnimAssetEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FAnimSegment& Segment) const
        {
            double AnimStartTime = Segment.AnimStartTime;
            if (Desc->TryGetNumberField(TEXT("AnimStartTime"), AnimStartTime))
            {
                Segment.AnimStartTime = static_cast<float>(AnimStartTime);
            }

            double AnimEndTime = Segment.AnimEndTime;
            if (Desc->TryGetNumberField(TEXT("AnimEndTime"), AnimEndTime))
            {
                Segment.AnimEndTime = static_cast<float>(AnimEndTime);
            }

            double PlayRate = Segment.AnimPlayRate;
            if (Desc->TryGetNumberField(TEXT("PlayRate"), PlayRate))
            {
                if (FMath::IsNearlyZero(PlayRate))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: PlayRate 0 would stall the segment"), *Context.AssetPath);
                    return false;
                }

                Segment.AnimPlayRate = static_cast<float>(PlayRate);
            }

            int32 LoopingCount = Segment.LoopingCount;
            if (Desc->TryGetNumberField(TEXT("LoopingCount"), LoopingCount))
            {
                if (LoopingCount < 1)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: LoopingCount %d is below one"), *Context.AssetPath, LoopingCount);
                    return false;
                }

                Segment.LoopingCount = LoopingCount;
            }

            if (Segment.AnimEndTime <= Segment.AnimStartTime)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AnimEndTime %.4f is not past AnimStartTime %.4f"), *Context.AssetPath, Segment.AnimEndTime, Segment.AnimStartTime);
                return false;
            }

            Segment.UpdateCachedPlayLength();
            return true;
        }
    };
}

TUniquePtr<IAnimAssetWriter> MakeAnimMontageSlotWriter()
{
    return MakeUnique<FAnimMontageSlotWriter>();
}
