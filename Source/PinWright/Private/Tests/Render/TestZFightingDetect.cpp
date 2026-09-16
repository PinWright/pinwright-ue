// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for render.detect_z_fighting.
//
// The two fixture tests below are a MATCHED PAIR and neither is meaningful alone. A detector
// that is switched off, misconfigured, or silently rendering nothing returns zero affected
// pixels — indistinguishable from a clean scene — so a test that only asserts "a clean scene
// reports clean" proves nothing at all. The coplanar fixture asserts a NON-ZERO result on
// geometry built to fight, and the separated fixture asserts ZERO on the same two surfaces
// pulled apart. Together they pin both ends. Delete either and the suite stops being evidence.
//
// Both drive the PRODUCTION path through InvokeHandlerWithCapture rather than calling the
// analysis helpers directly: a helper-level test passes even when the handler no longer calls
// the helper, which is how a shipped opacity test in this codebase kept passing with its
// production call site deleted.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Render/ZFightingAnalysis.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"

namespace
{
    // Far from anything a host project is likely to have authored, so the fixture is the only
    // geometry in frame and whatever level happens to be open cannot pollute the separated
    // fixture's "exactly zero" assertion. The empty background is still analysed rather than
    // clip-excluded (measured: clipExcludedPixels 0, analyzedPixels == totalPixels), but it
    // renders identically under both near planes and so contributes exactly zero flags.
    const FVector ZFightFixtureOrigin(500000.0, 500000.0, 200000.0);

    void DeleteZFightMaskIfPresent(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid()
            && Capture.Result->TryGetStringField(TEXT("maskPath"), Path) && !Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    // Two flat slabs, each wearing a DIFFERENT engine material. The differing material is not
    // decoration: the detector's signal is a visible surface swap, so two slabs sharing one
    // material would fight exactly as hard and be correctly reported as nothing to look at.
    // The 45-degree yaw on the second slab gives the pair two different triangulations of the
    // same plane, so their interpolated depths straddle each other by the last bit or two.
    // That only produces a measurable population of near-ties when the plane is viewed at an
    // angle — see the camera note in MakeZFightPayload, which is the other half of this
    // fixture and cannot be changed independently of it.
    bool SpawnZFightFixture(UWorld* World, double SecondSlabZOffset,
        AStaticMeshActor*& OutFirst, AStaticMeshActor*& OutSecond, FString& OutSkipReason)
    {
        OutFirst = nullptr;
        OutSecond = nullptr;

        UMaterialInterface* FirstMaterial = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
        UMaterialInterface* SecondMaterial = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        if (!FirstMaterial || !SecondMaterial)
        {
            OutSkipReason = TEXT("engine fixture materials unavailable");
            return false;
        }

        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutFirst = SpawnTransientCubeActor(World,
            FString::Printf(TEXT("PW_ZFight_A_%s"), *Suffix), ZFightFixtureOrigin);
        OutSecond = SpawnTransientCubeActor(World,
            FString::Printf(TEXT("PW_ZFight_B_%s"), *Suffix),
            ZFightFixtureOrigin + FVector(0.0, 0.0, SecondSlabZOffset));
        if (!OutFirst || !OutSecond)
        {
            OutSkipReason = TEXT("could not spawn the cube fixture (engine cube unavailable)");
            return false;
        }

        const auto Configure = [](AStaticMeshActor* Actor, UMaterialInterface* Material, double Yaw)
        {
            if (USceneComponent* Root = Actor->GetRootComponent())
            {
                // Static mobility rejects transform changes; the fixture is posed after spawn.
                Root->SetMobility(EComponentMobility::Movable);
            }
            Actor->SetActorRotation(FRotator(0.0, Yaw, 0.0));
            // The engine cube is 100 uu; this makes a 400x400 uu slab 2 uu thick, so both
            // slabs' top faces sit at exactly the same height when the Z offset is zero.
            Actor->SetActorScale3D(FVector(4.0, 4.0, 0.02));
            if (UStaticMeshComponent* MeshComponent = Actor->GetStaticMeshComponent())
            {
                MeshComponent->SetMaterial(0, Material);
            }
        };
        Configure(OutFirst, FirstMaterial, 0.0);
        Configure(OutSecond, SecondMaterial, 45.0);
        return true;
    }

    TSharedPtr<FJsonObject> MakeZFightPayload(int32 Resolution = 0)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

        // The camera pose is OBLIQUE, and that is load-bearing rather than aesthetic.
        //
        // Seen face-on (straight down at these horizontal slabs), both surfaces are planes
        // perpendicular to the view, so W is the same constant for every pixel of both and
        // DeviceZ = Near/W is BIT-IDENTICAL, not merely close. The base pass tests
        // CF_DepthNearOrEqual, so an exact tie is resolved by draw order — identically every
        // frame and under every projection — and there is no tie left for a near-plane
        // perturbation to re-decide. Measured: a face-on camera reports 0 affected pixels on
        // this fixture at 1920x1920, at nearPlaneRatio 3, 5, 7 and 11, and in farPlane mode.
        // Tilting one slab by 1 degree does not help either: fighting needs the two surfaces
        // within ~6e-8 of W, and a 1-degree gradient crosses that band over ~1e-3 uu, which is
        // far under one pixel.
        //
        // Seen obliquely, the same two slabs interpolate the same plane through DIFFERENT
        // triangulations (the second is yawed 45 degrees), the per-pixel rounding differs, and
        // the perturbation re-decides a genuine population of near-ties.
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), ZFightFixtureOrigin.X - 800.0);
        Location->SetNumberField(TEXT("y"), ZFightFixtureOrigin.Y);
        Location->SetNumberField(TEXT("z"), ZFightFixtureOrigin.Z + 150.0);
        Payload->SetObjectField(TEXT("location"), Location);

        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), -15.0);
        Rotation->SetNumberField(TEXT("yaw"), 0.0);
        Rotation->SetNumberField(TEXT("roll"), 0.0);
        Payload->SetObjectField(TEXT("rotation"), Rotation);

        if (Resolution > 0)
        {
            Payload->SetNumberField(TEXT("width"), Resolution);
            Payload->SetNumberField(TEXT("height"), Resolution);
        }

        // Leaving width/height unset takes the production default. ResolutionComparison measures
        // this exact fixture at both 1920 and 768 and guards the decision to use 768.
        Payload->SetBoolField(TEXT("mask"), false);
        return Payload;
    }

    // Shared skip gate. A run with no RHI, no editor world, or no scene-capture support must
    // not fail the suite, but it must also not pass the pixel assertions by accident, so an
    // unsuccessful response has to carry a TYPED failure rather than any failure.
    bool ZFightResponseUsable(FAutomationTestBase& Test, const FTestResponseCapture& Capture)
    {
        if (Capture.bSuccess)
        {
            return true;
        }
        const bool bTyped =
            Capture.ErrorCode == TEXT("SCENE_CAPTURE_FAILED") ||
            Capture.ErrorCode == TEXT("READ_PIXELS_FAILED") ||
            Capture.ErrorCode == TEXT("RENDER_TARGET_CREATE_FAILED") ||
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE");
        Test.TestTrue(TEXT("failure is typed when offscreen capture is unavailable"), bTyped);
        Test.AddInfo(TEXT("Skipped pixel assertions: offscreen scene capture unavailable in this run."));
        return false;
    }

    struct FZFightResolutionReading
    {
        double AnalyzedPixels = 0.0;
        double AffectedPixels = 0.0;
        int32 RegionCount = 0;
        bool bPass = false;
    };

    bool MeasureZFightResolution(FAutomationTestBase& Test, int32 Resolution,
        FZFightResolutionReading& OutReading)
    {
        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("render.detect_z_fighting handler found"),
                InvokeHandlerWithCapture(
                    TEXT("render.detect_z_fighting"), MakeZFightPayload(Resolution), Capture)))
        {
            return false;
        }
        ON_SCOPE_EXIT { DeleteZFightMaskIfPresent(Capture); };
        if (!ZFightResponseUsable(Test, Capture) || !Capture.Result.IsValid())
        {
            return false;
        }

        Capture.Result->TryGetNumberField(TEXT("analyzedPixels"), OutReading.AnalyzedPixels);
        Capture.Result->TryGetNumberField(TEXT("affectedPixels"), OutReading.AffectedPixels);
        Capture.Result->TryGetBoolField(TEXT("pass"), OutReading.bPass);
        const TArray<TSharedPtr<FJsonValue>>* Regions = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("regions"), Regions) && Regions)
        {
            OutReading.RegionCount = Regions->Num();
        }
        return Test.TestTrue(TEXT("the resolution comparison analysed pixels"),
            OutReading.AnalyzedPixels > 0.0);
    }
}

// ============================================================================
// A power-of-two nearPlaneRatio must be REJECTED, not quietly obeyed.
//
// Under the infinite-far reversed-Z projection a scene capture builds, clip-space Z is the
// near plane itself, so scaling the near plane by a power of two scales every stored depth by
// that same power of two exactly — no rounding changes, no depth comparison anywhere in the
// frame can flip, and the analysis is guaranteed to report zero affected pixels on any scene
// whatsoever. That is a detector which passes everything. This test exists because 4.0 is an
// entirely natural-looking value for a caller to pass.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderDetectZFightingRejectsPowerOfTwoRatioTest,
    "PinWright.render.detect_z_fighting.RejectsPowerOfTwoNearPlaneRatio",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderDetectZFightingRejectsPowerOfTwoRatioTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    for (const double Ratio : { 2.0, 4.0, 8.0, 1.0, 0.5 })
    {
        TSharedPtr<FJsonObject> Payload = MakeZFightPayload();
        Payload->SetNumberField(TEXT("nearPlaneRatio"), Ratio);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.detect_z_fighting handler found"),
            InvokeHandlerWithCapture(TEXT("render.detect_z_fighting"), Payload, Capture));
        ON_SCOPE_EXIT { DeleteZFightMaskIfPresent(Capture); };

        TestFalse(FString::Printf(TEXT("nearPlaneRatio %g is rejected"), Ratio), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("nearPlaneRatio %g is rejected as INVALID_ARGUMENT"), Ratio),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(FString::Printf(TEXT("nearPlaneRatio %g rejection explains why"), Ratio),
            Capture.Message.Contains(TEXT("power of two")));
    }

    // The documented default must itself be a legal value.
    TestFalse(TEXT("the default nearPlaneRatio is not a power of two"),
        PinWrightZFighting::IsPowerOfTwoRatio(PinWrightZFighting::DefaultNearPlaneRatio));

    return true;
}

// ============================================================================
// Coplanar fixture: the detector must report a NON-ZERO affected count and FAIL.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderDetectZFightingCoplanarFixtureTest,
    "PinWright.render.detect_z_fighting.CoplanarFixtureIsDetected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderDetectZFightingCoplanarFixtureTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* First = nullptr;
    AStaticMeshActor* Second = nullptr;
    FString SkipReason;
    if (!SpawnZFightFixture(World, /*SecondSlabZOffset=*/0.0, First, Second, SkipReason))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-fixture-unavailable"),
            FString::Printf(TEXT("Skipped: %s."), *SkipReason));
        return true;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.detect_z_fighting handler found"),
        InvokeHandlerWithCapture(TEXT("render.detect_z_fighting"), MakeZFightPayload(), Capture));
    ON_SCOPE_EXIT { DeleteZFightMaskIfPresent(Capture); };

    if (!ZFightResponseUsable(*this, Capture))
    {
        return true;
    }
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("success carried no result object"));
        return false;
    }

    double AnalyzedPixels = 0.0;
    Capture.Result->TryGetNumberField(TEXT("analyzedPixels"), AnalyzedPixels);
    if (AnalyzedPixels <= 0.0)
    {
        // Nothing was measured, so this run says nothing about the detector. Fail rather than
        // pass: "could not check" reported as a pass is the failure mode this whole verb is
        // built to avoid, and a test that tolerates it re-introduces it.
        AddError(TEXT("the coplanar fixture produced no analysable pixels; the fixture was not in frame"));
        return false;
    }

    double AffectedPixels = 0.0;
    TestTrue(TEXT("result carries affectedPixels"),
        Capture.Result->TryGetNumberField(TEXT("affectedPixels"), AffectedPixels));
    TestTrue(
        FString::Printf(TEXT("two coplanar slabs are detected as fighting (affectedPixels=%.0f of %.0f analysed)"),
            AffectedPixels, AnalyzedPixels),
        AffectedPixels > 0.0);

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a coplanar fixture fails the gate"), bPass);

    const TArray<TSharedPtr<FJsonValue>>* Regions = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("regions"), Regions) && Regions && Regions->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* Region = nullptr;
        if ((*Regions)[0]->TryGetObject(Region) && Region)
        {
            double RegionPixels = 0.0;
            TestTrue(TEXT("the reported region has a pixel count"),
                (*Region)->TryGetNumberField(TEXT("pixels"), RegionPixels) && RegionPixels > 0.0);
            TestTrue(TEXT("the reported region is placed in the world"),
                (*Region)->HasField(TEXT("worldLocation")));
        }
    }
    else
    {
        AddError(TEXT("affected pixels were reported but no region was clustered from them"));
    }

    return true;
}

// ============================================================================
// Separated fixture: the SAME two surfaces, pulled apart, must report ZERO.
//
// This is the half that proves the detector is measuring depth-comparison outcomes rather
// than simply reacting to two differently-coloured surfaces overlapping on screen.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderDetectZFightingSeparatedFixtureTest,
    "PinWright.render.detect_z_fighting.SeparatedFixtureIsClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderDetectZFightingSeparatedFixtureTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* First = nullptr;
    AStaticMeshActor* Second = nullptr;
    FString SkipReason;
    // 50 cm apart: far beyond any depth-buffer resolution at this range, so the nearer slab
    // wins every pixel under both near planes.
    if (!SpawnZFightFixture(World, /*SecondSlabZOffset=*/50.0, First, Second, SkipReason))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-fixture-unavailable"),
            FString::Printf(TEXT("Skipped: %s."), *SkipReason));
        return true;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.detect_z_fighting handler found"),
        InvokeHandlerWithCapture(TEXT("render.detect_z_fighting"), MakeZFightPayload(), Capture));
    ON_SCOPE_EXIT { DeleteZFightMaskIfPresent(Capture); };

    if (!ZFightResponseUsable(*this, Capture))
    {
        return true;
    }
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("success carried no result object"));
        return false;
    }

    double AnalyzedPixels = 0.0;
    Capture.Result->TryGetNumberField(TEXT("analyzedPixels"), AnalyzedPixels);
    if (AnalyzedPixels <= 0.0)
    {
        AddError(TEXT("the separated fixture produced no analysable pixels; the fixture was not in frame"));
        return false;
    }

    double AffectedPixels = 0.0;
    Capture.Result->TryGetNumberField(TEXT("affectedPixels"), AffectedPixels);
    TestEqual(
        FString::Printf(TEXT("two clearly separated slabs report no fighting (of %.0f analysed)"), AnalyzedPixels),
        AffectedPixels, 0.0);

    bool bPass = false;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestTrue(TEXT("a separated fixture passes the gate"), bPass);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderDetectZFightingResolutionComparisonTest,
    "PinWright.render.detect_z_fighting.ResolutionComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderDetectZFightingResolutionComparisonTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world for z-fighting resolution comparison."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* First = nullptr;
    AStaticMeshActor* Second = nullptr;
    FString SkipReason;
    if (!SpawnZFightFixture(World, /*SecondSlabZOffset=*/0.0, First, Second, SkipReason))
    {
        AddWarning(FString::Printf(
            TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: z-fighting fixture unavailable: %s."), *SkipReason));
        return true;
    }

    FZFightResolutionReading Seam1920;
    FZFightResolutionReading Seam768;
    if (!MeasureZFightResolution(*this, 1920, Seam1920) ||
        !MeasureZFightResolution(*this, 768, Seam768))
    {
        return true;
    }
    TestTrue(TEXT("known coplanar seam is detected at 1920"), Seam1920.AffectedPixels > 0.0);
    TestTrue(TEXT("known coplanar seam is still detected at 768"), Seam768.AffectedPixels > 0.0);
    TestEqual(TEXT("768 preserves every flagged seam region"),
        Seam768.RegionCount, Seam1920.RegionCount);
    TestFalse(TEXT("known seam fails the gate at 1920"), Seam1920.bPass);
    TestFalse(TEXT("known seam fails the gate at 768"), Seam768.bPass);

    const FVector SeparatedLocation = Second->GetActorLocation() + FVector(0.0, 0.0, 50.0);
    TestTrue(TEXT("the control slab was separated"),
        Second->SetActorLocation(SeparatedLocation, false, nullptr, ETeleportType::TeleportPhysics));

    FZFightResolutionReading Clean1920;
    FZFightResolutionReading Clean768;
    if (!MeasureZFightResolution(*this, 1920, Clean1920) ||
        !MeasureZFightResolution(*this, 768, Clean768))
    {
        return true;
    }
    TestEqual(TEXT("separated control stays clean at 1920"), Clean1920.AffectedPixels, 0.0);
    TestEqual(TEXT("separated control stays clean at 768"), Clean768.AffectedPixels, 0.0);
    TestTrue(TEXT("separated control passes at 1920"), Clean1920.bPass);
    TestTrue(TEXT("separated control passes at 768"), Clean768.bPass);

    UE_LOG(LogTemp, Display,
        TEXT("PINWRIGHT_ZFIGHT_RESOLUTION_MEASUREMENT seam1920 affected=%.0f regions=%d; ")
        TEXT("seam768 affected=%.0f regions=%d; clean1920 affected=%.0f regions=%d; ")
        TEXT("clean768 affected=%.0f regions=%d"),
        Seam1920.AffectedPixels, Seam1920.RegionCount,
        Seam768.AffectedPixels, Seam768.RegionCount,
        Clean1920.AffectedPixels, Clean1920.RegionCount,
        Clean768.AffectedPixels, Clean768.RegionCount);
    return true;
}

// ============================================================================
// Mask downscaling must MAX-POOL, not average.
//
// Headless unit test, no RHI. A one-pixel seam at analysis resolution has to survive into the
// mask as a visible pixel; averaging would fade it below notice, which is the same
// false-negative shape as under-sampling the analysis itself.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderZFightingMaskPreservesSinglePixelTest,
    "PinWright.render.detect_z_fighting.MaskDownscalePreservesSinglePixel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderZFightingMaskPreservesSinglePixelTest::RunTest(const FString& Parameters)
{
    constexpr int32 Width = 64;
    constexpr int32 Height = 64;

    TArray<uint8> Mask;
    Mask.SetNumZeroed(Width * Height);
    Mask[33 * Width + 17] = PinWrightZFighting::MASK_Flagged;

    const FIntPoint MaskSize = PinWrightZFighting::ResolveMaskSize(Width, Height, 8);
    TestEqual(TEXT("mask size caps the long edge"), MaskSize.X, 8);
    TestEqual(TEXT("mask size preserves aspect"), MaskSize.Y, 8);

    TArray<FColor> Bitmap;
    PinWrightZFighting::BuildMaskBitmap(Mask, Width, Height, MaskSize.X, MaskSize.Y, Bitmap);
    TestEqual(TEXT("mask bitmap has one pixel per output cell"), Bitmap.Num(), MaskSize.X * MaskSize.Y);

    int32 AffectedCells = 0;
    int32 NonOpaque = 0;
    for (const FColor& Pixel : Bitmap)
    {
        if (Pixel.R > 128)
        {
            ++AffectedCells;
        }
        if (Pixel.A != 255)
        {
            ++NonOpaque;
        }
    }
    TestEqual(TEXT("a single affected pixel survives an 8x downscale"), AffectedCells, 1);
    TestEqual(TEXT("the mask bitmap is fully opaque"), NonOpaque, 0);

    // ResolveMaskSize must never upscale: a small analysis produces a small mask.
    const FIntPoint NoUpscale = PinWrightZFighting::ResolveMaskSize(100, 50, 640);
    TestEqual(TEXT("mask is not upscaled (width)"), NoUpscale.X, 100);
    TestEqual(TEXT("mask is not upscaled (height)"), NoUpscale.Y, 50);

    return true;
}

// ============================================================================
// Clip exclusion must not be silently countable as clean.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderZFightingClipExclusionTest,
    "PinWright.render.detect_z_fighting.ClipExclusionRemovesFlaggedPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderZFightingClipExclusionTest::RunTest(const FString& Parameters)
{
    TArray<uint8> Mask;
    Mask.SetNumZeroed(4);
    Mask[0] = PinWrightZFighting::MASK_Flagged; // inside the trusted range
    Mask[1] = PinWrightZFighting::MASK_Flagged; // nearer than the perturbed near plane
    Mask[2] = PinWrightZFighting::MASK_Flagged; // beyond the far plane
    Mask[3] = PinWrightZFighting::MASK_Flagged; // no usable depth

    TArray<FLinearColor> Depth;
    Depth.Add(FLinearColor(500.0f, 0.0f, 0.0f, 1.0f));
    Depth.Add(FLinearColor(20.0f, 0.0f, 0.0f, 1.0f));
    Depth.Add(FLinearColor(90000.0f, 0.0f, 0.0f, 1.0f));
    Depth.Add(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));

    const int32 Excluded = PinWrightZFighting::ApplyDepthRangeExclusion(Depth, 30.0, 80000.0, Mask);
    TestEqual(TEXT("three of four pixels are excluded"), Excluded, 3);
    TestEqual(TEXT("only the in-range pixel counts as affected"),
        PinWrightZFighting::CountAffected(Mask), 1);

    return true;
}
