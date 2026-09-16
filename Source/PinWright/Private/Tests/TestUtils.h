// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared test utilities for PinWright handler tests.
// Extracts duplicated InvokeHandler() and provides response capture.
#pragma once
#include <initializer_list>
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Tests/TestSkipReporting.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Misc/EngineVersionComparison.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "K2Node_Event.h"
#include "HAL/FileManager.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "ShaderCompiler.h"
#include "Tests/TestAssetTeardown.h"

// FTestResponseCapture is defined in HandlerContext.h (guarded by WITH_DEV_AUTOMATION_TESTS).

// Returns the first UActorComponent on Actor whose name equals Name (case-insensitive),
// or nullptr. Centralizes the TInlineComponentArray + GetComponents + name-match scan that
// duplicate/rename/component tests otherwise hand-roll to locate a component the handler
// created (e.g. a "<source>_Copy"). Callers Cast<> the result to the concrete component type.
inline UActorComponent* FindActorComponentByName(AActor* Actor, const FString& Name)
{
    if (!Actor)
    {
        return nullptr;
    }
    TInlineComponentArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    for (UActorComponent* Component : Components)
    {
        if (Component && Component->GetName().Equals(Name, ESearchCase::IgnoreCase))
        {
            return Component;
        }
    }
    return nullptr;
}

// Returns true if a handler with the given method name is registered.
inline bool IsHandlerRegistered(const FString& MethodName)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == MethodName)
        {
            return true;
        }
    }
    return false;
}

// Alias used by some test files.
inline bool IsRegistered(const FString& MethodName)
{
    return IsHandlerRegistered(MethodName);
}

// Returns the registered Summary string for a method, or empty if not registered.
// Centralizes the registration-scan loop so summary-metadata assertions don't
// each hand-roll their own copy.
inline FString GetRegisteredSummary(const FString& MethodName)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == MethodName)
        {
            return Reg.Summary;
        }
    }
    return FString();
}

// Returns the FParamSpec for a registered method's named parameter, or nullptr if
// the method isn't registered or has no such param. Centralizes the two-level
// "find handler registration, then find its named param spec" scan that
// param-description regression tests otherwise hand-roll inline. (Named to sit
// alongside GetRegisteredSummary and not collide with the file-local FindParamSpec
// in TestAssetPathParamAlias.cpp under a Unity merge.)
inline const FParamSpec* GetRegisteredParamSpec(const FString& MethodName, const FString& ParamName)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != MethodName)
        {
            continue;
        }
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name == ParamName)
            {
                return &Spec;
            }
        }
    }
    return nullptr;
}

// Returns the first entry of the named array field on Object that is a JSON object
// whose SubField string value equals Value, or nullptr if none. Generic
// find-and-return walk shared by tests that need the matched entry (not just a
// membership bool — JsonArrayHasObjectWithStringField delegates here for the bool).
inline TSharedPtr<FJsonObject> JsonArrayFindObjectByStringField(const TSharedPtr<FJsonObject>& Object,
    const FString& ArrayName, const FString& SubField, const FString& Value)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(ArrayName, Arr) || !Arr)
    {
        return nullptr;
    }
    for (const TSharedPtr<FJsonValue>& Val : *Arr)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Val.IsValid() && Val->TryGetObject(Entry) && Entry && (*Entry).IsValid())
        {
            FString Found;
            if ((*Entry)->TryGetStringField(SubField, Found) && Found == Value)
            {
                return *Entry;
            }
        }
    }
    return nullptr;
}

// Returns true if the named array field on Object holds an entry that is a JSON
// object whose SubField string value equals Value. Generic membership walk shared
// by tests that assert a typed entry appears in a result array.
inline bool JsonArrayHasObjectWithStringField(const TSharedPtr<FJsonObject>& Object,
    const FString& ArrayName, const FString& SubField, const FString& Value)
{
    return JsonArrayFindObjectByStringField(Object, ArrayName, SubField, Value).IsValid();
}

// Returns true if the JSON value array holds a bare string entry equal to Value.
// Core membership walk for string arrays (e.g. applied[] of UPROPERTY names);
// the object+field variant above is for arrays of objects, this one for arrays
// of plain strings.
inline bool JsonValueArrayContainsString(const TArray<TSharedPtr<FJsonValue>>* Arr, const FString& Value)
{
    if (!Arr) return false;
    for (const TSharedPtr<FJsonValue>& Val : *Arr)
    {
        FString Found;
        if (Val.IsValid() && Val->TryGetString(Found) && Found == Value)
        {
            return true;
        }
    }
    return false;
}

// Returns true if the named array field on Object is a string array containing Value.
// Sibling of JsonArrayHasObjectWithStringField for arrays of bare JSON strings
// (not objects). Wraps the array lookup around the shared JsonValueArrayContainsString walk.
inline bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object,
    const FString& ArrayName, const FString& Value)
{
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(ArrayName, Arr))
    {
        return false;
    }
    return JsonValueArrayContainsString(Arr, Value);
}

// Finds and invokes the named handler with the given payload.
// Returns true if the handler was found in the registration list, false if missing.
//
// THIS CALLS THE HANDLER BODY DIRECTLY AND RUNS NO PART OF THE DISPATCHER. In particular
// it skips FRpcDispatcher::ValidateHandlerParams (Dispatch/RpcDispatcher.cpp), the gate that
// rejects a request naming a parameter the registration never declared. That is deliberate --
// it lets a test drive one handler without standing up a dispatcher -- but it means a test
// written this way CANNOT observe a whole class of defect: a verb whose body reads a wire key
// its RPC_PARAMS omits works here and is refused UNKNOWN_PARAMS for every real caller. That is
// not hypothetical; see B-niagara-set-parameter-emitter-scope-unreachable, where the entire
// resolution pipeline worked and only the declaration was missing, with the verb's tests green.
//
// Two existing routes see what this one cannot, and a test asserting a parameter contract wants
// one of them rather than a third invoke path here:
//   - Tests/Infra/ParamSpecTestHelpers.h -- ParamSpecTestHelpers::IsParamAccepted(Method, Key)
//     answers "does the dispatcher accept this wire name for this verb" off the registry, with
//     no dispatch and no handler body. Cheapest, and the right assertion when the question is
//     the DECLARATION.
//   - Tests/Infra/DispatcherTestHelpers.h -- MakeDispatcher + Dispatch routes a real payload
//     through a real FRpcDispatcher, gate included. Use when the question is end-to-end
//     acceptance; note it RUNS the handler body when the payload clears the gate.
// PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams sweeps the whole registry for
// this defect class, so a new verb does not depend on someone remembering to assert it.
inline bool InvokeHandler(const FString& MethodName, const TSharedPtr<FJsonObject>& Payload = nullptr)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == MethodName)
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContext(
                TEXT("test-id"), MethodName, Payload);
            Reg.Func(Ctx);
            return true;
        }
    }
    return false;
}

// Finds and invokes the named handler, capturing the response into Capture.
// Returns true if the handler was found in the registration list, false if missing.
inline bool InvokeHandlerWithCapture(const FString& MethodName,
    const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
{
    Capture.Reset();
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == MethodName)
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
                TEXT("test-id"), MethodName, Payload, &Capture);
            Reg.Func(Ctx);
            return true;
        }
    }
    return false;
}

// Cancels a Play-In-Editor session request queued on the current stack, if any, so a
// deferred RequestPlaySession issued by a handler under test (editor.play) never
// actually starts PIE on a later tick. Starting PIE under -unattended
// walks dirty transient Blueprints left by sibling tests and can crash compile, so the
// PIE-alias no-crash / revert-detector tests neutralize the request on the same stack — a
// same-stack cancel guarantees StartQueuedPlaySessionRequest is a no-op. Only cancels a
// request we caused (a no-op when nothing is queued). Shared so the neutralize-PIE contract
// lives in one place for every current and future PIE-starting handler test.
inline void CancelAnyQueuedPlaySession()
{
    if (GEditor && GEditor->IsPlaySessionRequestQueued())
    {
        GEditor->CancelRequestPlaySession();
    }
}

// Invokes the named handler with the given payload and asserts the honest-failure
// NOT_IMPLEMENTED contract: handler is registered, the response is an error (not a
// fake success), and the error code is exactly "NOT_IMPLEMENTED". Shared by every
// stub-handler regression test so the contract lives in one place.
inline void TestHandlerReturnsNotImplemented(FAutomationTestBase& Test, const FString& Method,
    const TSharedPtr<FJsonObject>& Payload = MakeShared<FJsonObject>())
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), *Method), bFound);
    Test.TestFalse(*FString::Printf(TEXT("%s response is error, not success"), *Method),
        Capture.bSuccess);
    Test.TestEqual(*FString::Printf(TEXT("%s error code is NOT_IMPLEMENTED"), *Method),
        Capture.ErrorCode, FString(TEXT("NOT_IMPLEMENTED")));
}

// Invokes the named handler with the given payload and asserts the disabled
// dead-end contract shared by the deprecated ai.* BT-authoring verbs: handler is
// registered, the response is an error (not a fake success), the error code is
// exactly "DEPRECATED_HANDLER", and the steer Message routes to the replacement
// (SteerSubstring). A plausible-but-bogus payload proves the verb errors off before
// any asset lookup — reverting the disablement would make the old body fail
// differently (e.g. NOT_FOUND on the bogus path), not DEPRECATED_HANDLER. Shared by
// every disabled-orphaning-verb regression test so the contract lives in one place
// rather than being copy-pasted per case.
inline void TestHandlerReturnsDeprecated(FAutomationTestBase& Test, const FString& Method,
    const TSharedPtr<FJsonObject>& Payload, const FString& SteerSubstring)
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), *Method), bFound);
    Test.TestFalse(*FString::Printf(TEXT("%s response is error, not a fake success"), *Method),
        Capture.bSuccess);
    Test.TestEqual(*FString::Printf(TEXT("%s error code is DEPRECATED_HANDLER"), *Method),
        Capture.ErrorCode, FString(TEXT("DEPRECATED_HANDLER")));
    Test.TestTrue(*FString::Printf(TEXT("%s steers to %s"), *Method, *SteerSubstring),
        Capture.Message.Contains(SteerSubstring));
}

// Invokes a spawn-style handler with the given payload and asserts it spawned the
// expected actor class: handler is registered, the response is a success, and its
// shared "actorClass" field (emitted by AddActorVerification) equals ExpectedActorClass.
// Shared by misc.create_camera variant tests so the spawn-class contract lives in one
// place rather than being copy-pasted per case.
inline void TestSpawnHandlerProducesActorClass(FAutomationTestBase& Test, const FString& Method,
    const TSharedPtr<FJsonObject>& Payload, const FString& ExpectedActorClass)
{
    FTestResponseCapture Capture;
    Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), *Method),
        InvokeHandlerWithCapture(Method, Payload, Capture));
    Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), *Method), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ActorClass;
        Test.TestTrue(*FString::Printf(TEXT("%s response carries actorClass"), *Method),
            Capture.Result->TryGetStringField(TEXT("actorClass"), ActorClass));
        Test.TestEqual(*FString::Printf(TEXT("%s spawns %s"), *Method, *ExpectedActorClass),
            ActorClass, ExpectedActorClass);
    }
}

// Asserts a create-style handler honors an optional `name` slot as the spawned
// actor's label (board E-environment-build-create-no-name-param contract). Verifies:
// the `name` param is registered/discoverable; a unique requested name (GUID-suffixed
// to dodge SetActorLabel disambiguation) is echoed verbatim in `actorName`; and
// omitting `name` falls back to DefaultLabelPrefix. Both cases assert the create
// SUCCEEDED before reading the echo: the echo assertions sit inside an
// `if (bSuccess && Result.IsValid())` block, so without that assertion a verb that
// errored out would skip every real check and the helper would report green having
// proved nothing. (These verbs need a real editor world; the EditorContext test flag
// guarantees one, so a failure here is a regression, not a host difference.)
// Fails if the `name` slot is dropped (the echo reverts to the hardcoded default).
// Shared by the create_sky_sphere / create_fog_volume regression tests so the contract
// lives in one place rather than being copy-pasted per verb.
inline void TestCreateHandlerHonorsNameSlot(FAutomationTestBase& Test, const FString& Method,
    const FString& DefaultLabelPrefix)
{
    // The `name` slot must be registered so callers can discover it.
    Test.TestNotNull(*FString::Printf(TEXT("%s exposes a name param"), *Method),
        GetRegisteredParamSpec(Method, TEXT("name")));

    // Case 1: a unique requested name lands verbatim as the actor label and is echoed
    // in `actorName` (which carries the LABEL, not the internal object name - the create
    // verbs never set the internal FName). The GUID keeps the label distinct from any
    // other actor's so the assertion is about this verb rather than about a collision.
    {
        const FString RequestedName = FString::Printf(TEXT("PW_%s_%s"),
            *DefaultLabelPrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), RequestedName);

        FTestResponseCapture Capture;
        Test.TestTrue(*FString::Printf(TEXT("%s handler found (named)"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        // Assert the create actually succeeded BEFORE the echo block. Without this the
        // whole `if` body is skipped on any failure and the helper reports green having
        // proved nothing — a verb that dropped the `name` slot and then errored out would
        // sail through. Surface the error text so a genuine failure is diagnosable.
        Test.TestTrue(*FString::Printf(TEXT("%s succeeds with a requested name"), *Method),
            Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            Test.AddError(*FString::Printf(TEXT("%s (named) failed: %s — %s"),
                *Method, *Capture.ErrorCode, *Capture.Message));
        }
        Test.TestTrue(*FString::Printf(TEXT("%s (named) returned a result payload"), *Method),
            Capture.Result.IsValid());
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString EchoedName;
            Test.TestTrue(TEXT("named response carries actorName"),
                Capture.Result->TryGetStringField(TEXT("actorName"), EchoedName));
            Test.TestEqual(TEXT("requested name becomes the actor label"), EchoedName, RequestedName);
        }
    }

    // Case 2: omitting `name` keeps the historical default label (never empty). Match the
    // PREFIX rather than the whole label: the default is derived from the mesh/class name,
    // so repeated runs produce labels that differ only in a trailing disambiguator.
    // NOTE: that disambiguator does NOT come from SetActorLabel. AActor::SetActorLabel
    // stores a label verbatim and does not uniquify (UE 5.8 ActorEditor.cpp:1291) - only
    // FActorLabelUtilities::SetActorLabelUnique appends a suffix (EditorEngine.cpp:6579).
    // An earlier version of this comment asserted the opposite, which is part of why
    // duplicate labels were assumed unreachable; see rpc-design.md §15.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        FTestResponseCapture Capture;
        Test.TestTrue(*FString::Printf(TEXT("%s handler found (unnamed)"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        // Same reason as Case 1: without a success assertion the default-label check is
        // skipped whenever the verb errors, so the helper cannot fail.
        Test.TestTrue(*FString::Printf(TEXT("%s succeeds with no requested name"), *Method),
            Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            Test.AddError(*FString::Printf(TEXT("%s (unnamed) failed: %s — %s"),
                *Method, *Capture.ErrorCode, *Capture.Message));
        }
        Test.TestTrue(*FString::Printf(TEXT("%s (unnamed) returned a result payload"), *Method),
            Capture.Result.IsValid());
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString EchoedName;
            Test.TestTrue(TEXT("unnamed response carries actorName"),
                Capture.Result->TryGetStringField(TEXT("actorName"), EchoedName));
            Test.TestTrue(*FString::Printf(TEXT("default label begins with %s"), *DefaultLabelPrefix),
                EchoedName.StartsWith(DefaultLabelPrefix));
        }
    }
}

// Async-safe variant of InvokeHandlerWithCapture: drives a handler that finishes
// inside an AsyncTask / next-tick lambda. The capture is shared-owned so the
// handler's late completion routes through a weak handle and cannot write through
// a freed capture if the lambda drains after the test scope ends. Returns true if
// the handler was found. Callers keep the returned TSharedRef alive while pumping.
inline bool InvokeHandlerWithSharedCapture(const FString& MethodName,
    const TSharedPtr<FJsonObject>& Payload, const TSharedRef<FTestResponseCapture>& Capture)
{
    Capture->Reset();
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == MethodName)
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                TEXT("test-id"), MethodName, Payload, Capture);
            Reg.Func(Ctx);
            return true;
        }
    }
    return false;
}

// Pumps the game-thread task queue until the shared capture is populated or the
// timeout elapses. Async handlers (landscape.create / asset.set_tags, etc.)
// enqueue their work via AsyncTask(ENamedThreads::GameThread, ...), which lands
// on the named GameThread's *MainQueue* (queue index 0). ProcessThreadUntilIdle
// must be called with ENamedThreads::GameThread (MainQueue), not GameThread_Local
// — the LocalQueue bit selects a separate queue (index 1) that AsyncTask never
// touches, so a _Local pump drains the wrong queue and the lambda only runs after
// RunTest returns (its stack-owned capture already freed → response dropped).
// Shared pump for every InvokeHandlerWithSharedCapture call site.
inline void PumpUntilCaptured(FTestResponseCapture& Capture, double TimeoutSeconds)
{
    const double Start = FPlatformTime::Seconds();
    while (!Capture.bWasCalled)
    {
        if ((FPlatformTime::Seconds() - Start) > TimeoutSeconds)
        {
            break;
        }
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(0.001f);
    }
}

// Host-dependent fixture gate. Some tests load fixture content by absolute
// /Game/ path (the Lyra mannequin AnimBP / Skeleton / Control Rig) that only
// certain host projects ship. When the fixture PACKAGE does not exist in this
// host project, the test passes early with an audit-greppable FIXTURE-SKIP note
// (grep run logs for "FIXTURE-SKIP:"). When the package exists but the
// subsequent LoadObject fails, call sites keep their hard failure — that is a
// real regression signal, not a host difference. Accepts an object path
// ("/Game/Pkg/Name.Name") or a bare package path ("/Game/Pkg/Name"); use inside
// RunTest bodies only (expands PinWrightTestSkip::SkipAssertions + `return true;`).
//
// The skip goes through the shared emitter rather than AddInfo so it carries the
// PINWRIGHT_ASSERTIONS_SKIPPED wire marker: `check_suite_log.py` counts that marker
// and refuses to classify the run COMPLETED_CLEAN. An AddInfo note IS written to the
// automation log (`AutomationControllerManager.cpp:1685-1693` logs Info entries at
// Log verbosity), so FIXTURE-SKIP: stayed greppable -- but it carries no severity and
// no marker, so a host missing this fixture reported green having measured nothing.
// The FIXTURE-SKIP: token is kept in the detail because docs/test-organization.md
// names it as the audit token.
#define PINWRIGHT_SKIP_IF_FIXTURE_MISSING(FixturePath) \
    do \
    { \
        const FString PinWrightFixturePackage = \
            FPackageName::ObjectPathToPackageName(FString(FixturePath)); \
        if (!FPackageName::DoesPackageExist(PinWrightFixturePackage)) \
        { \
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"), FString::Printf( \
                TEXT("FIXTURE-SKIP: %s not present in this host project; test requires Lyra mannequin content."), \
                *PinWrightFixturePackage)); \
            return true; \
        } \
    } while (false)

// Candidate-list variant of PINWRIGHT_SKIP_IF_FIXTURE_MISSING for tests that probe a
// LIST of alternative host fixtures (e.g. any of several Lyra mannequin montages).
// Skips (FIXTURE-SKIP note + early pass) only when NONE of the candidates' packages
// exist in this host project. When at least one package exists, the test proceeds —
// and a subsequent LoadObject failure on an existing package stays a hard failure
// (real regression signal, not a host difference). Accepts a TArray<FString> of
// object paths or bare package paths; use inside RunTest bodies only.
#define PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(CandidatePaths) \
    do \
    { \
        bool bPinWrightAnyFixtureExists = false; \
        for (const FString& PinWrightCandidate : CandidatePaths) \
        { \
            if (FPackageName::DoesPackageExist( \
                    FPackageName::ObjectPathToPackageName(PinWrightCandidate))) \
            { \
                bPinWrightAnyFixtureExists = true; \
                break; \
            } \
        } \
        if (!bPinWrightAnyFixtureExists) \
        { \
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"), FString::Printf( \
                TEXT("FIXTURE-SKIP: none of the %d candidate fixture packages are present in this host project; test requires Lyra mannequin / example content."), \
                (CandidatePaths).Num())); \
            return true; \
        } \
    } while (false)

// Derives the object-path form (PackagePath.AssetName) from a package path, the
// idiom UEditorAssetLibrary / LoadObject expect. Falls back to the bare package
// path when the asset name can't be extracted (matches the convention centralized
// by TestUIHandlers.cpp's ToWidgetObjectPath). Shared so the "%s.%s" object-path
// contract lives in one place instead of being open-coded per test.
inline FString ToObjectPath(const FString& PackagePath)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    return AssetName.IsEmpty()
        ? PackagePath
        : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

inline UEdGraph* FindImplementedInterfaceGraphByName(
    UBlueprint* Blueprint,
    const FString& FunctionName)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        for (UEdGraph* Graph : Interface.Graphs)
        {
            if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
    }
    return nullptr;
}

inline bool HasOrdinaryFunctionGraphByName(
    UBlueprint* Blueprint,
    const FString& FunctionName)
{
    if (!Blueprint)
    {
        return false;
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}

inline bool HasOverrideEventByName(
    UBlueprint* Blueprint,
    const FString& FunctionName)
{
    if (!Blueprint)
    {
        return false;
    }
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
            if (Event
                && Event->bOverrideFunction
                && Event->EventReference.GetMemberName().ToString().Equals(
                    FunctionName,
                    ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
    }
    return false;
}

// Discard a test-created asset by package path (e.g. "/Game/Input/IA_Jump"). Never use
// ObjectTools::ForceDeleteObjects here: its O(live UObject) reference walk plus GC consumed
// 3325 s of a 4102 s suite (81%, 811 calls). DeleteAsset is also forbidden because its fallback cold
// LoadPackage can merge a stale file into a live, incompletely loaded package and crash. Probe
// filenames with TryConvertLongPackageNameToFilename + FileSize; DoesPackageExist logs a warning
// for unmounted names, which becomes a test failure when warnings are elevated to errors.
// Saved/on-disk fixtures must release their package linker before file deletion or a same-path
// save can hit ERROR_SHARING_VIOLATION.
// On UE 5.4, both UEditorAssetLibrary::DeleteAsset and DeleteLoadedAsset route through
// ObjectTools::ForceDeleteObjects. Its GatherObjectReferencersForDeletion archive serializes
// freshly-created, never-reloaded assets and can dereference null for a UHLODLayer with a null
// NoClear HLODBuilderSettings export or for a ULandscapeLayerInfoObject (EXCEPTION_ACCESS_VIOLATION).
// Keep this path independent of that archive on every engine version.
inline void CleanupTestAsset(const FString& PackagePath)
{
    if (PackagePath.IsEmpty())
    {
        return;
    }

    if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
    {
        GShaderCompilingManager->FinishAllCompilation();
    }

    FString PackageName = PackagePath;
    int32 DotIndex = INDEX_NONE;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName.LeftInline(DotIndex);
    }

    FString Filename;
    const bool bFilenameResolved = FPackageName::TryConvertLongPackageNameToFilename(
        PackageName, Filename, FPackageName::GetAssetPackageExtension());
    const bool bFileExists = bFilenameResolved && IFileManager::Get().FileSize(*Filename) >= 0;

    PwTestAssetTeardown::DrainGameThreadBeforeTeardown();

    UObject* InMemory = FindObject<UObject>(nullptr, *ToObjectPath(PackagePath));
    int32 InputDotIndex = INDEX_NONE;
    if (!InMemory && PackagePath.FindChar(TEXT('.'), InputDotIndex))
    {
        InMemory = FindObject<UObject>(nullptr, *PackagePath);
    }

    UPackage* OriginalPackage = InMemory
        ? InMemory->GetOutermost()
        : FindPackage(nullptr, *PackageName);
#if WITH_EDITOR
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (EditorWorld && OriginalPackage == EditorWorld->GetOutermost())
    {
        return;
    }
#endif

    // ResetLoaders must target the source package before DiscardLoadedAssetNoGc renames it into
    // /Transient. The measured post-discard ladder left the package linker attached (linker=1)
    // through every follow-up release, so the renamed package is no longer a valid reset target.
    if (bFileExists && OriginalPackage)
    {
        ResetLoaders(OriginalPackage);
    }

    UPackage* Package = InMemory
        ? PwTestAssetTeardown::DiscardLoadedAssetNoGc(InMemory)
        : OriginalPackage;
    // The in-memory core declines the active editor world (and its package); never fall through
    // to filename conversion or disk deletion when that guard returns nullptr.
    if (InMemory && !Package)
    {
        return;
    }
    if (!InMemory && Package)
    {
        // Prevent the package auto-saver from recreating the file after deletion.
        Package->SetDirtyFlag(false);
    }

    if (!bFileExists)
    {
        return;
    }

    constexpr int32 MaxDeleteAttempts = 40;
    int32 DeleteAttemptCount = 0;
    const auto TryDelete = [&Filename, &DeleteAttemptCount]()
    {
        ++DeleteAttemptCount;
        return IFileManager::Get().Delete(*Filename, /*RequireExists=*/false,
            /*EvenReadOnly=*/true, /*Quiet=*/true);
    };

    bool bDeleted = TryDelete();
    if (!bDeleted)
    {
        for (int32 Attempt = 0; Attempt < MaxDeleteAttempts; ++Attempt)
        {
            bDeleted = TryDelete();
            if (bDeleted)
            {
                break;
            }
            if (Attempt + 1 < MaxDeleteAttempts)
            {
                FPlatformProcess::Sleep(0.05f);
            }
        }
    }
    if (!bDeleted)
    {
        UE_LOG(LogTemp, Log, TEXT("CleanupTestAsset failed to delete fixture file '%s' after %d attempts; a stale fixture file at this fixed path will poison the next run."), *Filename, DeleteAttemptCount);
        return;
    }

    if (!InMemory)
    {
        if (Package)
        {
            FAssetRegistryModule::PackageDeleted(Package);
        }
        else
        {
            IAssetRegistry::GetChecked().ScanModifiedAssetFiles(
                { FPaths::ConvertRelativePathToFull(Filename) });
        }
    }
}

// Resolves a package filename (/Game/.../Name -> ...\Name.uasset) from a handler's
// reported assetPath. AddAssetVerification overwrites assetPath with the asset's package
// path, so it is already in long-package-name form; an object-path suffix (.AssetName),
// if present, is stripped first. Shared by the create-save-writes-to-disk regression
// tests (material / audio / geometry) so the assetPath -> on-disk-.uasset resolver lives
// in one place instead of being copy-pasted per disk-write test.
inline FString PackageFilenameFromAssetPath(const FString& AssetPath)
{
    if (AssetPath.IsEmpty())
    {
        return FString();
    }
    FString PackageName = AssetPath;
    int32 DotIndex = INDEX_NONE;
    if (PackageName.FindChar(TEXT('.'), DotIndex))
    {
        PackageName.LeftInline(DotIndex);
    }
    return FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());
}

// Drives a create-style save handler and asserts the honest persistence contract shared
// by the B-*-save-no-disk-write regression family: with save:true the new .uasset must be
// on disk and the response reports saved:true; with save:false nothing is written and
// saved is false. In both directions the .uasset is FileSize-probed to prove durability,
// then the leftover in-memory package is deleted (save:true) or de-dirtied (save:false)
// so the disposable host is left clean. bExpectDiskFile selects the direction. AssetClass
// is the concrete runtime UClass (e.g. UMaterial::StaticClass() / USoundClass::StaticClass())
// used to locate the created object for teardown. Shared so the save-to-disk contract
// lives in one place instead of a per-verb ~65-line clone. Reverting a save helper to a
// mark-dirty-only body fails the save:true disk-presence (and saved:true) assertions.
inline bool TestCreateHandlerSaveWritesToDisk(FAutomationTestBase& Test, const FString& Method,
    UClass* AssetClass, const FString& NamePrefix, const FString& Folder,
    bool bSave, bool bExpectDiskFile)
{
    const FString AssetName = FString::Printf(
        TEXT("%s_%s"), *NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), bSave);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(*FString::Printf(TEXT("%s handler is registered"), *Method), bFound);
    Test.TestTrue(*FString::Printf(TEXT("%s responded"), *Method), Capture.bWasCalled);
    Test.TestTrue(*FString::Printf(TEXT("%s succeeded"), *Method), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // The honest persistence verdict: saved must match disk presence. Default the probe
    // so an absent field reads as "not saved".
    bool bSavedReported = !bExpectDiskFile;
    Capture.Result->TryGetBoolField(TEXT("saved"), bSavedReported);
    if (bExpectDiskFile)
    {
        Test.TestTrue(TEXT("save:true reports saved:true"), bSavedReported);
    }
    else
    {
        Test.TestFalse(TEXT("save:false reports saved:false"), bSavedReported);
    }

    // Hard disk-presence proof: pre-fix (mark-dirty-only save) no file was ever written,
    // so FileSize < 0 even for save:true.
    FString AssetPath;
    Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
    Test.TestFalse(TEXT("resolved a package filename from assetPath"), PackageFilename.IsEmpty());
    const int64 OnDiskSize = IFileManager::Get().FileSize(*PackageFilename);
    if (bExpectDiskFile)
    {
        Test.TestTrue(TEXT("the .uasset is on disk after save:true"), OnDiskSize > 0);
    }
    else
    {
        Test.TestTrue(TEXT("no .uasset on disk after save:false"), OnDiskSize < 0);
    }

    const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *Folder, *AssetName);
    if (bExpectDiskFile)
    {
        CleanupTestAsset(FullPackagePath);
    }
    else
    {
        // The in-memory package must not be left dirty behind us. Clear it so the
        // disposable host isn't carrying an unsaved package after the test.
        if (UObject* Created = StaticFindObject(AssetClass, nullptr, *ToObjectPath(FullPackagePath)))
        {
            if (UPackage* Pkg = Created->GetOutermost())
            {
                Pkg->SetDirtyFlag(false);
            }
        }
    }
    return true;
}

// Returns the first unbound APostProcessVolume in the editor world, or nullptr.
// The PostProcessVolume.* / lighting.* typed setters target this volume; shared by
// every PPV-echo test so the bUnbound lookup lives in one place.
inline APostProcessVolume* FindUnboundPPV(UWorld* World)
{
    if (!World) return nullptr;
    for (TActorIterator<APostProcessVolume> It(World); It; ++It)
    {
        if (It->bUnbound) return *It;
    }
    return nullptr;
}

// One {field name -> expected applied value} number echo to assert on a setter response.
struct FExpectedNumberEcho
{
    const TCHAR* Field;
    float Expected;
};

// One {field name -> expected applied state} bool echo to assert on a setter response.
struct FExpectedBoolEcho
{
    const TCHAR* Field;
    bool Expected;
};

// Drives a PPV-targeting "update settings" setter (lighting.set_exposure /
// set_ambient_occlusion, post_process.* setters) and asserts it echoes the values it
// just applied inline — the board E-lighting-set-ao-exposure-no-echo contract, so a
// caller never needs a property.get round-trip on FPostProcessSettings to confirm the
// write. Centralizes the world-guard, the bPreExistingPPV spawn/destroy lifecycle (only
// the volume this test spawned is cleaned up), the invoke+success guard, and the
// `TryGetNumberField` + `TestEqual((float)…, expected)` number-echo / `TryGetBoolField`
// bool-echo assertion loop so each setter-echo regression test is a few lines instead of
// a copy-pasted ~55-line block. If a refactor reverts a setter to {success, actorName}
// only, the per-field echo assertions fire.
inline void TestPPVSetterEchoesAppliedValues(FAutomationTestBase& Test, const FString& Method,
    const TSharedPtr<FJsonObject>& Payload,
    std::initializer_list<FExpectedNumberEcho> NumberEchoes,
    std::initializer_list<FExpectedBoolEcho> BoolEchoes = {})
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(Test, TEXT("no-editor-world"),
            FString::Printf(
                TEXT("Editor world not available — skipping %s echo test"), *Method));
        return;
    }

    // Only destroy the PPV if this test spawned it — never clobber a fixture left
    // behind by a prior test or the level.
    const bool bPreExistingPPV = (FindUnboundPPV(World) != nullptr);

    FTestResponseCapture Capture;
    Test.TestTrue(*FString::Printf(TEXT("%s handler found"), *Method),
        InvokeHandlerWithCapture(Method, Payload, Capture));

    APostProcessVolume* PPV = FindUnboundPPV(World);
    ON_SCOPE_EXIT
    {
        if (!bPreExistingPPV && PPV)
        {
            PPV->Destroy();
        }
    };

    Test.TestTrue(*FString::Printf(TEXT("%s succeeds"), *Method), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return;
    }

    // The applied values must be echoed inline — not requiring a property.get.
    for (const FExpectedNumberEcho& Echo : NumberEchoes)
    {
        double Echoed = 0.0;
        Test.TestTrue(*FString::Printf(TEXT("%s response echoes %s"), *Method, Echo.Field),
            Capture.Result->TryGetNumberField(Echo.Field, Echoed));
        Test.TestEqual(*FString::Printf(TEXT("%s echoed %s matches applied value"), *Method, Echo.Field),
            (float)Echoed, Echo.Expected);
    }
    for (const FExpectedBoolEcho& Echo : BoolEchoes)
    {
        bool bEchoed = false;
        Test.TestTrue(*FString::Printf(TEXT("%s response echoes %s"), *Method, Echo.Field),
            Capture.Result->TryGetBoolField(Echo.Field, bEchoed));
        const FString StateMsg = FString::Printf(TEXT("%s echoed %s matches applied state"), *Method, Echo.Field);
        if (Echo.Expected)
        {
            Test.TestTrue(*StateMsg, bEchoed);
        }
        else
        {
            Test.TestFalse(*StateMsg, bEchoed);
        }
    }
}

// Replaces comment bodies and raw-string bodies with spaces, preserving line structure and
// leaving ordinary string literals untouched -- the codes and parameter keys live in those.
//
// Shared by every test that LINTS PLUGIN SOURCE TEXT off disk, because scanning raw bytes makes a
// file's prose part of its behaviour. Both replacements are load-bearing, in opposite directions.
// Comments must go because handler files quote their own call shapes in prose ("reads
// Ctx.GetString(TEXT(\"emitter\"))", "Raw literal, not ErrorCodes::ERR_INVALID_PARAMS: ..."), and a
// quoted shape is then scored as the real thing -- that is board
// B-error-code-adoption-test-scans-comments, where one sentence explaining why a file avoided a
// constant flipped it to "adopting" and failed it for all 40 codes it spells by hand. Raw strings
// must go because R"(...)" bodies routinely contain unbalanced braces and stray quotes (JSON
// samples, IR fixtures); left in, they desync a brace matcher walking the same text.
inline FString NeutralizeSourceText(const FString& In)
{
    const int32 Len = In.Len();
    FString Out;
    Out.Reserve(Len);

    int32 i = 0;
    while (i < Len)
    {
        const TCHAR C = In[i];

        // Line comment.
        if (C == TEXT('/') && i + 1 < Len && In[i + 1] == TEXT('/'))
        {
            while (i < Len && In[i] != TEXT('\n'))
            {
                Out.AppendChar(TEXT(' '));
                ++i;
            }
            continue;
        }

        // Block comment.
        if (C == TEXT('/') && i + 1 < Len && In[i + 1] == TEXT('*'))
        {
            Out.Append(TEXT("  "));
            i += 2;
            while (i < Len && !(In[i] == TEXT('*') && i + 1 < Len && In[i + 1] == TEXT('/')))
            {
                Out.AppendChar(In[i] == TEXT('\n') ? TEXT('\n') : TEXT(' '));
                ++i;
            }
            if (i < Len)
            {
                Out.Append(TEXT("  "));
                i += 2;
            }
            continue;
        }

        // Raw string: R"delim( ... )delim". Emitted as an empty literal plus filler so byte
        // offsets and line counts stay comparable to the input.
        if (C == TEXT('R') && i + 1 < Len && In[i + 1] == TEXT('"'))
        {
            int32 OpenParen = INDEX_NONE;
            for (int32 j = i + 2; j < Len && j <= i + 18; ++j)
            {
                if (In[j] == TEXT('('))
                {
                    OpenParen = j;
                    break;
                }
            }
            if (OpenParen != INDEX_NONE)
            {
                const FString Terminator =
                    FString(TEXT(")")) + In.Mid(i + 2, OpenParen - (i + 2)) + TEXT("\"");
                const int32 Close = In.Find(Terminator, ESearchCase::CaseSensitive,
                    ESearchDir::FromStart, OpenParen + 1);
                if (Close != INDEX_NONE)
                {
                    const int32 End = Close + Terminator.Len();
                    Out.Append(TEXT("\"\""));
                    for (int32 j = i + 2; j < End; ++j)
                    {
                        Out.AppendChar(In[j] == TEXT('\n') ? TEXT('\n') : TEXT(' '));
                    }
                    i = End;
                    continue;
                }
            }
            Out.AppendChar(C);
            ++i;
            continue;
        }

        // Ordinary string / character literal: copied through verbatim.
        if (C == TEXT('"') || C == TEXT('\''))
        {
            const TCHAR Quote = C;
            Out.AppendChar(C);
            ++i;
            while (i < Len)
            {
                if (In[i] == TEXT('\\') && i + 1 < Len)
                {
                    Out.AppendChar(In[i]);
                    Out.AppendChar(In[i + 1]);
                    i += 2;
                    continue;
                }
                Out.AppendChar(In[i]);
                const bool bClosing = (In[i] == Quote);
                ++i;
                if (bClosing)
                {
                    break;
                }
            }
            continue;
        }

        Out.AppendChar(C);
        ++i;
    }
    return Out;
}
