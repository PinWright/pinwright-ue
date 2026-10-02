// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-drive-chrome-blank-capture-passes: the editor-chrome Set-of-Mark capture must reach the
// renderer's IsBlankReadback with its alpha as read back. FDriveEditorChrome::CaptureWindow used to
// stamp ForceOpaqueAlpha before returning, so every alpha byte was 0xFF by the time the renderer's
// blank check ran and a never-drawn window passed as a black frame with success:true.
//
// A source contract, not a live capture: no headless configuration makes Slate hand back an
// all-zero window readback on demand (an undrawn window fails TakeScreenshot instead), so the
// ordering is pinned where it is decided.

#include "Misc/AutomationTest.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Tests/TestUtils.h"

namespace DriveEditorChromeBlankCaptureTest
{
    FString LoadPluginSource(FAutomationTestBase& Test, const TCHAR* RelativePath)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        FString Source;
        Test.TestTrue(*FString::Printf(TEXT("source loads: %s"), RelativePath),
            Plugin.IsValid() && FFileHelper::LoadFileToString(Source, *(Plugin->GetBaseDir() / RelativePath)));
        return NeutralizeSourceText(Source.Replace(TEXT("\r\n"), TEXT("\n")));
    }

    // Text from the function's definition marker up to the next top-level closing brace.
    FString FunctionBody(const FString& Source, const TCHAR* DefinitionMarker)
    {
        const int32 Start = Source.Find(DefinitionMarker);
        const int32 End = Start == INDEX_NONE
            ? INDEX_NONE
            : Source.Find(TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
        return End == INDEX_NONE ? FString() : Source.Mid(Start, End - Start);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveEditorChromeCaptureLeavesAlphaForBlankCheckTest,
    "PinWright.drive.editorchrome.CaptureLeavesAlphaForBlankCheck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveEditorChromeCaptureLeavesAlphaForBlankCheckTest::RunTest(const FString& Parameters)
{
    using namespace DriveEditorChromeBlankCaptureTest;

    const FString CaptureWindow = FunctionBody(
        LoadPluginSource(*this, TEXT("Source/PinWright/Private/Handlers/Drive/DriveEditorChrome.cpp")),
        TEXT("bool FDriveEditorChrome::CaptureWindow("));
    TestTrue(TEXT("CaptureWindow body found"), CaptureWindow.Contains(TEXT("TakeSlateScreenshot")));
    TestFalse(TEXT("CaptureWindow returns the readback without stamping alpha"),
        CaptureWindow.Contains(TEXT("ForceOpaqueAlpha")));

    const FString Annotate = FunctionBody(
        LoadPluginSource(*this, TEXT("Source/PinWright/Private/Handlers/Drive/DriveSetOfMarkRenderer.cpp")),
        TEXT("bool FDriveSetOfMarkRenderer::CaptureAnnotated("));
    const int32 ChromeCapture = Annotate.Find(TEXT("FDriveEditorChrome::CaptureWindow("));
    const int32 BlankCheck = Annotate.Find(TEXT("IsBlankReadback("));
    const int32 AlphaStamp = Annotate.Find(TEXT("ForceOpaqueAlpha("));
    TestTrue(TEXT("CaptureAnnotated checks the chrome readback for blank before stamping alpha"),
        ChromeCapture != INDEX_NONE && BlankCheck > ChromeCapture && AlphaStamp > BlankCheck);
    return true;
}
