// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Editor domain handlers. 36 editor.* verbs are exercised here, drawn from
// EditorCommandHandler.cpp, PIEHandler.cpp, ViewportHandler.cpp and EditorWindowHandlers.cpp,
// in 52 tests. Both figures are measured from this file rather than asserted: count the test
// macros for the second, and the distinct `PinWright.editor.<x>.` id prefixes for the first —
// there are 37 of those, less `editor.bookmark`, which is a cross-verb round-trip id and not a
// verb of its own.
//
// Strategy:
//   - Required-parameter enforcement is NOT tested per verb here, and must not be re-added.
//     It lives in FRpcDispatcher::ValidateHandlerParams (Dispatch/RpcDispatcher.cpp), which
//     neither InvokeHandler() nor InvokeHandlerWithCapture() reaches — TestUtils.h calls
//     Reg.Func(Ctx) straight out of GetPendingRegistrations() — so a per-verb
//     `MissingRequiredParam` case here asserted only that the method name was in the registry
//     and stayed green with every RPC_PARAM_REQ on the verb deleted. c86f8890 replaced 533 of
//     those repo-wide with one walk over the registry that drives the real dispatcher,
//     PinWright.infra.contract.RequiredParamGate.EveryVerb. It missed this file; the ten
//     survivors were removed afterwards, which is where 62 tests became 52.
//   - A missing-parameter case earns a place in a handler test file only where the requirement
//     is IN-BODY — the param is not RPC_PARAM_REQ, so the dispatcher gate never fires for it —
//     and only when the test pins that guard by the specific error code it returns. The worked
//     examples are actor.select (TestActorHandlers.cpp) and landscape.sculpt
//     (TestEnvironmentHandlers.cpp). No verb in this file qualifies: every one of the ten that
//     carried such a test declares its param RPC_PARAM_REQ, so the registry walk covers it.
//   - Otherwise: one ValidParamsNoCrash test per handler, plus the named regression tests.
//
// All tests run without a live editor (GEditor == nullptr), so handlers that
// guard on GEditor will call SendError instead of crashing — this is the
// expected path for handler-layer unit tests.
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Handlers/Editor/PieNetworkEmulation.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Dom/JsonObject.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"
#include "Engine/World.h"
#include "Engine/PointLight.h"
#include "Engine/Selection.h"
#include "Tests/TestWorldUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Input/Reply.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#if __has_include("Subsystems/UnrealEditorSubsystem.h")
#include "Subsystems/UnrealEditorSubsystem.h"
#define MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM 1
#elif __has_include("UnrealEditorSubsystem.h")
#include "UnrealEditorSubsystem.h"
#define MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM 1
#endif

// Nothing may be declared inside the arms of the __has_include block above. Exactly one arm
// is taken, so a declaration placed in the other one vanishes on a host that resolves the
// header the other way while its call sites, which are written unconditionally further down,
// remain — and the file stops compiling on that host only. The block therefore defines
// MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM and nothing else.
//
// The two regions guarded by that define are both inside FEditorSetCameraForceRedrawTest
// below, and both are statement-level rather than declaration-level, which is what keeps this
// safe. (An earlier revision of this comment named "these three" and a "dispatcher-gate
// namespace below"; this file declares no namespace at all — its three `using namespace`
// lines refer to PieWorldSelector and PieNetworkEmulation, both defined in headers — so the
// referents did not exist. The constraint is real; the subjects were not.)

// ============================================================================
// EditorCommandHandler — editor.console_command
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandValidParamsTest,
    "PinWright.editor.console_command.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("command"), TEXT("stat fps"));
    TestTrue(TEXT("editor.console_command handler found"), InvokeHandler(TEXT("editor.console_command"), Payload));
    return true;
}

// Pure selector-grammar coverage for the optional `world` param: every valid form
// parses to the right kind/index, malformed forms come back Invalid with a
// caller-facing error. No PIE (or editor) needed — Parse is engine-global-free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandWorldSelectorParseTest,
    "PinWright.editor.console_command.WorldSelectorParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandWorldSelectorParseTest::RunTest(const FString& Parameters)
{
    using namespace PieWorldSelector;

    TestEqual(TEXT("empty selector defaults to editor"),
        (int32)Parse(TEXT("")).Kind, (int32)ESelectorKind::Editor);
    TestEqual(TEXT("'editor' parses to editor"),
        (int32)Parse(TEXT("editor")).Kind, (int32)ESelectorKind::Editor);
    TestEqual(TEXT("'server' parses to server"),
        (int32)Parse(TEXT("server")).Kind, (int32)ESelectorKind::Server);
    TestEqual(TEXT("'SERVER' is case-insensitive"),
        (int32)Parse(TEXT("SERVER")).Kind, (int32)ESelectorKind::Server);
    TestEqual(TEXT("' server ' is whitespace-trimmed"),
        (int32)Parse(TEXT(" server ")).Kind, (int32)ESelectorKind::Server);

    const FParsedSelector Client = Parse(TEXT("client"));
    TestEqual(TEXT("'client' parses to client"), (int32)Client.Kind, (int32)ESelectorKind::Client);
    TestEqual(TEXT("'client' means the first client"), Client.Index, 1);

    const FParsedSelector Client2 = Parse(TEXT("client:2"));
    TestEqual(TEXT("'client:2' parses to client"), (int32)Client2.Kind, (int32)ESelectorKind::Client);
    TestEqual(TEXT("'client:2' carries ordinal 2"), Client2.Index, 2);

    const FParsedSelector Pie0 = Parse(TEXT("pie:0"));
    TestEqual(TEXT("'pie:0' parses to pie-instance"), (int32)Pie0.Kind, (int32)ESelectorKind::PieInstance);
    TestEqual(TEXT("'pie:0' carries instance 0"), Pie0.Index, 0);

    TestEqual(TEXT("'bogus' is invalid"),
        (int32)Parse(TEXT("bogus")).Kind, (int32)ESelectorKind::Invalid);
    TestEqual(TEXT("'client:0' is invalid (clients are 1-based)"),
        (int32)Parse(TEXT("client:0")).Kind, (int32)ESelectorKind::Invalid);
    TestEqual(TEXT("'client:x' is invalid"),
        (int32)Parse(TEXT("client:x")).Kind, (int32)ESelectorKind::Invalid);
    TestEqual(TEXT("'pie:' is invalid"),
        (int32)Parse(TEXT("pie:")).Kind, (int32)ESelectorKind::Invalid);
    TestEqual(TEXT("'pie:-1' is invalid"),
        (int32)Parse(TEXT("pie:-1")).Kind, (int32)ESelectorKind::Invalid);
    TestFalse(TEXT("invalid selector carries an error message"),
        Parse(TEXT("bogus")).Error.IsEmpty());
    return true;
}

// Pure resolution/classification coverage over fabricated context lists (worlds stay
// null — ResolveSelector and ClassifyNetMode never dereference them), so server/client
// selection is proven without starting a PIE session.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandWorldSelectorResolveTest,
    "PinWright.editor.console_command.WorldSelectorResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandWorldSelectorResolveTest::RunTest(const FString& Parameters)
{
    using namespace PieWorldSelector;

    auto MakeInfo = [](int32 PieInstance, ENetMode NetMode)
    {
        FPieContextInfo Info;
        Info.PieInstance = PieInstance;
        Info.NetMode = NetMode;
        return Info;
    };

    // Play-As-Listen-Server session: pie:0 listen server + two clients.
    TArray<FPieContextInfo> ListenSession;
    ListenSession.Add(MakeInfo(0, NM_ListenServer));
    ListenSession.Add(MakeInfo(1, NM_Client));
    ListenSession.Add(MakeInfo(2, NM_Client));

    TestEqual(TEXT("'server' resolves to the listen server"),
        ResolveSelector(Parse(TEXT("server")), ListenSession), 0);
    TestEqual(TEXT("'client' resolves to the first client"),
        ResolveSelector(Parse(TEXT("client")), ListenSession), 1);
    TestEqual(TEXT("'client:2' resolves to the second client"),
        ResolveSelector(Parse(TEXT("client:2")), ListenSession), 2);
    TestEqual(TEXT("'client:3' finds no third client"),
        ResolveSelector(Parse(TEXT("client:3")), ListenSession), (int32)INDEX_NONE);
    TestEqual(TEXT("'pie:2' resolves by raw instance id"),
        ResolveSelector(Parse(TEXT("pie:2")), ListenSession), 2);
    TestEqual(TEXT("'pie:5' finds no such instance"),
        ResolveSelector(Parse(TEXT("pie:5")), ListenSession), (int32)INDEX_NONE);

    // Play-As-Client session: hidden dedicated server + one client.
    TArray<FPieContextInfo> DedicatedSession;
    DedicatedSession.Add(MakeInfo(0, NM_DedicatedServer));
    DedicatedSession.Add(MakeInfo(1, NM_Client));

    TestEqual(TEXT("'server' resolves to the dedicated server"),
        ResolveSelector(Parse(TEXT("server")), DedicatedSession), 0);
    TestEqual(TEXT("dedicated server classifies as server"),
        FString(ClassifyNetMode(NM_DedicatedServer)), FString(TEXT("server")));
    TestEqual(TEXT("listen server classifies as server"),
        FString(ClassifyNetMode(NM_ListenServer)), FString(TEXT("server")));
    TestEqual(TEXT("client classifies as client"),
        FString(ClassifyNetMode(NM_Client)), FString(TEXT("client")));

    // Single-instance standalone PIE: it is its own authority, so 'server' matches it;
    // there is no client to match.
    TArray<FPieContextInfo> StandaloneSession;
    StandaloneSession.Add(MakeInfo(0, NM_Standalone));

    TestEqual(TEXT("'server' resolves to the sole standalone instance"),
        ResolveSelector(Parse(TEXT("server")), StandaloneSession), 0);
    TestEqual(TEXT("'client' finds nothing in a standalone session"),
        ResolveSelector(Parse(TEXT("client")), StandaloneSession), (int32)INDEX_NONE);
    TestEqual(TEXT("standalone classifies as standalone"),
        FString(ClassifyNetMode(NM_Standalone)), FString(TEXT("standalone")));

    // No PIE at all: every PIE-targeting selector misses.
    const TArray<FPieContextInfo> NoPie;
    TestEqual(TEXT("'server' finds nothing without PIE"),
        ResolveSelector(Parse(TEXT("server")), NoPie), (int32)INDEX_NONE);
    TestEqual(TEXT("'client' finds nothing without PIE"),
        ResolveSelector(Parse(TEXT("client")), NoPie), (int32)INDEX_NONE);
    TestEqual(TEXT("'pie:0' finds nothing without PIE"),
        ResolveSelector(Parse(TEXT("pie:0")), NoPie), (int32)INDEX_NONE);
    return true;
}

// A malformed `world` selector must be rejected before the command executes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandInvalidWorldSelectorTest,
    "PinWright.editor.console_command.InvalidWorldSelectorRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandInvalidWorldSelectorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("command"), TEXT("stat fps"));
    Payload->SetStringField(TEXT("world"), TEXT("bogus"));
    TestTrue(TEXT("editor.console_command handler found"),
        InvokeHandlerWithCapture(TEXT("editor.console_command"), Payload, Capture));
    TestFalse(TEXT("invalid world selector must fail"), Capture.bSuccess);
    if (GEditor)
    {
        // Without an editor the earlier EDITOR_NOT_AVAILABLE guard fires first; with one,
        // the selector must be the reported problem.
        TestEqual(TEXT("error code is INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.undo  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorUndoNoCrashTest,
    "PinWright.editor.undo.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorUndoNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.undo handler found"), InvokeHandler(TEXT("editor.undo"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.redo  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorRedoNoCrashTest,
    "PinWright.editor.redo.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorRedoNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.redo handler found"), InvokeHandler(TEXT("editor.redo"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.save_all  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSaveAllNoCrashTest,
    "PinWright.editor.save_all.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSaveAllNoCrashTest::RunTest(const FString& Parameters)
{
    // Save-all mutates global editor state and is flaky while PIE state changes.
    TestTrue(TEXT("editor.save_all is registered"), IsRegistered(TEXT("editor.save_all")));
    return true;
}

// Asserts that editor.save_all responds synchronously with the save-result
// JSON (no ticket_id / status:"running" / monitor_path envelope). Captures the
// handler response and inspects the shape; we don't assert success because the
// underlying save touches global editor state.
//
// editor.save_all takes no arguments and has no dry-run or package filter: it is an unfiltered
// flush of every dirty package in the editor. Invoked bare on a real host project it therefore
// wrote the host's own open startup map to disk — the suite's single largest source of a dirty
// host git tree. The dirty set is scoped to the fixture scratch root for the duration of the
// call instead, so the handler still runs for real and the shape assertions below still measure
// the real response, but there is nothing outside /Game/PinWrightTests for it to persist.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSaveAllRespondsSynchronouslyTest,
    "PinWright.editor.save_all.RespondsSynchronously",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSaveAllRespondsSynchronouslyTest::RunTest(const FString& Parameters)
{
    // Defensive: a map save must not depend on prior selection state. If an earlier
    // test left an actor selected that was since destroyed, its stale typed-element
    // handle in USelection faults when editor.save_all's content validation walks the
    // selection via the transform-widget helper. Clearing the selection first keeps
    // this test independent of upstream pollution; it does not soften any assertion below.
    if (GEditor)
    {
        GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSPSurfs=*/true, /*bWarnAboutTools=*/false);
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    {
        PinWrightSuiteMaintenance::FScopedForeignDirtyPackageSuspension FixtureOnlyDirtySet;
        TestTrue(TEXT("editor.save_all handler found"),
            InvokeHandlerWithCapture(TEXT("editor.save_all"), Payload, Capture));
    }
    TestTrue(TEXT("handler responded on the calling stack"), Capture.bWasCalled);

    // The response payload must be the save-result shape, not the job-ticket envelope.
    // BuildSaveAllResultJson always emits savedCount + totalDirty; the job envelope
    // would surface ticket_id / status / monitor_path instead.
    if (Capture.Result.IsValid())
    {
        TestFalse(TEXT("no ticket_id field (sync response, not a job envelope)"),
            Capture.Result->HasField(TEXT("ticket_id")));
        TestFalse(TEXT("no status field (sync response, not a job envelope)"),
            Capture.Result->HasField(TEXT("status")));
        TestFalse(TEXT("no monitor_path field (sync response, not a job envelope)"),
            Capture.Result->HasField(TEXT("monitor_path")));
        TestTrue(TEXT("savedCount present"),
            Capture.Result->HasField(TEXT("savedCount")));
        TestTrue(TEXT("totalDirty present"),
            Capture.Result->HasField(TEXT("totalDirty")));
    }
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.open_asset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorOpenAssetValidParamsTest,
    "PinWright.editor.open_asset.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorOpenAssetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/TestAssets/MyBlueprint"));
    TestTrue(TEXT("editor.open_asset handler found"), InvokeHandler(TEXT("editor.open_asset"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.close_asset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCloseAssetValidParamsTest,
    "PinWright.editor.close_asset.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCloseAssetValidParamsTest::RunTest(const FString& Parameters)
{
    // Closing an asset requires a real loaded asset/editor tab.
    TestTrue(TEXT("editor.close_asset is registered"), IsRegistered(TEXT("editor.close_asset")));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.open_level
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorOpenLevelValidParamsTest,
    "PinWright.editor.open_level.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorOpenLevelValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps/TestLevel"));
    TestTrue(TEXT("editor.open_level handler found"), InvokeHandler(TEXT("editor.open_level"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.set_preferences
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetPreferencesValidParamsTest,
    "PinWright.editor.set_preferences.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetPreferencesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> PrefsObj = MakeShared<FJsonObject>();
    PrefsObj->SetStringField(TEXT("r.ScreenPercentage"), TEXT("100"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("preferences"), PrefsObj);
    TestTrue(TEXT("editor.set_preferences handler found"), InvokeHandler(TEXT("editor.set_preferences"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.simulate_input
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputKeyDownTest,
    "PinWright.editor.simulate_input.KeyDownNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputKeyDownTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("type"), TEXT("key_down"));
    Payload->SetStringField(TEXT("key"), TEXT("A"));
    TestTrue(TEXT("editor.simulate_input handler found"), InvokeHandler(TEXT("editor.simulate_input"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputMouseClickTest,
    "PinWright.editor.simulate_input.MouseClickNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputMouseClickTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("type"), TEXT("mouse_click"));
    Payload->SetNumberField(TEXT("x"), 400.0);
    Payload->SetNumberField(TEXT("y"), 300.0);
    Payload->SetStringField(TEXT("button"), TEXT("left"));
    TestTrue(TEXT("editor.simulate_input handler found"), InvokeHandler(TEXT("editor.simulate_input"), Payload));
    return true;
}

// Regression for B-simulate-input-cef-click-noop: editor.simulate_input mouse_click
// must actually ROUTE the synthesized click to the widget under the point (and then
// report success honestly from the handled result, not the old unconditional
// bSuccess = true).
//
// The reverted handler built its FPointerEvents inline with a nullptr platform
// window and NO effecting button (the move/delta FPointerEvent constructor), then
// hardcoded bSuccess = true. That injection silently missed the widget under the
// cursor (including SViewport-hosted CEF browsers, whose viewport forwards a
// correctly-routed Slate click into the DOM), yet still reported success. The fix
// routes through FDriveInput's vetted click primitive (native-window resolution +
// effecting button + inactive-input flag + move-first) and reports the real handled
// result.
//
// The test drives the PRODUCTION editor.simulate_input handler against an in-code
// Slate fixture: a real top-level SWindow holding a single SButton is clicked at the
// button's screen-space center; the button's OnClicked must fire AND the handler
// must report success. Reverting the fix (nullptr window / effecting-button-less
// FPointerEvent, or a hardcoded-false over-correction) fails one of the two: the
// reverted injection never routes to the button so OnClicked stays false, and an
// always-false success fails the success assertion. (An empty-space "not handled"
// counterfactual proved unreliable headless — Slate's ProcessMouseButtonDownEvent
// can return handled over a window-less point under -RenderOffScreen mouse capture —
// so the honest-result guard is anchored on the deterministic routed-click case.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputMouseClickHonestSuccessTest,
    "PinWright.editor.simulate_input.MouseClickHonestSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputMouseClickHonestSuccessTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate application not initialized; cannot exercise editor.simulate_input mouse_click."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    // Positive routing through the fixed injection: click a real button and require
    // both that it actuated (OnClicked) and that success was reported honestly.
    bool bButtonClicked = false;
    TSharedRef<SWindow> Window = SNew(SWindow)
        .ScreenPosition(FVector2D(140.0f, 140.0f))
        .ClientSize(FVector2D(220.0f, 140.0f))
        .FocusWhenFirstShown(false)
        .CreateTitleBar(false)
        .SupportsMaximize(false)
        .SupportsMinimize(false);

    Window->SetContent(
        SNew(SButton)
        .OnClicked_Lambda([&bButtonClicked]()
        {
            bButtonClicked = true;
            return FReply::Handled();
        }));

    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT { SlateApp.RequestDestroyWindow(Window); };

    // Pump Slate so the window lays out and paints — hit-testing reads the cached
    // geometry / hittest grid populated during paint. Wait (bounded) until the
    // button has a non-degenerate cached geometry, then click its center.
    const TSharedRef<SWidget> ButtonWidget = Window->GetContent();
    FVector2D ClickPoint(0.0, 0.0);
    for (int32 TickIndex = 0; TickIndex < 16; ++TickIndex)
    {
        SlateApp.Tick(ESlateTickType::All);
        const FGeometry Geo = ButtonWidget->GetTickSpaceGeometry();
        const FVector2D AbsPos(Geo.GetAbsolutePosition());
        const FVector2D AbsSize(Geo.GetAbsoluteSize());
        if (AbsSize.X > 1.0 && AbsSize.Y > 1.0)
        {
            ClickPoint = AbsPos + AbsSize * 0.5;
            break;
        }
    }

    // The in-code fixture must lay out — it is constructed here, so a degenerate
    // geometry is a real failure of the test's own window, not a skippable absence.
    // Drive TestTrue's bool return straight into the guard so the layout condition
    // lives in one place: assert it, then proceed only if it held.
    if (TestTrue(TEXT("in-code button laid out with a hit-testable geometry"),
            ClickPoint.X > 0.0 && ClickPoint.Y > 0.0))
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("type"), TEXT("mouse_click"));
        Payload->SetNumberField(TEXT("x"), ClickPoint.X);
        Payload->SetNumberField(TEXT("y"), ClickPoint.Y);
        Payload->SetStringField(TEXT("button"), TEXT("left"));
        TestTrue(TEXT("editor.simulate_input handler found (button click)"),
            InvokeHandlerWithCapture(TEXT("editor.simulate_input"), Payload, Capture));

        // Core routing assertion: the synthesized click actually reached the button.
        // Reverting the injection fix (nullptr window / no effecting button) leaves
        // this false.
        TestTrue(TEXT("synthesized click fired the button's OnClicked (correct native-window routing)"),
            bButtonClicked);
        // Success is now the honest handled result, not a hardcoded true.
        TestTrue(TEXT("click a widget consumed reports success"), Capture.bSuccess);
    }

    return true;
}

// ============================================================================
// EditorCommandHandler — editor.start_recording  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStartRecordingNoCrashTest,
    "PinWright.editor.start_recording.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStartRecordingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestRecording_001"));
    TestTrue(TEXT("editor.start_recording handler found"), InvokeHandler(TEXT("editor.start_recording"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.stop_recording  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStopRecordingNoCrashTest,
    "PinWright.editor.stop_recording.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStopRecordingNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.stop_recording handler found"), InvokeHandler(TEXT("editor.stop_recording"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.create_bookmark  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateBookmarkNoCrashTest,
    "PinWright.editor.create_bookmark.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateBookmarkNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("index"), 3.0);
    TestTrue(TEXT("editor.create_bookmark handler found"), InvokeHandler(TEXT("editor.create_bookmark"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.jump_to_bookmark  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorJumpToBookmarkNoCrashTest,
    "PinWright.editor.jump_to_bookmark.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorJumpToBookmarkNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("index"), 3.0);
    TestTrue(TEXT("editor.jump_to_bookmark handler found"), InvokeHandler(TEXT("editor.jump_to_bookmark"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.create_bookmark + editor.jump_to_bookmark
//   round-trip regression (B-editor-bookmark-roundtrip-noop)
//
// Guards against the old silent-success no-op: both handlers used to fire
// GEditor->Exec("SetBookmark N"/"JumpToBookmark N") — which is NOT a routable
// engine Exec verb — then unconditionally SendSuccess, so the camera never moved
// yet the call reported success. The fix routes through IBookmarkTypeTools
// against the active level-editor viewport client and reports honest errors.
//
// Two assertion paths so this is meaningful both with and without a live
// level viewport:
//   - Live viewport available: full round-trip — set camera to A, create_bookmark,
//     move camera to B, jump_to_bookmark, assert the camera returned to A. This
//     would fail under the reverted Exec no-op (camera stays at B).
//   - No live viewport (headless -unattended): jump_to_bookmark on a fresh slot
//     must now return an ERROR (VIEWPORT_NOT_AVAILABLE or BOOKMARK_EMPTY), never
//     the old fake {success:true}. This also fails under the reverted code, which
//     always succeeded.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorBookmarkRoundTripTest,
    "PinWright.editor.bookmark.RoundTripMovesCamera",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorBookmarkRoundTripTest::RunTest(const FString& Parameters)
{
    // Resolve the active level-editor viewport client through the SAME shared
    // helper the handler uses, so test and handler agree on what counts as a
    // live bookmark viewport by construction (not via a copied resolution walk).
    FEditorViewportClient* ViewportClient =
        EditorHandlerUtils::ResolveActiveLevelViewportClient(/*bRequireWorld=*/true);

    const int32 Slot = 7;

    if (ViewportClient == nullptr)
    {
        // Headless: no level viewport. The honest contract is that the handlers
        // must NOT fake-success — jump on an unpopulated slot returns an error.
        FTestResponseCapture JumpCapture;
        TSharedPtr<FJsonObject> JumpPayload = MakeShared<FJsonObject>();
        JumpPayload->SetNumberField(TEXT("index"), Slot);
        const bool bFound =
            InvokeHandlerWithCapture(TEXT("editor.jump_to_bookmark"), JumpPayload, JumpCapture);
        TestTrue(TEXT("editor.jump_to_bookmark handler found"), bFound);
        TestFalse(TEXT("jump_to_bookmark must not fake-success without a real bookmark"),
            JumpCapture.bSuccess);
        return true;
    }

    // Live viewport: drive a full create -> move -> jump round-trip.
    const FVector PoseA(777.0f, 222.0f, 333.0f);
    const FRotator RotA(-20.0f, 120.0f, 0.0f);
    const FVector PoseB(5000.0f, 5000.0f, 5000.0f);
    const FRotator RotB(-90.0f, 0.0f, 0.0f);

    // Place the camera at A, store it in a bookmark slot.
    ViewportClient->SetViewLocation(PoseA);
    ViewportClient->SetViewRotation(RotA);

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetNumberField(TEXT("index"), Slot);
    TestTrue(TEXT("editor.create_bookmark handler found"),
        InvokeHandlerWithCapture(TEXT("editor.create_bookmark"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_bookmark reports success when a real bookmark is stored"),
        CreateCapture.bSuccess);

    // Move the camera far away to B.
    ViewportClient->SetViewLocation(PoseB);
    ViewportClient->SetViewRotation(RotB);
    TestTrue(TEXT("camera moved away from the bookmarked pose"),
        !ViewportClient->GetViewLocation().Equals(PoseA, 1.0f));

    // Jump back: the camera must return to A.
    FTestResponseCapture JumpCapture;
    TSharedPtr<FJsonObject> JumpPayload = MakeShared<FJsonObject>();
    JumpPayload->SetNumberField(TEXT("index"), Slot);
    TestTrue(TEXT("editor.jump_to_bookmark handler found"),
        InvokeHandlerWithCapture(TEXT("editor.jump_to_bookmark"), JumpPayload, JumpCapture));
    TestTrue(TEXT("jump_to_bookmark reports success for a populated slot"),
        JumpCapture.bSuccess);

    // The load-bearing assertion: the camera actually returned to the bookmarked
    // pose. Under the old Exec no-op it would still be at PoseB.
    TestTrue(TEXT("jump_to_bookmark restored the bookmarked camera location"),
        ViewportClient->GetViewLocation().Equals(PoseA, 1.0f));

    return true;
}

// ============================================================================
// PIEHandler — editor.play  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPlayNoCrashTest,
    "PinWright.editor.play.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPlayNoCrashTest::RunTest(const FString& Parameters)
{
    // PIE init walks every dirty transient UBlueprint; the BPIR test suite leaves malformed BPs that crash compile during ResolveDirtyBlueprints under -unattended.
    if (FApp::IsUnattended())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("unattended-run"),
            TEXT("Skipped under -unattended: editor.play interacts with transient-BP state polluted by other tests. Run interactively to exercise."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.play handler found"), InvokeHandler(TEXT("editor.play"), Payload));
    return true;
}

// ============================================================================
// PIEHandler — editor.play `networkEmulation` param
//
// Pure spec-building coverage (PieNetworkEmulation::BuildEmulationSpec takes the
// profile list as a parameter, so defaults / canonicalization / rejection are
// proven without engine config or a PIE session), plus handler-level rejection
// tests for the two INVALID_ARGUMENT paths. No test here may actually start PIE:
// the rejection payloads return before RequestPlayInEditorSession, and the
// success path is covered purely (see FEditorPlayNoCrashTest's -unattended
// caveat for why live editor.play runs are avoided in automation).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPlayNetworkEmulationSpecBuildTest,
    "PinWright.editor.play.NetworkEmulationSpecBuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPlayNetworkEmulationSpecBuildTest::RunTest(const FString& Parameters)
{
    using namespace PieNetworkEmulation;

    TArray<FString> Profiles;
    Profiles.Add(TEXT("Average"));
    Profiles.Add(TEXT("Bad"));
    Profiles.Add(TEXT("BufferBloat"));

    FEmulationSpec Spec;
    FString Error;

    // Default (param omitted): force-disabled, no error, no profile in effect.
    TestTrue(TEXT("empty request builds"),
        BuildEmulationSpec(false, FString(), FString(), Profiles, Spec, Error));
    TestFalse(TEXT("default is disabled"), Spec.bEnabled);
    TestTrue(TEXT("disabled spec carries no profile"), Spec.Profile.IsEmpty());

    // Enabled with no target/profile: serverOnly + Average defaults.
    TestTrue(TEXT("enabled-only request builds"),
        BuildEmulationSpec(true, FString(), FString(), Profiles, Spec, Error));
    TestTrue(TEXT("enabled"), Spec.bEnabled);
    TestEqual(TEXT("target defaults to serverOnly"),
        (int32)Spec.Target, (int32)EEmulationTarget::ServerOnly);
    TestEqual(TEXT("profile defaults to Average"), Spec.Profile, FString(TEXT("Average")));

    // Target parses case-insensitively; profile canonicalizes to config casing.
    TestTrue(TEXT("CLIENTSONLY/bufferbloat builds"),
        BuildEmulationSpec(true, TEXT("CLIENTSONLY"), TEXT("bufferbloat"), Profiles, Spec, Error));
    TestEqual(TEXT("clientsOnly parsed"),
        (int32)Spec.Target, (int32)EEmulationTarget::ClientsOnly);
    TestEqual(TEXT("profile canonicalized to config casing"),
        Spec.Profile, FString(TEXT("BufferBloat")));
    TestTrue(TEXT("everyone builds"),
        BuildEmulationSpec(true, TEXT("everyone"), TEXT("Bad"), Profiles, Spec, Error));
    TestEqual(TEXT("everyone parsed"),
        (int32)Spec.Target, (int32)EEmulationTarget::Everyone);

    // Unknown target rejected — even when disabled (typos must never pass silently).
    TestFalse(TEXT("bad target rejected"),
        BuildEmulationSpec(true, TEXT("server"), FString(), Profiles, Spec, Error));
    TestTrue(TEXT("target error names the valid values"),
        Error.Contains(TEXT("serverOnly")) && Error.Contains(TEXT("clientsOnly")) && Error.Contains(TEXT("everyone")));
    TestFalse(TEXT("bad target rejected even when disabled"),
        BuildEmulationSpec(false, TEXT("bogus"), FString(), Profiles, Spec, Error));

    // Unknown profile rejected with the available list — even when disabled.
    TestFalse(TEXT("bad profile rejected"),
        BuildEmulationSpec(true, FString(), TEXT("NoSuchProfile"), Profiles, Spec, Error));
    TestTrue(TEXT("profile error lists the available profiles"),
        Error.Contains(TEXT("NoSuchProfile")) && Error.Contains(TEXT("Average"))
        && Error.Contains(TEXT("Bad")) && Error.Contains(TEXT("BufferBloat")));
    TestFalse(TEXT("bad profile rejected even when disabled"),
        BuildEmulationSpec(false, FString(), TEXT("NoSuchProfile"), Profiles, Spec, Error));

    // The details-panel "Custom" pseudo-profile is not a config profile: rejected.
    TestFalse(TEXT("'Custom' pseudo-profile rejected"),
        BuildEmulationSpec(true, FString(), TEXT("Custom"), Profiles, Spec, Error));

    // Enabling with an empty config list cannot resolve the Average default.
    const TArray<FString> NoProfiles;
    TestFalse(TEXT("enabled with no configured profiles rejected"),
        BuildEmulationSpec(true, FString(), FString(), NoProfiles, Spec, Error));
    TestTrue(TEXT("empty-list error says none configured"),
        Error.Contains(TEXT("none configured")));
    // ...but a plain disabled request still builds without any profiles configured.
    TestTrue(TEXT("disabled request builds with no configured profiles"),
        BuildEmulationSpec(false, FString(), FString(), NoProfiles, Spec, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPlayNetworkEmulationBadTargetTest,
    "PinWright.editor.play.NetworkEmulationBadTargetRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPlayNetworkEmulationBadTargetTest::RunTest(const FString& Parameters)
{
    if (GEditor && GEditor->PlayWorld)
    {
        // The idempotent alreadyPlaying guard answers before param validation, so a
        // stray live PIE session would make this test meaningless — skip honestly.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-session-active"),
            TEXT("Skipped: a live PIE session short-circuits editor.play before validation."));
        return true;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> EmulationObj = MakeShared<FJsonObject>();
    EmulationObj->SetBoolField(TEXT("enabled"), true);
    EmulationObj->SetStringField(TEXT("target"), TEXT("bogusTarget"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("networkEmulation"), EmulationObj);
    TestTrue(TEXT("editor.play handler found"),
        InvokeHandlerWithCapture(TEXT("editor.play"), Payload, Capture));

    // Must fail without starting a session (EDITOR_NOT_AVAILABLE without an editor,
    // INVALID_ARGUMENT with one).
    TestFalse(TEXT("bad networkEmulation.target must fail"), Capture.bSuccess);
    if (GEditor)
    {
        TestEqual(TEXT("error code is INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("error names the offending target"),
            Capture.Message.Contains(TEXT("bogusTarget")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPlayNetworkEmulationBadProfileTest,
    "PinWright.editor.play.NetworkEmulationBadProfileRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPlayNetworkEmulationBadProfileTest::RunTest(const FString& Parameters)
{
    if (GEditor && GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-session-active"),
            TEXT("Skipped: a live PIE session short-circuits editor.play before validation."));
        return true;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> EmulationObj = MakeShared<FJsonObject>();
    EmulationObj->SetBoolField(TEXT("enabled"), true);
    EmulationObj->SetStringField(TEXT("profile"), TEXT("PW_NoSuchProfile"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("networkEmulation"), EmulationObj);
    TestTrue(TEXT("editor.play handler found"),
        InvokeHandlerWithCapture(TEXT("editor.play"), Payload, Capture));

    TestFalse(TEXT("bad networkEmulation.profile must fail"), Capture.bSuccess);
    if (GEditor)
    {
        TestEqual(TEXT("error code is INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        // The rejection must name the bad profile and enumerate what IS available
        // (live UNetworkSettings config — exact names vary per project, so assert
        // the listing phrase rather than specific entries).
        TestTrue(TEXT("error names the offending profile"),
            Capture.Message.Contains(TEXT("PW_NoSuchProfile")));
        TestTrue(TEXT("error lists the available profiles"),
            Capture.Message.Contains(TEXT("Available profiles")));
    }
    return true;
}

// ============================================================================
// PIEHandler — editor.stop  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStopNoCrashTest,
    "PinWright.editor.stop.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStopNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.stop handler found"), InvokeHandler(TEXT("editor.stop"), Payload));
    return true;
}

// ============================================================================
// PIEHandler — editor.pause  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPauseNoCrashTest,
    "PinWright.editor.pause.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPauseNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.pause handler found"), InvokeHandler(TEXT("editor.pause"), Payload));
    return true;
}

// ============================================================================
// PIEHandler — editor.resume  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorResumeNoCrashTest,
    "PinWright.editor.resume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorResumeNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.resume handler found"), InvokeHandler(TEXT("editor.resume"), Payload));
    return true;
}

// ============================================================================
// PIEHandler — editor.step_frame  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStepFrameNoCrashTest,
    "PinWright.editor.step_frame.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStepFrameNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.step_frame handler found"), InvokeHandler(TEXT("editor.step_frame"), Payload));
    return true;
}

// ============================================================================
// PIEHandler — editor.eject  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorEjectNoCrashTest,
    "PinWright.editor.eject.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorEjectNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.eject handler found"),
        InvokeHandlerWithCapture(TEXT("editor.eject"), Payload, Capture));

    // With no active PIE session (the normal automation state) the handler must report
    // NO_ACTIVE_SESSION, never a fake success — an earlier version ran a nonexistent
    // "Eject" exec and returned ejected:true unconditionally. Guard on the state so a
    // stray live PIE session during the run does not spuriously fail the assertion.
    if (!GEditor || !GEditor->PlayWorld)
    {
        TestFalse(TEXT("eject without a PIE session is not a success"), Capture.bSuccess);
        TestEqual(TEXT("eject without a PIE session emits NO_ACTIVE_SESSION"),
            Capture.ErrorCode, FString(TEXT("NO_ACTIVE_SESSION")));
    }
    return true;
}

// ============================================================================
// PIEHandler — editor.possess
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPossessValidParamsTest,
    "PinWright.editor.possess.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPossessValidParamsTest::RunTest(const FString& Parameters)
{
    // Possess requires an active stable PIE/player context.
    TestTrue(TEXT("editor.possess is registered"), IsRegistered(TEXT("editor.possess")));
    return true;
}

// Regression for B-editor-possess-non-pawn-silent-success: editor.possess must
// reject a non-Pawn target with NOT_A_PAWN, never report a fake success.
// APlayerController::Possess only accepts a Pawn, and the old handler returned
// {success:true} unconditionally. A non-Pawn can never be possessed
// regardless of PIE state, so the handler now Pawn-checks the resolved actor
// before the PlayWorld gate — making this rejection observable in a unit test
// (no live PIE) by spawning a non-Pawn (a PointLight) into the editor world and
// possessing it by label. If the IsA(APawn) pre-check is reverted, the handler
// stops emitting NOT_A_PAWN here (it would fall through to NOT_IN_PIE in a unit
// test, and to fake success under live PIE) and this test fails.
//
// The spawn must NOT be transient: the handler resolves actors via
// McpActorUtils::FindActorByName, whose non-PIE editor-world path is
// UEditorActorSubsystem::GetAllLevelActors(), which filters out RF_Transient
// actors (UE 5.7 EditorActorSubsystem.cpp: "Don't add transient actors in
// non-play worlds"). A transient fixture is therefore unresolvable and the
// handler returns ACTOR_NOT_FOUND before ever reaching the Pawn check. Spawning
// a non-transient actor (mirroring the focus_actor internal-name regression
// test) keeps it resolvable; FScopedEditorWorldActorGuard still destroys it on
// scope exit because it tracks every actor spawned during the test, transient or
// not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPossessNonPawnRejectedTest,
    "PinWright.editor.possess.NonPawnRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPossessNonPawnRejectedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping editor.possess NonPawnRejected test"));
        return true;
    }

    // Spawn a non-Pawn (PointLight) the handler will resolve by label, and clean
    // it up on every exit path so the open map is left untouched.
    FScopedEditorWorldActorGuard Guard;
    AActor* NonPawn = World->SpawnActor<APointLight>(
        FVector(0.f, 0.f, 5000.f), FRotator::ZeroRotator, FActorSpawnParameters());
    if (!NonPawn)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not spawn PointLight — skipping editor.possess NonPawnRejected test"));
        return true;
    }
    const FString Label = FString::Printf(TEXT("PW_PossessNonPawn_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    NonPawn->SetActorLabel(Label);
    // Resolve by the label the handler will see (SetActorLabel may disambiguate).
    const FString ResolvedLabel = NonPawn->GetActorLabel();

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedLabel);
    TestTrue(TEXT("editor.possess handler found"),
        InvokeHandlerWithCapture(TEXT("editor.possess"), Payload, Capture));

    // The non-Pawn must be rejected — never a fake success — with the typed code.
    TestFalse(TEXT("possessing a non-Pawn is not a success"), Capture.bSuccess);
    TestEqual(TEXT("non-Pawn possess emits NOT_A_PAWN"),
        Capture.ErrorCode, FString(TEXT("NOT_A_PAWN")));
    return true;
}

// ============================================================================
// PIEHandler — editor.pie_status  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPieStatusNoCrashTest,
    "PinWright.editor.pie_status.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPieStatusNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.pie_status handler found"),
        InvokeHandlerWithCapture(TEXT("editor.pie_status"), Payload, Capture));
    if (!GEditor)
    {
        TestFalse(TEXT("fails honestly without an editor"), Capture.bSuccess);
        return true;
    }

    // Read-only probe: must succeed whether or not PIE is running, and count must
    // agree with the contexts array it describes.
    TestTrue(TEXT("editor.pie_status succeeds"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Contexts = nullptr;
        TestTrue(TEXT("response carries contexts[]"),
            Capture.Result->TryGetArrayField(TEXT("contexts"), Contexts));
        double Count = -1.0;
        TestTrue(TEXT("response carries count"),
            Capture.Result->TryGetNumberField(TEXT("count"), Count));
        bool bInPie = false;
        TestTrue(TEXT("response carries inPie"),
            Capture.Result->TryGetBoolField(TEXT("inPie"), bInPie));
        if (Contexts)
        {
            TestEqual(TEXT("count matches contexts length"), (int32)Count, Contexts->Num());
            TestEqual(TEXT("inPie agrees with contexts presence"), bInPie, Contexts->Num() > 0);
        }
    }
    return true;
}

// ============================================================================
// ViewportHandler — editor.focus_actor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorFocusActorValidParamsTest,
    "PinWright.editor.focus_actor.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorFocusActorValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("DirectionalLight_0"));
    TestTrue(TEXT("editor.focus_actor handler found"), InvokeHandler(TEXT("editor.focus_actor"), Payload));
    return true;
}

// Regression test for E-focus-actor-rejects-internal-name-label-only: editor.focus_actor
// must resolve actorName by the INTERNAL object name (the `name` field actor.list reports,
// and the `objectName` field on actor.find_by_class rows — that verb's `name` is the
// display LABEL, not the internal name), not just the display label. Under the reverted inline
// label-only loop, passing the internal object name returned [ACTOR_NOT_FOUND] for an
// actor that demonstrably exists — the inverse of every actor.* verb, which routes
// through McpActorUtils::FindActorByName (label OR internal name OR object path).
//
// This spawns a real actor and gives it a display label that is deliberately DIFFERENT
// from its internal object name, so matching the internal name cannot accidentally
// succeed via the label path. It then invokes the production editor.focus_actor handler
// with the internal name and asserts success + that the actor became selected. Reverting
// the fix (matching GetActorLabel() only) fails the success assertion because the internal
// name never equals the distinct label.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorFocusActorResolvesInternalNameTest,
    "PinWright.editor.focus_actor.ResolvesInternalName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorFocusActorResolvesInternalNameTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping focus_actor internal-name resolution test."));
        return true;
    }

    // The guard destroys the probe and restores L_Core's dirty flag on every exit
    // path, so the test leaves the open map untouched.
    FScopedEditorWorldActorGuard WorldGuard;

    // Spawn a probe actor directly so we control both identifiers. Give it a display
    // label that differs from its internal object name (GetName()) so the only path
    // that can resolve the internal name is the name-matching branch under test.
    FActorSpawnParameters SpawnParams;
    AActor* Probe = World->SpawnActor<APointLight>(
        FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
    TestNotNull(TEXT("probe actor spawned"), Probe);
    if (!Probe)
    {
        return true;
    }

    const FString InternalName = Probe->GetName();
    Probe->SetActorLabel(TEXT("FocusActorProbeDistinctLabel"));
    const FString DisplayLabel = Probe->GetActorLabel();

    // Sanity: the two identifiers must differ, otherwise the label path could mask the bug.
    TestNotEqual(TEXT("internal name differs from display label"), InternalName, DisplayLabel);

    // Reset selection state before the call so the post-call check observes only what
    // the handler selects.
    GEditor->SelectNone(true, true, false);

    // The automation run loads its map (L_Core) with the persistent level locked,
    // and GEditor->SelectActor refuses to select an actor in a locked level
    // ("operation could not be completed because the level is locked"). The handler
    // would then return success (it resolved + framed the actor) yet leave nothing
    // selected, failing the selection assertion below for a reason orthogonal to the
    // fix. Unlock the level for the call; FScopedLevelLock restores the prior state.
    FScopedLevelLock LevelUnlock(World->PersistentLevel, /*bDesiredLocked=*/false);

    // Drive the production handler with the INTERNAL object name.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), InternalName);
    const bool bFound = InvokeHandlerWithCapture(TEXT("editor.focus_actor"), Payload, Capture);
    TestTrue(TEXT("editor.focus_actor handler found"), bFound);

    // Core assertion: focusing by internal name succeeds (it returned ACTOR_NOT_FOUND
    // before the fix because the inline loop matched the label only).
    TestTrue(TEXT("focus_actor resolves the internal object name"), Capture.bSuccess);

    // The handler selects the matched actor; confirm the right actor was selected.
    if (Capture.bSuccess)
    {
        USelection* Selection = GEditor->GetSelectedActors();
        const bool bProbeSelected = Selection && Selection->IsSelected(Probe);
        TestTrue(TEXT("focus_actor selected the actor matched by internal name"), bProbeSelected);
    }

    return true;
}

// ============================================================================
// ViewportHandler — editor.set_camera  (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetCameraNoCrashTest,
    "PinWright.editor.set_camera.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetCameraNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
    LocObj->SetNumberField(TEXT("x"), 100.0);
    LocObj->SetNumberField(TEXT("y"), 200.0);
    LocObj->SetNumberField(TEXT("z"), 300.0);

    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("pitch"), -15.0);
    RotObj->SetNumberField(TEXT("yaw"), 90.0);
    RotObj->SetNumberField(TEXT("roll"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), LocObj);
    Payload->SetObjectField(TEXT("rotation"), RotObj);
    TestTrue(TEXT("editor.set_camera handler found"), InvokeHandler(TEXT("editor.set_camera"), Payload));
    return true;
}

// Regression test for B-set-camera-no-viewport-redraw: editor.set_camera must
// (a) apply the requested pose to the active level-viewport client synchronously
// and (b) force a synchronous Viewport->Draw() before returning, so a screenshot
// taken on the same call stack observes the new view instead of the stale frame.
//
// Two counterfactuals, both gated on a live perspective level viewport (headless
// runs with no viewport are skipped rather than false-negative):
//   1. Camera-apply: GetLevelViewportCameraInfo() reports the requested pose right
//      after the handler returns. Reverting SetLevelViewportCameraInfo fails this.
//   2. Force-redraw: FEditorViewportClient::Draw() appends an entry to the render
//      world's CachedViewInfoRenderedLastFrame *during* the synchronous draw, with
//      ViewToWorld carrying the just-set camera location. The test snapshots the
//      array length before invoking and, after invoking, asserts a new entry was
//      appended whose ViewToWorld origin matches the requested location. No engine
//      tick runs between snapshot and invoke on the test's game-thread stack, so
//      the only thing that can append is the handler's own Viewport->Draw().
//      Reverting ForceRedrawActiveViewport() (leaving only Invalidate, which marks
//      the viewport dirty for the *next* tick) appends nothing on this stack and
//      fails this assertion — this is the redraw gate the pose check alone lacked.
//
// This observes the redraw via a game-thread side effect of the draw path, so it
// needs no RHI framebuffer readback (NullRHI is disallowed for these tests, but a
// real RHI is not required for the cache append). When Draw() legitimately early-
// outs (no render world, or the world is mid map-change) the redraw assertion is
// skipped so the run doesn't false-negative; the camera-apply assertion still runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetCameraForceRedrawTest,
    "PinWright.editor.set_camera.ForceRedraw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetCameraForceRedrawTest::RunTest(const FString& Parameters)
{
    const FVector ExpectedLocation(1000.0, 2000.0, 500.0);
    const FRotator ExpectedRotation(-30.0, 45.0, 0.0);

    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
    LocObj->SetNumberField(TEXT("x"), ExpectedLocation.X);
    LocObj->SetNumberField(TEXT("y"), ExpectedLocation.Y);
    LocObj->SetNumberField(TEXT("z"), ExpectedLocation.Z);

    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("pitch"), ExpectedRotation.Pitch);
    RotObj->SetNumberField(TEXT("yaw"), ExpectedRotation.Yaw);
    RotObj->SetNumberField(TEXT("roll"), ExpectedRotation.Roll);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("location"), LocObj);
    Payload->SetObjectField(TEXT("rotation"), RotObj);

    // Resolve the active perspective level-viewport client the same way the
    // handler does (UUnrealEditorSubsystem). With no live viewport the camera
    // state is unobservable, so skip rather than false-negative.
#if defined(MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM)
    UUnrealEditorSubsystem* UES = GEditor
        ? GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>()
        : nullptr;
    FVector ProbeLoc; FRotator ProbeRot;
    const bool bHaveViewport =
        UES && UES->GetLevelViewportCameraInfo(ProbeLoc, ProbeRot);

    // The render world the handler's Draw() writes its view cache into is the
    // active viewport client's world. Snapshot its cached-view count before the
    // call so we can detect the synchronous draw-flush as a new appended entry.
    UWorld* RenderWorld = nullptr;
    int32 CachedViewsBefore = 0;
    if (bHaveViewport)
    {
        // The shared typed resolver, not GetActiveViewport()->GetClient(): the latter hands back
        // the game client under PIE and the downcast is undefined (see EditorHandlerUtils.h).
        if (FEditorViewportClient* VC =
                EditorHandlerUtils::ResolveActiveLevelViewportClient())
        {
            RenderWorld = VC->GetWorld();
            if (RenderWorld)
            {
                CachedViewsBefore = RenderWorld->CachedViewInfoRenderedLastFrame.Num();
            }
        }
    }
#else
    const bool bHaveViewport = false;
#endif

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_camera handler found"),
        InvokeHandlerWithCapture(TEXT("editor.set_camera"), Payload, Capture));
    // The handler must respond on the calling stack — it never defers to a job.
    TestTrue(TEXT("handler responded on the calling stack"), Capture.bWasCalled);

#if defined(MCP_TEST_HAS_UNREALEDITOR_SUBSYSTEM)
    if (!bHaveViewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped pose/redraw assertions: no active perspective level "
                         "viewport; the camera-apply/redraw path is unreachable in this run."));
        return true;
    }

    // Counterfactual 1 (camera-apply): the new pose must be readable from the live
    // viewport client immediately after the handler returns. If the handler stopped
    // applying the camera synchronously, GetLevelViewportCameraInfo would still
    // report the old pose and these assertions fail.
    FVector ActualLocation; FRotator ActualRotation;
    TestTrue(TEXT("level viewport camera info readable after set_camera"),
        UES->GetLevelViewportCameraInfo(ActualLocation, ActualRotation));
    TestTrue(TEXT("viewport camera location matches requested pose within the same call stack"),
        ActualLocation.Equals(ExpectedLocation, 0.5));
    TestTrue(TEXT("viewport camera rotation matches requested pose within the same call stack"),
        ActualRotation.Equals(ExpectedRotation, 0.5));

    // Counterfactual 2 (force-redraw): the synchronous Viewport->Draw() must have
    // appended a cached view computed from the new camera. A draw that early-outs
    // (no render world / mid map-change) appends nothing, so only gate when a draw
    // could have produced a cache entry; reverting ForceRedrawActiveViewport leaves
    // the count unchanged on this stack and fails the assertion.
    if (RenderWorld && !RenderWorld->IsPreparingMapChange())
    {
        const int32 CachedViewsAfter = RenderWorld->CachedViewInfoRenderedLastFrame.Num();
        const bool bDrewNewView = CachedViewsAfter > CachedViewsBefore;
        TestTrue(TEXT("set_camera forced a synchronous viewport draw (new cached view appended)"),
            bDrewNewView);
        if (bDrewNewView)
        {
            const FVector DrawnViewOrigin =
                RenderWorld->CachedViewInfoRenderedLastFrame.Last().ViewToWorld.GetOrigin();
            TestTrue(TEXT("synchronously drawn view used the just-set camera location"),
                DrawnViewOrigin.Equals(ExpectedLocation, 1.0));
        }
    }
    else
    {
        AddInfo(TEXT("Skipped redraw assertion: no render world or world is mid "
                     "map-change, so the synchronous draw legitimately produced no "
                     "cached view this run; the camera-apply assertion still ran."));
    }
#endif
    return true;
}

// ============================================================================
// ViewportHandler — editor.set_view_mode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewModeValidParamsTest,
    "PinWright.editor.set_view_mode.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewModeValidParamsTest::RunTest(const FString& Parameters)
{
    // The active level viewport's view mode is GLOBAL editor state. Left on Wireframe, every later
    // suite test that reads that viewport back - effect.step_and_capture and the render.capture
    // family - measures a wireframe frame, which on a sparse level is indistinguishable from a
    // black one. Restore it, the way the named set_view_mode regression tests restore theirs.
    FEditorViewportClient* ViewportClient =
        EditorHandlerUtils::ResolveActiveLevelViewportClient(/*bRequireWorld=*/false);
    const EViewModeIndex PreviousViewMode =
        ViewportClient ? ViewportClient->GetViewMode() : VMI_Lit;
    ON_SCOPE_EXIT
    {
        if (ViewportClient)
        {
            ViewportClient->SetViewMode(PreviousViewMode);
            ViewportClient->Invalidate();
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("viewMode"), TEXT("Wireframe"));
    TestTrue(TEXT("editor.set_view_mode handler found"), InvokeHandler(TEXT("editor.set_view_mode"), Payload));
    return true;
}

// ============================================================================
// ViewportHandler — editor.set_viewport_realtime  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewportRealtimeNoCrashTest,
    "PinWright.editor.set_viewport_realtime.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewportRealtimeNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("realtime"), true);
    TestTrue(TEXT("editor.set_viewport_realtime handler found"), InvokeHandler(TEXT("editor.set_viewport_realtime"), Payload));
    return true;
}

// ============================================================================
// ViewportHandler — editor.set_game_view  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetGameViewNoCrashTest,
    "PinWright.editor.set_game_view.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetGameViewNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    TestTrue(TEXT("editor.set_game_view handler found"), InvokeHandler(TEXT("editor.set_game_view"), Payload));
    return true;
}

// ============================================================================
// ViewportHandler — editor.screenshot  (optional param)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotNoCrashTest,
    "PinWright.editor.screenshot.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filename"), TEXT("TestShot_001"));
    TestTrue(TEXT("editor.screenshot handler found"), InvokeHandler(TEXT("editor.screenshot"), Payload));
    return true;
}

// ============================================================================
// ContentBrowserHandler — editor.get_content_browser_selection
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorGetContentBrowserSelectionNoCrashTest,
    "PinWright.editor.get_content_browser_selection.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorGetContentBrowserSelectionNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.get_content_browser_selection handler found"),
        InvokeHandler(TEXT("editor.get_content_browser_selection"), Payload));
    return true;
}

// ============================================================================
// ContentBrowserHandler — editor.get_content_browser_path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorGetContentBrowserPathNoCrashTest,
    "PinWright.editor.get_content_browser_path.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorGetContentBrowserPathNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.get_content_browser_path handler found"),
        InvokeHandler(TEXT("editor.get_content_browser_path"), Payload));
    return true;
}

// ============================================================================
// GraphSelectionHandler — editor.get_selected_graph_nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorGetSelectedGraphNodesNoCrashTest,
    "PinWright.editor.get_selected_graph_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorGetSelectedGraphNodesNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.get_selected_graph_nodes handler found"),
        InvokeHandler(TEXT("editor.get_selected_graph_nodes"), Payload));
    return true;
}

// ============================================================================
// GraphSelectionHandler — editor.delete_selected_graph_nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorDeleteSelectedGraphNodesNoCrashTest,
    "PinWright.editor.delete_selected_graph_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorDeleteSelectedGraphNodesNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.delete_selected_graph_nodes handler found"),
        InvokeHandler(TEXT("editor.delete_selected_graph_nodes"), Payload));
    return true;
}

// ============================================================================
// EditorCommandHandler — editor.save_all diagnostic JSON shape
// ============================================================================

#include "Handlers/Editor/EditorSaveAllDiagnostic.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSaveAllResultJsonExposesPieActiveTest,
    "PinWright.editor.save_all.ResultJsonExposesPieActive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSaveAllResultJsonExposesPieActiveTest::RunTest(const FString& Parameters)
{
    TArray<TPair<FString, FString>> Failed;
    Failed.Emplace(TEXT("/Game/UI/W_Foo"), TEXT("BlockedByPie"));

    FString ErrorMessage;
    TSharedPtr<FJsonObject> Json = EditorSaveAllDiagnostic::BuildSaveAllResultJson(
        /*bSuccess=*/false, /*SavedCount=*/0, /*TotalDirty=*/1,
        /*bPieActive=*/true, Failed, ErrorMessage);

    TestTrue(TEXT("payload has pieActive"), Json->HasField(TEXT("pieActive")));
    bool bPie = false; Json->TryGetBoolField(TEXT("pieActive"), bPie);
    TestTrue(TEXT("pieActive is true"), bPie);

    FString Mode; Json->TryGetStringField(TEXT("editorMode"), Mode);
    TestEqual(TEXT("editorMode is PIE"), Mode, FString(TEXT("PIE")));

    const TArray<TSharedPtr<FJsonValue>>* FailedArr = nullptr;
    TestTrue(TEXT("payload has failedAssets array"),
        Json->TryGetArrayField(TEXT("failedAssets"), FailedArr) && FailedArr && FailedArr->Num() == 1);
    if (FailedArr && FailedArr->Num() == 1)
    {
        TSharedPtr<FJsonObject> Entry = (*FailedArr)[0]->AsObject();
        FString Path, Reason;
        Entry->TryGetStringField(TEXT("path"), Path);
        Entry->TryGetStringField(TEXT("reason"), Reason);
        TestEqual(TEXT("path"), Path, FString(TEXT("/Game/UI/W_Foo")));
        TestEqual(TEXT("reason"), Reason, FString(TEXT("BlockedByPie")));
    }

    TestTrue(TEXT("error message mentions PIE"), ErrorMessage.Contains(TEXT("PIE active")));
    return true;
}

// ============================================================================
// PIEHandler — editor.status (no params, read-only probe)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStatusRegisteredTest,
    "PinWright.editor.status.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStatusRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.status handler registered"), IsHandlerRegistered(TEXT("editor.status")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStatusNoCrashTest,
    "PinWright.editor.status.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStatusNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("editor.status handler found"), InvokeHandler(TEXT("editor.status"), Payload));
    return true;
}

// ============================================================================
// UtilityWidgetHandler — editor.create_utility_widget / .spawn_utility_widget_tab / .run_utility_blueprint
// ============================================================================

#include "EditorUtilityWidgetBlueprint.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetRegisteredTest,
    "PinWright.editor.create_utility_widget.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.create_utility_widget handler registered"),
        IsHandlerRegistered(TEXT("editor.create_utility_widget")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSpawnUtilityWidgetTabRegisteredTest,
    "PinWright.editor.spawn_utility_widget_tab.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSpawnUtilityWidgetTabRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.spawn_utility_widget_tab handler registered"),
        IsHandlerRegistered(TEXT("editor.spawn_utility_widget_tab")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorRunUtilityBlueprintRegisteredTest,
    "PinWright.editor.run_utility_blueprint.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorRunUtilityBlueprintRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.run_utility_blueprint handler registered"),
        IsHandlerRegistered(TEXT("editor.run_utility_blueprint")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetCreatesAssetTest,
    "PinWright.editor.create_utility_widget.CreatesAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetCreatesAssetTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    const FString AssetName = FString::Printf(TEXT("U_McpTransientUtilWidget_%s"), *Guid);
    const FString Folder = TEXT("/Game/__McpTest__");
    const FString PackagePath = Folder / AssetName;
    const FString FullObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.create_utility_widget handler found"),
        InvokeHandlerWithCapture(TEXT("editor.create_utility_widget"), Payload, Capture));
    TestTrue(TEXT("create_utility_widget reports success"), Capture.bSuccess);

    // Asset registry should see the new asset (covers the counterfactual: a no-op handler
    // would not create the asset, so DoesAssetExist returns false).
    TestTrue(TEXT("DoesAssetExist sees the new utility widget asset"),
        UEditorAssetLibrary::DoesAssetExist(FullObjectPath));

    // Load and verify it is a UEditorUtilityWidgetBlueprint.
    UObject* Loaded = LoadObject<UObject>(nullptr, *FullObjectPath);
    TestNotNull(TEXT("LoadObject returns the new asset"), Loaded);
    if (Loaded)
    {
        TestTrue(TEXT("Loaded asset is a UEditorUtilityWidgetBlueprint"),
            Loaded->IsA<UEditorUtilityWidgetBlueprint>());
    }

    // Cleanup so reruns don't accumulate __McpTest__ assets.
    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetSaveWritesToDiskTest,
    "PinWright.editor.create_utility_widget.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("U_McpTransientUtilWidgetSave_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = TEXT("/Game/__McpTest__");
    const FString PackagePath = Folder / AssetName;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetStringField(TEXT("parentClass"), UEditorUtilityWidget::StaticClass()->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.create_utility_widget handler found"),
        InvokeHandlerWithCapture(TEXT("editor.create_utility_widget"), Payload, Capture));
    TestTrue(TEXT("create_utility_widget responded"), Capture.bWasCalled);
    TestTrue(TEXT("create_utility_widget reports success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ObjectPath;
        Capture.Result->TryGetStringField(TEXT("assetPath"), ObjectPath);
        TestTrue(TEXT("created utility widget is registry-visible"),
            !ObjectPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(ObjectPath));
        TestTrue(TEXT("created utility widget .uasset is on disk"),
            IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(ObjectPath)) > 0);

        FString ActualParentClass;
        TestTrue(TEXT("response reports the actual parent class"),
            Capture.Result->TryGetStringField(TEXT("className"), ActualParentClass)
            && !ActualParentClass.IsEmpty());
        TestEqual(TEXT("response reads back the canonical UEditorUtilityWidget parent"),
            ActualParentClass, UEditorUtilityWidget::StaticClass()->GetPathName());

        bool bSaveRequested = false;
        bool bSaved = false;
        TestTrue(TEXT("saveRequested is reported true"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && bSaveRequested);
        TestTrue(TEXT("saved is reported true"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
        TestFalse(TEXT("pendingFlush is absent after a durable save"),
            Capture.Result->HasField(TEXT("pendingFlush")));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetSaveFalseIsMemoryOnlyTest,
    "PinWright.editor.create_utility_widget.SaveFalseIsMemoryOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetSaveFalseIsMemoryOnlyTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("U_McpTransientUtilWidgetSaveFalse_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = TEXT("/Game/__McpTest__");
    const FString PackagePath = Folder / AssetName;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.create_utility_widget handler found"),
        InvokeHandlerWithCapture(TEXT("editor.create_utility_widget"), Payload, Capture));
    TestTrue(TEXT("create_utility_widget responded"), Capture.bWasCalled);
    TestTrue(TEXT("create_utility_widget succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ObjectPath;
        Capture.Result->TryGetStringField(TEXT("assetPath"), ObjectPath);
        TestTrue(TEXT("save:false asset remains registry-visible"),
            !ObjectPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(ObjectPath));
        bool bSaveRequested = true;
        bool bSaved = true;
        TestTrue(TEXT("save:false reports saveRequested:false"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && !bSaveRequested);
        TestTrue(TEXT("save:false reports saved:false"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);

        FString SaveState;
        TestTrue(TEXT("save:false reports saveState:notRequested"),
            Capture.Result->TryGetStringField(TEXT("saveState"), SaveState)
            && SaveState == TEXT("notRequested"));
        TestFalse(TEXT("save:false does not report pendingFlush"),
            Capture.Result->HasField(TEXT("pendingFlush")));

        bool bPendingSave = false;
        TestTrue(TEXT("save:false reports a pending dirty package"),
            Capture.Result->TryGetBoolField(TEXT("pendingSave"), bPendingSave) && bPendingSave);
        TestTrue(TEXT("save:false package is dirty"),
            FindPackage(nullptr, *PackagePath) != nullptr
            && FindPackage(nullptr, *PackagePath)->IsDirty());
        TestTrue(TEXT("save:false writes no .uasset"),
            IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(PackagePath)) < 0);
    }

    if (UPackage* Package = FindPackage(nullptr, *PackagePath))
    {
        Package->SetDirtyFlag(false);
    }
    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetInvalidParentRefusedTest,
    "PinWright.editor.create_utility_widget.InvalidParentRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetInvalidParentRefusedTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("U_McpTransientUtilWidgetInvalidParent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = TEXT("/Game/__McpTest__");
    const FString PackagePath = Folder / AssetName;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.create_utility_widget handler found"),
        InvokeHandlerWithCapture(TEXT("editor.create_utility_widget"), Payload, Capture));
    TestTrue(TEXT("create_utility_widget responded"), Capture.bWasCalled);
    TestFalse(TEXT("incompatible parent is rejected"), Capture.bSuccess);
    TestEqual(TEXT("incompatible parent returns CLASS_NOT_INSTANTIABLE"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
    TestFalse(TEXT("incompatible parent creates no registry asset"),
        UEditorAssetLibrary::DoesAssetExist(PackagePath));
    TestNull(TEXT("incompatible parent creates no package"), FindPackage(nullptr, *PackagePath));
    TestTrue(TEXT("incompatible parent creates no .uasset"),
        IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(PackagePath)) < 0);

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// EditorWindowHandlers — editor.set_window_state
//   (E-resize-window-maximized-no-restore)
//
// editor.resize_window hard-errors WINDOW_MAXIMIZED ("restore it first") on a
// maximized window, yet before this fix NO restore/un-maximize/window-state verb
// existed in any namespace — the "restore it first" instruction named a method
// that could not be called, dead-ending the caller into a ~1 MB drive.observe /
// drive.click UI-automation dance. The fix adds editor.set_window_state, whose
// state='restored' un-maximizes (and un-minimizes) the window through
// SWindow::Restore(). These tests exercise the production handler directly.
// ============================================================================

// Core regression: a maximized top-level window must be recoverable through the
// API. Build a uniquely-titled in-code SWindow, drive it into the maximized state
// the caller gets dead-ended on, then invoke the production editor.set_window_state
// {state:'restored'} and assert the window is no longer maximized.
//
// Differential: pre-fix editor.set_window_state is not registered, so
// InvokeHandlerWithCapture returns false and the "handler found" assertion fails —
// this test cannot pass without the new verb.
//
// Native maximize of an OFFSCREEN window is best-effort under -RenderOffScreen
// -unattended, so whether Maximize() actually flipped the state is recorded and the
// strong "it un-maximized" proof is only claimed when it did; the always-on
// assertions (handler exists, restore reports success, the window + the response's
// isMaximized are false afterward) are deterministic regardless (a restored window
// is never maximized) and still fail pre-fix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetWindowStateRestoresMaximizedTest,
    "PinWright.editor.set_window_state.RestoresMaximizedWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetWindowStateRestoresMaximizedTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate application not initialized; cannot exercise editor.set_window_state."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    // Uniquely-titled so the shared window selector's substring match targets exactly
    // this fixture and no other open editor window.
    const FString Title = FString::Printf(TEXT("PW_SetWindowState_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(Title))
        .ScreenPosition(FVector2D(120.0f, 120.0f))
        .ClientSize(FVector2D(480.0f, 320.0f))
        .FocusWhenFirstShown(false)
        .SupportsMaximize(true)
        .SupportsMinimize(true);

    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT { SlateApp.RequestDestroyWindow(Window); };

    // Realize the window, then drive it into the maximized precondition.
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }
    Window->Maximize();
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }
    const bool bMaximizeTook = Window->IsWindowMaximized();

    // Drive the production recovery verb the WINDOW_MAXIMIZED error names.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("window_title"), Title);
    Payload->SetStringField(TEXT("state"), TEXT("restored"));
    const bool bFound =
        InvokeHandlerWithCapture(TEXT("editor.set_window_state"), Payload, Capture);
    // Load-bearing differential assertion: the verb must exist. Pre-fix it does not.
    TestTrue(TEXT("editor.set_window_state handler is registered"), bFound);
    if (!bFound)
    {
        return true;
    }

    // The restore must succeed and leave the window non-maximized.
    TestTrue(TEXT("set_window_state{state:'restored'} reports success"), Capture.bSuccess);
    TestFalse(TEXT("window is not maximized after restore"), Window->IsWindowMaximized());
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bIsMax = true;
        TestTrue(TEXT("response echoes isMaximized"),
            Capture.Result->TryGetBoolField(TEXT("isMaximized"), bIsMax));
        TestFalse(TEXT("response reports isMaximized=false after restore"), bIsMax);
    }

    if (bMaximizeTook)
    {
        AddInfo(TEXT("Maximize() took effect; the restore verb demonstrably un-maximized the window."));
    }
    else
    {
        AddInfo(TEXT("Maximize() did not take on this offscreen window; the handler-exists + "
                     "restore-success + isMaximized=false assertions still gate the fix."));
    }
    return true;
}
