#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditLevelTopologyCommandlet.generated.h"

/*
 * Classify every level (.umap) by its place in the streaming graph: which levels host sublevels,
 * which levels are hosted, and which stand alone. Callers that need to drive streaming (metrics,
 * budget runs) have to know which level to open as the persistent one, and hardcoding that name
 * is what makes such tooling project-specific.
 *
 * Read-only. Loads each level package to read its streaming levels, saves nothing.
 *
 * A level that streams sublevels in is a PersistentHost even when some other level references it,
 * because it can still be opened standalone to drive those sublevels. referenced_by records the
 * nesting either way.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=AuditLevelTopology
 *       [-levels="/Game/Maps/L_A,/Game/Maps/L_B"]
 *       [-scandir="/Game/Maps"]
 *       [-report="<abs path to report JSON>"]
 *
 * Args:
 *   -levels=   Explicit comma-separated level package paths. Takes precedence over -scandir.
 *   -scandir=  Content root to enumerate levels under. Defaults to /Game when neither is given.
 *   -report=   Where to write the report JSON. Defaults under Intermediate/AuditLevelTopology/.
 *
 * Exit code: 0 = report written, 1 = bad args or report write failure, 2 = editor is running.
 */
UCLASS()
class UAuditLevelTopologyCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditLevelTopologyCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    struct FLevelNode
    {
        FString LevelPath;
        TArray<FString> StreamingLevels;
        TArray<FString> ReferencedBy;
    };

    void ReadStreamingLevels(FName LevelPackage, FLevelNode& OutNode) const;

    bool WriteReport(const FString& ReportPath, const TArray<FLevelNode>& Nodes) const;
};
