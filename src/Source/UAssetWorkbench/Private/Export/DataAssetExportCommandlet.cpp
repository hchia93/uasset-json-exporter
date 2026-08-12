#include "Export/DataAssetExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UDataAssetExportCommandlet::UDataAssetExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UDataAssetExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - DataAssetExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/DA_A,/Game/Path/DA_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UDataAsset* DataAsset = LoadObject<UDataAsset>(nullptr, *AssetPath);
        if (!DataAsset)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load DataAsset: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportDataAsset(DataAsset);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export DataAsset: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetWorkbench::GetExportPath(AssetPath);
        if (UAssetWorkbench::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d data assets exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UDataAssetExportCommandlet::ExportDataAsset(UDataAsset* DataAsset) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("DataAsset"));
    Root->SetStringField(TEXT("DataAssetName"), DataAsset->GetName());
    Root->SetStringField(TEXT("AssetPath"), DataAsset->GetPathName());
    Root->SetStringField(TEXT("Class"), DataAsset->GetClass()->GetName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Class hierarchy
    TArray<TSharedPtr<FJsonValue>> HierarchyArray;
    UClass* CurrentClass = DataAsset->GetClass();
    while (CurrentClass && CurrentClass != UObject::StaticClass())
    {
        HierarchyArray.Add(MakeShared<FJsonValueString>(CurrentClass->GetName()));
        CurrentClass = CurrentClass->GetSuperClass();
    }
    Root->SetArrayField(TEXT("ClassHierarchy"), HierarchyArray);

    // All properties from the DataAsset subclass down to (but not including) UDataAsset base
    TSharedPtr<FJsonObject> Props = UAssetWorkbench::ExportSubclassProperties(DataAsset, UDataAsset::StaticClass());
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Properties"), Props);
    }

    return Root;
}

