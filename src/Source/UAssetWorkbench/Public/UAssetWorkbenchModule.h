#pragma once

#include "Modules/ModuleManager.h"

// One category per group, so a bulk run can be filtered down to the stage that failed.
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchCore, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchExporter, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchMigrator, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchImporter, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchAuditor, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogUAssetWorkbenchEditor, Log, All);

// Exit codes every commandlet returns. IssuesFound is a run that worked and still found something to
// fix, which is not Failed.
enum class EUAssetWorkbenchExitType : int32
{
    Success = 0,
    Failed = 1,
    EditorConflict = 2,
    IssuesFound = 3
};

constexpr int32 ToExitCode(EUAssetWorkbenchExitType ExitType)
{
    return static_cast<int32>(ExitType);
}

class FUAssetWorkbenchModule : public IModuleInterface
{
public:

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};

namespace UAssetWorkbench
{
    // Message log listing this plugin reports mutating runs into.
    inline constexpr const TCHAR* MessageLogName = TEXT("UAssetWorkbench");

    // Which group a run belongs to. Decides how success is judged and where the result is reported.
    enum class EGroup : uint8
    {
        Export,
        Migrate,
        Import,
        Edit,
        Audit
    };

    // Import and Export read as noun suffixes, every other verb leads. Create lands in Import because
    // both write assets. Repair runs carry no marker, so Migrate is the fallback.
    EGroup ResolveGroup(const FString& RunName);

    // Returns true if a recent heartbeat is present (in-editor queue subsystem active).
    bool IsLiveEditorPresent();

    // Logs and returns true if a live editor was detected. Commandlets call this and
    // early-return to avoid fighting the editor for the project lock.
    bool AbortIfLiveEditor();

    // While alive, AbortIfLiveEditor returns false, so the subsystem's own heartbeat cannot make a
    // commandlet it dispatched refuse to run.
    struct FInternalDispatchScope
    {
        FInternalDispatchScope();
        ~FInternalDispatchScope();
    };
}
