// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests: verify that every auto-registered handler is correctly
// discoverable and properly documented
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

// ============================================================================
// Test: Every handler has a non-empty category and summary
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractAllHandlersDocumentedTest,
    "PinWright.infra.contract.AllHandlersDocumented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractAllHandlersDocumentedTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    for (const FHandlerRegistration& Reg : Registrations)
    {
        // Skip test-only registrations (prefixed with _test.)
        if (Reg.MethodName.StartsWith(TEXT("_test.")))
        {
            continue;
        }

        // Handler has non-empty category
        TestFalse(FString::Printf(TEXT("%s has category"), *Reg.MethodName),
            Reg.Category.IsEmpty());

        // Handler has non-empty summary
        TestFalse(FString::Printf(TEXT("%s has summary"), *Reg.MethodName),
            Reg.Summary.IsEmpty());

        // Handler has a non-null function pointer
        TestTrue(FString::Printf(TEXT("%s has handler function"), *Reg.MethodName),
            Reg.Func != nullptr);
    }

    return true;
}

// ============================================================================
// Test: Every required param has a non-empty name, type, and description
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractParamsDocumentedTest,
    "PinWright.infra.contract.RequiredParamsDocumented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractParamsDocumentedTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    for (const FHandlerRegistration& Reg : Registrations)
    {
        if (Reg.MethodName.StartsWith(TEXT("_test.")))
        {
            continue;
        }

        for (const FParamSpec& Param : Reg.Params)
        {
            // Every param has a name
            TestFalse(FString::Printf(TEXT("%s.%s has name"),
                *Reg.MethodName, *Param.Name), Param.Name.IsEmpty());

            // Every param has a type
            TestFalse(FString::Printf(TEXT("%s.%s has type"),
                *Reg.MethodName, *Param.Name), Param.Type.IsEmpty());

            // Every param has a description
            TestFalse(FString::Printf(TEXT("%s.%s has description"),
                *Reg.MethodName, *Param.Name), Param.Description.IsEmpty());
        }
    }

    return true;
}

// ============================================================================
// Test: No duplicate method names across registrations
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractNoDuplicateMethodsTest,
    "PinWright.infra.contract.NoDuplicateMethodNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractNoDuplicateMethodsTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    TSet<FString> SeenMethods;
    for (const FHandlerRegistration& Reg : Registrations)
    {
        bool bAlreadyInSet = false;
        SeenMethods.Add(Reg.MethodName, &bAlreadyInSet);
        TestFalse(FString::Printf(TEXT("%s is not a duplicate"), *Reg.MethodName),
            bAlreadyInSet);
    }

    return true;
}

// ============================================================================
// Test: Registration count is non-trivial (sanity check that auto-registration
// actually populated the array with domain handlers, not just test stubs)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRegistrationCountTest,
    "PinWright.infra.contract.RegistrationCountSanity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRegistrationCountTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    // This bound is the non-vacuity guard for the five sibling loops in this file
    // (AllHandlersDocumented, RequiredParamsDocumented, NoDuplicateMethodNames,
    // MethodNameFormat, ParamTypes.ValidTypeNames): each iterates GetPendingRegistrations()
    // and asserts nothing when the array is short, so they report green over an empty or
    // gutted registry. A floor of 10 could not discriminate — losing 99% of the handlers
    // still cleared it.
    //
    // Source/PinWright alone (always compiled, no integration gate) carries 1083
    // `^REGISTER_RPC_HANDLER(` registrations; the optional sub-modules
    // (PinWrightGeometry/PCG/Chooser/CommonUI/PoseSearch) add ~119 more only when their
    // engine plugins are enabled, so they are deliberately NOT counted in this floor.
    // 800 leaves ~26% headroom for verb removals while still failing loudly if
    // auto-registration breaks or a whole handler tree stops linking in.
    const int32 MinExpected = 800;
    TestTrue(FString::Printf(TEXT("At least %d registrations (found %d)"),
        MinExpected, Registrations.Num()),
        Registrations.Num() >= MinExpected);

    return true;
}

// ============================================================================
// Test: Method names follow the dot-separated namespace.verb convention
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractMethodNameFormatTest,
    "PinWright.infra.contract.MethodNameFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractMethodNameFormatTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    for (const FHandlerRegistration& Reg : Registrations)
    {
        // Method names should contain at least one dot (namespace.verb)
        TestTrue(FString::Printf(TEXT("%s contains a dot separator"), *Reg.MethodName),
            Reg.MethodName.Contains(TEXT(".")));

        // Method name should not contain spaces
        TestFalse(FString::Printf(TEXT("%s has no spaces"), *Reg.MethodName),
            Reg.MethodName.Contains(TEXT(" ")));

        // Category naming is intentionally flexible (flat or nested taxonomies).
        // Keep this test focused on method-name shape only.
    }

    return true;
}

// ============================================================================
// Test: Every param type is one of the valid type names
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractParamTypesValidTest,
    "PinWright.infra.contract.ParamTypes.ValidTypeNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractParamTypesValidTest::RunTest(const FString& Parameters)
{
    const TArray<FHandlerRegistration>& Registrations =
        FAutoRegisterHandler::GetPendingRegistrations();

    // Must stay in lockstep with PinWrightIsKnownTypeAtom (Handlers/ParamTypeCheck.h): that gate
    // FAILS OPEN on a token it does not know, so this list is the only thing that catches a
    // mis-declared type at all. A name here that the gate does not know is worse than a typo - it
    // reads as enforced and enforces nothing.
    const TSet<FString> ValidAtomicTypes = {
        TEXT("string"),
        TEXT("number"),
        TEXT("integer"),
        TEXT("boolean"),
        TEXT("bool"),
        TEXT("object"),
        TEXT("array"),
        TEXT("any"),
        // The three path-shaped spellings. They accept exactly the JSON shapes `string` accepts;
        // `path` and `classref` additionally refuse a value containing '//' (which reaches
        // CreatePackage and ends the process), and `filepath` deliberately does not, because a UNC
        // path normalises to '//server/share'.
        TEXT("path"),
        TEXT("classref"),
        TEXT("filepath")
    };

    auto IsSupportedTypeExpr = [&ValidAtomicTypes](const FString& TypeExpr) -> bool
    {
        TArray<FString> Parts;
        TypeExpr.ParseIntoArray(Parts, TEXT("|"), true);

        if (Parts.Num() == 0)
        {
            return false;
        }

        for (FString Part : Parts)
        {
            Part = Part.TrimStartAndEnd().ToLower();
            if (!ValidAtomicTypes.Contains(Part))
            {
                return false;
            }
        }

        return true;
    };

    for (const FHandlerRegistration& Reg : Registrations)
    {
        for (const FParamSpec& Param : Reg.Params)
        {
            TestTrue(FString::Printf(TEXT("%s.%s type '%s' is valid"),
                *Reg.MethodName, *Param.Name, *Param.Type),
                IsSupportedTypeExpr(Param.Type));

            // FParamAliasSpec::Type is READ AT RUNTIME and was not covered here.
            // CollectDeclaredTypesByWireName (Dispatch/RpcDispatcher.cpp) resolves a typed alias
            // against its OWN declared type - that is the entire reason the field exists - so a
            // typo there silently fails the shape gate open for that wire name while the canonical
            // stays enforced, which is the hardest version of this defect to notice. An EMPTY
            // alias type is legal and means "inherit the spec's", which is what the dispatcher
            // does with it.
            for (const FParamAliasSpec& Alias : Param.TypedAliases)
            {
                TestTrue(FString::Printf(TEXT("%s.%s alias '%s' type '%s' is valid or empty"),
                    *Reg.MethodName, *Param.Name, *Alias.Name, *Alias.Type),
                    Alias.Type.IsEmpty() || IsSupportedTypeExpr(Alias.Type));
            }
        }
    }

    return true;
}

// ============================================================================
// Required-param gate: one registry walk + the mechanism tests under it
// ============================================================================
//
// WHAT THIS REPLACES. 532 hand-written per-verb gate tests across 26 test files,
// each a copy of the same ~25-line body. What actually varied between them was two
// strings - the method name and the expected param name - and both are already in
// the registry, so 532 copies tested one shared mechanism 532 times and tested the
// DECLARATION not at all. They asserted 518 distinct (verb, slot) pairs between
// them; this walk asserts all 518 plus 1146 more, and covers verbs that do not
// exist yet - which is the failure mode the copies had, since a copy must be
// remembered for each new verb. 25 of them (the asset.* family) were still in the
// original shape that could not fail: `TestTrue(InvokeHandler("ns.verb", {}))`
// calls Reg.Func(Ctx) straight out of FAutoRegisterHandler::GetPendingRegistrations()
// and reaches neither call site of the gate, so it asserted only that the method
// name was in the registry.
//
// WHAT IT ASSERTS. For every registration and every required FParamSpec on it, a
// request that omits exactly that slot must be REJECTED by
// FRpcDispatcher::ValidateHandlerParams (RpcDispatcher.cpp:91) with
// MISSING_REQUIRED_PARAM, and the message must name that slot in quotes. Quoted,
// not bare: the message appends the accepted aliases UNQUOTED after
// ". Accepted aliases: ", so a bare Contains(Name) also passes when Name appears
// in a DIFFERENT param's alias list, pinning the wrong slot.
//
// ALL THREE DECLARATION SHAPES ARE COVERED BY CONSTRUCTION, because they all
// produce the same FParamSpec::bRequired the walk reads:
//   - RPC_PARAM_REQ                                  (ParamSpec.h:33)
//   - ParamAliasUtils::MakeAliasParamSpec(..., true) (ParamAliasUtils.h:38)
//   - the required-spec factories built on it - AssetPathParamUtils::AssetPathParamReq,
//     MaterialCreatePathParamUtils::MaterialCreateNameParamReq, ActorNameParamUtils::
//     ActorNameParamReq, BlueprintPathParamReq, WidgetAssetPathParamReq,
//     MaterialHandlerUtils::MaterialAssetPathParamReq / MaterialExpressionClassParamReq
// A new shape needs no new test as long as it sets bRequired; the Mechanism tests
// below pin one live example of each shape end to end so a shape that stops
// setting bRequired fails loudly rather than silently dropping out of the walk.
//
// WHY IT IS SAFE TO POINT AT DESTRUCTIVE VERBS. ValidateHandlerParams runs inside
// the auto-registration bridge lambda BEFORE RegCopy.Func(Ctx)
// (RpcDispatcher.cpp:324-338), so a payload that is missing a required slot never
// reaches a handler body: nothing is deleted, saved, or torn down. The walk
// refuses to dispatch at all when it cannot construct a payload that leaves at
// least one required slot unsatisfied (see ShadowedParams below).
//
// TICK-UNSAFE VERBS ARE NOT EXCLUDED. The safe-point gate is
// `IsTickUnsafeMethod(Method) && !IsSafeNow()` (RpcDispatcher.cpp), and IsSafeNow()
// is now three terms: !ForcedUnsafeForTests() && !IsAnyWorldTicking() &&
// !IsInsideNamedThreadPump(). An automation body satisfies all three - it is not
// inside UWorld::bInTick, and it is not draining a task-graph named-thread queue
// either, because IAutomationWorkerModule::Tick is a direct FEngineLoop::Tick call
// and the worker drains its own .WithInbox() endpoint from that Tick, so a test body
// never runs out of a message-delivery task. They answer inline;
// RequiredParamGate.Mechanism.TickUnsafeVerbAnswersInline pins that empirically on
// animation.cleanup rather than assuming it.
//
// FAILURE REPORTING. One test covering ~940 verbs is only usable if a failure
// names the offending verb and slot exactly, so the loop accumulates and reports
// every failure instead of returning at the first: one red naming twelve verbs
// beats twelve runs each finding one.
// ============================================================================

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightRequiredParamGate, Log, All);

namespace RequiredParamGate
{
    // Mirrors the dispatcher's CollectParamNames (RpcDispatcher.cpp:285): every wire
    // name one spec accepts, canonical first. Used only to decide which slot the gate
    // will name; the assertion itself always reads the dispatcher's real message.
    inline TArray<FString> AcceptedNames(const FParamSpec& Spec)
    {
        TArray<FString> Names;
        Names.Add(Spec.Name);
        Names.Append(Spec.Aliases);
        for (const FParamAliasSpec& Alias : Spec.TypedAliases)
        {
            Names.Add(Alias.Name);
        }
        return Names;
    }

    inline bool PayloadSatisfies(const TSharedPtr<FJsonObject>& Payload, const FParamSpec& Spec)
    {
        for (const FString& Name : AcceptedNames(Spec))
        {
            if (Payload->HasField(Name))
            {
                return true;
            }
        }
        return false;
    }

    // Placeholder for a slot the gate only has to SEE. PayloadHasParamOrAlias
    // (RpcDispatcher.cpp:92) tests HasField and never the type, so the value is
    // irrelevant to the gate; it is typed anyway so a payload quoted in a failure
    // message reads like a real request.
    inline void SetPlaceholder(const TSharedPtr<FJsonObject>& Payload, const FParamSpec& Spec)
    {
        const FString Type = Spec.Type.ToLower();
        if (Type.StartsWith(TEXT("number")) || Type.StartsWith(TEXT("integer")))
        {
            Payload->SetNumberField(Spec.Name, 1.0);
        }
        else if (Type.StartsWith(TEXT("bool")))
        {
            Payload->SetBoolField(Spec.Name, true);
        }
        else if (Type.StartsWith(TEXT("array")))
        {
            Payload->SetArrayField(Spec.Name, TArray<TSharedPtr<FJsonValue>>());
        }
        else if (Type.StartsWith(TEXT("object")))
        {
            Payload->SetObjectField(Spec.Name, MakeShared<FJsonObject>());
        }
        else
        {
            Payload->SetStringField(Spec.Name, TEXT("__pinwright_required_param_probe__"));
        }
    }

    // Outcome of one probe: dispatch Payload at Method and settle any deferral, then
    // hand back what the sink saw. Shared by the walk and the Mechanism tests so both
    // exercise the identical production path (ProcessRequest, not Reg.Func).
    struct FProbe
    {
        bool bResponded = false;
        bool bSuccess = false;
        bool bAnsweredInline = false;   // false = the request was parked on PendingQueue
        FString ErrorCode;
        FString Message;
    };

    inline FProbe Probe(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
                        const FString& Method, const FString& RequestId,
                        const TSharedPtr<FJsonObject>& Payload)
    {
        FProbe Out;
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Payload,
                                        bSuccess, ErrorCode);

        // ProcessRequest can park a request on PendingQueue rather than answer it on
        // this stack - the safe-point gate does it for any GTickUnsafeMethodNames entry
        // while a world is inside UWorld::Tick (RpcDispatcher.cpp:448), and the
        // Saving/GC gate does it independently (:415). A bare FRpcDispatcher has no
        // UPinWrightSubsystem ticker to drain that queue, so without this the sink would
        // stay silent and every assertion would read a default-constructed capture.
        // Draining re-enters ProcessRequest, so a stack that really is unsafe re-defers
        // and bResponded stays false - the caller then fails honestly instead of being
        // weakened into a pass.
        Out.bAnsweredInline = Sink->bWasCalled;
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

    // The one assertion the whole family is about: Probe must have been rejected with
    // MISSING_REQUIRED_PARAM naming ParamName in quotes. Returns a human-readable
    // reason on failure and an empty string on success, so the walk can accumulate
    // reasons and the Mechanism tests can assert on one directly.
    inline FString WhyNotRejected(const FProbe& P, const FString& ParamName)
    {
        if (!P.bResponded)
        {
            return TEXT("no response - the request was deferred or dropped");
        }
        if (P.bSuccess)
        {
            return TEXT("ACCEPTED the payload - the gate did not fire and the handler body ran");
        }
        if (!P.ErrorCode.Equals(ErrorCodes::ERR_MISSING_REQUIRED_PARAM))
        {
            return FString::Printf(TEXT("rejected with '%s', expected MISSING_REQUIRED_PARAM (message: %s)"),
                                   *P.ErrorCode, *P.Message);
        }
        const FString Quoted = FString::Printf(TEXT("'%s'"), *ParamName);
        if (!P.Message.Contains(Quoted))
        {
            return FString::Printf(TEXT("MISSING_REQUIRED_PARAM does not name %s (message: %s)"),
                                   *Quoted, *P.Message);
        }
        return FString();
    }

    // Assert one probe from a Mechanism test.
    inline void TestRejectsMissing(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
                                   DispatcherTestHelpers::FSinkPtr& Sink, const FString& Method,
                                   const FString& ParamName,
                                   const TSharedPtr<FJsonObject>& Payload = MakeShared<FJsonObject>())
    {
        const FProbe P = Probe(Dispatcher, Sink, Method,
                               FString::Printf(TEXT("mech-%s-%s"), *Method, *ParamName), Payload);
        const FString Why = WhyNotRejected(P, ParamName);
        Test.TestTrue(*FString::Printf(TEXT("%s rejects a payload missing '%s': %s"),
                                       *Method, *ParamName, Why.IsEmpty() ? TEXT("ok") : *Why),
                      Why.IsEmpty());
    }
}

// `.EveryVerb` and not a bare `...RequiredParamGate`: a test id that is a strict
// PREFIX of another test's id is silently dropped from the automation queue -
// the controller turns the shared prefix into a branch node and the test that
// wanted to be that node never registers. Measured: filtering on
// `PinWright.infra.contract` enumerated the six `RequiredParamGate.Mechanism.*`
// leaves and ran 12 tests, with the bare-prefix walk missing and no error
// anywhere in the log. Every id here must be a leaf.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateTest,
    "PinWright.infra.contract.RequiredParamGate.EveryVerb",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateTest::RunTest(const FString& Parameters)
{
    // One dispatcher for the whole walk. MakeDispatcher drains with a null subsystem,
    // which wires the response capture (RpcDispatcher.cpp:319-321) so Ctx.SendError's
    // MISSING_REQUIRED_PARAM reaches the sink instead of a transport.
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const TMap<FString, FHandlerRegistration>& Registry = Dispatcher.GetAutoRegisteredHandlers();

    // Sorted so a failure list, the Verbose coverage dump, and two runs on the same
    // tree all read in the same order. The dispatcher's map is what production
    // dispatch resolves against, so duplicate registrations collapse here exactly as
    // they do at runtime; NoDuplicateMethodNames above is what fails on the duplicate.
    TArray<FString> Methods;
    Registry.GenerateKeyArray(Methods);
    Methods.Sort();

    TArray<FString> Failures;
    int32 VerbsExercised = 0;
    int32 ParamsExercised = 0;
    int32 ShadowedParams = 0;

    for (const FString& Method : Methods)
    {
        // Test-only registrations exist to exercise the dispatcher itself and are
        // asserted directly by Infra.Dispatcher.*; skipping them matches the five
        // sibling loops above.
        if (Method.StartsWith(TEXT("_test.")))
        {
            continue;
        }

        const FHandlerRegistration& Reg = Registry[Method];
        bool bVerbCounted = false;

        for (int32 Index = 0; Index < Reg.Params.Num(); ++Index)
        {
            if (!Reg.Params[Index].bRequired)
            {
                continue;
            }

            // Satisfy every required slot declared BEFORE this one so the gate walks
            // past them and stops here. ValidateHandlerParams reports only the FIRST
            // missing required spec and returns, which is why declaration order
            // matters and why the payload is built this way rather than left empty:
            // an empty payload would only ever probe each verb's first required slot,
            // and the per-verb tests this walk replaces covered later slots too
            // (blueprint.add_function.MissingRequiredFunctionName and 50 siblings).
            // Optional slots are never inspected by the gate, so they stay out.
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            for (int32 Prior = 0; Prior < Index; ++Prior)
            {
                if (Reg.Params[Prior].bRequired)
                {
                    RequiredParamGate::SetPlaceholder(Payload, Reg.Params[Prior]);
                }
            }

            // Which spec the gate will actually name: the first required one this
            // payload does not satisfy, in declaration order. That is normally this
            // slot. It is an earlier slot only when this slot's canonical name is also
            // an accepted alias of a prior required slot, so the prior placeholder
            // satisfied both - then this slot cannot be probed on its own, and if
            // NOTHING is left unsatisfied the gate would PASS and the handler body
            // would run for real. Never dispatch that.
            const FParamSpec* Expected = nullptr;
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.bRequired && !RequiredParamGate::PayloadSatisfies(Payload, Spec))
                {
                    Expected = &Spec;
                    break;
                }
            }

            if (Expected != &Reg.Params[Index])
            {
                ++ShadowedParams;
                AddInfo(FString::Printf(
                    TEXT("%s: required param '%s' cannot be probed on its own - one of its "
                         "accepted wire names is already supplied by an earlier required slot, "
                         "so the gate stops on %s instead."),
                    *Method, *Reg.Params[Index].Name,
                    Expected ? *FString::Printf(TEXT("'%s'"), *Expected->Name)
                             : TEXT("nothing (every required slot is satisfied)")));
                continue;
            }

            const RequiredParamGate::FProbe P = RequiredParamGate::Probe(
                Dispatcher, Sink, Method,
                FString::Printf(TEXT("req-gate-%s-%d"), *Method, Index), Payload);

            ++ParamsExercised;
            if (!bVerbCounted)
            {
                bVerbCounted = true;
                ++VerbsExercised;
            }

            // The exercised list, one line per (verb, slot). Verbose so a normal run
            // is quiet; dump it with -LogCmds='LogPinWrightRequiredParamGate Verbose'
            // and grep PWGATE-COVER to diff this walk's coverage against any other set.
            UE_LOG(LogPinWrightRequiredParamGate, Verbose, TEXT("PWGATE-COVER %s|%s"),
                   *Method, *Expected->Name);

            const FString Why = RequiredParamGate::WhyNotRejected(P, Expected->Name);
            if (!Why.IsEmpty())
            {
                Failures.Add(FString::Printf(TEXT("%s (required param #%d '%s'): %s"),
                                             *Method, Index, *Expected->Name, *Why));
            }
        }
    }

    // Every failure, named. Accumulated rather than returned at the first one so a
    // single red run names every offending verb.
    for (const FString& Failure : Failures)
    {
        AddError(Failure);
    }
    TestEqual(TEXT("Required-param gate failures"), Failures.Num(), 0);

    // Non-vacuity floors, the same role RegistrationCountSanity plays for the five
    // metadata loops above: this loop asserts nothing over an empty registry, and a
    // registry walk structurally cannot notice a required declaration that was
    // DELETED - the slot just drops out of the walk. These floors are what catches
    // that class of regression in bulk.
    //
    // Measured 2026-08-20 by this test on a host where every integration loaded
    // (`PinWright integrations: loaded=[geometry,model,pcg,chooser,pose_search,ui]
    // skipped=[]`): 1664 required params across 958 verbs, 0 failures. A static parse
    // of every REGISTER_RPC_HANDLER params list attributes 1497 of those params and
    // 840 of those verbs to the ALWAYS-compiled main module; the five gated
    // sub-modules are simply absent on a host whose engine plugin is disabled, so the
    // floors sit under the main-module-only figures rather than the measured totals.
    // 700 / 1200 leave ~17% headroom for verb removals while still failing loudly if
    // auto-registration breaks or a whole handler tree stops linking in.
    TestTrue(FString::Printf(TEXT("At least 700 verbs declare a required param (found %d)"),
                             VerbsExercised),
             VerbsExercised >= 700);
    TestTrue(FString::Printf(TEXT("At least 1200 required params exercised (found %d)"),
                             ParamsExercised),
             ParamsExercised >= 1200);

    // PINWRIGHT_INFO_IS_NOT_A_SKIP: every verb in the walk above was probed and its
    // result asserted; this publishes the coverage totals the floors were judged against.
    AddInfo(FString::Printf(
        TEXT("Required-param gate: %d required params across %d verbs exercised, %d not "
             "independently probeable, %d failures. Dump the covered list with "
             "-LogCmds='LogPinWrightRequiredParamGate Verbose' and grep PWGATE-COVER."),
        ParamsExercised, VerbsExercised, ShadowedParams, Failures.Num()));

    return true;
}

// ============================================================================
// Mechanism tests. The walk above proves every verb is wired to the gate; these
// prove the gate itself works, one per declaration shape plus the two
// interactions worth pinning (aliases, the deferral path). They name concrete
// verbs on purpose - that is what makes them fail when a shape stops producing
// bRequired, which the walk cannot see.
//
// The sixth mechanism fact - that the gate blocks the handler BODY, not just the
// response - is pinned by Infra.Dispatcher.AutoValidate.RejectsMissingRequiredParam
// (Tests/Infra/TestDispatcher.cpp), which counts invocations of the `_test.require_name`
// body. Not duplicated here.
// ============================================================================

// Shape 1: the RPC_PARAM_REQ macro (ParamSpec.h:33).
// editor.console_command declares exactly one required slot, `command`
// (Handlers/Editor/EditorCommandHandler.cpp:287).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateMacroShapeTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.RpcParamReqMacro",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateMacroShapeTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("editor.console_command"), TEXT("command"));
    return true;
}

// Shape 2: ParamAliasUtils::MakeAliasParamSpec(..., bRequired=true, ...) called
// directly (ParamAliasUtils.h:38). skeleton.create_physics_asset declares its only
// required slot that way - `skeletalMeshPath` with `skeletonPath` as a
// schema-registered alias (Handlers/Animation/PhysicsAssetHandler.cpp:227) - so a
// green result here means bRequired survives that constructor, and the alias line
// means the alias set survives with it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateAliasSpecShapeTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.MakeAliasParamSpec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateAliasSpecShapeTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("skeleton.create_physics_asset"), TEXT("skeletalMeshPath"));

    // The error enumerates the accepted aliases so a wrong-key caller self-corrects
    // without a wiki round-trip (ValidateHandlerParams, RpcDispatcher.cpp:104-111).
    TestTrue(TEXT("the rejection lists the skeletonPath alias"),
        Sink->Message.Contains(TEXT("Accepted aliases:")) &&
        Sink->Message.Contains(TEXT("skeletonPath")));
    return true;
}

// Shape 3: the required-spec factories layered on MakeAliasParamSpec. Two live
// examples, because they derive the canonical name differently: AssetPathParamReq
// takes it as an argument (AssetPathParamUtils.h:37) while MaterialCreateNameParamReq
// hardcodes `name` and rides `assetName`/`assetPath` as aliases
// (MaterialCreatePathParamUtils.h:80).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateFactoryShapeTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.RequiredSpecFactories",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateFactoryShapeTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("asset.exists"), TEXT("assetPath"));
    // The full list, not Contains("path") - "assetPath" contains "path", so the
    // short form passes with the alias deleted.
    TestTrue(*FString::Printf(TEXT("asset.exists lists its path alias (message: %s)"),
                              *Sink->Message),
        Sink->Message.Contains(TEXT("Accepted aliases: assetPath, path")));

    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("material.authoring.create_material"), TEXT("name"));
    TestTrue(TEXT("create_material lists its assetName/assetPath aliases"),
        Sink->Message.Contains(TEXT("assetName")) &&
        Sink->Message.Contains(TEXT("assetPath")));
    return true;
}

// Interaction 1: an ALIAS satisfies the requirement. asset.exists' slot is
// canonically `assetPath` and accepts `path`; a caller who used `path` must not be
// refused. Without this, an alias set could be advertised in the error message and
// still be rejected on the wire, and every rejection test in the family would stay
// green. asset.exists is read-only, so letting the body run is safe.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateAliasSatisfiesTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.AliasSatisfiesRequirement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateAliasSatisfiesTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/__PinWright_NoSuchAsset__"));

    const RequiredParamGate::FProbe P = RequiredParamGate::Probe(
        Dispatcher, Sink, TEXT("asset.exists"), TEXT("mech-alias-satisfies"), Payload);

    TestTrue(TEXT("asset.exists responded"), P.bResponded);
    TestNotEqual(TEXT("the path alias satisfies the required assetPath slot"),
        P.ErrorCode, FString(ErrorCodes::ERR_MISSING_REQUIRED_PARAM));
    TestFalse(TEXT("and the alias is not rejected as an unknown parameter either"),
        P.ErrorCode.Equals(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// Interaction 2: the gate reports the FIRST missing required slot in declaration
// order and stops there. ai.add_blackboard_key declares three
// (blackboardPath, keyName, keyType - Handlers/AI/AIHandler.cpp:414), so the same
// verb answers with a different name depending on what the caller supplied. This is
// the property the walk's payload construction depends on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateDeclarationOrderTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.FirstMissingInDeclarationOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateDeclarationOrderTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("ai.add_blackboard_key"), TEXT("blackboardPath"));

    TSharedPtr<FJsonObject> WithPath = MakeShared<FJsonObject>();
    WithPath->SetStringField(TEXT("blackboardPath"), TEXT("/Game/__PinWright_NoSuchBB__"));
    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("ai.add_blackboard_key"), TEXT("keyName"), WithPath);

    TSharedPtr<FJsonObject> WithPathAndName = MakeShared<FJsonObject>();
    WithPathAndName->SetStringField(TEXT("blackboardPath"), TEXT("/Game/__PinWright_NoSuchBB__"));
    WithPathAndName->SetStringField(TEXT("keyName"), TEXT("Probe"));
    RequiredParamGate::TestRejectsMissing(*this, Dispatcher, Sink,
        TEXT("ai.add_blackboard_key"), TEXT("keyType"), WithPathAndName);
    return true;
}

// Interaction 3: the deferral path. animation.cleanup is on
// Dispatch/SafePoint.cpp's GTickUnsafeMethodNames list (SafePoint.cpp:71) because
// its body closes asset editors, forces GC and deletes assets in a loop
// (AnimationHandler.cpp:266-279). The safe-point gate is
// `IsTickUnsafeMethod(Method) && !IsSafeNow()` (RpcDispatcher.cpp:448), and
// automation runs from FTSTicker::GetCoreTicker, outside UWorld::Tick - so
// IsSafeNow() is true and the request is answered on this stack, not parked on
// PendingQueue. This test asserts that INLINE answer explicitly (bAnsweredInline),
// which is why tick-unsafe verbs are not excluded from the walk. Dispatching it is
// safe: the gate runs before the body, so no editor is closed and no asset deleted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContractRequiredParamGateTickUnsafeTest,
    "PinWright.infra.contract.RequiredParamGate.Mechanism.TickUnsafeVerbAnswersInline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContractRequiredParamGateTickUnsafeTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const RequiredParamGate::FProbe P = RequiredParamGate::Probe(
        Dispatcher, Sink, TEXT("animation.cleanup"), TEXT("mech-tick-unsafe"),
        MakeShared<FJsonObject>());

    TestTrue(TEXT("a tick-unsafe verb is answered on the automation stack, not deferred"),
        P.bAnsweredInline);
    const FString Why = RequiredParamGate::WhyNotRejected(P, TEXT("artifacts"));
    TestTrue(*FString::Printf(TEXT("animation.cleanup rejects an empty payload: %s"),
                              Why.IsEmpty() ? TEXT("ok") : *Why),
        Why.IsEmpty());
    return true;
}
