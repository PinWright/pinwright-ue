// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the capture-time r.ViewDistanceScale override that keeps a wide orthographic frame
// from distance-culling its own content (board task #96).
//
// WHAT THESE ASSERT, AND WHY IT IS NOT "the verb returned success". The defect being fixed is a
// capture that succeeds while the pixels are missing the geometry the capture was taken to look
// at, so "returns success" is the one property that was never in question. The load-bearing
// property is a coverage inequality, per primitive that can actually be culled:
//
//     p.CullDistance * AppliedScale  >=  Dist(p, CullingOrigin) + p.SphereRadius
//
// That is checkable as a pure function without a level, a viewport or an RHI, so it is asserted
// directly and exactly rather than inferred from an image. The end-to-end tests then check that
// the handler routes a real capture through that function and puts the cvar back.
//
// TWO THINGS THIS FILE EXISTS TO STOP COMING BACK, both of which shipped once as "fixed":
//
//  1. CULLING ORIGIN != CAMERA. A lit orthographic editor view culls from a point up to
//     UE_OLD_WORLD_MAX (2097152 cm) BEHIND the camera -- EditorViewportClient.cpp:1420 sets
//     OrthoNearClipPlane = -UE_OLD_WORLD_MAX and SceneView.cpp:609 applies it to ViewOrigin,
//     which SceneVisibility.cpp:867 then culls against. A derivation measured from the camera
//     under-scales by that whole offset, reports overridden:true, and recovers nothing.
//     OrthoPushbackIsCovered pins this.
//
//  2. THE CAP MUST NOT BE LOAD-BEARING BY ACCIDENT. The first fix derived
//     MaxPrimitiveDistance / MinCullDistance, pairing the reach of one primitive with the cull
//     distance of an unrelated one. On the host map a single actor with a 7.6e12 cm bounding
//     sphere pinned that ratio to the cap, and the cap happened to be large enough -- so the
//     mechanism passed its own test while being wrong for any map with sane bounds.
//     UnculledGiantDoesNotSetTheScale pins the per-primitive form that removes the accident.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"
#include "Tests/TestUtils.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshCompiler.h"

namespace
{
    using PinWrightRenderCapture::FViewDistanceSurvey;

    // A one-culled-primitive scene: something that reaches MaxPrimitiveDistance from the culling
    // origin and culls itself at MinCullDistance. RequiredScale is the ratio SurveyViewDistances
    // would have accumulated for it.
    FViewDistanceSurvey MakeSurvey(double MinCullDistance, double MaxPrimitiveDistance)
    {
        FViewDistanceSurvey Survey;
        Survey.bValid = true;
        Survey.MinCullDistance = MinCullDistance;
        Survey.MaxPrimitiveDistance = MaxPrimitiveDistance;
        Survey.MinPrimitiveDistance = MaxPrimitiveDistance;
        Survey.NumPrimitives = 1;
        Survey.NumCulledPrimitives = MinCullDistance > 0.0 ? 1 : 0;
        Survey.RequiredScale = MinCullDistance > 0.0 ? MaxPrimitiveDistance / MinCullDistance : 0.0;
        Survey.bCullingOriginMeasured = true;
        return Survey;
    }

    // Every primitive with a finite cull distance is inside its own scaled cull radius.
    bool CoversEveryCulledPrimitive(const FViewDistanceSurvey& Survey, double Scale)
    {
        return Survey.RequiredScale <= 0.0 || Scale >= Survey.RequiredScale;
    }

    float ReadViewDistanceScaleCVar()
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ViewDistanceScale"));
        return CVar ? CVar->GetFloat() : -1.0f;
    }

    // Name is deliberately not the sibling file's DeleteCaptureFileIfPresent: two anonymous
    // namespaces in one Unity TU merge, so a duplicate helper name in the same Tests/ folder is
    // a latent redefinition error that adaptive unity hides until the files are committed
    // (docs/lessons.md).
    void DeleteViewDistanceCaptureFile(const FTestResponseCapture& Capture)
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("path"), Path))
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }
}

// ============================================================================
// The coverage inequality -- the actual property, asserted directly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceScaleCoversFarPrimitivesTest,
    "PinWright.render.view_distance_scale.AutoScaleCoversFarPrimitives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceScaleCoversFarPrimitivesTest::RunTest(const FString& Parameters)
{
    // The shape of the real failure: a 64000 cm map shot top-down from 60000 cm up puts the far
    // corners ~74000 cm from the camera, while the foliage that draws the forest paths culls at
    // 15000 cm. Un-scaled, everything past the middle of the frame disappears.
    const double MinCull = 15000.0;
    const double MaxDistance = 74000.0;
    const FViewDistanceSurvey Survey = MakeSurvey(MinCull, MaxDistance);

    TestFalse(TEXT("the un-scaled cull radius does NOT reach the far corners (the defect)"),
        MinCull * 1.0 >= MaxDistance);

    const float Scale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(Survey);
    TestTrue(TEXT("a scale above 1 is required"), Scale > 1.0f);
    TestTrue(TEXT("the scaled cull radius reaches every primitive the shot can see"),
        MinCull * static_cast<double>(Scale) >= MaxDistance);
    TestTrue(TEXT("the scale stays inside the cap derived for this cull distance"),
        Scale <= PinWrightRenderCapture::MaxAutoViewDistanceScaleFor(MinCull));
    return true;
}

// ============================================================================
// Regression: the orthographic culling-origin pushback.
//
// This is the case the previous fix got wrong and shipped anyway. The numbers are the tester's
// measured ones: 16 probe cubes culling at 20000 cm, 40000-50675 cm from the CAMERA in a
// top-down ortho at (0,0,60000) with orthoWidth 64000. Measured from the camera the requirement
// looks like ~2.5; measured from where the renderer actually culls -- 2097152 cm behind the
// camera -- it is ~105. A scale of 2.5 recovers nothing while reporting a clean override.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceOrthoPushbackIsCoveredTest,
    "PinWright.render.view_distance_scale.OrthoPushbackIsCovered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceOrthoPushbackIsCoveredTest::RunTest(const FString& Parameters)
{
    const double ProbeCull = 20000.0;
    const double ReachFromCamera = 50675.0;
    // UE_OLD_WORLD_MAX, the exact pushback a lit ortho editor view applies (EngineDefines.h:37,
    // EditorViewportClient.cpp:1420, SceneView.cpp:609).
    const double Pushback = 2097152.0;
    const double ReachFromCullingOrigin = Pushback + ReachFromCamera;

    const FViewDistanceSurvey CameraRelative = MakeSurvey(ProbeCull, ReachFromCamera);
    const FViewDistanceSurvey OriginRelative = MakeSurvey(ProbeCull, ReachFromCullingOrigin);

    const float CameraRelativeScale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(CameraRelative);
    const float OriginRelativeScale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(OriginRelative);

    // The failure being pinned: a camera-relative derivation is not merely imprecise here, it is
    // short by a factor of ~40 and recovers nothing.
    TestFalse(TEXT("a camera-relative scale does NOT cover the real culling distance"),
        ProbeCull * static_cast<double>(CameraRelativeScale) >= ReachFromCullingOrigin);

    TestTrue(TEXT("the origin-relative scale covers the real culling distance"),
        ProbeCull * static_cast<double>(OriginRelativeScale) >= ReachFromCullingOrigin);
    TestTrue(TEXT("the origin-relative scale is well clear of the camera-relative one"),
        OriginRelativeScale > CameraRelativeScale * 10.0f);
    // And it does not need the cap to get there -- the whole point of the fix.
    TestTrue(TEXT("covering the pushback does not require the cap to bind"),
        OriginRelativeScale < PinWrightRenderCapture::MaxAutoViewDistanceScaleFor(ProbeCull));
    return true;
}

// A short foliage cull distance plus the same pushback is the case the old 4096 cap silently
// failed: grass culling at 500 cm needs ~4400, which 4096 clips without covering.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceShortCullDistanceCoveredTest,
    "PinWright.render.view_distance_scale.ShortCullDistanceCovered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceShortCullDistanceCoveredTest::RunTest(const FString& Parameters)
{
    const double GrassCull = 500.0;
    const double Reach = 2097152.0 + 45000.0;
    const FViewDistanceSurvey Survey = MakeSurvey(GrassCull, Reach);
    const float Scale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(Survey);

    TestTrue(TEXT("short-cull foliage is covered from the ortho culling origin"),
        GrassCull * static_cast<double>(Scale) >= Reach);
    // The number the old cap was: it is not a limit any more, and this asserts that rather than
    // trusting the constant to have moved.
    TestTrue(TEXT("the required scale exceeds the retired 4096 cap"), Scale > 4096.0f);
    return true;
}

// ============================================================================
// Regression: the cap must not be reached by an irrelevant primitive.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceUnculledGiantDoesNotSetScaleTest,
    "PinWright.render.view_distance_scale.UnculledGiantDoesNotSetTheScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceUnculledGiantDoesNotSetScaleTest::RunTest(const FString& Parameters)
{
    // The host map's real shape: one primitive with a 7.6e12 cm bounding sphere and NO cull
    // distance, plus foliage at 20000 cm that reaches 2.14e6 cm from the culling origin. Only the
    // second one can be culled, so only it may set the requirement.
    FViewDistanceSurvey Survey = MakeSurvey(20000.0, 2097152.0 + 45000.0);
    Survey.NumPrimitives = 3204;
    Survey.NumInstancedComponents = 10;
    // The giant contributes to MaxPrimitiveDistance -- it is genuinely the farthest thing in the
    // world -- but not to RequiredScale, because nothing culls it.
    Survey.MaxPrimitiveDistance = 7.6e12;

    const float Scale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(Survey);

    TestTrue(TEXT("the derived scale is set by the culled foliage, not by the giant"),
        CoversEveryCulledPrimitive(Survey, static_cast<double>(Scale)));
    TestTrue(TEXT("the giant does not drag the scale to the cap"),
        Scale < PinWrightRenderCapture::MaxAutoViewDistanceScaleFor(Survey.MinCullDistance));
    // The old form -- MaxPrimitiveDistance / MinCullDistance -- would have asked for 3.8e8 here
    // and been clipped to whatever the cap was, which is exactly how the cap became load-bearing.
    TestTrue(TEXT("the retired global-ratio form would have hit any cap"),
        Survey.MaxPrimitiveDistance / Survey.MinCullDistance >
            static_cast<double>(PinWrightRenderCapture::MaxAutoViewDistanceScale));
    return true;
}

// ============================================================================
// The caps, by design rather than by luck.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceCapIsDerivedTest,
    "PinWright.render.view_distance_scale.CapIsDerivedFromEngineLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceCapIsDerivedTest::RunTest(const FString& Parameters)
{
    using PinWrightRenderCapture::MaxAutoViewDistanceScale;
    using PinWrightRenderCapture::MaxAutoViewDistanceScaleFor;
    using PinWrightRenderCapture::MaxScaledCullDistance;

    // The engine bound: HierarchicalInstancedStaticMesh.cpp:1675 truncates
    // `EndCullDistance * MaxDrawDistanceScale` into an int32, so the product is what is capped --
    // never a bare scale. For every cull distance, the cap keeps the product inside that bound.
    const double CullDistances[] = { 100.0, 500.0, 2000.0, 20000.0, 100000.0 };
    for (double Cull : CullDistances)
    {
        const double Cap = static_cast<double>(MaxAutoViewDistanceScaleFor(Cull));
        // 1.0001 absorbs the float round-trip in MaxAutoViewDistanceScaleFor's return type; the
        // real headroom being asserted is the factor of two below, not this epsilon.
        TestTrue(TEXT("the capped product stays inside the int32 truncation bound"),
            Cull * Cap <= MaxScaledCullDistance * 1.0001);
        TestTrue(TEXT("the cap never exceeds the absolute ceiling"),
            Cap <= static_cast<double>(MaxAutoViewDistanceScale));
    }

    // Below the crossover the absolute ceiling binds, above it the int32 bound does. Asserting
    // both directions is what makes the cap documented rather than incidental.
    TestEqual(TEXT("a sub-centimetre cull distance is limited by the absolute ceiling"),
        MaxAutoViewDistanceScaleFor(1.0), MaxAutoViewDistanceScale);
    TestTrue(TEXT("a large cull distance is limited by the int32 bound, below the ceiling"),
        MaxAutoViewDistanceScaleFor(100000.0) < MaxAutoViewDistanceScale);
    TestEqual(TEXT("no cull distance in the scene -> the absolute ceiling"),
        MaxAutoViewDistanceScaleFor(0.0), MaxAutoViewDistanceScale);

    // The int32 truncation this bound exists to avoid, stated as the arithmetic it is: the
    // product must stay representable, and MaxScaledCullDistance is half of INT32_MAX.
    TestTrue(TEXT("MaxScaledCullDistance leaves a factor-of-two margin under INT32_MAX"),
        MaxScaledCullDistance * 2.0 <= static_cast<double>(TNumericLimits<int32>::Max()) + 1.0);
    return true;
}

// A scale multiplies NEAR culling too (SceneVisibility.cpp:999/:1025). Recovering distant foliage
// by deleting near geometry is not a fix, so the derivation is bounded by it and says so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceRespectsNearCullTest,
    "PinWright.render.view_distance_scale.RespectsNearCullBound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceRespectsNearCullTest::RunTest(const FString& Parameters)
{
    FViewDistanceSurvey Survey = MakeSurvey(20000.0, 2097152.0 + 45000.0);
    // The scene needs ~112 to recover its foliage, but one primitive near-culls at 40000 cm and
    // its far edge is 2.14e6 cm from the culling origin, so anything above ~53 deletes it. The
    // near bound wins, and the capture reports the shortfall rather than trading one loss for
    // another silently.
    Survey.MaxMinDrawDistance = 40000.0;
    Survey.MinPrimitiveDistance = 2140000.0;

    const float Scale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(Survey);
    TestTrue(TEXT("the near bound actually binds in this scene"),
        static_cast<double>(Scale) < Survey.RequiredScale);
    TestTrue(TEXT("the scale does not push near culling past the closest primitive"),
        Survey.MaxMinDrawDistance * static_cast<double>(Scale) <= Survey.MinPrimitiveDistance * 1.0001);

    // Without a MinDrawDistance anywhere in the world the bound does not exist, and the same
    // scene derives the full scale it needs.
    FViewDistanceSurvey NoNearCull = Survey;
    NoNearCull.MaxMinDrawDistance = 0.0;
    TestTrue(TEXT("with no MinDrawDistance in the level the bound does not apply"),
        PinWrightRenderCapture::ComputeAutoViewDistanceScale(NoNearCull) > Scale);
    return true;
}

// A map whose cull distances already reach the frame corners must not have its rendering touched.
// Scaling it anyway would change what an already-correct capture shows for no reason, and it is
// the branch that keeps the override from becoming an unconditional global.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceScaleNoOpWhenAlreadyCoveredTest,
    "PinWright.render.view_distance_scale.NoOpWhenAlreadyCovered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceScaleNoOpWhenAlreadyCoveredTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("cull distance already exceeds the farthest primitive -> no override"),
        PinWrightRenderCapture::ComputeAutoViewDistanceScale(MakeSurvey(50000.0, 12000.0)), 1.0f);
    TestEqual(TEXT("cull distance exactly reaches the farthest primitive -> no override"),
        PinWrightRenderCapture::ComputeAutoViewDistanceScale(MakeSurvey(12000.0, 12000.0)), 1.0f);
    TestEqual(TEXT("nothing in the world is distance-culled -> no override"),
        PinWrightRenderCapture::ComputeAutoViewDistanceScale(MakeSurvey(0.0, 74000.0)), 1.0f);

    FViewDistanceSurvey Invalid;
    TestEqual(TEXT("an unsurveyed world -> no override"),
        PinWrightRenderCapture::ComputeAutoViewDistanceScale(Invalid), 1.0f);
    return true;
}

// The cap has to bind rather than run away, and when it binds the frame is still short of full
// coverage -- the capture reports that, so the test pins the inequality failing in that case too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureAutoViewDistanceScaleClampedTest,
    "PinWright.render.view_distance_scale.AutoScaleClamped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureAutoViewDistanceScaleClampedTest::RunTest(const FString& Parameters)
{
    // A 1 cm cull distance against a 1e10 cm reach asks for a scale of 1e10, well past the
    // absolute ceiling. Note the numbers this test used to carry -- 1 cm against 100000 cm --
    // no longer clamp at all: that requirement is 100000, which the retired 4096 cap clipped and
    // the derived cap covers outright. The case had to be made genuinely degenerate to still be
    // a clamp, which is itself the evidence the cap moved off the working range.
    const FViewDistanceSurvey Survey = MakeSurvey(1.0, 1.0e10);
    const float Scale = PinWrightRenderCapture::ComputeAutoViewDistanceScale(Survey);
    TestEqual(TEXT("the derived scale is clipped to the cap for this cull distance"),
        Scale, PinWrightRenderCapture::MaxAutoViewDistanceScaleFor(Survey.MinCullDistance));
    TestFalse(TEXT("a clipped scale does not claim full coverage"),
        CoversEveryCulledPrimitive(Survey, static_cast<double>(Scale)));
    return true;
}

// ============================================================================
// Parameter contract
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewDistanceScaleParamParsedTest,
    "PinWright.render.view_distance_scale.ParamParsed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewDistanceScaleParamParsedTest::RunTest(const FString& Parameters)
{
    PinWrightRenderCapture::FViewportCaptureRequest Request;
    FString ErrorCode;
    FString ErrorMessage;

    TSharedPtr<FJsonObject> Omitted = MakeShared<FJsonObject>();
    TestTrue(TEXT("payload without viewDistanceScale parses"),
        PinWrightRenderCapture::ParseViewportCaptureRequest(Omitted, Request, ErrorCode, ErrorMessage));
    TestFalse(TEXT("omitted viewDistanceScale is not marked provided"), Request.bViewDistanceScaleProvided);

    TSharedPtr<FJsonObject> Provided = MakeShared<FJsonObject>();
    Provided->SetNumberField(TEXT("viewDistanceScale"), 12.5);
    PinWrightRenderCapture::FViewportCaptureRequest ProvidedRequest;
    TestTrue(TEXT("explicit viewDistanceScale parses"),
        PinWrightRenderCapture::ParseViewportCaptureRequest(Provided, ProvidedRequest, ErrorCode, ErrorMessage));
    TestTrue(TEXT("explicit viewDistanceScale is marked provided"), ProvidedRequest.bViewDistanceScaleProvided);
    TestEqual(TEXT("explicit viewDistanceScale round-trips"), ProvidedRequest.ViewDistanceScale, 12.5f);

    // Zero is rejected instead of being read as "leave it alone": zero is also a legal
    // r.ViewDistanceScale meaning "cull everything", and the two readings are opposite.
    TSharedPtr<FJsonObject> Zero = MakeShared<FJsonObject>();
    Zero->SetNumberField(TEXT("viewDistanceScale"), 0.0);
    PinWrightRenderCapture::FViewportCaptureRequest ZeroRequest;
    TestFalse(TEXT("viewDistanceScale of zero is rejected"),
        PinWrightRenderCapture::ParseViewportCaptureRequest(Zero, ZeroRequest, ErrorCode, ErrorMessage));
    TestEqual(TEXT("zero viewDistanceScale is a typed INVALID_ARGUMENT"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    TSharedPtr<FJsonObject> Negative = MakeShared<FJsonObject>();
    Negative->SetNumberField(TEXT("viewDistanceScale"), -3.0);
    PinWrightRenderCapture::FViewportCaptureRequest NegativeRequest;
    TestFalse(TEXT("negative viewDistanceScale is rejected"),
        PinWrightRenderCapture::ParseViewportCaptureRequest(Negative, NegativeRequest, ErrorCode, ErrorMessage));
    return true;
}

// ============================================================================
// End to end: the handler applies it, reports it, and puts the cvar back.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelReportsViewDistanceTest,
    "PinWright.render.capture_open_level.ReportsViewDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelReportsViewDistanceTest::RunTest(const FString& Parameters)
{
    const float ScaleBeforeCall = ReadViewDistanceScaleCVar();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Payload->SetNumberField(TEXT("orthoWidth"), 64000);
    Payload->SetBoolField(TEXT("allowBlank"), true);
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0);
    Location->SetNumberField(TEXT("y"), 0);
    Location->SetNumberField(TEXT("z"), 60000);
    Payload->SetObjectField(TEXT("location"), Location);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -90);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(Capture);
    };

    // The decisive assertion, and it holds on a headless host with no viewport too: whatever the
    // verb did, it must not have left a global rendering cvar moved. A capture that changes
    // r.ViewDistanceScale as a side effect makes every later capture in the session incomparable.
    TestEqual(TEXT("r.ViewDistanceScale is back where the call found it"),
        ReadViewDistanceScaleCVar(), ScaleBeforeCall);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* ViewDistance = nullptr;
        TestTrue(TEXT("a successful capture reports its viewDistance block"),
            Capture.Result->TryGetObjectField(TEXT("viewDistance"), ViewDistance) && ViewDistance != nullptr);
        if (ViewDistance && ViewDistance->IsValid())
        {
            bool bRestored = false;
            TestTrue(TEXT("viewDistance carries a restored flag"),
                (*ViewDistance)->TryGetBoolField(TEXT("restored"), bRestored));
            TestTrue(TEXT("the capture reports the cvar as restored"), bRestored);

            FString Source;
            TestTrue(TEXT("viewDistance names how the scale was chosen"),
                (*ViewDistance)->TryGetStringField(TEXT("source"), Source));
            const bool bKnownSource =
                Source == TEXT("auto") || Source == TEXT("caller") || Source == TEXT("none");
            TestTrue(TEXT("viewDistance source is one of auto/caller/none"), bKnownSource);

            // The survey has to say where it measured from, and that field is the difference
            // between a scale that recovers foliage and one that only reports doing so.
            const TSharedPtr<FJsonObject>* Survey = nullptr;
            if ((*ViewDistance)->TryGetObjectField(TEXT("survey"), Survey) && Survey && Survey->IsValid())
            {
                const TSharedPtr<FJsonObject>* Origin = nullptr;
                TestTrue(TEXT("the survey reports the culling origin it measured from"),
                    (*Survey)->TryGetObjectField(TEXT("cullingOrigin"), Origin) && Origin != nullptr);
                double Pushback = -1.0;
                TestTrue(TEXT("the survey reports the culling-origin pushback"),
                    (*Survey)->TryGetNumberField(TEXT("cullingOriginPushback"), Pushback));
                TestTrue(TEXT("the pushback is a non-negative distance"), Pushback >= 0.0);
            }

            // When the auto path did fire, the coverage inequality has to hold in the live scene
            // too, and it is checked against what the RENDERER read rather than what was written.
            double EffectiveScale = 0.0;
            if (Source == TEXT("auto") &&
                (*ViewDistance)->TryGetNumberField(TEXT("scaleEffective"), EffectiveScale) &&
                Survey && Survey->IsValid())
            {
                double RequiredScale = 0.0;
                double MinCull = 0.0;
                (*Survey)->TryGetNumberField(TEXT("requiredScale"), RequiredScale);
                (*Survey)->TryGetNumberField(TEXT("minCullDistance"), MinCull);

                // The clamp verdict is READ, not inferred from the numbers, and the branch it
                // selects is asserted on BOTH sides. The previous shape stopped asserting
                // entirely once the cap bound -- which is the one case where the capture returns
                // a clean-looking override over a frame that is still missing content, so it was
                // the case that most needed an assertion and the only one that had none.
                bool bClamped = true;
                TestTrue(TEXT("an auto capture publishes a machine-readable clamped flag"),
                    (*ViewDistance)->TryGetBoolField(TEXT("clamped"), bClamped));

                if (RequiredScale > 0.0 && MinCull > 0.0 && !bClamped)
                {
                    TestTrue(TEXT("the scale the renderer read covers every culled primitive"),
                        EffectiveScale >= RequiredScale);
                    bool bSufficient = false;
                    (*ViewDistance)->TryGetBoolField(TEXT("coverageSufficient"), bSufficient);
                    TestTrue(TEXT("an unclamped auto capture reports sufficient coverage"), bSufficient);
                }
                else if (bClamped)
                {
                    FString ClampReason;
                    TestTrue(TEXT("a clamped capture names which cap bound"),
                        (*ViewDistance)->TryGetStringField(TEXT("clampReason"), ClampReason));
                    const bool bKnownReason = ClampReason == TEXT("nearCull") ||
                        ClampReason == TEXT("int32") || ClampReason == TEXT("absolute");
                    TestTrue(TEXT("clampReason is one of nearCull/int32/absolute"), bKnownReason);

                    FString ClampWarning;
                    TestTrue(TEXT("a clamped capture carries the prose warning beside the flag"),
                        (*ViewDistance)->TryGetStringField(TEXT("viewDistanceWarning"), ClampWarning));
                    TestFalse(TEXT("the clamp warning is not empty"), ClampWarning.IsEmpty());

                    bool bSufficient = true;
                    (*ViewDistance)->TryGetBoolField(TEXT("coverageSufficient"), bSufficient);
                    TestFalse(TEXT("a clamped capture does NOT claim sufficient coverage"), bSufficient);
                    TestTrue(TEXT("a clamped capture applied strictly less than it needed"),
                        EffectiveScale < RequiredScale);
                }
            }
        }
    }
    return true;
}

// An explicit viewDistanceScale must win over the derived one, and must still be restored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelExplicitViewDistanceScaleTest,
    "PinWright.render.capture_open_level.ExplicitViewDistanceScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelExplicitViewDistanceScaleTest::RunTest(const FString& Parameters)
{
    const float ScaleBeforeCall = ReadViewDistanceScaleCVar();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 128);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Payload->SetNumberField(TEXT("orthoWidth"), 8000);
    Payload->SetNumberField(TEXT("viewDistanceScale"), 7.0);
    Payload->SetBoolField(TEXT("allowBlank"), true);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -90);
    Rotation->SetNumberField(TEXT("yaw"), 0);
    Rotation->SetNumberField(TEXT("roll"), 0);
    Payload->SetObjectField(TEXT("rotation"), Rotation);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(Capture);
    };

    TestEqual(TEXT("r.ViewDistanceScale is back where the call found it"),
        ReadViewDistanceScaleCVar(), ScaleBeforeCall);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* ViewDistance = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("viewDistance"), ViewDistance) && ViewDistance && ViewDistance->IsValid())
        {
            FString Source;
            (*ViewDistance)->TryGetStringField(TEXT("source"), Source);
            // "none" is the legitimate outcome when the project already sits at 7.0 and there was
            // nothing to change; anything else means the caller's value was ignored.
            const bool bCallerHonoured = Source == TEXT("caller") || Source == TEXT("none");
            TestTrue(TEXT("an explicit viewDistanceScale is not overridden by the derived one"), bCallerHonoured);
            if (Source == TEXT("caller"))
            {
                double Applied = 0.0;
                (*ViewDistance)->TryGetNumberField(TEXT("scaleApplied"), Applied);
                TestTrue(TEXT("the caller's scale is what was applied"), FMath::IsNearlyEqual(Applied, 7.0, 0.001));
            }
        }
    }
    return true;
}

// A perspective capture must be left exactly as it was: the auto path is orthographic-only on
// purpose, because turning it on for perspective would move every measurement already taken with
// this verb. This test is the guard on that scoping decision.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelPerspectiveLeavesViewDistanceAloneTest,
    "PinWright.render.capture_open_level.PerspectiveLeavesViewDistanceAlone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelPerspectiveLeavesViewDistanceAloneTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 128);
    Payload->SetNumberField(TEXT("height"), 128);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(Capture);
    };

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* ViewDistance = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("viewDistance"), ViewDistance) && ViewDistance && ViewDistance->IsValid())
        {
            bool bOverridden = true;
            (*ViewDistance)->TryGetBoolField(TEXT("overridden"), bOverridden);
            TestFalse(TEXT("a perspective capture with no explicit scale does not override the cvar"), bOverridden);
            FString Source;
            (*ViewDistance)->TryGetStringField(TEXT("source"), Source);
            TestEqual(TEXT("a perspective capture reports source 'none'"), Source, FString(TEXT("none")));
        }
    }
    return true;
}

// ============================================================================
// REAL COMPONENTS, NOT A HAND-BUILT STRUCT.
//
// Everything above this line asserts what ComputeAutoViewDistanceScale does with a survey somebody
// typed in. That is the right shape for the arithmetic and the wrong shape for the ticket's
// headline case: the survey those tests feed was never produced by an engine component, so the
// instanced path -- the one that empties a forest out of a wide top-down -- had no coverage at all.
// The tests below build the components the engine actually culls and read the numbers back off
// them, and the last one reads PIXELS.
// ============================================================================

namespace
{
    // Prefixed, deliberately. This file merges with its Tests/ siblings under adaptive Unity, so a
    // bare SpawnScatter / ReadPixels here is a latent redefinition that only appears once both
    // files land in the same TU (docs/lessons.md).
    UWorld* ViewDistanceTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    IConsoleVariable* ViewDistanceTestFoliageCeilingCVar()
    {
        return IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.MaxEndCullDistance"));
    }

    // Writes at the priority the variable already carries, for the same reason
    // FScopedViewDistanceScale does: ECVF_SetByCode would leave it pinned above scalability for
    // the rest of the session, and a test has no business doing that to a shared editor.
    void ViewDistanceTestSetFoliageCeiling(IConsoleVariable* CVar, int32 Value)
    {
        if (CVar)
        {
            CVar->Set(Value, static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask));
        }
    }

    // An instanced holder carrying SideCount x SideCount cubes on a square grid centred on the
    // world origin at z = 0. Instanced on purpose: HierarchicalInstancedStaticMesh.cpp culls its
    // INSTANCES against its own end-cull distance and its own temporal-LOD origin, and that path
    // is what the ticket is about. Returns nullptr when there is no world or no engine cube.
    //
    // ComponentClass is a parameter because foliage.MaxEndCullDistance keys on the PROXY: only
    // UHierarchicalInstancedStaticMeshComponent builds the FHierarchicalStaticMeshSceneProxy that
    // reads the cvar, so a plain UInstancedStaticMeshComponent is the control case.
    UInstancedStaticMeshComponent* ViewDistanceTestSpawnScatterOfClass(
        UWorld* World, TSubclassOf<UInstancedStaticMeshComponent> ComponentClass,
        const TCHAR* ComponentName, int32 SideCount, double HalfExtentCm, double InstanceScale)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !Cube || !ComponentClass || SideCount < 1)
        {
            return nullptr;
        }
        AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
            FRotator::ZeroRotator);
        if (!Holder)
        {
            return nullptr;
        }
        UInstancedStaticMeshComponent* Instanced = NewObject<UInstancedStaticMeshComponent>(Holder,
            ComponentClass, ComponentName, RF_Transactional);
        Holder->SetRootComponent(Instanced);
        Holder->AddInstanceComponent(Instanced);
        Instanced->OnComponentCreated();
        // AddInstance only records an instance for rendering when the mesh is not still
        // async-compiling (HierarchicalInstancedStaticMesh.cpp: the branch is gated on
        // `!GetStaticMesh()->IsCompiling()`), and it queues the cluster tree ASYNC. A capture taken
        // in the same frame then measures a scatter the renderer was never handed.
        if (Cube->IsCompiling())
        {
            FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
        }
        Instanced->SetStaticMesh(Cube);
        Instanced->RegisterComponent();

        const double Step = (SideCount > 1) ? (2.0 * HalfExtentCm / (SideCount - 1)) : 0.0;
        for (int32 Row = 0; Row < SideCount; ++Row)
        {
            for (int32 Column = 0; Column < SideCount; ++Column)
            {
                const FVector Offset(
                    (SideCount > 1) ? (-HalfExtentCm + Row * Step) : 0.0,
                    (SideCount > 1) ? (-HalfExtentCm + Column * Step) : 0.0,
                    0.0);
                Instanced->AddInstance(FTransform(FQuat::Identity, Offset, FVector(InstanceScale)));
            }
        }
        if (UHierarchicalInstancedStaticMeshComponent* Hierarchical =
                Cast<UHierarchicalInstancedStaticMeshComponent>(Instanced))
        {
            // Synchronous and forced: the async build AddInstance queued has not landed by the
            // time the capture below renders.
            Hierarchical->BuildTreeIfOutdated(/*Async*/ false, /*ForceUpdate*/ true);
        }
        Instanced->MarkRenderStateDirty();
        Holder->SetActorLabel(TEXT("PWViewDistanceScatterProbe"));
        return Instanced;
    }

    UHierarchicalInstancedStaticMeshComponent* ViewDistanceTestSpawnScatter(
        UWorld* World, int32 SideCount, double HalfExtentCm, double InstanceScale)
    {
        return Cast<UHierarchicalInstancedStaticMeshComponent>(ViewDistanceTestSpawnScatterOfClass(
            World, UHierarchicalInstancedStaticMeshComponent::StaticClass(),
            TEXT("HISM_PWViewDistanceProbe"), SideCount, HalfExtentCm, InstanceScale));
    }

    // A plain static-mesh probe that NEAR-culls. The near term is the half of the derivation that
    // no test had ever fed from a real component, and it is the one that can make a fix worse than
    // the defect: r.ViewDistanceScale multiplies MinDrawDistance too.
    AStaticMeshActor* ViewDistanceTestSpawnNearCullProbe(UWorld* World, float MinDrawDistanceCm)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !Cube)
        {
            return nullptr;
        }
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor || !Actor->GetStaticMeshComponent())
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Actor->GetStaticMeshComponent()->MinDrawDistance = MinDrawDistanceCm;
        Actor->GetStaticMeshComponent()->MarkRenderStateDirty();
        Actor->SetActorLabel(TEXT("PWViewDistanceNearCullProbe"));
        return Actor;
    }

    bool ViewDistanceTestReadCapturePixels(const FTestResponseCapture& Capture,
        TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
    {
        OutPixels.Reset();
        OutWidth = 0;
        OutHeight = 0;
        FString Path;
        if (!Capture.bSuccess || !Capture.Result.IsValid() ||
            !Capture.Result->TryGetStringField(TEXT("path"), Path))
        {
            return false;
        }
        TArray<uint8> Encoded;
        if (!FFileHelper::LoadFileToArray(Encoded, *Path))
        {
            return false;
        }
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
        TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        TArray64<uint8> Raw;
        if (!PngWrapper.IsValid() || !PngWrapper->SetCompressed(Encoded.GetData(), Encoded.Num()) ||
            !PngWrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
        {
            return false;
        }
        OutWidth = PngWrapper->GetWidth();
        OutHeight = PngWrapper->GetHeight();
        const int64 PixelCount = static_cast<int64>(OutWidth) * static_cast<int64>(OutHeight);
        if (PixelCount <= 0 || Raw.Num() < PixelCount * 4)
        {
            return false;
        }
        OutPixels.SetNumUninitialized(static_cast<int32>(PixelCount));
        for (int64 Index = 0; Index < PixelCount; ++Index)
        {
            const int64 Offset = Index * 4;
            // BGRA byte order out of GetRaw.
            OutPixels[static_cast<int32>(Index)] = FColor(
                Raw[Offset + 2], Raw[Offset + 1], Raw[Offset + 0], Raw[Offset + 3]);
        }
        return true;
    }

    // Tolerance, not equality. The frames are 8-bit and a lit editor frame's temporal passes move
    // the low bits between two draws of the same scene, so a bare != counts dither as content.
    int32 ViewDistanceTestCountDifferingPixels(const TArray<FColor>& Left, const TArray<FColor>& Right)
    {
        const int32 Tolerance = 8;
        const int32 Count = FMath::Min(Left.Num(), Right.Num());
        int32 Differing = 0;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const int32 DeltaR = FMath::Abs(static_cast<int32>(Left[Index].R) - static_cast<int32>(Right[Index].R));
            const int32 DeltaG = FMath::Abs(static_cast<int32>(Left[Index].G) - static_cast<int32>(Right[Index].G));
            const int32 DeltaB = FMath::Abs(static_cast<int32>(Left[Index].B) - static_cast<int32>(Right[Index].B));
            if (FMath::Max3(DeltaR, DeltaG, DeltaB) > Tolerance)
            {
                ++Differing;
            }
        }
        return Differing;
    }

    // The top-down orthographic framing every test below shares, so the four frames of the pixel
    // test are the same shot with one variable changed.
    TSharedPtr<FJsonObject> ViewDistanceTestMakeTopDownPayload(int32 Edge)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), Edge);
        Payload->SetNumberField(TEXT("height"), Edge);
        Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
        Payload->SetNumberField(TEXT("orthoWidth"), 64000);
        Payload->SetBoolField(TEXT("allowBlank"), true);
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 0);
        Location->SetNumberField(TEXT("y"), 0);
        Location->SetNumberField(TEXT("z"), 60000);
        Payload->SetObjectField(TEXT("location"), Location);
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), -90);
        Rotation->SetNumberField(TEXT("yaw"), 0);
        Rotation->SetNumberField(TEXT("roll"), 0);
        Payload->SetObjectField(TEXT("rotation"), Rotation);
        return Payload;
    }
}

// The instanced end-cull distance is a SEPARATE number from the primitive's MaxDrawDistance, and a
// survey that reads only the latter sees a forest as un-cullable and derives no scale for it. This
// pins the survey against a real UHierarchicalInstancedStaticMeshComponent rather than a struct
// literal: the same component is surveyed twice, and the only thing that changes between the two
// readings is SetCullDistances.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewDistanceSurveyReadsInstancedEndCullTest,
    "PinWright.render.view_distance_scale.SurveyReadsInstancedEndCullDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewDistanceSurveyReadsInstancedEndCullTest::RunTest(const FString& Parameters)
{
    UWorld* World = ViewDistanceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() was null, so no component could be surveyed"));
        return true;
    }

    // Pinned to the engine default for the duration: a non-zero foliage.MaxEndCullDistance gives
    // EVERY instanced component a finite cull distance, which would make the two readings below
    // identical for a reason that has nothing to do with what this test measures.
    IConsoleVariable* CeilingCVar = ViewDistanceTestFoliageCeilingCVar();
    const int32 CeilingBefore = CeilingCVar ? CeilingCVar->GetInt() : 0;
    ViewDistanceTestSetFoliageCeiling(CeilingCVar, 0);
    ON_SCOPE_EXIT
    {
        ViewDistanceTestSetFoliageCeiling(CeilingCVar, CeilingBefore);
    };

    FScopedEditorWorldActorGuard Guard;

    // Far enough out that the scatter is well past any cull distance it is given -- the shape of a
    // lit orthographic view, whose culling origin sits ~2.1e6 cm behind the camera.
    const FVector CullingOrigin(0.0, 0.0, 2200000.0);

    UHierarchicalInstancedStaticMeshComponent* Hism =
        ViewDistanceTestSpawnScatter(World, /*SideCount=*/3, /*HalfExtentCm=*/20000.0, /*InstanceScale=*/60.0);
    if (!Hism)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("could not build the instanced scatter probe (/Engine/BasicShapes/Cube or SpawnActor failed)"));
        return true;
    }

    const FViewDistanceSurvey WithoutEndCull =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    TestTrue(TEXT("the survey ran over the editor world"), WithoutEndCull.bValid);
    TestTrue(TEXT("the instanced probe is counted as an instanced component"),
        WithoutEndCull.NumInstancedComponents >= 1);

    Hism->SetCullDistances(0, 20000);

    const FViewDistanceSurvey WithEndCull =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    // The whole point: giving a HISM an end-cull distance and NOTHING else moves it into the
    // at-risk population. A survey that read only CachedMaxDrawDistance would report the same
    // number twice and derive no scale for a forest that is being culled.
    TestEqual(TEXT("an instanced end-cull distance adds exactly one primitive to the at-risk set"),
        WithEndCull.NumCulledPrimitives, WithoutEndCull.NumCulledPrimitives + 1);
    TestTrue(TEXT("the smallest finite cull distance is now no larger than the probe's"),
        WithEndCull.MinCullDistance > 0.0 && WithEndCull.MinCullDistance <= 20000.0);
    // Measured from the CULLING ORIGIN, so the requirement is ~110 here rather than the ~1 a
    // camera-relative measurement would produce for a probe sitting under the camera.
    TestTrue(TEXT("the requirement is measured from the culling origin, not from the probe's z"),
        WithEndCull.RequiredScale >= 2200000.0 / 20000.0);

    // The derived scale either covers that requirement or is clipped by a cap -- and this file's
    // AutoScaleClamped test owns the clipped branch, so there is no third outcome.
    const double DerivedScale =
        static_cast<double>(PinWrightRenderCapture::ComputeAutoViewDistanceScale(WithEndCull));
    const double CapForScene = static_cast<double>(
        PinWrightRenderCapture::MaxAutoViewDistanceScaleFor(WithEndCull.MinCullDistance));
    TestTrue(TEXT("the derived scale covers the probe, or is at the cap and says so"),
        DerivedScale >= WithEndCull.RequiredScale ||
        DerivedScale >= CapForScene - UE_KINDA_SMALL_NUMBER);
    return true;
}

// The NEAR term, read off a real component. r.ViewDistanceScale multiplies MinDrawDistance as well
// as MaxDrawDistance (SceneVisibility.cpp:999), so a scale derived without it recovers distant
// foliage by deleting near geometry -- a fix that trades one missing-content defect for another.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewDistanceSurveyReadsMinDrawDistanceTest,
    "PinWright.render.view_distance_scale.SurveyReadsMinDrawDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewDistanceSurveyReadsMinDrawDistanceTest::RunTest(const FString& Parameters)
{
    UWorld* World = ViewDistanceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() was null, so no component could be surveyed"));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FVector CullingOrigin(0.0, 0.0, 2200000.0);

    const FViewDistanceSurvey Before =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    const float ProbeMinDraw = 40000.0f;
    AStaticMeshActor* NearProbe = ViewDistanceTestSpawnNearCullProbe(World, ProbeMinDraw);
    if (!NearProbe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("could not build the near-cull probe (/Engine/BasicShapes/Cube or SpawnActor failed)"));
        return true;
    }

    const FViewDistanceSurvey After =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    TestTrue(TEXT("the survey sees a non-zero MinDrawDistance on a real component"),
        After.MaxMinDrawDistance >= static_cast<double>(ProbeMinDraw));
    TestTrue(TEXT("it also records how close the nearest primitive comes to the culling origin"),
        After.MinPrimitiveDistance > 0.0);
    TestTrue(TEXT("adding a near-culling probe cannot lower the recorded near-cull distance"),
        After.MaxMinDrawDistance >= Before.MaxMinDrawDistance);

    // The bound the derivation owes this scene: either it left the cvar alone, or the scale it
    // chose keeps the closest primitive outside the SCALED near-cull radius.
    const double Scale = static_cast<double>(PinWrightRenderCapture::ComputeAutoViewDistanceScale(After));
    TestTrue(TEXT("the derived scale does not push near culling past the closest primitive"),
        Scale <= 1.0 || After.MaxMinDrawDistance * Scale <= After.MinPrimitiveDistance * 1.0001);
    return true;
}

// foliage.MaxEndCullDistance is the one term in the instanced path that no r.ViewDistanceScale can
// lift, because the engine applies it AFTER the multiply
// (HierarchicalInstancedStaticMesh.cpp:1675-1686). What this test pins is the exclusion that
// follows: a component past the ceiling must NOT drive the derived scale (it would peg a cap it
// cannot cash in, which is exactly how the old cap became load-bearing), and a component the
// ceiling already covers must not raise the requirement either -- there is nothing to recover.
// It also pins the boundary the rest of the file cannot see: at the engine default of 0 the
// ceiling is DISABLED, so nothing is ceiling-limited and the survey reads as it did before the
// ceiling was modelled at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureViewDistanceFoliageCeilingIsNotScalableTest,
    "PinWright.render.view_distance_scale.FoliageCeilingIsNotScalable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureViewDistanceFoliageCeilingIsNotScalableTest::RunTest(const FString& Parameters)
{
    UWorld* World = ViewDistanceTestEditorWorld();
    IConsoleVariable* CeilingCVar = ViewDistanceTestFoliageCeilingCVar();
    if (!World || !CeilingCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world-or-foliage-cvar"),
            FString::Printf(TEXT("world=%s foliage.MaxEndCullDistance=%s"),
                World ? TEXT("present") : TEXT("null"),
                CeilingCVar ? TEXT("present") : TEXT("absent")));
        return true;
    }

    const int32 CeilingBefore = CeilingCVar->GetInt();
    ON_SCOPE_EXIT
    {
        ViewDistanceTestSetFoliageCeiling(CeilingCVar, CeilingBefore);
    };

    FScopedEditorWorldActorGuard Guard;
    const FVector CullingOrigin(0.0, 0.0, 2200000.0);
    // Below the probe's ~2.2e6 cm reach, so the probe is unreachable at any scale.
    const int32 LowCeiling = 100000;
    // Above it, so the ceiling stops binding and the probe behaves like ordinary foliage.
    const int32 HighCeiling = 3000000;

    ViewDistanceTestSetFoliageCeiling(CeilingCVar, LowCeiling);
    const FViewDistanceSurvey LowBaseline =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    ViewDistanceTestSetFoliageCeiling(CeilingCVar, HighCeiling);
    const FViewDistanceSurvey HighBaseline =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    // The engine DEFAULT, and the state this suite runs in, so it gets a baseline of its own.
    ViewDistanceTestSetFoliageCeiling(CeilingCVar, 0);
    const FViewDistanceSurvey ZeroBaseline =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    // NO end cull distance of its own -- the case where the ceiling, and only the ceiling, decides
    // whether the component draws, and where no scale changes that either way.
    UHierarchicalInstancedStaticMeshComponent* Hism =
        ViewDistanceTestSpawnScatter(World, /*SideCount=*/3, /*HalfExtentCm=*/20000.0, /*InstanceScale=*/60.0);
    if (!Hism)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("could not build the instanced scatter probe (/Engine/BasicShapes/Cube or SpawnActor failed)"));
        return true;
    }

    ViewDistanceTestSetFoliageCeiling(CeilingCVar, LowCeiling);
    const FViewDistanceSurvey LowWithProbe =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    TestEqual(TEXT("a ceiling below the probe's reach marks exactly one more component unreachable"),
        LowWithProbe.NumFoliageCeilingLimited, LowBaseline.NumFoliageCeilingLimited + 1);
    TestEqual(TEXT("the survey reports the ceiling it measured against"),
        LowWithProbe.FoliageMaxEndCullDistance, static_cast<double>(LowCeiling));
    // The load-bearing half: an unreachable component must not ask for a scale. Asking would peg
    // the cap and report a clean override over a frame the scale cannot fix.
    TestTrue(TEXT("an unreachable component does not raise the required scale"),
        LowWithProbe.RequiredScale <= LowBaseline.RequiredScale + UE_KINDA_SMALL_NUMBER);

    ViewDistanceTestSetFoliageCeiling(CeilingCVar, HighCeiling);
    const FViewDistanceSurvey HighWithProbe =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);

    TestEqual(TEXT("a ceiling above the probe's reach marks nothing extra unreachable"),
        HighWithProbe.NumFoliageCeilingLimited, HighBaseline.NumFoliageCeilingLimited);
    // Reach 2.2e6 against a 3.0e6 ceiling: covered without any scale, so still no requirement.
    // Note what this forbids -- reporting reach/ceiling as the requirement. The ceiling is not a
    // distance the scale trades against: this probe has NO end cull distance of its own, and
    // `EndCullDistance * MaxDrawDistanceScale` is 0 at every scale, so the ceiling becomes its
    // cull distance outright and nothing about it is derivable.
    TestTrue(TEXT("a component the ceiling already covers does not raise the required scale"),
        HighWithProbe.RequiredScale <= HighBaseline.RequiredScale + UE_KINDA_SMALL_NUMBER);

    // 0 IS THE DEFAULT AND IT MEANS "NO CEILING", not "a ceiling of zero" -- the engine's entire
    // clamp sits inside `if (MaxEndCullDistance > 0)` (HierarchicalInstancedStaticMesh.cpp:1676).
    // Read the other way round, every instanced component reaches past a zero ceiling, the
    // exclusion swallows the whole instanced population, and the derivation silently stops
    // covering the forests it exists for -- while every assertion above, all taken at non-zero
    // ceilings, still passes. This is the state the suite itself runs in, so it is asserted here.
    ViewDistanceTestSetFoliageCeiling(CeilingCVar, 0);
    const FViewDistanceSurvey ZeroWithProbe =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    TestEqual(TEXT("a disabled ceiling is reported as 0"),
        ZeroWithProbe.FoliageMaxEndCullDistance, 0.0);
    TestEqual(TEXT("nothing is ceiling-limited while the ceiling is disabled"),
        ZeroWithProbe.NumFoliageCeilingLimited, 0);
    TestTrue(TEXT("the instanced probe is still surveyed at the default ceiling"),
        ZeroWithProbe.NumInstancedComponents >= ZeroBaseline.NumInstancedComponents + 1);
    // With no ceiling and no end cull distance of its own the probe is genuinely un-cullable, so
    // it joins neither the at-risk population nor the requirement.
    TestEqual(TEXT("a probe with no end cull distance adds no at-risk primitive at the default"),
        ZeroWithProbe.NumCulledPrimitives, ZeroBaseline.NumCulledPrimitives);
    TestTrue(TEXT("and it asks for no scale at the default"),
        ZeroWithProbe.RequiredScale <= ZeroBaseline.RequiredScale + UE_KINDA_SMALL_NUMBER);

    ViewDistanceTestSetFoliageCeiling(CeilingCVar, HighCeiling);

    // Now give it an end cull distance well inside the high ceiling: the scale CAN reach it, so
    // now -- and only now -- the requirement must move.
    Hism->SetCullDistances(0, 20000);
    const FViewDistanceSurvey ScalableWithProbe =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    // Stated as a floor rather than "greater than the baseline", so the assertion holds on a host
    // map that already needs a large scale of its own: RequiredScale is a MAX, and the probe alone
    // guarantees at least this much of it.
    TestTrue(TEXT("a component the scale CAN reach raises the requirement to at least its own ratio"),
        ScalableWithProbe.RequiredScale >= 2200000.0 / 20000.0);
    TestTrue(TEXT("and it never lowers what the rest of the level already required"),
        ScalableWithProbe.RequiredScale >= HighBaseline.RequiredScale - UE_KINDA_SMALL_NUMBER);

    // Same component, same end cull distance, ceiling back below its reach: unreachable again, so
    // it must contribute NOTHING rather than ask for a scale the engine will clamp away. This is
    // the assertion that separates a modelled ceiling from an unmodelled one -- without the model
    // the requirement stays at the ~110 above and the capture reports a clean override over a
    // frame whose instances are still culled.
    ViewDistanceTestSetFoliageCeiling(CeilingCVar, LowCeiling);
    const FViewDistanceSurvey ClampedWithProbe =
        PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
    TestEqual(TEXT("the ceiling still marks it unreachable once it has an end cull distance"),
        ClampedWithProbe.NumFoliageCeilingLimited, LowBaseline.NumFoliageCeilingLimited + 1);
    TestTrue(TEXT("and it stops asking for a scale that cannot draw it"),
        ClampedWithProbe.RequiredScale <= LowBaseline.RequiredScale + UE_KINDA_SMALL_NUMBER);

    // THE CEILING IS HISM-ONLY, WHICH IS NARROWER THAN "INSTANCED". Both reads of the cvar sit in
    // FHierarchicalStaticMeshSceneProxy (HierarchicalInstancedStaticMesh.cpp:1658, :1870), and only
    // UHierarchicalInstancedStaticMeshComponent builds that proxy (:3004); a plain
    // UInstancedStaticMeshComponent gets FInstancedStaticMeshSceneProxy
    // (InstancedStaticMesh.cpp:2600) and is never clamped. So this control probe -- same geometry,
    // same reach, same end cull distance, one class up -- must behave as if no ceiling existed,
    // even though the ceiling that just excluded its HISM twin is still set. Applying the ceiling
    // to it would exclude from RequiredScale a component the scale genuinely recovers, and warn
    // about a limit it does not have.
    UInstancedStaticMeshComponent* Ism = ViewDistanceTestSpawnScatterOfClass(
        World, UInstancedStaticMeshComponent::StaticClass(), TEXT("ISM_PWViewDistanceProbe"),
        /*SideCount=*/3, /*HalfExtentCm=*/20000.0, /*InstanceScale=*/60.0);
    if (Ism)
    {
        Ism->SetCullDistances(0, 20000);
        const FViewDistanceSurvey IsmWithProbe =
            PinWrightRenderCapture::SurveyViewDistances(World, CullingOrigin);
        TestEqual(TEXT("a plain instanced component is never ceiling-limited"),
            IsmWithProbe.NumFoliageCeilingLimited, ClampedWithProbe.NumFoliageCeilingLimited);
        TestTrue(TEXT("and it still raises the requirement under a ceiling that excludes a HISM"),
            IsmWithProbe.RequiredScale >= 2200000.0 / 20000.0);
    }
    return true;
}

// ============================================================================
// PIXELS. Not an inequality -- the frames.
//
// Every other assertion in this file is a model of what the renderer will do. This one renders
// four frames and counts what changed, because the defect being fixed is a capture that satisfies
// every model and comes back with an empty forest. The measurement is differential and taken
// WITHIN a scale, never across one:
//
//     scatter's pixels at scale 1        = diff(with scatter, without scatter) at scale 1
//     scatter's pixels at derived scale  = diff(with scatter, without scatter) at the derived scale
//
// Comparing the two frames at ONE scale is what makes the number attributable. A naive
// before/after across scales also moves the exposure, the temporal passes and every other
// distance-culled thing in the level, which is why the previous recovery evidence had to be
// hand-corrected for a 2.4-2.65% noise floor and why it proved nothing about instanced meshes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureOpenLevelHismFoliagePixelRecoveryTest,
    "PinWright.render.capture_open_level.HismFoliagePixelRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureOpenLevelHismFoliagePixelRecoveryTest::RunTest(const FString& Parameters)
{
    UWorld* World = ViewDistanceTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() was null, so no frame could be rendered"));
        return true;
    }

    const float ScaleBeforeCall = ReadViewDistanceScaleCVar();
    FScopedEditorWorldActorGuard Guard;

    UHierarchicalInstancedStaticMeshComponent* Hism =
        ViewDistanceTestSpawnScatter(World, /*SideCount=*/3, /*HalfExtentCm=*/20000.0, /*InstanceScale=*/60.0);
    if (!Hism)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("could not build the instanced scatter probe (/Engine/BasicShapes/Cube or SpawnActor failed)"));
        return true;
    }
    // 20000 cm against a frame whose corners are 64000 cm across and whose culling origin is
    // ~2.1e6 cm behind the camera: culled everywhere at scale 1, recoverable at the derived scale.
    Hism->SetCullDistances(0, 20000);
    AActor* Holder = Hism->GetOwner();

    const int32 Edge = 256;

    // Every frame below is pinned to Lit rather than inheriting the viewport's mode. The property
    // measured here belongs to a LIT orthographic view: the near-plane correction that moves the
    // culling origin behind the camera is skipped outright in wireframe
    // (EditorViewportClient.cpp: `if(!ViewFamily->EngineShowFlags.Wireframe)`), and an ortho editor
    // viewport defaults to Wireframe through UE 5.5. Left to the default the four frames are
    // wireframe on those engines, where the scatter contributes no pixels at any scale and the
    // differential answers a different question than the one this test asks.
    auto MakeLitTopDownPayload = [Edge]()
    {
        TSharedPtr<FJsonObject> Payload = ViewDistanceTestMakeTopDownPayload(Edge);
        Payload->SetStringField(TEXT("viewMode"), TEXT("Lit"));
        return Payload;
    };

    // --- frame C: the derived scale, scatter present. Also the source of the scale to reuse. ---
    FTestResponseCapture AutoWithScatter;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"),
            MakeLitTopDownPayload(), AutoWithScatter));
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(AutoWithScatter);
    };

    TArray<FColor> PixelsAutoWithScatter;
    int32 Width = 0;
    int32 Height = 0;
    if (!ViewDistanceTestReadCapturePixels(AutoWithScatter, PixelsAutoWithScatter, Width, Height))
    {
        // A host with no level viewport cannot render at all. That is a host limitation, not the
        // interesting condition, and it is reported through the marker so the run cannot be quoted
        // as clean.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-capture-unavailable"),
            FString::Printf(TEXT("render.capture_open_level produced no readable frame (success=%s)"),
                AutoWithScatter.bSuccess ? TEXT("true") : TEXT("false")));
        return true;
    }

    const TSharedPtr<FJsonObject>* ViewDistance = nullptr;
    if (!AutoWithScatter.Result->TryGetObjectField(TEXT("viewDistance"), ViewDistance) ||
        !ViewDistance || !ViewDistance->IsValid())
    {
        AddError(TEXT("an orthographic capture reported no viewDistance block"));
        return false;
    }

    FString Source;
    (*ViewDistance)->TryGetStringField(TEXT("source"), Source);
    // NOT a skip: a scatter culling at 20000 cm inside a 64000 cm ortho frame MUST make the
    // derivation fire. If it did not, the derivation is broken and this test says so.
    TestEqual(TEXT("a scene with distance-culled instances derives a scale"), Source, FString(TEXT("auto")));

    double AppliedScale = 0.0;
    (*ViewDistance)->TryGetNumberField(TEXT("scaleApplied"), AppliedScale);
    bool bClamped = false;
    (*ViewDistance)->TryGetBoolField(TEXT("clamped"), bClamped);
    if (Source != TEXT("auto"))
    {
        // The TestEqual above already recorded the failure and named the value; stopping here
        // avoids three more captures that cannot mean anything.
        return false;
    }
    if (AppliedScale <= 1.0)
    {
        AddError(FString::Printf(
            TEXT("source was 'auto' but scaleApplied read back as %.4f, so nothing was overridden"),
            AppliedScale));
        return false;
    }
    if (bClamped)
    {
        // The host map's own content limited the scale below what it needs, so this frame cannot
        // demonstrate a full recovery. Named and counted rather than passed over quietly.
        FString ClampReason;
        (*ViewDistance)->TryGetStringField(TEXT("clampReason"), ClampReason);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("derived-scale-clamped-by-host-map"),
            FString::Printf(TEXT("scaleApplied=%.2f clampReason=%s -- the open map's own cull ")
                            TEXT("distances cap the scale below full coverage"),
                AppliedScale, *ClampReason));
        return true;
    }

    // --- frame A: scale 1, scatter present ---
    TSharedPtr<FJsonObject> ScaleOnePayload = MakeLitTopDownPayload();
    ScaleOnePayload->SetNumberField(TEXT("viewDistanceScale"), 1.0);
    FTestResponseCapture ScaleOneWithScatter;
    InvokeHandlerWithCapture(TEXT("render.capture_open_level"), ScaleOnePayload, ScaleOneWithScatter);
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(ScaleOneWithScatter);
    };

    // --- the scatter leaves; the level is otherwise untouched ---
    if (IsValid(Holder))
    {
        World->EditorDestroyActor(Holder, /*bShouldModifyLevel=*/false);
    }

    // --- frame B: scale 1, no scatter ---
    FTestResponseCapture ScaleOneWithout;
    InvokeHandlerWithCapture(TEXT("render.capture_open_level"), ScaleOnePayload, ScaleOneWithout);
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(ScaleOneWithout);
    };

    // --- frame D: the SAME derived scale, no scatter ---
    TSharedPtr<FJsonObject> DerivedScalePayload = MakeLitTopDownPayload();
    DerivedScalePayload->SetNumberField(TEXT("viewDistanceScale"), AppliedScale);
    FTestResponseCapture DerivedWithout;
    InvokeHandlerWithCapture(TEXT("render.capture_open_level"), DerivedScalePayload, DerivedWithout);
    ON_SCOPE_EXIT
    {
        DeleteViewDistanceCaptureFile(DerivedWithout);
    };

    TestEqual(TEXT("r.ViewDistanceScale is back where the test found it"),
        ReadViewDistanceScaleCVar(), ScaleBeforeCall);

    TArray<FColor> PixelsScaleOneWithScatter;
    TArray<FColor> PixelsScaleOneWithout;
    TArray<FColor> PixelsDerivedWithout;
    int32 UnusedWidth = 0;
    int32 UnusedHeight = 0;
    const bool bAllFramesRead =
        ViewDistanceTestReadCapturePixels(ScaleOneWithScatter, PixelsScaleOneWithScatter, UnusedWidth, UnusedHeight) &&
        ViewDistanceTestReadCapturePixels(ScaleOneWithout, PixelsScaleOneWithout, UnusedWidth, UnusedHeight) &&
        ViewDistanceTestReadCapturePixels(DerivedWithout, PixelsDerivedWithout, UnusedWidth, UnusedHeight);
    if (!bAllFramesRead)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-capture-unavailable"),
            TEXT("one of the four differential frames could not be rendered or decoded"));
        return true;
    }

    const int32 TotalPixels = Width * Height;
    const int32 ScatterPixelsAtScaleOne =
        ViewDistanceTestCountDifferingPixels(PixelsScaleOneWithScatter, PixelsScaleOneWithout);
    const int32 ScatterPixelsAtDerivedScale =
        ViewDistanceTestCountDifferingPixels(PixelsAutoWithScatter, PixelsDerivedWithout);

    AddInfo(FString::Printf(
        TEXT("instanced scatter footprint: %d px of %d at scale 1, %d px at the derived scale %.2f"),
        ScatterPixelsAtScaleOne, TotalPixels, ScatterPixelsAtDerivedScale, AppliedScale));

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // THE RECOVERY, in pixels. 9 cubes of 6000 cm in a 64000 cm frame is ~8% of the image; 1% is a
    // deliberately loose floor so the assertion is about presence, not about a footprint estimate.
    TestTrue(TEXT("the instanced scatter draws pixels at the derived scale"),
        ScatterPixelsAtDerivedScale > TotalPixels / 100);
    // THE DEFECT, in pixels. Same scatter, same frame, scale 1: essentially nothing.
    TestTrue(TEXT("the same instanced scatter draws essentially nothing at scale 1"),
        ScatterPixelsAtScaleOne * 4 < ScatterPixelsAtDerivedScale);
#else
    // THE TWO PIXEL ASSERTIONS ARE NOT MEASURABLE FROM UE 5.6 ON, and the reason is in the engine
    // rather than in the derivation. They read the difference between a frame where instanced
    // content is distance-culled and one where it is not. UE 5.6 added
    // FHierarchicalStaticMeshSceneProxy::GetInstanceDrawDistanceMinMax returning FALSE
    // (HierarchicalInstancedStaticMesh.cpp) -- 5.5 and earlier inherited the
    // FInstancedStaticMeshSceneProxy implementation, which reports the end-cull distance and has
    // the instances culled on the GPU through InstanceDrawDistanceMinMaxSquared. From 5.6 that is
    // deliberately off for HISM ("HISM already does distance culling before submitting instances"),
    // and the CPU cluster path it defers to does not cull this probe: measured on 5.7, a scatter
    // given a ONE-CENTIMETRE end-cull distance still draws every instance at scale 1, from any
    // distance, and so does the same scatter built as a plain UInstancedStaticMeshComponent.
    //
    // With nothing culled there is nothing for r.ViewDistanceScale to recover, so the frames above
    // are the same frame and these two assertions would fail on a property the renderer no longer
    // has. Everything else in this test still runs on 5.6+: the capture is taken, the derivation
    // is asserted to fire, to exceed 1.0, to go unclamped, and to leave the cvar where it found
    // it. Only the pixel pair is stepped over, and the suite gate counts that.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("instanced-end-cull-not-honoured"),
        FString::Printf(
            TEXT("UE 5.6+ does not distance-cull instanced content in an orthographic editor ")
            TEXT("capture (FHierarchicalStaticMeshSceneProxy::GetInstanceDrawDistanceMinMax ")
            TEXT("returns false from 5.6), so no scale can recover content the renderer never ")
            TEXT("removed: the scatter measured %d px of %d at scale 1 and %d px at the derived ")
            TEXT("scale %.2f"),
            ScatterPixelsAtScaleOne, TotalPixels, ScatterPixelsAtDerivedScale, AppliedScale));
#endif
    return true;
}
