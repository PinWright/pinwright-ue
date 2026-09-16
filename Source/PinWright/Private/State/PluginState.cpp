// Copyright (c) 2026 Alexander Penkin. MIT License.

// Singleton implementation for FPluginState
#include "State/PluginState.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PinWrightSettings.h"
#include "Utils/JobMonitorLog.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

FPluginState::FPluginState()
{
    ReloadCompleteDelegateHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(
        this, &FPluginState::OnReloadComplete);
}

FPluginState::~FPluginState()
{
    FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteDelegateHandle);
}

FPluginState& FPluginState::Get()
{
    static FPluginState Instance;
    return Instance;
}

void FPluginState::OnReloadComplete(EReloadCompleteReason Reason)
{
    UE_LOG(LogTemp, Log, TEXT("FPluginState: Reload complete — invalidating function library cache"));
    InvalidateFunctionLibraryCache();
}

void FPluginState::InvalidateFunctionLibraryCache()
{
    ScannedFunctionLibraries.Empty();
    bFunctionLibrariesScanned = false;
}

const TMap<FString, UClass*>& FPluginState::GetScannedFunctionLibraries()
{
    if (!bFunctionLibrariesScanned)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (IsValid(Class)
                && Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass())
                && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
            {
                ScannedFunctionLibraries.Add(Class->GetName(), Class);
            }
        }
        bFunctionLibrariesScanned = true;
    }
    return ScannedFunctionLibraries;
}

FJobMonitorLog& FPluginState::GetJobMonitorLog()
{
    if (!JobMonitorLogInstance.IsValid())
    {
        const UPinWrightSettings* S =
            GetDefault<UPinWrightSettings>();
        FString IsolatedTestChildMonitorPath;
        const bool bHasPrivateMonitorPath = FParse::Value(
            FCommandLine::Get(), TEXT("PinWrightIsolatedTestChild="), IsolatedTestChildMonitorPath)
            && !IsolatedTestChildMonitorPath.IsEmpty();
        const FString Path = bHasPrivateMonitorPath
            ? IsolatedTestChildMonitorPath
            : FPaths::ProjectDir() / JobMonitorLog::JobsJsonlRelativePath;
        JobMonitorLogInstance = MakeUnique<FJobMonitorLog>(
            Path, (int64)S->MonitorFileMaxBytes, S->MonitorFileRotationKeep);
    }
    return *JobMonitorLogInstance;
}

FJobRegistry& FPluginState::GetJobRegistry()
{
    if (!JobRegistryInstance.IsValid())
    {
        const UPinWrightSettings* S =
            GetDefault<UPinWrightSettings>();
        JobRegistryInstance = MakeUnique<FJobRegistry>(
            S->CompletedTicketTtlSeconds,
            S->ProgressEventMinIntervalMs,
            &GetJobMonitorLog());
    }
    return *JobRegistryInstance;
}

FPwCandidateRegistry& FPluginState::GetCandidateRegistry()
{
    if (!CandidateRegistryInstance.IsValid())
    {
        CandidateRegistryInstance = MakeUnique<FPwCandidateRegistry>();
    }
    return *CandidateRegistryInstance;
}

#if WITH_DEV_AUTOMATION_TESTS
void FPluginState::ResetForTesting()
{
    BlueprintTrackerInstance.Reset();
    SaveThrottlerInstance.Reset();
    JobMonitorLogInstance.Reset();
    JobRegistryInstance.Reset();
    CandidateRegistryInstance.Reset();

    LiveCodingLastCompileResultValue = TEXT("None");

    SequenceRegistryMap.Empty();
    CurrentSequencePathValue.Empty();
    NiagaraRegistryMap.Empty();
    CachedActorSnapshotsMap.Empty();
    NodeCreationStackData.Empty();
    Phase0RanStackData.Empty();

    if ((FolderDumpInstance.bInProgress || !FolderDumpInstance.JobTicketId.IsEmpty())
        && FolderDumpInstance.TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(FolderDumpInstance.TickerHandle);
    }
    FolderDumpInstance = FAsyncFolderDumpState{};

    InvalidateFunctionLibraryCache();
}
#endif
