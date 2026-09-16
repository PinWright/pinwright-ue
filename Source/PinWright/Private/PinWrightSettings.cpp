// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightSettings.h"
#include "Dom/JsonObject.h"

#include "Internationalization/Text.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"

namespace
{
    // Base of the reserved port range; the derived port is BasePort + hash slot.
    constexpr int32 DerivedPortBase = 19880;
    // Number of distinct port slots above the base (range 19880-30119).
    constexpr uint32 DerivedPortSlotCount = 10240u;
}

UPinWrightSettings::UPinWrightSettings()
{
    bEnableHttpTransport = true;
    bAutoDerivePort = true;
    HttpPort = 19880;
    HttpMaxRequestBodyBytes = 1024 * 1024;
    HttpResponseSpillThresholdCharacters = 10000;
    HttpDefaultTimeoutMs = 120000;
    HttpMaxTimeoutMs = 300000;
    bRequireAuthToken = true;
    SseHeartbeatSeconds = 15;

    // Unattended-operation defaults. Both suppressors default ON: a modal that owns
    // the game thread is unclearable by any RPC, and the 2 s report threshold keeps a
    // human's quick dismissal from surfacing as a non-retryable failure.
    bSuppressModalDialogsDuringRpc = true;
    bDeclineAutoSaveRecoveryPrompt = true;
    ModalBlockedReportSeconds = 2.0f;

    // 90 s, not the 2 s the modal probe uses: multi-second game-thread stalls are
    // routine (map loads, saves, synchronous asset compiles) and the C++ automation
    // suite has shown legitimate stalls of 53.9 s under load. Firing on a
    // healthy-but-busy editor would be worse than not probing at all, so the default
    // sits well clear of that with the wedge still reported ~112x faster than the
    // 168-minute hang that motivated the probe.
    GameThreadStallReportSeconds = 90.0f;

    // Default logging behavior
    LogVerbosity = EMcpLogVerbosity::Log;
    bApplyLogVerbosityToAll = false;

    // Jobs lifecycle defaults
    CompletedTicketTtlSeconds = 3600;
    MonitorFileMaxBytes = 10 * 1024 * 1024;
    MonitorFileRotationKeep = 3;
    // One minute was the old default, chosen when this throttle only bounded jobs.jsonl
    // growth. It is the wrong number for a progress feature: a job polled through
    // system.job_status recorded a single event per minute, which reads the same as a
    // wedged editor. 1s bounds the log at 60 lines/minute per job while keeping a poll
    // informative. Streamed progress bypasses this entirely (RecordProgress's
    // bBypassRateLimit) — a client watching live wants every event.
    ProgressEventMinIntervalMs = 1000;

    // Folder-sweep release step. 200 assets keeps the resident set of a /Game-sized
    // sweep bounded without paying a compilation drain more than ~100 times over
    // 22k assets; the watermark is the backstop for a run whose assets are far
    // heavier than average (texture/mesh DDC builds) and would blow past the
    // interval's headroom. Half of physical RAM leaves room for the editor's own
    // baseline plus whatever GetMemoryAvailableForAssetCompilation wants to schedule.
    AssetDumpReleaseIntervalAssets = 200;
    AssetDumpReleaseMemoryWatermark = 0.5f;

    // Automation suite maintenance. 25 tests is the interval the suite has run at since the
    // force-delete removal (the reset is what reclaims detached fixtures). 0.55 sits below the
    // 0.60 external Job Object cap scripts/Run-SuiteCapped.ps1 applies, so the in-process
    // reclaim gets a chance before an allocation can fail; 0.75 still occupied AFTER a full
    // collect means the reclaim did not work and the run is worth flagging rather than trusting.
    TestSuiteResetIntervalTests = 25;
    TestSuiteResetMemoryWatermark = 0.55f;
    TestSuiteMemoryHardFraction = 0.75f;

    // Journal recorder defaults
    bJournalEnabled = true;
    JournalRetentionCap = 20;
    JournalRecordingsSubdir = TEXT("PinWright/Recordings");

    // Startup setup screen
    bShowSetupScreenOnLaunch = true;
    bShowSetupScreenOnProblem = true;

    // Default to the bundled-Python stdio proxy for agent configs (zero-install,
    // survives editor restarts). Falls back to direct HTTP if Python isn't found.
    bUseStdioProxy = true;
}

FText UPinWrightSettings::GetSectionText() const
{
    return NSLOCTEXT("PinWright", "SettingsSection", "PinWright");
}

int32 UPinWrightSettings::DerivePortFromPath(const FString& InProjectPath)
{
    // Lowercase + normalize so the result is stable across path-casing and trailing-slash
    // differences (Windows is case-insensitive). The rare cross-folder hash collision is
    // handled by the bind probe in SocketHttpServer.
    FString Normalized = InProjectPath;
    FPaths::NormalizeDirectoryName(Normalized);
    Normalized.ToLowerInline();
    return DerivedPortBase + static_cast<int32>(FCrc::StrCrc32(*Normalized) % DerivedPortSlotCount);
}

int32 UPinWrightSettings::ResolveHttpPort(const UPinWrightSettings* Settings)
{
    if (Settings && !Settings->bAutoDerivePort)
    {
        return Settings->HttpPort;
    }
    return DerivePortFromPath(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
}
