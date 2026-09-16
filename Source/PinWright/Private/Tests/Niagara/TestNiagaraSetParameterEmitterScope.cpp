// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.set_parameter's emitter-scoped parameter stores
// (B-niagara-set-parameter-emitter-scope-unreachable).
//
// The defect: three of the verb's six scopes — rendererBindings, spawnRapidIteration and
// updateRapidIteration — live on an emitter. ResolveParameterStore refuses all three with
// EMITTER_REQUIRED unless the resolved target carries EmitterData, and the only wire key
// that produces EmitterData is `emitter` (ParseParameterPayload reads it into
// FNiagaraParameterEditPayload::EmitterName, which ValidateParameterPayload copies into the
// target spec). That key was never DECLARED on the verb, so the dispatcher's
// declared-parameter gate (RpcDispatcher.cpp ValidateHandlerParams -> AddKnownParamNames)
// rejected the payload with UNKNOWN_PARAMS before the handler body ran. Two contradictory
// refusals and no third option: every emitter rapid-iteration and renderer-binding value was
// unwritable by any argument combination. The sibling parameter-store verbs that already
// declare `emitter` (niagara.set_curve_keys, niagara.add_data_interface,
// niagara.rename_parameter) are what make this an omission rather than a design.
//
// Both tests route through a REAL FRpcDispatcher on purpose: Tests/TestUtils.h's
// InvokeHandler calls the registered function directly and never runs ValidateHandlerParams,
// so it cannot observe the gate that caused this bug at all.
//
// Counterfactual — deleting the RPC_PARAM_OPT("emitter", ...) line fails both tests:
// EmitterParamReachesTheHandler sees UNKNOWN_PARAMS where it expects ASSET_NOT_FOUND, and
// EmitterScopedStoreIsWritable sees UNKNOWN_PARAMS where it expects a successful write.
// Reverting the resolver so the emitter branch is skipped instead fails the second test's
// read-back while leaving the first green — which is why the write is asserted on the store,
// not on the response.
#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with identically-shaped helpers in the
// sibling Niagara tests (same convention as Tests/Infra/ParamSpecTestHelpers.h).
namespace NiagaraSetParameterEmitterScopeTestLocal
{
    // An emitter-scoped scope, so the payload always takes ResolveParameterStore's
    // post-EMITTER_REQUIRED branch — the region the defect made unreachable.
    const TCHAR* const EmitterScope = TEXT("spawnRapidIteration");

    // Synthetic rapid-iteration parameter name. Real ones are
    // Constants.<Emitter>.<Module>.<Input>; nothing in the write path parses the shape, so a
    // fixture-owned name keeps the test independent of any host's module layout.
    const TCHAR* const ProbeParameter = TEXT("Constants.PinWrightProbe.Scalar");

    TSharedPtr<FJsonObject> MakePayload(const FString& AssetPath, const FString& Emitter, double Value)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("scope"), EmitterScope);
        Params->SetStringField(TEXT("name"), ProbeParameter);
        Params->SetStringField(TEXT("type"), TEXT("float"));
        Params->SetNumberField(TEXT("value"), Value);
        Params->SetBoolField(TEXT("compile"), false);
        Params->SetBoolField(TEXT("save"), false);
        if (!Emitter.IsEmpty())
        {
            Params->SetStringField(TEXT("emitter"), Emitter);
        }
        return Params;
    }
}

// ============================================================================
// 1. Declaration + gate: `emitter` is a declared optional string, and a payload carrying it
//    clears the dispatcher's declared-parameter gate and reaches the handler body. The
//    probe asset never existed, so ASSET_NOT_FOUND is proof the payload got as far as
//    ResolveTarget — a stronger statement than "not UNKNOWN_PARAMS". The control payload
//    carries a genuinely undeclared key so a vacuous pass (a gate that stopped running at
//    all) fails here too.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterEmitterParamReachesHandlerTest,
    "PinWright.niagara.set_parameter.EmitterParamReachesTheHandler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterEmitterParamReachesHandlerTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSetParameterEmitterScopeTestLocal;

    const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("niagara.set_parameter"), TEXT("emitter"));
    if (!TestNotNull(TEXT("niagara.set_parameter declares an 'emitter' parameter"), Spec))
    {
        return false;
    }
    TestEqual(TEXT("'emitter' is declared as a string"), Spec->Type, FString(TEXT("string")));
    TestFalse(TEXT("'emitter' is optional (the system-wide scopes do not need it)"), Spec->bRequired);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // A path under a folder no host ships: the handler must fail on the asset, not on the schema.
    const FString MissingAsset(TEXT("/Game/__PW_GatewayTests/DoesNotExist_NiagaraSetParameterEmitter"));

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
        TEXT("req-niagara-set-parameter-emitter"),
        MakePayload(MissingAsset, TEXT("AnyEmitter"), 1.0), bSuccess, ErrorCode);

    TestFalse(TEXT("a missing asset still fails"), bSuccess);
    TestNotEqual(TEXT("'emitter' is not rejected as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    TestEqual(TEXT("the payload reached ResolveTarget"), ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));

    // Control: the gate is still live, so this test cannot pass vacuously.
    TSharedPtr<FJsonObject> Undeclared = MakePayload(MissingAsset, TEXT("AnyEmitter"), 1.0);
    Undeclared->SetStringField(TEXT("notAParameter"), TEXT("x"));
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
        TEXT("req-niagara-set-parameter-undeclared"),
        Undeclared, bSuccess, ErrorCode);
    TestEqual(TEXT("an undeclared key is still rejected by the gate"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}

// ============================================================================
// 2. Acceptance: the declared `emitter` selects the emitter's spawn rapid-iteration store and
//    the value lands in it. Asserted on the store rather than on the response, because a
//    response echo cannot distinguish "wrote the emitter's store" from "wrote nothing". The
//    emitter-omitted call is asserted first: it must still fail EMITTER_REQUIRED and leave
//    the seeded value untouched, which is what proves the parameter is load-bearing rather
//    than accepted-and-ignored.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterEmitterScopedStoreWritableTest,
    "PinWright.niagara.set_parameter.EmitterScopedStoreIsWritable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterEmitterScopedStoreWritableTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSetParameterEmitterScopeTestLocal;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    FNiagaraEmitterHandle* Handle = nullptr;
    for (const FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
    {
        if (Candidate.GetName() == EmitterName)
        {
            Handle = const_cast<FNiagaraEmitterHandle*>(&Candidate);
            break;
        }
    }
    if (!TestNotNull(TEXT("emitter handle is present"), Handle))
    {
        return false;
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    UNiagaraScript* SpawnScript = EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr;
    if (!TestNotNull(TEXT("emitter spawn script resolved"), SpawnScript))
    {
        return false;
    }

    // Seed the parameter: set_parameter writes existing entries only (add_parameter creates).
    FNiagaraParameterStore& Store = SpawnScript->RapidIterationParameters;
    const FNiagaraVariable Probe(FNiagaraTypeDefinition::GetFloatDef(), FName(ProbeParameter));
    const float SeedValue = 1.0f;
    Store.SetParameterData(reinterpret_cast<const uint8*>(&SeedValue), Probe, /*bAdd=*/true);
    if (!TestEqual(TEXT("probe seeded into the emitter spawn rapid-iteration store"),
            Store.GetParameterValueOrDefault<float>(Probe, 0.0f), SeedValue))
    {
        return false;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Without `emitter` the emitter-scoped store is still — correctly — unreachable.
    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
        TEXT("req-niagara-set-parameter-no-emitter"),
        MakePayload(SystemPath, FString(), 7.5), bSuccess, ErrorCode);
    TestFalse(TEXT("an emitter scope without 'emitter' fails"), bSuccess);
    TestEqual(TEXT("it fails as EMITTER_REQUIRED"), ErrorCode, FString(TEXT("EMITTER_REQUIRED")));
    TestEqual(TEXT("the refused call wrote nothing"),
        Store.GetParameterValueOrDefault<float>(Probe, 0.0f), SeedValue);

    // With `emitter` the resolver reaches the emitter's store and the write lands.
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
        TEXT("req-niagara-set-parameter-with-emitter"),
        MakePayload(SystemPath, EmitterName.ToString(), 7.5), bSuccess, Result, ErrorCode);
    if (!TestTrue(FString::Printf(TEXT("set_parameter succeeds with 'emitter' (error was '%s')"), *ErrorCode),
            bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("emitter spawn rapid-iteration value updated"),
        Store.GetParameterValueOrDefault<float>(Probe, 0.0f), 7.5f);

    if (TestNotNull(TEXT("set_parameter returns a result"), Result.Get()))
    {
        FString EchoedScope;
        Result->TryGetStringField(TEXT("scope"), EchoedScope);
        TestEqual(TEXT("the response echoes the emitter scope"), EchoedScope, FString(EmitterScope));
    }

    return true;
}

// ============================================================================
// 3. Refusal: the three SYSTEM-WIDE scopes must not accept an `emitter` and ignore it
//    (B-niagara-emitter-param-accepted-and-ignored).
//
//    The defect: ResolveTarget resolves `emitter` into an FNiagaraEmitterHandle for every
//    target kind, but ResolveParameterStore's user / systemSpawnRapidIteration /
//    systemUpdateRapidIteration branches return a store hanging off the UNiagaraSystem and
//    never read the handle. A caller who named an emitter got `success` back for a write that
//    emitter had nothing to do with -- rpc-design.md section 21's accepted-and-ignored failure.
//
//    All three scopes are exercised because the fix lives in the shared resolver, not in the
//    verb: a per-verb or per-scope guard would leave the others diverging. Both directions are
//    asserted per scope -- the refusal writes nothing, and the same payload WITHOUT `emitter`
//    still writes -- so a guard that over-refuses fails here too.
//
//    Counterfactual: reverting the ResolveParameterStore guard makes every scope's first
//    dispatch succeed, failing both the INVALID_ARGUMENT assertion and the "wrote nothing"
//    read-back on the store.
// ============================================================================
namespace NiagaraSetParameterSystemScopeTestLocal
{
    // One probe name per store: the three system-wide stores do not agree on the name they keep.
    // The two rapid-iteration stores are plain FNiagaraParameterStores and hold what they are given.
    // The user store is a FNiagaraUserRedirectionParameterStore whose AddParameter override rewrites
    // an un-namespaced entry to User.<Name> and files the bare name only as a redirect key
    // (NiagaraUserRedirectionParameterStore.cpp). The verb matches on the STORED name --
    // ApplyParameterMutation's FindParameterByName walks GetParameters(), which reports the qualified
    // name -- while FNiagaraParameterStore::IndexOf goes through the virtual FindParameterOffset and
    // follows the redirect. Seeding the bare name into the user store therefore reads back fine here
    // and is still invisible to the verb, which answers PARAMETER_NOT_FOUND. Address that store by its
    // qualified name, as the sibling tests and the production niagara.add_parameter callers do.
    const TCHAR* const SystemProbeParameter = TEXT("PinWrightSystemScopeProbe");
    const TCHAR* const UserProbeParameter = TEXT("User.PinWrightSystemScopeProbe");

    TSharedPtr<FJsonObject> MakePayload(const FString& AssetPath, const TCHAR* Scope, const TCHAR* ParameterName, const FString& Emitter, double Value)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("scope"), Scope);
        Params->SetStringField(TEXT("name"), ParameterName);
        Params->SetStringField(TEXT("type"), TEXT("float"));
        Params->SetNumberField(TEXT("value"), Value);
        Params->SetBoolField(TEXT("compile"), false);
        Params->SetBoolField(TEXT("save"), false);
        if (!Emitter.IsEmpty())
        {
            Params->SetStringField(TEXT("emitter"), Emitter);
        }
        return Params;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterSystemScopeRejectsEmitterTest,
    "PinWright.niagara.set_parameter.SystemScopeRejectsEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterSystemScopeRejectsEmitterTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSetParameterSystemScopeTestLocal;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript();
    UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();
    if (!TestNotNull(TEXT("system spawn script resolved"), SystemSpawnScript)
        || !TestNotNull(TEXT("system update script resolved"), SystemUpdateScript))
    {
        return false;
    }

    struct FSystemScopeProbe
    {
        const TCHAR* Scope;
        const TCHAR* Parameter;
        FNiagaraParameterStore* Store;
    };
    const FSystemScopeProbe Probes[] = {
        { TEXT("user"), UserProbeParameter, &System->GetExposedParameters() },
        { TEXT("systemSpawnRapidIteration"), SystemProbeParameter, &SystemSpawnScript->RapidIterationParameters },
        { TEXT("systemUpdateRapidIteration"), SystemProbeParameter, &SystemUpdateScript->RapidIterationParameters },
    };

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const float SeedValue = 1.0f;
    const float WriteValue = 7.5f;

    for (const FSystemScopeProbe& ScopeProbe : Probes)
    {
        const FString ScopeName(ScopeProbe.Scope);
        FNiagaraParameterStore& Store = *ScopeProbe.Store;
        const FNiagaraVariable Probe(FNiagaraTypeDefinition::GetFloatDef(), FName(ScopeProbe.Parameter));

        // set_parameter writes existing entries only, so seed one per store.
        Store.SetParameterData(reinterpret_cast<const uint8*>(&SeedValue), Probe, /*bAdd=*/true);
        if (!TestEqual(FString::Printf(TEXT("[%s] probe seeded"), *ScopeName),
                Store.GetParameterValueOrDefault<float>(Probe, 0.0f), SeedValue))
        {
            return false;
        }

        // Naming an emitter on a system-wide scope is refused, not silently honoured elsewhere.
        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
            FString::Printf(TEXT("req-niagara-set-parameter-system-scope-%s"), *ScopeName),
            MakePayload(SystemPath, ScopeProbe.Scope, ScopeProbe.Parameter, EmitterName.ToString(), WriteValue),
            bSuccess, ErrorCode);
        TestFalse(FString::Printf(TEXT("[%s] 'emitter' is refused, not accepted-and-ignored"), *ScopeName), bSuccess);
        TestEqual(FString::Printf(TEXT("[%s] it fails as INVALID_ARGUMENT"), *ScopeName),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestEqual(FString::Printf(TEXT("[%s] the refused call wrote nothing"), *ScopeName),
            Store.GetParameterValueOrDefault<float>(Probe, 0.0f), SeedValue);

        // The same payload without 'emitter' still writes: the guard is scoped, not a blanket refusal.
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
            FString::Printf(TEXT("req-niagara-set-parameter-system-scope-plain-%s"), *ScopeName),
            MakePayload(SystemPath, ScopeProbe.Scope, ScopeProbe.Parameter, FString(), WriteValue),
            bSuccess, ErrorCode);
        TestTrue(FString::Printf(TEXT("[%s] the same write without 'emitter' succeeds (error was '%s')"),
            *ScopeName, *ErrorCode), bSuccess);
        TestEqual(FString::Printf(TEXT("[%s] the accepted call wrote the value"), *ScopeName),
            Store.GetParameterValueOrDefault<float>(Probe, 0.0f), WriteValue);
    }

    return true;
}
