// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the shared shot-planning layer in Handlers/Render/CameraShotPlanUtils.h - the
// six-sides table, the shotDistribution serializer, the per-shot stats block and the one
// `resolutionSource` vocabulary. Every case here runs headless on synthetic input: none of these
// functions touches a viewport, an RHI or an asset, so nothing in this file can take a
// conditional-skip path and report success without having asserted anything.
//
// What is actually being defended:
//   * ONE six-sides table. It existed verbatim in three handlers, and three copies of an ordered
//     table are three chances for one of them to be reordered - a reordered `sides` set is six
//     individually correct images that no longer line up with the archived set they are compared
//     against. The expected angles are written here as LITERALS, not read back from the function
//     under test, so a silent reordering or a re-spelling of `back` as -180 fails.
//   * The per-shot `imageStats` block carries the numbers its verdicts are made of. AddCaptureFields
//     gained `litPixelCount` / `litPixelFraction` / `litLuminanceThreshold` and the tone range;
//     AddShotFields did not, so every shot of every multi-shot set reported four numbers while the
//     single-shot verbs reported nine. That is the exact drift AddShotFields exists to prevent.
//   * The absence direction of the tone range: an UNMEASURED frame must report no range at all
//     rather than the most collapsed range possible.
//   * `resolutionSource` is one vocabulary. It had three - "caller"|"budget"|"default",
//     "caller"|"burstBudget"|"singleStill", and nothing - for one concept.
//   * PinWrightCameraFrame::GetActiveLevelViewport emits REGISTERED error codes. This scan was
//     written because the PinWright.core.error_codes registry walk was anchored on
//     SendError(TEXT("CODE")) call sites, and this function assigns its code to an out-parameter
//     that a handler later passes to SendError as a VARIABLE - invisible to it. That gap is now
//     closed at the source: the walk grew a third pattern for out-parameter assignment, and
//     core.error_codes.RegistryAdoptingFilesUseConstantsOnly holds this header to zero hand-spelled
//     codes because it cites the registry. What is kept here and NOT covered there is the positive
//     direction - that the three specific constants exist, spell their wire strings exactly, and
//     are the things actually assigned. Belt and braces, deliberately.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/CaptureRendererNames.h"

// Deliberately NO file-scope using-directive for PinWrightCameraFrame, matching the rule its own
// header states: under Unity the directive would leak into every later translation unit in the
// blob and collide with the same-named anonymous-namespace helpers in sibling Render files
// (IsValidProjectionMode exists in PreviewViewportCaptureUtils.cpp too). Each test body pulls the
// namespace in locally instead.
namespace
{
    // One planned side, spelled as a literal pair so the test carries its own copy of the
    // contract rather than reading the answer off the function under test.
    struct FPwShotPlanExpectedSide
    {
        const TCHAR* Name;
        float Azimuth;
        float Elevation;
    };

    // The six axis-aligned views in emission order, transcribed from the table that lived at
    // CameraFrameHandler.cpp:361-383 before it was lifted. `back` is 180 (not -180) and `right`
    // is -90 (not 270): both spellings keep FRotator::NormalizeAxis from reporting a spurious
    // orthographic snap, so they are contract, not formatting.
    const FPwShotPlanExpectedSide GPwShotPlanExpectedSides[] = {
        { TEXT("front"),    0.0f,   0.0f },
        { TEXT("back"),   180.0f,   0.0f },
        { TEXT("left"),    90.0f,   0.0f },
        { TEXT("right"),  -90.0f,   0.0f },
        { TEXT("top"),      0.0f,  90.0f },
        { TEXT("bottom"),   0.0f, -90.0f },
    };

    // A fully populated stats block. Values are chosen distinct and exactly representable so a
    // field read out of the wrong slot is visible rather than coincidentally equal.
    PinWrightRenderCapture::FCaptureImageStats PwShotPlanMakeMeasuredStats()
    {
        PinWrightRenderCapture::FCaptureImageStats Stats;
        Stats.MeanLuminance = 0.375;
        Stats.LuminanceVariance = 0.0625;
        Stats.MinLuminance = 0.125;
        Stats.MaxLuminance = 0.875;
        Stats.LitPixelCount = 12345;
        Stats.LitPixelFraction = 0.5;
        Stats.bBlank = false;
        Stats.bStatsMeasured = true;
        Stats.ToneLevelsUsed = 37;
        Stats.ToneLevelMinPixelsUsed = 64;
        Stats.bCrushed = false;
        Stats.bBlownOut = false;
        return Stats;
    }

    // Run AddShotFields over a synthetic capture and hand back the `imageStats` sub-object.
    TSharedPtr<FJsonObject> PwShotPlanBuildShotStats(
        const PinWrightRenderCapture::FCaptureImageStats& Stats, TSharedPtr<FJsonObject>& OutShot)
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.Path = TEXT("D:/shots/shot_0.png");
        Capture.Filename = TEXT("shot_0.png");
        Capture.Width = 640;
        Capture.Height = 640;
        Capture.SizeBytes = 4096;
        // The value the REAL producer of this field writes. PreviewViewportCaptureUtils.cpp:1443
        // sets `sceneViewportReadPixels` and CameraShotPlanUtils.h copies it onto the response, so
        // that is the only spelling AddShotFields ever sees in production. The fixture used to say
        // "editorViewport", which no producer emits and which CaptureRendererNames.h does not
        // define - a fixture carrying a value outside the field's vocabulary cannot catch a
        // renderer-vocabulary defect, because it was never in the vocabulary to begin with.
        Capture.Renderer = PinWrightCaptureRenderer::SceneViewportReadPixels;
        Capture.ImageStats = Stats;
        Capture.bCameraAimApplied = true;

        PinWrightRenderCapture::FViewportCaptureRequest Request;
        Request.ProjectionMode = TEXT("perspective");
        Request.Fov = 50.0f;

        OutShot = MakeShared<FJsonObject>();
        PinWrightCameraFrame::AddShotFields(Capture, Request, OutShot);

        const TSharedPtr<FJsonObject>* ImageStats = nullptr;
        if (OutShot->TryGetObjectField(TEXT("imageStats"), ImageStats) && ImageStats)
        {
            return *ImageStats;
        }
        return nullptr;
    }

    FString PwShotPlanResolveHeaderPath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("Render") / TEXT("CameraShotPlanUtils.h");
    }
}

// ============================================================================
// one six-sides table
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanSidesTableIsOneTableTest,
    "PinWright.render.shot_plan.SidesTableIsOneTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanSidesTableIsOneTableTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    const TArray<FPlannedShot> Sides = MakeSideViews(TEXT("orthographic"));

    if (!TestEqual(TEXT("MakeSideViews plans exactly six views"), Sides.Num(), GSideViewCount))
    {
        return true;
    }
    TestEqual(TEXT("GSideViewCount names the table's own length"), GSideViewCount, 6);

    for (int32 Index = 0; Index < Sides.Num(); ++Index)
    {
        const FPwShotPlanExpectedSide& Expected = GPwShotPlanExpectedSides[Index];
        TestEqual(*FString::Printf(TEXT("side %d (%s) azimuth"), Index, Expected.Name),
            Sides[Index].Azimuth, Expected.Azimuth);
        TestEqual(*FString::Printf(TEXT("side %d (%s) elevation"), Index, Expected.Name),
            Sides[Index].Elevation, Expected.Elevation);
        TestEqual(*FString::Printf(TEXT("side %d (%s) carries the caller's projection"),
            Index, Expected.Name), Sides[Index].ProjectionMode, FString(TEXT("orthographic")));
    }

    // The two spellings that are contract rather than formatting. Asserted as inequalities as
    // well, because 180 and -180 name the same direction and only one of them keeps the snap
    // silent - an equality test alone would pass on either.
    TestTrue(TEXT("`back` is spelled 180, not -180"), Sides[1].Azimuth > 0.0f);
    TestTrue(TEXT("`right` is spelled -90, not 270"), Sides[3].Azimuth < 0.0f);

    // Every pose already lands on a cardinal world axis, so the orthographic snap is a no-op on
    // all six. A table that drifted off-axis would still return six shots and still look right in
    // a response; this is the assertion that catches it.
    for (int32 Index = 0; Index < Sides.Num(); ++Index)
    {
        float Azimuth = Sides[Index].Azimuth;
        float Elevation = Sides[Index].Elevation;
        TestFalse(*FString::Printf(TEXT("side %d (%s) needs no orthographic snap"),
            Index, GPwShotPlanExpectedSides[Index].Name),
            SnapOrbitAnglesToOrthographicAxis(Azimuth, Elevation));
    }

    // The projection mode is the caller's, not a constant baked into the table: each verb decides
    // whether an unsized `views` call means orthographic or honours an explicit projectionMode.
    const TArray<FPlannedShot> PerspectiveSides = MakeSideViews(TEXT("perspective"));
    if (TestEqual(TEXT("a perspective sides plan is still six views"),
        PerspectiveSides.Num(), GSideViewCount))
    {
        TestEqual(TEXT("projection mode is the caller's"), PerspectiveSides[0].ProjectionMode,
            FString(TEXT("perspective")));
        TestEqual(TEXT("angles do not depend on the projection mode"),
            PerspectiveSides[5].Elevation, -90.0f);
    }

    return true;
}

// ============================================================================
// per-shot stats: the full set, and the absence rule on the tone range
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanPerShotStatsCarryTheLitPairTest,
    "PinWright.render.shot_plan.PerShotStatsCarryTheLitPair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanPerShotStatsCarryTheLitPairTest::RunTest(const FString& Parameters)
{
    const PinWrightRenderCapture::FCaptureImageStats Stats = PwShotPlanMakeMeasuredStats();

    TSharedPtr<FJsonObject> Shot;
    const TSharedPtr<FJsonObject> ImageStats = PwShotPlanBuildShotStats(Stats, Shot);
    if (!TestTrue(TEXT("AddShotFields emits an imageStats object"), ImageStats.IsValid()))
    {
        return true;
    }

    // The four that were always there, so a regression that adds the new keys by dropping the old
    // ones cannot pass.
    TestEqual(TEXT("meanLuminance"), ImageStats->GetNumberField(TEXT("meanLuminance")),
        Stats.MeanLuminance);
    TestEqual(TEXT("luminanceVariance"), ImageStats->GetNumberField(TEXT("luminanceVariance")),
        Stats.LuminanceVariance);
    TestEqual(TEXT("minLuminance"), ImageStats->GetNumberField(TEXT("minLuminance")),
        Stats.MinLuminance);
    TestEqual(TEXT("maxLuminance"), ImageStats->GetNumberField(TEXT("maxLuminance")),
        Stats.MaxLuminance);

    // The three the blank verdict is made of.
    TestTrue(TEXT("litPixelCount is present"), ImageStats->HasField(TEXT("litPixelCount")));
    TestTrue(TEXT("litPixelFraction is present"), ImageStats->HasField(TEXT("litPixelFraction")));
    TestTrue(TEXT("litLuminanceThreshold is present"),
        ImageStats->HasField(TEXT("litLuminanceThreshold")));
    TestEqual(TEXT("litPixelCount carries the measured count"),
        ImageStats->GetNumberField(TEXT("litPixelCount")), static_cast<double>(Stats.LitPixelCount));
    TestEqual(TEXT("litPixelFraction carries the measured fraction"),
        ImageStats->GetNumberField(TEXT("litPixelFraction")), Stats.LitPixelFraction);
    // The threshold is the shared constant, not a per-verb copy: a caller reproducing the verdict
    // against a different number would reach a different verdict.
    TestEqual(TEXT("litLuminanceThreshold is the shared classifier's threshold"),
        ImageStats->GetNumberField(TEXT("litLuminanceThreshold")),
        PinWrightRenderCapture::BlankLitLuminanceThreshold);

    // The tone range, compared against what the SHARED helper writes rather than against
    // transcribed literals - AddCaptureFields builds its own block with the same call, so this is
    // the same values AddCaptureFields would produce for the same FCaptureImageStats.
    const TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
    PinWrightRenderCapture::AddToneRangeStatsFields(Stats, Reference);
    TestTrue(TEXT("the reference block carries toneLevelsUsed"),
        Reference->HasField(TEXT("toneLevelsUsed")));
    TestTrue(TEXT("toneLevelsUsed is present on the shot"),
        ImageStats->HasField(TEXT("toneLevelsUsed")));
    TestTrue(TEXT("toneLevelMinPixels is present on the shot"),
        ImageStats->HasField(TEXT("toneLevelMinPixels")));
    TestEqual(TEXT("toneLevelsUsed matches the shared helper"),
        ImageStats->GetNumberField(TEXT("toneLevelsUsed")),
        Reference->GetNumberField(TEXT("toneLevelsUsed")));
    TestEqual(TEXT("toneLevelMinPixels matches the shared helper"),
        ImageStats->GetNumberField(TEXT("toneLevelMinPixels")),
        Reference->GetNumberField(TEXT("toneLevelMinPixels")));

    // The verdict still sits beside its inputs, one level up.
    TestTrue(TEXT("the shot still publishes the blank verdict"), Shot->HasField(TEXT("blank")));
    TestFalse(TEXT("a measured non-blank shot reports blank:false"),
        Shot->GetBoolField(TEXT("blank")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanUnmeasuredShotReportsNoToneRangeTest,
    "PinWright.render.shot_plan.UnmeasuredShotReportsNoToneRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanUnmeasuredShotReportsNoToneRangeTest::RunTest(const FString& Parameters)
{
    // A readback that returned no pixels: bStatsMeasured false, and ToneLevelsUsed therefore 0.
    // Emitting `toneLevelsUsed: 0` here would report the most collapsed frame possible - a verdict
    // nobody reached. This is the absence direction, and it is the half a "field is present" test
    // can never catch.
    PinWrightRenderCapture::FCaptureImageStats Stats;
    Stats.bStatsMeasured = false;
    Stats.bBlank = true;

    // Precondition, asserted rather than assumed: without it the test would pass on a fixture that
    // simply had a measured range of zero levels.
    if (!TestFalse(TEXT("the fixture frame was never measured"), Stats.bStatsMeasured))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Shot;
    const TSharedPtr<FJsonObject> ImageStats = PwShotPlanBuildShotStats(Stats, Shot);
    if (!TestTrue(TEXT("AddShotFields emits an imageStats object"), ImageStats.IsValid()))
    {
        return true;
    }

    TestFalse(TEXT("toneLevelsUsed is OMITTED on an unmeasured frame"),
        ImageStats->HasField(TEXT("toneLevelsUsed")));
    TestFalse(TEXT("toneLevelMinPixels is OMITTED on an unmeasured frame"),
        ImageStats->HasField(TEXT("toneLevelMinPixels")));

    // The lit pair is NOT gated on bStatsMeasured, matching AddCaptureFields exactly: they are
    // counts, and a zero count is a real answer where a zero tone range is not.
    TestTrue(TEXT("litPixelCount is still published"), ImageStats->HasField(TEXT("litPixelCount")));
    TestTrue(TEXT("litPixelFraction is still published"),
        ImageStats->HasField(TEXT("litPixelFraction")));
    TestTrue(TEXT("the blank verdict is still published"), Shot->HasField(TEXT("blank")));
    TestTrue(TEXT("an unmeasured frame reports blank:true"), Shot->GetBoolField(TEXT("blank")));

    return true;
}

// ============================================================================
// one resolutionSource vocabulary
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanResolutionSourceIsOneVocabularyTest,
    "PinWright.render.shot_plan.ResolutionSourceIsOneVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanResolutionSourceIsOneVocabularyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    // The exact strings, transcribed as literals. Three of the four are asserted by the
    // regression floor (Tests/Render/TestCameraFrameHandlers.cpp), so a rename here is a wire
    // break there; writing them out means this file fails first and says why.
    TestEqual(TEXT("caller"), ResolutionSourceKey(EResolutionSource::Caller),
        FString(TEXT("caller")));
    TestEqual(TEXT("budget"), ResolutionSourceKey(EResolutionSource::Budget),
        FString(TEXT("budget")));
    TestEqual(TEXT("singleStill"), ResolutionSourceKey(EResolutionSource::SingleStill),
        FString(TEXT("singleStill")));
    TestEqual(TEXT("default"), ResolutionSourceKey(EResolutionSource::Default),
        FString(TEXT("default")));

    // The caller always wins, whichever semantic default the verb would otherwise have applied.
    TestEqual(TEXT("caller size beats the multi-image budget"),
        ResolveResolutionSource(true, EResolutionSource::Budget), FString(TEXT("caller")));
    TestEqual(TEXT("caller size beats the single-still budget"),
        ResolveResolutionSource(true, EResolutionSource::SingleStill), FString(TEXT("caller")));
    TestEqual(TEXT("caller size beats the legacy default"),
        ResolveResolutionSource(true, EResolutionSource::Default), FString(TEXT("caller")));

    // Equal integers must not merge the semantic branches.
    TestEqual(TEXT("set policy reports budget"),
        ResolveResolutionSource(false, EResolutionSource::Budget), FString(TEXT("budget")));
    TestEqual(TEXT("single-image policy reports singleStill"),
        ResolveResolutionSource(false, EResolutionSource::SingleStill), FString(TEXT("singleStill")));
    TestEqual(TEXT("ordinary policy reports default"),
        ResolveResolutionSource(false, EResolutionSource::Default), FString(TEXT("default")));

    // The values the vocabulary is anchored to, spelled out so a change to any of them is a
    // deliberate act with a failing test attached.
    TestEqual(TEXT("multi-image budget long edge"), GMultiImageBudgetEdge, 768);
    TestEqual(TEXT("single-still long edge"), GSingleStillEdge, 768);
    TestEqual(TEXT("ordinary default long edge"), GLegacyDefaultEdge, 768);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCaptureDefaultSizeTest,
    "PinWright.render.capture.DefaultSizeIs768Square",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwCaptureDefaultSizeTest::RunTest(const FString& Parameters)
{
    PinWrightRenderCapture::FViewportCaptureRequest Request;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("an omitted-size request parses"),
        PinWrightRenderCapture::ParseViewportCaptureRequest(
            TSharedPtr<FJsonObject>(), Request, ErrorCode, ErrorMessage));
    TestEqual(TEXT("default capture width"), Request.Width, 768);
    TestEqual(TEXT("default capture height"), Request.Height, 768);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCaptureProjectionCircleTest,
    "PinWright.render.capture.DefaultProjectionKeepsCircleCircular",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwCaptureProjectionCircleTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // Project a radius-100 circle on a plane 1000 units in front of a 90-degree horizontal-FOV
    // camera. If a 16:9 projection were retained for the square buffer, these pixel radii would be
    // 38.4 and 68.3: an obvious ellipse. Deriving aspect from 768x768 makes them equal.
    constexpr double Radius = 100.0;
    constexpr double Distance = 1000.0;
    constexpr double HalfHorizontalFov = UE_PI / 4.0;
    constexpr double TolerancePixels = 0.01;
    const int32 Width = DefaultCaptureEdge;
    const int32 Height = DefaultCaptureEdge;
    const double Aspect = ResolveCaptureProjectionAspect(Width, Height);
    const double HalfVerticalFov = FMath::Atan(FMath::Tan(HalfHorizontalFov) / Aspect);
    const double HorizontalExtent = (Width * 0.5) * Radius /
        (Distance * FMath::Tan(HalfHorizontalFov));
    const double VerticalExtent = (Height * 0.5) * Radius /
        (Distance * FMath::Tan(HalfVerticalFov));

    TestTrue(*FString::Printf(TEXT("known circle has equal pixel extents (x=%.3f, y=%.3f)"),
        HorizontalExtent, VerticalExtent),
        FMath::Abs(HorizontalExtent - VerticalExtent) <= TolerancePixels);

    // Square is a default, not a constraint. The same circle stays circular at explicit 960x540.
    constexpr int32 WideWidth = 960;
    constexpr int32 WideHeight = 540;
    const double WideAspect = ResolveCaptureProjectionAspect(WideWidth, WideHeight);
    const double WideHalfVerticalFov = FMath::Atan(FMath::Tan(HalfHorizontalFov) / WideAspect);
    const double WideHorizontalExtent = (WideWidth * 0.5) * Radius /
        (Distance * FMath::Tan(HalfHorizontalFov));
    const double WideVerticalExtent = (WideHeight * 0.5) * Radius /
        (Distance * FMath::Tan(WideHalfVerticalFov));
    TestTrue(TEXT("explicit non-square projection also keeps the circle circular"),
        FMath::Abs(WideHorizontalExtent - WideVerticalExtent) <= TolerancePixels);
    return true;
}

// ============================================================================
// one shotDistribution serializer
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanDistributionBlockTest,
    "PinWright.render.shot_plan.DistributionBlockReportsTheSeedAndThePlan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanDistributionBlockTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    // Unseeded ring over a `count` plan - the default shape.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Ring;
        Plan.bAppliesToPlan = true;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (!TestTrue(TEXT("the unseeded block is built"), Block.IsValid()))
        {
            return true;
        }
        TestEqual(TEXT("distribution names the construction"),
            Block->GetStringField(TEXT("distribution")), FString(TEXT("ring")));
        TestFalse(TEXT("seeded is false"), Block->GetBoolField(TEXT("seeded")));
        TestTrue(TEXT("appliesToPlan is true for a count plan"),
            Block->GetBoolField(TEXT("appliesToPlan")));
        // Absence direction: no seed was in force, so no seed is reported. An echoed 0 would read
        // as a seed the caller could pass back, which would not reproduce anything.
        TestFalse(TEXT("seed is OMITTED when none was supplied"), Block->HasField(TEXT("seed")));
        TestFalse(TEXT("azimuthOffsetDegrees is OMITTED when no seed was supplied"),
            Block->HasField(TEXT("azimuthOffsetDegrees")));
        TestFalse(TEXT("no elevation warning on a ring plan"),
            Block->HasField(TEXT("elevationWarning")));
        TestFalse(TEXT("no elevationIgnored flag on a ring plan"),
            Block->HasField(TEXT("elevationIgnored")));
    }

    // Seeded sphere over an `angles` plan, with an elevation the construction cannot use.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Sphere;
        Plan.bSeeded = true;
        Plan.Seed = 7;
        Plan.AzimuthOffsetDegrees = SeedToAzimuthOffsetDegrees(7);
        Plan.bAppliesToPlan = false;
        Plan.bElevationProvided = true;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (!TestTrue(TEXT("the seeded block is built"), Block.IsValid()))
        {
            return true;
        }
        TestEqual(TEXT("distribution names the construction"),
            Block->GetStringField(TEXT("distribution")), FString(TEXT("sphere")));
        TestTrue(TEXT("seeded is true"), Block->GetBoolField(TEXT("seeded")));
        TestEqual(TEXT("the seed is echoed exactly, so the set can be retaken"),
            static_cast<int32>(Block->GetNumberField(TEXT("seed"))), 7);
        TestEqual(TEXT("the derived azimuth offset is published"),
            Block->GetNumberField(TEXT("azimuthOffsetDegrees")), SeedToAzimuthOffsetDegrees(7));
        TestFalse(TEXT("appliesToPlan is false for an angles plan"),
            Block->GetBoolField(TEXT("appliesToPlan")));
        // A supplied-and-unusable parameter is reported, not dropped.
        TestTrue(TEXT("elevationIgnored fires"), Block->GetBoolField(TEXT("elevationIgnored")));
        const FString Warning = Block->GetStringField(TEXT("elevationWarning"));
        TestTrue(TEXT("the warning names the argument"), Warning.Contains(TEXT("elevation")));
        TestTrue(TEXT("the warning says it was IGNORED"), Warning.Contains(TEXT("IGNORED")));
        TestTrue(TEXT("the warning names the way out"), Warning.Contains(TEXT("distribution:'ring'")));
    }

    // Sphere WITHOUT an elevation argument: nothing was dropped, so nothing is warned about.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Sphere;
        Plan.bAppliesToPlan = true;
        Plan.bElevationProvided = false;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (TestTrue(TEXT("the block is built"), Block.IsValid()))
        {
            TestFalse(TEXT("no elevation warning when no elevation was passed"),
                Block->HasField(TEXT("elevationWarning")));
            TestFalse(TEXT("no elevationIgnored flag when no elevation was passed"),
                Block->HasField(TEXT("elevationIgnored")));
        }
    }

    return true;
}

// ============================================================================
// the level-viewport acquisition codes come from the registry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanLevelViewportCodesAreRegisteredTest,
    "PinWright.render.shot_plan.LevelViewportCodesComeFromTheRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanLevelViewportCodesAreRegisteredTest::RunTest(const FString& Parameters)
{
    // The wire strings are unchanged by the swap to constants. Asserted first, because a constant
    // whose value differs from the literal it replaced is a silent contract break that the
    // source scan below would happily call clean.
    TestEqual(TEXT("ERR_EDITOR_NOT_AVAILABLE spells the emitted code"),
        FString(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE), FString(TEXT("EDITOR_NOT_AVAILABLE")));
    TestEqual(TEXT("ERR_NO_EDITOR_WORLD spells the emitted code"),
        FString(ErrorCodes::ERR_NO_EDITOR_WORLD), FString(TEXT("NO_EDITOR_WORLD")));
    TestEqual(TEXT("ERR_NO_ACTIVE_LEVEL_VIEWPORT spells the emitted code"),
        FString(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT),
        FString(TEXT("NO_ACTIVE_LEVEL_VIEWPORT")));

    // A source scan, because the runtime path cannot be forced: GetActiveLevelViewport's three
    // refusals need an editor with no world / no LevelEditor module / no active viewport, and a
    // test that only exercised the success path would assert nothing about the codes at all.
    // PinWright.core.error_codes.AllEmittedCodesAreRegistered used not to cover this - it matched
    // SendError(TEXT("CODE")) call sites only, and these codes are assigned to an out-parameter
    // that a handler later passes to SendError as a variable. It now carries a third pattern for
    // exactly that shape, so the NEGATIVE half below (no raw literal assigned to OutErrCode) is
    // duplicated coverage. The POSITIVE half is not: only this test asserts that the three
    // constants are the things actually assigned.
    const FString HeaderPath = PwShotPlanResolveHeaderPath();
    if (!TestFalse(TEXT("resolved CameraShotPlanUtils.h through IPluginManager"),
        HeaderPath.IsEmpty()))
    {
        return true;
    }
    FString Contents;
    if (!TestTrue(TEXT("read CameraShotPlanUtils.h from disk"),
        FFileHelper::LoadFileToString(Contents, *HeaderPath)))
    {
        return true;
    }

    TestFalse(TEXT("no raw TEXT(\"...\") error code is assigned to OutErrCode"),
        Contents.Contains(TEXT("OutErrCode = TEXT(\"")));
    TestTrue(TEXT("EDITOR_NOT_AVAILABLE comes from the registry constant"),
        Contents.Contains(TEXT("OutErrCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;")));
    TestTrue(TEXT("NO_EDITOR_WORLD comes from the registry constant"),
        Contents.Contains(TEXT("OutErrCode = ErrorCodes::ERR_NO_EDITOR_WORLD;")));
    TestTrue(TEXT("NO_ACTIVE_LEVEL_VIEWPORT comes from the registry constant"),
        Contents.Contains(TEXT("OutErrCode = ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT;")));

    return true;
}

// ============================================================================
// `elevation` that could not reach a camera is REPORTED, never dropped
// ============================================================================
//
// THE DEFECT. With projectionMode:"orthographic" a `count` set answered pitch 0 and `_el0`
// filenames while the perspective A/B of the same call answered pitch -12 and `_el12`; the
// top-level `elevation` echoed null on both and `shotDistribution` said nothing at all. Two
// separate falsehoods sat in that: an argument accepted and silently discarded
// (docs/rpc-design.md section 21), and the documented DEFAULT of 30 silently rewritten to 0 for
// a caller who never passed `elevation` and therefore had nothing to suspect.
//
// IT CANNOT BE HONOURED, and that was checked against the engine rather than assumed.
// FEditorViewportClient::CalcSceneView builds an orthographic view rotation matrix from the
// VIEWPORT TYPE and never reads the camera rotation (UE 5.8 EditorViewportClient.cpp:1341-1401),
// and LVT_OrthoFreelook is a seventh FIXED matrix rather than a free one - so only the six
// cardinal directions are renderable and SnapOrbitAnglesToOrthographicAxis is correct engine
// behaviour, not the bug. The parameter's own wire description already promised the remedy for
// exactly this case: an elevation that cannot be used "says so in shotDistribution rather than
// dropping it silently". This asserts the promise for all three constructions that swallow it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwShotPlanElevationIgnoredReportingTest,
    "PinWright.render.shot_plan.ElevationThatCannotBeHeldIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwShotPlanElevationIgnoredReportingTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCameraFrame;

    // ---- the snap really does flatten, which is the premise everything below reports on.
    {
        float Azimuth = 45.0f;
        float Elevation = 30.0f;
        const bool bMoved = SnapOrbitAnglesToOrthographicAxis(Azimuth, Elevation);
        TestTrue(TEXT("the orthographic snap MOVES a 30-degree elevation"), bMoved);
        TestEqual(TEXT("an elevation below 45 flattens to the horizon"), Elevation, 0.0f);
    }

    // ---- ORTHOGRAPHIC, and the caller never passed `elevation`: the DEFAULT was rewritten.
    // The case the old code was most silent about. bElevationProvided is false, so a report gated
    // on "did the caller supply one" would still say nothing while 30 became 0.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Ring;
        Plan.bAppliesToPlan = true;
        Plan.bElevationProvided = false;
        Plan.ElevationDegrees = 30.0f;
        Plan.PlannedShots = 4;
        Plan.ElevationSnappedShots = 4;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (!TestTrue(TEXT("the orthographic block is built"), Block.IsValid()))
        {
            return true;
        }
        TestTrue(TEXT("elevationIgnored fires even though the caller supplied nothing"),
            Block->GetBoolField(TEXT("elevationIgnored")));
        TestEqual(TEXT("the reason names the projection, not the distribution"),
            Block->GetStringField(TEXT("elevationIgnoredReason")),
            FString(ElevationIgnoredReason::OrthographicProjection));
        TestEqual(TEXT("how many shots the snap moved is MEASURED and published"),
            static_cast<int32>(Block->GetNumberField(TEXT("elevationSnappedShots"))), 4);
        // The whole point of publishing the requested value: without it, 30 holding and 30 being
        // rewritten to 0 produce the same response.
        TestEqual(TEXT("the elevation the plan was BUILT from is published"),
            Block->GetNumberField(TEXT("elevationRequestedDegrees")), 30.0);
        const FString Warning = Block->GetStringField(TEXT("elevationWarning"));
        TestTrue(TEXT("the warning names the projection as the cause"),
            Warning.Contains(TEXT("ORTHOGRAPHIC")));
        TestTrue(TEXT("the warning cites the engine behaviour rather than asserting it"),
            Warning.Contains(TEXT("EditorViewportClient.cpp")));
        TestTrue(TEXT("the warning names the remedy"),
            Warning.Contains(TEXT("perspective")));
        TestTrue(TEXT("the warning points at the per-shot angles for what was placed"),
            Warning.Contains(TEXT("angle.elevation")));
    }

    // ---- ORTHOGRAPHIC where the plan was ALREADY cardinal: nothing was dropped, so nothing is
    // claimed. The `sides` table is axis-aligned by construction and must not trip the report - a
    // false "your elevation was ignored" is the same class of wrong statement, mirrored. This is
    // also why the count is measured off the two angle tables instead of deduced from
    // projectionMode.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Ring;
        Plan.bAppliesToPlan = true;
        Plan.ElevationDegrees = 0.0f;
        Plan.PlannedShots = 6;
        Plan.ElevationSnappedShots = 0;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (TestTrue(TEXT("the cardinal block is built"), Block.IsValid()))
        {
            TestFalse(TEXT("no elevationIgnored when the snap moved nothing"),
                Block->HasField(TEXT("elevationIgnored")));
            TestFalse(TEXT("no elevationWarning when the snap moved nothing"),
                Block->HasField(TEXT("elevationWarning")));
            TestFalse(TEXT("no elevationSnappedShots when the snap moved nothing"),
                Block->HasField(TEXT("elevationSnappedShots")));
            TestTrue(TEXT("the requested elevation is still published on a ring plan"),
                Block->HasField(TEXT("elevationRequestedDegrees")));
        }
    }

    // ---- the ORTHOGRAPHIC reason OUTRANKS the sphere one. A measured outcome beats a deduction
    // from the request shape: both are true here, but only one of them moved the camera, and a
    // caller told to switch distribution would change the wrong argument and get the same set.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Sphere;
        Plan.bAppliesToPlan = true;
        Plan.bElevationProvided = true;
        Plan.ElevationDegrees = 55.0f;
        Plan.PlannedShots = 8;
        Plan.ElevationSnappedShots = 8;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (TestTrue(TEXT("the sphere-plus-orthographic block is built"), Block.IsValid()))
        {
            TestEqual(TEXT("the measured cause wins over the request-shape deduction"),
                Block->GetStringField(TEXT("elevationIgnoredReason")),
                FString(ElevationIgnoredReason::OrthographicProjection));
        }
    }

    // ---- a NAMED shot plan (`views`) with an elevation: the third silent drop. `views` reads no
    // elevation at all, so the argument was accepted and positively confirmed while doing nothing.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Ring;
        Plan.bAppliesToPlan = false;   // `views` names its own poses
        Plan.bElevationProvided = true;
        Plan.ElevationDegrees = 30.0f;
        Plan.PlannedShots = 6;
        Plan.ElevationSnappedShots = 0;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (TestTrue(TEXT("the named-plan block is built"), Block.IsValid()))
        {
            TestTrue(TEXT("elevationIgnored fires for a named shot plan"),
                Block->GetBoolField(TEXT("elevationIgnored")));
            TestEqual(TEXT("the reason names the shot plan"),
                Block->GetStringField(TEXT("elevationIgnoredReason")),
                FString(ElevationIgnoredReason::NamedViewPlan));
            TestTrue(TEXT("the warning names count as what elevation applies to"),
                Block->GetStringField(TEXT("elevationWarning")).Contains(TEXT("count")));
            // A named plan sits on no orbiting circle, so there is no "the plan was built from"
            // value to publish and claiming one would be an invention.
            TestFalse(TEXT("no elevationRequestedDegrees on a plan that names its own poses"),
                Block->HasField(TEXT("elevationRequestedDegrees")));
        }
    }

    // ---- the sphere case still reports its own reason when no snap happened. Guards the branch
    // ORDER: the orthographic test above only proves precedence if this one proves the sphere
    // branch is still reachable at all.
    {
        FShotDistributionPlan Plan;
        Plan.Distribution = EShotDistribution::Sphere;
        Plan.bAppliesToPlan = true;
        Plan.bElevationProvided = true;
        Plan.PlannedShots = 8;
        Plan.ElevationSnappedShots = 0;
        const TSharedPtr<FJsonObject> Block = MakeShotDistributionObject(Plan);
        if (TestTrue(TEXT("the sphere block is built"), Block.IsValid()))
        {
            TestEqual(TEXT("the sphere reason survives the new branch order"),
                Block->GetStringField(TEXT("elevationIgnoredReason")),
                FString(ElevationIgnoredReason::SphereDistribution));
        }
    }

    return true;
}
