#include "Import/CreateAssetCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

UCreateAssetCommandlet::UCreateAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UCreateAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("UAssetWorkbench v%s - CreateAsset"), UASSET_WORKBENCH_VERSION_STRING);

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

    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Assets"), Assets))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec has no Assets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 Created = 0;
    int32 Skipped = 0;
    int32 Failures = 0;

    for (const TSharedPtr<FJsonValue>& Value : *Assets)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value->TryGetObject(Entry))
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Assets carries an entry that is not an object"));
            ++Failures;
            continue;
        }

        switch (CreateOne(*Entry))
        {
        case EOutcome::Created:
            ++Created;
            break;
        case EOutcome::Skipped:
            ++Skipped;
            break;
        default:
            ++Failures;
            break;
        }
    }

    if (Failures > 0)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("CreateAsset: %d created, %d already existed, %d failed"), Created, Skipped, Failures);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    // Saying "created" when every entry already existed is how a caller ends up believing a run did
    // something it did not, so the skip count is stated even when it is the whole run.
    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("CreateAsset: %d created, %d already existed"), Created, Skipped);
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

UCreateAssetCommandlet::EOutcome UCreateAssetCommandlet::CreateOne(const TSharedPtr<FJsonObject>& Entry) const
{
    FString PackagePath;
    FString AssetName;
    FString ClassName;
    if (!Entry->TryGetStringField(TEXT("PackagePath"), PackagePath) || !Entry->TryGetStringField(TEXT("AssetName"), AssetName) || !Entry->TryGetStringField(TEXT("Class"), ClassName))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Entry needs PackagePath, AssetName and Class"));
        return EOutcome::Failed;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
    if (LoadObject<UObject>(nullptr, *ObjectPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("Already exists, nothing created: %s"), *ObjectPath);
        return EOutcome::Skipped;
    }

    UClass* AssetClass = ResolveAssetClass(ClassName);
    if (!AssetClass)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Unresolved class: %s"), *ClassName);
        return EOutcome::Failed;
    }

    UFactory* Factory = ResolveFactory(AssetClass);

    const TSharedPtr<FJsonObject>* FactoryProperties = nullptr;
    if (Entry->TryGetObjectField(TEXT("FactoryProperties"), FactoryProperties))
    {
        if (!Factory)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("FactoryProperties given but no factory supports %s"), *ClassName);
            return EOutcome::Failed;
        }

        int32 FactoryFailures = 0;
        UAssetWorkbench::ApplyProperties(Factory, *FactoryProperties, FactoryFailures);
        if (FactoryFailures > 0)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d factory propert(ies) failed for %s"), FactoryFailures, *AssetName);
            return EOutcome::Failed;
        }
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    // Never CreateAssetWithDialog, that is the path that runs ConfigureProperties and opens modal windows.
    UObject* Asset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, Factory);
    if (!Asset)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("CreateAsset returned nothing for %s, check the factory's required fields"), *AssetName);
        return EOutcome::Failed;
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (Entry->TryGetObjectField(TEXT("Properties"), Properties))
    {
        int32 PropertyFailures = 0;
        UAssetWorkbench::ApplyProperties(Asset, *Properties, PropertyFailures);
        if (PropertyFailures > 0)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d propert(ies) failed on %s"), PropertyFailures, *AssetName);
            return EOutcome::Failed;
        }
    }

    // Materials need this to rebuild cached expression data. Shader compilation stays skipped headless.
    Asset->PostEditChange();

    if (!UAssetWorkbench::CompileAndSavePackage(Asset, /* bCompileBlueprint */ false))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to save %s"), *ObjectPath);
        return EOutcome::Failed;
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("Created %s"), *ObjectPath);
    return EOutcome::Created;
}

UClass* UCreateAssetCommandlet::ResolveAssetClass(const FString& ClassName)
{
    // TryFindTypeSlow only searches loaded objects, a Blueprint generated class needs its package loaded.
    const bool bIsPathName = ClassName.Contains(TEXT(".")) || ClassName.StartsWith(TEXT("/"));
    if (bIsPathName)
    {
        return FSoftClassPath(ClassName).TryLoadClass<UObject>();
    }

    return UClass::TryFindTypeSlow<UClass>(ClassName);
}

UFactory* UCreateAssetCommandlet::ResolveFactory(UClass* AssetClass)
{
    UClass* ExactMatch = nullptr;
    UClass* LooseMatch = nullptr;

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        const bool bIsFactory = Candidate->IsChildOf(UFactory::StaticClass());
        const bool bIsUsable = !Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
        if (!bIsFactory || !bIsUsable)
        {
            continue;
        }

        UFactory* DefaultFactory = Candidate->GetDefaultObject<UFactory>();
        if (!DefaultFactory->CanCreateNew() || DefaultFactory->ImportPriority < 0)
        {
            continue;
        }

        if (DefaultFactory->SupportedClass == AssetClass)
        {
            ExactMatch = Candidate;
            break;
        }

        // Multi-class factories leave SupportedClass null and answer through DoesSupportClass.
        // ResolveSupportedClass would check()-crash on those.
        if (DefaultFactory->SupportedClass == nullptr && DefaultFactory->DoesSupportClass(AssetClass))
        {
            LooseMatch = Candidate;
        }
    }

    UClass* Chosen = ExactMatch ? ExactMatch : LooseMatch;
    if (!Chosen)
    {
        return nullptr;
    }

    // Never hand out the CDO, ParentClass / Struct / Width are per-call state.
    return NewObject<UFactory>(GetTransientPackage(), Chosen);
}
