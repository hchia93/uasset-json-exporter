#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UAssetJsonExporter
{
    // Parse "-assets=A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    TArray<FString> ParseAssetPaths(const FString& Params);

    // Map a /Game/... asset path to <ProjectDir>/Intermediate/UAssetExport/Game/.../<asset>.json
    FString GetExportPath(const FString& AssetPath);

    // Serialize JsonObject to FilePath as UTF-8 (no BOM). Creates output dir if missing.
    bool SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath);

    // Reflection walk from Object's class down to (not including) StopAtClass. Skips Transient/Deprecated.
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass);
}
