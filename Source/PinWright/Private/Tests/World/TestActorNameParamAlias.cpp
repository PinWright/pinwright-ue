// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-actor-verbs-reject-actorpath-slot and
// E-effect-actor-name-slot-vs-actorname (which extended the same migration to the
// actor.* transform/readback verbs actor.get_transform / set_transform /
// get_bounding_box and actor.get).
// The actor.* reader verbs (actor.get_components, actor.describe, the transform verbs,
// actor.get) declared their target slot as the bare canonical `actorName` with NO
// aliases, so a caller who reused the `actorPath` key that actor.spawn / actor.duplicate
// return (or the path-shaped `objectPath`) hard-failed MISSING_REQUIRED_PARAM 'actorName'
// at the wire level —
// the spec carried no aliases, so ValidateHandlerParams rejected the payload before the
// body's resolver ran. The fix annotates the slot via ActorNameParamUtils so
// `objectPath` / `actorPath` / `actor_name` are accepted aliases of `actorName`, and the
// body reads the value via GetStringFirstOf(ActorNameKeys()) so the alias resolves
// end-to-end (FindActorByName already matches a label, internal name, OR object path).
//
// Two layers of coverage, mirroring TestAssetPathParamAlias:
//  - Static registration check: each reader verb's required actorName spec carries the
//    `actorPath` and `objectPath` aliases. Exercises the production FParamSpec set.
//  - End-to-end dispatch: route `{actorPath: <missing actor>}` through the real dispatcher
//    and assert it is NOT rejected with MISSING_REQUIRED_PARAM / UNKNOWN_PARAMS — i.e. the
//    alias passed ValidateHandlerParams and the body read the value, yielding a domain
//    outcome (ACTOR_NOT_FOUND) instead of the param error.
// Counterfactual: reverting the alias makes the dispatcher reject `{actorPath:...}` with
// MISSING_REQUIRED_PARAM 'actorName' and the static check fails (Aliases is empty).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // The actor.* reader/consumer verbs an agent naturally chains after a spawn — the
    // repro path. The transform verbs and actor.get were migrated to the same
    // ActorNameParamUtils alias set by E-effect-actor-name-slot-vs-actorname, so they
    // share this coverage list.
    const TArray<FString>& ActorReaderVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("actor.get_components"),
            TEXT("actor.describe"),
            TEXT("actor.get_transform"),
            TEXT("actor.set_transform"),
            TEXT("actor.get_bounding_box"),
            TEXT("actor.get")
        };
        return Verbs;
    }

    const FParamSpec* FindParamSpec(const FString& Method, const FString& ParamName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (!Reg.MethodName.Equals(Method))
            {
                continue;
            }
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.Name.Equals(ParamName))
                {
                    return &Spec;
                }
            }
        }
        return nullptr;
    }
}

// 1. Static: each actor.* reader verb registers its actorName slot with the actorPath +
//    objectPath aliases (the keys their own producers / sibling namespaces emit).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorReaderVerbsDeclareActorPathAliasTest,
    "PinWright.actor.aliases.ReaderVerbsDeclareActorPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorReaderVerbsDeclareActorPathAliasTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : ActorReaderVerbs())
    {
        const FParamSpec* Spec = FindParamSpec(Verb, TEXT("actorName"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares an actorName param"), *Verb), Spec))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s actorName is required"), *Verb), Spec->bRequired);
        TestTrue(*FString::Printf(TEXT("%s actorName carries the 'actorPath' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("actorPath")));
        TestTrue(*FString::Printf(TEXT("%s actorName carries the 'objectPath' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("objectPath")));
    }
    return true;
}

// 2. End-to-end: the actor.* reader verbs accept `{actorPath:...}` at the wire level —
//    the alias passes ValidateHandlerParams so the request is never rejected as
//    MISSING_REQUIRED_PARAM (it reaches the body, which reports the domain outcome).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorReaderVerbsAcceptActorPathAliasOnWireTest,
    "PinWright.actor.aliases.ReaderVerbsAcceptActorPathAliasOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorReaderVerbsAcceptActorPathAliasOnWireTest::RunTest(const FString& Parameters)
{
    // A value that resolves to no actor and no asset: the body returns a domain result
    // (ACTOR_NOT_FOUND), never the param error — that is the discriminating signal the alias
    // was honored. actor.get_components only probes UEditorAssetLibrary::LoadAsset for a
    // BP CDO when DoesAssetExist reports the path, so a guaranteed-absent name never reaches
    // LoadAsset and emits no editor-level "LoadAsset failed" error log (which the automation
    // framework scores as a failure). The path carries a fresh GUID so no residual asset or
    // stale asset-registry entry on a mutated fuzzing host can ever make DoesAssetExist true
    // for it — keeping the test deterministic regardless of host content state.
    const FString MissingActor = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/DoesNotExist_ActorPathAlias_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    for (const FString& Verb : ActorReaderVerbs())
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorPath"), MissingActor);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, Verb,
            FString::Printf(TEXT("req-%s-actorpath-alias"), *Verb), Params, bSuccess, ErrorCode);

        // The alias must satisfy the required-param check and the known-params set: neither
        // the missing-param error nor the unknown-param error may appear.
        TestNotEqual(*FString::Printf(TEXT("%s does not reject 'actorPath' as MISSING_REQUIRED_PARAM"), *Verb),
            ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
        TestNotEqual(*FString::Printf(TEXT("%s does not reject 'actorPath' as UNKNOWN_PARAMS"), *Verb),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    return true;
}
