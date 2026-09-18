// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Engine/DeveloperSettings.h"
#include "PinWrightSettings.generated.h"

// Per-user PinWright settings, persisted to EditorPerProjectUserSettings.ini
// so each developer keeps their own transport/logging/journal preferences.
// Team-shared output locations live in UPinWrightProjectSettings (defaultconfig).

UENUM()
enum class EMcpLogVerbosity : uint8
{
    NoLogging     UMETA(DisplayName = "No Logging"),
    Fatal         UMETA(DisplayName = "Fatal"),
    Error         UMETA(DisplayName = "Error"),
    Warning       UMETA(DisplayName = "Warning"),
    Display       UMETA(DisplayName = "Display"),
    Log           UMETA(DisplayName = "Log"),
    Verbose       UMETA(DisplayName = "Verbose"),
    VeryVerbose   UMETA(DisplayName = "VeryVerbose")
};

UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "PinWright"))
class PINWRIGHT_API UPinWrightSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UPinWrightSettings();

    /** Enable the HTTP automation API transport. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP")
    bool bEnableHttpTransport;

    /** On by default. When enabled, the HTTP port is derived from a hash of the
     *  project path so each project folder gets a stable, unique port with no manual config -
     *  useful when running several projects on one machine. When disabled, the fixed
     *  HttpPort below is used: the same port on every machine, so committed agent configs stay
     *  portable across a team. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP",
        meta = (DisplayName = "Auto-derive Port From Project Path"))
    bool bAutoDerivePort;

    /** Fixed port the HTTP transport binds to. Used only when Auto-derive Port is disabled. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP",
        meta = (ClampMin = "1", ClampMax = "65535", EditCondition = "!bAutoDerivePort"))
    int32 HttpPort;

    /** Maximum accepted HTTP request body size in bytes. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (ClampMin = "1024"))
    int32 HttpMaxRequestBodyBytes;

    /** Direct HTTP responses larger than this many characters are written to Saved/PinWright/HttpResponses. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (ClampMin = "1024"))
    int32 HttpResponseSpillThresholdCharacters;

    /** Default timeout for HTTP /rpc requests. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (ClampMin = "1000"))
    int32 HttpDefaultTimeoutMs;

    /** Maximum timeout accepted from HTTP /rpc requests. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (ClampMin = "1000"))
    int32 HttpMaxTimeoutMs;

    /** When enabled, every HTTP request must carry an Authorization: Bearer <token> header.
     *  The token lives in Saved/PinWright/gateway-token (auto-created on first launch; delete
     *  the file and restart the editor to rotate it). Disable only for MCP clients that cannot
     *  send HTTP headers. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (DisplayName = "Require Auth Token (Bearer)"))
    bool bRequireAuthToken;

    /** Heartbeat interval (in seconds) for idle SSE streams. */
    UPROPERTY(config, EditAnywhere, Category = "HTTP", meta = (ClampMin = "1"))
    int32 SseHeartbeatSeconds;

    // ---- Unattended operation ----
    // PinWright RPCs execute on the game thread. A modal dialog owns that thread in a
    // nested Slate loop, so no RPC can dismiss one and the readiness ticker, the
    // completion-timeout sweep, and the deferred dispatch queue all stop together.
    // See call("unattended") for the full contract.

    /** Scope GIsRunningUnattendedScript over every dispatched RPC handler, so engine
     *  confirmation dialogs auto-answer instead of blocking the game thread. Leave on unless
     *  you are debugging a handler and want the prompts back. Note this makes saving BETTER,
     *  not worse: FEditorFileUtils::PromptForCheckoutAndSave takes a non-prompting SAVE path
     *  under this flag, whereas the launch-time -unattended switch makes it cancel and save
     *  nothing. */
    UPROPERTY(config, EditAnywhere, Category = "Unattended",
        meta = (DisplayName = "Suppress modal dialogs during RPCs"))
    bool bSuppressModalDialogsDuringRpc;

    /** Auto-decline the boot-time "Restore Packages" auto-save recovery prompt. That prompt is
     *  a modal raised before PinWright's ticker has run even once, so no RPC can dismiss it and
     *  the transport cannot even report it. Declining discards only the restore manifest - the
     *  auto-saved .uasset files under Saved/Autosaves/ stay on disk and can be recovered by
     *  hand. Turn off to get the prompt back on an editor you drive by hand. */
    UPROPERTY(config, EditAnywhere, Category = "Unattended",
        meta = (DisplayName = "Decline auto-save recovery prompt at startup"))
    bool bDeclineAutoSaveRecoveryPrompt;

    /** How long the game thread must stay blocked by a modal before the transport reports the
     *  non-retryable EDITOR_BLOCKED_ON_MODAL condition. Below this threshold every response is
     *  byte-identical to the unblocked case, so a human dismissing a dialog quickly never
     *  produces a spurious failure. */
    UPROPERTY(config, EditAnywhere, Category = "Unattended",
        meta = (DisplayName = "Modal blocked report threshold (seconds)",
                ClampMin = "0.0", ClampMax = "300.0"))
    float ModalBlockedReportSeconds;

    /** How long the game thread must go without completing a tick before `ping` reports the
     *  retryable EDITOR_GAME_THREAD_STALLED condition, naming the RPC it is stuck inside.
     *  This is the ONLY probe that can see a wedged handler: a modal broadcasts a Slate
     *  event, a looping handler broadcasts nothing, so the absence of a heartbeat is the
     *  signal. Raise it on projects that routinely do long synchronous game-thread work
     *  (large imports, heavy map loads) - a threshold that fires on a healthy-but-busy
     *  editor is worse than no probe. The 90 s default clears the worst legitimate stall
     *  observed on the C++ automation suite (53.9 s under load) with room to spare. 0
     *  disables the probe. Never gates tools/call: a stalled thread may still drain, and
     *  the queued request runs when it does. */
    UPROPERTY(config, EditAnywhere, Category = "Unattended",
        meta = (DisplayName = "Game thread stall report threshold (seconds)",
                ClampMin = "0.0", ClampMax = "3600.0"))
    float GameThreadStallReportSeconds;

    /** Optional runtime log verbosity override exposed via Project Settings. */

    UPROPERTY(config, EditAnywhere, Category = "Debug")
    EMcpLogVerbosity LogVerbosity;

    /** When true, apply the selected LogVerbosity to this plugin's log category at runtime. */
    UPROPERTY(config, EditAnywhere, Category = "Debug")
    bool bApplyLogVerbosityToAll;

    /** How long (in seconds) a completed ticket is retained before being reaped. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Completed ticket TTL (seconds)",
                ClampMin = "60", ClampMax = "86400"))
    int32 CompletedTicketTtlSeconds;

    /** Maximum size (in bytes) of a single monitor JSONL file before rotation. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Monitor file max bytes",
                ClampMin = "1048576", ClampMax = "104857600"))
    int32 MonitorFileMaxBytes;

    /** Number of rotated monitor file segments to keep on disk. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Monitor file rotations to keep",
                ClampMin = "1", ClampMax = "20"))
    int32 MonitorFileRotationKeep;

    /** Minimum interval (in milliseconds) between progress events emitted for a single job. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Min progress event interval (ms)",
                ClampMin = "0", ClampMax = "60000"))
    int32 ProgressEventMinIntervalMs;

    /** Assets an asset.dump_folder sweep processes between release steps (drain compilation,
     *  unload the packages the sweep itself loaded, collect). 0 disables the count trigger;
     *  the memory watermark below still applies. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Asset dump release interval (assets)",
                ClampMin = "0", ClampMax = "100000"))
    int32 AssetDumpReleaseIntervalAssets;

    /** Fraction of physical RAM the editor's working set may reach before a folder sweep
     *  forces a release step ahead of the count trigger. 0 disables the watermark. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Asset dump release memory watermark (fraction of physical RAM)",
                ClampMin = "0.0", ClampMax = "0.95"))
    float AssetDumpReleaseMemoryWatermark;

    /** Working-set growth (in GiB) a folder sweep may accumulate since the last release step
     *  before one is forced, regardless of how few assets have been processed. 0 disables the
     *  growth trigger. This is the trigger that bounds a sweep over assets far heavier than
     *  the count trigger assumes, on a machine with enough RAM that the watermark never fires. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Asset dump release working-set growth (GiB)",
                ClampMin = "0.0", ClampMax = "1024.0"))
    float AssetDumpReleaseGrowthGiB;

    // The three settings below drive the automation suite's periodic maintenance reset
    // (PinWrightSuiteMaintenance). They share the "Jobs" category with the sweep knobs above
    // because the plugin publishes no test-only settings category; they are deliberately a
    // SEPARATE pair from AssetDumpRelease* -- a folder sweep and a 4000-test suite have
    // unrelated allocation shapes, and one knob serving both would tune neither.

    /** Completed PinWright automation tests between suite maintenance resets (drain compilation,
     *  reset the transaction buffer, collect). 0 disables the count trigger; the memory
     *  watermark below still applies. Overridden by pinwright.TestGcEvery and
     *  -PinWrightTestGcEvery=N. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Test suite reset interval (tests)",
                ClampMin = "0", ClampMax = "100000"))
    int32 TestSuiteResetIntervalTests;

    /** Fraction of physical RAM the editor's working set may reach before a suite reset is
     *  forced ahead of the count trigger. Kept below the external Job Object cap that
     *  scripts/Run-SuiteCapped.ps1 applies (0.60 of RAM) so the in-process reclaim runs first.
     *  0 disables the watermark. Overridden by pinwright.TestMemoryWatermark and
     *  -PinWrightTestMemoryWatermark=F. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Test suite reset memory watermark (fraction of physical RAM)",
                ClampMin = "0.0", ClampMax = "0.95"))
    float TestSuiteResetMemoryWatermark;

    /** Fraction of physical RAM that, if still occupied AFTER a suite reset, means the reset
     *  failed to recover: the run logs PINWRIGHT_MEMORY_WATERMARK_EXCEEDED at Error so
     *  check_suite_log.py can classify the run COMPLETED_WITH_MEMORY_PRESSURE. 0 disables the
     *  escalation. Overridden by pinwright.TestMemoryHardFraction and
     *  -PinWrightTestMemoryHardFraction=F. */
    UPROPERTY(EditAnywhere, Config, Category = "Jobs",
        meta = (DisplayName = "Test suite memory hard fraction (fraction of physical RAM)",
                ClampMin = "0.0", ClampMax = "0.99"))
    float TestSuiteMemoryHardFraction;

    /** Master switch for the editor-only flight/gameplay journal recorder; when false, no session opens on PIE start. */
    UPROPERTY(EditAnywhere, config, Category = "Journal")
    bool bJournalEnabled;

    /** Newest-N journal session files kept on disk; older ones are pruned when a new session starts. */
    UPROPERTY(EditAnywhere, config, Category = "Journal", meta = (ClampMin = "1", UIMin = "1"))
    int32 JournalRetentionCap;

    /** Subdirectory under the project's Saved folder where journal session NDJSON files are written. */
    UPROPERTY(EditAnywhere, config, Category = "Journal")
    FString JournalRecordingsSubdir;

    /** Show the PinWright setup screen (one-click MCP config for AI agent clients) when the editor starts. */
    UPROPERTY(EditAnywhere, config, Category = "Setup")
    bool bShowSetupScreenOnLaunch;

    /** Show the setup screen when something needs attention: the MCP server failed to bind its port
     *  (e.g. another editor already uses it), or an installed agent config is outdated and needs a
     *  one-click Update. */
    UPROPERTY(EditAnywhere, config, Category = "Setup")
    bool bShowSetupScreenOnProblem;

    /** Generate agent MCP configs that launch a bundled-Python stdio proxy in front of the editor's
     *  HTTP endpoint. The proxy keeps the AI client's MCP connection alive across editor
     *  launch/kill/relaunch (only individual tool calls error while the editor is down) and needs no
     *  install (it runs on the engine's bundled Python). When false - or when that Python can't be
     *  found - the legacy direct-HTTP config is written instead. */
    UPROPERTY(EditAnywhere, config, Category = "Setup",
        meta = (DisplayName = "Use stdio proxy for agent configs"))
    bool bUseStdioProxy;

    virtual FName GetContainerName() const override { return TEXT("Editor"); }
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    virtual FName GetSectionName() const override { return TEXT("PinWright"); }
    virtual FText GetSectionText() const override;

    /** Returns the effective HTTP port: the path-derived port when auto-derive is on
     *  (or Settings is null), otherwise the fixed Settings->HttpPort override. */
    static int32 ResolveHttpPort(const UPinWrightSettings* Settings);

    /** Pure path-to-port mapping, exposed for unit testing: normalizes the path and
     *  returns the hash-derived port in the reserved range. */
    static int32 DerivePortFromPath(const FString& InProjectPath);

    // Persist changed properties immediately when edited in Project Settings
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
    {
        Super::PostEditChangeProperty(PropertyChangedEvent);
        SaveConfig();
    }
};
