#include "Edit/EditAnimMontageCommandlet.h"
#include "Edit/MontageWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    TArray<TUniquePtr<IMontageWriter>> MakeWriters()
    {
        TArray<TUniquePtr<IMontageWriter>> Writers;
        Writers.Add(MakeMontageNotifyWriter());
        return Writers;
    }
}

UEditAnimMontageCommandlet::UEditAnimMontageCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditAnimMontageCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditAnimMontage"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Targets"), Targets))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec has no Targets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditAnimMontage: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UAnimMontage*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        if (!Entry.IsValid() || !ApplyTarget(Entry, bApply, Touched, Ops))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target failed, nothing saved"));
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) staged across %d montage(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UAnimMontage* Montage : Touched)
    {
        if (!UAssetWorkbench::CompileAndSavePackage(Montage, false))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to save package for %s"), *Montage->GetPathName());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) across %d montage(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditAnimMontageCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, bool bApply, TSet<UAnimMontage*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *AssetPath);
    if (!Montage)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load AnimMontage: %s"), *AssetPath);
        return false;
    }

    FMontageEditContext Context;
    Context.Montage = Montage;
    Context.AssetPath = AssetPath;
    Context.bApply = bApply;

    TArray<TUniquePtr<IMontageWriter>> Writers = MakeWriters();

    int32 Matched = 0;
    for (const TUniquePtr<IMontageWriter>& Writer : Writers)
    {
        const TSharedPtr<FJsonValue> Section = Entry->TryGetField(Writer->GetSpecKey());
        if (!Section.IsValid())
        {
            continue;
        }

        ++Matched;
        if (!Writer->Apply(Context, Section))
        {
            return false;
        }
    }

    if (Matched == 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Notifies"), *AssetPath);
        return false;
    }

    OutOps += Context.Ops;

    if (bApply)
    {
        // Sorts the notify array and rebuilds the panel's per-track lists. Ops address notifies by
        // array position, so this runs once the target is done, never between its ops.
        Montage->RefreshCacheData();
        Montage->PostEditChange();
        OutTouched.Add(Montage);
    }

    return true;
}
