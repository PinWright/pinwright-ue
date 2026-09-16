// Copyright (c) 2026 Alexander Penkin. MIT License.

// The dispatcher's PATH-SEPARATOR gate: FRpcDispatcher::ValidateHandlerParams refuses a value
// containing '//' in a parameter declared `path` or `classref`, above every handler body.
//
// WHY THIS ONE IS FULLY TESTABLE WHEN THE REST OF THE TICKET IS NOT. The sibling guards on this
// board sit over a LOAD, and the precedent
// (Tests/Sequencer/TestSequencerExportAnimSequencePathSafety.cpp) states the limit plainly: no wire
// payload can discriminate a build with such a check from one without it, because the shapes that
// WOULD discriminate end the process on the build without it. This gate is different. The refusal
// happens in the dispatcher, before the handler is entered at all, so a '//' payload never reaches a
// loader on EITHER build - the fixture body below simply does not run, and the assertion is on the
// error code and the untouched call counter.
//
// BOTH DIRECTIONS, AND THE ACCEPT HALF IS THE LARGER ONE ON PURPOSE. A gate that refuses too much is
// the failure mode that breaks working callers, and the shapes at risk are exactly the ones a
// "thorough" guard would reject: FPackageName::IsValidLongPackageName refuses a leading-slash-less
// short name AND refuses '.' (INVALID_LONGPACKAGE_CHARACTERS, NameTypes.h), so using it here would
// refuse `PointLight`, `/Script/UMG.UserWidget` and `/Game/BP/BP_X.BP_X_C` - roughly half the shapes
// ClassUtils::ResolveUClass documents, across ~45 verbs. Every one of those is pinned below, as is
// the UNC case that is the entire reason `filepath` exists as a separate token.
//
// WHY THESE TESTS ROUTE THROUGH DispatcherTestHelpers AND NOT InvokeHandler. Tests/TestUtils.h's
// InvokeHandler calls Reg.Func(Ctx) straight out of the registration list and reaches neither call
// site of ValidateHandlerParams, so it cannot see this gate at all (board
// B-test-invokehandler-bypasses-param-gate). Every assertion here dispatches a real payload through
// FRpcDispatcher::ProcessRequest.
//
// THE FAILING-BEFORE PROPERTY. Before the gate, every refusal below answered `success` from the
// fixture body - the body reads nothing and reports ok, so nothing else could have refused it. The
// assertions are on the ERROR CODE, so a reverted build fails here rather than passing by accident.

#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test-only registration: one parameter of each path-shaped token, one array-of-paths union, and one
// plain `string` control. The body reads NO parameter - every assertion here is about whether the
// dispatcher lets the body run at all, and a body that reads nothing cannot mask a gate that failed
// to fire. `_test.` verbs are skipped by every registry-wide contract walk.
// ---------------------------------------------------------------------------

static int32 GPathSeparatorGateBodyCallCount = 0;

REGISTER_RPC_HANDLER("_test.path_separator_gate", "_test",
    "Path-separator gate fixture: one slot per path-shaped declared type plus a string control.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Required asset/package/object path"),
        RPC_PARAM_OPT("className", "classref", "Optional class reference"),
        RPC_PARAM_OPT("filePath", "filepath", "Optional path on disk - carries NO '//' rule"),
        RPC_PARAM_OPT("assetPaths", "path|array", "Optional array-of-paths union"),
        RPC_PARAM_OPT("label", "string", "Optional plain string - outside the rule entirely")
    ))
{
    GPathSeparatorGateBodyCallCount++;
    Ctx.SendSuccess(TEXT("path_separator_gate ok"));
    return true;
}

namespace PathSeparatorGateTests
{
    const FString GExpectedCode = TEXT("INVALID_ARGUMENT");
    const FString GCleanPath = TEXT("/Game/PinWrightGateProbe/SM_Clean");

    struct FOutcome
    {
        bool bResponded = false;
        bool bSuccess = false;
        FString ErrorCode;
        FString Message;
    };

    // Dispatch one payload and settle any deferral, mirroring ParamTypeGateTests::DispatchAndSettle:
    // ProcessRequest can park a request on PendingQueue (the safe-point gate, or the Saving/GC gate)
    // and a bare FRpcDispatcher has no subsystem ticker to drain it, so without this the sink stays
    // silent and every assertion reads a default-constructed capture.
    FOutcome DispatchAndSettle(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
                               const FString& RequestId, const TSharedPtr<FJsonObject>& Payload)
    {
        FOutcome Out;
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.path_separator_gate"),
                                        RequestId, Payload, bSuccess, ErrorCode);
        if (!Sink->bWasCalled)
        {
            Dispatcher.ProcessPendingRequests();
        }

        Out.bResponded = Sink->bWasCalled;
        Out.bSuccess = Sink->bSuccess;
        Out.ErrorCode = Sink->ErrorCode;
        Out.Message = Sink->Message;
        return Out;
    }

    TSharedPtr<FJsonObject> MakePayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), GCleanPath);
        return Payload;
    }

    // Refused, refused as INVALID_ARGUMENT, the message names the slot AND quotes the value the
    // caller sent, and the handler body never ran.
    void TestRefused(FAutomationTestBase& Test, const FString& What,
                     const TSharedPtr<FJsonObject>& Payload, const FString& OffendingLabel,
                     const FString& OffendingValue)
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        GPathSeparatorGateBodyCallCount = 0;
        const FOutcome Out = DispatchAndSettle(
            Dispatcher, Sink, FString::Printf(TEXT("path-gate-%s"), *What), Payload);

        Test.TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), *What), Out.bResponded);
        Test.TestFalse(*FString::Printf(TEXT("%s: response is an error"), *What), Out.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s: error code (message: %s)"), *What, *Out.Message),
                       Out.ErrorCode, GExpectedCode);
        Test.TestTrue(*FString::Printf(TEXT("%s: message names '%s' (message: %s)"),
                                       *What, *OffendingLabel, *Out.Message),
                      Out.Message.Contains(FString::Printf(TEXT("'%s'"), *OffendingLabel)));
        // Load-bearing, not cosmetic: Tests/Gameplay/TestAnimationAuthoringNamePathSafety.cpp
        // asserts the refusal CONTAINS the offending value, and that test only stays green because
        // the refusal moved here.
        Test.TestTrue(*FString::Printf(TEXT("%s: message quotes the offending value (message: %s)"),
                                       *What, *Out.Message),
                      Out.Message.Contains(OffendingValue));
        Test.TestEqual(*FString::Printf(TEXT("%s: handler body did NOT run"), *What),
                       GPathSeparatorGateBodyCallCount, 0);
    }

    // Accepted, and the body ran - the discriminator against a gate that refuses too much.
    void TestAccepted(FAutomationTestBase& Test, const FString& What,
                      const TSharedPtr<FJsonObject>& Payload)
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        GPathSeparatorGateBodyCallCount = 0;
        const FOutcome Out = DispatchAndSettle(
            Dispatcher, Sink, FString::Printf(TEXT("path-gate-ok-%s"), *What), Payload);

        Test.TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), *What), Out.bResponded);
        Test.TestTrue(*FString::Printf(TEXT("%s: accepted (code: %s, message: %s)"),
                                       *What, *Out.ErrorCode, *Out.Message), Out.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s: handler body ran"), *What),
                       GPathSeparatorGateBodyCallCount, 1);
    }
}

// ============================================================================
// A '//' in a path- or classref-typed parameter is refused
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathSeparatorGateRefusesDoubleSlashTest,
    "PinWright.infra.dispatcher.PathParamGate.RefusesDoubledSlashInPathTypedParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathSeparatorGateRefusesDoubleSlashTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the three passes
    // around it do. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to TRUE, so
    // a warning raised inside a running test is promoted to an error.
    bSuppressLogWarnings = true;

    // The exact shape that killed a live editor: '//' in an asset path reaches CreatePackage
    // (UObjectGlobals.cpp:1094-1096), which logs Fatal and ends the process.
    {
        const FString Bad = TEXT("/Game//Props/SM_Wall");
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Bad);
        PathSeparatorGateTests::TestRefused(*this, TEXT("path-required"), Payload,
                                            TEXT("assetPath"), Bad);
    }

    // A classref slot carries the same rule and nothing else.
    {
        const FString Bad = TEXT("/Script//Engine.PointLight");
        TSharedPtr<FJsonObject> Payload = PathSeparatorGateTests::MakePayload();
        Payload->SetStringField(TEXT("className"), Bad);
        PathSeparatorGateTests::TestRefused(*this, TEXT("classref"), Payload,
                                            TEXT("className"), Bad);
    }

    // Lethal WITHOUT a leading slash and WITHOUT a dot: ResolveName2 returns immediately when there
    // is no delimiter, and StaticLoadObjectInternal (:1474-1482) then re-enters itself with
    // `InName + "." + GetShortName(InName)` - the second pass has the dot. LoadObject(nullptr,
    // TEXT("A//B")) is an editor kill, which is why the rule is not conditioned on either.
    {
        const FString Bad = TEXT("A//B");
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Bad);
        PathSeparatorGateTests::TestRefused(*this, TEXT("bare-name"), Payload,
                                            TEXT("assetPath"), Bad);
    }

    // Three or more slashes is the same fault, not a different one.
    {
        const FString Bad = TEXT("/Game///Props/SM_Wall");
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Bad);
        PathSeparatorGateTests::TestRefused(*this, TEXT("triple-slash"), Payload,
                                            TEXT("assetPath"), Bad);
    }

    return true;
}

// ============================================================================
// An array-of-paths union checks its string ELEMENTS, and names the index
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathSeparatorGateArrayElementsTest,
    "PinWright.infra.dispatcher.PathParamGate.RefusesDoubledSlashInArrayElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathSeparatorGateArrayElementsTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    // `path|array` is how an array-of-paths slot is spelled (assetPaths, and its siblings across
    // asset.* and source_control.*). Without element-level checking the union would be decoration:
    // the whole value is an array, so a string-level rule would never look inside it.
    const FString Bad = TEXT("/Game//Props/SM_Second");
    TArray<TSharedPtr<FJsonValue>> Paths;
    Paths.Add(MakeShared<FJsonValueString>(PathSeparatorGateTests::GCleanPath));
    Paths.Add(MakeShared<FJsonValueString>(Bad));

    TSharedPtr<FJsonObject> Payload = PathSeparatorGateTests::MakePayload();
    Payload->SetArrayField(TEXT("assetPaths"), Paths);

    // The index is in the label so a caller with a 200-entry batch is told WHICH entry to fix.
    PathSeparatorGateTests::TestRefused(*this, TEXT("array-element"), Payload,
                                        TEXT("assetPaths[1]"), Bad);

    // A clean array is untouched.
    TArray<TSharedPtr<FJsonValue>> CleanPaths;
    CleanPaths.Add(MakeShared<FJsonValueString>(PathSeparatorGateTests::GCleanPath));
    CleanPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Other/SM_Fine")));

    TSharedPtr<FJsonObject> CleanPayload = PathSeparatorGateTests::MakePayload();
    CleanPayload->SetArrayField(TEXT("assetPaths"), CleanPaths);
    PathSeparatorGateTests::TestAccepted(*this, TEXT("clean-array"), CleanPayload);

    return true;
}

// ============================================================================
// Every legitimate path shape is still accepted - the direction that breaks callers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathSeparatorGateAcceptsLegitimateShapesTest,
    "PinWright.infra.dispatcher.PathParamGate.AcceptsEveryLegitimatePathShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathSeparatorGateAcceptsLegitimateShapesTest::RunTest(const FString& Parameters)
{
    // Each of these is a shape FPackageName::IsValidLongPackageName would REFUSE, and each is a
    // documented input to ClassUtils::ResolveUClass or to an object-path reader. This test is what
    // stops a later "make it thorough" tightening from breaking ~45 verbs.
    struct FCase
    {
        const TCHAR* What;
        const TCHAR* Field;
        const TCHAR* Value;
    };

    const FCase Cases[] = {
        // A bare short class name has no leading slash at all.
        { TEXT("bare-short-class-name"), TEXT("className"), TEXT("PointLight") },
        // A /Script object path carries a '.', which is in INVALID_LONGPACKAGE_CHARACTERS.
        { TEXT("script-object-path"), TEXT("className"), TEXT("/Script/UMG.UserWidget") },
        // A Blueprint asset path, and its generated-class form.
        { TEXT("blueprint-asset-path"), TEXT("className"), TEXT("/Game/BP/BP_Door") },
        { TEXT("generated-class-path"), TEXT("className"), TEXT("/Game/BP/BP_Door.BP_Door_C") },
        // A plugin mount is a perfectly ordinary root.
        { TEXT("plugin-mount"), TEXT("assetPath"), TEXT("/MyPlugin/Meshes/SM_Part") },
        // An object path: package, dot, object.
        { TEXT("object-path"), TEXT("assetPath"), TEXT("/Game/A/B.B") },
        // A SUBOBJECT path adds ':', which is also outside the long-package character set.
        { TEXT("subobject-path"), TEXT("assetPath"), TEXT("/Game/A/B.B:Component") },
        // A single trailing slash is not a doubled separator. Pinned because
        // Tests/Gameplay/TestAnimationAuthoringNamePathSafety.cpp requires a trailing-slash folder
        // to keep reaching the handler rather than being refused here.
        { TEXT("trailing-slash"), TEXT("assetPath"), TEXT("/Game/Animations/") },
        // The UNC case, and the entire reason `filepath` is a separate token: a disk path
        // normalises to //server/share, so the rule must not apply to it.
        { TEXT("unc-filepath"), TEXT("filePath"), TEXT("//buildserver/share/Exports/mesh.fbx") },
        // A plain `string` slot is outside the rule: the gate is keyed on the DECLARED type, not on
        // the value, so a '//' in a label is none of its business.
        { TEXT("string-slot-with-double-slash"), TEXT("label"), TEXT("see http://example.test") },
    };

    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = PathSeparatorGateTests::MakePayload();
        Payload->SetStringField(Case.Field, Case.Value);
        PathSeparatorGateTests::TestAccepted(*this, FString(Case.What), Payload);
    }

    return true;
}

// ============================================================================
// The pass sits between the shape pass and the nested-key pass
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathSeparatorGateOrderingTest,
    "PinWright.infra.dispatcher.PathParamGate.EarlierPassesWinOverPathFaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathSeparatorGateOrderingTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // MISSING-REQUIRED wins. A caller who omitted the required slot must be told that, not told
    // something about a different key - the omission is the fault they have to act on first.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("/Script//Engine.PointLight"));

        GPathSeparatorGateBodyCallCount = 0;
        const PathSeparatorGateTests::FOutcome Out = PathSeparatorGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("path-gate-order-missing"), Payload);

        TestEqual(*FString::Printf(
            TEXT("a missing required param wins over a '//' fault on another key (message: %s)"),
            *Out.Message), Out.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    }

    // SHAPE wins. "'assetPath' contains //" is not an answer about a value the caller sent as an
    // object; the shape complaint is. This is what pins the pass BELOW the declared-type gate.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("assetPath"), MakeShared<FJsonObject>());
        Payload->SetStringField(TEXT("className"), TEXT("/Script//Engine.PointLight"));

        GPathSeparatorGateBodyCallCount = 0;
        const PathSeparatorGateTests::FOutcome Out = PathSeparatorGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("path-gate-order-shape"), Payload);

        TestEqual(*FString::Printf(
            TEXT("a wrong SHAPE wins over a '//' fault (message: %s)"), *Out.Message),
            Out.ErrorCode, FString(TEXT("PARAM_TYPE_MISMATCH")));
    }

    // UNKNOWN-NAME wins, for the same reason it wins over the shape pass: a caller who misspelled a
    // key gets the list of valid ones.
    {
        TSharedPtr<FJsonObject> Payload = PathSeparatorGateTests::MakePayload();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Game//Props/SM_Wall"));
        Payload->SetStringField(TEXT("notAParameter"), TEXT("x"));

        GPathSeparatorGateBodyCallCount = 0;
        const PathSeparatorGateTests::FOutcome Out = PathSeparatorGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("path-gate-order-unknown"), Payload);

        TestEqual(*FString::Printf(
            TEXT("an unknown parameter wins over a '//' fault (message: %s)"), *Out.Message),
            Out.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    return true;
}

// ============================================================================
// Every fault is collected, not just the first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathSeparatorGateCollectsEveryFaultTest,
    "PinWright.infra.dispatcher.PathParamGate.CollectsEveryFaultInOneRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathSeparatorGateCollectsEveryFaultTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // One refusal naming both bad slots beats two round trips, which is the same reason the
    // unknown-name and shape passes list every fault they find.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game//Props/SM_Wall"));
    Payload->SetStringField(TEXT("className"), TEXT("/Script//Engine.PointLight"));

    GPathSeparatorGateBodyCallCount = 0;
    const PathSeparatorGateTests::FOutcome Out = PathSeparatorGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("path-gate-collects"), Payload);

    TestEqual(*FString::Printf(TEXT("refused (message: %s)"), *Out.Message),
              Out.ErrorCode, PathSeparatorGateTests::GExpectedCode);
    TestTrue(*FString::Printf(TEXT("names 'assetPath' (message: %s)"), *Out.Message),
             Out.Message.Contains(TEXT("'assetPath'")));
    TestTrue(*FString::Printf(TEXT("names 'className' too (message: %s)"), *Out.Message),
             Out.Message.Contains(TEXT("'className'")));
    TestEqual(TEXT("handler body did NOT run"), GPathSeparatorGateBodyCallCount, 0);

    return true;
}
