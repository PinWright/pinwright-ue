// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-landscape-sculpt-stale-bounds.
//
// On UE 5.7 landscapes are edit-layer based. landscape.sculpt / landscape.edit
// write the heightmap via FLandscapeEditDataInterface::SetHeightData + Flush(),
// but SetHeightData does NOT recompute the component bounds synchronously — it
// only RequestHeightmapUpdate()s a DEFERRED layer regeneration
// (LandscapeEditInterface.cpp:438) and Flush() merely uploads the dirty texels
// to the GPU. The pass that fixes bounds — RegenerateLayersHeightmaps ->
// UpdateForChangedHeightmaps -> ULandscapeComponent::UpdateCachedBounds()
// (LandscapeEditLayers.cpp:5058) — runs on a LATER editor tick, after the RPC
// has already returned. So before the fix a same-turn bounds readback
// (CachedLocalBox.Max.Z, and the live actor.get_bounding_box that derives from
// it) still reported the pre-sculpt flat Z=0 even though the raise landed.
//
// The fix calls ALandscape::ForceUpdateLayersContent() after the flushing
// SetHeightData+Flush (LandscapeHandler.cpp landscape.sculpt / landscape.edit),
// which drives the GPU heightmap readback + merge + RegenerateLayersHeightmaps
// synchronously (the same path the interactive sculpt tool uses to flush pending
// evaluation), so UpdateCachedBounds() runs before the RPC returns and a
// same-turn bounds readback reflects the new relief.
//
// This test builds its fixture IN-CODE: it creates a real 2x2 edit-layer
// landscape through the production landscape.create handler (whose CreateDefaultLayer()
// path makes it edit-layer based, exactly the model the bug lives in), reads the
// baseline flat CachedLocalBox off the spawned ULandscapeComponent, runs a real
// Raise sculpt through landscape.sculpt, then re-reads CachedLocalBox on the SAME
// component in the same turn. It exercises production code (the registered
// handlers + the engine's edit-layer pipeline), loads no external content, and
// treats a landscape it fails to create as a FAILURE, not a skip.
//
// Counterfactual: revert the ForceUpdateLayersContent() call in landscape.sculpt
// and the deferred regeneration never runs within the RPC, so the post-sculpt
// CachedLocalBox.Max.Z stays at the flat baseline (~0, or 1 after the engine's
// zero-extent flicker-guard expansion) and the "raised above baseline" assertion
// below fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Returns the first ULandscapeComponent of the ALandscape in World whose
    // actor label matches Label, or nullptr if the actor / a component is absent.
    ULandscapeComponent* FindFirstLandscapeComponent(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            ALandscape* Landscape = *It;
            if (!Landscape->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
            {
                continue;
            }
            for (ULandscapeComponent* Comp : Landscape->LandscapeComponents)
            {
                if (Comp)
                {
                    return Comp;
                }
            }
        }
        return nullptr;
    }
}

// ---- landscape.sculpt refreshes the component bounds after a raise ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptRefreshesBoundsTest,
    "PinWright.landscape.sculpt.RefreshesBoundsAfterRaise",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptRefreshesBoundsTest::RunTest(const FString& Parameters)
{
    // The edit-layer readback + regeneration this test drives requires a real
    // editor world (the automation suite runs in an editor context). The world is
    // an environment precondition, not the test fixture — the fixture (the
    // landscape) is built in-code below and its absence IS a failure.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.sculpt bounds-refresh test"));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));
    TestTrue(TEXT("landscape.sculpt handler registered"),
        IsHandlerRegistered(TEXT("landscape.sculpt")));

    // Destroy any actor spawned during the test and restore the level dirty flag
    // on scope exit, so this test leaves the open map exactly as it found it.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString LandscapeLabel = FString::Printf(TEXT("MCP_SculptBounds_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // --- Build the fixture in-code: a 2x2, 63-quad edit-layer landscape (the
    //     ticket's repro shape). CreateDefaultLayer() makes it edit-layer based. ---
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), LandscapeLabel);
        CreatePayload->SetNumberField(TEXT("componentsX"), 2);
        CreatePayload->SetNumberField(TEXT("componentsY"), 2);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 63);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> CreateCapture = MakeShared<FTestResponseCapture>();
        const bool bCreateFound = InvokeHandlerWithSharedCapture(
            TEXT("landscape.create"), CreatePayload, CreateCapture);
        TestTrue(TEXT("landscape.create invoked"), bCreateFound);
        if (!bCreateFound)
        {
            return false;
        }
        PumpUntilCaptured(*CreateCapture, /*TimeoutSeconds=*/30.0);
        TestTrue(TEXT("landscape.create responded"), CreateCapture->bWasCalled);
        // The landscape fixture is REQUIRED — a create that did not succeed is a
        // test failure, not a skip.
        TestTrue(TEXT("landscape.create succeeded (fixture built)"), CreateCapture->bSuccess);
        if (!CreateCapture->bWasCalled || !CreateCapture->bSuccess)
        {
            return false;
        }
    }

    // Locate the spawned landscape's first component — the field the ticket's
    // repro read back (CachedLocalBox). Its absence means the fixture is unusable.
    ULandscapeComponent* Component = FindFirstLandscapeComponent(World, LandscapeLabel);
    TestNotNull(TEXT("spawned landscape has a ULandscapeComponent"), Component);
    if (!Component)
    {
        return false;
    }

    // Baseline: a freshly-created flat landscape reports Max.Z == 0 (the repro's
    // "Max:[63,63,0]"). Assert the fixture really starts flat so the post-sculpt
    // rise is attributable to the sculpt, not to pre-existing relief.
    const double BaselineMaxZ = Component->CachedLocalBox.Max.Z;
    TestTrue(
        FString::Printf(TEXT("baseline CachedLocalBox.Max.Z is flat (~0), got %f"), BaselineMaxZ),
        BaselineMaxZ <= 1.5);

    // --- Run a real Raise sculpt at the landscape center (world ~ [6300,6300]
    //     for a 2x2 x 63-quad grid at unit-ish scale), matching the repro. ---
    {
        TSharedPtr<FJsonObject> SculptPayload = MakeShared<FJsonObject>();
        SculptPayload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 6300.0);
        Loc->SetNumberField(TEXT("y"), 6300.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        SculptPayload->SetObjectField(TEXT("location"), Loc);
        SculptPayload->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        SculptPayload->SetNumberField(TEXT("brushRadius"), 4000.0);
        SculptPayload->SetNumberField(TEXT("brushFalloff"), 0.5);
        // strength is an open-ended per-stamp multiplier (see landscape.sculpt param
        // doc). The handler raises the peak vertex by strength*100*(128/ScaleZ) height
        // units, i.e. strength*100 CENTIMETERS of world relief regardless of ScaleZ.
        // A large value here makes the resulting rise (tens of local-space units) sit
        // far above the engine's 1-unit zero-extent flicker guard, so the bounds-refresh
        // assertion below is unambiguous — a fix that only nudged the box to the guard
        // value would still fail it. This is about a clearly measurable stimulus, not a
        // realistic brush setting.
        SculptPayload->SetNumberField(TEXT("strength"), 150.0);

        TSharedRef<FTestResponseCapture> SculptCapture = MakeShared<FTestResponseCapture>();
        const bool bSculptFound = InvokeHandlerWithSharedCapture(
            TEXT("landscape.sculpt"), SculptPayload, SculptCapture);
        TestTrue(TEXT("landscape.sculpt invoked"), bSculptFound);
        if (!bSculptFound)
        {
            return false;
        }
        PumpUntilCaptured(*SculptCapture, /*TimeoutSeconds=*/30.0);
        TestTrue(TEXT("landscape.sculpt responded"), SculptCapture->bWasCalled);
        TestTrue(TEXT("landscape.sculpt succeeded"), SculptCapture->bSuccess);
        if (!SculptCapture->bWasCalled || !SculptCapture->bSuccess)
        {
            return false;
        }
        // Guard that the raise actually touched vertices — if it were a no-op,
        // there would be nothing for the bounds refresh to reflect and the core
        // assertion below would be meaningless.
        if (SculptCapture->Result.IsValid())
        {
            double ModifiedVertices = 0.0;
            SculptCapture->Result->TryGetNumberField(TEXT("modifiedVertices"), ModifiedVertices);
            TestTrue(
                FString::Printf(TEXT("sculpt modified vertices (got %f)"), ModifiedVertices),
                ModifiedVertices > 0.0);
        }
    }

    // --- Core regression assertion ---
    // In the SAME turn (no extra editor ticks beyond the handler's own
    // ForceUpdateLayersContent), the component's cached bounds must now reflect
    // the raise: Max.Z is clearly positive, well above the engine's 1-unit
    // zero-extent flicker guard. Before the fix this stayed at the flat baseline
    // (~0/1) because the deferred edit-layer regeneration had not run within the
    // RPC.
    const double PostSculptMaxZ = Component->CachedLocalBox.Max.Z;
    TestTrue(
        FString::Printf(
            TEXT("after Raise, CachedLocalBox.Max.Z reflects relief (baseline %f -> %f, expected > 5)"),
            BaselineMaxZ, PostSculptMaxZ),
        PostSculptMaxZ > 5.0);
    TestTrue(
        FString::Printf(TEXT("post-sculpt Max.Z (%f) rose above baseline (%f)"),
            PostSculptMaxZ, BaselineMaxZ),
        PostSculptMaxZ > BaselineMaxZ);

    return true;
}
