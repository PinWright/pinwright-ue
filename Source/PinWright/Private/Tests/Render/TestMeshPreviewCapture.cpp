// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Handlers/Render/MeshPreviewCaptureUtils.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/Package.h"

namespace
{
    PinWrightMeshPreviewCapture::FMeshCaptureRequest MakeCubeRequest(float SubjectScale)
    {
        PinWrightMeshPreviewCapture::FMeshCaptureRequest Request;
        Request.AssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
        Request.Capture.Width = 128;
        Request.Capture.Height = 128;
        Request.Capture.Fov = 50.0f;
        Request.Capture.Exposure.Mode = PinWrightRenderCapture::EExposureRequestMode::Fixed;
        Request.Capture.Exposure.Ev100 = 0.0f;
        Request.Capture.PreviewSceneRig.bRequested = true;
        Request.Capture.PreviewSceneRig.bShowFloorProvided = true;
        Request.Capture.PreviewSceneRig.bShowFloor = false;
        Request.Capture.PreviewSceneRig.bShowEnvironmentProvided = true;
        Request.Capture.PreviewSceneRig.bShowEnvironment = false;
        Request.Capture.bRetainPixels = true;
        Request.SubjectScale = SubjectScale;
        Request.bWriteFile = false;
        return Request;
    }

    bool RunCubeCaptureAssertions(FAutomationTestBase& Test, float SubjectScale)
    {
        if (!FApp::CanEverRender() || !GDynamicRHI)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-gpu"),
                TEXT("This host has no rendering device, so scene-capture readback cannot run."));
            return true;
        }

        const PinWrightMeshPreviewCapture::FMeshCaptureRequest Request =
            MakeCubeRequest(SubjectScale);
        PinWrightMeshPreviewCapture::FMeshCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        const bool bCaptured = PinWrightMeshPreviewCapture::CaptureMeshToPng(
            Request, Output, ErrorCode, ErrorMessage);
        if (!Test.TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"),
                *ErrorCode, *ErrorMessage), bCaptured))
        {
            return false;
        }

        Test.TestEqual(TEXT("captured width"), Output.Capture.Width, 128);
        Test.TestEqual(TEXT("captured height"), Output.Capture.Height, 128);
        Test.TestEqual(TEXT("readback pixel count"), Output.Capture.Pixels.Num(), 128 * 128);
        Test.TestTrue(TEXT("PNG was encoded without a persistent file"), Output.PngData.Num() > 8);
        Test.TestTrue(TEXT("flat-region analysis measured the rendered pixels"),
            Output.FlatRegion.bMeasured);
        Test.TestTrue(TEXT("the frame contains non-flat pixels"),
            Output.FlatRegion.FlatBlockFraction < 1.0
                && Output.FlatRegion.LargestRegionFraction < 1.0);
        Test.TestTrue(TEXT("the cube changes luminance across the frame"),
            Output.Capture.ImageStats.LuminanceVariance > 0.0);
        Test.TestTrue(TEXT("the framed cube occupies a non-empty subset of the image"),
            Output.SubjectCoverage.IsSet()
                && Output.SubjectCoverage.GetValue() > 0.0
                && Output.SubjectCoverage.GetValue() < 1.0);
        // B-capture-mesh-cold-first-frame-no-readiness-wait: the session waits on the mesh's slot
        // materials, not only its build, and the first shot is settled by measurement.
        Test.TestTrue(TEXT("readiness waited on the mesh build"),
            Output.Readiness.bWaitedForAssetCompilation);
        Test.TestTrue(TEXT("readiness asked the cube's slot material for its shader map"),
            Output.Readiness.bShaderMapChecked);
        Test.TestTrue(TEXT("the slot material's shader map is complete after the wait"),
            Output.Readiness.bShaderMapCompleteAfter);
        Test.TestTrue(TEXT("the first shot was compared against an identical redraw"),
            Output.Settle.bMeasured);
        Test.TestTrue(TEXT("a warm cube draws the same frame twice"), Output.Settle.bSettled);
        Test.TestTrue(TEXT("redrawRetries is a bounded count"),
            Output.Capture.RedrawRetries >= 0
                && Output.Capture.RedrawRetries
                    <= PinWrightMeshPreviewCapture::MaxSettleRedrawRetries);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureCubePixelsTest,
    "PinWright.render.capture_mesh.TransientCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureCubePixelsTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    return RunCubeCaptureAssertions(*this, 1.0f);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureFiveCentimeterCubePixelsTest,
    "PinWright.render.capture_mesh.TransientFiveCentimeterCubeHasNonFlatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureFiveCentimeterCubePixelsTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    return RunCubeCaptureAssertions(*this, 0.05f);
}

namespace TestMeshPreviewCaptureSettle
{
    // Feeds SettleFrame a scripted sequence of redraws: each entry is one frame filled with that
    // grey level.
    struct FScriptedRedraw
    {
        TArray<uint8> Levels;
        int32 Calls = 0;

        bool Draw(TArray<FColor>& OutPixels)
        {
            if (!Levels.IsValidIndex(Calls))
            {
                return false;
            }
            const uint8 Level = Levels[Calls++];
            OutPixels.Init(FColor(Level, Level, Level, 255), 16);
            return true;
        }
    };

    TArray<FColor> MakeFrame(uint8 Level)
    {
        TArray<FColor> Pixels;
        Pixels.Init(FColor(Level, Level, Level, 255), 16);
        return Pixels;
    }
}

// Pure logic: no GPU. Counterfactual: make SettleFrame return after one draw without comparing,
// or count the comparison draw as a retry, and the cold and stable cases below go red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureSettleFrameTest,
    "PinWright.render.capture_mesh.SettleFrameCountsRedrawRetries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureSettleFrameTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMeshPreviewCapture;
    using TestMeshPreviewCaptureSettle::FScriptedRedraw;
    using TestMeshPreviewCaptureSettle::MakeFrame;

    {
        FScriptedRedraw Redraw{ { 100 } };
        TArray<FColor> Pixels = MakeFrame(100);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("stable: settles"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestTrue(TEXT("stable: measured"), Report.bMeasured);
        TestTrue(TEXT("stable: settled"), Report.bSettled);
        TestEqual(TEXT("stable: the comparison draw is not a retry"), Retries, 0);
        TestEqual(TEXT("stable: one comparison draw"), Redraw.Calls, 1);
    }
    {
        // Cold first frame (black), then the finished picture twice.
        FScriptedRedraw Redraw{ { 200, 200 } };
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("cold: settles"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestTrue(TEXT("cold: settled on the retry"), Report.bSettled);
        TestEqual(TEXT("cold: one redraw retry"), Retries, 1);
        TestEqual(TEXT("cold: the returned frame is the settled one"), Pixels[0].R, (uint8)200);
    }
    {
        // Never agrees: bounded, and reported unsettled rather than looping.
        FScriptedRedraw Redraw{ { 50, 150, 250 } };
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestTrue(TEXT("unsettled: returns"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
        TestFalse(TEXT("unsettled: reported"), Report.bSettled);
        TestEqual(TEXT("unsettled: retries capped"), Retries, MaxSettleRedrawRetries);
        TestEqual(TEXT("unsettled: draws capped"), Redraw.Calls, MaxSettleRedrawRetries + 1);
        TestTrue(TEXT("unsettled: whole frame changed"), Report.ChangedPixelFraction > 0.99);

        FMeshCaptureOutput Output;
        Output.Settle = Report;
        Output.Capture.RedrawRetries = Retries;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        TestTrue(TEXT("unsettled: readiness published"), Result->HasField(TEXT("readiness")));
        TestFalse(TEXT("unsettled: frameSettled false"), Result->GetBoolField(TEXT("frameSettled")));
        TestTrue(TEXT("unsettled: frameWarning present"), Result->HasField(TEXT("frameWarning")));
    }
    {
        FScriptedRedraw Redraw;
        TArray<FColor> Pixels = MakeFrame(0);
        int32 Retries = -1;
        FFrameSettleReport Report;
        TestFalse(TEXT("a failed redraw fails the settle"), SettleFrame([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, MaxSettleRedrawRetries, Retries, Report));
    }
    {
        FMeshCaptureOutput Output;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        TestFalse(TEXT("an unmeasured shot publishes no frameSettled"),
            Result->HasField(TEXT("frameSettled")));
        TestFalse(TEXT("an unmeasured shot publishes no frameWarning"),
            Result->HasField(TEXT("frameWarning")));
    }
    return true;
}

// #15, pure logic. A cold Nanite mesh draws its root clusters twice inside one tick, so the
// in-tick settle agrees on the coarse frame; the pages land a few frame numbers later.
// Counterfactual: stop SettleAcrossFrames at the first agreeing pair (SettleFrame's rule) and the
// coarse case returns grey 100 after one pump; drop the reset of the stable count on a change and
// it settles before NaniteStableFrames agreeing frames.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureSettleAcrossFramesTest,
    "PinWright.render.capture_mesh.NaniteSettleWaitsAcrossFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureSettleAcrossFramesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMeshPreviewCapture;
    using TestMeshPreviewCaptureSettle::FScriptedRedraw;
    using TestMeshPreviewCaptureSettle::MakeFrame;

    {
        // Coarse frame agrees with itself once, then the streamed pages change it.
        FScriptedRedraw Redraw{ { 100, 200 } };
        for (int32 Index = 0; Index < NaniteStableFrames; ++Index)
        {
            Redraw.Levels.Add(200);
        }
        TArray<FColor> Pixels = MakeFrame(100);
        FNaniteResidencyReport Report;
        TestTrue(TEXT("coarse: returns"), SettleAcrossFrames([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, NaniteStableFrames, NaniteMaxPumpedFrames, Report));
        TestTrue(TEXT("coarse: waited"), Report.bWaited);
        TestTrue(TEXT("coarse: settled"), Report.bSettled);
        TestTrue(TEXT("coarse: the change across frames is reported"), Report.bChangedAcrossFrames);
        TestEqual(TEXT("coarse: the returned frame has the streamed pages"), Pixels[0].R, (uint8)200);
        TestEqual(TEXT("coarse: pumped until NaniteStableFrames frames agreed"),
            Report.PumpedFrames, 2 + NaniteStableFrames);
    }
    {
        // Never agrees: bounded, reported unsettled, and warned on the wire.
        FScriptedRedraw Redraw;
        for (int32 Index = 0; Index < 5; ++Index)
        {
            Redraw.Levels.Add(static_cast<uint8>(Index % 2 ? 0 : 255));
        }
        TArray<FColor> Pixels = MakeFrame(0);
        FMeshCaptureOutput Output;
        TestTrue(TEXT("unsettled: returns"), SettleAcrossFrames([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, NaniteStableFrames, 5, Output.Nanite));
        TestFalse(TEXT("unsettled: reported"), Output.Nanite.bSettled);
        TestEqual(TEXT("unsettled: frames capped"), Output.Nanite.PumpedFrames, 5);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        const TSharedPtr<FJsonObject>* Readiness = nullptr;
        const TSharedPtr<FJsonObject>* Nanite = nullptr;
        TestTrue(TEXT("unsettled: readiness.naniteResidency published"),
            Result->TryGetObjectField(TEXT("readiness"), Readiness)
                && (*Readiness)->TryGetObjectField(TEXT("naniteResidency"), Nanite));
        TestTrue(TEXT("unsettled: naniteResidency.settled false"),
            Nanite && !(*Nanite)->GetBoolField(TEXT("settled")));
        TestTrue(TEXT("unsettled: naniteResidency.pumpedFrames 5"),
            Nanite && (*Nanite)->GetIntegerField(TEXT("pumpedFrames")) == 5);
        TestTrue(TEXT("unsettled: frameWarning present"), Result->HasField(TEXT("frameWarning")));
    }
    {
        FScriptedRedraw Redraw;
        TArray<FColor> Pixels = MakeFrame(0);
        FNaniteResidencyReport Report;
        TestFalse(TEXT("a failed redraw fails the wait"), SettleAcrossFrames([&](TArray<FColor>& Out)
            { return Redraw.Draw(Out); }, Pixels, NaniteStableFrames, NaniteMaxPumpedFrames, Report));
    }
    {
        FMeshCaptureOutput Output;
        Output.Nanite.SkipReason = TEXT("noNaniteData");
        TSharedPtr<FJsonObject> Nanite = MakeNaniteResidencyObject(Output.Nanite);
        TestFalse(TEXT("skipped: waited false"), Nanite->GetBoolField(TEXT("waited")));
        TestEqual(TEXT("skipped: reason named"), Nanite->GetStringField(TEXT("reason")),
            FString(TEXT("noNaniteData")));
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        TestFalse(TEXT("skipped: no frameWarning"), Result->HasField(TEXT("frameWarning")));
    }
    {
        // Both waits unsettled on shot 0: the in-tick settle warning must not replace the Nanite
        // one. Counterfactual: SetStringField instead of appending drops "Nanite".
        FMeshCaptureOutput Output;
        Output.Nanite.bWaited = true;
        Output.Nanite.PumpedFrames = 16;
        Output.Nanite.bBudgetExhausted = true;
        Output.Settle.bMeasured = true;
        Output.Settle.bSettled = false;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddMeshFrameEvidenceFields(Output, Result);
        const FString Warning = Result->GetStringField(TEXT("frameWarning"));
        TestTrue(TEXT("combined: the Nanite warning survives"), Warning.Contains(TEXT("Nanite pages")));
        TestTrue(TEXT("combined: it names the exhausted budget"), Warning.Contains(TEXT("budget ran out")));
        TestTrue(TEXT("combined: the in-tick settle warning is kept too"),
            Warning.Contains(TEXT("first shot did not settle")));
        const TSharedPtr<FJsonObject>* Readiness = nullptr;
        const TSharedPtr<FJsonObject>* Nanite = nullptr;
        TestTrue(TEXT("combined: budgetExhausted published"),
            Result->TryGetObjectField(TEXT("readiness"), Readiness)
                && (*Readiness)->TryGetObjectField(TEXT("naniteResidency"), Nanite)
                && (*Nanite)->GetBoolField(TEXT("budgetExhausted")));
    }
    {
        // A later shot's bound is as loud as shot 0's. Counterfactual: publish only Captures[0]
        // and shots[1] carries no frameWarning, the top level no naniteUnsettledShots.
        TArray<FMeshCaptureOutput> Captures;
        Captures.SetNum(2);
        Captures[0].Nanite.bWaited = true;
        Captures[0].Nanite.bSettled = true;
        Captures[1].Nanite.bWaited = true;
        Captures[1].Nanite.PumpedFrames = NaniteMaxPumpedFrames;
        TArray<TSharedPtr<FJsonValue>> Shots;
        Shots.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
        Shots.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddNaniteShotFields(Captures, Shots, Result);
        TestTrue(TEXT("shots: each shot carries naniteResidency"),
            Shots[0]->AsObject()->HasField(TEXT("naniteResidency"))
                && Shots[1]->AsObject()->HasField(TEXT("naniteResidency")));
        TestFalse(TEXT("shots: a settled shot gets no frameWarning"),
            Shots[0]->AsObject()->HasField(TEXT("frameWarning")));
        TestTrue(TEXT("shots: the unsettled shot gets its own frameWarning"),
            Shots[1]->AsObject()->HasField(TEXT("frameWarning")));
        const TArray<TSharedPtr<FJsonValue>>* Unsettled = nullptr;
        TestTrue(TEXT("shots: naniteUnsettledShots is [1]"),
            Result->TryGetArrayField(TEXT("naniteUnsettledShots"), Unsettled)
                && Unsettled->Num() == 1 && (*Unsettled)[0]->AsNumber() == 1.0);
        TestTrue(TEXT("shots: the top level warns"), Result->HasField(TEXT("frameWarning")));
    }
    // The call-wide pump budget. Counterfactual: no budget and 24 shots at the per-shot cap
    // pump 2880 frames inside one call.
    TestEqual(TEXT("budget: small frames hit the frame cap"),
        NaniteSessionPumpBudget(128 * 128), NaniteSessionMaxPumpedFrames);
    TestEqual(TEXT("budget: 768x768 is bounded by the pixel budget"),
        NaniteSessionPumpBudget(768 * 768), static_cast<int32>(NanitePumpPixelBudget / (768 * 768)));
    TestEqual(TEXT("budget: a huge frame still gets enough frames to settle once"),
        NaniteSessionPumpBudget(4096ll * 4096ll), 2 * NaniteStableFrames);
    return true;
}

// #15 end to end, on a host that draws Nanite: a Nanite mesh really pumps frames, and a warm one
// settles in exactly NaniteStableFrames pumps with no change across frames (a change there would
// mean the frame-number advance itself perturbs shading and the heuristic is unusable).
// Counterfactual: drop the SettleAcrossFrames call and `waited` stays false. Skips where the
// project or platform does not render Nanite (r.Nanite.ProjectEnabled=False, r.Nanite=0, SM5).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureNanitePumpRpcTest,
    "PinWright.render.capture_mesh.NaniteMeshPumpsFramesOverRpc",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureNanitePumpRpcTest::RunTest(const FString& Parameters)
{
    using PinWrightMeshPreviewCapture::NaniteStableFrames;
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    if (!UseNanite(GMaxRHIShaderPlatform))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("nanite-not-rendered"),
            TEXT("UseNanite(GMaxRHIShaderPlatform) is false on this host (project r.Nanite.ProjectEnabled, ")
            TEXT("r.Nanite or the shader platform), so render.capture_mesh never pumps frames here."));
        return true;
    }

    UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (!TestNotNull(TEXT("the engine sphere loads"), Source))
    {
        return true;
    }
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/PWNaniteResidency_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = Package ? DuplicateObject<UStaticMesh>(Source, Package,
        FName(*FPackageName::GetLongPackageAssetName(PackagePath))) : nullptr;
    if (!TestNotNull(TEXT("the Nanite fixture mesh is created"), Mesh))
    {
        return true;
    }
    FAssetRegistryModule::AssetCreated(Mesh);
    const FString MeshPath = Mesh->GetPathName();
    TArray<FString> WrittenFiles;
    ON_SCOPE_EXIT
    {
        for (const FString& File : WrittenFiles)
        {
            IFileManager::Get().Delete(*File, false, true, true);
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(MeshPath);
    };
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
    Settings.bEnabled = true;
    Mesh->SetNaniteSettings(Settings);
#else
    Mesh->NaniteSettings.bEnabled = true;
#endif
    Mesh->Build(/*bInSilent=*/true);
    if (!TestTrue(TEXT("precondition: the fixture has Nanite data"), Mesh->HasValidNaniteData()))
    {
        return true;
    }

    auto Capture = [&](TSharedPtr<FJsonObject>& OutResult) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MeshPath);
        Payload->SetNumberField(TEXT("width"), 128);
        Payload->SetNumberField(TEXT("height"), 128);
        Payload->SetNumberField(TEXT("count"), 2);
        Payload->SetBoolField(TEXT("measureCoverage"), false);
        Payload->SetNumberField(TEXT("exposure"), 0);
        FTestResponseCapture Response;
        const bool bOk = InvokeHandlerWithCapture(TEXT("render.capture_mesh"), Payload, Response)
            && Response.bSuccess && Response.Result.IsValid();
        TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"),
            *Response.ErrorCode, *Response.Message), bOk);
        OutResult = Response.Result;
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (bOk && OutResult->TryGetArrayField(TEXT("shots"), Shots))
        {
            for (const TSharedPtr<FJsonValue>& Shot : *Shots)
            {
                FString Path;
                if (Shot->AsObject().IsValid() && Shot->AsObject()->TryGetStringField(TEXT("path"), Path))
                {
                    WrittenFiles.Add(Path);
                }
            }
        }
        return bOk;
    };
    auto NaniteOf = [](const TSharedPtr<FJsonObject>& Result) -> TSharedPtr<FJsonObject>
    {
        const TSharedPtr<FJsonObject>* Readiness = nullptr;
        const TSharedPtr<FJsonObject>* Nanite = nullptr;
        if (Result->TryGetObjectField(TEXT("readiness"), Readiness)
            && (*Readiness)->TryGetObjectField(TEXT("naniteResidency"), Nanite))
        {
            return *Nanite;
        }
        return TSharedPtr<FJsonObject>();
    };

    TSharedPtr<FJsonObject> Cold;
    if (!Capture(Cold))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> ColdNanite = NaniteOf(Cold);
    if (!TestTrue(TEXT("readiness.naniteResidency is published"), ColdNanite.IsValid()))
    {
        return true;
    }
    TestTrue(TEXT("a Nanite mesh pumps frames"), ColdNanite->GetBoolField(TEXT("waited")));
    TestTrue(TEXT("at least NaniteStableFrames frames were pumped"),
        ColdNanite->GetIntegerField(TEXT("pumpedFrames")) >= NaniteStableFrames);
    TestTrue(TEXT("the cold call settles"), ColdNanite->GetBoolField(TEXT("settled")));

    TSharedPtr<FJsonObject> Warm;
    if (!Capture(Warm))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> WarmNanite = NaniteOf(Warm);
    if (!TestTrue(TEXT("warm: naniteResidency is published"), WarmNanite.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("warm: settles in exactly NaniteStableFrames pumps"),
        static_cast<int32>(WarmNanite->GetIntegerField(TEXT("pumpedFrames"))), NaniteStableFrames);
    TestFalse(TEXT("warm: advancing the frame number changes no pixels"),
        WarmNanite->GetBoolField(TEXT("changedAcrossFrames")));
    return true;
}

// #15: the streaming manager installs pages only when the render-thread frame number changes.
// Counterfactual: drop the ENQUEUE_RENDER_COMMAND from AdvanceRenderFrameNumber and the
// render-thread counter stays behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureAdvanceFrameTest,
    "PinWright.render.capture_mesh.FramePumpAdvancesRenderThreadFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureAdvanceFrameTest::RunTest(const FString& Parameters)
{
    FlushRenderingCommands();
    const uint64 Before = GFrameCounterRenderThread;
    PinWrightMeshPreviewCapture::AdvanceRenderFrameNumber();
    FlushRenderingCommands();
    TestTrue(TEXT("the render-thread frame number moved"), GFrameCounterRenderThread > Before);
    TestEqual(TEXT("the render thread matches the game-thread frame counter"),
        GFrameCounterRenderThread, GFrameCounter);
    return true;
}

// The readiness helper used to ask only a MATERIAL subject for its shader map; a mesh subject got
// its own build waited on and nothing else. Counterfactual: drop the mesh branches from
// WaitForThumbnailSubjectReadiness and shaderMapChecked goes false here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureReadinessMaterialsTest,
    "PinWright.render.capture_mesh.ReadinessWaitsOnMeshMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureReadinessMaterialsTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("the engine cube loads"), Cube))
    {
        return true;
    }
    if (!TestTrue(TEXT("precondition: the cube has a slot material"),
        Cube->GetStaticMaterials().Num() > 0
            && Cube->GetStaticMaterials()[0].MaterialInterface != nullptr))
    {
        return true;
    }
    PinWrightThumbnail::FThumbnailReadinessReport Report;
    PinWrightThumbnail::WaitForThumbnailSubjectReadiness(Cube, Report);
    TestTrue(TEXT("the mesh build was waited on"), Report.bWaitedForAssetCompilation);
    TestTrue(TEXT("the slot material's shader map was checked"), Report.bShaderMapChecked);
    TestTrue(TEXT("the slot material's shader map is complete after the wait"),
        Report.bShaderMapCompleteAfter);
    return true;
}

// The wire half: RenderHandler's one AddMeshFrameEvidenceFields call is the only thing that puts
// `readiness` / `frameSettled` on the response. Counterfactual: delete that call and the readiness
// and frameSettled asserts go red; settle on every shot instead of the first and shots[1] goes red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshPreviewCaptureRpcFrameEvidenceTest,
    "PinWright.render.capture_mesh.RpcPublishesReadinessAndSettle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMeshPreviewCaptureRpcFrameEvidenceTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Payload->SetNumberField(TEXT("width"), 128);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetNumberField(TEXT("count"), 2);
    Payload->SetBoolField(TEXT("measureCoverage"), false);
    Payload->SetNumberField(TEXT("exposure"), 0);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_mesh handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_mesh"), Payload, Capture))
        || !TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess && Capture.Result.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject>& Result = Capture.Result;

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Result->TryGetArrayField(TEXT("shots"), Shots))
    {
        for (const TSharedPtr<FJsonValue>& Shot : *Shots)
        {
            FString Path;
            if (Shot->AsObject().IsValid() && Shot->AsObject()->TryGetStringField(TEXT("path"), Path))
            {
                IFileManager::Get().Delete(*Path, false, false, true);
            }
        }
    }

    const TSharedPtr<FJsonObject>* Readiness = nullptr;
    TestTrue(TEXT("readiness is published"), Result->TryGetObjectField(TEXT("readiness"), Readiness));
    TestTrue(TEXT("readiness.shaderMapChecked"),
        Readiness && (*Readiness)->GetBoolField(TEXT("shaderMapChecked")));
    TestTrue(TEXT("frameSettled is published"), Result->HasField(TEXT("frameSettled")));
    // #15: the engine cube has no Nanite data, so no frames are pumped and the reason says why.
    const TSharedPtr<FJsonObject>* Nanite = nullptr;
    TestTrue(TEXT("readiness.naniteResidency names why no pages were waited for"),
        Readiness && (*Readiness)->TryGetObjectField(TEXT("naniteResidency"), Nanite)
            && !(*Nanite)->GetBoolField(TEXT("waited"))
            && (*Nanite)->GetStringField(TEXT("reason")) == TEXT("noNaniteData"));

    if (!TestTrue(TEXT("two shots"), Shots && Shots->Num() == 2))
    {
        return true;
    }
    TestTrue(TEXT("shots[1] carries its own naniteResidency"),
        (*Shots)[1]->AsObject()->HasField(TEXT("naniteResidency")));
    const int32 FirstRetries = (*Shots)[0]->AsObject()->GetIntegerField(TEXT("redrawRetries"));
    TestTrue(TEXT("shots[0].redrawRetries is 0 or 1"), FirstRetries == 0 || FirstRetries == 1);
    TestEqual(TEXT("shots[1].redrawRetries is 0: settle runs once per session"),
        (*Shots)[1]->AsObject()->GetIntegerField(TEXT("redrawRetries")), 0);
    return true;
}
