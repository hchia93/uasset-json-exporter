#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

class FJsonObject;

namespace UAssetWorkbench
{
    // Everything a run says, on one Message Log page named after that run.
    //
    // Commandlets only ever UE_LOG, which lands in the output log and scrolls away. Alive for the
    // duration of a run, this taps GLog and mirrors every LogUAssetWorkbench* line onto the listing,
    // so a run that already finished can still be read back, and so a warning a run buried among a
    // hundred Display lines is one filter click away. Nothing in a commandlet has to know it exists.
    //
    // Mirroring is one-way on purpose: the FMessageLog used here suppresses its own write back to the
    // output log, otherwise every captured line would come straight back in.
    class FRunReport : public FOutputDevice
    {
    public:
        explicit FRunReport(const FString& RunName);
        virtual ~FRunReport();

        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;

        int32 GetWarningCount() const { return m_WarningCount; }
        int32 GetErrorCount() const { return m_ErrorCount; }

        // Closing line on the page. Severity follows what the run actually reported.
        void Finish(const FString& Summary, bool bSuccess);

    private:
        FString m_RunName;
        int32 m_WarningCount = 0;
        int32 m_ErrorCount = 0;
        bool m_bEmitting = false;
    };

    // Parse "-<ParamName>A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    // ParamName carries its own leading dash and trailing equals, e.g. TEXT("-blueprints=").
    TArray<FString> ParsePathList(const FString& Params, const TCHAR* ParamName);

    // Parse "-assets=A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    TArray<FString> ParseAssetPaths(const FString& Params);

    // Compile Blueprints, mark dirty, write the package to disk. Extension follows level vs asset.
    bool CompileAndSavePackage(UObject* Asset, bool bCompileBlueprint = true);

    // Write JSON values onto an object by property path. A bare name is the object's own property,
    // "Array[2].Field" reaches inside arrays, structs and instanced sub-objects. A string goes through
    // ImportText, which is the exporter's own format, anything else goes through the json converter.
    // Returns properties written, OutFailures counts the ones that resolved but would not take.
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
