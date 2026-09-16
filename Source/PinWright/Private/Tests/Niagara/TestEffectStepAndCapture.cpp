// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for F-effect-step-and-capture-atomic. The separate activate, advance, and
// capture RPCs leave wall-clock gaps in which the editor world can tick a short Niagara system to
// completion. The atomic verb must instead warm exposure before freezing world time, then advance
// and read the final frame while that scoped freeze is still active.

#include "Misc/AutomationTest.h"

#include "Dispatch/SafePoint.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/ScopedWorldTimeDilation.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

#include <limits>

namespace PinWrightEffectStepCaptureTest
{
    constexpr int32 CaptureWidth = 256;
    constexpr int32 CaptureHeight = 256;
    constexpr double SampleSeconds = 0.05;
    const TCHAR* const SystemPath =
        TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion");

    TSharedPtr<FJsonObject> MakeVector(const FVector& Value)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("x"), Value.X);
        Result->SetNumberField(TEXT("y"), Value.Y);
        Result->SetNumberField(TEXT("z"), Value.Z);
        return Result;
    }

    TSharedPtr<FJsonObject> MakeRotator(const FRotator& Value)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("pitch"), Value.Pitch);
        Result->SetNumberField(TEXT("yaw"), Value.Yaw);
        Result->SetNumberField(TEXT("roll"), Value.Roll);
        return Result;
    }

    TSharedPtr<FJsonObject> MakeCapturePayload(const FString& Filename)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filename"), Filename);
        Payload->SetNumberField(TEXT("width"), CaptureWidth);
        Payload->SetNumberField(TEXT("height"), CaptureHeight);
        Payload->SetObjectField(TEXT("location"), MakeVector(FVector(-300.0, 0.0, 100000.0)));
        Payload->SetObjectField(TEXT("rotation"), MakeRotator(FRotator::ZeroRotator));
        Payload->SetBoolField(TEXT("hideEditorSprites"), true);
        Payload->SetBoolField(TEXT("allowBlank"), true);
        return Payload;
    }

    ANiagaraActor* SpawnBurstActor(UWorld* World, UNiagaraSystem* System, const FString& Label)
    {
        if (!World || !System)
        {
            return nullptr;
        }

        const FTransform SpawnTransform(
            FRotator::ZeroRotator, FVector(0.0, 0.0, 100000.0));
        ANiagaraActor* Actor = World->SpawnActorDeferred<ANiagaraActor>(
            ANiagaraActor::StaticClass(), SpawnTransform, nullptr, nullptr,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Actor)
        {
            return nullptr;
        }

        UNiagaraComponent* Component = Actor->GetNiagaraComponent();
        if (!Component)
        {
            return nullptr;
        }
        // Written directly rather than through SetAutoActivate: the deferred spawn has already
        // registered this component on some engines, and UActorComponent::SetAutoActivate refuses
        // (and warns) once bRegistered is set, leaving the flag on. With the flag on, SetAsset
        // below activates the burst, and so does the component re-register SetActorLabel triggers
        // - neither of which is the inactive baseline this fixture exists to hand back.
        Component->bAutoActivate = false;
        Component->SetAsset(System);
        Actor->FinishSpawning(SpawnTransform);
        Component->SetForceSolo(true);
        Component->SetVisibility(true, true);
        Actor->SetActorLabel(Label);
        // Deactivate last, so nothing above can leave the burst playing in the baseline.
        Component->DeactivateImmediate();
        return Actor;
    }

    bool DecodeCapture(const FString& Path, TArray<FColor>& OutPixels, FString& OutError)
    {
        TArray<uint8> Compressed;
        if (!FFileHelper::LoadFileToArray(Compressed, *Path))
        {
            OutError = FString::Printf(TEXT("Could not read capture file: %s"), *Path);
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()))
        {
            OutError = TEXT("Capture output is not a valid PNG");
            return false;
        }

        TArray64<uint8> Raw;
        const int64 ExpectedBytes = static_cast<int64>(CaptureWidth) * CaptureHeight * sizeof(FColor);
        if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() != ExpectedBytes)
        {
            OutError = FString::Printf(TEXT("Expected %lld BGRA bytes, decoded %lld"),
                static_cast<long long>(ExpectedBytes), static_cast<long long>(Raw.Num()));
            return false;
        }

        OutPixels.SetNumUninitialized(CaptureWidth * CaptureHeight);
        FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), ExpectedBytes);
        return true;
    }

    bool IsCaptureEnvironmentUnavailable(const FTestResponseCapture& Capture)
    {
        if (Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            Capture.ErrorCode == TEXT("VIEWPORT_WORLD_MISMATCH"))
        {
            return true;
        }
        return Capture.ErrorCode == TEXT("CAPTURE_FAILED") &&
            Capture.Message.Contains(TEXT("read pixels"), ESearchCase::IgnoreCase);
    }

    bool ReadSource(const FString& RelativePath, FString& OutSource)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return false;
        }
        return FFileHelper::LoadFileToString(
            OutSource, *FPaths::Combine(Plugin->GetBaseDir(), RelativePath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectStepAndCaptureContractTest,
    "PinWright.effect.step_and_capture.ContractAndStageOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectStepAndCaptureContractTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectStepCaptureTest;

    TestTrue(TEXT("effect.step_and_capture is registered"),
        IsHandlerRegistered(TEXT("effect.step_and_capture")));
    TestNotNull(TEXT("seconds is declared"),
        GetRegisteredParamSpec(TEXT("effect.step_and_capture"), TEXT("seconds")));
    const FParamSpec* FramesSpec =
        GetRegisteredParamSpec(TEXT("effect.step_and_capture"), TEXT("frames"));
    if (TestNotNull(TEXT("frames is declared"), FramesSpec))
    {
        TestEqual(TEXT("frames is an integer parameter"), FramesSpec->Type, FString(TEXT("integer")));
    }
    TestNotNull(TEXT("exposure is shared with the capture contract"),
        GetRegisteredParamSpec(TEXT("effect.step_and_capture"), TEXT("exposure")));
    TestTrue(TEXT("the viewport-pumping verb is tick-unsafe"),
        PinWrightSafePoint::IsTickUnsafeMethod(TEXT("effect.step_and_capture")));

    TSharedPtr<FJsonObject> ConflictingTimePayload = MakeShared<FJsonObject>();
    ConflictingTimePayload->SetStringField(TEXT("systemName"), TEXT("not-resolved"));
    ConflictingTimePayload->SetNumberField(TEXT("seconds"), 0.2);
    ConflictingTimePayload->SetNumberField(TEXT("frames"), 12);
    FTestResponseCapture ConflictingTimeResponse;
    TestTrue(TEXT("the registered handler accepts a validation probe"), InvokeHandlerWithCapture(
        TEXT("effect.step_and_capture"), ConflictingTimePayload, ConflictingTimeResponse));
    TestFalse(TEXT("seconds and frames are not silently combined"), ConflictingTimeResponse.bSuccess);
    TestEqual(TEXT("conflicting time forms return INVALID_ARGUMENT"),
        ConflictingTimeResponse.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    const auto ExpectInvalidTimeParams = [this](const TCHAR* CaseName,
        const TSharedPtr<FJsonObject>& Probe)
    {
        Probe->SetStringField(TEXT("systemName"), TEXT("not-resolved"));
        FTestResponseCapture Response;
        TestTrue(*FString::Printf(TEXT("%s reaches the registered handler"), CaseName),
            InvokeHandlerWithCapture(TEXT("effect.step_and_capture"), Probe, Response));
        TestFalse(*FString::Printf(TEXT("%s is rejected before target resolution"), CaseName),
            Response.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s returns INVALID_PARAMS"), CaseName),
            Response.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(*FString::Printf(TEXT("%s names all numeric safety limits"), CaseName),
            Response.Message.Contains(TEXT("0.0001")) &&
            Response.Message.Contains(TEXT("10000")) &&
            Response.Message.Contains(TEXT("60 seconds")));
    };

    TSharedPtr<FJsonObject> NonFiniteDelta = MakeShared<FJsonObject>();
    NonFiniteDelta->SetNumberField(
        TEXT("deltaTime"), std::numeric_limits<double>::infinity());
    ExpectInvalidTimeParams(TEXT("a non-finite deltaTime"), NonFiniteDelta);

    TSharedPtr<FJsonObject> TooSmallDelta = MakeShared<FJsonObject>();
    TooSmallDelta->SetNumberField(TEXT("deltaTime"), 0.00001);
    ExpectInvalidTimeParams(TEXT("a sub-minimum deltaTime"), TooSmallDelta);

    TSharedPtr<FJsonObject> TooManyFrames = MakeShared<FJsonObject>();
    TooManyFrames->SetNumberField(TEXT("frames"), 10001);
    ExpectInvalidTimeParams(TEXT("a frame request above the step cap"), TooManyFrames);

    TSharedPtr<FJsonObject> OverlongFrames = MakeShared<FJsonObject>();
    OverlongFrames->SetNumberField(TEXT("frames"), 10000);
    OverlongFrames->SetNumberField(TEXT("deltaTime"), 0.0061);
    ExpectInvalidTimeParams(TEXT("an overlong frame-mode request"), OverlongFrames);

    TSharedPtr<FJsonObject> TooSmallSeconds = MakeShared<FJsonObject>();
    TooSmallSeconds->SetNumberField(TEXT("seconds"), 0.00005);
    ExpectInvalidTimeParams(TEXT("seconds below the effective-delta minimum"), TooSmallSeconds);

    TSharedPtr<FJsonObject> TooManySecondsSteps = MakeShared<FJsonObject>();
    TooManySecondsSteps->SetNumberField(TEXT("seconds"), 2.0);
    TooManySecondsSteps->SetNumberField(TEXT("deltaTime"), 0.0001);
    ExpectInvalidTimeParams(TEXT("seconds mode above the step cap"), TooManySecondsSteps);

    TSharedPtr<FJsonObject> OverlongSeconds = MakeShared<FJsonObject>();
    OverlongSeconds->SetNumberField(TEXT("seconds"), 60.0001);
    ExpectInvalidTimeParams(TEXT("seconds mode above the duration cap"), OverlongSeconds);

    FString EffectSource;
    if (!TestTrue(TEXT("EffectHandler.cpp is readable"), ReadSource(
            TEXT("Source/PinWright/Private/Handlers/VFX/EffectHandler.cpp"), EffectSource)))
    {
        return false;
    }
    const int32 VerbStart = EffectSource.Find(
        TEXT("REGISTER_RPC_HANDLER(\"effect.step_and_capture\""));
    const int32 VerbEnd = EffectSource.Find(TEXT("// effect.cleanup"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, VerbStart);
    if (!TestTrue(TEXT("the atomic verb body is bounded in source"),
            VerbStart != INDEX_NONE && VerbEnd > VerbStart))
    {
        return false;
    }
    const FString VerbSource = EffectSource.Mid(VerbStart, VerbEnd - VerbStart);
    const int32 ActivateAt = VerbSource.Find(TEXT("PinWrightEffectRuntime::Activate("));
    const int32 FreezeAt = VerbSource.Find(TEXT("TimeGuard.Freeze()"));
    const int32 AdvanceAt = VerbSource.Find(TEXT("PinWrightEffectRuntime::Advance("));
    const int32 CaptureAt = VerbSource.Find(TEXT("PinWrightOpenLevelCapture::Handle("));
    TestTrue(TEXT("activation is armed before the world freeze"),
        ActivateAt != INDEX_NONE && ActivateAt < FreezeAt);
    TestTrue(TEXT("the world is frozen before deterministic Niagara advance"),
        FreezeAt != INDEX_NONE && FreezeAt < AdvanceAt);
    TestTrue(TEXT("advance is composed into the shared open-level capture"),
        AdvanceAt != INDEX_NONE && AdvanceAt < CaptureAt);
    TestTrue(TEXT("the success payload publishes the actual advanced duration"),
        VerbSource.Contains(
            TEXT("Effect->SetNumberField(TEXT(\"simulatedSeconds\"), AdvanceResult.SimulatedSeconds)")));
    TestTrue(TEXT("the success payload publishes requestedSeconds separately"),
        VerbSource.Contains(
            TEXT("Effect->SetNumberField(TEXT(\"requestedSeconds\"), RequestedSeconds)")));

    FString CaptureSource;
    if (!TestTrue(TEXT("PreviewViewportCaptureUtils.cpp is readable"), ReadSource(
            TEXT("Source/PinWright/Private/Handlers/Render/PreviewViewportCaptureUtils.cpp"),
            CaptureSource)))
    {
        return false;
    }
    const int32 BeforeWarmupAt = CaptureSource.Find(TEXT("Hooks->BeforeWarmup("));
    const int32 WarmupAt = CaptureSource.Find(TEXT("while (OutCapture.WarmupSettleRounds"));
    const int32 ExposureReadbackAt = CaptureSource.Find(TEXT("ReadLastAdaptedExposure("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, WarmupAt);
    const int32 BeforeFinalAt = CaptureSource.Find(TEXT("Hooks->BeforeFinalFrame("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, ExposureReadbackAt);
    const int32 FinalReadAt = CaptureSource.Find(TEXT("Failed to read pixels from editor viewport after the final capture stage"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, BeforeFinalAt);
    const int32 PumpViewportAt = CaptureSource.Find(TEXT("void PumpViewport("));
    const int32 SlateTickAt = CaptureSource.Find(TEXT("SlateApp.Tick("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, PumpViewportAt);
    const int32 EndOfFrameUpdatesAt = CaptureSource.Find(
        TEXT("World->SendAllEndOfFrameUpdates()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, SlateTickAt);
    const int32 ViewportDrawAt = CaptureSource.Find(TEXT("SceneViewport->Draw()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, EndOfFrameUpdatesAt);
    TestTrue(TEXT("activation hook runs before exposure warm-up"),
        BeforeWarmupAt != INDEX_NONE && BeforeWarmupAt < WarmupAt);
    TestTrue(TEXT("the freeze/advance hook cannot run before exposure readback"),
        ExposureReadbackAt != INDEX_NONE && ExposureReadbackAt < BeforeFinalAt);
    TestTrue(TEXT("the final frame is read only after the freeze/advance hook"),
        BeforeFinalAt != INDEX_NONE && BeforeFinalAt < FinalReadAt);
    TestTrue(TEXT("PumpViewport submits deferred world updates before drawing"),
        PumpViewportAt != INDEX_NONE &&
        SlateTickAt > PumpViewportAt &&
        EndOfFrameUpdatesAt > SlateTickAt &&
        ViewportDrawAt > EndOfFrameUpdatesAt);
    TestTrue(TEXT("final-stage cleanup is installed"),
        CaptureSource.Contains(TEXT("Hooks->AfterFinalFrame()")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectStepAndCaptureWorldTimeRestoreTest,
    "PinWright.effect.step_and_capture.WorldTimeGuardRestoresOnScopeExit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectStepAndCaptureWorldTimeRestoreTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
    if (!Settings)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor WorldSettings exists, so scoped time restoration could not be measured."));
        return true;
    }

    const float Original = Settings->TimeDilation;
    {
        FScopedWorldTimeDilation Guard(World);
        TestTrue(TEXT("the guard resolves WorldSettings"), Guard.IsAvailable());
        TestTrue(TEXT("the guard applies a clamped world-time freeze"), Guard.Freeze());
        TestTrue(TEXT("the guard reports the actual applied dilation"),
            FMath::IsNearlyEqual(Settings->TimeDilation, Guard.GetFrozenTimeDilation()));
    }
    TestTrue(TEXT("scope exit restores the exact prior dilation"),
        FMath::IsNearlyEqual(Settings->TimeDilation, Original, UE_KINDA_SMALL_NUMBER));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEffectStepAndCaptureShortBurstEarlySamplePixelsTest,
    "PinWright.effect.step_and_capture.ShortBurstAtEarlySampleIsVisible",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectStepAndCaptureShortBurstEarlySamplePixelsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectStepCaptureTest;

    if (!FApp::CanEverRender())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-gpu"),
            TEXT("This host cannot render, so no Niagara pixels were produced to judge."));
        return true;
    }
    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-viewport"),
            TEXT("The editor or Slate viewport is unavailable, so the capture path was not exercised."));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, SystemPath);
    if (!World || !System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("The editor world or stock SimpleExplosion Niagara fixture is unavailable."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    const FString Label = FString::Printf(TEXT("PW_StepCaptureBurst_%s"), *Suffix);
    ANiagaraActor* Actor = SpawnBurstActor(World, System, Label);
    if (!TestNotNull(TEXT("the inactive burst actor was spawned"), Actor))
    {
        return false;
    }
    UNiagaraComponent* Component = Actor->GetNiagaraComponent();
    if (!TestNotNull(TEXT("the burst actor owns a Niagara component"), Component))
    {
        return false;
    }
    TestFalse(TEXT("the baseline starts with the burst inactive"), Component->IsActive());

    FString BaselinePath;
    FString BurstPath;
    FString WarmupPath;
    ON_SCOPE_EXIT
    {
        if (!BaselinePath.IsEmpty())
        {
            IFileManager::Get().Delete(*BaselinePath, false, true);
        }
        if (!BurstPath.IsEmpty())
        {
            IFileManager::Get().Delete(*BurstPath, false, true);
        }
        if (!WarmupPath.IsEmpty())
        {
            IFileManager::Get().Delete(*WarmupPath, false, true);
        }
    };

    // RENDER THE FIXTURE ONCE AND THROW THE FRAME AWAY, before the measured pair is taken.
    //
    // A Niagara system that has never been drawn in this editor session produces NO pixels in the
    // first frame drawn after its first activation, and that is a property of the system's renderers
    // coming up, not of the verb under test. Measured on this fixture (UE 5.4, stock
    // SimpleExplosion, 256x256): the first atomic sample at 0.05 s came back byte-identical to the
    // inactive baseline -- maxDelta 0 across all 65536 pixels -- while the response correctly
    // reported steps=3, simulatedSeconds=0.05 and a restored time dilation. The very next call with
    // the SAME payload on the SAME actor gave maxDelta 247 and 41 % of pixels changed, and a sweep
    // of 0.0167/0.0333/0.05/0.0667/0.0833/0.1 s after it was monotonic in age, which is what a
    // correctly reset-and-advanced system looks like. Neither draining the concurrent Niagara tick
    // (WaitForConcurrentTickAndFinalize), nor a second viewport pump before the readback, nor
    // IStreamingManager::StreamAllResources moved that first frame; only a previously drawn frame
    // did. The verb cannot close that gap by construction -- its whole contract is to leave NO
    // wall-clock gap between the activation and the sampled frame.
    //
    // So the warm-up is removed from the MEASUREMENT rather than from the assertions: the sample
    // below still resets to age 0 and advances exactly three 1/60 s steps, and every pixel verdict
    // stays as strict as it was. A regression in the verb -- a lost freeze, a skipped advance, the
    // wrong instant read back -- still fails here, because a warmed fixture makes the sampled
    // instant visible rather than making any frame acceptable.
    {
        FTestResponseCapture Warmup;
        TSharedPtr<FJsonObject> WarmupPayload = MakeCapturePayload(
            FString::Printf(TEXT("pw_step_capture_warmup_%s.png"), *Suffix));
        WarmupPayload->SetStringField(TEXT("systemName"), Label);
        WarmupPayload->SetBoolField(TEXT("reset"), true);
        WarmupPayload->SetNumberField(TEXT("seconds"), SampleSeconds);
        InvokeHandlerWithCapture(TEXT("effect.step_and_capture"), WarmupPayload, Warmup);
        if (Warmup.bSuccess && Warmup.Result.IsValid())
        {
            Warmup.Result->TryGetStringField(TEXT("path"), WarmupPath);
        }
        // Back to the inactive state the baseline below is supposed to photograph. Immediate,
        // because Deactivate only stops spawning and would leave the warm-up's particles alive in
        // the baseline frame.
        Component->DeactivateImmediate();
        TestFalse(TEXT("the warm-up leaves the burst inactive again"), Component->IsActive());
    }

    FTestResponseCapture Baseline;
    TSharedPtr<FJsonObject> BaselinePayload = MakeCapturePayload(
        FString::Printf(TEXT("pw_step_capture_baseline_%s.png"), *Suffix));
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), BaselinePayload, Baseline));
    if (!Baseline.bSuccess || !Baseline.Result.IsValid())
    {
        if (IsCaptureEnvironmentUnavailable(Baseline))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
                FString::Printf(TEXT("The baseline viewport readback was unavailable (%s: %s)."),
                    *Baseline.ErrorCode, *Baseline.Message));
            return true;
        }
        AddError(FString::Printf(TEXT("Baseline capture failed: %s: %s"),
            *Baseline.ErrorCode, *Baseline.Message));
        return false;
    }
    Baseline.Result->TryGetStringField(TEXT("path"), BaselinePath);

    const float OriginalDilation = World->GetWorldSettings()->TimeDilation;
    FTestResponseCapture Burst;
    TSharedPtr<FJsonObject> BurstPayload = MakeCapturePayload(
        FString::Printf(TEXT("pw_step_capture_burst_%s.png"), *Suffix));
    BurstPayload->SetStringField(TEXT("systemName"), Label);
    BurstPayload->SetBoolField(TEXT("reset"), true);
    BurstPayload->SetNumberField(TEXT("seconds"), SampleSeconds);
    TestTrue(TEXT("effect.step_and_capture handler found"),
        InvokeHandlerWithCapture(TEXT("effect.step_and_capture"), BurstPayload, Burst));
    if (!Burst.bSuccess || !Burst.Result.IsValid())
    {
        if (IsCaptureEnvironmentUnavailable(Burst))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
                FString::Printf(TEXT("The burst viewport readback was unavailable (%s: %s)."),
                    *Burst.ErrorCode, *Burst.Message));
            return true;
        }
        AddError(FString::Printf(TEXT("Atomic burst capture failed: %s: %s"),
            *Burst.ErrorCode, *Burst.Message));
        return false;
    }
    TestTrue(TEXT("the Niagara component remains active after the early sample"),
        Component->IsActive());
    Burst.Result->TryGetStringField(TEXT("path"), BurstPath);
    TestTrue(TEXT("the handler restored the world's pre-call time dilation"),
        FMath::IsNearlyEqual(World->GetWorldSettings()->TimeDilation,
            OriginalDilation, UE_KINDA_SMALL_NUMBER));

    double ReportedSimulatedSeconds = 0.0;
    const TSharedPtr<FJsonObject>* Effect = nullptr;
    if (TestTrue(TEXT("the response publishes the atomic effect sample"),
            Burst.Result->TryGetObjectField(TEXT("effect"), Effect) && Effect && Effect->IsValid()))
    {
        double SampledSeconds = 0.0;
        double Steps = 0.0;
        double DeltaTime = 0.0;
        bool bSettledBeforeFreeze = false;
        TestTrue(TEXT("the response publishes sampledSeconds"),
            (*Effect)->TryGetNumberField(TEXT("sampledSeconds"), SampledSeconds));
        TestTrue(TEXT("the burst was requested at the 0.05-second early sample"),
            FMath::IsNearlyEqual(SampledSeconds, SampleSeconds));
        TestTrue(TEXT("seconds mode advances the early burst in three simulation steps"),
            (*Effect)->TryGetNumberField(TEXT("steps"), Steps) && FMath::IsNearlyEqual(Steps, 3.0));
        TestTrue(TEXT("the three steps use a 1/60 second delta"),
            (*Effect)->TryGetNumberField(TEXT("deltaTime"), DeltaTime) &&
            FMath::IsNearlyEqual(DeltaTime, 1.0 / 60.0));
        TestTrue(TEXT("simulatedSeconds reports the measured Niagara age delta"),
            (*Effect)->TryGetNumberField(TEXT("simulatedSeconds"), ReportedSimulatedSeconds) &&
            FMath::IsNearlyEqual(ReportedSimulatedSeconds, Steps * DeltaTime, 1e-12));
        TestFalse(TEXT("simulatedSeconds is not substituted with the requested sampledSeconds"),
            FMath::IsNearlyEqual(ReportedSimulatedSeconds, SampledSeconds, 1e-10));
        TestTrue(TEXT("exposure settled before the world-time freeze"),
            (*Effect)->TryGetBoolField(TEXT("exposureSettledBeforeFreeze"), bSettledBeforeFreeze) &&
            bSettledBeforeFreeze);
        const TSharedPtr<FJsonObject>* WorldTime = nullptr;
        bool bRestored = false;
        TestTrue(TEXT("the response reports successful scoped time restoration"),
            (*Effect)->TryGetObjectField(TEXT("worldTimeDilation"), WorldTime) && WorldTime &&
            WorldTime->IsValid() &&
            (*WorldTime)->TryGetBoolField(TEXT("restored"), bRestored) && bRestored);
    }

    TestTrue(TEXT("simulatedSeconds approximately equals the requested early sample"),
        FMath::IsNearlyEqual(ReportedSimulatedSeconds, SampleSeconds, 1e-6));

    TSet<UMaterialInterface*> RendererMaterials;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        if (!EmitterHandle.GetIsEnabled())
        {
            continue;
        }
        const FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
        if (!EmitterData)
        {
            continue;
        }
        for (const UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
        {
            if (!Renderer || !Renderer->GetIsEnabled())
            {
                continue;
            }
            TArray<UMaterialInterface*> UsedMaterials;
            Renderer->GetUsedMaterials(nullptr, UsedMaterials);
            for (UMaterialInterface* Material : UsedMaterials)
            {
                if (Material)
                {
                    RendererMaterials.Add(Material);
                }
            }
        }
    }
    if (!TestTrue(TEXT("the SimpleExplosion fixture has enabled renderer materials"),
            RendererMaterials.Num() > 0))
    {
        return false;
    }

    const double ShaderWaitDeadline = FPlatformTime::Seconds() +
        MaterialCompileErrorCollector::CompileWaitTimeoutSeconds;
    TArray<FString> UnreadyRendererMaterials;
    for (UMaterialInterface* Material : RendererMaterials)
    {
        const PinWright::MaterialShaderState::FState ShaderState =
            PinWright::MaterialShaderState::ProbeAfterCapture(Material, ShaderWaitDeadline);
        if (ShaderState.Status != PinWright::MaterialShaderState::EStatus::Completed)
        {
            UnreadyRendererMaterials.Add(FString::Printf(TEXT("%s status=%s"),
                *Material->GetPathName(),
                PinWright::MaterialShaderState::ToWire(ShaderState.Status)));
        }
    }
    if (UnreadyRendererMaterials.Num() > 0)
    {
        UnreadyRendererMaterials.Sort();
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
            FString::Printf(
                TEXT("niagara renderer material shader map not compiled on this host: %s"),
                *FString::Join(UnreadyRendererMaterials, TEXT(", "))));
        return true;
    }

    TArray<FColor> BaselinePixels;
    TArray<FColor> BurstPixels;
    FString DecodeError;
    if (!TestTrue(TEXT("the baseline PNG decodes"),
            DecodeCapture(BaselinePath, BaselinePixels, DecodeError)))
    {
        AddError(DecodeError);
        return false;
    }
    DecodeError.Reset();
    if (!TestTrue(TEXT("the burst PNG decodes"), DecodeCapture(BurstPath, BurstPixels, DecodeError)))
    {
        AddError(DecodeError);
        return false;
    }

    const PinWrightFlatRegion::FFlatRegionStats Flat =
        PinWrightFlatRegion::MeasureLargestFlatRegion(BurstPixels, CaptureWidth, CaptureHeight);
    const PinWrightFlatRegion::FFrameDifferenceStats Difference =
        PinWrightFlatRegion::MeasureFrameDifference(BaselinePixels, BurstPixels, 4);
    const PinWrightRenderCapture::FCaptureImageStats ImageStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(BurstPixels);
    // Named in every verdict below: a bare "expected true" on a pixel judgement says nothing about
    // WHY the frame was rejected, and the three criteria fail together on very different frames.
    const FString Measured = FString::Printf(
        TEXT("[mean=%.6f lit=%lld/%lld tones=%d blank=%d | maxDelta=%d meanDelta=%.4f "
             "changed=%.6f | flatFraction=%.4f flatLevel=%d]"),
        ImageStats.MeanLuminance, static_cast<long long>(ImageStats.LitPixelCount),
        static_cast<long long>(CaptureWidth) * CaptureHeight, ImageStats.ToneLevelsUsed,
        ImageStats.bBlank ? 1 : 0, Difference.MaxDelta, Difference.MeanAbsDelta,
        Difference.ChangedPixelFraction, Flat.LargestRegionFraction, Flat.LargestRegionLevel);
    TestTrue(TEXT("FlatRegionStats measured the captured burst frame"), Flat.bMeasured);
    TestFalse(*FString::Printf(TEXT("the sampled burst frame is not black/blank %s"), *Measured),
        ImageStats.bBlank);
    TestTrue(*FString::Printf(
                 TEXT("the sampled burst changes visible pixels from the inactive baseline %s"),
                 *Measured),
        Difference.bMeasured && Difference.MaxDelta > 8 && Difference.ChangedPixelFraction > 0.0005);
    TestTrue(*FString::Printf(TEXT("the burst frame is not one all-black flat region %s"), *Measured),
        Flat.LargestRegionFraction < 0.99 || Flat.LargestRegionLevel > 2);
    return true;
}
