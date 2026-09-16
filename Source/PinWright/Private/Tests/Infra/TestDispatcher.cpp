// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FRpcDispatcher: register+dispatch, unknown method, reentrancy guard
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/LogUtils.h"
#include "Logging/LogScopedVerbosityOverride.h"
#include "Misc/OutputDeviceRedirector.h"
#include <stdexcept>

// Test-only handler with a required param that sets a static flag when invoked.
// Used by auto-validation tests to detect whether the handler body actually ran.
static int32 GAutoValidateHandlerCallCount = 0;

REGISTER_RPC_HANDLER("_test.require_name", "_test", "Handler requiring name param",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Required name"),
        RPC_PARAM_OPT("tag", "string", "Optional tag")
    ))
{
    GAutoValidateHandlerCallCount++;
    Ctx.SendSuccess(TEXT("require_name ok"));
    return true;
}

// Test-only handler with a required canonical path and wire aliases.
static int32 GAutoValidateAliasHandlerCallCount = 0;

REGISTER_RPC_HANDLER("_test.require_path_alias", "_test", "Handler requiring path alias param",
    RPC_PARAMS(
        FParamSpec{
            TEXT("path"),
            TEXT("string"),
            TEXT("Required path"),
            true,
            TEXT(""),
            TArray<FString>({TEXT("assetPath"), TEXT("blueprintPath")}),
            TArray<FParamAliasSpec>({
                FParamAliasSpec{TEXT("blueprintCandidates"), TEXT("array"), TEXT("Candidate Blueprint paths")}
            })
        }
    ))
{
    GAutoValidateAliasHandlerCallCount++;
    Ctx.SendSuccess(TEXT("require_path_alias ok"));
    return true;
}

// Test-only handler with no required params.
static int32 GOptionalOnlyHandlerCallCount = 0;

REGISTER_RPC_HANDLER("_test.optional_only", "_test", "Handler with optional params only",
    RPC_PARAMS(
        RPC_PARAM_OPT("mode", "string", "Optional mode")
    ))
{
    GOptionalOnlyHandlerCallCount++;
    Ctx.SendSuccess(TEXT("optional_only ok"));
    return true;
}

// ============================================================================
// Test: Register a handler, dispatch a request, verify response
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherRegisterAndDispatchTest,
    "PinWright.infra.dispatcher.RegisterAndDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherRegisterAndDispatchTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;

    // We do not need a transport for a basic DispatchMethod test -- the handler
    // itself returns true/false without needing to resolve completions.
    bool bHandlerCalled = false;
    FString ReceivedRequestId;
    FString ReceivedAction;

    Dispatcher.RegisterHandler(TEXT("test.echo"),
        [&bHandlerCalled, &ReceivedRequestId, &ReceivedAction]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            bHandlerCalled = true;
            ReceivedRequestId = RequestId;
            ReceivedAction = Action;
            return true;
        });

    // Dispatch via the direct DispatchMethod call (bypasses reentrancy/transport)
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("msg"), TEXT("hello"));

    bool bResult = Dispatcher.DispatchMethod(TEXT("test.echo"), TEXT("req-1"), Params);

    TestTrue(TEXT("DispatchMethod returns true"), bResult);
    TestTrue(TEXT("Handler was called"), bHandlerCalled);
    TestEqual(TEXT("RequestId passed through"), ReceivedRequestId, TEXT("req-1"));
    TestEqual(TEXT("Action passed through"), ReceivedAction, TEXT("test.echo"));

    return true;
}

// ============================================================================
// Test: Dispatch unknown method returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownMethodTest,
    "PinWright.infra.dispatcher.UnknownMethodReturnsFalse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownMethodTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    bool bResult = Dispatcher.DispatchMethod(TEXT("nonexistent.method"), TEXT("req-2"), Params);

    TestFalse(TEXT("Unknown method returns false"), bResult);

    return true;
}

// ============================================================================
// Test: ProcessRequest with transport sends UNKNOWN_ACTION error for missing handler
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownActionErrorTest,
    "PinWright.infra.dispatcher.ProcessRequestUnknownAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownActionErrorTest::RunTest(const FString& Parameters)
{
    // Create a dispatcher with a sink that captures the error response
    bool bCompletionFired = false;
    FString CapturedErrorCode;
    FString CapturedMessage;

    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&bCompletionFired, &CapturedErrorCode, &CapturedMessage]
        (const FString& RequestId, bool bSuccess, const FString& Message,
         const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
        {
            bCompletionFired = true;
            CapturedErrorCode = ErrorCode;
            CapturedMessage = Message;
        }));

    // Process a request for a non-existent method
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Dispatcher.ProcessRequest(TEXT("req-unknown"), TEXT("does.not.exist"), Params);

    TestTrue(TEXT("Completion fired"), bCompletionFired);
    TestEqual(TEXT("Error code is UNKNOWN_ACTION"), CapturedErrorCode, TEXT("UNKNOWN_ACTION"));
    TestTrue(TEXT("Message mentions the method"),
        CapturedMessage.Contains(TEXT("does.not.exist")));

    return true;
}

// ============================================================================
// Test: UNKNOWN_ACTION for a near-miss method offers "Did you mean:" suggestions
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownActionSuggestionsTest,
    "PinWright.infra.dispatcher.UnknownActionSuggestions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownActionSuggestionsTest::RunTest(const FString& Parameters)
{
    // Drain the production auto-registrations so asset.* method names populate the
    // suggestion pool the miss site ranks against. The shared sink capture carries
    // the message, so no manual completion registration is needed.
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // "asset.find" is not a registered verb, but "asset.find_by_tag" contains it.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Dispatcher.ProcessRequest(TEXT("req-suggest"), TEXT("asset.find"), Params);

    TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
    TestEqual(TEXT("Error code is UNKNOWN_ACTION"), Sink->ErrorCode, TEXT("UNKNOWN_ACTION"));
    TestTrue(TEXT("Message offers suggestions"),
        Sink->Message.Contains(TEXT("Did you mean:")));
    TestTrue(TEXT("Suggestions include asset.find_by_tag"),
        Sink->Message.Contains(TEXT("asset.find_by_tag")));

    return true;
}

// ============================================================================
// Test: UNKNOWN_ACTION for pure garbage offers no suggestions
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownActionNoCloseMatchTest,
    "PinWright.infra.dispatcher.UnknownActionNoCloseMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownActionNoCloseMatchTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // `~` is the load-bearing choice, not the length. Registered method names are
    // dotted lower snake_case - the live registry's whole alphabet is [a-z0-9._-] -
    // so a run of `~` shares NO character with any candidate, which pins the
    // similarity at its disjoint-pair ceiling: Levenshtein distance is exactly
    // max(LenQuery, LenName), and the normalised score is therefore <= 0.5, hitting
    // 0.5 exactly when the two lengths coincide. That is the property under test, and
    // it holds whatever is in the registry.
    //
    // The previous input was 64 `z`s justified by "64 chars exceeds every registered
    // method name" - a guess about the registry, not a property of the ranker. It
    // expired the day a 64-character verb was registered
    // (material.authoring.set_material_instance_base_property_overrides), which tied
    // the query's length exactly and put it on the 0.5 boundary that `Sim >= 0.5`
    // admitted. Fixed in SuggestionHelpers.h by making the cutoff strict.
    //
    // Several lengths because the 0.5 boundary is reachable only where the query's
    // length TIES a candidate's. At the time of writing 16, 32 and 64 characters tie
    // 33, 43 and 1 registered verbs respectively, so a `>=` cutoff answers all three;
    // 8 is shorter than every registered verb and answers nothing either way. Sweeping
    // them together keeps the assertion from quietly going vacuous when the registry's
    // length distribution shifts - which is exactly how the single-input version died.
    const int32 GarbageLengths[] = { 8, 16, 32, 64 };
    for (const int32 Length : GarbageLengths)
    {
        const FString GarbageMethod = FString::ChrN(Length, TEXT('~'));
        *Sink = DispatcherTestHelpers::FSinkCapture();
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Dispatcher.ProcessRequest(TEXT("req-nomatch"), GarbageMethod, Params);

        TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
        TestEqual(TEXT("Error code is UNKNOWN_ACTION"), Sink->ErrorCode, TEXT("UNKNOWN_ACTION"));
        TestTrue(*FString::Printf(TEXT("Message mentions unknown action (%d chars)"), Length),
            Sink->Message.Contains(TEXT("Unknown action:")));

        // The message rides along in the failure text: "expected false, got true"
        // otherwise costs a rerun to learn WHICH verb the garbage matched.
        TestFalse(*FString::Printf(TEXT("%d chars of garbage offers no suggestions. Message: %s"),
                Length, *Sink->Message),
            Sink->Message.Contains(TEXT("Did you mean:")));
    }

    return true;
}

// ============================================================================
// Test: Reentrancy guard defers nested dispatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherReentrancyGuardTest,
    "PinWright.infra.dispatcher.ReentrancyGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherReentrancyGuardTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    // These handlers return true without resolving, so an empty sink suffices.
    Dispatcher.Initialize(FResponseSink(
        [](const FString&, bool, const FString&, const TSharedPtr<FJsonObject>&, const FString&) {}));

    int32 OuterCallCount = 0;
    int32 InnerCallCount = 0;

    // Register an outer handler that tries to process another request recursively
    Dispatcher.RegisterHandler(TEXT("test.outer"),
        [&Dispatcher, &OuterCallCount, &InnerCallCount]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            OuterCallCount++;

            // Attempt to process a nested request (should be deferred, not immediate)
            TSharedPtr<FJsonObject> InnerParams = MakeShared<FJsonObject>();
            Dispatcher.ProcessRequest(TEXT("req-inner"), TEXT("test.inner"), InnerParams);

            return true;
        });

    Dispatcher.RegisterHandler(TEXT("test.inner"),
        [&InnerCallCount]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            InnerCallCount++;
            return true;
        });

    // Process the outer request
    TSharedPtr<FJsonObject> OuterParams = MakeShared<FJsonObject>();
    Dispatcher.ProcessRequest(TEXT("req-outer"), TEXT("test.outer"), OuterParams);

    TestEqual(TEXT("Outer handler called once"), OuterCallCount, 1);

    // The inner request was deferred and then processed after the outer completed
    // (via the ON_SCOPE_EXIT / ProcessPendingRequests mechanism)
    TestEqual(TEXT("Inner handler called once (deferred)"), InnerCallCount, 1);

    return true;
}

// ============================================================================
// Test: GetRegisteredToolKeys returns registered handler keys sorted
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherToolKeysTest,
    "PinWright.infra.dispatcher.GetRegisteredToolKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherToolKeysTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;

    Dispatcher.RegisterHandler(TEXT("b.method"), [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) { return true; });
    Dispatcher.RegisterHandler(TEXT("a.method"), [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) { return true; });
    Dispatcher.RegisterHandler(TEXT("c.method"), [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) { return true; });

    TArray<FString> Keys;
    Dispatcher.GetRegisteredToolKeys(Keys);

    TestEqual(TEXT("3 keys registered"), Keys.Num(), 3);

    // Keys should be sorted alphabetically
    if (Keys.Num() == 3)
    {
        TestEqual(TEXT("First key"), Keys[0], TEXT("a.method"));
        TestEqual(TEXT("Second key"), Keys[1], TEXT("b.method"));
        TestEqual(TEXT("Third key"), Keys[2], TEXT("c.method"));
    }

    return true;
}

// ============================================================================
// Test: Handler returns false → transport gets UNKNOWN_ACTION
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherHandlerReturnsFalseTest,
    "PinWright.infra.dispatcher.HandlerReturnsFalse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherHandlerReturnsFalseTest::RunTest(const FString& Parameters)
{
    FString CapturedErrorCode;
    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&CapturedErrorCode](const FString&, bool, const FString&,
            const TSharedPtr<FJsonObject>&, const FString& ErrorCode)
        {
            CapturedErrorCode = ErrorCode;
        }));

    Dispatcher.RegisterHandler(TEXT("test.decline"),
        [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            return false; // Handler declines to handle
        });

    Dispatcher.ProcessRequest(TEXT("req-decline"), TEXT("test.decline"), MakeShared<FJsonObject>());

    TestEqual(TEXT("Error code is UNKNOWN_ACTION"), CapturedErrorCode, TEXT("UNKNOWN_ACTION"));

    return true;
}

// ============================================================================
// Test: std::exception in handler → INTERNAL_ERROR with message
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherStdExceptionTest,
    "PinWright.infra.dispatcher.ExceptionHandling.StdException",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherStdExceptionTest::RunTest(const FString& Parameters)
{
    FString CapturedErrorCode;
    FString CapturedMessage;
    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&](const FString&, bool, const FString& Message,
            const TSharedPtr<FJsonObject>&, const FString& ErrorCode)
        {
            CapturedErrorCode = ErrorCode;
            CapturedMessage = Message;
        }));

    Dispatcher.RegisterHandler(TEXT("test.throw_std"),
        [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            throw std::runtime_error("test boom");
        });

    bSuppressLogErrors = true;
    Dispatcher.ProcessRequest(TEXT("req-throw"), TEXT("test.throw_std"), MakeShared<FJsonObject>());

    TestEqual(TEXT("Error code is INTERNAL_ERROR"), CapturedErrorCode, TEXT("INTERNAL_ERROR"));
    TestTrue(TEXT("Message contains exception text"), CapturedMessage.Contains(TEXT("test boom")));

    return true;
}

// ============================================================================
// Test: Unknown exception type → INTERNAL_ERROR generic message
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownExceptionTest,
    "PinWright.infra.dispatcher.ExceptionHandling.Unknown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownExceptionTest::RunTest(const FString& Parameters)
{
    FString CapturedErrorCode;
    FString CapturedMessage;
    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&](const FString&, bool, const FString& Message,
            const TSharedPtr<FJsonObject>&, const FString& ErrorCode)
        {
            CapturedErrorCode = ErrorCode;
            CapturedMessage = Message;
        }));

    Dispatcher.RegisterHandler(TEXT("test.throw_int"),
        [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            throw 42;
        });

    bSuppressLogErrors = true;
    Dispatcher.ProcessRequest(TEXT("req-throw-int"), TEXT("test.throw_int"), MakeShared<FJsonObject>());

    TestEqual(TEXT("Error code is INTERNAL_ERROR"), CapturedErrorCode, TEXT("INTERNAL_ERROR"));
    TestTrue(TEXT("Message mentions unknown"), CapturedMessage.Contains(TEXT("unknown")));

    return true;
}

// ============================================================================
// Test: RegisterHandler with null handler → not added to map
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherNullHandlerTest,
    "PinWright.infra.dispatcher.NullHandler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherNullHandlerTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;

    Dispatcher.RegisterHandler(TEXT("test.null"), FAutomationHandler());

    bool bResult = Dispatcher.DispatchMethod(TEXT("test.null"), TEXT("req"), MakeShared<FJsonObject>());
    TestFalse(TEXT("Null handler not dispatched"), bResult);

    return true;
}

// ============================================================================
// Test: Second handler for same name replaces first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherOverwriteTest,
    "PinWright.infra.dispatcher.Overwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherOverwriteTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;

    int32 FirstCallCount = 0;
    int32 SecondCallCount = 0;

    Dispatcher.RegisterHandler(TEXT("test.overwrite"),
        [&FirstCallCount](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            FirstCallCount++;
            return true;
        });

    Dispatcher.RegisterHandler(TEXT("test.overwrite"),
        [&SecondCallCount](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            SecondCallCount++;
            return true;
        });

    Dispatcher.DispatchMethod(TEXT("test.overwrite"), TEXT("req"), MakeShared<FJsonObject>());

    TestEqual(TEXT("First handler not called"), FirstCallCount, 0);
    TestEqual(TEXT("Second handler called"), SecondCallCount, 1);

    return true;
}

// ============================================================================
// Test: Auto-validation rejects request missing required param
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoValidateRejectsMissingParamTest,
    "PinWright.infra.dispatcher.AutoValidate.RejectsMissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoValidateRejectsMissingParamTest::RunTest(const FString& Parameters)
{
    // _test.require_name has RPC_PARAM_REQ("name", "string", ...) — omitting
    // it should block the handler body from running.
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    GAutoValidateHandlerCallCount = 0;

    // Send request WITHOUT the required "name" field
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("tag"), TEXT("optional_value"));

    bool bResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_name"), TEXT("req-missing"), Params);

    // The lambda returns true (error was sent), but the handler body never ran.
    TestTrue(TEXT("Lambda returns true (handled)"), bResult);
    TestEqual(TEXT("Handler body was NOT called"), GAutoValidateHandlerCallCount, 0);

    return true;
}

// ============================================================================
// Test: Auto-validation UNKNOWN_PARAMS discovery guidance
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherUnknownParamsDiscoveryGuidanceTest,
    "PinWright.infra.dispatcher.AutoValidate.UnknownParamsDiscoveryGuidance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherUnknownParamsDiscoveryGuidanceTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    bool bCompletionFired = false;
    bool bCompletionSuccess = true;
    FString CapturedErrorCode;
    FString CapturedMessage;

    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&bCompletionFired, &bCompletionSuccess, &CapturedErrorCode, &CapturedMessage]
        (const FString& RequestId, bool bSuccess, const FString& Message,
         const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
        {
            bCompletionFired = true;
            bCompletionSuccess = bSuccess;
            CapturedErrorCode = ErrorCode;
            CapturedMessage = Message;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);

    GAutoValidateHandlerCallCount = 0;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TEXT("TestValue"));
    Params->SetStringField(TEXT("unexpected"), TEXT("extra"));

    const bool bResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_name"), TEXT("req-unknown-param"), Params);

    TestTrue(TEXT("Request was handled by auto-validation"), bResult);
    TestTrue(TEXT("Completion fired"), bCompletionFired);
    TestFalse(TEXT("Completion reports failure"), bCompletionSuccess);
    TestEqual(TEXT("Error code is UNKNOWN_PARAMS"), CapturedErrorCode, TEXT("UNKNOWN_PARAMS"));
    TestTrue(TEXT("Message mentions method"), CapturedMessage.Contains(TEXT("_test.require_name")));
    TestTrue(TEXT("Message mentions unknown param"), CapturedMessage.Contains(TEXT("unexpected")));
    TestTrue(TEXT("Message mentions valid parameters"), CapturedMessage.Contains(TEXT("Valid parameters")));
    TestTrue(TEXT("Message mentions optional tag param"), CapturedMessage.Contains(TEXT("tag")));
    TestTrue(TEXT("Message mentions omit-args discovery"), CapturedMessage.Contains(TEXT("with no 'args' field")));
    TestTrue(TEXT("Message mentions wiki page discovery"), CapturedMessage.Contains(TEXT("fetch its wiki page")));
    TestFalse(TEXT("Message omits stale question-mark suffix"), CapturedMessage.Contains(TEXT("?")));
    TestEqual(TEXT("Handler body was NOT called"), GAutoValidateHandlerCallCount, 0);

    return true;
}

// ============================================================================
// Test: Auto-validation passes when required param is present
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoValidatePassesWithRequiredParamTest,
    "PinWright.infra.dispatcher.AutoValidate.PassesWithRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoValidatePassesWithRequiredParamTest::RunTest(const FString& Parameters)
{
    // _test.require_name requires "name" — providing it should let the handler
    // body run and increment the call counter.
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    GAutoValidateHandlerCallCount = 0;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TEXT("TestValue"));

    bool bResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_name"), TEXT("req-present"), Params);

    TestTrue(TEXT("Handler returns true"), bResult);
    TestEqual(TEXT("Handler body was called"), GAutoValidateHandlerCallCount, 1);

    return true;
}

// ============================================================================
// Test: Auto-validation accepts aliases for required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoValidateAcceptsRequiredParamAliasTest,
    "PinWright.infra.dispatcher.AutoValidate.AcceptsRequiredParamAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoValidateAcceptsRequiredParamAliasTest::RunTest(const FString& Parameters)
{
    // _test.require_path_alias requires canonical "path"; alias-only payloads
    // should pass dispatcher validation and reach the handler body.
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    GAutoValidateAliasHandlerCallCount = 0;

    TSharedPtr<FJsonObject> AssetPathParams = MakeShared<FJsonObject>();
    AssetPathParams->SetStringField(TEXT("assetPath"), TEXT("/Game/Test/BP_Test"));
    bool bAssetPathResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_path_alias"), TEXT("req-alias-assetpath"), AssetPathParams);

    TestTrue(TEXT("assetPath alias request is handled"), bAssetPathResult);
    TestEqual(TEXT("assetPath alias invoked handler"), GAutoValidateAliasHandlerCallCount, 1);

    TSharedPtr<FJsonObject> BlueprintPathParams = MakeShared<FJsonObject>();
    BlueprintPathParams->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_Test"));
    bool bBlueprintPathResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_path_alias"), TEXT("req-alias-blueprintpath"), BlueprintPathParams);

    TestTrue(TEXT("blueprintPath alias request is handled"), bBlueprintPathResult);
    TestEqual(TEXT("blueprintPath alias invoked handler"), GAutoValidateAliasHandlerCallCount, 2);

    TSharedPtr<FJsonObject> CandidateParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Candidates;
    Candidates.Add(MakeShared<FJsonValueString>(TEXT("/Game/Test/BP_Test")));
    CandidateParams->SetArrayField(TEXT("blueprintCandidates"), Candidates);
    bool bCandidateResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_path_alias"), TEXT("req-alias-blueprintcandidates"), CandidateParams);

    TestTrue(TEXT("typed array alias request is handled"), bCandidateResult);
    TestEqual(TEXT("typed array alias invoked handler"), GAutoValidateAliasHandlerCallCount, 3);

    return true;
}

// ============================================================================
// Test: blueprint.modify_scs declares Blueprint path aliases
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherBlueprintModifyScsPathAliasMetadataTest,
    "PinWright.infra.dispatcher.AutoValidate.BlueprintModifyScsPathAliasMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherBlueprintModifyScsPathAliasMetadataTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    const FHandlerRegistration* Registration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("blueprint.modify_scs"));
    if (!TestNotNull(TEXT("blueprint.modify_scs is auto-registered"), Registration))
    {
        return false;
    }

    const FParamSpec* PathParam = nullptr;
    for (const FParamSpec& Param : Registration->Params)
    {
        if (Param.Name == TEXT("path"))
        {
            PathParam = &Param;
            break;
        }
    }
    if (!TestNotNull(TEXT("blueprint.modify_scs path param is registered"), PathParam))
    {
        return false;
    }

    TestTrue(TEXT("path param remains required"), PathParam->bRequired);
    TestTrue(TEXT("assetPath is a scalar alias"), PathParam->Aliases.Contains(TEXT("assetPath")));
    TestTrue(TEXT("blueprintPath is a scalar alias"), PathParam->Aliases.Contains(TEXT("blueprintPath")));
    TestTrue(TEXT("name is a scalar alias"), PathParam->Aliases.Contains(TEXT("name")));

    const bool bHasBlueprintCandidatesAlias = PathParam->TypedAliases.ContainsByPredicate(
        [](const FParamAliasSpec& Alias)
        {
            return Alias.Name == TEXT("blueprintCandidates") && Alias.Type == TEXT("array");
        });
    const bool bHasCandidatesAlias = PathParam->TypedAliases.ContainsByPredicate(
        [](const FParamAliasSpec& Alias)
        {
            return Alias.Name == TEXT("candidates") && Alias.Type == TEXT("array");
        });

    TestTrue(TEXT("blueprintCandidates is a typed array alias"), bHasBlueprintCandidatesAlias);
    TestTrue(TEXT("candidates is a typed array alias"), bHasCandidatesAlias);

    return true;
}

// ============================================================================
// Test: widget.remove_widget declares widget asset path aliases
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherWidgetRemoveWidgetPathAliasMetadataTest,
    "PinWright.infra.dispatcher.AutoValidate.WidgetRemoveWidgetPathAliasMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherWidgetRemoveWidgetPathAliasMetadataTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    const FHandlerRegistration* Registration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("widget.remove_widget"));
    if (!TestNotNull(TEXT("widget.remove_widget is auto-registered"), Registration))
    {
        return false;
    }

    const FParamSpec* PathParam = nullptr;
    for (const FParamSpec& Param : Registration->Params)
    {
        if (Param.Name == TEXT("widgetPath"))
        {
            PathParam = &Param;
            break;
        }
    }
    if (!TestNotNull(TEXT("widget.remove_widget widgetPath param is registered"), PathParam))
    {
        return false;
    }

    TestTrue(TEXT("widgetPath param remains required"), PathParam->bRequired);
    TestTrue(TEXT("assetPath is a scalar alias"), PathParam->Aliases.Contains(TEXT("assetPath")));
    TestTrue(TEXT("path is a scalar alias"), PathParam->Aliases.Contains(TEXT("path")));
    TestTrue(TEXT("blueprintPath is a scalar alias"), PathParam->Aliases.Contains(TEXT("blueprintPath")));
    TestTrue(TEXT("blueprint_path is a scalar alias"), PathParam->Aliases.Contains(TEXT("blueprint_path")));
    TestTrue(TEXT("requestedPath is a scalar alias"), PathParam->Aliases.Contains(TEXT("requestedPath")));

    return true;
}

// ============================================================================
// Test: Auto-validation passes for handler with only optional params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoValidateOptionalOnlyTest,
    "PinWright.infra.dispatcher.AutoValidate.OptionalOnlyPassesEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoValidateOptionalOnlyTest::RunTest(const FString& Parameters)
{
    // _test.optional_only has only optional params — an empty payload should
    // pass validation and the handler body should run.
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    GOptionalOnlyHandlerCallCount = 0;

    TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
    bool bResult = Dispatcher.DispatchMethod(
        TEXT("_test.optional_only"), TEXT("req-opt"), EmptyParams);

    TestTrue(TEXT("Handler returns true"), bResult);
    TestEqual(TEXT("Handler body was called"), GOptionalOnlyHandlerCallCount, 1);

    return true;
}

// ============================================================================
// Test: Auto-validation rejects null payload when required params exist
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherAutoValidateNullPayloadTest,
    "PinWright.infra.dispatcher.AutoValidate.RejectsNullPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherAutoValidateNullPayloadTest::RunTest(const FString& Parameters)
{
    // Suppress ambient MapCheck/level errors that can fire during editor testing.
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // _test.require_name requires "name" — a null payload should block the
    // handler body from running.
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    GAutoValidateHandlerCallCount = 0;

    TSharedPtr<FJsonObject> NullPayload;
    bool bResult = Dispatcher.DispatchMethod(
        TEXT("_test.require_name"), TEXT("req-null"), NullPayload);

    TestTrue(TEXT("Lambda returns true (error was sent)"), bResult);
    TestEqual(TEXT("Handler body was NOT called"), GAutoValidateHandlerCallCount, 0);

    return true;
}

// ============================================================================
// Test: DrainAutoRegistrations populates AutoRegistered map
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherDrainPopulatesMapTest,
    "PinWright.infra.dispatcher.DrainAutoRegistrations.PopulatesMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherDrainPopulatesMapTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    const TMap<FString, FHandlerRegistration>& AutoRegs = Dispatcher.GetAutoRegisteredHandlers();
    TestTrue(TEXT("AutoRegistered contains _test.alpha"), AutoRegs.Contains(TEXT("_test.alpha")));
    TestTrue(TEXT("AutoRegistered contains _test.beta"), AutoRegs.Contains(TEXT("_test.beta")));

    return true;
}

// ============================================================================
// Test: After drain, DispatchMethod for auto-registered handler returns true
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherDrainCallableTest,
    "PinWright.infra.dispatcher.DrainAutoRegistrations.Callable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherDrainCallableTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainAutoRegistrations(nullptr);

    bool bResult = Dispatcher.DispatchMethod(TEXT("_test.beta"), TEXT("req-drain"), MakeShared<FJsonObject>());
    TestTrue(TEXT("Drained handler is callable"), bResult);

    return true;
}

// ============================================================================
// Test: ProcessRequest emits Verbose entry log before invoking handler
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherEntryLogTest,
    "PinWright.infra.dispatcher.EntryLogObservable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherEntryLogTest::RunTest(const FString& Parameters)
{
    FMcpOutputCapture Capture;
    GLog->AddOutputDevice(&Capture);

    LOG_SCOPE_VERBOSITY_OVERRIDE(LogRpcDispatcher, ELogVerbosity::Verbose);

    FRpcDispatcher Dispatcher;
    Dispatcher.RegisterHandler(TEXT("test.entry_log"),
        [](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            return true;
        });

    Dispatcher.ProcessRequest(TEXT("req-entry"), TEXT("test.entry_log"), MakeShared<FJsonObject>());

    // GLog forwards via a buffered redirector; drain before detaching so the
    // verbose lines reach Capture before RemoveOutputDevice severs the chain.
    GLog->Flush();
    GLog->RemoveOutputDevice(&Capture);

    bool bFoundMethod = false;
    bool bFoundId = false;
    for (const FString& Line : Capture.Lines)
    {
        if (Line.Contains(TEXT("Dispatching 'test.entry_log'")))
        {
            bFoundMethod = true;
        }
        if (Line.Contains(TEXT("id=req-entry")))
        {
            bFoundId = true;
        }
    }

    TestTrue(TEXT("Log contains Dispatching 'test.entry_log'"), bFoundMethod);
    TestTrue(TEXT("Log contains id=req-entry"), bFoundId);

    return true;
}
