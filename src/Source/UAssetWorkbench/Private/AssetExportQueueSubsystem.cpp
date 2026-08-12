#include "AssetExportQueueSubsystem.h"

#include "AssetExportQueueProtocol.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Commandlets/Commandlet.h"
#include "Containers/Ticker.h"
#include "DirectoryWatcherModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDirectoryWatcher.h"
#include "Logging/MessageLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "UAssetExportQueue"

namespace
{
    bool LoadJsonObjectFromFile(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
    {
        FString Raw;
        if (!FFileHelper::LoadFileToString(Raw, *Path))
        {
            return false;
        }
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
    }

    // Asset paths reduced to bare names for toast text. Caps at 3 to keep the
    // notification one line. Remainder collapses to "+N more".
    FString FormatAssetNames(const TArray<FString>& Assets)
    {
        const int32 MaxShown = 3;

        TArray<FString> Names;
        Names.Reserve(Assets.Num());
        for (const FString& AssetPath : Assets)
        {
            Names.Add(FPaths::GetBaseFilename(AssetPath));
        }

        if (Names.Num() <= MaxShown)
        {
            return FString::Join(Names, TEXT(", "));
        }

        const TArray<FString> Head(Names.GetData(), MaxShown);
        return FString::Printf(TEXT("%s +%d more"),
            *FString::Join(Head, TEXT(", ")), Names.Num() - MaxShown);
    }
}

FString UAssetExportQueueSubsystem::GetQueueRoot()
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectDir(), UAssetExportQueue::QueueRootRelative));
}

FString UAssetExportQueueSubsystem::GetPendingDir()
{
    return FPaths::Combine(GetQueueRoot(), UAssetExportQueue::PendingSubdir);
}

FString UAssetExportQueueSubsystem::GetProcessingDir()
{
    return FPaths::Combine(GetQueueRoot(), UAssetExportQueue::ProcessingSubdir);
}

FString UAssetExportQueueSubsystem::GetDoneDir()
{
    return FPaths::Combine(GetQueueRoot(), UAssetExportQueue::DoneSubdir);
}

FString UAssetExportQueueSubsystem::GetAliveFile()
{
    return FPaths::Combine(GetQueueRoot(), UAssetExportQueue::AliveFileName);
}

FString UAssetExportQueueSubsystem::GetExportOutputRoot()
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("UAssetExport")));
}

bool UAssetExportQueueSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    // Live-editor side of the export handshake only. Inside a commandlet this subsystem's own
    // heartbeat would trip the commandlet's live-editor guard and deadlock the standalone path.
    if (IsRunningCommandlet())
    {
        return false;
    }
    return Super::ShouldCreateSubsystem(Outer);
}

void UAssetExportQueueSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    InitializeQueueDirectories();
    TouchHeartbeat();

    m_HeartbeatHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UAssetExportQueueSubsystem::HeartbeatTick),
        UAssetExportQueue::HeartbeatIntervalSeconds);

    m_WatchedDirectory = GetPendingDir();
    FDirectoryWatcherModule& WatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
    if (IDirectoryWatcher* Watcher = WatcherModule.Get())
    {
        Watcher->RegisterDirectoryChangedCallback_Handle(
            m_WatchedDirectory,
            IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &UAssetExportQueueSubsystem::OnPendingDirectoryChanged),
            m_DirectoryWatcherHandle);
    }

    ScanPendingDirectory();

    UE_LOG(LogUAssetWorkbenchCore, Log, TEXT("AssetExportQueueSubsystem online at %s"), *GetQueueRoot());
}

void UAssetExportQueueSubsystem::Deinitialize()
{
    if (m_HeartbeatHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(m_HeartbeatHandle);
        m_HeartbeatHandle.Reset();
    }

    if (m_DirectoryWatcherHandle.IsValid() && !m_WatchedDirectory.IsEmpty())
    {
        FDirectoryWatcherModule* WatcherModule = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
        if (WatcherModule)
        {
            if (IDirectoryWatcher* Watcher = WatcherModule->Get())
            {
                Watcher->UnregisterDirectoryChangedCallback_Handle(m_WatchedDirectory, m_DirectoryWatcherHandle);
            }
        }
        m_DirectoryWatcherHandle.Reset();
    }

    IFileManager::Get().Delete(*GetAliveFile(), false, false, true);

    Super::Deinitialize();
}

void UAssetExportQueueSubsystem::InitializeQueueDirectories() const
{
    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*GetQueueRoot(), true);
    FileManager.MakeDirectory(*GetPendingDir(), true);
    FileManager.MakeDirectory(*GetProcessingDir(), true);
    FileManager.MakeDirectory(*GetDoneDir(), true);
}

void UAssetExportQueueSubsystem::TouchHeartbeat() const
{
    const FString AlivePath = GetAliveFile();
    if (!IFileManager::Get().FileExists(*AlivePath))
    {
        FFileHelper::SaveStringToFile(TEXT(""), *AlivePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    IFileManager::Get().SetTimeStamp(*AlivePath, FDateTime::UtcNow());
}

bool UAssetExportQueueSubsystem::HeartbeatTick(float DeltaTime)
{
    TouchHeartbeat();
    return true;
}

void UAssetExportQueueSubsystem::OnPendingDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
{
    ScanPendingDirectory();
}

void UAssetExportQueueSubsystem::ScanPendingDirectory()
{
    if (m_Processing)
    {
        return;
    }
    TGuardValue<bool> ProcessingGuard(m_Processing, true);

    const FString PendingGlob = FPaths::Combine(GetPendingDir(), TEXT("*.json"));
    TArray<FString> PendingFiles;
    IFileManager::Get().FindFiles(PendingFiles, *PendingGlob, true, false);
    PendingFiles.Sort();

    for (const FString& FileName : PendingFiles)
    {
        const FString FullPath = FPaths::Combine(GetPendingDir(), FileName);
        ProcessTaskFile(FullPath);
    }
}

void UAssetExportQueueSubsystem::ProcessTaskFile(const FString& PendingPath)
{
    const FString BaseName = FPaths::GetBaseFilename(PendingPath);
    const FString ProcessingPath = FPaths::Combine(GetProcessingDir(), BaseName + TEXT(".json"));
    const FString DonePath = FPaths::Combine(GetDoneDir(), BaseName + TEXT(".json"));

    if (!IFileManager::Get().Move(*ProcessingPath, *PendingPath, true, true))
    {
        UE_LOG(LogUAssetWorkbenchCore, Warning, TEXT("Failed to claim task %s"), *PendingPath);
        return;
    }

    TSharedPtr<FJsonObject> Task;
    if (!LoadJsonObjectFromFile(ProcessingPath, Task))
    {
        WriteDoneFile(DonePath, 2, {}, TEXT("Failed to parse pending task json"));
        IFileManager::Get().Delete(*ProcessingPath);
        return;
    }

    FString RunName;
    Task->TryGetStringField(UAssetExportQueue::FieldRunName, RunName);

    TArray<FString> Assets;
    const TArray<TSharedPtr<FJsonValue>>* AssetsValue = nullptr;
    if (Task->TryGetArrayField(UAssetExportQueue::FieldAssets, AssetsValue) && AssetsValue)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *AssetsValue)
        {
            if (Entry.IsValid())
            {
                Assets.Add(Entry->AsString());
            }
        }
    }

    FString ExtraArgs;
    Task->TryGetStringField(UAssetExportQueue::FieldExtraArgs, ExtraArgs);

    if (RunName.IsEmpty() || Assets.IsEmpty())
    {
        WriteDoneFile(DonePath, 2, {}, TEXT("Pending task missing RunName or Assets"));
        IFileManager::Get().Delete(*ProcessingPath);
        return;
    }

    const bool bIsExport = UAssetWorkbench::ResolveGroup(RunName) == UAssetWorkbench::EGroup::Export;

    TSharedPtr<SNotificationItem> Toast;
    if (bIsExport)
    {
        Toast = ShowStartToast(RunName, Assets);
    }

    const FString AssetsCsv = FString::Join(Assets, TEXT(","));
    const int32 RunExitCode = DispatchRun(RunName, AssetsCsv, ExtraArgs);

    // An audit that found something still ran correctly, only Failed and EditorConflict are dispatch failures.
    const bool bHasFindings = RunExitCode == ToExitCode(EUAssetWorkbenchExitType::IssuesFound);
    const bool bDispatched = RunExitCode == ToExitCode(EUAssetWorkbenchExitType::Success) || bHasFindings;

    // Only export leaves a file behind to count. Mutating runs write back into the asset, so their
    // exit code is the only evidence there is.
    TArray<FString> Outputs;
    int32 PresentCount = 0;
    if (bIsExport)
    {
        for (const FString& AssetPath : Assets)
        {
            const FString OutPath = UAssetWorkbench::GetExportPath(AssetPath);
            Outputs.Add(OutPath);
            if (IFileManager::Get().FileExists(*OutPath))
            {
                ++PresentCount;
            }
        }
    }

    const bool bOutputsComplete = !bIsExport || PresentCount == Assets.Num();
    const bool bSuccess = bDispatched && bOutputsComplete;

    // Pass the run's own code through, the caller needs IssuesFound distinguishable from Failed.
    const int32 ExitCode = bSuccess ? RunExitCode : ToExitCode(EUAssetWorkbenchExitType::Failed);

    FString ErrorMessage;
    if (!bDispatched)
    {
        ErrorMessage = FString::Printf(TEXT("Dispatch failed for run '%s'"), *RunName);
    }
    else if (!bOutputsComplete)
    {
        ErrorMessage = FString::Printf(TEXT("Only %d/%d outputs present"), PresentCount, Assets.Num());
    }

    WriteDoneFile(DonePath, ExitCode, Outputs, ErrorMessage);
    IFileManager::Get().Delete(*ProcessingPath);

    const FString AssetNames = FormatAssetNames(Assets);
    FString Summary;
    if (bIsExport && bSuccess)
    {
        Summary = FString::Printf(TEXT("Exported %d/%d (%s): %s"), PresentCount, Assets.Num(), *RunName, *AssetNames);
    }
    else if (bIsExport)
    {
        Summary = FString::Printf(TEXT("Export incomplete %d/%d (%s): %s"), PresentCount, Assets.Num(), *RunName, *AssetNames);
    }
    else if (bHasFindings)
    {
        Summary = FString::Printf(TEXT("%s found issues: %s"), *RunName, *AssetNames);
    }
    else if (bSuccess)
    {
        Summary = FString::Printf(TEXT("%s done: %s"), *RunName, *AssetNames);
    }
    else
    {
        Summary = FString::Printf(TEXT("%s failed: %s"), *RunName, *AssetNames);
    }

    if (bIsExport)
    {
        FinishToast(Toast, bSuccess, Summary);
        return;
    }

    // Findings are not a failure but must not read as clean either.
    ReportToMessageLog(bSuccess && !bHasFindings, Summary);
}

void UAssetExportQueueSubsystem::ReportToMessageLog(bool bSuccess, const FString& Summary)
{
    const FName ListingName(UAssetWorkbench::MessageLogName);
    FMessageLog Log(ListingName);
    if (bSuccess)
    {
        Log.Info(FText::FromString(Summary));
    }
    else
    {
        Log.Error(FText::FromString(Summary));
    }

    Log.Open(bSuccess ? EMessageSeverity::Info : EMessageSeverity::Error, /* bOpenEvenIfEmpty */ false);
}

int32 UAssetExportQueueSubsystem::DispatchRun(const FString& RunName, const FString& AssetsCsv, const FString& ExtraArgs) const
{
    UClass* CmdClass = FindCommandletClass(RunName);
    if (!CmdClass)
    {
        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Unknown commandlet run '%s'"), *RunName);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UCommandlet* Commandlet = NewObject<UCommandlet>(GetTransientPackage(), CmdClass);
    if (!Commandlet)
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FString Params = FString::Printf(TEXT("-assets=\"%s\""), *AssetsCsv);
    if (!ExtraArgs.IsEmpty())
    {
        Params += TEXT(" ");
        Params += ExtraArgs;
    }

    UAssetWorkbench::FInternalDispatchScope DispatchScope;
    return Commandlet->Main(Params);
}

UClass* UAssetExportQueueSubsystem::FindCommandletClass(const FString& RunName)
{
    const FString ClassName = RunName + TEXT("Commandlet");
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (Candidate && Candidate->IsChildOf(UCommandlet::StaticClass()) && Candidate->GetName() == ClassName)
        {
            return Candidate;
        }
    }
    return nullptr;
}

void UAssetExportQueueSubsystem::WriteDoneFile(const FString& DoneFilePath, int32 ExitCode, const TArray<FString>& Outputs, const FString& ErrorMessage)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(UAssetExportQueue::FieldExitCode, ExitCode);

    TArray<TSharedPtr<FJsonValue>> OutputsArray;
    for (const FString& OutPath : Outputs)
    {
        OutputsArray.Add(MakeShared<FJsonValueString>(OutPath));
    }
    Root->SetArrayField(UAssetExportQueue::FieldOutputs, OutputsArray);

    if (!ErrorMessage.IsEmpty())
    {
        Root->SetStringField(UAssetExportQueue::FieldError, ErrorMessage);
    }

    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(Root, Writer);
    FFileHelper::SaveStringToFile(Serialized, *DoneFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

TSharedPtr<SNotificationItem> UAssetExportQueueSubsystem::ShowStartToast(const FString& RunName, const TArray<FString>& Assets) const
{
    const FString Names = FormatAssetNames(Assets);
    const FString Message = FString::Printf(TEXT("Exporting %d (%s): %s..."), Assets.Num(), *RunName, *Names);

    FNotificationInfo Info(FText::FromString(Message));
    Info.bFireAndForget = false;
    Info.ExpireDuration = 0.0f;
    Info.bUseLargeFont = false;

    const FString OutputRoot = GetExportOutputRoot();
    Info.Hyperlink = FSimpleDelegate::CreateLambda([OutputRoot]()
    {
        FPlatformProcess::ExploreFolder(*OutputRoot);
    });
    Info.HyperlinkText = LOCTEXT("RevealOutputFolder", "Reveal Output Folder");

    TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
    if (Item.IsValid())
    {
        Item->SetCompletionState(SNotificationItem::CS_Pending);
    }
    return Item;
}

void UAssetExportQueueSubsystem::FinishToast(TSharedPtr<SNotificationItem> Item, bool bSuccess, const FString& Summary)
{
    if (!Item.IsValid())
    {
        return;
    }
    Item->SetText(FText::FromString(Summary));
    Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
    Item->SetExpireDuration(4.0f);
    Item->ExpireAndFadeout();
}

#undef LOCTEXT_NAMESPACE
