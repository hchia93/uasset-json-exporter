#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "SanitizeLevelReferenceCommandlet.generated.h"

// Repoints asset references inside level packages, then resaves the level, so an old asset kept alive
// only for other levels' imports can finally be deleted. Both old and new must still be on disk, so this
// runs before the delete, never after. Pairs with AuditLevelReference, which finds the breakage.
//   UnrealEditor-Cmd.exe Project.uproject -run=SanitizeLevelReference -levels= -replace= [-report=] [-dryrun]
// Contract: Docs/Migrate.md
UCLASS()
class USanitizeLevelReferenceCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    USanitizeLevelReferenceCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    struct FReplacePair
    {
        FString OldPath;
        FString NewPath;
    };

    struct FOptions
    {
        TArray<FString> LevelPaths;
        TArray<FReplacePair> Pairs;
        FString ReportPath;
        bool bDryRun = false;
    };

    struct FLevelResult
    {
        FString LevelPath;
        int32 ReferencesReplaced = 0;
        bool bSaved = false;
        FString FailReason;
    };

    bool ParseOptions(const FString& Params, FOptions& OutOptions) const;

    bool ResolvePairs(const TArray<FReplacePair>& Pairs, TMap<UObject*, UObject*>& OutReplaceMap) const;

    bool ProcessLevel(const FString& LevelPath, const TMap<UObject*, UObject*>& ReplaceMap, bool bDryRun, FLevelResult& OutResult) const;

    bool WriteReport(const FString& ReportPath, const TArray<FLevelResult>& Results, bool bDryRun) const;
};
