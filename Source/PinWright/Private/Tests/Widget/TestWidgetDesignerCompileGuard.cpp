// Copyright (c) 2026 Alexander Penkin. MIT License.

// Board B-screenshot-designer-leaves-designer-open-compile-crash: a widget.screenshot_designer
// left the UMG Designer open, and the next blueprint.compile_bpir on that widget rebuilt its class
// under the live preview, killing the editor on the following Slate paint
// (UUserWidget::RebuildWidget -> SDesignerView::UpdatePreviewWidget, access violation at 0x38).
//
// Two independent defects, tested separately here:
//   1. the COMPILE never tore the Designer preview down first, which the engine's own
//      FWidgetBlueprintEditor::Compile always does (WidgetBlueprintEditor.cpp:1799-1803);
//   2. the CAPTURE verb opened an asset editor it never closed.
//
// Counterfactuals. Remove the RegisterPreCompileGuard() call from PinWrightModule.cpp and
// PreCompileHookRunsBeforeTheCompile fails, because a real compile of a widget with its Designer
// open then moves the jettison counter by 0 instead of 1. Remove the FScopedDesignerAssetEditor
// from WidgetDesignerScreenshotHandler.cpp and ClosesDesignerItOpened fails on
// assetEditorCloseDeferred. Make that scope close unconditionally and
// LeavesPreexistingDesignerOpen fails, because a tab the caller already had open is theirs.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/UI/WidgetDesignerCompileGuard.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

// Fixture helpers stay fully qualified below: a file-scope `using` leaks into every other test
// file sharing this unity blob.
namespace
{
    UAssetEditorSubsystem* PwDesignerGuardAssetEditorSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    }

    // Opens the Widget Blueprint editor and returns the toolkit with a live preview, or null.
    FWidgetBlueprintEditor* PwDesignerGuardOpenEditorWithPreview(UWidgetBlueprint* WBP)
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = PwDesignerGuardAssetEditorSubsystem();
        if (!AssetEditorSubsystem || !WBP || !AssetEditorSubsystem->OpenEditorForAsset(WBP))
        {
            return nullptr;
        }
        FWidgetBlueprintEditor* WidgetEditor =
            FWidgetGeometryResolver::FindWidgetBlueprintEditor(WBP, /*bFocusIfOpen=*/false);
        if (WidgetEditor && WidgetEditor->GetPreview() == nullptr)
        {
            // The toolkit builds the preview on its own tick; RefreshPreview is the engine's
            // public "do it now" (WidgetBlueprintEditor.cpp:1786).
            WidgetEditor->RefreshPreview();
        }
        return WidgetEditor;
    }

    void PwDesignerGuardCloseEditors(UWidgetBlueprint* WBP)
    {
        if (UAssetEditorSubsystem* AssetEditorSubsystem = PwDesignerGuardAssetEditorSubsystem())
        {
            if (WBP)
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
            }
        }
    }

    // Closes every Widget Blueprint editor so a test starts from the genuine cold-open path.
    void PwDesignerGuardCloseAllWidgetEditors()
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = PwDesignerGuardAssetEditorSubsystem();
        if (!AssetEditorSubsystem)
        {
            return;
        }
        for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
        {
            if (UWidgetBlueprint* OpenWBP = Cast<UWidgetBlueprint>(Asset))
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(OpenWBP);
            }
        }
    }
}

// The hook is what makes the guard reach compiles the plugin does not route itself: the engine
// broadcasts it once per blueprint immediately before the class is purged
// (BlueprintCompilationManager.cpp:1361-1367). A one-line registration is otherwise unobservable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerCompileGuardRegisteredTest,
    "PinWright.widget.designer_compile_guard.HookRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerCompileGuardRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("pre-compile designer guard is subscribed to GEditor->OnBlueprintPreCompile"),
        WidgetDesignerCompileGuard::IsPreCompileGuardRegistered());
    return true;
}

// The guard declines everything that is not "a Widget Blueprint with a live Designer preview",
// because a guard that fires elsewhere would destroy state nobody asked it to.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerCompileGuardDeclinesUnaffectedTest,
    "PinWright.widget.designer_compile_guard.DeclinesUnaffected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerCompileGuardDeclinesUnaffectedTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("null blueprint is declined"),
        WidgetDesignerCompileGuard::JettisonDesignerPreviewBeforeCompile(nullptr));
    TestFalse(TEXT("null widget editor is declined"),
        WidgetDesignerCompileGuard::JettisonDesignerPreview(nullptr));

    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_GuardDeclines"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        PwDesignerGuardCloseEditors(WBP);
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    PwDesignerGuardCloseAllWidgetEditors();
    TestFalse(TEXT("a widget blueprint with no open editor is declined"),
        WidgetDesignerCompileGuard::JettisonDesignerPreviewBeforeCompile(WBP));
    return true;
}

// The primitive: it destroys the live preview the way FWidgetBlueprintEditor::DestroyPreview does,
// and says so only once. GetPreview() reading null afterwards is the whole point — that is what
// SDesignerView::UpdatePreviewWidget consults, and a null there paints "No Widget Preview" instead
// of dereferencing a WidgetTree the compile is about to null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerCompileGuardJettisonsPreviewTest,
    "PinWright.widget.designer_compile_guard.JettisonsLivePreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerCompileGuardJettisonsPreviewTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_GuardJettison"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }
    WidgetTestFixtures::AddSizedPreviewLabel(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    ON_SCOPE_EXIT
    {
        PwDesignerGuardCloseEditors(WBP);
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    FWidgetBlueprintEditor* WidgetEditor = PwDesignerGuardOpenEditorWithPreview(WBP);
    TestNotNull(TEXT("widget blueprint editor opened"), WidgetEditor);
    if (!WidgetEditor)
    {
        return false;
    }
    TestNotNull(TEXT("designer preview is live before the guard runs"), WidgetEditor->GetPreview());
    if (!WidgetEditor->GetPreview())
    {
        return false;
    }

    TestTrue(TEXT("guard reports it tore a preview down"),
        WidgetDesignerCompileGuard::JettisonDesignerPreviewBeforeCompile(WBP));
    TestNull(TEXT("designer preview is gone after the guard"), WidgetEditor->GetPreview());
    TestFalse(TEXT("guard is idempotent: nothing left to tear down"),
        WidgetDesignerCompileGuard::JettisonDesignerPreviewBeforeCompile(WBP));
    return true;
}

// End to end through the engine's own broadcast: a REAL compile of a widget whose Designer is open
// must run the guard, and a real compile of one whose Designer is closed must not.
//
// THE OBVIOUS TEST FOR THIS IS UNWRITEABLE, and the reason is worth stating because it cost a red
// suite. Watching the guard's effect from a second pre-compile listener cannot work in either
// direction: TMulticastDelegateBase::Broadcast walks its invocation list BACKWARDS — "call bound
// functions in reverse order, so we ignore any instances that may be added by callees" (UE 5.8
// Core/Public/Delegates/MulticastDelegateBase.h:299-300) — while AddDelegateInstance APPENDS
// (:336). The LAST listener registered therefore runs FIRST, so an observer added at test time
// always executes BEFORE the guard registered at module startup and always sees a live preview,
// whether the guard works or not. That assertion had no discriminating power at all; the counter
// below does, and it is order-free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerCompileGuardRunsBeforeCompileTest,
    "PinWright.widget.designer_compile_guard.PreCompileHookRunsBeforeTheCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerCompileGuardRunsBeforeCompileTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_GuardPreCompile"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !GEditor)
    {
        return false;
    }
    WidgetTestFixtures::AddSizedPreviewLabel(WBP);

    ON_SCOPE_EXIT
    {
        PwDesignerGuardCloseEditors(WBP);
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    TestTrue(TEXT("guard is subscribed, so a compile can reach it"),
        WidgetDesignerCompileGuard::IsPreCompileGuardRegistered());

    // NEGATIVE CONTROL FIRST, on the same asset the positive case uses, so the two runs differ in
    // exactly one thing: whether this widget's Designer is open. No close is needed and none is
    // attempted — the asset was created moments ago under a GUID-unique path, so no editor has
    // ever been opened for it, which makes this free of the close-is-not-instant timing that the
    // sibling designer tests have to tick-wait around.
    const int64 BeforeColdCompile =
        static_cast<int64>(WidgetDesignerCompileGuard::GetJettisonedPreviewCount());
    FKismetEditorUtilities::CompileBlueprint(WBP);
    TestEqual(TEXT("a compile with no Designer open tears nothing down"),
        static_cast<int64>(WidgetDesignerCompileGuard::GetJettisonedPreviewCount()),
        BeforeColdCompile);

    FWidgetBlueprintEditor* WidgetEditor = PwDesignerGuardOpenEditorWithPreview(WBP);
    TestNotNull(TEXT("widget blueprint editor opened"), WidgetEditor);
    if (!WidgetEditor)
    {
        return false;
    }
    TestNotNull(TEXT("designer preview is live before the compile"), WidgetEditor->GetPreview());
    if (!WidgetEditor->GetPreview())
    {
        return false;
    }

    const int64 BeforeOpenCompile =
        static_cast<int64>(WidgetDesignerCompileGuard::GetJettisonedPreviewCount());
    FKismetEditorUtilities::CompileBlueprint(WBP);

    // Exactly one: the compile reached the guard once, for this blueprint. A compile that never
    // reached the pre-compile hook, or a guard that failed to resolve the open editor, leaves this
    // at BeforeOpenCompile — which is precisely the crash precondition, a live preview carried into
    // the class rebuild.
    TestEqual(TEXT("a compile with the Designer open ran the guard exactly once"),
        static_cast<int64>(WidgetDesignerCompileGuard::GetJettisonedPreviewCount()),
        BeforeOpenCompile + 1);
    return true;
}

// The capture side: a Designer this verb opened is put back, and the response says so with the
// same three fields render.capture_asset_preview publishes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerClosesDesignerItOpenedTest,
    "PinWright.widget.screenshot_designer.ClosesDesignerItOpened",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerClosesDesignerItOpenedTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_CaptureClosesDesigner"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }
    WidgetTestFixtures::AddSizedPreviewLabel(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        PwDesignerGuardCloseEditors(WBP);
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UAssetEditorSubsystem* AssetEditorSubsystem = PwDesignerGuardAssetEditorSubsystem();
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }
    PwDesignerGuardCloseAllWidgetEditors();
    PinWrightCaptureSubject::FlushDeferredAssetEditorCloses();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("preview"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        // A host that cannot realize the Designer (no Slate, no preview bounds) has nothing to
        // say about the close policy; the state assertion below would be vacuous.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("designer-capture-unavailable"),
            TEXT("Designer capture unavailable on this host; close-policy assertions skipped."));
        return true;
    }
    Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

    TestFalse(TEXT("the capture opened the designer itself"),
        Capture.Result->GetBoolField(TEXT("assetEditorWasAlreadyOpen")));
    // Never closed on the capture's own stack — destroying a toolkit there is its own crash — so
    // the honest pair is closed:false + deferred:true.
    TestFalse(TEXT("close is not claimed as already done"),
        Capture.Result->GetBoolField(TEXT("assetEditorClosed")));
    TestTrue(TEXT("close is queued for the next editor tick"),
        Capture.Result->GetBoolField(TEXT("assetEditorCloseDeferred")));
    TestTrue(TEXT("the queue actually holds this asset"),
        PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(WidgetPath));

    PinWrightCaptureSubject::FlushDeferredAssetEditorCloses();
    TestNull(TEXT("designer is gone once the queued close runs"),
        AssetEditorSubsystem->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false));
    return true;
}

// The other half of the three-state rule: a Designer the caller already had open is theirs, and a
// read-only capture must not close it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerLeavesPreexistingDesignerOpenTest,
    "PinWright.widget.screenshot_designer.LeavesPreexistingDesignerOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerLeavesPreexistingDesignerOpenTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_CaptureKeepsDesigner"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }
    WidgetTestFixtures::AddSizedPreviewLabel(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        PwDesignerGuardCloseEditors(WBP);
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UAssetEditorSubsystem* AssetEditorSubsystem = PwDesignerGuardAssetEditorSubsystem();
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }
    PwDesignerGuardCloseAllWidgetEditors();
    PinWrightCaptureSubject::FlushDeferredAssetEditorCloses();

    // The caller's own window, opened before the verb runs.
    TestNotNull(TEXT("designer opened by the caller"), PwDesignerGuardOpenEditorWithPreview(WBP));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("preview"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("designer-capture-unavailable"),
            TEXT("Designer capture unavailable on this host; close-policy assertions skipped."));
        return true;
    }
    Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

    TestTrue(TEXT("the capture found the designer already open"),
        Capture.Result->GetBoolField(TEXT("assetEditorWasAlreadyOpen")));
    TestFalse(TEXT("a pre-existing designer is not queued for close"),
        Capture.Result->GetBoolField(TEXT("assetEditorCloseDeferred")));
    TestFalse(TEXT("the queue holds nothing for this asset"),
        PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(WidgetPath));

    PinWrightCaptureSubject::FlushDeferredAssetEditorCloses();
    TestNotNull(TEXT("the caller's designer is still open"),
        AssetEditorSubsystem->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false));
    return true;
}
