#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchVersion.h"
#include "AssetExportQueueProtocol.h"

#include "HAL/FileManager.h"
#include "MessageLogModule.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogUAssetWorkbenchCore);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchExporter);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchMigrator);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchImporter);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchAuditor);

void FUAssetWorkbenchModule::StartupModule()
{
    UE_LOG(LogUAssetWorkbenchCore, Log, TEXT("UAssetWorkbench v%s loaded."), UASSET_WORKBENCH_VERSION_STRING);

    // Commandlet runs have no message log UI, registering a listing there is dead weight.
    if (IsRunningCommandlet())
    {
        return;
    }

    FMessageLogModule* MessageLogModule = FModuleManager::LoadModulePtr<FMessageLogModule>(TEXT("MessageLog"));
    if (!MessageLogModule)
    {
        return;
    }

    FMessageLogInitializationOptions Options;
    Options.bShowFilters = true;
    Options.bShowPages = true;
    MessageLogModule->RegisterLogListing(FName(UAssetWorkbench::MessageLogName), NSLOCTEXT("UAssetWorkbench", "MessageLogLabel", "UAsset Workbench"), Options);
}

void FUAssetWorkbenchModule::ShutdownModule()
{
    FMessageLogModule* MessageLogModule = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog"));
    if (MessageLogModule)
    {
        MessageLogModule->UnregisterLogListing(FName(UAssetWorkbench::MessageLogName));
    }
}

UAssetWorkbench::EGroup UAssetWorkbench::ResolveGroup(const FString& RunName)
{
    if (RunName.EndsWith(TEXT("Export")))
    {
        return EGroup::Export;
    }

    if (RunName.EndsWith(TEXT("Import")))
    {
        return EGroup::Import;
    }

    if (RunName.StartsWith(TEXT("Audit")))
    {
        return EGroup::Audit;
    }

    return EGroup::Migrate;
}

bool UAssetWorkbench::IsLiveEditorPresent()
{
    const FString AlivePath = FPaths::Combine(
        FPaths::ProjectDir(),
        UAssetExportQueue::QueueRootRelative,
        UAssetExportQueue::AliveFileName);

    if (!IFileManager::Get().FileExists(*AlivePath))
    {
        return false;
    }

    const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*AlivePath);
    if (ModTime == FDateTime::MinValue())
    {
        return false;
    }

    const FTimespan Age = FDateTime::UtcNow() - ModTime;
    return Age.GetTotalSeconds() <= UAssetExportQueue::HeartbeatFreshnessSeconds;
}

static int32 GInternalDispatchDepth = 0;

UAssetWorkbench::FInternalDispatchScope::FInternalDispatchScope()
{
    ++GInternalDispatchDepth;
}

UAssetWorkbench::FInternalDispatchScope::~FInternalDispatchScope()
{
    --GInternalDispatchDepth;
}

bool UAssetWorkbench::AbortIfLiveEditor()
{
    if (GInternalDispatchDepth > 0)
    {
        return false;
    }
    if (!IsLiveEditorPresent())
    {
        return false;
    }
    UE_LOG(LogUAssetWorkbenchCore, Error,
        TEXT("Live editor session detected. Refusing commandlet run to avoid project lock conflict. ")
        TEXT("Close the editor, or let the in-editor queue subsystem handle the export."));
    return true;
}

IMPLEMENT_MODULE(FUAssetWorkbenchModule, UAssetWorkbench)
