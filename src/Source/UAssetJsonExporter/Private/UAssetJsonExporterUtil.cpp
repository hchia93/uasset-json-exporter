#include "UAssetJsonExporterUtil.h"

#include "UAssetJsonExporterModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TArray<FString> UAssetJsonExporter::ParseAssetPaths(const FString& Params)
{
    TArray<FString> Result;

    FString AssetsValue;
    if (FParse::Value(*Params, TEXT("-assets="), AssetsValue, false))
    {
        AssetsValue.TrimQuotesInline();
        AssetsValue.ParseIntoArray(Result, TEXT(","), true);

        for (FString& Path : Result)
        {
            Path.TrimStartAndEndInline();
        }
    }

    return Result;
}

FString UAssetJsonExporter::GetExportPath(const FString& AssetPath)
{
    FString RelativePath = AssetPath;
    RelativePath.RemoveFromStart(TEXT("/"));

    return FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("UAssetExport"), RelativePath + TEXT(".json"));
}

bool UAssetJsonExporter::SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath)
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
        UE_LOG(LogUAssetJsonExporter, Error, TEXT("Failed to serialize JSON for: %s"), *FilePath);
        return false;
    }

    if (!FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUAssetJsonExporter, Error, TEXT("Failed to write file: %s"), *FilePath);
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> UAssetJsonExporter::ExportSubclassProperties(UObject* Object, UClass* StopAtClass)
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
