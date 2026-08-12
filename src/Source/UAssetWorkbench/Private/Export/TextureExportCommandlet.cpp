#include "Export/TextureExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

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
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - TextureExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/T_A,/Game/Path/T_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
        if (!Texture)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load Texture: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportTexture(Texture);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export Texture: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetWorkbench::GetExportPath(AssetPath);
        if (UAssetWorkbench::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d textures exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UTextureExportCommandlet::ExportTexture(UTexture* Texture) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
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

    // No surface summary. PlatformData is unbuilt in commandlet mode, dimensions live in ImportedSize / Source
    // All properties from the texture class down to (but not including) UStreamableRenderAsset base
    TSharedPtr<FJsonObject> Props = UAssetWorkbench::ExportSubclassProperties(Texture, UStreamableRenderAsset::StaticClass());
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Properties"), Props);
    }

    return Root;
}
