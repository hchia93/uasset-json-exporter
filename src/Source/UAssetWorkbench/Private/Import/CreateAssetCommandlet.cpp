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
    int32 Failures = 0;

    for (const TSharedPtr<FJsonValue>& Value : *Assets)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value->TryGetObject(Entry))
        {
            ++Failures;
            continue;
        }

        if (CreateOne(*Entry))
        {
            ++Created;
            continue;
        }

        ++Failures;
    }

    if (Failures > 0)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("CreateAsset: %d created, %d failed"), Created, Failures);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("CreateAsset: %d asset(s) created"), Created);
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

UObject* UCreateAssetCommandlet::CreateOne(const TSharedPtr<FJsonObject>& Entry) const
{
    FString PackagePath;
    FString AssetName;
    FString ClassName;
    if (!Entry->TryGetStringField(TEXT("PackagePath"), PackagePath) || !Entry->TryGetStringField(TEXT("AssetName"), AssetName) || !Entry->TryGetStringField(TEXT("Class"), ClassName))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Entry needs PackagePath, AssetName and Class"));
        return nullptr;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
    if (UObject* Existing = LoadObject<UObject>(nullptr, *ObjectPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("Already exists, skipped: %s"), *ObjectPath);
        return Existing;
    }

    UClass* AssetClass = ResolveAssetClass(ClassName);
    if (!AssetClass)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Unresolved class: %s"), *ClassName);
        return nullptr;
    }

    UFactory* Factory = ResolveFactory(AssetClass);

    const TSharedPtr<FJsonObject>* FactoryProperties = nullptr;
    if (Entry->TryGetObjectField(TEXT("FactoryProperties"), FactoryProperties))
    {
        if (!Factory)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("FactoryProperties given but no factory supports %s"), *ClassName);
            return nullptr;
        }

        int32 FactoryFailures = 0;
        UAssetWorkbench::ApplyProperties(Factory, *FactoryProperties, FactoryFailures);
        if (FactoryFailures > 0)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d factory propert(ies) failed for %s"), FactoryFailures, *AssetName);
            return nullptr;
        }
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    // Never CreateAssetWithDialog, that is the path that runs ConfigureProperties and opens modal windows.
    UObject* Asset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, Factory);
    if (!Asset)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("CreateAsset returned nothing for %s, check the factory's required fields"), *AssetName);
        return nullptr;
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (Entry->TryGetObjectField(TEXT("Properties"), Properties))
    {
        int32 PropertyFailures = 0;
        UAssetWorkbench::ApplyProperties(Asset, *Properties, PropertyFailures);
        if (PropertyFailures > 0)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d propert(ies) failed on %s"), PropertyFailures, *AssetName);
            return nullptr;
        }
    }

    // Materials need this to rebuild cached expression data. Shader compilation stays skipped headless.
    Asset->PostEditChange();

    if (!UAssetWorkbench::CompileAndSavePackage(Asset, /* bCompileBlueprint */ false))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to save %s"), *ObjectPath);
        return nullptr;
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("Created %s"), *ObjectPath);
    return Asset;
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
