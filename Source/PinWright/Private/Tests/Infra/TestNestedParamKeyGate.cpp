// Copyright (c) 2026 Alexander Penkin. MIT License.

// The dispatcher's NESTED-key gate: FRpcDispatcher::ValidateHandlerParams now reads
// FParamSpec::NestedKeys and refuses a key inside an object/array parameter that declares its
// nested schema (board B-declared-param-guard-blind-to-nested-keys, option C). Until this landed,
// the first three passes all iterated `Params->Values` one level deep, so a key nested inside a
// parameter was validated by NOTHING and a caller who sent a plausible one got `success` and a
// silently discarded input.
//
// BOTH DIRECTIONS ARE ASSERTED HERE, and that is the whole design of this file. A gate that
// refuses every undeclared nested key across the 317 object/array-taking verbs would break a large
// fraction of real callers; a gate that refuses nothing is the defect. So:
//   * RefusesUndeclaredNestedKey        - an ADOPTED slot refuses, naming the key and listing the
//                                         accepted ones (object, array element, and alias spelling);
//   * LeavesUndeclaredSlotsUnchecked    - a slot that declares NO nested schema is untouched, with
//                                         a deliberately absurd nested payload;
//   * AcceptsDeclaredNestedKeys         - the declared keys still pass;
//   * AdoptedVerbsRefuseAndSiblingsDoNot- the same two directions on REAL registry verbs, including
//                                         material.authoring.set_material_instance_parameters,
//                                         whose maps are keyed by caller-chosen parameter names and
//                                         which must therefore never adopt;
//   * AdoptionSetIsRatcheted            - the adoption set is enumerated, so a blanket sweep across
//                                         the remaining ~315 parameters cannot land silently. That
//                                         is the guard the ticket asks for: closing an object is a
//                                         compatibility break and lands one parameter at a time.
//
// WHY THESE TESTS ROUTE THROUGH DispatcherTestHelpers AND NOT InvokeHandler. Tests/TestUtils.h's
// InvokeHandler calls Reg.Func(Ctx) straight out of the registration list and reaches neither call
// site of ValidateHandlerParams, so it cannot see this gate at all (board
// B-test-invokehandler-bypasses-param-gate). Every refusal assertion here dispatches a real payload
// through FRpcDispatcher::ProcessRequest.
//
// THE FAILING-BEFORE PROPERTY. Every refusal assertion below answered `success` before the gate
// existed - the fixture body runs and reports ok, and material.authoring.create_material_instance
// answered its own body error. The assertions are on the ERROR CODE, so a pre-gate run fails rather
// than passing by accident.

#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test-only registration. Carries one ADOPTED object slot, one ADOPTED array slot, one ADOPTED slot
// reachable under an alias, and one UNADOPTED object slot - the four shapes the pass switches on.
// The body reads NO parameter: every assertion here is about whether the dispatcher lets the body
// run at all, and a body that reads nothing cannot mask a gate that failed to fire. `_test.` verbs
// are skipped by every registry-wide contract walk (TestContractConsistency.cpp).
// ---------------------------------------------------------------------------

static int32 GNestedParamKeyGateBodyCallCount = 0;

REGISTER_RPC_HANDLER("_test.nested_param_gate", "_test",
    "Nested-key gate fixture: adopted object / array / aliased slots plus one unadopted slot.",
    RPC_PARAMS(
        RPC_PARAM_OPT_NESTED("grid", "object",
            "Adopted object slot: { spacing, extent } and nothing else.",
            TEXT("spacing"), TEXT("extent")),
        RPC_PARAM_OPT_NESTED("states", "array",
            "Adopted array slot: each element is { name, isEntry } and nothing else.",
            TEXT("name"), TEXT("isEntry")),
        FParamSpec{
            TEXT("keepOut"),
            TEXT("object"),
            TEXT("Adopted object slot reachable under the alias keep_out: { min, max }."),
            false,
            TEXT(""),
            TArray<FString>({TEXT("keep_out")}),
            TArray<FParamAliasSpec>(),
            TArray<FString>({TEXT("min"), TEXT("max")})
        },
        RPC_PARAM_OPT("options", "object",
            "UNADOPTED object slot: declares no nested schema, so nothing inside it is checked.")
    ))
{
    GNestedParamKeyGateBodyCallCount++;
    Ctx.SendSuccess(TEXT("nested_param_gate ok"));
    return true;
}

namespace NestedParamKeyGateTests
{
    const FString GExpectedCode = TEXT("UNKNOWN_NESTED_PARAMS");

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
                               const FString& Method, const FString& RequestId,
                               const TSharedPtr<FJsonObject>& Payload)
    {
        FOutcome Out;
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Payload,
                                        bSuccess, ErrorCode);
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

    // Assert one payload is refused by the nested-key gate, naming the offending DOTTED PATH.
    void TestNestedRefused(FAutomationTestBase& Test, const FString& What, const FString& Method,
                           const TSharedPtr<FJsonObject>& Payload, const FString& OffendingPath,
                           int32& BodyCallCount)
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        BodyCallCount = 0;
        const FOutcome Out = DispatchAndSettle(Dispatcher, Sink, Method,
                                               FString::Printf(TEXT("nested-gate-%s"), *What), Payload);

        Test.TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), *What), Out.bResponded);
        Test.TestFalse(*FString::Printf(TEXT("%s: response is an error"), *What), Out.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s: error code (message: %s)"), *What, *Out.Message),
                       Out.ErrorCode, GExpectedCode);
        Test.TestTrue(*FString::Printf(TEXT("%s: message names '%s' (message: %s)"),
                                       *What, *OffendingPath, *Out.Message),
                      Out.Message.Contains(FString::Printf(TEXT("'%s'"), *OffendingPath)));
        Test.TestEqual(*FString::Printf(TEXT("%s: handler body did NOT run"), *What),
                       BodyCallCount, 0);
    }

    TSharedPtr<FJsonObject> MakeGridPayload(const TSharedPtr<FJsonObject>& Grid)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("grid"), Grid);
        return Payload;
    }
}

// ============================================================================
// An undeclared key inside an ADOPTED parameter is refused, and named
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateRefusesTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.RefusesUndeclaredNestedKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateRefusesTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the three passes
    // above it do. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to TRUE, so
    // a warning raised inside a running test is promoted to an error.
    bSuppressLogWarnings = true;

    // The ticket's instance #3, in fixture form: `grid.between` / `grid.lines` / `grid.origin` were
    // invented by reading a description written as a JSON-shaped brace of English, and the verb read
    // none of them - the caller got `success` with `overlays.grid.spacing` echoed back as if it had
    // been their request.
    {
        TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("spacing"), 100.0);
        Grid->SetNumberField(TEXT("between"), 100.0);
        NestedParamKeyGateTests::TestNestedRefused(*this, TEXT("object-key"),
            TEXT("_test.nested_param_gate"), NestedParamKeyGateTests::MakeGridPayload(Grid),
            TEXT("grid.between"), GNestedParamKeyGateBodyCallCount);
    }

    // The message must carry the accepted set too: a refusal that names only the offending key
    // leaves the caller with a second round trip to find the right one.
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("between"), 100.0);

        const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-valid-keys"),
            NestedParamKeyGateTests::MakeGridPayload(Grid));

        TestTrue(*FString::Printf(TEXT("message lists the accepted keys (message: %s)"), *Out.Message),
                 Out.Message.Contains(TEXT("Valid keys: [spacing, extent]")));
    }

    // An ARRAY element carries the same schema, and the message indexes the element - the shape of
    // the ticket's instance #1 (animation.create_state_machine's states[]), where a documented
    // `states[].animation` was read by nothing and reported as `statesCreated: N`.
    {
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("name"), TEXT("Idle"));
        State->SetStringField(TEXT("animation"), TEXT("/Game/Anims/A_Idle"));

        TArray<TSharedPtr<FJsonValue>> States;
        States.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
        States.Add(MakeShared<FJsonValueObject>(State));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("states"), States);

        NestedParamKeyGateTests::TestNestedRefused(*this, TEXT("array-element-key"),
            TEXT("_test.nested_param_gate"), Payload, TEXT("states[1].animation"),
            GNestedParamKeyGateBodyCallCount);
    }

    // The schema belongs to the SLOT, not to one spelling of it: an alias reaches the same gate and
    // the message names the spelling the caller used.
    {
        TSharedPtr<FJsonObject> KeepOut = MakeShared<FJsonObject>();
        KeepOut->SetNumberField(TEXT("min"), 1.0);
        KeepOut->SetNumberField(TEXT("radius"), 5.0);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("keep_out"), KeepOut);

        NestedParamKeyGateTests::TestNestedRefused(*this, TEXT("alias-slot"),
            TEXT("_test.nested_param_gate"), Payload, TEXT("keep_out.radius"),
            GNestedParamKeyGateBodyCallCount);
    }

    return true;
}

// ============================================================================
// A parameter that declares NO nested schema is untouched
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateUnadoptedTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.LeavesUndeclaredSlotsUnchecked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateUnadoptedTest::RunTest(const FString& Parameters)
{
    // This is the half that makes the gate shippable. 317 of 1,220 verbs declare an object/array
    // parameter and only a handful declare what may go inside one; refusing undeclared keys on the
    // rest would turn a documentation gap into a live rejection for every caller of them. An empty
    // FParamSpec::NestedKeys means UNDECLARED, not empty-set.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Deep = MakeShared<FJsonObject>();
    Deep->SetNumberField(TEXT("deeper"), 1.0);

    TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
    Options->SetStringField(TEXT("noSuchKey"), TEXT("x"));
    Options->SetStringField(TEXT("betweenLinesOrigin"), TEXT("y"));
    Options->SetObjectField(TEXT("nested"), Deep);

    // ... alongside an ADOPTED slot filled correctly, so the pass definitely ran on this payload.
    TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
    Grid->SetNumberField(TEXT("spacing"), 100.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("options"), Options);
    Payload->SetObjectField(TEXT("grid"), Grid);

    GNestedParamKeyGateBodyCallCount = 0;
    const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-unadopted"), Payload);

    TestTrue(TEXT("dispatcher responded"), Out.bResponded);
    TestTrue(*FString::Printf(TEXT("undeclared slot accepted (code: %s, message: %s)"),
                              *Out.ErrorCode, *Out.Message), Out.bSuccess);
    TestEqual(TEXT("handler body ran"), GNestedParamKeyGateBodyCallCount, 1);

    // The deeper level of an ADOPTED slot is not checked either: one level down is the level that
    // was declared, and inventing a schema for the level below it is how a gate starts refusing
    // shapes nobody wrote down.
    TSharedPtr<FJsonObject> DeepValue = MakeShared<FJsonObject>();
    DeepValue->SetNumberField(TEXT("undeclaredAtDepthTwo"), 3.0);

    TSharedPtr<FJsonObject> NestedGrid = MakeShared<FJsonObject>();
    NestedGrid->SetObjectField(TEXT("spacing"), DeepValue);

    GNestedParamKeyGateBodyCallCount = 0;
    const NestedParamKeyGateTests::FOutcome DeepOut = NestedParamKeyGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-depth-two"),
        NestedParamKeyGateTests::MakeGridPayload(NestedGrid));

    TestTrue(*FString::Printf(TEXT("depth-two key accepted (code: %s)"), *DeepOut.ErrorCode),
             DeepOut.bSuccess);
    TestEqual(TEXT("handler body ran for the depth-two payload"),
              GNestedParamKeyGateBodyCallCount, 1);

    return true;
}

// ============================================================================
// The declared keys still pass
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateAcceptsTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.AcceptsDeclaredNestedKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateAcceptsTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
    Grid->SetNumberField(TEXT("spacing"), 100.0);
    Grid->SetNumberField(TEXT("extent"), 1000.0);

    TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
    First->SetStringField(TEXT("name"), TEXT("Idle"));
    TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
    Second->SetStringField(TEXT("name"), TEXT("Run"));
    Second->SetBoolField(TEXT("isEntry"), true);

    TArray<TSharedPtr<FJsonValue>> States;
    States.Add(MakeShared<FJsonValueObject>(First));
    States.Add(MakeShared<FJsonValueObject>(Second));

    TSharedPtr<FJsonObject> Payload = NestedParamKeyGateTests::MakeGridPayload(Grid);
    Payload->SetArrayField(TEXT("states"), States);

    GNestedParamKeyGateBodyCallCount = 0;
    const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-accepts"), Payload);

    TestTrue(*FString::Printf(TEXT("declared nested keys accepted (code: %s, message: %s)"),
                              *Out.ErrorCode, *Out.Message), Out.bSuccess);
    TestEqual(TEXT("handler body ran"), GNestedParamKeyGateBodyCallCount, 1);

    // Case is not the discriminator, and that is the accessor's decision rather than this gate's:
    // FJsonObject hashes its keys with FCrc::Strihash_DEPRECATED, so TryGetNumberField("spacing")
    // really does find a "Spacing" the caller sent. Refusing it here would be the gate promising a
    // refusal the reader contradicts.
    TSharedPtr<FJsonObject> MixedCase = MakeShared<FJsonObject>();
    MixedCase->SetNumberField(TEXT("Spacing"), 100.0);

    GNestedParamKeyGateBodyCallCount = 0;
    const NestedParamKeyGateTests::FOutcome MixedOut = NestedParamKeyGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-case"),
        NestedParamKeyGateTests::MakeGridPayload(MixedCase));

    TestTrue(*FString::Printf(TEXT("case-differing nested key accepted (code: %s)"),
                              *MixedOut.ErrorCode), MixedOut.bSuccess);

    return true;
}

// ============================================================================
// Ordering: the three earlier passes still answer first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateOrderingTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.EarlierPassesWinOverNestedFaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateOrderingTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the three passes
    // above it do. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to TRUE, so
    // a warning raised inside a running test is promoted to an error.
    bSuppressLogWarnings = true;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // An unknown TOP-LEVEL name wins: that caller needs the list of valid parameters, not a key
    // complaint about a different slot.
    {
        TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("between"), 1.0);

        TSharedPtr<FJsonObject> Payload = NestedParamKeyGateTests::MakeGridPayload(Grid);
        Payload->SetStringField(TEXT("nosuchparam"), TEXT("x"));

        const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-order-unknown"),
            Payload);

        TestEqual(*FString::Printf(TEXT("unknown-name wins (message: %s)"), *Out.Message),
                  Out.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    // A wrong SHAPE wins too: a key list is not an answer about a value whose shape is already
    // wrong, and complaining about keys inside a `grid` sent as a string names nothing the caller
    // can act on.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("grid"), TEXT("fine"));

        const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("_test.nested_param_gate"), TEXT("nested-gate-order-shape"),
            Payload);

        TestEqual(*FString::Printf(TEXT("type mismatch wins (message: %s)"), *Out.Message),
                  Out.ErrorCode, FString(TEXT("PARAM_TYPE_MISMATCH")));
    }

    return true;
}

// ============================================================================
// The two directions on REAL registry verbs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateRealVerbsTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.AdoptedVerbsRefuseAndSiblingsDoNot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateRealVerbsTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the three passes
    // above it do. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to TRUE, so
    // a warning raised inside a running test is promoted to an error.
    bSuppressLogWarnings = true;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    if (!Dispatcher.GetAutoRegisteredHandlers().Contains(TEXT("material.authoring.create_material_instance"))
        || !Dispatcher.GetAutoRegisteredHandlers().Contains(TEXT("material.authoring.set_material_instance_parameters")))
    {
        AddError(TEXT("The material.authoring instance verbs are not registered - the fixtures these "
                      "assertions are built on have moved, and they would pass vacuously."));
        return false;
    }

    // ADOPTED. The ticket's instance #2: `parameters` is probed only for the four bucket names, so a
    // payload one level too shallow - the correct {r,g,b} shape written where a bucket belongs -
    // matched nothing and returned `success` with an empty applied[] AND an empty failed[]. Two
    // empty arrays were the only evidence the caller got.
    //
    // The parent material path does not exist, so the ONLY way this call can answer
    // UNKNOWN_NESTED_PARAMS is if the dispatcher refused before the body ran: a body that runs
    // answers ASSET_NOT_FOUND from its own LoadObject failure. Nothing is created or saved either
    // way.
    {
        TSharedPtr<FJsonObject> MisNested = MakeShared<FJsonObject>();
        MisNested->SetNumberField(TEXT("r"), 1.0);
        MisNested->SetNumberField(TEXT("g"), 0.0);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MI_PinWrightNestedGateProbe"));
        Payload->SetStringField(TEXT("parentMaterial"),
            TEXT("/Game/PinWrightNestedGateProbe/M_DoesNotExist"));
        Payload->SetObjectField(TEXT("parameters"), MisNested);
        Payload->SetBoolField(TEXT("save"), false);

        const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("material.authoring.create_material_instance"),
            TEXT("nested-gate-material-adopted"), Payload);

        TestTrue(TEXT("dispatcher responded"), Out.bResponded);
        TestFalse(TEXT("response is an error, not a success"), Out.bSuccess);
        TestEqual(*FString::Printf(
                      TEXT("refused by the dispatcher before the body ran (message: %s)"), *Out.Message),
                  Out.ErrorCode, NestedParamKeyGateTests::GExpectedCode);
        TestTrue(*FString::Printf(TEXT("message names 'parameters.r' (message: %s)"), *Out.Message),
                 Out.Message.Contains(TEXT("'parameters.r'")));
    }

    // NOT ADOPTED, and it must never be. set_material_instance_parameters takes the same four maps
    // at the TOP level, and the keys inside each one are caller-chosen material parameter names -
    // there is no closed set to declare, which is exactly why the ticket records its `ParamName`
    // description entries as metavariables rather than promised keys.
    {
        TSharedPtr<FJsonObject> ScalarMap = MakeShared<FJsonObject>();
        ScalarMap->SetNumberField(TEXT("SomeCallerChosenParameter"), 0.5);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"),
            TEXT("/Game/PinWrightNestedGateProbe/MI_DoesNotExist"));
        Payload->SetObjectField(TEXT("scalar"), ScalarMap);
        Payload->SetBoolField(TEXT("save"), false);

        const NestedParamKeyGateTests::FOutcome Out = NestedParamKeyGateTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("material.authoring.set_material_instance_parameters"),
            TEXT("nested-gate-material-unadopted"), Payload);

        TestTrue(TEXT("dispatcher responded"), Out.bResponded);
        TestNotEqual(*FString::Printf(
                         TEXT("a caller-chosen map key is NOT refused as a nested key (message: %s)"),
                         *Out.Message),
                     Out.ErrorCode, NestedParamKeyGateTests::GExpectedCode);
    }

    return true;
}

// ============================================================================
// The adoption set is enumerated, so a blanket sweep cannot land silently
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedParamKeyGateAdoptionRatchetTest,
    "PinWright.infra.dispatcher.NestedParamKeyGate.AdoptionSetIsRatcheted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedParamKeyGateAdoptionRatchetTest::RunTest(const FString& Parameters)
{
    // Closing a nested object is a COMPATIBILITY BREAK - a caller sending a stray nested key gets
    // `success` today and UNKNOWN_NESTED_PARAMS afterwards. The ticket is explicit that this lands
    // one parameter at a time with the description updated in the same commit, never as a sweep
    // across the ~317 object/array-taking verbs. This test is what makes a sweep a deliberate act:
    // adding an adopter fails here until it is written down, together with the fact that its
    // description was updated to say the slot is closed.
    // "<method>:<parameter>", one line per parameter that has adopted the gate.
    static const TArray<FString> ExpectedAdopters = {
        // Board instance #3: grid.between / grid.lines / grid.origin were invented from a brace of
        // English prose and read by nothing; the description now says the brace is the contract.
        TEXT("render.capture_annotated:grid"),
        // Board instance #2: any key outside the four buckets matched nothing and returned success
        // with empty applied[] and failed[]. The caller-chosen parameter names live one level below
        // and are deliberately not checked.
        TEXT("material.authoring.create_material_instance:parameters"),
        // Board instance #1: animation/isExit were advertised but ignored. The convenience verb
        // now promises and accepts only the two fields it actually consumes.
        TEXT("animation.create_state_machine:states"),
        // Concurrent adoption: mapping modifier/trigger entries are closed around the reflected
        // properties bag, whose caller-selected keys deliberately remain open one level below.
        TEXT("input.add_mapping:modifiers"),
        TEXT("input.add_mapping:triggers"),
        // The gate fixture in this file.
        TEXT("_test.nested_param_gate:grid"),
        TEXT("_test.nested_param_gate:states"),
        TEXT("_test.nested_param_gate:keepOut"),
    };

    TArray<FString> Actual;
    TSet<FString> RegisteredMethods;
    int32 ObjectOrArrayParams = 0;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        RegisteredMethods.Add(Reg.MethodName);
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Type.Contains(TEXT("object")) || Spec.Type.Contains(TEXT("array")))
            {
                ObjectOrArrayParams++;
            }
            if (Spec.NestedKeys.Num() > 0)
            {
                Actual.Add(FString::Printf(TEXT("%s:%s"), *Reg.MethodName, *Spec.Name));
            }
        }
    }

    // UNABLE TO FAIL IF the registry is empty or carries no object/array parameter at all - then
    // every assertion below would hold vacuously on a walk that saw nothing.
    if (ObjectOrArrayParams < 100)
    {
        AddError(FString::Printf(
            TEXT("Only %d object/array parameters were walked (expected hundreds). The registration "
                 "walk this test is built on is broken; its result would be vacuous."),
            ObjectOrArrayParams));
        return false;
    }

    // Direction 1: every listed adopter still declares its schema. Losing one silently re-opens a
    // slot the description now promises is closed.
    for (const FString& Expected : ExpectedAdopters)
    {
        FString Method;
        FString ParamName;
        if (!Expected.Split(TEXT(":"), &Method, &ParamName))
        {
            AddError(FString::Printf(TEXT("Malformed adopter entry '%s' (expected method:param)."),
                                     *Expected));
            continue;
        }
        if (!RegisteredMethods.Contains(Method))
        {
            // A method not registered on this host (a gated integration sub-module whose engine
            // plugin is disabled) is skipped rather than failed, as the registry-wide walks do.
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s still declares a nested schema"), *Expected),
                 Actual.Contains(Expected));
    }

    // Direction 2: nothing adopts without being listed here. This is the guard against the blanket
    // sweep the ticket rules out - a new adopter is a live rejection for its callers.
    for (const FString& Found : Actual)
    {
        TestTrue(*FString::Printf(
                     TEXT("%s adopts the nested-key gate and is listed in this test. Adding an "
                          "adopter is a compatibility break: list it here, and update that "
                          "parameter's description in the same commit to say the slot is closed."),
                     *Found),
                 ExpectedAdopters.Contains(Found));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedInputClosureConfirmedCasesTest,
    "PinWright.infra.dispatcher.NestedInputClosure.ConfirmedCasesRefuseBeforeLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedInputClosureConfirmedCasesTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    static const TCHAR* const RequiredMethods[] = {
        TEXT("animation.create_state_machine"),
        TEXT("material.authoring.create_material_instance"),
        TEXT("render.capture_annotated"),
        TEXT("blueprint.add_function"),
        TEXT("networking.create_rpc_function"),
        TEXT("eqs.set_test_filter")
    };
    for (const TCHAR* Method : RequiredMethods)
    {
        if (!Dispatcher.GetAutoRegisteredHandlers().Contains(Method))
        {
            AddError(FString::Printf(TEXT("Required production handler is not registered: %s"), Method));
            return false;
        }
    }

    auto Dispatch = [&Dispatcher, &Sink](const TCHAR* Method, const TCHAR* Id,
                                         const TSharedPtr<FJsonObject>& Payload)
    {
        return NestedParamKeyGateTests::DispatchAndSettle(Dispatcher, Sink, Method, Id, Payload);
    };
    auto ExpectCode = [this, &Dispatch](const TCHAR* Label, const TCHAR* Method,
                                        const TSharedPtr<FJsonObject>& Payload,
                                        const TCHAR* ExpectedCode)
    {
        const NestedParamKeyGateTests::FOutcome Out = Dispatch(Method, Label, Payload);
        TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), Label), Out.bResponded);
        TestFalse(*FString::Printf(TEXT("%s: response is an error"), Label), Out.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s: rejected before missing-asset work (message: %s)"),
            Label, *Out.Message), Out.ErrorCode, FString(ExpectedCode));
    };
    auto ExpectPastValidator = [this, &Dispatch](const TCHAR* Label, const TCHAR* Method,
                                                 const TSharedPtr<FJsonObject>& Payload,
                                                 const TCHAR* ValidatorCode)
    {
        const NestedParamKeyGateTests::FOutcome Out = Dispatch(Method, Label, Payload);
        TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), Label), Out.bResponded);
        TestNotEqual(*FString::Printf(TEXT("%s: valid shape passed its validator (message: %s)"),
            Label, *Out.Message), Out.ErrorCode, FString(ValidatorCode));
    };

    {
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("name"), TEXT("Idle"));
        State->SetStringField(TEXT("animation"), TEXT("/Game/DoesNotExist/A_Idle"));
        TArray<TSharedPtr<FJsonValue>> States = { MakeShared<FJsonValueObject>(State) };
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/DoesNotExist/ABP_Missing"));
        Payload->SetArrayField(TEXT("states"), States);
        ExpectCode(TEXT("animation-stale-state-field"), TEXT("animation.create_state_machine"),
            Payload, TEXT("UNKNOWN_NESTED_PARAMS"));

        State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("name"), TEXT("Idle"));
        State->SetBoolField(TEXT("isEntry"), true);
        States = { MakeShared<FJsonValueObject>(State) };
        Payload->SetArrayField(TEXT("states"), States);
        ExpectPastValidator(TEXT("animation-valid-state-shape"),
            TEXT("animation.create_state_machine"), Payload, TEXT("UNKNOWN_NESTED_PARAMS"));
    }

    {
        TSharedPtr<FJsonObject> ParametersObject = MakeShared<FJsonObject>();
        ParametersObject->SetNumberField(TEXT("r"), 1.0);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MI_NestedClosureProbe"));
        Payload->SetStringField(TEXT("parentMaterial"), TEXT("/Game/DoesNotExist/M_Missing"));
        Payload->SetObjectField(TEXT("parameters"), ParametersObject);
        Payload->SetBoolField(TEXT("save"), false);
        ExpectCode(TEXT("material-unknown-bucket"),
            TEXT("material.authoring.create_material_instance"), Payload,
            TEXT("UNKNOWN_NESTED_PARAMS"));

        ParametersObject = MakeShared<FJsonObject>();
        ParametersObject->SetObjectField(TEXT("scalar"), MakeShared<FJsonObject>());
        Payload->SetObjectField(TEXT("parameters"), ParametersObject);
        ExpectPastValidator(TEXT("material-valid-bucket"),
            TEXT("material.authoring.create_material_instance"), Payload,
            TEXT("UNKNOWN_NESTED_PARAMS"));
    }

    {
        TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("between"), 100.0);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("grid"), Grid);
        ExpectCode(TEXT("render-unknown-grid-field"), TEXT("render.capture_annotated"),
            Payload, TEXT("UNKNOWN_NESTED_PARAMS"));

        Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("spacing"), -1.0);
        Payload->SetObjectField(TEXT("grid"), Grid);
        ExpectCode(TEXT("render-valid-schema-reaches-handler"), TEXT("render.capture_annotated"),
            Payload, TEXT("INVALID_ARGUMENT"));
    }

    auto MakePinPayload = [](const TCHAR* PathField, bool bValidElement, bool bMixedCase)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(PathField, TEXT("/Game/DoesNotExist/BP_Missing"));
        Payload->SetStringField(TEXT("functionName"), TEXT("NestedClosureProbe"));
        TArray<TSharedPtr<FJsonValue>> Inputs;
        if (bValidElement)
        {
            TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
            Pin->SetStringField(bMixedCase ? TEXT("Name") : TEXT("name"), TEXT("Damage"));
            Pin->SetStringField(bMixedCase ? TEXT("Type") : TEXT("type"), TEXT("float"));
            Inputs.Add(MakeShared<FJsonValueObject>(Pin));
        }
        else
        {
            Inputs.Add(MakeShared<FJsonValueString>(TEXT("Damage")));
        }
        Payload->SetArrayField(TEXT("inputs"), Inputs);
        return Payload;
    };

    {
        TSharedPtr<FJsonObject> Invalid = MakePinPayload(TEXT("path"), false, false);
        ExpectCode(TEXT("blueprint-non-object-pin"), TEXT("blueprint.add_function"), Invalid,
            TEXT("INVALID_ARGUMENT"));
        TSharedPtr<FJsonObject> Valid = MakePinPayload(TEXT("path"), true, true);
        ExpectPastValidator(TEXT("blueprint-mixed-case-valid-pin"), TEXT("blueprint.add_function"), Valid,
            TEXT("INVALID_ARGUMENT"));
    }

    {
        TSharedPtr<FJsonObject> Invalid = MakePinPayload(TEXT("blueprintPath"), false, false);
        Invalid->SetStringField(TEXT("rpcType"), TEXT("Server"));
        ExpectCode(TEXT("networking-non-object-pin"), TEXT("networking.create_rpc_function"),
            Invalid, TEXT("INVALID_ARGUMENT"));
        TSharedPtr<FJsonObject> Valid = MakePinPayload(TEXT("blueprintPath"), true, false);
        Valid->SetStringField(TEXT("rpcType"), TEXT("Server"));
        ExpectPastValidator(TEXT("networking-valid-pin"), TEXT("networking.create_rpc_function"),
            Valid, TEXT("INVALID_ARGUMENT"));
    }

    {
        auto MakeEqsPayload = [](const TSharedPtr<FJsonObject>& Filter)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("queryPath"), TEXT("/Game/DoesNotExist/EQS_Missing"));
            Payload->SetNumberField(TEXT("generatorIndex"), 0.0);
            Payload->SetNumberField(TEXT("testIndex"), 0.0);
            Payload->SetObjectField(TEXT("filter"), Filter);
            return Payload;
        };
        auto MakeFilter = [](const TCHAR* Kind, bool bValue, bool bMin, bool bMax,
                             bool bMixedCase = false)
        {
            TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
            Filter->SetStringField(bMixedCase ? TEXT("KiNd") : TEXT("kind"), Kind);
            if (bValue)
            {
                Filter->SetBoolField(bMixedCase ? TEXT("VaLuE") : TEXT("value"), true);
            }
            if (bMin)
            {
                Filter->SetNumberField(TEXT("min"), 1.0);
            }
            if (bMax)
            {
                Filter->SetNumberField(TEXT("max"), 2.0);
            }
            return Filter;
        };
        ExpectCode(TEXT("eqs-off-branch-field"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("bool"), true, true, false)), TEXT("INVALID_ARGUMENT"));

        TSharedPtr<FJsonObject> Conflicting = MakeEqsPayload(MakeFilter(TEXT("bool"), true, false, false));
        const TSharedPtr<FJsonObject>* ConflictingFilter = nullptr;
        Conflicting->TryGetObjectField(TEXT("filter"), ConflictingFilter);
        (*ConflictingFilter)->SetStringField(TEXT("filterType"), TEXT("match"));
        ExpectCode(TEXT("eqs-conflicting-discriminators"), TEXT("eqs.set_test_filter"),
            Conflicting, TEXT("INVALID_ARGUMENT"));

        ExpectCode(TEXT("eqs-match-requires-value"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("bool"), false, false, false)), TEXT("INVALID_ARGUMENT"));
        ExpectCode(TEXT("eqs-minimum-requires-min"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("minimum"), false, false, false)), TEXT("INVALID_ARGUMENT"));
        ExpectCode(TEXT("eqs-maximum-requires-max"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("maximum"), false, false, false)), TEXT("INVALID_ARGUMENT"));
        ExpectCode(TEXT("eqs-range-requires-max"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("range"), false, true, false)), TEXT("INVALID_ARGUMENT"));
        ExpectCode(TEXT("eqs-range-requires-min"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("range"), false, false, true)), TEXT("INVALID_ARGUMENT"));

        ExpectPastValidator(TEXT("eqs-valid-filter"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("bool"), true, false, false)), TEXT("INVALID_ARGUMENT"));
        ExpectPastValidator(TEXT("eqs-mixed-case-valid-filter"), TEXT("eqs.set_test_filter"),
            MakeEqsPayload(MakeFilter(TEXT("bool"), true, false, false, true)), TEXT("INVALID_ARGUMENT"));
    }

    return true;
}
