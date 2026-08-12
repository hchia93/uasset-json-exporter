#include "Export/AnimMontageExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UAnimMontageExportCommandlet::UAnimMontageExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAnimMontageExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - AnimMontageExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/AM_A,/Game/Path/AM_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *AssetPath);
        if (!Montage)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load AnimMontage: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportMontage(Montage);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export AnimMontage: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetWorkbench::GetExportPath(AssetPath);
        if (UAssetWorkbench::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d montages exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UAnimMontageExportCommandlet::ExportMontage(UAnimMontage* Montage) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("AnimMontage"));
    Root->SetStringField(TEXT("MontageName"), Montage->GetName());
    Root->SetStringField(TEXT("AssetPath"), Montage->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Skeleton
    if (USkeleton* Skeleton = Montage->GetSkeleton())
    {
        Root->SetStringField(TEXT("Skeleton"), Skeleton->GetPathName());
    }

    // Duration
    Root->SetNumberField(TEXT("SequenceLength"), Montage->GetPlayLength());

    // Blend settings
    Root->SetNumberField(TEXT("BlendInTime"), Montage->BlendIn.GetBlendTime());
    Root->SetNumberField(TEXT("BlendOutTime"), Montage->BlendOut.GetBlendTime());

    // Sections
    TArray<TSharedPtr<FJsonValue>> SectionsArray;
    for (int32 i = 0; i < Montage->CompositeSections.Num(); i++)
    {
        const FCompositeSection& Section = Montage->CompositeSections[i];
        TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
        SectionObj->SetStringField(TEXT("Name"), Section.SectionName.ToString());
        SectionObj->SetNumberField(TEXT("StartTime"), Section.GetTime());
        SectionObj->SetNumberField(TEXT("SectionIndex"), i);

        if (Section.NextSectionName != NAME_None)
        {
            SectionObj->SetStringField(TEXT("NextSection"), Section.NextSectionName.ToString());
        }

        SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObj));
    }
    Root->SetArrayField(TEXT("Sections"), SectionsArray);

    // Slot tracks
    TArray<TSharedPtr<FJsonValue>> SlotsArray;
    for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
    {
        TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
        SlotObj->SetStringField(TEXT("SlotName"), SlotTrack.SlotName.ToString());

        TArray<TSharedPtr<FJsonValue>> SegmentsArray;
        for (const FAnimSegment& Segment : SlotTrack.AnimTrack.AnimSegments)
        {
            TSharedPtr<FJsonObject> SegObj = MakeShared<FJsonObject>();

            if (Segment.GetAnimReference())
            {
                SegObj->SetStringField(TEXT("AnimSequence"), Segment.GetAnimReference()->GetName());
                SegObj->SetStringField(TEXT("AnimSequencePath"), Segment.GetAnimReference()->GetPathName());
            }

            SegObj->SetNumberField(TEXT("StartPos"), Segment.StartPos);
            SegObj->SetNumberField(TEXT("AnimStartTime"), Segment.AnimStartTime);
            SegObj->SetNumberField(TEXT("AnimEndTime"), Segment.AnimEndTime);
            SegObj->SetNumberField(TEXT("AnimPlayRate"), Segment.AnimPlayRate);

            SegmentsArray.Add(MakeShared<FJsonValueObject>(SegObj));
        }
        SlotObj->SetArrayField(TEXT("Segments"), SegmentsArray);

        SlotsArray.Add(MakeShared<FJsonValueObject>(SlotObj));
    }
    Root->SetArrayField(TEXT("SlotTracks"), SlotsArray);

    // Notifies (ANS + AN)
    TArray<TSharedPtr<FJsonValue>> NotifiesArray;
    for (const FAnimNotifyEvent& NotifyEvent : Montage->Notifies)
    {
        TSharedPtr<FJsonObject> NotifyObj = ExportNotify(NotifyEvent);
        if (NotifyObj.IsValid())
        {
            NotifiesArray.Add(MakeShared<FJsonValueObject>(NotifyObj));
        }
    }
    Root->SetArrayField(TEXT("Notifies"), NotifiesArray);

    return Root;
}

TSharedPtr<FJsonObject> UAnimMontageExportCommandlet::ExportNotify(const FAnimNotifyEvent& NotifyEvent) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("NotifyName"), NotifyEvent.NotifyName.ToString());
    Obj->SetNumberField(TEXT("TriggerTime"), NotifyEvent.GetTriggerTime());
    Obj->SetNumberField(TEXT("Duration"), NotifyEvent.GetDuration());
    Obj->SetNumberField(TEXT("TrackIndex"), NotifyEvent.TrackIndex);

    bool bIsState = NotifyEvent.NotifyStateClass != nullptr;
    Obj->SetBoolField(TEXT("IsState"), bIsState);

    if (bIsState)
    {
        UAnimNotifyState* State = NotifyEvent.NotifyStateClass;
        Obj->SetStringField(TEXT("NotifyClass"), State->GetClass()->GetName());

        TSharedPtr<FJsonObject> Params = ExportSubclassProperties(State, UAnimNotifyState::StaticClass());
        if (Params.IsValid() && Params->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("Parameters"), Params);
        }
    }
    else if (NotifyEvent.Notify)
    {
        UAnimNotify* Notify = NotifyEvent.Notify;
        Obj->SetStringField(TEXT("NotifyClass"), Notify->GetClass()->GetName());

        TSharedPtr<FJsonObject> Params = ExportSubclassProperties(Notify, UAnimNotify::StaticClass());
        if (Params.IsValid() && Params->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("Parameters"), Params);
        }
    }

    return Obj;
}

TSharedPtr<FJsonObject> UAnimMontageExportCommandlet::ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    UClass* CurrentClass = Object->GetClass();
    while (CurrentClass && CurrentClass != StopAtClass)
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Prop->GetName(), Value);
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}

