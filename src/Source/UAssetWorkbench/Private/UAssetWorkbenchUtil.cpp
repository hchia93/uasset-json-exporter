#include "UAssetWorkbenchUtil.h"

#include "UAssetWorkbenchModule.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/DateTime.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
    // One "Name" or "Name[3]" hop of a property path.
    struct FPropertyPathHop
    {
        FString Name;
        int32 Index = INDEX_NONE;
    };

    FPropertyPathHop ParsePropertyPathHop(const FString& Segment)
    {
        FPropertyPathHop Hop;

        int32 Bracket = INDEX_NONE;
        if (Segment.FindChar(TEXT('['), Bracket))
        {
            Hop.Name = Segment.Left(Bracket);
            Hop.Index = FCString::Atoi(*Segment.Mid(Bracket + 1));
            return Hop;
        }

        Hop.Name = Segment;
        return Hop;
    }

    // Walks "Array[2].Field" down to the property that takes the value. Owner tracks the last instanced
    // sub-object crossed, ImportText resolves object references against it.
    bool ResolvePropertyPath(UObject* Root, const FString& Path, FProperty*& OutProperty, void*& OutAddress, UObject*& OutOwner)
    {
        TArray<FString> Segments;
        Path.ParseIntoArray(Segments, TEXT("."));
        if (Segments.IsEmpty())
        {
            return false;
        }

        UStruct* Struct = Root->GetClass();
        void* Base = Root;
        UObject* Owner = Root;

        for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
        {
            const FPropertyPathHop Hop = ParsePropertyPathHop(Segments[SegmentIndex]);

            FProperty* Property = Struct->FindPropertyByName(FName(*Hop.Name));
            if (!Property)
            {
                return false;
            }

            void* Address = Property->ContainerPtrToValuePtr<void>(Base);

            if (Hop.Index != INDEX_NONE)
            {
                FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
                if (!ArrayProperty)
                {
                    return false;
                }

                FScriptArrayHelper Helper(ArrayProperty, Address);
                if (!Helper.IsValidIndex(Hop.Index))
                {
                    return false;
                }

                Property = ArrayProperty->Inner;
                Address = Helper.GetRawPtr(Hop.Index);
            }

            const bool bLastSegment = SegmentIndex == Segments.Num() - 1;
            if (bLastSegment)
            {
                OutProperty = Property;
                OutAddress = Address;
                OutOwner = Owner;
                return true;
            }

            if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
            {
                Struct = StructProperty->Struct;
                Base = Address;
                continue;
            }

            if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                UObject* Inner = ObjectProperty->GetObjectPropertyValue(Address);
                if (!Inner)
                {
                    return false;
                }

                Struct = Inner->GetClass();
                Base = Inner;
                Owner = Inner;
                continue;
            }

            return false;
        }

        return false;
    }
    // Strip an object suffix so "/Game/Maps/L_A.L_A" and "/Game/Maps/L_A" both yield the package name.
    FString ToPackageName(const FString& Path)
    {
        FString Trimmed = Path;
        Trimmed.TrimStartAndEndInline();

        int32 DotIndex;
        if (Trimmed.FindChar(TEXT('.'), DotIndex))
        {
            Trimmed = Trimmed.Left(DotIndex);
        }

        return Trimmed;
    }
}

UAssetWorkbench::FLevelScanOptions UAssetWorkbench::ParseLevelScanOptions(const FString& Params, const TCHAR* ReportSubdir)
{
    FLevelScanOptions Options;

    for (const FString& Path : ParsePathList(Params, TEXT("-levels=")))
    {
        Options.LevelPaths.Add(ToPackageName(Path));
    }

    if (!FParse::Value(*Params, TEXT("-scandir="), Options.ScanDir, false) || Options.ScanDir.IsEmpty())
    {
        Options.ScanDir = TEXT("/Game");
    }
    Options.ScanDir.TrimQuotesInline();

    if (!FParse::Value(*Params, TEXT("-report="), Options.ReportPath, false) || Options.ReportPath.IsEmpty())
    {
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
        Options.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), ReportSubdir, FString::Printf(TEXT("%s.json"), *Stamp));
    }
    Options.ReportPath.TrimQuotesInline();

    return Options;
}

void UAssetWorkbench::CollectLevelPackages(const FLevelScanOptions& Options, TArray<FName>& OutLevelPackages)
{
    // Explicit list wins, skip the directory scan entirely.
    if (Options.LevelPaths.Num() > 0)
    {
        for (const FString& Path : Options.LevelPaths)
        {
            OutLevelPackages.AddUnique(FName(*Path));
        }
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FARFilter Filter;
    Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*Options.ScanDir));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> LevelAssets;
    AssetRegistryModule.Get().GetAssets(Filter, LevelAssets);

    for (const FAssetData& LevelAsset : LevelAssets)
    {
        OutLevelPackages.AddUnique(LevelAsset.PackageName);
    }
}

TArray<FString> UAssetWorkbench::ParsePathList(const FString& Params, const TCHAR* ParamName)
{
    TArray<FString> Result;

    FString RawValue;
    if (FParse::Value(*Params, ParamName, RawValue, false))
    {
        RawValue.TrimQuotesInline();
        RawValue.ParseIntoArray(Result, TEXT(","), true);

        for (FString& Path : Result)
        {
            Path.TrimStartAndEndInline();
        }
    }

    return Result;
}

TArray<FString> UAssetWorkbench::ParseAssetPaths(const FString& Params)
{
    return ParsePathList(Params, TEXT("-assets="));
}

int32 UAssetWorkbench::ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures)
{
    int32 Written = 0;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        FProperty* Property = nullptr;
        void* Address = nullptr;
        UObject* Owner = nullptr;
        if (!ResolvePropertyPath(Target, Pair.Key, Property, Address, Owner))
        {
            UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Cannot resolve %s on %s"), *Pair.Key, *Target->GetClass()->GetName());
            ++OutFailures;
            continue;
        }

        // A string is the exporter's own format, the json converter cannot read those struct literals.
        FString StringValue;
        if (Pair.Value->TryGetString(StringValue))
        {
            if (Property->ImportText_Direct(*StringValue, Address, Owner, PPF_None))
            {
                ++Written;
                continue;
            }

            UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("ImportText failed for %s = %s"), *Pair.Key, *StringValue);
            ++OutFailures;
            continue;
        }

        if (FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Address))
        {
            ++Written;
            continue;
        }

        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Json conversion failed for %s"), *Pair.Key);
        ++OutFailures;
    }

    return Written;
}

bool UAssetWorkbench::CompileAndSavePackage(UObject* Asset, bool bCompileBlueprint)
{
    if (!Asset)
    {
        return false;
    }

    if (bCompileBlueprint)
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }
    }

    UPackage* Package = Asset->GetOutermost();
    Package->MarkPackageDirty();

    const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_None;

    return UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);
}

FString UAssetWorkbench::GetExportPath(const FString& AssetPath)
{
    FString RelativePath = AssetPath;
    RelativePath.RemoveFromStart(TEXT("/"));

    return FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("UAssetExport"), RelativePath + TEXT(".json"));
}

bool UAssetWorkbench::SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath)
{
    const FString OutputDir = FPaths::GetPath(FilePath);
    if (!IFileManager::Get().DirectoryExists(*OutputDir))
    {
        IFileManager::Get().MakeDirectory(*OutputDir, true);
    }

    FString OutputString;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    if (!FJsonSerializer::Serialize(JsonObject, Writer))
    {
        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Failed to serialize JSON for: %s"), *FilePath);
        return false;
    }

    if (!FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Failed to write file: %s"), *FilePath);
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> UAssetWorkbench::ExportSubclassProperties(UObject* Object, UClass* StopAtClass)
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

            // Handle array properties with element detail
            if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
            {
                FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Object));
                int32 Num = ArrayHelper.Num();

                TArray<TSharedPtr<FJsonValue>> ElementsArray;
                FProperty* InnerProp = ArrayProp->Inner;

                // If inner is struct or object, export each element individually
                if (CastField<FStructProperty>(InnerProp) || CastField<FObjectProperty>(InnerProp))
                {
                    for (int32 i = 0; i < Num; i++)
                    {
                        FString ElemValue;
                        InnerProp->ExportTextItem_Direct(ElemValue, ArrayHelper.GetRawPtr(i), nullptr, Object, PPF_None);
                        ElementsArray.Add(MakeShared<FJsonValueString>(ElemValue));
                    }
                    // Stable key (no count suffix) so external consumers can index by property name.
                    TSharedPtr<FJsonObject> ArrayInfo = MakeShared<FJsonObject>();
                    ArrayInfo->SetNumberField(TEXT("Count"), Num);
                    ArrayInfo->SetArrayField(TEXT("Elements"), ElementsArray);
                    Props->SetObjectField(Prop->GetName(), ArrayInfo);
                }
                else
                {
                    FString Value;
                    Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
                    if (!Value.IsEmpty())
                    {
                        Props->SetStringField(Prop->GetName(), Value);
                    }
                }
            }
            else
            {
                FString Value;
                Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
                if (!Value.IsEmpty())
                {
                    Props->SetStringField(Prop->GetName(), Value);
                }
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}
