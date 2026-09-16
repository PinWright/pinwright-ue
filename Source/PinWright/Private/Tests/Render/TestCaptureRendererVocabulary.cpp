// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the two capture-wide wire vocabularies: the `renderer` field
// (Handlers/Render/CaptureRendererNames.h) and the perspective/orthographic field, which is
// spelled `projectionMode` on every producer.
//
// WHY THIS FILE EXISTS. Both vocabularies have already drifted in production, and in both cases
// nothing failed:
//   - render.capture_ortho_tiles published `renderer: "sceneCapture2D"` while
//     render.detect_z_fighting published `renderer: "sceneCaptureComponent2D"` for the SAME
//     offscreen USceneCaptureComponent2D. A comparison tool is supposed to REFUSE a pair whose
//     renderers differ (viewport vs. scene capture is 5.76% mean absolute error against a 1.30%
//     self-noise floor); two spellings for one renderer turn that refusal into a false alarm.
//   - render.capture_ortho_tiles published the projection concept as `projection` while the six
//     other response writers published `projectionMode`, so a caller pairing an ortho burst with
//     any other capture had to know two names for one field.
// Before this file, the ONLY assertion anywhere on a `renderer` value was the widget path's
// (Tests/Widget/TestWidgetDesignerScreenshotHandler.cpp:807-808). The scene-capture and viewport
// values had none, so neither drift could have been caught by the suite.
//
// THE CONTRACT IS CARRIED BY THE TWO SOURCE SCANS, NOT BY THE LIVE PAIR. A capture test can only
// read a published field on a run that actually rendered, and a headless commandlet often cannot;
// a guard that skips is exactly how a vocabulary drifts back in unnoticed. So the load-bearing
// tests here read the handler SOURCES and run identically on every host. The two live tests are
// corroboration - they prove the value survives the trip to the wire - and they follow this
// project's rule of asserting a TYPED refusal on the other branch and naming the branch taken.
//
// The wire spellings are written out as LITERALS below rather than compared against the
// constants. That duplication is the point: a test that reads
// PinWrightCaptureRenderer::SceneCapture2D on both sides of TestEqual cannot fail, whatever the
// constant is changed to. These literals are the wire contract docs/wiki-src/render.md quotes;
// if one of them has to change, this file, the header and the doc change together.

#include "Misc/AutomationTest.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Render/CaptureRendererNames.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Helper names are prefixed PwCapVocab because the test module is a Unity build: an
// anonymous-namespace helper sharing a name with one in a sibling .cpp is an ODR collision when
// Unity merges the two translation units. Same convention as DispatcherTestHelpers.h.
namespace
{
    // ---- the wire spellings, written as literals on purpose (see the file header) ----
    const TCHAR* const PwCapVocabSceneCapture2D = TEXT("sceneCapture2D");
    const TCHAR* const PwCapVocabSceneViewportReadPixels = TEXT("sceneViewportReadPixels");
    const TCHAR* const PwCapVocabWidgetRenderer = TEXT("widgetRenderer");
    const TCHAR* const PwCapVocabSlateScreenshot = TEXT("slateScreenshot");

    // Every module directory that ships handler code: the main module plus each integration
    // sub-module. Copied in shape from Tests/Core/TestErrorCodeRegistry.cpp so a sub-module that
    // starts publishing a `renderer` is covered the day it lands rather than being invisible.
    TArray<FString> PwCapVocabHandlerRoots()
    {
        TArray<FString> Roots;
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return Roots;
        }
        const FString SourceDir = Plugin->GetBaseDir() / TEXT("Source");
        TArray<FString> ModuleDirNames;
        IFileManager::Get().FindFiles(ModuleDirNames,
            *(SourceDir / TEXT("PinWright*")), /*Files=*/false, /*Directories=*/true);
        ModuleDirNames.Sort();
        for (const FString& ModuleDirName : ModuleDirNames)
        {
            const FString HandlersDir = SourceDir / ModuleDirName / TEXT("Private") / TEXT("Handlers");
            if (IFileManager::Get().DirectoryExists(*HandlersDir))
            {
                Roots.Add(HandlersDir);
            }
        }
        return Roots;
    }

    TArray<FString> PwCapVocabHandlerSourceFiles()
    {
        TArray<FString> Files;
        for (const FString& Root : PwCapVocabHandlerRoots())
        {
            IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.cpp"), true, false, false);
            IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.h"), true, false, false);
        }
        return Files;
    }

    FString PwCapVocabReadFile(const FString& Path)
    {
        FString Contents;
        FFileHelper::LoadFileToString(Contents, *Path);
        return Contents;
    }

    // Absolute path of one file under the main module's handler tree.
    FString PwCapVocabHandlerFile(const FString& RelativePath)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir() / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / RelativePath;
    }

    int32 PwCapVocabCountMatches(const FString& Contents, const FString& Pattern)
    {
        const FRegexPattern Compiled(Pattern);
        FRegexMatcher Matcher(Compiled, Contents);
        int32 Count = 0;
        while (Matcher.FindNext())
        {
            ++Count;
        }
        return Count;
    }

    // `renderer` published as a bare string literal, either shape the codebase uses:
    //   Result->SetStringField(TEXT("renderer"), TEXT("<value>"));
    //   OutCapture.Renderer = TEXT("<value>");
    // Producers that already reference PinWrightCaptureRenderer::* are correctly invisible here -
    // they are covered by the constant test instead.
    TSet<FString> PwCapVocabCollectLiteralRendererValues()
    {
        TSet<FString> Values;
        const FRegexPattern FieldPattern(
            TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"renderer\"\\s*\\)\\s*,\\s*TEXT\\(\\s*\"([A-Za-z0-9_]+)\"\\s*\\)"));
        const FRegexPattern AssignPattern(
            TEXT("\\.Renderer\\s*=\\s*TEXT\\(\\s*\"([A-Za-z0-9_]+)\"\\s*\\)"));

        for (const FString& File : PwCapVocabHandlerSourceFiles())
        {
            const FString Contents = PwCapVocabReadFile(File);
            if (Contents.IsEmpty())
            {
                continue;
            }
            FRegexMatcher FieldMatcher(FieldPattern, Contents);
            while (FieldMatcher.FindNext())
            {
                Values.Add(FieldMatcher.GetCaptureGroup(1));
            }
            FRegexMatcher AssignMatcher(AssignPattern, Contents);
            while (AssignMatcher.FindNext())
            {
                Values.Add(AssignMatcher.GetCaptureGroup(1));
            }
        }
        return Values;
    }

    // Files that publish a JSON string field literally named `projection`.
    TArray<FString> PwCapVocabFilesPublishingProjectionKey()
    {
        TArray<FString> Offenders;
        const FRegexPattern Pattern(
            TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"projection\"\\s*\\)"));
        for (const FString& File : PwCapVocabHandlerSourceFiles())
        {
            const FString Contents = PwCapVocabReadFile(File);
            if (Contents.IsEmpty())
            {
                continue;
            }
            FRegexMatcher Matcher(Pattern, Contents);
            if (Matcher.FindNext())
            {
                Offenders.AddUnique(FPaths::GetCleanFilename(File));
            }
        }
        Offenders.Sort();
        return Offenders;
    }

    // The typed, non-crashing exits a scene-capture verb is allowed to return when the host has no
    // usable RHI surface or no editor world. Anything outside this list is a real defect, so the
    // guard cannot swallow one.
    bool PwCapVocabIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("EDITOR_NOT_AVAILABLE")
            || ErrorCode == TEXT("NO_EDITOR_WORLD")
            || ErrorCode == TEXT("SCENE_BOUNDS_NOT_MEASURED")
            || ErrorCode == TEXT("SCENE_CAPTURE_FAILED")
            || ErrorCode == TEXT("CAPTURE_CAMERA_NOT_APPLIED")
            || ErrorCode == TEXT("RENDER_TARGET_CREATE_FAILED")
            || ErrorCode == TEXT("READ_PIXELS_FAILED")
            || ErrorCode == TEXT("BLANK_CAPTURE")
            || ErrorCode == TEXT("ENCODE_FAILED")
            || ErrorCode == TEXT("WRITE_FAILED");
    }

    // The three assertions that make up "this object speaks the capture vocabulary": the renderer
    // value, the projection value under the ONE key, and the absence of the superseded key. The
    // absence half is what catches a half-finished rename that writes both.
    void PwCapVocabAssertCaptureFields(FAutomationTestBase& Test, const FString& Where,
        const TSharedPtr<FJsonObject>& Obj, const TCHAR* ExpectedProjectionMode)
    {
        if (!Obj.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("%s: no object to read the capture vocabulary from"), *Where));
            return;
        }

        FString Renderer;
        if (Test.TestTrue(FString::Printf(
                TEXT("%s publishes a `renderer` at all - without it a comparison cannot refuse a cross-renderer pair"),
                *Where),
            Obj->TryGetStringField(TEXT("renderer"), Renderer)))
        {
            Test.TestEqual(FString::Printf(
                    TEXT("%s publishes renderer '%s'; anything else means the two scene-capture producers have drifted apart again"),
                    *Where, PwCapVocabSceneCapture2D),
                Renderer, FString(PwCapVocabSceneCapture2D));
        }

        FString ProjectionMode;
        if (Test.TestTrue(FString::Printf(
                TEXT("%s publishes `projectionMode`, the one spelling every other capture producer uses"), *Where),
            Obj->TryGetStringField(TEXT("projectionMode"), ProjectionMode)))
        {
            Test.TestEqual(FString::Printf(TEXT("%s reports projectionMode '%s'"),
                    *Where, ExpectedProjectionMode),
                ProjectionMode, FString(ExpectedProjectionMode));
        }

        Test.TestFalse(FString::Printf(
                TEXT("%s does NOT also publish the superseded `projection` key - two names for one field is the defect this test exists to prevent (`projection` belongs to editor.set_view_mode and means which view-mode slot to write)"),
                *Where),
            Obj->HasField(TEXT("projection")));
    }
}

// ============================================================================
// 1. The constants ARE the wire spellings, and no two renderers share one.
//    Runs on every host; needs no RHI, no editor world and no render.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabRendererSpellingsAreExactTest,
    "PinWright.render.capture_vocabulary.RendererSpellingsAreExactAndDistinct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabRendererSpellingsAreExactTest::RunTest(const FString& Parameters)
{
    // Fails on: any edit to a PinWrightCaptureRenderer constant. That is a wire-contract change -
    // it changes every stored response and the values quoted in docs/wiki-src/render.md and
    // docs/wiki-src/widget.md - so it must not be possible to make it quietly.
    TestEqual(TEXT("PinWrightCaptureRenderer::SceneCapture2D is the published spelling"),
        FString(PinWrightCaptureRenderer::SceneCapture2D), FString(PwCapVocabSceneCapture2D));
    TestEqual(TEXT("PinWrightCaptureRenderer::SceneViewportReadPixels is the published spelling"),
        FString(PinWrightCaptureRenderer::SceneViewportReadPixels), FString(PwCapVocabSceneViewportReadPixels));
    TestEqual(TEXT("PinWrightCaptureRenderer::WidgetRenderer is the published spelling"),
        FString(PinWrightCaptureRenderer::WidgetRenderer), FString(PwCapVocabWidgetRenderer));
    TestEqual(TEXT("PinWrightCaptureRenderer::SlateScreenshot is the published spelling"),
        FString(PinWrightCaptureRenderer::SlateScreenshot), FString(PwCapVocabSlateScreenshot));

    // Fails on: a copy-paste that gives two different renderers the same spelling. That is the
    // mirror of the drift bug and is worse - it makes a genuinely cross-renderer pair read as
    // same-renderer, so the comparison reports a 5.76% renderer difference as content.
    TSet<FString> Distinct;
    Distinct.Add(FString(PinWrightCaptureRenderer::SceneCapture2D));
    Distinct.Add(FString(PinWrightCaptureRenderer::SceneViewportReadPixels));
    Distinct.Add(FString(PinWrightCaptureRenderer::WidgetRenderer));
    Distinct.Add(FString(PinWrightCaptureRenderer::SlateScreenshot));
    TestEqual(TEXT("the four renderer spellings are pairwise distinct"), Distinct.Num(), 4);

    return true;
}

// ============================================================================
// 2. Every `renderer` value emitted anywhere in the handler trees is one of the four registered
//    spellings. Same shape as PinWright.core.error_codes.AllEmittedCodesAreRegistered, and the
//    same reason: a fifth spelling invented at a call site is invisible until something compares
//    two captures and gets the wrong answer.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabEmittedRendererValuesRegisteredTest,
    "PinWright.render.capture_vocabulary.EveryEmittedRendererValueIsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabEmittedRendererValuesRegisteredTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Roots = PwCapVocabHandlerRoots();
    if (!TestTrue(TEXT("found the plugin's handler source trees to scan"), Roots.Num() > 0))
    {
        return false;
    }
    // Guards the discovery itself: with only the main module found, every sub-module producer is
    // silently outside this contract and the test would pass by not looking.
    TestTrue(TEXT("the scan reaches the integration sub-module handler trees too"), Roots.Num() > 1);

    const TSet<FString> Emitted = PwCapVocabCollectLiteralRendererValues();

    // Precondition, so a broken regex cannot pass as a clean tree. These three are written as
    // literals at their call sites today (WidgetDesignerScreenshotHandler.cpp,
    // PreviewViewportCaptureUtils.cpp); adopting the constants there is a documented follow-up,
    // and when it happens this precondition is what will say so out loud.
    if (!TestTrue(FString::Printf(
            TEXT("the scan found literal renderer values to check (found %d); zero means the pattern stopped matching, not that the tree is clean"),
            Emitted.Num()),
        Emitted.Num() >= 3))
    {
        return false;
    }

    TSet<FString> Registered;
    Registered.Add(FString(PinWrightCaptureRenderer::SceneCapture2D));
    Registered.Add(FString(PinWrightCaptureRenderer::SceneViewportReadPixels));
    Registered.Add(FString(PinWrightCaptureRenderer::WidgetRenderer));
    Registered.Add(FString(PinWrightCaptureRenderer::SlateScreenshot));

    TArray<FString> Unregistered;
    for (const FString& Value : Emitted)
    {
        if (!Registered.Contains(Value))
        {
            Unregistered.Add(Value);
        }
    }
    Unregistered.Sort();
    for (const FString& Value : Unregistered)
    {
        AddError(FString::Printf(
            TEXT("A handler publishes renderer '%s', which is not one of the spellings declared in ")
            TEXT("Handlers/Render/CaptureRendererNames.h. Either it is a new renderer and needs a ")
            TEXT("constant plus a doc line, or it is a second spelling of an existing one - which is ")
            TEXT("the defect that made a same-renderer pair read as cross-renderer."), *Value));
    }
    return TestEqual(TEXT("every emitted renderer value is a registered spelling"), Unregistered.Num(), 0);
}

// ============================================================================
// 3. The two scene-capture producers reference the constant rather than re-spelling it, and the
//    ortho verb publishes the projection concept as `projectionMode`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabSceneCaptureProducersShareOneConstantTest,
    "PinWright.render.capture_vocabulary.SceneCaptureProducersShareOneConstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabSceneCaptureProducersShareOneConstantTest::RunTest(const FString& Parameters)
{
    const FString OrthoPath = PwCapVocabHandlerFile(TEXT("Render/OrthoTileCaptureHandler.cpp"));
    const FString ZFightPath = PwCapVocabHandlerFile(TEXT("Render/ZFightingHandler.cpp"));
    const FString Ortho = PwCapVocabReadFile(OrthoPath);
    const FString ZFight = PwCapVocabReadFile(ZFightPath);
    if (!TestTrue(TEXT("read OrthoTileCaptureHandler.cpp"), !Ortho.IsEmpty())
        || !TestTrue(TEXT("read ZFightingHandler.cpp"), !ZFight.IsEmpty()))
    {
        return false;
    }

    const FString ConstantWrite =
        TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"renderer\"\\s*\\)\\s*,\\s*PinWrightCaptureRenderer::SceneCapture2D\\s*\\)");
    const FString LiteralWrite =
        TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"renderer\"\\s*\\)\\s*,\\s*TEXT\\(");

    // Fails on: either producer going back to a literal. That is precisely how the two drifted -
    // one file said "sceneCapture2D", the other "sceneCaptureComponent2D", for one component.
    TestEqual(TEXT("render.capture_ortho_tiles writes `renderer` from the shared constant, in both the manifest and the response"),
        PwCapVocabCountMatches(Ortho, ConstantWrite), 2);
    TestEqual(TEXT("render.capture_ortho_tiles writes no `renderer` string literal"),
        PwCapVocabCountMatches(Ortho, LiteralWrite), 0);
    TestEqual(TEXT("render.detect_z_fighting writes `renderer` from the shared constant"),
        PwCapVocabCountMatches(ZFight, ConstantWrite), 1);
    TestEqual(TEXT("render.detect_z_fighting writes no `renderer` string literal"),
        PwCapVocabCountMatches(ZFight, LiteralWrite), 0);

    // The projection concept, on the producer that used to spell it differently. Fails on a revert
    // of the rename, and on a half-revert that writes both keys.
    const FString ProjectionModeWrite =
        TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"projectionMode\"\\s*\\)");
    const FString ProjectionWrite =
        TEXT("SetStringField\\s*\\(\\s*TEXT\\(\\s*\"projection\"\\s*\\)");
    TestEqual(TEXT("render.capture_ortho_tiles publishes `projectionMode` in both the manifest and the response"),
        PwCapVocabCountMatches(Ortho, ProjectionModeWrite), 2);
    TestEqual(TEXT("render.capture_ortho_tiles publishes no `projection` key"),
        PwCapVocabCountMatches(Ortho, ProjectionWrite), 0);
    TestEqual(TEXT("render.detect_z_fighting publishes `projectionMode`"),
        PwCapVocabCountMatches(ZFight, ProjectionModeWrite), 1);
    TestEqual(TEXT("render.detect_z_fighting publishes no `projection` key"),
        PwCapVocabCountMatches(ZFight, ProjectionWrite), 0);

    return true;
}

// ============================================================================
// 4. `projection` is reserved for editor.set_view_mode, tree-wide.
//
//    The two fields are genuinely different and must not share a name: on the capture verbs the
//    value is perspective/orthographic (what the camera did), while on editor.set_view_mode it
//    names WHICH of the viewport client's two view-mode slots to write - both / perspective /
//    orthographic / active (docs/wiki-src/editor.md). Reading one as the other is silent and
//    plausible in both directions, which is why the allow-list is exactly one file rather than a
//    convention.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabProjectionKeyIsReservedTest,
    "PinWright.render.capture_vocabulary.ProjectionKeyBelongsOnlyToSetViewMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabProjectionKeyIsReservedTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Publishers = PwCapVocabFilesPublishingProjectionKey();

    // Precondition. The one legitimate publisher MUST be found: if it is not, the scan is broken
    // and an empty offender list below would be a vacuous pass rather than a clean tree.
    if (!TestTrue(FString::Printf(
            TEXT("the scan found ViewportHandler.cpp, the one file allowed to publish `projection` (found %d publisher(s) in total)"),
            Publishers.Num()),
        Publishers.Contains(TEXT("ViewportHandler.cpp"))))
    {
        return false;
    }

    TArray<FString> Offenders;
    for (const FString& File : Publishers)
    {
        if (File != TEXT("ViewportHandler.cpp"))
        {
            Offenders.Add(File);
        }
    }
    for (const FString& File : Offenders)
    {
        AddError(FString::Printf(
            TEXT("%s publishes a JSON field named `projection`. On a capture verb the ")
            TEXT("perspective/orthographic concept is spelled `projectionMode` - that is what the ")
            TEXT("other response writers and every wiki page use. `projection` means something else ")
            TEXT("entirely (editor.set_view_mode's view-mode slot selector), so the two cannot ")
            TEXT("share a name."), *File));
    }
    return TestEqual(TEXT("no capture producer publishes `projection`"), Offenders.Num(), 0);
}

// ============================================================================
// 5. Live corroboration: the values reach the wire.
//
//    Each producer is compared against the SAME literal rather than against the other, so one
//    branch skipping for want of an RHI surface does not disarm the other half of the pair.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabOrthoTilesPublishTheValuesTest,
    "PinWright.render.capture_vocabulary.OrthoTilesPublishTheSceneCaptureValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabOrthoTilesPublishTheValuesTest::RunTest(const FString& Parameters)
{
    const FString Prefix = FString::Printf(TEXT("pw_capvocab_ortho_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Short));

    TSharedPtr<FJsonObject> Min = MakeShared<FJsonObject>();
    Min->SetNumberField(TEXT("x"), -2000.0);
    Min->SetNumberField(TEXT("y"), -2000.0);
    Min->SetNumberField(TEXT("z"), 0.0);
    TSharedPtr<FJsonObject> Max = MakeShared<FJsonObject>();
    Max->SetNumberField(TEXT("x"), 2000.0);
    Max->SetNumberField(TEXT("y"), 2000.0);
    Max->SetNumberField(TEXT("z"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("worldMin"), Min);
    Payload->SetObjectField(TEXT("worldMax"), Max);
    Payload->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
    Payload->SetNumberField(TEXT("exposure"), 11.0);
    Payload->SetNumberField(TEXT("cols"), 1.0);
    Payload->SetNumberField(TEXT("rows"), 1.0);
    // A single 64 px tile. This test asserts published field values, which no resolution can
    // change, so unlike TestZFightingDetect's fixture there is nothing here for a small frame to
    // hide. Kept small so the vocabulary guard costs the suite almost nothing.
    Payload->SetNumberField(TEXT("tilePixels"), 64.0);
    Payload->SetStringField(TEXT("namePrefix"), Prefix);
    Payload->SetBoolField(TEXT("overwrite"), true);
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_ortho_tiles handler is registered"),
        InvokeHandlerWithCapture(TEXT("render.capture_ortho_tiles"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("the handler sent a response"), Capture.bWasCalled);

    FString OutputDir;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("outputDir"), OutputDir);
    }

    if (!Capture.bSuccess)
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the orthographic capture was REFUSED with %s, so the wire assertions are SKIPPED here. ")
            TEXT("The vocabulary contract itself is still enforced by the source scans in this file, which do not ")
            TEXT("render. Message: %s"), *Capture.ErrorCode, *Capture.Message));
        TestTrue(FString::Printf(TEXT("the refusal is a typed capture failure (got '%s')"), *Capture.ErrorCode),
            PwCapVocabIsTypedCaptureFailure(Capture.ErrorCode));
        if (!OutputDir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*OutputDir, false, true);
        }
        return true;
    }

    AddInfo(TEXT("BRANCH: the orthographic capture RAN - the response and on-disk manifest assertions are live."));
    PwCapVocabAssertCaptureFields(*this, TEXT("the render.capture_ortho_tiles response"),
        Capture.Result, TEXT("orthographic"));

    // The manifest matters more than the response: it is the artifact image.tile / image.annotate /
    // image.compare read back later, so a manifest written with the old key would outlive the run
    // that made it. Read from DISK, not from the object just built in memory.
    FString ManifestPath;
    if (TestTrue(TEXT("a successful capture publishes its manifest path"),
        Capture.Result->TryGetStringField(TEXT("manifest"), ManifestPath) && !ManifestPath.IsEmpty()))
    {
        FString Text;
        if (TestTrue(FString::Printf(TEXT("the manifest is readable at %s"), *ManifestPath),
            FFileHelper::LoadFileToString(Text, *ManifestPath)))
        {
            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            if (TestTrue(TEXT("the manifest parses as a JSON object"),
                FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()))
            {
                PwCapVocabAssertCaptureFields(*this, TEXT("the on-disk ortho tile manifest"),
                    Root, TEXT("orthographic"));
            }
        }
    }

    if (!OutputDir.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*OutputDir, false, true);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureVocabZFightingPublishesTheValuesTest,
    "PinWright.render.capture_vocabulary.ZFightingPublishesTheSceneCaptureValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureVocabZFightingPublishesTheValuesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0.0);
    Location->SetNumberField(TEXT("y"), 0.0);
    Location->SetNumberField(TEXT("z"), 1000.0);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), -30.0);
    Rotation->SetNumberField(TEXT("yaw"), 0.0);
    Rotation->SetNumberField(TEXT("roll"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // An explicit pose, so the assertion does not depend on whatever the viewport camera happens
    // to be pointing at when the suite reaches this test.
    Payload->SetObjectField(TEXT("location"), Location);
    Payload->SetObjectField(TEXT("rotation"), Rotation);
    // 256 px and no mask. TestZFightingDetect.cpp deliberately runs its fixture at the production
    // 1920 because a small frame can report ZERO affected pixels on a scene that genuinely fights -
    // that warning is about the pixel population, and nothing here reads it. This test asserts
    // three published strings, which the resolution cannot change.
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetBoolField(TEXT("mask"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.detect_z_fighting handler is registered"),
        InvokeHandlerWithCapture(TEXT("render.detect_z_fighting"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("the handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess)
    {
        AddInfo(FString::Printf(
            TEXT("BRANCH: the z-fighting probe was REFUSED with %s, so the wire assertions are SKIPPED here. ")
            TEXT("The vocabulary contract itself is still enforced by the source scans in this file, which do not ")
            TEXT("render. Message: %s"), *Capture.ErrorCode, *Capture.Message));
        return TestTrue(FString::Printf(TEXT("the refusal is a typed capture failure (got '%s')"), *Capture.ErrorCode),
            PwCapVocabIsTypedCaptureFailure(Capture.ErrorCode));
    }

    AddInfo(TEXT("BRANCH: the z-fighting probe RAN - the response assertions are live."));
    // Perspective, not orthographic: this probe always renders a perspective frame, and that is
    // the value a caller pairs against another capture's.
    PwCapVocabAssertCaptureFields(*this, TEXT("the render.detect_z_fighting response"),
        Capture.Result, TEXT("perspective"));
    return true;
}
