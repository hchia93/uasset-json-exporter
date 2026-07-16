#include "TextureExportCommandlet.h"
#include "UAssetJsonExporterModule.h"
#include "UAssetJsonExporterUtil.h"
#include "UAssetJsonExporterVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StreamableRenderAsset.h"
#include "Engine/Texture.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UTextureExportCommandlet::UTextureExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UTextureExportCommandlet::Main(const FString& Params)
{
    if (UAssetJsonExporter::AbortIfLiveEditor())
    {
        return 2;
    }

    UE_LOG(LogUAssetJsonExporter, Display, TEXT("UAssetJsonExporter v%s - TextureExport"), UASSET_JSON_EXPORTER_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetJsonExporter::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetJsonExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/T_A,/Game/Path/T_B\""));
        return 1;
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
        if (!Texture)
        {
            UE_LOG(LogUAssetJsonExporter, Warning, TEXT("Failed to load Texture: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportTexture(Texture);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetJsonExporter, Warning, TEXT("Failed to export Texture: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetJsonExporter::GetExportPath(AssetPath);
        if (UAssetJsonExporter::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetJsonExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetJsonExporter, Display, TEXT("Export complete. %d/%d textures exported."), ExportedCount, AssetPaths.Num());
    return 0;
}

TSharedPtr<FJsonObject> UTextureExportCommandlet::ExportTexture(UTexture* Texture) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_JSON_EXPORTER_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("Texture"));
    Root->SetStringField(TEXT("TextureName"), Texture->GetName());
    Root->SetStringField(TEXT("AssetPath"), Texture->GetPathName());
    Root->SetStringField(TEXT("Class"), Texture->GetClass()->GetName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Class hierarchy
    TArray<TSharedPtr<FJsonValue>> HierarchyArray;
    UClass* CurrentClass = Texture->GetClass();
    while (CurrentClass && CurrentClass != UObject::StaticClass())
    {
        HierarchyArray.Add(MakeShared<FJsonValueString>(CurrentClass->GetName()));
        CurrentClass = CurrentClass->GetSuperClass();
    }
    Root->SetArrayField(TEXT("ClassHierarchy"), HierarchyArray);

    // No surface summary; PlatformData is unbuilt in commandlet mode, dimensions live in ImportedSize / Source
    // All properties from the texture class down to (but not including) UStreamableRenderAsset base
    TSharedPtr<FJsonObject> Props = UAssetJsonExporter::ExportSubclassProperties(Texture, UStreamableRenderAsset::StaticClass());
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Properties"), Props);
    }

    return Root;
}
