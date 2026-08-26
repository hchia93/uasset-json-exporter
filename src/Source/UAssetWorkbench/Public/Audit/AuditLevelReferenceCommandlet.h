#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditLevelReferenceCommandlet.generated.h"

class IAssetRegistry;

// Finds levels importing assets that no longer exist on disk, the classic LD breakage after a rename
// or a partial branch sync. Read-only, walks the Asset Registry dependency graph, never loads worlds.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditLevelReference [-levels=] [-scandir=] [-report=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditLevelReferenceCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditLevelReferenceCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    struct FLevelResult
    {
        FString LevelPath;
        TArray<FString> BrokenRefs;
    };

    void AuditLevel(FName LevelPackage, IAssetRegistry& AssetRegistry, FLevelResult& OutResult) const;

    bool WriteReport(const FString& ReportPath, const TArray<FLevelResult>& Results, int32 LevelsScanned) const;
};
