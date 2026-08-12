#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UAssetWorkbench
{
    // Parse "-<ParamName>A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    // ParamName carries its own leading dash and trailing equals, e.g. TEXT("-blueprints=").
    TArray<FString> ParsePathList(const FString& Params, const TCHAR* ParamName);

    // Parse "-assets=A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    TArray<FString> ParseAssetPaths(const FString& Params);

    // Compile Blueprints, mark dirty, write the package to disk. Extension follows level vs asset.
    bool CompileAndSavePackage(UObject* Asset, bool bCompileBlueprint = true);

    // Write JSON values onto an object by property name. A string goes through ImportText, which is the
    // exporter's own format, anything else goes through the json converter. Returns properties written,
    // OutFailures counts the ones that resolved but would not take.
    int32 ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures);

    // Map a /Game/... asset path to <ProjectDir>/Intermediate/UAssetExport/Game/.../<asset>.json
    FString GetExportPath(const FString& AssetPath);

    // Serialize JsonObject to FilePath as UTF-8 (no BOM). Creates output dir if missing.
    bool SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath);

    // Reflection walk from Object's class down to (not including) StopAtClass. Skips Transient/Deprecated.
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass);

    // Level selection shared by the audit runs. An explicit -levels= list wins over -scandir=.
    struct FLevelScanOptions
    {
        TArray<FString> LevelPaths;
        FString ScanDir;
        FString ReportPath;
    };

    // Parse -levels= / -scandir= / -report=. ReportSubdir names the folder under Intermediate that
    // the default report path lands in.
    FLevelScanOptions ParseLevelScanOptions(const FString& Params, const TCHAR* ReportSubdir);

    // Resolve scan options into level package names through the asset registry.
    void CollectLevelPackages(const FLevelScanOptions& Options, TArray<FName>& OutLevelPackages);
}
