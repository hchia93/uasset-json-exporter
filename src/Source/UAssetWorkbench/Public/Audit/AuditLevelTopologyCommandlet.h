#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditLevelTopologyCommandlet.generated.h"

// Classifies every level by its place in the streaming graph: host, hosted, standalone. Callers that
// drive streaming need to know which level opens as the persistent one instead of hardcoding a name.
// Loads each level package to read its streaming levels, saves nothing.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditLevelTopology [-levels=] [-scandir=] [-report=]
// Contract: Docs/Audit.md
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
