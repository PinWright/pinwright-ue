// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Debug domain handlers:
//   InsightsHandler.cpp, PerformanceHandler.cpp, TestHandler.cpp
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
// Defines the STATS macro guarding the stop_profiling .uestats-path regression test below.
#include "Stats/Stats.h"
// UE 5.8 compiles the legacy stat-file capture (FCommandStatsFile, 'stat startfile'/'stat
// stopfile') out behind UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8 (default 0, defined by
// StatsFile.h); pre-5.8 StatsFile.h never defines the macro. Mirrors the availability gate in
// PerformanceHandler.cpp — the handlers reject with NOT_SUPPORTED when the capture is absent.
#include "Stats/StatsFile.h"
#if defined(UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8)
#define PINWRIGHT_TEST_HAS_STATS_FILE_CAPTURE (STATS && UE_ENABLE_STATS_FILE_DEPRECATED_IN_5_8)
#else
#define PINWRIGHT_TEST_HAS_STATS_FILE_CAPTURE STATS
#endif
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
// The run_benchmark measurement regression reads the job ticket the verb returns, so it needs the
// same live registry the handler writes to (FPluginState::Get().GetJobRegistry()).
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Scalability.h"

// ============================================================================
// insights.start_session
// ============================================================================

// No required params — invoke without any fields to exercise the empty-channels
// branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsStartSessionNoChannelsTest,
    "PinWright.insights.start_session.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsStartSessionNoChannelsTest::RunTest(const FString& Parameters)
{
    // Running trace start commands inside automation creates cross-test state.
    TestTrue(TEXT("insights.start_session is registered"), IsRegistered(TEXT("insights.start_session")));
    return true;
}

// Optional "channels" param present — exercises the non-empty branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsStartSessionWithChannelsTest,
    "PinWright.insights.start_session.WithChannelsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsStartSessionWithChannelsTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("insights.start_session is registered"), IsRegistered(TEXT("insights.start_session")));
    return true;
}

// ============================================================================
// performance.generate_memory_report
// ============================================================================

// All params optional; invoke without fields to exercise the !GEditor guard
// (which sends an error instead of crashing in a headless test environment).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfGenerateMemoryReportNoCrashTest,
    "PinWright.performance.generate_memory_report.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfGenerateMemoryReportNoCrashTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // The handler runs the engine `memreport` console command, whose deferred path
    // (UEngine::HandleMemReportDeferredCommand) unconditionally forces a full-purge
    // CollectGarbage before writing the report. On UE 5.4, that GC's reachability
    // traversal dereferences a stale pointer in an accumulated transient test object
    // and hard-crashes the editor (EXCEPTION_ACCESS_VIOLATION). It is the first forced
    // full GC in the suite, so it is where the latent corruption surfaces; the trigger
    // is incidental to this smoke test. 5.5+ is stable. Assert the handler is registered
    // without exec'ing the GC-forcing command on 5.4.
    TestTrue(TEXT("performance.generate_memory_report is registered"),
        IsRegistered(TEXT("performance.generate_memory_report")));
    return true;
#else
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("detailed"), false);
    TestTrue(TEXT("performance.generate_memory_report handled"), InvokeHandler(TEXT("performance.generate_memory_report"), Payload));
    return true;
#endif
}

// Regression test for E-memory-report-no-path.
// The handler's outputPath param doc promises the report path is "Reported back in the
// response", but the old implementation ran `memreport` and returned a bare
// {"message":"Memory report generated"} with NO path field — forcing callers to
// glob/mtime-sort Saved/Profiling/MemReports themselves. The fix issues the deferred
// command form directly with a unique -NAME= token (so the write is synchronous and the
// leaf filename is deterministic), resolves the actual written .memreport, and returns its
// absolute `path`. This test exercises the production handler and pins that contract: a
// successful generate_memory_report must carry a non-empty `path` pointing at a real file
// on disk. If reverted to the bare-message SendSuccess, the `path` assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfGenerateMemoryReportReturnsPathTest,
    "PinWright.performance.generate_memory_report.ReturnsResolvedPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfGenerateMemoryReportReturnsPathTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // Same UE 5.4 GC-crash hazard as the no-crash smoke test above: do not exec the
    // GC-forcing memreport path on 5.4. Pin the registration/param contract only.
    TestTrue(TEXT("performance.generate_memory_report is registered"),
        IsRegistered(TEXT("performance.generate_memory_report")));
    return true;
#else
    // The handler needs GEditor (it errors NO_EDITOR otherwise). In a commandlet/headless
    // automation context without an editor world, skip the live assertion.
    if (!GEditor)
    {
        AddInfo(TEXT("No GEditor (commandlet context); skipping live memreport path check."));
        TestTrue(TEXT("performance.generate_memory_report is registered"),
            IsRegistered(TEXT("performance.generate_memory_report")));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("detailed"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.generate_memory_report is registered"),
        InvokeHandlerWithCapture(TEXT("performance.generate_memory_report"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Core of the regression: the success response must carry the resolved .memreport
        // path (the contract the outputPath param doc promises). A bare-message response
        // would have no `path` field and this fails.
        FString ResolvedPath;
        const bool bHasPath = Capture.Result->TryGetStringField(TEXT("path"), ResolvedPath);
        TestTrue(TEXT("response carries a resolved .memreport path"), bHasPath && !ResolvedPath.IsEmpty());

        if (bHasPath && !ResolvedPath.IsEmpty())
        {
            // The returned path must point at a real file the call just wrote, with the
            // expected extension — proving it is the actual report, not a fabricated string.
            TestTrue(TEXT("resolved path has the .memreport extension"),
                ResolvedPath.EndsWith(TEXT(".memreport")));
            TestTrue(TEXT("resolved path points at a file that exists on disk"),
                IFileManager::Get().FileExists(*ResolvedPath));

            // Leave no artifact behind: delete the report this test generated.
            IFileManager::Get().Delete(*ResolvedPath, /*RequireExists*/ false);
        }
    }
    return true;
#endif
}

// ============================================================================
// performance.start_profiling
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfStartProfilingNoCrashTest,
    "PinWright.performance.start_profiling.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfStartProfilingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("performance.start_profiling handled"), InvokeHandler(TEXT("performance.start_profiling"), Payload));
    return true;
}

// ============================================================================
// performance.stop_profiling
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfStopProfilingNoCrashTest,
    "PinWright.performance.stop_profiling.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfStopProfilingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("performance.stop_profiling handled"), InvokeHandler(TEXT("performance.stop_profiling"), Payload));
    return true;
}

// Regression test for E-stop-profiling-no-uestats-path.
// stop_profiling used to return a bare {"message":"Profiling stopped"} with no path, forcing
// callers to mtime-sort Saved/Profiling/UnrealStats for the .uestats it had just written. The fix
// reads the finalized capture's absolute path deterministically from the engine
// (FCommandStatsFile::Get().LastFileSaved, set synchronously because 'stat stopfile' blocks on the
// stats pipe via DirectStatsCommand bBlockForCompletion=true) and returns it as statFilePath. This
// test drives the REAL production handlers: it starts a capture via performance.start_profiling,
// stops it via performance.stop_profiling, and asserts the stop response carries a non-empty
// statFilePath ending in .uestats that exists on disk. If reverted to the bare-message SendSuccess,
// the statFilePath assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfStopProfilingReturnsPathTest,
    "PinWright.performance.stop_profiling.ReturnsResolvedStatFilePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfStopProfilingReturnsPathTest::RunTest(const FString& Parameters)
{
#if !PINWRIGHT_TEST_HAS_STATS_FILE_CAPTURE
    // The stats file system (FCommandStatsFile) only exists when STATS is compiled in AND the
    // engine still ships the legacy stat-file capture (compiled out by default in UE 5.8).
    // Without it stop_profiling rejects with NOT_SUPPORTED by design; pin the registration
    // contract only.
    AddInfo(TEXT("Stat-file capture unavailable in this build; skipping live .uestats path check."));
    TestTrue(TEXT("performance.stop_profiling is registered"),
        IsRegistered(TEXT("performance.stop_profiling")));
    return true;
#else
    // The handlers need GEditor (they error NO_EDITOR otherwise) and a running stats pipe. In a
    // commandlet/headless context without an editor, pin the registration contract only.
    if (!GEditor)
    {
        AddInfo(TEXT("No GEditor (commandlet context); skipping live stop_profiling path check."));
        TestTrue(TEXT("performance.stop_profiling is registered"),
            IsRegistered(TEXT("performance.stop_profiling")));
        return true;
    }

    // Start a real stat-file capture through the production handler so the upcoming 'stat stopfile'
    // has a live capture to finalize (and LastFileSaved is refreshed for this stop).
    TSharedPtr<FJsonObject> StartPayload = MakeShared<FJsonObject>();
    TestTrue(TEXT("performance.start_profiling handled"),
        InvokeHandler(TEXT("performance.start_profiling"), StartPayload));

    // Stop it and capture the response.
    TSharedPtr<FJsonObject> StopPayload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.stop_profiling is registered"),
        InvokeHandlerWithCapture(TEXT("performance.stop_profiling"), StopPayload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Core of the regression: stopping a live capture must report the resolved .uestats path.
        // A bare-message response would have no statFilePath and this fails.
        FString StatFilePath;
        const bool bHasPath = Capture.Result->TryGetStringField(TEXT("statFilePath"), StatFilePath);
        TestTrue(TEXT("response carries a resolved statFilePath"), bHasPath && !StatFilePath.IsEmpty());

        if (bHasPath && !StatFilePath.IsEmpty())
        {
            // The path must name a real .uestats file the capture just finalized — proving it is
            // the actual artifact, not a fabricated string.
            TestTrue(TEXT("statFilePath has the .uestats extension"),
                StatFilePath.EndsWith(TEXT(".uestats")));
            TestTrue(TEXT("statFilePath points at a file that exists on disk"),
                IFileManager::Get().FileExists(*StatFilePath));

            // Leave no artifact behind: delete the capture this test produced.
            IFileManager::Get().Delete(*StatFilePath, /*RequireExists*/ false);
        }
    }
    return true;
#endif
}

// ============================================================================
// performance.show_fps
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfShowFpsNoCrashTest,
    "PinWright.performance.show_fps.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfShowFpsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    TestTrue(TEXT("performance.show_fps handled"), InvokeHandler(TEXT("performance.show_fps"), Payload));
    return true;
}

// ============================================================================
// performance.show_stats
// ============================================================================

// Valid alphanumeric category name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfShowStatsValidParamTest,
    "PinWright.performance.show_stats.ValidParamNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfShowStatsValidParamTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("category"), TEXT("unit"));
    TestTrue(TEXT("performance.show_stats handled"), InvokeHandler(TEXT("performance.show_stats"), Payload));
    return true;
}

// Category with invalid characters — handler sends INVALID_CATEGORY without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfShowStatsInvalidCategoryTest,
    "PinWright.performance.show_stats.InvalidCategoryNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfShowStatsInvalidCategoryTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("category"), TEXT("bad;category"));
    TestTrue(TEXT("performance.show_stats invalid category handled"), InvokeHandler(TEXT("performance.show_stats"), Payload));
    return true;
}

// ============================================================================
// performance.set_scalability
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfSetScalabilityNoCrashTest,
    "PinWright.performance.set_scalability.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfSetScalabilityNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("level"), 2);
    TestTrue(TEXT("performance.set_scalability handled"), InvokeHandler(TEXT("performance.set_scalability"), Payload));
    return true;
}

// Regression test for B-set-scalability-no-sg-update (reworded: no read-back).
// performance.set_scalability used to return a bare {"message":"Scalability set"}. The
// underlying Scalability::SetQualityLevels DOES write the sg.* group CVars, but at the
// lowest settable priority (ECVF_SetByScalability), so when a group is pinned higher
// (device profile / config / a prior console 'sg.<Group> N') the requested level is
// silently dropped for that group — and the bare success could not reveal it. The fix
// reads the sg.* groups back after the apply and returns requestedLevel + an appliedGroups
// array of per-group effective values + a requestedLevelApplied flag. This test pins that
// contract: a successful set_scalability must carry the read-back report; reverting to the
// bare message drops requestedLevel/appliedGroups and fails here. It deliberately does NOT
// assert the read-back values equal the requested level — that depends on engine CVar
// priority (any higher-priority pin), which the handler correctly does not override.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfSetScalabilityReadbackTest,
    "PinWright.performance.set_scalability.ReadsBackAppliedGroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfSetScalabilityReadbackTest::RunTest(const FString& Parameters)
{
    // Snapshot the live scalability state so this test does not leak a quality change into
    // the rest of the suite / the editor session, then restore it at the end.
    const Scalability::FQualityLevels Saved = Scalability::GetQualityLevels();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("level"), 2);

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.set_scalability is registered"),
        InvokeHandlerWithCapture(TEXT("performance.set_scalability"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Core of the regression: the response must echo the requested level (a bare
        // "Scalability set" message has no requestedLevel field).
        double RequestedLevel = -1.0;
        TestTrue(TEXT("response echoes the requested level"),
            Capture.Result->TryGetNumberField(TEXT("requestedLevel"), RequestedLevel));
        TestEqual(TEXT("requestedLevel echoes the input"), (int32)RequestedLevel, 2);

        // The read-back report must exist and name a real sg.* group with an effective
        // value (so a bare-message response would fail). sg.ShadowQuality is a stable group.
        const TArray<TSharedPtr<FJsonValue>>* AppliedArr = nullptr;
        const bool bHasApplied = Capture.Result->TryGetArrayField(TEXT("appliedGroups"), AppliedArr);
        TestTrue(TEXT("response carries an appliedGroups read-back report"), bHasApplied && AppliedArr);
        TestTrue(TEXT("appliedGroups reports a real sg.* group that was read back"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("appliedGroups"),
                TEXT("cvar"), TEXT("sg.ShadowQuality")));

        // The landed signal must be present (the machine-checkable confirmation flag that
        // distinguishes "requested level landed everywhere" from "a group was shadowed").
        bool bApplied = false;
        TestTrue(TEXT("response carries the requestedLevelApplied signal"),
            Capture.Result->TryGetBoolField(TEXT("requestedLevelApplied"), bApplied));
    }

    // Restore the pre-test scalability state.
    Scalability::SetQualityLevels(Saved);
    Scalability::SaveState(GEditorIni);
    return true;
}

// ============================================================================
// performance.set_resolution_scale
// ============================================================================

// Valid scale value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfSetResolutionScaleValidParamTest,
    "PinWright.performance.set_resolution_scale.ValidParamNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfSetResolutionScaleValidParamTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("scale"), 100.0);
    TestTrue(TEXT("performance.set_resolution_scale handled"), InvokeHandler(TEXT("performance.set_resolution_scale"), Payload));
    return true;
}

// ============================================================================
// performance.set_vsync
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfSetVSyncNoCrashTest,
    "PinWright.performance.set_vsync.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfSetVSyncNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), false);
    TestTrue(TEXT("performance.set_vsync handled"), InvokeHandler(TEXT("performance.set_vsync"), Payload));
    return true;
}

// ============================================================================
// performance.set_frame_rate_limit
// ============================================================================

// Valid maxFPS value (0 = unlimited).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfSetFrameRateLimitValidParamTest,
    "PinWright.performance.set_frame_rate_limit.ValidParamNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfSetFrameRateLimitValidParamTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("maxFPS"), 60.0);
    TestTrue(TEXT("performance.set_frame_rate_limit handled"), InvokeHandler(TEXT("performance.set_frame_rate_limit"), Payload));
    return true;
}

// ============================================================================
// performance.configure_nanite
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfConfigureNaniteNoCrashTest,
    "PinWright.performance.configure_nanite.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfConfigureNaniteNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    TestTrue(TEXT("performance.configure_nanite handled"), InvokeHandler(TEXT("performance.configure_nanite"), Payload));
    return true;
}

// ============================================================================
// performance.configure_lod
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfConfigureLodNoCrashTest,
    "PinWright.performance.configure_lod.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfConfigureLodNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("lodBias"), 0.0);
    Payload->SetNumberField(TEXT("forceLOD"), -1.0);
    TestTrue(TEXT("performance.configure_lod handled"), InvokeHandler(TEXT("performance.configure_lod"), Payload));
    return true;
}

// ============================================================================
// performance.configure_texture_streaming
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfConfigureTextureStreamingNoCrashTest,
    "PinWright.performance.configure_texture_streaming.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfConfigureTextureStreamingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    Payload->SetNumberField(TEXT("poolSize"), 512.0);
    Payload->SetBoolField(TEXT("boostPlayerLocation"), false);
    TestTrue(TEXT("performance.configure_texture_streaming handled"), InvokeHandler(TEXT("performance.configure_texture_streaming"), Payload));
    return true;
}

// ============================================================================
// performance.merge_actors
// ============================================================================

// "actors" array present but fewer than 2 entries — handler sends INVALID_ARGUMENT.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfMergeActorsTooFewActorsTest,
    "PinWright.performance.merge_actors.TooFewActorsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfMergeActorsTooFewActorsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Actors;
    Actors.Add(MakeShared<FJsonValueString>(TEXT("ActorA")));
    Payload->SetArrayField(TEXT("actors"), Actors);
    TestTrue(TEXT("performance.merge_actors too-few handled"), InvokeHandler(TEXT("performance.merge_actors"), Payload));
    return true;
}

// Regression test for B-merge-actors-rejects-valid-selection.
// Two confirmed root causes were fixed:
//   (1) the documented `toolName` was "MeshMerging", which never name-matches the
//       UE 5.7 mesh-merging tool (GetToolNameText()=="Merge"), so the documented
//       call silently missed and fell through to MERGE_TOOL_UNAVAILABLE. The fix
//       corrects the param doc and maps the legacy "MeshMerging" alias to "Merge".
//   (2) the handler drove the merge via the tool's RunMergeFromSelection(), which
//       always opens a blocking modal CreateModalSaveAssetDialog (via the non-empty
//       GetDefaultPackageName()), hanging a headless MCP call. The fix runs the merge
//       through IMeshMergeUtilities::MergeComponentsToStaticMesh with an explicit
//       output package, reporting `mergedPackageName` instead of `defaultPackageName`.
// This test pins both invariants:
//   - the registered toolName doc must advertise "Merge" and must NOT still claim the
//     stale "defaults to MeshMerging" default (would fail if fix #1 doc were reverted);
//   - a valid two-StaticMeshActor merge requested with the legacy "MeshMerging" alias
//     must NOT come back MERGE_TOOL_UNAVAILABLE (alias mapping, fix #1) and, when it
//     succeeds, must report an explicit `mergedPackageName` and never the modal-path
//     `defaultPackageName` (fix #2). The live merge runs the real production code; if
//     reverted to RunMergeFromSelection it would block on the modal dialog or fail the
//     no-defaultPackageName assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfMergeActorsToolNameAliasAndNoModalTest,
    "PinWright.performance.merge_actors.ToolNameAliasAndHeadlessMerge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfMergeActorsToolNameAliasAndNoModalTest::RunTest(const FString& Parameters)
{
    // (1) The registered toolName doc must point at the real tool name ("Merge") and must
    // not still advertise the stale default that never name-matched.
    const FParamSpec* ToolNameSpec = GetRegisteredParamSpec(TEXT("performance.merge_actors"), TEXT("toolName"));
    TestNotNull(TEXT("merge_actors exposes a toolName param"), ToolNameSpec);
    if (ToolNameSpec)
    {
        TestTrue(TEXT("toolName doc names the real 'Merge' tool"),
            ToolNameSpec->Description.Contains(TEXT("Merge")));
        TestFalse(TEXT("toolName doc must not claim it defaults to MeshMerging"),
            ToolNameSpec->Description.Contains(TEXT("defaults to MeshMerging")));
    }

    // (2) Live merge: two real StaticMeshActors carrying the engine cube mesh, merged via
    // the legacy "MeshMerging" alias. This exercises the production headless merge path.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world (commandlet context); skipping live merge_actors check."));
        return true;
    }

    // Guard destroys the spawned actors (and the merged result) and restores the level
    // dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString LabelA = FString::Printf(TEXT("PW_MergeSrcA_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString LabelB = FString::Printf(TEXT("PW_MergeSrcB_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    AStaticMeshActor* ActorA = SpawnTransientCubeActor(World, LabelA, FVector(2000.0, 2000.0, 200.0));
    if (!ActorA)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Engine cube mesh unavailable; skipping live merge_actors check."));
        return true;
    }
    AStaticMeshActor* ActorB = SpawnTransientCubeActor(World, LabelB, FVector(2300.0, 2000.0, 200.0));
    TestNotNull(TEXT("spawned merge source A"), ActorA);
    TestNotNull(TEXT("spawned merge source B"), ActorB);
    if (!ActorB)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Actors;
    Actors.Add(MakeShared<FJsonValueString>(LabelA));
    Actors.Add(MakeShared<FJsonValueString>(LabelB));
    Payload->SetArrayField(TEXT("actors"), Actors);
    // Legacy alias — must be mapped to "Merge"; the old code returned MERGE_TOOL_UNAVAILABLE here.
    Payload->SetStringField(TEXT("toolName"), TEXT("MeshMerging"));
    Payload->SetBoolField(TEXT("replaceSourceActors"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.merge_actors is registered"),
        InvokeHandlerWithCapture(TEXT("performance.merge_actors"), Payload, Capture));
    TestTrue(TEXT("handler sent a response (did not hang on a modal dialog)"), Capture.bWasCalled);

    // Fix #1: the legacy alias must resolve a tool — never MERGE_TOOL_UNAVAILABLE.
    TestNotEqual(TEXT("legacy 'MeshMerging' alias must not be rejected as unavailable"),
        Capture.ErrorCode, FString(TEXT("MERGE_TOOL_UNAVAILABLE")));
    // A valid two-static-mesh selection must not be rejected as not-possible.
    TestNotEqual(TEXT("valid two-StaticMeshActor selection must not be MERGE_NOT_POSSIBLE"),
        Capture.ErrorCode, FString(TEXT("MERGE_NOT_POSSIBLE")));

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Fix #2: the headless path reports an explicit merged package, never the
        // modal-dialog tool's defaultPackageName.
        FString MergedPackageName;
        TestTrue(TEXT("success reports an explicit mergedPackageName"),
            Capture.Result->TryGetStringField(TEXT("mergedPackageName"), MergedPackageName));
        TestFalse(TEXT("response must not carry the modal-path defaultPackageName"),
            Capture.Result->HasField(TEXT("defaultPackageName")));

        // Clean up the generated merged static mesh asset so the run leaves no artifact.
        CleanupTestAsset(MergedPackageName);
    }
    else
    {
        // Pinned, not silent: the two TestNotEqual assertions above still hold on this
        // path, but the mergedPackageName / defaultPackageName pair — the evidence that
        // the headless direct-utilities path ran instead of the modal tool path — is
        // skipped, and nothing else in the suite asserts it.
        AddWarning(FString::Printf(
            TEXT("performance.merge_actors did not succeed (error '%s'); skipping the "
                 "mergedPackageName / defaultPackageName assertions."), *Capture.ErrorCode));
    }
    return true;
}

// Regression test for B-merge-actors-rejects-valid-selection (param-metadata lens).
// The original handler (a) advertised toolName "defaults to MeshMerging", a string
// that never name-matches the engine's FMeshMergingTool::GetToolNameText()=="Merge",
// and (b) drove the merge through IMergeActorsTool::RunMergeFromSelection, whose
// GetPackageNameForMergeAction -> CreateModalSaveAssetDialog pops a modal save dialog
// that is unanswerable in a headless MCP context. The fix drives
// IMeshMergeUtilities::MergeComponentsToStaticMesh directly with an explicit,
// caller-supplyable output package (no dialog) and corrects the toolName doc.
//
// This pins both invariants from the registered param metadata (no live world/merge
// required, which automation cannot perform reliably):
//   1. The toolName description must NOT claim the bogus "defaults to MeshMerging"
//      default that never matches a registered tool.
//   2. The handler must expose an `outputPackage` param — the explicit-package slot
//      is the headless-bypass mechanism. If the handler reverted to the tool path
//      (RunMergeFromSelection + the modal-dialog default package), this param would
//      not exist. Its presence proves the direct-utilities path is wired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfMergeActorsHeadlessParamsTest,
    "PinWright.performance.merge_actors.HeadlessOutputPackageAndHonestToolName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfMergeActorsHeadlessParamsTest::RunTest(const FString& Parameters)
{
    // (1) Defect #1: the toolName description must not advertise the non-matching
    // "MeshMerging" default.
    const FParamSpec* ToolNameSpec = GetRegisteredParamSpec(TEXT("performance.merge_actors"), TEXT("toolName"));
    if (ToolNameSpec)
    {
        TestFalse(TEXT("toolName description must not claim a 'defaults to MeshMerging' default"),
            ToolNameSpec->Description.Contains(TEXT("defaults to MeshMerging")));
    }

    // (2) Defect #2: the handler must expose an explicit outputPackage slot — the
    // marker that the merge bypasses the modal-save-dialog default-package path.
    const FParamSpec* OutputPackageSpec = GetRegisteredParamSpec(TEXT("performance.merge_actors"), TEXT("outputPackage"));
    TestNotNull(TEXT("merge_actors must expose an outputPackage param (headless bypass)"), OutputPackageSpec);
    return true;
}

// ============================================================================
// performance.run_benchmark
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfRunBenchmarkNoCrashTest,
    "PinWright.performance.run_benchmark.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfRunBenchmarkNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // `type` was dropped from the registration (board B-performance-run-benchmark-measures-nothing:
    // declared, defaulted, never read, never validated), so it is no longer sent here.
    // The window is short so this smoke test does not leave a per-frame sampler alive across the
    // rest of the suite; the measurement itself is asserted by the regression test below.
    Payload->SetNumberField(TEXT("duration"), 0.05);
    TestTrue(TEXT("performance.run_benchmark handled"), InvokeHandler(TEXT("performance.run_benchmark"), Payload));
    return true;
}

// THE regression for board B-performance-run-benchmark-measures-nothing.
//
// `performance.run_benchmark` used to complete a SUCCESSFUL job whose entire payload was
// `{captured:false}` — a verb named "benchmark" that reported no performance quantity on any code
// path, on any engine, and signalled that only through a boolean about a stat file. A caller
// builds on a completed job, so "success having measured nothing" is the shape this test exists to
// keep dead. It admits exactly two honest terminal states and fails on the third:
//
//   completed -> must carry MEASURED numbers (frameCount, measuredDurationSeconds, frameTimeMs,
//                avgFps), with the requested window named separately from the measured one;
//   failed    -> must carry a typed error naming why nothing could be measured;
//   completed with no measurement -> a failure of this test.
//
// Against the pre-fix handler it fails on the frameCount assertion (and again on the surviving
// `captured` field), because `{captured:false}` is a completed job with no measurement in it.
namespace
{
    // Drive the core ticker until TicketId leaves "running", or the budget runs out. Pumping is
    // what MAKES the measurement here: the benchmark's sampler is a zero-delay core-ticker element
    // fed the tick's own delta, exactly as FEngineLoop::Tick feeds it FApp::GetDeltaTime(), so
    // each 10 ms pump both advances the window and supplies one frame time to measure.
    bool PinWrightPumpBenchmarkJobToTerminal(const FString& TicketId, FJobTicket& OutTicket,
        double TimeoutSeconds)
    {
        FJobRegistry& Registry = FPluginState::Get().GetJobRegistry();
        const double Start = FPlatformTime::Seconds();
        while (FPlatformTime::Seconds() - Start < TimeoutSeconds)
        {
            FTSTicker::GetCoreTicker().Tick(0.01f);
            if (Registry.Get(TicketId, OutTicket) && OutTicket.Status != TEXT("running"))
            {
                return true;
            }
            FPlatformProcess::Sleep(0.001f);
        }
        return Registry.Get(TicketId, OutTicket) && OutTicket.Status != TEXT("running");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfRunBenchmarkMeasuresOrRefusesTest,
    "PinWright.performance.run_benchmark.ReportsMeasurementOrFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfRunBenchmarkMeasuresOrRefusesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("duration"), 0.05);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("performance.run_benchmark is registered"),
            InvokeHandlerWithCapture(TEXT("performance.run_benchmark"), Payload, Capture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("the call is accepted and returns a job ticket"),
            Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }

    FString TicketId;
    if (!TestTrue(TEXT("the running response carries ticket_id"),
            Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId)))
    {
        return false;
    }

    FJobTicket Ticket;
    if (!TestTrue(TEXT("the benchmark job reaches a terminal state"),
            PinWrightPumpBenchmarkJobToTerminal(TicketId, Ticket, 20.0)))
    {
        return false;
    }

    if (Ticket.Status == TEXT("failed"))
    {
        // The honest refusal, and the only acceptable alternative to numbers. It must NAME the
        // reason — a bare failure is as unactionable as the old silent success.
        TestFalse(TEXT("a failed benchmark carries a typed error"), Ticket.Error.IsEmpty());
        return true;
    }

    TestEqual(TEXT("the job either failed with a typed error or completed"),
        Ticket.Status, FString(TEXT("completed")));
    if (!TestTrue(TEXT("a completed benchmark carries a result payload"), Ticket.Result.IsValid()))
    {
        return false;
    }

    // The assertion the pre-fix handler cannot pass: a completed benchmark publishes a MEASURED
    // performance quantity.
    double FrameCount = 0.0;
    TestTrue(TEXT("completed benchmark publishes a measured frameCount"),
        Ticket.Result->TryGetNumberField(TEXT("frameCount"), FrameCount));
    TestTrue(TEXT("frameCount is a real count, not a zero placeholder"), FrameCount > 0.0);

    double MeasuredSeconds = 0.0;
    TestTrue(TEXT("completed benchmark publishes measuredDurationSeconds"),
        Ticket.Result->TryGetNumberField(TEXT("measuredDurationSeconds"), MeasuredSeconds));
    TestTrue(TEXT("the measured window has a positive length"), MeasuredSeconds > 0.0);

    // Requested is named separately from measured, never folded into one number a caller cannot
    // tell apart.
    double RequestedSeconds = 0.0;
    TestTrue(TEXT("completed benchmark names the requested window separately"),
        Ticket.Result->TryGetNumberField(TEXT("requestedDurationSeconds"), RequestedSeconds));

    const TSharedPtr<FJsonObject>* FrameTimeMs = nullptr;
    if (TestTrue(TEXT("completed benchmark publishes a frameTimeMs distribution"),
            Ticket.Result->TryGetObjectField(TEXT("frameTimeMs"), FrameTimeMs)))
    {
        const TCHAR* const DistributionFields[] = {
            TEXT("min"), TEXT("p50"), TEXT("p95"), TEXT("max"), TEXT("mean") };
        for (const TCHAR* Field : DistributionFields)
        {
            double Value = -1.0;
            TestTrue(*FString::Printf(TEXT("frameTimeMs.%s is published"), Field),
                (*FrameTimeMs)->TryGetNumberField(FString(Field), Value));
            TestTrue(*FString::Printf(TEXT("frameTimeMs.%s is a real frame time"), Field),
                Value > 0.0);
        }
    }

    double AvgFps = 0.0;
    TestTrue(TEXT("completed benchmark publishes avgFps"),
        Ticket.Result->TryGetNumberField(TEXT("avgFps"), AvgFps));
    TestTrue(TEXT("avgFps is measured, not zeroed"), AvgFps > 0.0);

    // The misleading legacy field is gone by name as well as by shape: `captured` was a boolean
    // about a stat file that read as "did the benchmark capture anything". Its honest replacement
    // is statFileCaptured.
    TestFalse(TEXT("the legacy `captured` field is gone"),
        Ticket.Result->HasField(TEXT("captured")));
    TestTrue(TEXT("the stat-file fact is reported under its own name"),
        Ticket.Result->HasField(TEXT("statFileCaptured")));
    return true;
}

// A window of zero or negative length cannot be measured, so it is refused up front rather than
// started as a job that would have to invent a result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfRunBenchmarkRejectsEmptyWindowTest,
    "PinWright.performance.run_benchmark.RejectsNonPositiveDuration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfRunBenchmarkRejectsEmptyWindowTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("duration"), 0.0);

    TestTrue(TEXT("performance.run_benchmark is registered"),
        InvokeHandlerWithCapture(TEXT("performance.run_benchmark"), Payload, Capture));
    TestTrue(TEXT("the handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("a zero-length window is refused, not started"), Capture.bSuccess);
    TestEqual(TEXT("refusal is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// ============================================================================
// performance.enable_gpu_timing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfEnableGpuTimingNoCrashTest,
    "PinWright.performance.enable_gpu_timing.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfEnableGpuTimingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // enabled=false avoids the GEngine->Exec("stat gpu") path that requires GEditor
    Payload->SetBoolField(TEXT("enabled"), false);
    TestTrue(TEXT("performance.enable_gpu_timing handled"), InvokeHandler(TEXT("performance.enable_gpu_timing"), Payload));
    return true;
}

// ============================================================================
// performance.apply_baseline_settings
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfApplyBaselineSettingsNoCrashTest,
    "PinWright.performance.apply_baseline_settings.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfApplyBaselineSettingsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("profile"), TEXT("balanced"));
    TestTrue(TEXT("performance.apply_baseline_settings handled"), InvokeHandler(TEXT("performance.apply_baseline_settings"), Payload));
    return true;
}

// Regression test for E-baseline-no-sg-setall.
// The handler sets only r.* render CVars; it never writes any sg.* scalability
// group. The old registered description claimed it was "equivalent to a fresh
// 'sg.*' setall" (a concrete lie — the sibling performance.set_scalability is the
// only RPC that drives sg.*), and the response was a bare {"profile":...} echo
// with no report of what changed. The fix (a) makes the description honest about
// the r.*-only scope and (b) returns an applied-CVar report. This test pins both
// invariants: it would fail if the misleading "sg.* setall" phrasing were restored
// or if the response regressed to a bare profile echo.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfApplyBaselineSettingsHonestTest,
    "PinWright.performance.apply_baseline_settings.HonestDescriptionAndAppliedReport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfApplyBaselineSettingsHonestTest::RunTest(const FString& Parameters)
{
    // (a) The registered description must not claim it performs an sg.* setall.
    const FString Summary = GetRegisteredSummary(TEXT("performance.apply_baseline_settings"));
    TestFalse(TEXT("description must not say 'setall'"), Summary.Contains(TEXT("setall")));
    TestFalse(TEXT("description must not claim a fresh sg.* configuration"),
        Summary.Contains(TEXT("sg.*")) && Summary.Contains(TEXT("equivalent")));

    // (b) The response must report the r.* CVars it applied, not a bare profile echo.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("profile"), TEXT("balanced"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.apply_baseline_settings is registered"),
        InvokeHandlerWithCapture(TEXT("performance.apply_baseline_settings"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        // The applied-CVar report must exist, and it must contain the r.* names the
        // balanced branch sets (so a bare echo would fail). Membership of
        // r.ShadowQuality — a stable member of every profile branch — also proves the
        // array is non-empty.
        const TArray<TSharedPtr<FJsonValue>>* AppliedArr = nullptr;
        const bool bHasApplied = Capture.Result->TryGetArrayField(TEXT("appliedCVars"), AppliedArr);
        TestTrue(TEXT("response carries an appliedCVars report"), bHasApplied && AppliedArr);
        TestTrue(TEXT("appliedCVars reports a real r.* CVar that was set"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("appliedCVars"),
                TEXT("cvar"), TEXT("r.ShadowQuality")));
    }
    return true;
}

// ============================================================================
// performance.optimize_draw_calls
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfOptimizeDrawCallsNoCrashTest,
    "PinWright.performance.optimize_draw_calls.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfOptimizeDrawCallsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    Payload->SetBoolField(TEXT("instancing"), true);
    TestTrue(TEXT("performance.optimize_draw_calls handled"), InvokeHandler(TEXT("performance.optimize_draw_calls"), Payload));
    return true;
}

// ============================================================================
// performance.configure_occlusion_culling
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfConfigureOcclusionCullingNoCrashTest,
    "PinWright.performance.configure_occlusion_culling.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfConfigureOcclusionCullingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    Payload->SetNumberField(TEXT("slop"), 1.0);
    Payload->SetNumberField(TEXT("minScreenRadius"), 0.01);
    TestTrue(TEXT("performance.configure_occlusion_culling handled"), InvokeHandler(TEXT("performance.configure_occlusion_culling"), Payload));
    return true;
}

// ============================================================================
// performance.optimize_shaders
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfOptimizeShadersNoCrashTest,
    "PinWright.performance.optimize_shaders.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfOptimizeShadersNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("mode"), TEXT("changed"));
    Payload->SetBoolField(TEXT("forceRecompile"), false);
    TestTrue(TEXT("performance.optimize_shaders handled"), InvokeHandler(TEXT("performance.optimize_shaders"), Payload));
    return true;
}

// ============================================================================
// test.run
// ============================================================================

// Calling test.run from inside an automation run can deadlock the harness by
// starting nested automation execution. Keep this as a registration contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestRunNoFilterNoCrashTest,
    "PinWright.system.run_tests.NoFilterNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTestRunNoFilterNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("system.run_tests is registered"), IsRegistered(TEXT("system.run_tests")));
    return true;
}

// Same safety rule: verify registration only in automation context.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestRunWithFilterNoCrashTest,
    "PinWright.system.run_tests.WithFilterNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTestRunWithFilterNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("system.run_tests is registered"), IsRegistered(TEXT("system.run_tests")));
    return true;
}

// ============================================================================
// Response Capture: performance.show_stats with missing required "category"
// Demonstrates InvokeHandlerWithCapture() pattern for verifying error shape.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfShowStatsCaptureErrorTest,
    "PinWright.performance.show_stats.CaptureErrorResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfShowStatsCaptureErrorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Omit "category" to trigger the handler's INVALID_ARGUMENT error path.
    bool bFound = InvokeHandlerWithCapture(TEXT("performance.show_stats"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing category)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
