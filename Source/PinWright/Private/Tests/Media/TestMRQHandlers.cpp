// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Movie Render Queue (MRQ) handlers.
// Gated on the same __has_include block as the handler so the suite still
// compiles in builds where the MovieRenderPipeline plugin is disabled.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/MRQ/MRQHandlerTestHooks.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if __has_include("MoviePipelineQueueSubsystem.h") && \
    __has_include("MoviePipelinePIEExecutor.h") && \
    __has_include("MoviePipelineQueue.h") && \
    __has_include("MoviePipelinePrimaryConfig.h")
    #include "Editor.h"
    #include "MoviePipelineExecutor.h"
    #include "MoviePipelineQueueSubsystem.h"
    #include "MoviePipelineQueue.h"
    #include "MoviePipelinePrimaryConfig.h"
    // The pre-flight tests read the queued job's own output setting back, so the assertions
    // compare the response against the engine object rather than against a literal.
    #include "MoviePipelineOutputSetting.h"
    #include "Tests/Media/TestMRQSelectionExecutor.h"
    #define MCP_HAS_MRQ 1
#else
    #define MCP_HAS_MRQ 0
#endif

#if MCP_HAS_MRQ

namespace
{
    // The queue is editor-global state shared with every other test in the suite, so both tests
    // below restore its length rather than trusting their own success path to have done it.
    void PWMrqTrimQueueTo(UMoviePipelineQueue* Queue, int32 TargetJobCount)
    {
        while (Queue && Queue->GetJobs().Num() > TargetJobCount && Queue->GetJobs().Num() > 0)
        {
            const int32 Before = Queue->GetJobs().Num();
            Queue->DeleteJob(Queue->GetJobs().Last());
            // A DeleteJob that removed nothing (a null entry) would otherwise spin forever, and
            // a hung test is worse than a queue left one job long.
            if (Queue->GetJobs().Num() >= Before)
            {
                break;
            }
        }
    }

    bool PWMrqWriteSolidPng(const FString& Path, int32 Size, const FColor& Color)
    {
        PinWrightImage::FBitmap Bitmap;
        Bitmap.Width = Size;
        Bitmap.Height = Size;
        Bitmap.Pixels.Init(Color, Size * Size);
        FString ErrorCode;
        FString ErrorMessage;
        return PinWrightImage::SaveBitmapPng(Path, Bitmap, ErrorCode, ErrorMessage);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsNonexistentSequenceTest,
    "PinWright.mrq.create_job.NonexistentSequenceDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsNonexistentSequenceTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so invalid sequence-path refusal "
                 "was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/PinWrightTests/NoSuchSequence.NoSuchSequence"));
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/PinWrightTests/Map_NotLoaded.Map_NotLoaded"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("invalid sequence-path request responded"), Capture.bWasCalled);
    TestFalse(TEXT("nonexistent sequence is refused"), Capture.bSuccess);
    TestEqual(TEXT("nonexistent sequence reports ASSET_NOT_FOUND"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestEqual(TEXT("nonexistent sequence leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsWrongTypeSequenceTest,
    "PinWright.mrq.create_job.WrongTypeSequenceDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsWrongTypeSequenceTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so wrong-type sequence refusal "
                 "was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("PW_WrongTypeSequence_%s"), *Stamp);
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    // The fixture is RF_Standalone, so releasing the TStrongObjectPtr below is not enough to
    // reclaim it; detach it from its package too.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    UPackage* Package = CreatePackage(*PackageName);
    UMoviePipelinePrimaryConfig* WrongType = Package
        ? NewObject<UMoviePipelinePrimaryConfig>(Package, UMoviePipelinePrimaryConfig::StaticClass(),
            *AssetName, RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (!WrongType)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_wrong_type_fixture_uncreatable"),
            TEXT("An in-memory wrong-type asset could not be created, so refusal was not "
                 "exercised."));
        return true;
    }
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> WrongTypeGuard(WrongType);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), FString::Printf(TEXT("%s.%s"),
        *PackageName, *AssetName));
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/PinWrightTests/Map_NotLoaded.Map_NotLoaded"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("wrong-type sequence request responded"), Capture.bWasCalled);
    TestFalse(TEXT("wrong-type sequence is refused"), Capture.bSuccess);
    TestEqual(TEXT("wrong-type sequence reports ASSET_WRONG_TYPE"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_ASSET_WRONG_TYPE));
    TestEqual(TEXT("wrong-type sequence leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsUninitializedSequenceTest,
    "PinWright.mrq.create_job.UninitializedSequenceDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsUninitializedSequenceTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so invalid sequence refusal was "
                 "not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = FString::Printf(TEXT("/Temp/PinWrightTests/NullSequence_%s"), *Stamp);
    // The fixture is RF_Standalone, so releasing the TStrongObjectPtr below is not enough to
    // reclaim it; detach it from its package too.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    UPackage* Package = CreatePackage(*PackageName);
    ULevelSequence* Sequence = Package
        ? NewObject<ULevelSequence>(Package, ULevelSequence::StaticClass(),
            *FString::Printf(TEXT("NullSequence_%s"), *Stamp), RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (!Sequence)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_sequence_fixture_uncreatable"),
            TEXT("An uninitialized transient LevelSequence could not be created."));
        return true;
    }
    TStrongObjectPtr<ULevelSequence> SequenceGuard(Sequence);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Temp/PinWrightTests/NoMap.NoMap"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("uninitialized sequence request responded"), Capture.bWasCalled);
    TestFalse(TEXT("uninitialized sequence is refused"), Capture.bSuccess);
    TestEqual(TEXT("uninitialized sequence reports SEQUENCE_INVALID"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_SEQUENCE_INVALID));
    TestEqual(TEXT("uninitialized sequence leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsInvalidLevelPathTest,
    "PinWright.mrq.create_job.InvalidLevelPathDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsInvalidLevelPathTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so invalid level-path refusal was "
                 "not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/PinWrightTests/Seq_NotLoaded"));
    Payload->SetStringField(TEXT("levelPath"), TEXT("/NotMounted/Map_NotLoaded"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("invalid level-path request responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid level path is refused"), Capture.bSuccess);
    TestEqual(TEXT("invalid level path reports INVALID_PATH"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PATH));
    TestEqual(TEXT("invalid level path leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsEmptyOutputDirectoryTest,
    "PinWright.mrq.create_job.EmptyOutputDirectoryDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsEmptyOutputDirectoryTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so empty output-directory refusal "
                 "was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("PW_InvalidOutputPreset_%s"), *Stamp);
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    // The fixture is RF_Standalone, so releasing the TStrongObjectPtr below is not enough to
    // reclaim it; detach it from its package too.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    UPackage* Package = CreatePackage(*PackageName);
    UMoviePipelinePrimaryConfig* Preset = Package
        ? NewObject<UMoviePipelinePrimaryConfig>(Package, UMoviePipelinePrimaryConfig::StaticClass(),
            *AssetName, RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (!Preset)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_fixture_uncreatable"),
            TEXT("An in-memory UMoviePipelinePrimaryConfig could not be created, so empty "
                 "output-directory refusal was not exercised."));
        return true;
    }
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> PresetGuard(Preset);

    UMoviePipelineOutputSetting* OutputSetting = Preset->FindSetting<UMoviePipelineOutputSetting>();
    if (!OutputSetting)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_output_setting_absent"),
            TEXT("The in-memory preset carries no output setting, so empty output-directory "
                 "refusal was not exercised."));
        return true;
    }
    OutputSetting->OutputDirectory.Path.Empty();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/PinWrightTests/Seq_NotLoaded"));
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/PinWrightTests/Map_NotLoaded"));
    Payload->SetStringField(TEXT("presetPath"), FString::Printf(TEXT("%s.%s"),
        *PackageName, *AssetName));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("empty output-directory request responded"), Capture.bWasCalled);
    TestFalse(TEXT("empty output directory is refused"), Capture.bSuccess);
    TestEqual(TEXT("empty output directory reports INVALID_PATH"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PATH));
    TestEqual(TEXT("empty output directory leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsFileValuedOutputAncestorTest,
    "PinWright.mrq.create_job.FileValuedOutputAncestorDoesNotMutateQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsFileValuedOutputAncestorTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("The editor-global MRQ queue is unavailable, so file-valued output-ancestor "
                 "refusal was not exercised."));
        return true;
    }
    const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    if (ProjectFilePath.IsEmpty() || !IFileManager::Get().FileExists(*ProjectFilePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_project_file_unavailable"),
            TEXT("The project file is unavailable, so a file-valued output ancestor could not "
                 "be constructed."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("PW_FileAncestorPreset_%s"), *Stamp);
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    // The fixture is RF_Standalone, so releasing the TStrongObjectPtr below is not enough to
    // reclaim it; detach it from its package too.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    UPackage* Package = CreatePackage(*PackageName);
    UMoviePipelinePrimaryConfig* Preset = Package
        ? NewObject<UMoviePipelinePrimaryConfig>(Package, UMoviePipelinePrimaryConfig::StaticClass(),
            *AssetName, RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (!Preset)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_fixture_uncreatable"),
            TEXT("An in-memory UMoviePipelinePrimaryConfig could not be created, so the "
                 "file-valued output-ancestor refusal was not exercised."));
        return true;
    }
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> PresetGuard(Preset);

    UMoviePipelineOutputSetting* OutputSetting = Preset->FindSetting<UMoviePipelineOutputSetting>();
    if (!OutputSetting)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_output_setting_absent"),
            TEXT("The in-memory preset carries no output setting, so file-valued output-ancestor "
                 "refusal was not exercised."));
        return true;
    }
    OutputSetting->OutputDirectory.Path = FPaths::Combine(ProjectFilePath, TEXT("Child"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/PinWrightTests/Seq_NotLoaded"));
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/PinWrightTests/Map_NotLoaded"));
    Payload->SetStringField(TEXT("presetPath"), FString::Printf(TEXT("%s.%s"),
        *PackageName, *AssetName));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("file-valued output-ancestor request responded"), Capture.bWasCalled);
    TestFalse(TEXT("file-valued output ancestor is refused"), Capture.bSuccess);
    TestEqual(TEXT("file-valued output ancestor reports INVALID_PATH"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PATH));
    TestEqual(TEXT("file-valued output ancestor leaves the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

// THE DEFECT THIS PINS. mrq.create_job treated an unloadable `presetPath` as a soft failure: it
// logged a UE_LOG warning no caller sees, queued the job with NO configuration, and returned a
// success whose `presetPath` field was byte-identical to the success case. The caller's only
// evidence said their preset applied; the render then ran on the engine's CDO defaults —
// Quality/CRF 20, into a directory they never chose — for minutes, and produced a deliverable
// (B-mrq-render-result-omits-bitrate-and-size, ask 2).
//
// COUNTERFACTUAL. Every assertion below fails against the old handler, in four independent ways:
// it responded success, it set no error code, it echoed the preset it could not load, and it left
// a job in the shared queue. This drives the DISPATCHER rather than a helper, because the claim
// under test is a claim about the response (docs/rpc-design.md §1, "test at the layer the
// contract lives on").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobUnloadablePresetIsNotAppliedTest,
    "PinWright.mrq.create_job.UnloadablePresetIsNotReportedAsApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobUnloadablePresetIsNotAppliedTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable on this host, so the "
                 "unloadable-preset refusal was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    // The failing LoadObject writes engine warnings (LogUObjectGlobals / LogLinker) which the
    // automation framework elevates to test errors, and the handler adds its own LogMRQHandler
    // warning. None of them is what this test measures — every assertion below reads the captured
    // response, which bypasses log capture entirely — so log capture is switched off rather than
    // enumerating engine log spellings that vary by version. Same reason and same shape as
    // Tests/World/TestLevelHandlers.cpp.
    bSuppressLogs = true;

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PresetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/NoSuchPreset_%s.NoSuchPreset_%s"), *Stamp, *Stamp);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"),
        FString::Printf(TEXT("/Game/PinWrightTests/Seq_%s.Seq_%s"), *Stamp, *Stamp));
    Payload->SetStringField(TEXT("levelPath"),
        FString::Printf(TEXT("/Game/PinWrightTests/Map_%s.Map_%s"), *Stamp, *Stamp));
    Payload->SetStringField(TEXT("presetPath"), PresetPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("mrq.create_job responded"), Capture.bWasCalled);

    TestFalse(TEXT("a presetPath that will not load is refused, not queued"), Capture.bSuccess);
    TestEqual(TEXT("the refusal names the preset failure specifically"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_MRQ_PRESET_NOT_LOADABLE));

    // Asserted separately from bSuccess so a future handler that went back to answering success
    // still could not pass by staying quiet: whatever the outcome, nothing may report the preset
    // it failed to load as the preset this job carries.
    if (Capture.Result.IsValid())
    {
        FString EchoedPreset;
        const bool bEchoedTheUnloadablePreset =
            Capture.Result->TryGetStringField(TEXT("presetPath"), EchoedPreset)
            && EchoedPreset.Equals(PresetPath);
        TestFalse(TEXT("the response does not echo a preset that never loaded"),
            bEchoedTheUnloadablePreset);
    }

    // The other half of the claim, and the half a response-only check cannot see: an unconfigured
    // job left in the shared queue is a trap for anyone's later mrq.run_jobs.
    TestEqual(TEXT("nothing was added to the queue"), Queue->GetJobs().Num(), InitialJobs);

    // Restores the queue if the assertion above just failed — a red test must not also poison
    // every MRQ test that runs after it.
    PWMrqTrimQueueTo(Queue, InitialJobs);
    return true;
}

// Ask 2's other half: the caller can see what the job will produce BEFORE spending the render.
// The preset here is built in memory with a resolution and a filename format no default carries,
// so a `preflight` block that published constants, or that read the engine CDO instead of the
// queued job's own configuration, fails on the values rather than on their presence. That is also
// what makes this the honest replacement for a `presetApplied` boolean: the disclosure shows the
// preset's settings, so it cannot claim a preset took effect that did not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobPreflightDisclosesResolvedConfigTest,
    "PinWright.mrq.create_job.PreflightDisclosesTheResolvedConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobPreflightDisclosesResolvedConfigTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable on this host, so the "
                 "create_job pre-flight disclosure was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();

    // The happy path logs nothing; this covers the fallback below, where a host that cannot
    // resolve the in-memory fixture would otherwise be BOTH skipped and red from the loader's own
    // warnings. Every assertion here reads the captured response, never a log.
    bSuppressLogs = true;

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("PW_PreflightPreset_%s"), *Stamp);
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    // The fixture is RF_Standalone, so releasing the TStrongObjectPtr below is not enough to
    // reclaim it; detach it from its package too.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    UPackage* Package = CreatePackage(*PackageName);
    UMoviePipelinePrimaryConfig* Preset = Package
        ? NewObject<UMoviePipelinePrimaryConfig>(Package, UMoviePipelinePrimaryConfig::StaticClass(),
            *AssetName, RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (!Preset)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_fixture_uncreatable"),
            TEXT("An in-memory UMoviePipelinePrimaryConfig could not be created, so the pre-flight "
                 "disclosure was not compared against a known configuration."));
        return true;
    }
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> PresetGuard(Preset);

    // Values no CDO and no constant carries. 1920x1080, "{project_dir}/Saved/MovieRenders/" and
    // "{sequence_name}.{frame_number}" are the engine defaults, so a report that ignored the
    // queued job's configuration would still match those — and would fail every check below.
    const FString ExpectedDirectory = FString::Printf(TEXT("{project_dir}/Saved/PWPreflight_%s/"), *Stamp);
    const FString ExpectedFileName = FString::Printf(TEXT("PWShot_%s.{frame_number}"), *Stamp);
    UMoviePipelineOutputSetting* PresetOutput = Preset->FindSetting<UMoviePipelineOutputSetting>();
    if (!PresetOutput)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_preset_output_setting_absent"),
            TEXT("The fixture config carries no UMoviePipelineOutputSetting, so no distinctive "
                 "values could be planted for the pre-flight disclosure to be compared against."));
        return true;
    }
    PresetOutput->OutputResolution = FIntPoint(1234, 567);
    PresetOutput->OutputDirectory.Path = ExpectedDirectory;
    PresetOutput->FileNameFormat = ExpectedFileName;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"),
        FString::Printf(TEXT("/Game/PinWrightTests/Seq_%s.Seq_%s"), *Stamp, *Stamp));
    Payload->SetStringField(TEXT("levelPath"),
        FString::Printf(TEXT("/Game/PinWrightTests/Map_%s.Map_%s"), *Stamp, *Stamp));
    Payload->SetStringField(TEXT("presetPath"), FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    if (!Capture.bSuccess)
    {
        // A transient in-memory package is resolvable by StaticFindObject on every host this has
        // been exercised on, but the loader's behaviour is not this test's subject. Marked as a
        // skip rather than asserted, so a host where it does not resolve is visible in the log
        // instead of being a red that says nothing about the pre-flight block.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_in_memory_preset_unresolvable"),
            FString::Printf(TEXT("mrq.create_job refused the in-memory fixture preset [%s], so the "
                "pre-flight disclosure was not compared against a known configuration."),
                *Capture.ErrorCode));
        return true;
    }
    if (!TestTrue(TEXT("response has a result object"), Capture.Result.IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* Preflight = nullptr;
    if (!TestTrue(TEXT("the response carries a preflight block"),
        Capture.Result->TryGetObjectField(TEXT("preflight"), Preflight)
        && Preflight && Preflight->IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* Resolution = nullptr;
    if (TestTrue(TEXT("preflight discloses the resolution"),
        (*Preflight)->TryGetObjectField(TEXT("resolution"), Resolution)
        && Resolution && Resolution->IsValid()))
    {
        TestEqual(TEXT("the disclosed width is the preset's, not a default"),
            static_cast<int32>((*Resolution)->GetNumberField(TEXT("width"))), 1234);
        TestEqual(TEXT("the disclosed height is the preset's, not a default"),
            static_cast<int32>((*Resolution)->GetNumberField(TEXT("height"))), 567);
    }
    TestEqual(TEXT("the disclosed output directory is the preset's format string"),
        (*Preflight)->GetStringField(TEXT("outputDirectory")), ExpectedDirectory);
    TestEqual(TEXT("the disclosed file name format is the preset's"),
        (*Preflight)->GetStringField(TEXT("fileNameFormat")), ExpectedFileName);

    // The fixture carries no file writer, which is the case that renders for minutes and produces
    // nothing. `outputs` must say so and the warning must name it — an empty array with no warning
    // would be the same silent shape this ticket is about, one field further along.
    const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
    if (TestTrue(TEXT("preflight lists the outputs this job will write"),
        (*Preflight)->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs))
    {
        TestEqual(TEXT("a config with no file writer discloses an empty output list"),
            Outputs->Num(), 0);
    }
    bool bWarnedAboutWritingNothing = false;
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(TEXT("no files at all")))
            {
                bWarnedAboutWritingNothing = true;
            }
        }
    }
    TestTrue(TEXT("a job that will write nothing says so before the render is spent"),
        bWarnedAboutWritingNothing);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQListPresetsEmptySuccessShapeTest,
    "PinWright.mrq.list_presets.EmptySuccessShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQListPresetsEmptySuccessShapeTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("mrq.list_presets handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.list_presets"), Payload, Capture));
    TestTrue(TEXT("response captured"), Capture.bWasCalled);
    TestTrue(TEXT("success response"), Capture.bSuccess);
    // Assert the result object exists rather than skipping the shape checks when it
    // does not — a success with no payload must fail here, not sail past.
    if (!TestTrue(TEXT("response has a result object"), Capture.Result.IsValid()))
    {
        return true;
    }
    const TArray<TSharedPtr<FJsonValue>>* Presets = nullptr;
    TestTrue(TEXT("response has 'presets' array"),
        Capture.Result->TryGetArrayField(TEXT("presets"), Presets));
    double Count = -1.0;
    TestTrue(TEXT("response has 'count'"), Capture.Result->TryGetNumberField(TEXT("count"), Count));
    if (Presets)
    {
        // count must describe presets, not be an independent (stale/constant) number.
        TestEqual(TEXT("count matches the presets array length"),
            static_cast<int32>(Count), Presets->Num());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobRejectsGuidNamedNonexistentAssetsTest,
    "PinWright.mrq.create_job.GuidNamedNonexistentAssetsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobRejectsGuidNamedNonexistentAssetsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable — skipping"));
        return true;
    }
    UMoviePipelineQueueSubsystem* QSS = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
    if (!QSS || !QSS->GetQueue())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq-subsystem-unavailable"),
            TEXT("MoviePipelineQueueSubsystem unavailable — skipping"));
        return true;
    }
    UMoviePipelineQueue* Queue = QSS->GetQueue();
    const int32 InitialJobs = Queue->GetJobs().Num();

    // These GUID-shaped paths are syntactically valid but intentionally do not resolve to assets.
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SequencePath = FString::Printf(TEXT("/Game/PWTest/Seq_%s.Seq_%s"), *Stamp, *Stamp);
    const FString LevelPath    = FString::Printf(TEXT("/Game/PWTest/Map_%s.Map_%s"), *Stamp, *Stamp);
    const FString JobName      = FString::Printf(TEXT("PWJob_%s"), *Stamp);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), SequencePath);
    Payload->SetStringField(TEXT("levelPath"),    LevelPath);
    Payload->SetStringField(TEXT("jobName"),      JobName);

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("nonexistent asset request responded"), Capture.bWasCalled);
    TestFalse(TEXT("GUID-named nonexistent assets are refused"), Capture.bSuccess);
    TestEqual(TEXT("GUID-named nonexistent assets report ASSET_NOT_FOUND"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestEqual(TEXT("GUID-named nonexistent assets leave the queue unchanged"),
        Queue->GetJobs().Num(), InitialJobs);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQListJobsReportsQueueEntriesTest,
    "PinWright.mrq.list_jobs.ReportsQueueEntries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQListJobsReportsQueueEntriesTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so the queue list "
                 "shape was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    UMoviePipelineExecutorJob* Fixture = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Fixture)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate a fixture job."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString JobName = FString::Printf(TEXT("PWList_%s"), *Stamp);
    const FString SequencePath = FString::Printf(TEXT("/Game/PWTest/ListSeq_%s.ListSeq_%s"), *Stamp, *Stamp);
    const FString MapPath = FString::Printf(TEXT("/Game/PWTest/ListMap_%s.ListMap_%s"), *Stamp, *Stamp);
    Fixture->JobName = JobName;
    Fixture->SetSequence(FSoftObjectPath(SequencePath));
    Fixture->Map = FSoftObjectPath(MapPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.list_jobs handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.list_jobs"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("mrq.list_jobs responded"), Capture.bWasCalled);
    TestTrue(TEXT("mrq.list_jobs succeeded"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("mrq.list_jobs returned no result object"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Jobs = nullptr;
    if (!TestTrue(TEXT("response carries jobs array"),
        Capture.Result->TryGetArrayField(TEXT("jobs"), Jobs) && Jobs))
    {
        return true;
    }
    TestEqual(TEXT("jobs array includes the fixture"), Jobs->Num(), InitialJobs + 1);
    double Count = -1.0;
    TestTrue(TEXT("response carries count"), Capture.Result->TryGetNumberField(TEXT("count"), Count));
    TestEqual(TEXT("count matches jobs array"), static_cast<int32>(Count), Jobs->Num());

    if (Jobs->IsValidIndex(InitialJobs))
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (TestTrue(TEXT("fixture entry is an object"),
            (*Jobs)[InitialJobs]->TryGetObject(Entry) && Entry && Entry->IsValid()))
        {
            double Index = -1.0;
            TestTrue(TEXT("entry carries index"), (*Entry)->TryGetNumberField(TEXT("index"), Index));
            TestEqual(TEXT("entry index addresses the fixture"), static_cast<int32>(Index), InitialJobs);
            TestEqual(TEXT("entry carries jobName"), (*Entry)->GetStringField(TEXT("jobName")), JobName);
            TestTrue(TEXT("entry carries enabled state"), (*Entry)->HasField(TEXT("enabled")));
            TestEqual(TEXT("entry carries sequencePath"), (*Entry)->GetStringField(TEXT("sequencePath")), SequencePath);
            TestEqual(TEXT("entry carries mapPath"), (*Entry)->GetStringField(TEXT("mapPath")), MapPath);
            TestTrue(TEXT("entry carries configurationPath"), (*Entry)->HasField(TEXT("configurationPath")));
            TestTrue(TEXT("entry carries resolved outputDirectory"), (*Entry)->HasField(TEXT("outputDirectory")));
            TestTrue(TEXT("entry carries resolved fileNameFormat"), (*Entry)->HasField(TEXT("fileNameFormat")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobDisclosesQueuedJobsTest,
    "PinWright.mrq.create_job.DisclosesPreviouslyQueuedJobs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobDisclosesQueuedJobsTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so queued-job "
                 "disclosure was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    UMoviePipelineExecutorJob* Existing = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!Existing)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate a pre-existing fixture job."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ExistingName = FString::Printf(TEXT("PWExisting_%s"), *Stamp);
    Existing->JobName = ExistingName;
    Existing->SetSequence(FSoftObjectPath(FString::Printf(
        TEXT("/Game/PWTest/ExistingSeq_%s.ExistingSeq_%s"), *Stamp, *Stamp)));
    Existing->Map = FSoftObjectPath(FString::Printf(
        TEXT("/Game/PWTest/ExistingMap_%s.ExistingMap_%s"), *Stamp, *Stamp));

    const FString FixturePackageName = FString::Printf(TEXT("/Temp/PinWrightTests/%s"), *Stamp);
    UPackage* SequencePackage = CreatePackage(*FString::Printf(TEXT("%s_Sequence"), *FixturePackageName));
    ULevelSequence* Sequence = SequencePackage
        ? NewObject<ULevelSequence>(SequencePackage, ULevelSequence::StaticClass(),
            *FString::Printf(TEXT("Sequence_%s"), *Stamp), RF_Public | RF_Standalone | RF_Transient)
        : nullptr;
    if (Sequence)
    {
        Sequence->Initialize();
    }
    UPackage* MapPackage = CreatePackage(*FString::Printf(TEXT("%s_Map"), *FixturePackageName));
    UWorld* TransientMap = MapPackage
        ? UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false,
            FName(*FString::Printf(TEXT("Map_%s"), *Stamp)), MapPackage)
        : nullptr;
    FScopedTransientWorldGuard MapGuard(TransientMap);
    if (!Sequence || !TransientMap)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_target_fixture_uncreatable"),
            TEXT("Transient sequence/map fixtures could not be created, so queued-job disclosure "
                 "was not exercised."));
        return true;
    }
    TStrongObjectPtr<ULevelSequence> SequenceGuard(Sequence);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
    Payload->SetStringField(TEXT("levelPath"), TransientMap->GetPathName());
    Payload->SetStringField(TEXT("jobName"), FString::Printf(TEXT("PWNew_%s"), *Stamp));

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture));
    TestTrue(TEXT("mrq.create_job succeeded"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("mrq.create_job returned no result object"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* QueuedJobs = nullptr;
    if (TestTrue(TEXT("response carries queuedJobs array"),
        Capture.Result->TryGetArrayField(TEXT("queuedJobs"), QueuedJobs) && QueuedJobs))
    {
        TestEqual(TEXT("queuedJobs excludes the job created by this call"),
            QueuedJobs->Num(), InitialJobs + 1);
        bool bFoundExisting = false;
        for (const TSharedPtr<FJsonValue>& Value : *QueuedJobs)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && Entry->IsValid()
                && (*Entry)->GetStringField(TEXT("jobName")) == ExistingName)
            {
                bFoundExisting = true;
                break;
            }
        }
        TestTrue(TEXT("queuedJobs identifies the pre-existing job"), bFoundExisting);
    }

    bool bWarned = false;
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Warning;
            if (Value.IsValid() && Value->TryGetString(Warning)
                && Warning.Contains(TEXT("queuedJobs"))
                && Warning.Contains(TEXT("mrq.run_jobs")))
            {
                bWarned = true;
                break;
            }
        }
    }
    TestTrue(TEXT("pre-existing jobs produce a run_jobs warning"), bWarned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsRejectsEmptySelectionTest,
    "PinWright.mrq.run_jobs.RejectsEmptySelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsRejectsEmptySelectionTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so selection "
                 "validation was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    if (InitialJobs == 0 && !Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate a selection fixture job."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("jobs"), TArray<TSharedPtr<FJsonValue>>());
    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.run_jobs handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), Payload, Capture));
    TestTrue(TEXT("mrq.run_jobs responded"), Capture.bWasCalled);
    TestFalse(TEXT("empty selection is refused before starting a render"), Capture.bSuccess);
    TestEqual(TEXT("empty selection reports INVALID_ARGUMENT"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

// Counterfactual: without the production guard, the abstract-base request reaches StartJob and
// NewObject before invoking UMoviePipelineExecutorBase's fatal pure-virtual Execute body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsRejectsInvalidExecutorClassesTest,
    "PinWright.mrq.run_jobs.RejectsInvalidExecutorClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsRejectsInvalidExecutorClassesTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so executor-class "
                 "validation was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    if (InitialJobs == 0 && !Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate an executor-validation fixture "
                 "job."));
        return true;
    }
    const int32 FixtureJobs = Queue->GetJobs().Num();
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };
    bSuppressLogs = true;

    TSharedPtr<FJsonObject> AbstractPayload = MakeShared<FJsonObject>();
    AbstractPayload->SetStringField(TEXT("executorClass"),
        UMoviePipelineExecutorBase::StaticClass()->GetPathName());
    FTestResponseCapture AbstractCapture;
    TestTrue(TEXT("mrq.run_jobs handler found for abstract executor"),
        InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), AbstractPayload, AbstractCapture));
    TestTrue(TEXT("abstract executor request responded"), AbstractCapture.bWasCalled);
    TestFalse(TEXT("abstract executor is refused before the render starts"),
        AbstractCapture.bSuccess);
    TestEqual(TEXT("abstract executor reports INVALID_EXECUTOR_CLASS"),
        AbstractCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_EXECUTOR_CLASS));
    TestEqual(TEXT("abstract executor refusal leaves the queue unchanged"),
        Queue->GetJobs().Num(), FixtureJobs);

    const FString MissingClassPath = FString::Printf(
        TEXT("/Script/PinWright.NoSuchMRQExecutor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> MissingPayload = MakeShared<FJsonObject>();
    MissingPayload->SetStringField(TEXT("executorClass"), MissingClassPath);
    FTestResponseCapture MissingCapture;
    TestTrue(TEXT("mrq.run_jobs handler found for missing executor"),
        InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), MissingPayload, MissingCapture));
    TestTrue(TEXT("missing executor request responded"), MissingCapture.bWasCalled);
    TestFalse(TEXT("missing executor is refused before the render starts"),
        MissingCapture.bSuccess);
    TestEqual(TEXT("missing executor reports CLASS_NOT_FOUND"),
        MissingCapture.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_FOUND));
    TestEqual(TEXT("missing executor refusal leaves the queue unchanged"),
        Queue->GetJobs().Num(), FixtureJobs);
    return true;
}

// Counterfactual: before the terminal content gate, the production decoder marked a black PNG as
// suspect but the job registry still completed the request successfully. The injected executor
// result supplies only the file path; decoding, channel measurement, classification and terminal
// job completion all run through the shipped mrq.run_jobs path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsUnrenderableFramesFailTerminalJobTest,
    "PinWright.mrq.run_jobs.UnrenderableFramesFailTerminalJob",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsUnrenderableFramesFailTerminalJobTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue || QSS->IsRendering())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable_or_rendering"),
            TEXT("The MRQ queue is unavailable or already rendering, so the injected terminal "
                 "result was not exercised."));
        return true;
    }

    const int32 InitialJobs = Queue->GetJobs().Num();
    UMoviePipelineExecutorJob* FixtureJob =
        Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!FixtureJob)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate the unrenderable-frame fixture "
                 "job."));
        return true;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir() / TEXT("MRQRunJobsTests")
        / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlackFramePath = ScratchRoot / TEXT("Black_0000.png");
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    };
    FixtureJob->JobName = TEXT("PW_MRQ_BlackFrame");
    if (!TestTrue(TEXT("a real black PNG was written for the injected executor result"),
        PWMrqWriteSolidPng(BlackFramePath, 64, FColor::Black)))
    {
        return true;
    }

    PinWrightMRQHandlerTestHooks::FInjectedExecutorResult Injected;
    Injected.Files.Add(PinWrightMRQ::FRenderedFile{ BlackFramePath, TEXT("FinalImage") });
    Injected.EncodeContext.Width = 64;
    Injected.EncodeContext.Height = 64;
    Injected.JobName = FixtureJob->JobName;
    PinWrightMRQHandlerTestHooks::FScopedInjectedExecutorResult InjectResult(Injected);

    auto Invoke = [this, InitialJobs](bool bAllowUnrenderableFrames, FJobTicket& OutTicket)
    {
        TArray<TSharedPtr<FJsonValue>> RequestedJobs;
        RequestedJobs.Add(MakeShared<FJsonValueNumber>(InitialJobs));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("jobs"), RequestedJobs);
        Payload->SetStringField(TEXT("executorClass"),
            UTestMRQSelectionExecutor::StaticClass()->GetPathName());
        if (bAllowUnrenderableFrames)
        {
            Payload->SetBoolField(TEXT("allowUnrenderableFrames"), true);
        }

        FTestResponseCapture Capture;
        TestTrue(TEXT("mrq.run_jobs handler accepted the injected result request"),
            InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), Payload, Capture));
        if (!TestTrue(TEXT("the immediate response is a job ticket"),
            Capture.bSuccess && Capture.Result.IsValid()))
        {
            return false;
        }
        FString TicketId;
        if (!TestTrue(TEXT("the immediate response names the ticket"),
            Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId)))
        {
            return false;
        }
        return TestTrue(TEXT("the synchronous executor completed the ticket in the registry"),
            FPluginState::Get().GetJobRegistry().Get(TicketId, OutTicket));
    };

    FJobTicket RefusedTicket;
    if (!Invoke(false, RefusedTicket))
    {
        return true;
    }
    TestEqual(TEXT("uniform output fails the terminal job by default"),
        RefusedTicket.Status, FString(TEXT("failed")));
    TestEqual(TEXT("the registry carries the typed failure"), RefusedTicket.Error,
        FString(ErrorCodes::ERR_RENDER_UNRENDERABLE_FRAMES));
    if (!TestTrue(TEXT("the failed ticket retains measured artifact evidence"),
        RefusedTicket.Result.IsValid()))
    {
        return true;
    }
    TestFalse(TEXT("the terminal result does not report success"),
        RefusedTicket.Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("the result repeats the typed error code"),
        RefusedTicket.Result->GetStringField(TEXT("errorCode")),
        FString(ErrorCodes::ERR_RENDER_UNRENDERABLE_FRAMES));
    TestTrue(TEXT("the terminal result says unrenderable frames were detected"),
        RefusedTicket.Result->GetBoolField(TEXT("unrenderableFramesDetected")));

    const TArray<TSharedPtr<FJsonValue>>* Jobs = nullptr;
    if (TestTrue(TEXT("the result carries the injected job report"),
        RefusedTicket.Result->TryGetArrayField(TEXT("jobs"), Jobs) && Jobs && Jobs->Num() == 1))
    {
        const TSharedPtr<FJsonObject>* Job = nullptr;
        if (TestTrue(TEXT("the injected job report is an object"),
            (*Jobs)[0]->TryGetObject(Job) && Job && Job->IsValid()))
        {
            TestEqual(TEXT("its analyzed frame is counted as unrenderable"),
                static_cast<int32>((*Job)->GetNumberField(TEXT("framesUnrenderable"))), 1);
            const TArray<TSharedPtr<FJsonValue>>* OutputFiles = nullptr;
            if (TestTrue(TEXT("the black frame is retained in outputFiles"),
                (*Job)->TryGetArrayField(TEXT("outputFiles"), OutputFiles)
                    && OutputFiles && OutputFiles->Num() == 1))
            {
                const TSharedPtr<FJsonObject>* Frame = nullptr;
                if (TestTrue(TEXT("the black frame entry is an object"),
                    (*OutputFiles)[0]->TryGetObject(Frame) && Frame && Frame->IsValid()))
                {
                    TestTrue(TEXT("the frame is classified as uniform"),
                        (*Frame)->GetBoolField(TEXT("uniformColor")));
                    TestTrue(TEXT("the frame is classified as unrenderable"),
                        (*Frame)->GetBoolField(TEXT("unrenderable")));
                    const TSharedPtr<FJsonObject>* Stats = nullptr;
                    if (TestTrue(TEXT("the frame publishes measured channel statistics"),
                        (*Frame)->TryGetObjectField(TEXT("imageStats"), Stats)
                            && Stats && Stats->IsValid()))
                    {
                        TestEqual(TEXT("black mean red is measured"),
                            (*Stats)->GetNumberField(TEXT("meanRed")), 0.0);
                        TestEqual(TEXT("black mean green is measured"),
                            (*Stats)->GetNumberField(TEXT("meanGreen")), 0.0);
                        TestEqual(TEXT("black mean blue is measured"),
                            (*Stats)->GetNumberField(TEXT("meanBlue")), 0.0);
                        TestEqual(TEXT("black red variance is measured"),
                            (*Stats)->GetNumberField(TEXT("redVariance")), 0.0);
                        TestEqual(TEXT("black green variance is measured"),
                            (*Stats)->GetNumberField(TEXT("greenVariance")), 0.0);
                        TestEqual(TEXT("black blue variance is measured"),
                            (*Stats)->GetNumberField(TEXT("blueVariance")), 0.0);
                    }
                }
            }
        }
    }

    FJobTicket AllowedTicket;
    if (Invoke(true, AllowedTicket))
    {
        TestEqual(TEXT("the explicit opt-in allows intentional uniform output"),
            AllowedTicket.Status, FString(TEXT("completed")));
        if (TestTrue(TEXT("the allowed ticket retains its result"), AllowedTicket.Result.IsValid()))
        {
            TestTrue(TEXT("the allowed result reports success"),
                AllowedTicket.Result->GetBoolField(TEXT("success")));
            TestTrue(TEXT("the allowed result still exposes the detection"),
                AllowedTicket.Result->GetBoolField(TEXT("unrenderableFramesDetected")));
            TestTrue(TEXT("the opt-in is named in a warning"),
                AllowedTicket.Result->HasField(TEXT("unrenderableFramesWarning")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsExecutorFailureIsTerminalTest,
    "PinWright.mrq.run_jobs.ExecutorFailureIsTerminal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsExecutorFailureIsTerminalTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue || QSS->IsRendering())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable_or_rendering"),
            TEXT("The MRQ queue is unavailable or already rendering, so executor failure "
                 "propagation was not exercised."));
        return true;
    }

    const int32 InitialJobs = Queue->GetJobs().Num();
    UMoviePipelineExecutorJob* FixtureJob =
        Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!FixtureJob)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate the executor-failure fixture "
                 "job."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    FixtureJob->JobName = TEXT("PW_MRQ_ExecutorFailure");
    const FString FailureMessage = TEXT("Injected executor failure: output initialization failed");
    PinWrightMRQHandlerTestHooks::FInjectedExecutorResult Injected;
    Injected.bExecutorFailed = true;
    Injected.bExecutorFailureFatal = true;
    Injected.ExecutorFailureMessage = FailureMessage;
    PinWrightMRQHandlerTestHooks::FScopedInjectedExecutorResult InjectResult(Injected);

    TArray<TSharedPtr<FJsonValue>> RequestedJobs;
    RequestedJobs.Add(MakeShared<FJsonValueNumber>(InitialJobs));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("jobs"), RequestedJobs);
    Payload->SetStringField(TEXT("executorClass"),
        UTestMRQSelectionExecutor::StaticClass()->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.run_jobs handler accepted the executor failure request"),
        InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), Payload, Capture));
    if (!TestTrue(TEXT("the immediate response is a job ticket"),
        Capture.bSuccess && Capture.Result.IsValid()))
    {
        return true;
    }

    FString TicketId;
    if (!TestTrue(TEXT("the immediate response names the ticket"),
        Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId)))
    {
        return true;
    }
    FJobTicket Ticket;
    if (!TestTrue(TEXT("the synchronous executor completed the ticket in the registry"),
        FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)))
    {
        return true;
    }

    TestEqual(TEXT("executor failure fails the terminal job"), Ticket.Status,
        FString(TEXT("failed")));
    TestEqual(TEXT("the registry carries MRQ_EXECUTOR_FAILED"), Ticket.Error,
        FString(ErrorCodes::ERR_MRQ_EXECUTOR_FAILED));
    if (TestTrue(TEXT("the failed ticket retains the executor result"), Ticket.Result.IsValid()))
    {
        TestFalse(TEXT("the terminal result does not report success"),
            Ticket.Result->GetBoolField(TEXT("success")));
        TestEqual(TEXT("the result repeats the typed executor error code"),
            Ticket.Result->GetStringField(TEXT("errorCode")),
            FString(ErrorCodes::ERR_MRQ_EXECUTOR_FAILED));
        const FString ReportedMessage = Ticket.Result->GetStringField(TEXT("error"));
        TestTrue(TEXT("the executor failure message is nonempty"), !ReportedMessage.IsEmpty());
        TestEqual(TEXT("the result preserves the injected executor status message"),
            ReportedMessage, FailureMessage);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRemoveJobRemovesIndexedEntryTest,
    "PinWright.mrq.remove_job.RemovesIndexedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRemoveJobRemovesIndexedEntryTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so removal was "
                 "not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    if (!Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate a removal fixture job."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("jobIndex"), InitialJobs);
    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.remove_job handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.remove_job"), Payload, Capture));
    TestTrue(TEXT("mrq.remove_job succeeded"), Capture.bSuccess);
    TestEqual(TEXT("the fixture was removed"), Queue->GetJobs().Num(), InitialJobs);
    if (Capture.Result.IsValid())
    {
        double RemovedIndex = -1.0;
        TestTrue(TEXT("response carries removedIndex"),
            Capture.Result->TryGetNumberField(TEXT("removedIndex"), RemovedIndex));
        TestEqual(TEXT("response identifies the removed index"),
            static_cast<int32>(RemovedIndex), InitialJobs);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQClearQueueRemovesJobsTest,
    "PinWright.mrq.clear_queue.RemovesJobs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQClearQueueRemovesJobsTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so clear_queue "
                 "registration was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    if (InitialJobs != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_not_empty"),
            TEXT("clear_queue is destructive shared editor state; this behavior test only runs "
                 "when the queue is initially empty."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };
    if (!Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate a clear_queue fixture job."));
        return true;
    }
    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.clear_queue handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.clear_queue"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("mrq.clear_queue succeeded with queued jobs"), Capture.bSuccess);
    TestEqual(TEXT("clear_queue removes every fixture job"), Queue->GetJobs().Num(), 0);
    if (Capture.Result.IsValid())
    {
        double RemovedCount = -1.0;
        double QueueSize = -1.0;
        TestTrue(TEXT("response carries removedCount"),
            Capture.Result->TryGetNumberField(TEXT("removedCount"), RemovedCount));
        TestTrue(TEXT("response carries queueSize"),
            Capture.Result->TryGetNumberField(TEXT("queueSize"), QueueSize));
        TestEqual(TEXT("clear_queue reports the removed job"), static_cast<int32>(RemovedCount), 1);
        TestEqual(TEXT("clear_queue reports an empty queue"), static_cast<int32>(QueueSize), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsSelectionBehaviorTest,
    "PinWright.mrq.run_jobs.FiltersSelectedJobs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsSelectionBehaviorTest::RunTest(const FString& Parameters)
{
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable, so selected-job "
                 "filtering was not exercised."));
        return true;
    }
    const int32 InitialJobs = Queue->GetJobs().Num();
    if (InitialJobs != 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_not_empty"),
            TEXT("This behavior test uses the editor-global queue and only runs when it is "
                 "initially empty."));
        return true;
    }
    ON_SCOPE_EXIT
    {
        PWMrqTrimQueueTo(Queue, InitialJobs);
    };
    UMoviePipelineExecutorJob* First = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    UMoviePipelineExecutorJob* Selected = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    if (!First || !Selected)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_fixture_job_uncreatable"),
            TEXT("The editor-global MRQ queue could not allocate two selection fixture jobs."));
        return true;
    }
    First->JobName = TEXT("PW_MRQ_Unselected");
    Selected->JobName = TEXT("PW_MRQ_Selected");
    First->SetIsEnabled(false);
    Selected->SetIsEnabled(false);

    UTestMRQSelectionExecutor::ResetObservation();
    TArray<TSharedPtr<FJsonValue>> RequestedJobs;
    RequestedJobs.Add(MakeShared<FJsonValueNumber>(1));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("jobs"), RequestedJobs);
    Payload->SetStringField(TEXT("executorClass"),
        UTestMRQSelectionExecutor::StaticClass()->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.run_jobs handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.run_jobs"), Payload, Capture));
    TestTrue(TEXT("mrq.run_jobs starts the synchronous test executor"), Capture.bSuccess);
    TestEqual(TEXT("test executor receives only one selected job"),
        UTestMRQSelectionExecutor::ObservedJobNames.Num(), 1);
    if (UTestMRQSelectionExecutor::ObservedJobNames.Num() == 1)
    {
        TestEqual(TEXT("test executor receives the requested job"),
            UTestMRQSelectionExecutor::ObservedJobNames[0], Selected->JobName);
        TestTrue(TEXT("selected copy is enabled for the executor"),
            UTestMRQSelectionExecutor::ObservedJobEnabled.IsValidIndex(0)
            && UTestMRQSelectionExecutor::ObservedJobEnabled[0]);
    }
    TestEqual(TEXT("selection leaves the shared queue unchanged"), Queue->GetJobs().Num(), 2);
    TestFalse(TEXT("unselected shared job keeps its original enabled state"), First->IsEnabled());
    TestFalse(TEXT("selected shared job keeps its original enabled state"), Selected->IsEnabled());
    return true;
}

// Structural ratchet for the only behavior not safely observable without a long-running real
// render: the completion delegate must restore the temporary selected-copy state. This reads
// source only and never starts PIE.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQRunJobsSelectionRestorationStructureTest,
    "PinWright.mrq.run_jobs.RestoresSelectedCopyOnCompletion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQRunJobsSelectionRestorationStructureTest::RunTest(const FString& Parameters)
{
    const FString HandlerPath = FPaths::Combine(FPaths::ProjectPluginsDir(),
        TEXT("PinWright/Source/PinWright/Private/Handlers/MRQ/MRQHandler.cpp"));
    FString HandlerSource;
    if (!FFileHelper::LoadFileToString(HandlerSource, *HandlerPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_handler_source_unreadable"),
            FString::Printf(TEXT("Could not read the MRQ handler source at %s for the structural "
                "completion-restoration ratchet."), *HandlerPath));
        return true;
    }

    const int32 FinishPosition = HandlerSource.Find(TEXT("Exec->OnExecutorFinished().AddLambda("));
    const int32 RestorePosition = FinishPosition != INDEX_NONE
        ? HandlerSource.Find(TEXT("RestoreEnabledStates();"),
            ESearchCase::CaseSensitive, ESearchDir::FromStart, FinishPosition)
        : INDEX_NONE;
    TestTrue(TEXT("enabled state restoration is present in the finish delegate"),
        FinishPosition != INDEX_NONE && RestorePosition != INDEX_NONE
        && RestorePosition > FinishPosition);
    return true;
}

#else // !MCP_HAS_MRQ

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQListPresetsUnavailableReportsErrorTest,
    "PinWright.mrq.list_presets.UnavailableReportsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQListPresetsUnavailableReportsErrorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("mrq.list_presets handler found"),
        InvokeHandlerWithCapture(TEXT("mrq.list_presets"), Payload, Capture));
    TestTrue(TEXT("response captured"), Capture.bWasCalled);
    TestFalse(TEXT("error response when MRQ plugin disabled"), Capture.bSuccess);
    TestEqual(TEXT("error code is MRQ_NOT_AVAILABLE"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_MRQ_NOT_AVAILABLE));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQQueueManagementUnavailableReportsErrorTest,
    "PinWright.mrq.queue_management.UnavailableReportsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQQueueManagementUnavailableReportsErrorTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Methods = {
        TEXT("mrq.list_jobs"), TEXT("mrq.remove_job"), TEXT("mrq.clear_queue")};
    for (const FString& Method : Methods)
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (Method == TEXT("mrq.remove_job"))
        {
            Payload->SetNumberField(TEXT("jobIndex"), 0);
        }
        TestTrue(FString::Printf(TEXT("%s handler found"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(FString::Printf(TEXT("%s responded"), *Method), Capture.bWasCalled);
        TestFalse(FString::Printf(TEXT("%s reports unavailable"), *Method), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code"), *Method), Capture.ErrorCode,
            FString(ErrorCodes::ERR_MRQ_NOT_AVAILABLE));
    }
    return true;
}

#endif // MCP_HAS_MRQ
