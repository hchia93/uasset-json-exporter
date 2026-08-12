#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditLevelReferenceCommandlet.generated.h"

class IAssetRegistry;

/*
 * Audit level (.umap) packages for broken asset references: dependencies whose
 * package no longer exists on disk. Catches the classic LD breakage where an
 * asset was renamed or deleted (e.g. pulled in by a partial branch sync) while
 * a level still imports the old path.
 *
 * Read-only. Never loads worlds, never saves packages: walks the Asset Registry
 * dependency graph and checks each dependency with FPackageName::DoesPackageExist.
 *
 * Existence resolves per mounted content root; a dep under an unmounted root (a
 * disabled plugin) reads as missing, so run with the project's plugins enabled.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=AuditLevelReference
 *       [-levels="/Game/Maps/L_A,/Game/Maps/L_B"]
 *       [-scandir="/Game/Maps"]
 *       [-report="<abs path to report JSON>"]
 *
 * Args:
 *   -levels=   Explicit comma-separated level package paths. Takes precedence over -scandir.
 *   -scandir=  Content root to enumerate levels under. Defaults to /Game when neither is given.
 *   -report=   Where to write the report JSON. Defaults under Intermediate/AuditLevelReference/.
 *
 * Exit code: 0 = no broken refs, 3 = broken refs found (gate the sync commit),
 *            1 = bad args or report write failure, 2 = editor is running.
 */
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
