// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Dispatch/RpcDispatcher.h"
#include "Catalog/SuggestionHelpers.h"
#include "Compat/JsonKeyCompat.h"
#include "Dispatch/SafePoint.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dispatch/WorldPrecondition.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/NestedParamKeyCheck.h"
#include "Handlers/ParamTypeCheck.h"
#include "Handlers/FormatterRegistration.h"
#include "IntegrationGates.h"
#include "PinWrightSubsystem.h"
#include "State/ClientActivity.h"
#include "Transport/EditorIdentity.h"
#include "Transport/ModalStateProbe.h"
#include "Utils/HttpResponseSpill.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Match the JS bridge contract: the optional top-level `_format` field on
    // params selects the response shape. Today only "text" is recognized.
    static const FString GFormatFieldName = TEXT("_format");
    static const FString GFormatTextValue = TEXT("text");

    bool PayloadHasParamOrAlias(const TSharedPtr<FJsonObject>& Params, const FParamSpec& Spec)
    {
        if (!Params.IsValid())
        {
            return false;
        }

        if (Params->HasField(Spec.Name))
        {
            return true;
        }

        for (const FString& Alias : Spec.Aliases)
        {
            if (Params->HasField(Alias))
            {
                return true;
            }
        }
        for (const FParamAliasSpec& Alias : Spec.TypedAliases)
        {
            if (Params->HasField(Alias.Name))
            {
                return true;
            }
        }

        return false;
    }

    // All accepted wire names for one spec, canonical-first: Name, then Aliases,
    // then each TypedAliases[].Name. Single source of truth for the three name
    // sources, shared by the known-params set and the missing-param error hint.
    TArray<FString> CollectParamNames(const FParamSpec& Spec)
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

    void AddKnownParamNames(TSet<FString>& KnownParams, const FParamSpec& Spec)
    {
        for (const FString& Name : CollectParamNames(Spec))
        {
            KnownParams.Add(Name);
        }
    }

    // The declared type for every wire name one registration accepts. Canonical names are
    // written last so a canonical always wins a collision with some other spec's alias.
    //
    // Resolution is per MATCHED WIRE NAME, not per spec, because FParamAliasSpec carries its
    // own Type (ParamSpec.h:11) precisely for the cases where a typed alias differs from its
    // canonical - `blueprintCandidates` is `array` where its canonical `path` is `string`.
    // Checking the spec's type for every name it accepts would refuse the alias's own shape.
    // An untyped alias (FParamSpec::Aliases) has no type of its own and inherits the spec's.
    TMap<FString, FString> CollectDeclaredTypesByWireName(const FHandlerRegistration& Reg)
    {
        TMap<FString, FString> TypeByName;
        for (const FParamSpec& Spec : Reg.Params)
        {
            for (const FString& Alias : Spec.Aliases)
            {
                if (!TypeByName.Contains(Alias))
                {
                    TypeByName.Add(Alias, Spec.Type);
                }
            }
            for (const FParamAliasSpec& Alias : Spec.TypedAliases)
            {
                if (!TypeByName.Contains(Alias.Name))
                {
                    TypeByName.Add(Alias.Name, Alias.Type.IsEmpty() ? Spec.Type : Alias.Type);
                }
            }
        }
        for (const FParamSpec& Spec : Reg.Params)
        {
            TypeByName.Add(Spec.Name, Spec.Type);
        }
        return TypeByName;
    }

    // Validate required + unknown + wrongly-typed params, and the nested keys of the parameters
    // that declare a nested schema, against a registration's ParamSpec.
    // Returns true on success. On failure, sends the appropriate error via
    // `Ctx.SendError` and returns false. Used by both the auto-registration
    // bridge lambda and the captured-handler text-format path so the contract
    // stays in one place.
    bool ValidateHandlerParams(const FHandlerRegistration& Reg, FHandlerContext& Ctx,
                               const TSharedPtr<FJsonObject>& Params, const FString& Method)
    {
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.bRequired)
            {
                if (!PayloadHasParamOrAlias(Params, Spec))
                {
                    // Enumerate the accepted aliases so a wrong-key caller self-corrects
                    // from the error without a wiki round-trip (e.g. an actor.* reader
                    // names actorName / objectPath / actorPath, not just the canonical).
                    const TArray<FString> AcceptedNames = CollectParamNames(Spec);

                    FString Message = FString::Printf(
                        TEXT("Missing required parameter '%s' (type: %s)"),
                        *Spec.Name, *Spec.Type);
                    if (AcceptedNames.Num() > 1)
                    {
                        Message += FString::Printf(TEXT(". Accepted aliases: %s"),
                            *FString::Join(AcceptedNames, TEXT(", ")));
                    }

                    Ctx.SendError(TEXT("MISSING_REQUIRED_PARAM"), Message);
                    return false;
                }
            }
        }

        // Run the unknown-param rejection regardless of declared param count. With an
        // empty spec (RPC_NO_PARAMS) KnownParams is naturally empty, so any supplied
        // field is unknown and rejected; an empty args:{} or absent args have no fields
        // to reject and still pass. Reserved/transport fields (wait, _format, the spill
        // skip flag) are stripped by StripInternalDispatchFields before validation, so
        // they never reach here.
        if (Params.IsValid())
        {
            TSet<FString> KnownParams;
            for (const FParamSpec& Spec : Reg.Params)
            {
                AddKnownParamNames(KnownParams, Spec);
            }

            TArray<FString> UnknownParams;
            for (const auto& Field : Params->Values)
            {
                if (!KnownParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
                {
                    UnknownParams.Add(EARGCompat::JsonKeyToString(Field.Key));
                }
            }

            if (UnknownParams.Num() > 0)
            {
                const FString UnknownList = FString::Join(UnknownParams, TEXT(", "));
                const FString KnownList = FString::Join(KnownParams.Array(), TEXT(", "));
                UE_LOG(LogRpcDispatcher, Warning,
                    TEXT("[%s] Unknown parameter(s): %s. Known parameters: %s"),
                    *Method, *UnknownList, *KnownList);

                // A zero-known-param method reads more naturally as "takes no parameters"
                // than "Valid parameters: []"; the declared-param case keeps its list.
                const FString Message = KnownParams.Num() == 0
                    ? FString::Printf(TEXT("Unknown parameter(s) for '%s': [%s]. This method takes no parameters. Call '%s' with no 'args' field to fetch its wiki page."),
                        *Method, *UnknownList, *Method)
                    : FString::Printf(TEXT("Unknown parameter(s) for '%s': [%s]. Valid parameters: [%s]. Call '%s' with no 'args' field to fetch its wiki page."),
                        *Method, *UnknownList, *KnownList, *Method);

                Ctx.SendError(TEXT("UNKNOWN_PARAMS"), Message);
                return false;
            }

            // Declared-type gate: the runtime reader of FParamSpec::Type. See
            // Handlers/ParamTypeCheck.h for what is refused and why the LOSSLESS coercions
            // ("limit": "100", "force": "true") are deliberately kept.
            //
            // It runs LAST for a reason. A caller who misspells a key already gets a list of
            // every valid parameter, and a caller who omits a required one gets that slot
            // named; both are the answer the caller must act on first, so neither is displaced
            // by a shape complaint about some other key. What this pass adds is the missing
            // third answer - the caller who spelled it right and shaped it wrong, who until
            // now got a success payload built on a coerced or defaulted value
            // (board B-param-type-never-validated).
            //
            // Every fault is collected rather than returning on the first, for the same reason
            // the unknown-name pass lists every unknown key: one refusal naming three wrong
            // shapes beats three round trips.
            const TMap<FString, FString> DeclaredTypeByName = CollectDeclaredTypesByWireName(Reg);

            TArray<FString> TypeFaults;
            for (const auto& Field : Params->Values)
            {
                const FString Key = EARGCompat::JsonKeyToString(Field.Key);
                const FString* DeclaredType = DeclaredTypeByName.Find(Key);
                if (!DeclaredType)
                {
                    // Unreachable: the unknown-name pass above refused every undeclared key.
                    continue;
                }

                const PinWrightParamTypes::EDeclaredTypeVerdict Verdict =
                    PinWrightParamTypes::PinWrightCheckDeclaredType(*DeclaredType, Field.Value);
                if (Verdict == PinWrightParamTypes::EDeclaredTypeVerdict::Accepted)
                {
                    continue;
                }

                TypeFaults.Add(
                    Verdict == PinWrightParamTypes::EDeclaredTypeVerdict::ReceivedNull
                        ? FString::Printf(
                            TEXT("'%s' is declared %s and was sent as null - omit the parameter instead of sending null"),
                            *Key, **DeclaredType)
                        : FString::Printf(
                            TEXT("'%s' is declared %s and was sent as %s"),
                            *Key, **DeclaredType,
                            *PinWrightParamTypes::PinWrightDescribeJsonValueType(Field.Value)));
            }

            if (TypeFaults.Num() > 0)
            {
                const FString FaultList = FString::Join(TypeFaults, TEXT("; "));
                UE_LOG(LogRpcDispatcher, Warning,
                    TEXT("[%s] Parameter type mismatch: %s"), *Method, *FaultList);

                Ctx.SendError(ErrorCodes::ERR_PARAM_TYPE_MISMATCH,
                    FString::Printf(
                        TEXT("Parameter type mismatch for '%s': %s. Call '%s' with no 'args' field to fetch its wiki page."),
                        *Method, *FaultList, *Method));
                return false;
            }

            // Path-separator gate: the safety half of the `path` / `classref` declared types. See
            // Handlers/ParamTypeCheck.h for why `//` is the only rule, why nothing wider
            // (IsValidLongPackageName) is correct on a class or object reference, and why
            // `filepath` deliberately carries no rule.
            //
            // It runs BETWEEN the shape pass and the nested-key pass, and both halves of that
            // placement are deliberate.
            //
            // AFTER the shape pass, because this rule is about the CONTENT of a value that is
            // already the right shape. Telling a caller who sent `assetPath` as an object that "it
            // contains //" is not an answer they can act on; the shape complaint is. That is the
            // same reasoning the nested pass below states for itself.
            //
            // BEFORE the nested-key pass, because this is the only refusal in the chain that
            // prevents PROCESS DEATH rather than a wrong answer: `//` reaches CreatePackage, which
            // logs Fatal. When one payload carries both a `//` path and a stray nested key, the
            // `//` is the fault the caller must fix before the request can run at all; a list of
            // accepted keys is advice about a request that would have taken the editor down.
            //
            // It is a separate pass rather than a fourth EDeclaredTypeVerdict because it is a
            // different error code with a different remedy ("remove the doubled slash"), and the
            // shape pass's message vocabulary - "is declared X and was sent as Y" - cannot carry it.
            //
            // Faults are collected across every slot before answering, as the three passes around
            // it do.
            //
            // IT SEES ONLY THE TOP LEVEL, exactly like the three passes around it. A path nested
            // inside an object/array parameter (foliageTypes[].meshPath, nodes[].texturePath,
            // stems[].assetPath, captures[].attribute) still reaches its LoadObject unvalidated.
            // Closing that needs machinery this gate does not have - a TYPE on a nested key, which
            // FParamSpec::NestedKeys (an untyped allow-list) cannot express, and a rule that reads
            // map VALUES as well as keys - so it is a separate ticket rather than an extension
            // squeezed in here.
            TArray<FString> PathFaults;
            for (const auto& Field : Params->Values)
            {
                const FString Key = EARGCompat::JsonKeyToString(Field.Key);
                if (const FString* DeclaredType = DeclaredTypeByName.Find(Key))
                {
                    PinWrightParamTypes::PinWrightCollectPathSeparatorFaults(
                        Key, *DeclaredType, Field.Value, PathFaults);
                }
            }
            if (PathFaults.Num() > 0)
            {
                const FString FaultList = FString::Join(PathFaults, TEXT("; "));
                UE_LOG(LogRpcDispatcher, Warning,
                    TEXT("[%s] Unsafe path parameter(s): %s"), *Method, *FaultList);

                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("Unsafe path parameter(s) for '%s': %s. Call '%s' with no 'args' field to fetch its wiki page."),
                        *Method, *FaultList, *Method));
                return false;
            }

            // Nested-key gate: the runtime reader of FParamSpec::NestedKeys. See
            // Handlers/NestedParamKeyCheck.h for why the nested schema had to become a declaration
            // before it could become a gate, and why an empty NestedKeys means "undeclared" rather
            // than "accepts nothing".
            //
            // It runs FOURTH, after the shape pass, because a key list is not an answer about a
            // value whose shape is already wrong: complaining that `grid.spacing` is not a key of a
            // `grid` the caller sent as a string tells them nothing they can act on. And it runs at
            // ALL only for the parameters that declare a nested schema - the other ~315
            // object/array parameters in the registry keep the previous behaviour exactly, because
            // closing one is a compatibility break that lands per parameter with its description.
            //
            // Faults are collected across every declared slot before answering, for the same reason
            // the two passes above collect theirs.
            TArray<FString> NestedFaults;
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.NestedKeys.Num() == 0)
                {
                    continue;
                }

                // Per accepted WIRE NAME, not per spec: an alias carries the same nested schema as
                // the canonical it feeds, and the message must name the spelling the caller used.
                for (const FString& WireName : CollectParamNames(Spec))
                {
                    const TSharedPtr<FJsonValue> Field = Params->TryGetField(WireName);
                    if (Field.IsValid())
                    {
                        PinWrightNestedParams::PinWrightCollectUnknownNestedKeys(
                            WireName, Field, Spec.NestedKeys, NestedFaults);
                    }
                }
            }

            if (NestedFaults.Num() > 0)
            {
                const FString FaultList = FString::Join(NestedFaults, TEXT("; "));
                UE_LOG(LogRpcDispatcher, Warning,
                    TEXT("[%s] Unknown nested key(s): %s"), *Method, *FaultList);

                Ctx.SendError(ErrorCodes::ERR_UNKNOWN_NESTED_PARAMS,
                    FString::Printf(
                        TEXT("Unknown nested key(s) for '%s': %s. Call '%s' with no 'args' field to fetch its wiki page."),
                        *Method, *FaultList, *Method));
                return false;
            }
        }

        if (Reg.bMutating && !PinWrightWorldPrecondition::Validate(Ctx, Params))
        {
            return false;
        }

        return true;
    }

    bool IsInternalDispatchParam(const FString& Name)
    {
        // "wait" is the transport-level streaming opt-in (SSE triple gate);
        // the transport consumed it when deciding to stream, so it is stripped
        // here to keep handler param specs clean of transport concerns.
        // "_expect_editor" is the caller's statement of WHICH editor it means. The gate at the
        // top of ProcessRequest consumes it; stripping it here keeps it out of every handler's
        // param spec, so an assertion never trips UNKNOWN_PARAMS.
        return Name.Equals(GFormatFieldName) ||
            Name.Equals(HttpResponseSpill::GetInternalSkipParamName()) ||
            Name.Equals(PinWrightEditorIdentity::AssertionParamName()) ||
            Name.Equals(TEXT("wait"));
    }

    // Returns Params with internal transport/format hints stripped if present.
    // Returns the original pointer when no internal field is present.
    TSharedPtr<FJsonObject> StripInternalDispatchFields(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return Params;
        }

        bool bHasInternalField = false;
        for (const auto& Pair : Params->Values)
        {
            if (IsInternalDispatchParam(EARGCompat::JsonKeyToString(Pair.Key)))
            {
                bHasInternalField = true;
                break;
            }
        }

        if (!bHasInternalField)
        {
            return Params;
        }

        TSharedPtr<FJsonObject> Stripped = MakeShared<FJsonObject>();
        for (const auto& Pair : Params->Values)
        {
            if (!IsInternalDispatchParam(EARGCompat::JsonKeyToString(Pair.Key)))
            {
                Stripped->SetField(Pair.Key, Pair.Value);
            }
        }
        return Stripped;
    }

    void ResolveCapturedResponse(
        const FResponseSink& Sink,
        const FString& RequestId,
        const FResponseCapture& Capture)
    {
        if (!Capture.bWasCalled)
        {
            return;
        }

        if (!Sink)
        {
            return;
        }

        Sink(RequestId,
             Capture.bSuccess,
             Capture.Message,
             Capture.Result,
             Capture.ErrorCode);
    }

    // Guardrail for the capture-wired auto-registered path: a handler that
    // returns without calling Ctx.SendSuccess / Ctx.SendError would otherwise
    // leave the transport waiting for a completion that never arrives, surfacing
    // as a generic timeout the caller can't distinguish from a real hang. Emit an
    // error-level diagnostic and synthesize a deterministic NO_HANDLER_RESPONSE
    // error naming the method. This is a recoverable handler-contract violation,
    // so it must not generate an ensure crash report on every occurrence.
    void ResolveMissingHandlerResponse(
        const FResponseSink& Sink,
        const FString& RequestId,
        const FString& Method)
    {
        const FString Message = FString::Printf(
            TEXT("Handler '%s' returned without sending a response"), *Method);
        UE_LOG(LogRpcDispatcher, Error, TEXT("%s"), *Message);

        if (!Sink)
        {
            return;
        }

        Sink(RequestId,
             false,
             Message,
             nullptr,
             TEXT("NO_HANDLER_RESPONSE"));
    }
}

DEFINE_LOG_CATEGORY(LogRpcDispatcher);

FRpcDispatcher::FRpcDispatcher()
    : SafePointContinuationLifetime(MakeShared<FSafePointContinuationLifetime>())
{
    SafePointContinuationLifetime->Dispatcher = this;
}

FRpcDispatcher::~FRpcDispatcher()
{
    TArray<TSharedPtr<FAsyncRequestLifetimeLease>> AsyncLifetimesToAbandon;
    AsyncLifetimesToAbandon.Reserve(
        SafePointContinuationLifetime->AsyncRequestLifetimes.Num());
    for (const TPair<uint64, TWeakPtr<FAsyncRequestLifetimeLease>>& Entry :
         SafePointContinuationLifetime->AsyncRequestLifetimes)
    {
        if (TSharedPtr<FAsyncRequestLifetimeLease> Lease = Entry.Value.Pin())
        {
            AsyncLifetimesToAbandon.Add(MoveTemp(Lease));
        }
    }
    SafePointContinuationLifetime->AsyncRequestLifetimes.Empty();

    // A core-ticker delegate may outlive subsystem deinitialization. Sever raw
    // dispatcher access first, then synchronously abandon registered async work;
    // each owner can cancel its ticker before releasing any operation state.
    SafePointContinuationLifetime->Dispatcher = nullptr;
    for (const TSharedPtr<FAsyncRequestLifetimeLease>& Lease : AsyncLifetimesToAbandon)
    {
        Lease->Abandon();
    }
    if (bProcessingRequest)
    {
        ModalStateProbe::NoteRpcDispatchEnd();
    }
}

void FRpcDispatcher::Initialize(FResponseSink InSink)
{
    ResponseSink = MoveTemp(InSink);
}

void FRpcDispatcher::RegisterHandler(const FString& MethodName, FAutomationHandler Handler)
{
    if (Handler)
    {
        Handlers.Add(MethodName, Handler);
        // Bump the generation so registry-view caches (wiki catalog) rebuild after
        // any post-init registration rather than serving a frozen snapshot.
        ++RegistryGeneration;
    }
}

void FRpcDispatcher::DrainAutoRegistrations(UPinWrightSubsystem* Subsystem)
{
    SubsystemPtr = Subsystem;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        FHandlerRegistration EffectiveReg = Reg;
        if (EffectiveReg.bMutating)
        {
            EffectiveReg.Params.Add(FParamSpec{
                PinWrightWorldPrecondition::ParamName,
                TEXT("string"),
                TEXT("Optional active-world precondition. Accepts the current world object path or package path; WORLD_MISMATCH refuses before the handler runs. Successful mutating responses echo the current world object path in world."),
                false,
                TEXT("")});
        }

        // Store the effective registration so generated wiki pages expose the
        // shared parameter without duplicating it in every handler source file.
        AutoRegistered.Add(EffectiveReg.MethodName, EffectiveReg);

        // Bridge new handler signature to TMap dispatch signature.
        // Captures the registration by value (cheap copy of method name, ParamSpec
        // array, function pointer) so the validation contract lives in one place
        // (`ValidateHandlerParams`) and is shared with the captured-handler path.
        RegisterHandler(*EffectiveReg.MethodName,
            [this, Subsystem, RegCopy = EffectiveReg, Sink = ResponseSink]
            (const FString& RequestId, const FString& Action,
             const TSharedPtr<FJsonObject>& Payload) -> bool
            {
                FResponseCapture Capture;
                FHandlerContext Ctx;
                bool bCaptureWired = false;
                if (Subsystem)
                {
                    Ctx.RequestId = RequestId;
                    Ctx.Method = RegCopy.MethodName;
                    Ctx.Payload = Payload;
                    Ctx.Subsystem = Subsystem;
                }
                else
                {
                    Ctx = FHandlerContext::MakeContextWithCapture(
                        RequestId, RegCopy.MethodName, Payload, nullptr, &Capture);
                    bCaptureWired = true;
                }
                Ctx.Dispatcher = this;

                if (!ValidateHandlerParams(RegCopy, Ctx, Payload, RegCopy.MethodName))
                {
                    if (Subsystem && Capture.bWasCalled)
                    {
                        Subsystem->DecorateAutomationResponse(
                            RequestId, Capture.bSuccess, Capture.Result);
                    }
                    ResolveCapturedResponse(Sink, RequestId, Capture);
                    return true;
                }

                // Engine confirmation dialogs raised from inside a handler own the
                // game thread in a nested Slate loop, so no RPC could ever dismiss
                // one. Auto-answer them for the duration of the call instead.
                bool bHandled = false;
                {
                    FScopedUnattendedRpc UnattendedScope;
                    bHandled = RegCopy.Func(Ctx);
                }
                if (bCaptureWired && !Capture.bWasCalled)
                {
                    ResolveMissingHandlerResponse(Sink, RequestId, RegCopy.MethodName);
                    return true;
                }
                if (Subsystem && Capture.bWasCalled)
                {
                    // Text capture paths resolve through the dispatcher
                    // sink instead of SendAutomationResponse, so apply the
                    // same transport-boundary policy before forwarding them.
                    Subsystem->DecorateAutomationResponse(
                        RequestId, Capture.bSuccess, Capture.Result);
                }
                ResolveCapturedResponse(Sink, RequestId, Capture);
                return bHandled;
            });
    }
}

void FRpcDispatcher::DrainFormatterRegistrations()
{
    for (const FFormatterRegistration& Reg : FAutoRegisterFormatter::GetPendingRegistrations())
    {
        if (Reg.Func)
        {
            Formatters.Add(Reg.MethodName, Reg.Func);
        }
    }
}

void FRpcDispatcher::RegisterResponsePolicyForRequest(
    const FString& RequestId, const FString& Method)
{
    if (!SubsystemPtr || RequestId.IsEmpty())
    {
        return;
    }

    // AutoRegistered is immutable after subsystem initialization in production.
    // Register before the game-thread hop so a request queued behind another
    // handler can still be included in a later multi-subscriber fanout.
    if (const FHandlerRegistration* Reg = AutoRegistered.Find(Method))
    {
        SubsystemPtr->RegisterResponsePolicy(RequestId, Reg->bMutating, Method);
    }
}

void FRpcDispatcher::ProcessRequest(const FString& RequestId, const FString& Method,
                                    const TSharedPtr<FJsonObject>& Params)
{
    RegisterResponsePolicyForRequest(RequestId, Method);

    // Ensure processing happens on the game thread
    if (!IsInGameThread())
    {
        const TWeakPtr<FSafePointContinuationLifetime> WeakLifetime =
            SafePointContinuationLifetime.ToWeakPtr();
        AsyncTask(ENamedThreads::GameThread,
                  [WeakLifetime, RequestId, Method, Params]()
                  {
                      const TSharedPtr<FSafePointContinuationLifetime> Lifetime =
                          WeakLifetime.Pin();
                      if (Lifetime.IsValid() && Lifetime->Dispatcher)
                      {
                          Lifetime->Dispatcher->ProcessRequest(RequestId, Method, Params);
                      }
                  });
        return;
    }

    // Editor-identity gate, and deliberately the FIRST thing checked: a request aimed at the
    // wrong editor must not be queued, deferred, or run, and it must be refused even while this
    // editor is still starting. The port is derived from the PROJECT PATH
    // (UPinWrightSettings::DerivePortFromPath), so two editors of one project contend for one
    // port and whichever won the bind answers every caller - previously without a word about
    // which process that was. A caller that states its target is told it guessed wrong; a caller
    // that states nothing is served exactly as before, so this breaks no existing client.
    // See Transport/EditorIdentity.h.
    if (Params.IsValid())
    {
        const PinWrightEditorIdentity::FAssertionVerdict Verdict =
            PinWrightEditorIdentity::CheckRequestAssertion(Params);
        if (!Verdict.bAccepted)
        {
            // Warning, matching the neighbouring UNKNOWN_PARAMS / param-type refusals: this is a
            // caller-input rejection, the response carries the whole story, and an Error here
            // would be promoted into a test failure by FAutomationTestOutputDevice
            // (AutomationTest.cpp:245) for every test that exercises the gate.
            UE_LOG(LogRpcDispatcher, Warning,
                   TEXT("Refusing '%s' (id=%s) on an editor-identity assertion: %s"),
                   *Method, *RequestId, *Verdict.Message);
            if (ResponseSink)
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetStringField(TEXT("error"), Verdict.ErrorCode);
                // Not retryable: the same port reaches the same editor next time.
                Details->SetBoolField(TEXT("retryable"), false);
                if (Verdict.Expected.IsValid())
                {
                    Details->SetObjectField(TEXT("expected"), Verdict.Expected);
                }
                if (Verdict.Actual.IsValid())
                {
                    Details->SetObjectField(TEXT("actual"), Verdict.Actual);
                }
                ResponseSink(RequestId, false, Verdict.Message, Details, Verdict.ErrorCode);
            }
            return;
        }
    }

    // The transport gate runs before wiki lookup on the I/O thread. Keep this
    // game-thread check as defense in depth for requests already in flight when
    // startup state changes, and for direct in-editor callers that bypass the
    // socket transport. No public handler body may run before the editor is
    // operationally ready.
    if (SubsystemPtr && !SubsystemPtr->IsEditorReady())
    {
        if (ResponseSink)
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("error"), ErrorCodes::ERR_EDITOR_NOT_READY);
            Details->SetBoolField(TEXT("retryable"), true);
            ResponseSink(
                RequestId, false,
                TEXT("The Unreal editor is not ready for PinWright operations; retry after startup completes."),
                Details, ErrorCodes::ERR_EDITOR_NOT_READY);
        }
        return;
    }

    // Guard against unsafe engine states (Saving, GC).
    // Do not defer solely on async loading; that can starve request processing
    // under editor automation and commandlet runs where async loading is active
    // for long periods.
    if (UE::IsSavingPackage() || IsGarbageCollecting())
    {
        UE_LOG(LogRpcDispatcher, Verbose,
               TEXT("Deferring request due to active Serialization/GC: "
                    "RequestId=%s Method=%s"),
               *RequestId, *Method);

        FDeferredRequest P;
        P.RequestId = RequestId;
        P.Method = Method;
        P.Params = Params;

        {
            FScopeLock Lock(&PendingQueueMutex);
            PendingQueue.Add(MoveTemp(P));
            bPendingScheduled = true;
        }
        return;
    }

    // Safe-point gate. A handful of methods tear a ULevel/UWorld down, or pump
    // Slate and draw a viewport, on the handler's own stack. Requests arriving from
    // the socket thread are marshalled with AsyncTask(ENamedThreads::GameThread)
    // above, and the game thread drains that queue from INSIDE UWorld::Tick while
    // waiting on tick groups — so whether such a handler lands mid-frame is pure
    // timing. Re-queue it instead: PendingQueue is drained by
    // UPinWrightSubsystem::Tick (PinWrightSubsystem.cpp:303), which runs on
    // FTSTicker::GetCoreTicker() — pumped by FEngineLoop::Tick AFTER the world tick
    // has ended, and therefore a proven safe point. Worst-case added latency is one
    // 0.1s subsystem tick, and only on the unsafe path.
    //
    // Placed after the Saving/GC defer and before the reentrancy guard so it uses
    // the identical queue, and deliberately NOT inside each handler: hand-copying
    // the gate is what let four level verbs and the whole capture family ship
    // ungated. The table and the full engine evidence live in Dispatch/SafePoint.h.
    const bool bTickUnsafeMethod = PinWrightSafePoint::IsTickUnsafeMethod(Method);
    if (bTickUnsafeMethod && !PinWrightSafePoint::IsSafeNow())
    {
        // Verbose, matching the Saving/GC defer above: a request can be re-offered
        // (and re-deferred) once per queue drain that happens to land mid-tick, so
        // this must not spam the default log.
        UE_LOG(LogPinWrightSafePoint, Verbose,
               TEXT("Deferring '%s' (id=%s) to the next safe point: %s, and this method "
                    "tears down levels or drives the viewport."),
               *Method, *RequestId,
               // Name the term that actually fired. The gate is three terms now, and for a
               // request arriving over the socket transport the pump term is the usual one -
               // it is marshalled with AsyncTask(ENamedThreads::GameThread), so it always
               // executes from a named-thread pump. Saying "a world is inside UWorld::Tick"
               // unconditionally would name the wrong cause on most deferrals.
               PinWrightSafePoint::IsAnyWorldTicking()
                   ? TEXT("a world is inside UWorld::Tick")
                   : TEXT("the game thread is draining a task-graph named-thread queue"));

        FDeferredRequest P;
        P.RequestId = RequestId;
        P.Method = Method;
        P.Params = Params;

        {
            FScopeLock Lock(&PendingQueueMutex);
            PendingQueue.Add(MoveTemp(P));
            bPendingScheduled = true;
        }
        return;
    }

    // The inline counterpart of the deferral line above, and the reason it is Log
    // rather than Verbose: only the table entries reach here, so it costs one line
    // per tick-unsafe verb, not one per queue drain. Without it the asymmetry is
    // silent exactly where it is most expensive — after an editor death the log can
    // prove a deferral happened but cannot separate "the gate allowed this" from
    // "the gate never fired", which is the question
    // B-safepoint-tick-gate-inert-on-simpletickobjects-path had to answer by an
    // engine-source dive instead of a log read. Both terms of IsSafeNow() are named
    // because either one alone would be the wrong half of the story.
    if (bTickUnsafeMethod)
    {
        UE_LOG(LogPinWrightSafePoint, Log,
               TEXT("Running '%s' (id=%s) inline: no world is inside UWorld::Tick and the "
                    "game thread is not draining a task-graph named-thread queue."),
               *Method, *RequestId);
    }

    // Reentrancy guard / enqueue
    if (bProcessingRequest)
    {
        FDeferredRequest P;
        P.RequestId = RequestId;
        P.Method = Method;
        P.Params = Params;

        {
            FScopeLock Lock(&PendingQueueMutex);
            PendingQueue.Add(MoveTemp(P));
            bPendingScheduled = true;
        }
        return;
    }

    bProcessingRequest = true;
    const double DispatchStartSeconds = FPlatformTime::Seconds();

    // Publish what this thread is about to enter, so that if the handler never
    // returns the socket I/O thread can still name it on `ping`. Two FString
    // copies under a lock the game thread cannot be holding while wedged - the
    // whole point is that this record survives the thread that wrote it going
    // silent. See Transport/ModalStateProbe.h.
    ModalStateProbe::NoteRpcDispatchBegin(RequestId, Method);

    // Same moment, different question: the probe records WHAT the game thread is
    // inside, this records WHO asked for it. editor.quit reads it back to refuse
    // ending a process another client is still driving. See State/ClientActivity.h.
    ClientActivity::NoteDispatch(RequestId, Method);

    {
        ON_SCOPE_EXIT
        {
            FinishActiveRequest();
        };

        UE_LOG(LogRpcDispatcher, Verbose, TEXT("Dispatching '%s' (id=%s)"), *Method, *RequestId);

        try
        {
            // Text-format interception: when the caller passes `_format: "text"`
            // AND a formatter is registered for the resolved method, run the
            // handler with a captured response, format the JSON result, and
            // return a small `{ format, content }` envelope. This keeps the
            // factual contract (signature/summary/params) and the presentation
            // layer in C++, with no special-casing in the JS bridge.
            FString RequestedFormat;
            const bool bWantsTextFormat =
                Params.IsValid() &&
                Params->TryGetStringField(GFormatFieldName, RequestedFormat) &&
                RequestedFormat.Equals(GFormatTextValue, ESearchCase::IgnoreCase);

            if (bWantsTextFormat)
            {
                const FRpcFormatterFunc* Formatter = Formatters.Find(Method);
                const FHandlerRegistration* Reg = AutoRegistered.Find(Method);
                if (Formatter && Reg && Reg->Func)
                {
                    // Strip the `_format` discriminator so the validator and
                    // handler don't see it as an unknown parameter.
                    TSharedPtr<FJsonObject> InnerParams = StripInternalDispatchFields(Params);

                    FResponseCapture Capture;
                    FHandlerContext Ctx = FHandlerContext::MakeContextWithCapture(
                        RequestId, Method, InnerParams, SubsystemPtr, &Capture);
                    Ctx.Dispatcher = this;

                    if (ValidateHandlerParams(*Reg, Ctx, InnerParams, Method))
                    {
                        // Same modal-suppression scope as the main dispatch path.
                        FScopedUnattendedRpc UnattendedScope;
                        Reg->Func(Ctx);
                    }

                    const double DurationMs = (FPlatformTime::Seconds() - DispatchStartSeconds) * 1000.0;
                    UE_LOG(LogRpcDispatcher, Verbose,
                           TEXT("Handler '%s' (text-format) %s (%.1f ms)"),
                           *Method,
                           Capture.bWasCalled ? (Capture.bSuccess ? TEXT("OK") : TEXT("error"))
                                              : TEXT("no-response"),
                           DurationMs);

                    if (!ResponseSink)
                    {
                        return;
                    }

                    if (!Capture.bWasCalled)
                    {
                        TSharedPtr<FJsonObject> ErrorResult;
                        if (SubsystemPtr)
                        {
                            SubsystemPtr->DecorateAutomationResponse(RequestId, false, ErrorResult);
                        }
                        ResponseSink(RequestId, false,
                            FString::Printf(TEXT("Handler produced no response: %s"), *Method),
                            ErrorResult, TEXT("NO_RESPONSE"));
                        return;
                    }

                    if (!Capture.bSuccess)
                    {
                        // Forward error responses unchanged — formatter only applies on success.
                        TSharedPtr<FJsonObject> ErrorResult = Capture.Result;
                        if (SubsystemPtr)
                        {
                            SubsystemPtr->DecorateAutomationResponse(RequestId, false, ErrorResult);
                        }
                        ResponseSink(RequestId, false, Capture.Message,
                            ErrorResult, Capture.ErrorCode);
                        return;
                    }

                    TSharedPtr<FJsonObject> DecoratedResult = Capture.Result;
                    if (SubsystemPtr)
                    {
                        // Decorate before formatting so the formatter's content
                        // and the structured fallback observe the same result.
                        SubsystemPtr->DecorateAutomationResponse(RequestId, true, DecoratedResult);
                    }
                    FString FormattedText;
                    const bool bFormatted = (*Formatter)(DecoratedResult, FormattedText);
                    if (bFormatted)
                    {
                        TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
                        Envelope->SetStringField(TEXT("format"), GFormatTextValue);
                        Envelope->SetStringField(TEXT("content"), FormattedText);
                        ResponseSink(RequestId, true, TEXT(""), Envelope, TEXT(""));
                    }
                    else
                    {
                        // Formatter declined — fall back to the JSON shape so the
                        // caller still gets the data.
                        ResponseSink(RequestId, true, TEXT(""), DecoratedResult, TEXT(""));
                    }
                    return;
                }
            }

            // Normal dispatch path strips internal transport/format hints before
            // handler validation so public unknown-param checks stay strict.
            const TSharedPtr<FJsonObject> EffectiveParams =
                StripInternalDispatchFields(Params);

            // TMap-only dispatch (O(1) lookup)
            if (const FAutomationHandler* Handler = Handlers.Find(Method))
            {
                const bool bHandled = (*Handler)(RequestId, Method, EffectiveParams);
                const double DurationMs = (FPlatformTime::Seconds() - DispatchStartSeconds) * 1000.0;
                UE_LOG(LogRpcDispatcher, Verbose,
                       TEXT("Handler '%s' %s (%.1f ms)"),
                       *Method, bHandled ? TEXT("OK") : TEXT("declined"), DurationMs);
                if (bHandled)
                {
                    return;
                }
            }

            // No handler found - send error via the response sink
            if (ResponseSink)
            {
                // A method owned by a gated integration sub-module whose engine
                // plugin is disabled is a known name that cannot run in this
                // project, not a typo — name the disabled plugin instead of
                // offering fuzzy suggestions.
                const FString DisabledPlugin = IntegrationGates::FindSkippedByMethod(Method);
                if (!DisabledPlugin.IsEmpty())
                {
                    ResponseSink(RequestId, false,
                        FString::Printf(
                            TEXT("Method '%s' is unavailable because the '%s' engine plugin is disabled in this project; enable it and restart the editor."),
                            *Method, *DisabledPlugin),
                        nullptr, ErrorCodes::ERR_PLUGIN_DISABLED);
                    return;
                }

                // Offer near-miss suggestions so a wrong-name caller self-corrects
                // from the error without a wiki round-trip. Only rank when the method
                // has no registered handler at all; a registered handler that declined
                // (bHandled==false) also falls through here, and without the guard the
                // method would suggest itself.
                TArray<FString> Suggestions;
                if (!Handlers.Contains(Method))
                {
                    // Rank over name AND registered summary, not the dotted name
                    // alone: a bare five-name list cannot distinguish a tag search
                    // from a name search, and the name-only score is dominated by
                    // the shared namespace prefix. See SuggestionHelpers.h for the
                    // measured miss this replaces.
                    TArray<SuggestionHelpers::FSuggestionCandidate> Pool;
                    Pool.Reserve(AutoRegistered.Num());
                    for (const TPair<FString, FHandlerRegistration>& Entry : AutoRegistered)
                    {
                        Pool.Add({ Entry.Key, Entry.Value.Summary });
                    }
                    Suggestions = SuggestionHelpers::RankSuggestionsWithSummaries(
                        Method.ToLower(), Pool, 5);
                }

                const FString Message = Suggestions.Num() > 0
                    ? FString::Printf(
                        TEXT("Unknown action: %s. Did you mean: %s? Call one with no 'args' field to fetch its wiki page."),
                        *Method, *FString::Join(Suggestions, TEXT("; ")))
                    : FString::Printf(
                        TEXT("Unknown action: %s. No close matches. Call with no 'method' field to fetch the namespace index."),
                        *Method);

                ResponseSink(RequestId, false, Message, nullptr, TEXT("UNKNOWN_ACTION"));
            }
        }
        catch (const std::exception& E)
        {
            UE_LOG(LogRpcDispatcher, Error,
                   TEXT("Exception in handler %s: %s"), *RequestId, ANSI_TO_TCHAR(E.what()));
            if (ResponseSink)
            {
                ResponseSink(RequestId, false,
                    FString::Printf(TEXT("Internal error: %s"), ANSI_TO_TCHAR(E.what())),
                    nullptr, TEXT("INTERNAL_ERROR"));
            }
        }
        catch (...)
        {
            UE_LOG(LogRpcDispatcher, Error,
                   TEXT("Unknown exception in handler %s"), *RequestId);
            if (ResponseSink)
            {
                ResponseSink(RequestId, false,
                    TEXT("Internal error (unknown)."),
                    nullptr, TEXT("INTERNAL_ERROR"));
            }
        }
    }
}

bool FRpcDispatcher::DeferActiveRequestToSafePoint(
    TFunction<void()> Work, const TCHAR* Reason,
    TFunction<void()> OnOwnerAbandoned)
{
    if (!IsInGameThread() || !bProcessingRequest)
    {
        return false;
    }

    ++PendingSafePointContinuations;
    const TWeakPtr<FSafePointContinuationLifetime> WeakThis =
        SafePointContinuationLifetime.ToWeakPtr();
    PinWrightSafePoint::DeferToSafePoint(
        [WeakThis, Work = MoveTemp(Work),
         OnOwnerAbandoned = MoveTemp(OnOwnerAbandoned)]() mutable
        {
            const TSharedPtr<FSafePointContinuationLifetime> This = WeakThis.Pin();
            if (!This.IsValid() || !This->Dispatcher)
            {
                if (OnOwnerAbandoned)
                {
                    OnOwnerAbandoned();
                }
                return;
            }

            ON_SCOPE_EXIT
            {
                if (FRpcDispatcher* Dispatcher = This->Dispatcher)
                {
                    check(Dispatcher->PendingSafePointContinuations > 0);
                    --Dispatcher->PendingSafePointContinuations;
                    Dispatcher->FinishActiveRequest();
                }
            };
            Work();
        }, Reason);
    return true;
}

TSharedPtr<FAsyncRequestLifetimeLease> FRpcDispatcher::RetainAsyncRequestLifetime(
    TFunction<void()> OnOwnerAbandoned)
{
    if (!IsInGameThread() || !bProcessingRequest)
    {
        return nullptr;
    }

    const uint64 LifetimeId =
        ++SafePointContinuationLifetime->LastAsyncRequestLifetimeId;
    TSharedRef<FAsyncRequestLifetimeLease> Lease =
        MakeShareable(new FAsyncRequestLifetimeLease());
    Lease->OnOwnerAbandoned = MoveTemp(OnOwnerAbandoned);

    const TWeakPtr<FSafePointContinuationLifetime> WeakLifetime =
        SafePointContinuationLifetime.ToWeakPtr();
    Lease->ReleaseRegistration = [WeakLifetime, LifetimeId]()
    {
        if (const TSharedPtr<FSafePointContinuationLifetime> Lifetime = WeakLifetime.Pin())
        {
            Lifetime->AsyncRequestLifetimes.Remove(LifetimeId);
        }
    };
    SafePointContinuationLifetime->AsyncRequestLifetimes.Add(
        LifetimeId, Lease.ToWeakPtr());
    return Lease;
}

void FRpcDispatcher::FinishActiveRequest()
{
    if (PendingSafePointContinuations > 0)
    {
        return;
    }

    // Clear before draining: ProcessPendingRequests re-enters ProcessRequest and
    // publishes the next request's identity. Deferred safe-point continuations
    // keep this scope active until their last body returns.
    ModalStateProbe::NoteRpcDispatchEnd();
    bProcessingRequest = false;
    if (bPendingScheduled)
    {
        bPendingScheduled = false;
        ProcessPendingRequests();
    }
}

void FRpcDispatcher::ProcessPendingRequests()
{
    if (!IsInGameThread())
    {
        AsyncTask(ENamedThreads::GameThread,
                  [this]() { ProcessPendingRequests(); });
        return;
    }

    // An in-handler RunAtSafePoint continuation owns the current request scope
    // across ticker passes. Leave queued RPCs parked until that body releases it.
    if (bProcessingRequest)
    {
        return;
    }

    TArray<FDeferredRequest> LocalQueue;
    {
        FScopeLock Lock(&PendingQueueMutex);
        if (PendingQueue.Num() == 0)
        {
            bPendingScheduled = false;
            return;
        }
        LocalQueue = MoveTemp(PendingQueue);
        PendingQueue.Empty();
        bPendingScheduled = false;
    }

    for (const FDeferredRequest& Req : LocalQueue)
    {
        ProcessRequest(Req.RequestId, Req.Method, Req.Params);
    }
}

bool FRpcDispatcher::DispatchMethod(const FString& MethodName, const FString& RequestId,
                                    const TSharedPtr<FJsonObject>& Payload)
{
    const bool bNestedInAcceptedRequest = bProcessingRequest;
    if (const FAutomationHandler* Handler = Handlers.Find(MethodName))
    {
        const FHandlerRegistration* Reg = AutoRegistered.Find(MethodName);
        if (SubsystemPtr && !RequestId.IsEmpty())
        {
            if (Reg)
            {
                if (bNestedInAcceptedRequest)
                {
                    SubsystemPtr->RegisterNestedResponsePolicy(RequestId, Reg->bMutating, MethodName);
                }
                else
                {
                    // Direct callers of this public dispatch seam represent a new
                    // accepted request when no outer ProcessRequest is active.
                    SubsystemPtr->RegisterResponsePolicy(RequestId, Reg->bMutating, MethodName);
                }
            }
        }

        // DispatchMethod is also a public handler-to-handler seam and bypasses
        // ProcessRequest/ValidateHandlerParams. Apply the same world assertion
        // immediately before entry, while preserving the outer response policy
        // for nested calls.
        if (Reg && Reg->bMutating)
        {
            FHandlerContext PreconditionContext;
            PreconditionContext.RequestId = RequestId;
            PreconditionContext.Method = MethodName;
            PreconditionContext.Payload = Payload;
            PreconditionContext.Subsystem = SubsystemPtr;
            PreconditionContext.Dispatcher = this;
#if WITH_DEV_AUTOMATION_TESTS
            PreconditionContext.ResponseCapture = ResponseCaptureForTesting;
#endif
            if (!PinWrightWorldPrecondition::Validate(PreconditionContext, Payload))
            {
                return true;
            }
        }
        return (*Handler)(RequestId, MethodName, Payload);
    }
    return false;
}

void FRpcDispatcher::GetRegisteredToolKeys(TArray<FString>& OutToolKeys) const
{
    OutToolKeys.Reset();
    Handlers.GetKeys(OutToolKeys);
    OutToolKeys.Sort();
}
