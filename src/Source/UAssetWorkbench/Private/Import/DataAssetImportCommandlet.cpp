#include "Import/DataAssetImportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UDataAssetImportCommandlet::UDataAssetImportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UDataAssetImportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("UAssetWorkbench v%s - DataAssetImport"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FString AssetPath;
    if (!Spec->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec has no AssetPath field"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (!Spec->TryGetObjectField(TEXT("Properties"), Properties))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec has no Properties field"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UDataAsset* Asset = LoadObject<UDataAsset>(nullptr, *AssetPath);
    if (!Asset)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to load DataAsset: %s"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    Asset->Modify();

    int32 Failures = 0;
    const int32 Written = UAssetWorkbench::ApplyProperties(Asset, *Properties, Failures);

    if (Failures > 0)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d propert(ies) failed to import, nothing saved"), Failures);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (!UAssetWorkbench::CompileAndSavePackage(Asset, /* bCompileBlueprint */ false))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to save package for %s"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("Imported %d propert(ies) into %s"), Written, *AssetPath);
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
