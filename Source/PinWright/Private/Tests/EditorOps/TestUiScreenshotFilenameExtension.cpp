// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-ui-screenshot-doubles-png-extension: ui.screenshot must
// append the .png extension only when the caller's filename does not already end in
// it (case-insensitive), so "foo.png" -> "foo.png" and NEVER the doubled
// "foo.png.png" the inline `Filename + TEXT(".png")` composition produced.
//
// It exercises PinWrightScreenshotUtils::MakeUiScreenshotPath — the exact path/filename
// composition the ui.screenshot handler now delegates to (UiHandler.cpp), replacing the
// inline unconditional append. This is pure string composition (no viewport, no asset,
// no disk fixture), so it runs and asserts in the headless suite where ui.screenshot's
// live-capture path cannot — the doubling defect lived in the filename composition, well
// before the viewport capture, so composing the path is the faithful production seam.
//
// Counterfactual: reverting MakeUiScreenshotPath to the old `RequestedFilename + ".png"`
// append makes the ".png" input resolve to "hud_final_state_ui.png.png" and the
// extensionless input echo without its ".png" — both assertions below then fail.
#include "Misc/AutomationTest.h"
#include "Utils/ScreenshotUtils.h"

#include "Containers/Set.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiScreenshotFilenameExtensionTest,
    "PinWright.ui.screenshot.FilenameExtensionNotDoubled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiScreenshotFilenameExtensionTest::RunTest(const FString& Parameters)
{
    // The reported defect: a filename already ending in .png must resolve to a
    // single-extension file on disk — never doubled to .png.png. This is the exact
    // case the reverted inline append wrote wrong.
    {
        FString OutFilename;
        const FString FullPath = PinWrightScreenshotUtils::MakeUiScreenshotPath(
            FString(), TEXT("hud_final_state_ui.png"), OutFilename);
        TestEqual(TEXT("on-disk basename keeps a single .png (not .png.png)"),
            FPaths::GetCleanFilename(FullPath), FString(TEXT("hud_final_state_ui.png")));
        TestEqual(TEXT("echoed filename matches the on-disk basename"),
            OutFilename, FString(TEXT("hud_final_state_ui.png")));
        TestFalse(TEXT("composed path carries no doubled .png.png extension"),
            FullPath.EndsWith(TEXT(".png.png"), ESearchCase::IgnoreCase));
    }

    // Case-insensitive: "shot.PNG" already carries the extension, so it is recognized
    // as present and not doubled (nor lowercased).
    {
        FString OutFilename;
        const FString FullPath = PinWrightScreenshotUtils::MakeUiScreenshotPath(
            FString(), TEXT("shot.PNG"), OutFilename);
        TestEqual(TEXT("case-varied .PNG basename is not doubled"),
            FPaths::GetCleanFilename(FullPath), FString(TEXT("shot.PNG")));
    }

    // Append-when-absent: an extensionless filename still gains exactly one .png, and
    // the echoed filename reflects the resolved on-disk basename.
    {
        FString OutFilename;
        const FString FullPath = PinWrightScreenshotUtils::MakeUiScreenshotPath(
            FString(), TEXT("tuning_screen_improved"), OutFilename);
        TestEqual(TEXT("extensionless basename gains a single .png"),
            FPaths::GetCleanFilename(FullPath), FString(TEXT("tuning_screen_improved.png")));
        TestEqual(TEXT("echoed filename gains the .png too"),
            OutFilename, FString(TEXT("tuning_screen_improved.png")));
    }

    // ui.screenshot uniquely honors a caller-supplied directory (unlike the canonical
    // editor.screenshot, which forces Saved/Screenshots). The composed path must
    // preserve it — a fix that wrongly routed through MakeScreenshotOutputPath would
    // drop it for the forced directory.
    {
        FString OutFilename;
        const FString FullPath = PinWrightScreenshotUtils::MakeUiScreenshotPath(
            TEXT("MyShots/Sub"), TEXT("frame.png"), OutFilename);
        TestEqual(TEXT("basename resolves cleanly under a custom directory"),
            FPaths::GetCleanFilename(FullPath), FString(TEXT("frame.png")));
        TestTrue(TEXT("caller-supplied directory is preserved in the composed path"),
            FullPath.Contains(TEXT("MyShots")));
    }

    return true;
}

// The other half of the filename contract, and the one that cost real measurements: a GENERATED
// name must be unique, a SUPPLIED name must not be.
//
// WHAT THIS DEFENDS. MakeScreenshotFilename's generated branch carried a `%Y%m%d_%H%M%S` stamp and
// nothing else. A viewport capture takes ~60 ms, so two captures issued back to back composed the
// SAME path; the second write overwrote the first and both handlers returned success with a `path`
// field, one of which named a file holding the other's pixels. Nothing failed and nothing warned -
// the damage landed downstream, on every comparison of "shot A against shot B". It is why
// PinWright.render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance reported a
// same-EV100 difference of exactly 0.000 over 0/16384 px across every historical run: it was
// comparing one file with itself, and its "the headless preview scene is black" calibration was
// measured off the aliased files.
//
// Pure string composition plus one FileExists probe - no viewport, no RHI, so it asserts in the
// headless suite. Counterfactual: restore the second-resolution stamp and the first block below
// fails, because two calls inside one second produce one name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScreenshotGeneratedFilenameIsUniqueTest,
    "PinWright.editor.screenshot.GeneratedFilenameIsUnique",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScreenshotGeneratedFilenameIsUniqueTest::RunTest(const FString& Parameters)
{
    // Back-to-back generated names, which is exactly the shape a capture pair produces.
    {
        TSet<FString> Seen;
        for (int32 Shot = 0; Shot < 8; ++Shot)
        {
            FString OutFilename;
            const FString FullPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
                FString(), TEXT("PwUniqueProbe"), TEXT("PwUniqueProbe"), OutFilename);
            TestFalse(FString::Printf(
                TEXT("generated name %d is not one already handed out (%s)"), Shot, *OutFilename),
                Seen.Contains(FullPath));
            Seen.Add(FullPath);
        }
        // A loop that generated nothing would satisfy every assertion above.
        TestEqual(TEXT("all eight generated names are distinct"), Seen.Num(), 8);
    }

    // ...and the contract that must NOT change: a caller-supplied name is honoured exactly, every
    // time. PoseListCapture's throwaway warm-up frame and the ortho tile grid both re-write a
    // fixed name on purpose, so uniquifying a supplied name would break them.
    {
        FString FirstName;
        FString SecondName;
        const FString FirstPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
            TEXT("pw_fixed_probe.png"), TEXT("PwUniqueProbe"), TEXT("PwUniqueProbe"), FirstName);
        const FString SecondPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
            TEXT("pw_fixed_probe.png"), TEXT("PwUniqueProbe"), TEXT("PwUniqueProbe"), SecondName);
        TestEqual(TEXT("a supplied name resolves to the same basename twice"),
            FirstName, SecondName);
        TestEqual(TEXT("a supplied name resolves to the same path twice"), FirstPath, SecondPath);
        TestEqual(TEXT("the supplied basename is used verbatim"),
            FPaths::GetCleanFilename(FirstPath), FString(TEXT("pw_fixed_probe.png")));
    }
    return true;
}
