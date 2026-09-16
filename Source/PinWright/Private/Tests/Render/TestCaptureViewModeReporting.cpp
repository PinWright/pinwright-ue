// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the capture-response view-mode contract.
//
// The defect: a capture came back as a PURE WIREFRAME FRAME and did not trip BLANK_CAPTURE.
// It could not — BLANK_CAPTURE (PreviewViewportCaptureUtils.cpp, CalculateCaptureImageStats)
// detects an EMPTY frame by luminance mean/variance, and a wireframe frame is not empty, it is
// full of bright lines. The viewport had been left in "Wireframe only" by earlier work
// (editor.set_view_mode deliberately does not self-restore), so an agent doing visual
// verification received a confident-looking image showing none of the material, lighting or
// colour work it was sent to check, with nothing in the response saying so.
//
// These tests assert the FAILURE direction: that a non-Lit capture SAYS it is non-Lit. A test
// that a lit capture succeeds would not have caught the original defect.
//
// Headless-safe by construction: MakeViewportInfoObject is a pure function of
// FViewportCaptureOutput, so the reporting contract is testable without a live viewport (the
// capture itself needs an interactive editor). That separation is why the block is built by a
// shared function instead of inline in each of the six capture handlers.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Dom/JsonObject.h"
#include "Engine/EngineBaseTypes.h"

namespace
{
    // Build the capture output a real capture in ViewMode would produce, filling only the
    // view-mode fields CaptureEditorViewportToPng derives from the viewport client.
    PinWrightRenderCapture::FViewportCaptureOutput MakeCaptureIn(EViewModeIndex ViewMode)
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.ViewportType = TEXT("perspective");
        Capture.ViewModeValue = static_cast<int32>(ViewMode);
        Capture.ViewModeKey = PinWrightRenderCapture::GetViewModeKey(ViewMode);
        Capture.bLitViewMode = PinWrightRenderCapture::IsLitViewMode(ViewMode);
        // Stands in for UViewModeUtils::GetViewModeDisplayName, which needs no editor but is
        // localized; the tests below never assert on this string's content for that reason.
        Capture.ViewMode = FString::Printf(TEXT("display:%s"), *Capture.ViewModeKey);
        return Capture;
    }
}

// The core regression: the exact mode the reported capture was taken in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeWireframeIsReportedTest,
    "PinWright.render.capture_view_mode.WireframeCaptureReportsNotLit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeWireframeIsReportedTest::RunTest(const FString& Parameters)
{
    // VMI_BrushWireframe is the mode the editor UI calls "Wireframe only" and the mode
    // editor.set_view_mode sets for viewMode:"Wireframe" — i.e. the one the viewport was
    // actually left in.
    const PinWrightRenderCapture::FViewportCaptureOutput Capture =
        MakeCaptureIn(VMI_BrushWireframe);
    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(Capture);

    if (!TestTrue(TEXT("viewport block built"), Viewport.IsValid()))
    {
        return true;
    }

    bool bLit = true;
    TestTrue(TEXT("`lit` is present"), Viewport->TryGetBoolField(TEXT("lit"), bLit));
    TestFalse(TEXT("a wireframe capture reports lit=false"), bLit);

    FString Key;
    TestTrue(TEXT("`viewModeKey` is present"),
        Viewport->TryGetStringField(TEXT("viewModeKey"), Key));
    // The key round-trips into editor.set_view_mode, so it must be that verb's spelling
    // ("Wireframe"), not the engine display name ("Wireframe only") and not the enumerator.
    TestEqual(TEXT("key is the editor.set_view_mode spelling"), Key, FString(TEXT("Wireframe")));

    double ModeValue = -1.0;
    TestTrue(TEXT("`viewModeValue` is present"),
        Viewport->TryGetNumberField(TEXT("viewModeValue"), ModeValue));
    TestEqual(TEXT("raw enum value is the unlocalized machine key"),
        static_cast<int32>(ModeValue), static_cast<int32>(VMI_BrushWireframe));

    FString Warning;
    if (TestTrue(TEXT("a non-Lit capture carries viewModeWarning"),
            Viewport->TryGetStringField(TEXT("viewModeWarning"), Warning)))
    {
        // The warning has to name the mode; "something is wrong" would leave the agent to
        // guess which of its captures is untrustworthy.
        TestTrue(TEXT("warning names the mode"), Warning.Contains(TEXT("Wireframe")));
        // ...and it has to name the remedy, because the viewport does not restore itself and
        // every later capture inherits the same broken mode.
        TestTrue(TEXT("warning names the remedy verb"),
            Warning.Contains(TEXT("editor.set_view_mode")));
    }
    return true;
}

// The other half of the contract: a genuine lit capture must not cry wolf, or the warning
// becomes noise and stops being read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeLitIsCleanTest,
    "PinWright.render.capture_view_mode.LitCaptureCarriesNoWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeLitIsCleanTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(MakeCaptureIn(VMI_Lit));

    bool bLit = false;
    TestTrue(TEXT("`lit` is present"), Viewport->TryGetBoolField(TEXT("lit"), bLit));
    TestTrue(TEXT("VMI_Lit reports lit=true"), bLit);
    TestFalse(TEXT("a lit capture carries no viewModeWarning"),
        Viewport->HasField(TEXT("viewModeWarning")));

    FString Key;
    Viewport->TryGetStringField(TEXT("viewModeKey"), Key);
    TestEqual(TEXT("lit key"), Key, FString(TEXT("Lit")));
    return true;
}

// Every mode that hides materials or lighting must classify as not-lit. These are the modes a
// viewport is realistically left in — the collision pair in particular is reachable through the
// documented editor.collision-review workflow, which is also the reason a non-Lit capture warns
// instead of hard-failing: that workflow captures in collision mode on purpose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeLitClassificationTest,
    "PinWright.render.capture_view_mode.DebugModesClassifyAsNotLit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeLitClassificationTest::RunTest(const FString& Parameters)
{
    const EViewModeIndex NotLit[] = {
        VMI_BrushWireframe,          // "Wireframe only" — the mode that caused the defect
        VMI_Wireframe,               // CSG wireframe
        VMI_Unlit,                   // materials without lighting
        VMI_Lit_DetailLighting,      // lighting without materials
        VMI_LightingOnly,            // lighting without materials
        VMI_LightComplexity,
        VMI_ShaderComplexity,
        VMI_LightmapDensity,
        VMI_StationaryLightOverlap,
        VMI_ReflectionOverride,
        VMI_CollisionPawn,           // editor.set_view_mode "CollisionSimple"
        VMI_CollisionVisibility,     // editor.set_view_mode "CollisionComplex"
        VMI_VisualizeBuffer,
        VMI_VisualizeLumen,
        VMI_VisualizeNanite,
        VMI_LODColoration,
        VMI_QuadOverdraw,
        VMI_Unknown,
    };
    for (const EViewModeIndex Mode : NotLit)
    {
        TestFalse(FString::Printf(TEXT("mode %d is not lit"), static_cast<int32>(Mode)),
            PinWrightRenderCapture::IsLitViewMode(Mode));
    }

    // The only two that qualify. VMI_PathTracing is a fully shaded, fully lit reference render,
    // so excluding it would fire a false warning on the highest-fidelity capture available.
    TestTrue(TEXT("VMI_Lit is lit"), PinWrightRenderCapture::IsLitViewMode(VMI_Lit));
    TestTrue(TEXT("VMI_PathTracing is lit"), PinWrightRenderCapture::IsLitViewMode(VMI_PathTracing));
    return true;
}

// The key table is hand-written because the engine's own stable-name function GetViewModeName
// (UE 5.8 Runtime/Engine/Public/ShowFlags.h:623, defined in Private/ShowFlags.cpp:981) carries
// no ENGINE_API and cannot be linked from this module. A hand-written 45-entry table's realistic
// failure is a copy-pasted duplicate, which would make two different modes report the same name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeKeysAreDistinctTest,
    "PinWright.render.capture_view_mode.KeysAreDistinctAndSpecific",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeKeysAreDistinctTest::RunTest(const FString& Parameters)
{
    TSet<FString> Seen;
    for (int32 Index = 0; Index < static_cast<int32>(VMI_Max); ++Index)
    {
        const FString Key = PinWrightRenderCapture::GetViewModeKey(
            static_cast<EViewModeIndex>(Index));
        // "Unknown" is the deliberate catch-all for the gaps in the enum's numbering (7, 13, 17
        // are unused) and for the deprecated VMI_Lit_Wireframe, so it is allowed to repeat.
        if (Key == TEXT("Unknown"))
        {
            continue;
        }
        TestFalse(FString::Printf(TEXT("key '%s' (mode %d) is not a duplicate"), *Key, Index),
            Seen.Contains(Key));
        Seen.Add(Key);
    }
    TestTrue(TEXT("the table actually covers the enum"), Seen.Num() > 30);

    // An out-of-range value must degrade to "Unknown" rather than read past the table.
    TestEqual(TEXT("VMI_Unknown degrades safely"),
        FString(PinWrightRenderCapture::GetViewModeKey(VMI_Unknown)), FString(TEXT("Unknown")));

    // These twelve are the ones editor.set_view_mode accepts (its parse chain lives in
    // Handlers/Editor/ViewportHandler.cpp). This block is the drift guard between the two
    // halves of the mapping: if either side is respelled, the capture response stops
    // round-tripping into set_view_mode and this fails.
    TestEqual(TEXT("Lit"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_Lit)), FString(TEXT("Lit")));
    TestEqual(TEXT("Unlit"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_Unlit)), FString(TEXT("Unlit")));
    TestEqual(TEXT("Wireframe"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_BrushWireframe)), FString(TEXT("Wireframe")));
    TestEqual(TEXT("DetailLighting"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_Lit_DetailLighting)), FString(TEXT("DetailLighting")));
    TestEqual(TEXT("LightingOnly"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_LightingOnly)), FString(TEXT("LightingOnly")));
    TestEqual(TEXT("LightComplexity"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_LightComplexity)), FString(TEXT("LightComplexity")));
    TestEqual(TEXT("ShaderComplexity"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_ShaderComplexity)), FString(TEXT("ShaderComplexity")));
    TestEqual(TEXT("LightmapDensity"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_LightmapDensity)), FString(TEXT("LightmapDensity")));
    TestEqual(TEXT("StationaryLightOverlap"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_StationaryLightOverlap)), FString(TEXT("StationaryLightOverlap")));
    TestEqual(TEXT("ReflectionOverride"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_ReflectionOverride)), FString(TEXT("ReflectionOverride")));
    TestEqual(TEXT("CollisionSimple"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_CollisionPawn)), FString(TEXT("CollisionSimple")));
    TestEqual(TEXT("CollisionComplex"), FString(PinWrightRenderCapture::GetViewModeKey(VMI_CollisionVisibility)), FString(TEXT("CollisionComplex")));
    return true;
}

// ---- the four substitution-only debug modes, and why the set is exactly four ----
//
// front_back_face, clay, zebra and random_color are drawn ONLY by swapping each mesh batch's
// material inside ApplyViewModeOverrides (PrimitiveDrawingUtils.cpp:1750-1805). Every other mode
// either leaves materials alone or is implemented by a debug-view SHADER, which is a different
// engine path with different reachability. The distinction is not visible in any show flag, so it
// cannot be computed the way DistinguishingShowFlags is -- it is a fact about where the engine
// implements the mode, and this test is what pins the hand-written list to that fact.
//
// The classification is load-bearing rather than cosmetic: it gates the response's
// viewModeNaniteWarning, and a mode wrongly ADDED here would warn on captures that are fine,
// while a mode wrongly OMITTED would let a false pass through silently. The omission is the
// expensive direction, which is why the negative assertions below are as explicit as the
// positive ones.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeSubstitutionSetTest,
    "PinWright.render.capture_view_mode.SubstitutionOnlyModesAreExactlyFour",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeSubstitutionSetTest::RunTest(const FString& Parameters)
{
    // The four the engine implements as a material swap. All four enumerators arrived in UE 5.7;
    // on an older engine there is no such mode to classify, so the positive half of this test has
    // no subject and the negatives below carry it.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    TestTrue(TEXT("front_back_face substitutes"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_FrontBackFace));
    TestTrue(TEXT("clay substitutes"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_Clay));
    TestTrue(TEXT("zebra substitutes"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_Zebra));
    TestTrue(TEXT("random_color substitutes"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_RandomColor));
#endif

    // Lit and Unlit are the obvious negatives. Wireframe is the interesting one: it IS handled
    // inside the same function, but through Mesh.bWireframe plus GEngine->WireframeMaterial on a
    // branch that runs before the four, and it is additionally reproduced by the static-mesh proxy
    // itself (StaticMeshSceneProxy.cpp GetWireframeMeshElement), so it does not share their
    // reachability and must not inherit their warning.
    TestFalse(TEXT("Lit does not substitute"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_Lit));
    TestFalse(TEXT("Unlit does not substitute"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_Unlit));
    TestFalse(TEXT("Wireframe does not substitute"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_Wireframe));
    TestFalse(TEXT("ShaderComplexity is a debug-view shader, not a substitution"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_ShaderComplexity));
    TestFalse(TEXT("LightingOnly does not substitute"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_LightingOnly));
    TestFalse(TEXT("LODColoration does not substitute"),
        PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(VMI_LODColoration));

    // Exactly four across the whole enum -- the guard against a fifth being added here without
    // the engine evidence that it belongs.
    int32 Count = 0;
    for (int32 Index = 0; Index < static_cast<int32>(VMI_Max); ++Index)
    {
        if (PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(
                static_cast<EViewModeIndex>(Index)))
        {
            ++Count;
        }
    }
    // Four from UE 5.7, none before it: the enumerators themselves are what the engine added, so
    // the expected total tracks the vocabulary the engine actually offers rather than a constant
    // that would have to be wrong on one side of 5.7.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    TestEqual(TEXT("exactly four modes are substitution-only"), Count, 4);
#else
    TestEqual(TEXT("no mode is substitution-only before the engine adds them"), Count, 0);
#endif
    return true;
}

// The substitution-only set and the lit set must not overlap. If a mode were ever classified as
// both, a capture would be reported as carrying a material/lighting verdict AND as possibly
// untinted -- two claims that cannot both be true of one frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewModeSubstitutionIsNeverLitTest,
    "PinWright.render.capture_view_mode.SubstitutionOnlyModesAreNeverLit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewModeSubstitutionIsNeverLitTest::RunTest(const FString& Parameters)
{
    for (int32 Index = 0; Index < static_cast<int32>(VMI_Max); ++Index)
    {
        const EViewModeIndex Mode = static_cast<EViewModeIndex>(Index);
        if (!PinWrightRenderCapture::IsDebugMaterialSubstitutionMode(Mode))
        {
            continue;
        }
        TestFalse(FString::Printf(
            TEXT("mode %d substitutes a debug material and so cannot be a lit frame"), Index),
            PinWrightRenderCapture::IsLitViewMode(Mode));
        // A frame in one of these modes must also report a viewModeWarning through the shared
        // reporting path, which is what tells a caller the frame is diagnostic at all.
        const PinWrightRenderCapture::FViewportCaptureOutput Capture = MakeCaptureIn(Mode);
        const TSharedPtr<FJsonObject> Viewport =
            PinWrightRenderCapture::MakeViewportInfoObject(Capture);
        TestTrue(FString::Printf(TEXT("mode %d reports lit:false"), Index),
            Viewport.IsValid() && !Viewport->GetBoolField(TEXT("lit")));
    }
    return true;
}
