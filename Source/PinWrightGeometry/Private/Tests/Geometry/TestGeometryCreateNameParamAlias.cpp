// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-geometry-create-name-vs-actorname.
//
// The whole geometry.create_* family (create_box / create_sphere / ... /
// create_procedural_mesh) declared the new-actor slot as the bare canonical
// `name` (RPC_PARAM_OPT("name", ...)) with NO aliases, while every verb that
// subsequently operates on that same actor requires `actorName`
// (geometry.get_mesh_info, append_triangle, recalculate_normals, ...). So
// within one procedural-mesh build the caller had to spell the same actor's
// name two different ways: `name` to create it, then `actorName` for every
// following call. A caller who reused the `actorName` key on a create verb
// hard-failed UNKNOWN_PARAMS at the wire level — the spec carried no alias, so
// ValidateHandlerParams rejected the payload before the body ran.
//
// The fix annotates each create verb's `name` slot via GeometryNameParamUtils
// (built on the generic ParamAliasUtils) so `actorName` is an accepted alias of
// `name`, and the body reads the value via GetStringFirstOf({name, actorName})
// so the alias resolves end-to-end into the spawned actor's label.
//
// Two layers of coverage, mirroring TestActorNameParamAlias / TestAssetPathParamAlias:
//  - Static registration check: each create verb's optional `name` spec carries
//    the `actorName` alias. Exercises the production FParamSpec set.
//  - End-to-end dispatch: route `{actorName: <label>}` through the real
//    dispatcher and assert it is NOT rejected with UNKNOWN_PARAMS — i.e. the
//    alias passed ValidateHandlerParams and the body read the value, spawning an
//    actor whose label is the alias-supplied name.
// Counterfactual: reverting the alias makes the dispatcher reject `{actorName:...}`
// with UNKNOWN_PARAMS and the static check fails (Aliases is empty).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;
// Use the shared param-spec lookup instead of a file-local anonymous-namespace copy: an
// identical anon-namespace FindParamSpec collides under unity (ODR/C2668) with the sibling
// param-alias tests that `using ParamSpecTestHelpers::FindParamSpec;` whenever the blob
// partition shifts. Matches TestAssetPathParamAlias / TestCreatePhysicsAssetSkeletonPathAlias.
using ParamSpecTestHelpers::FindParamSpec;

namespace
{
    // Every geometry.create_* verb — the create family whose `name` slot now
    // accepts the operate-verb `actorName` spelling as an alias.
    const TArray<FString>& GeometryCreateVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("geometry.create_box"),
            TEXT("geometry.create_sphere"),
            TEXT("geometry.create_cylinder"),
            TEXT("geometry.create_cone"),
            TEXT("geometry.create_capsule"),
            TEXT("geometry.create_torus"),
            TEXT("geometry.create_plane"),
            TEXT("geometry.create_disc"),
            TEXT("geometry.create_stairs"),
            TEXT("geometry.create_spiral_stairs"),
            TEXT("geometry.create_ring"),
            TEXT("geometry.create_arch"),
            TEXT("geometry.create_pipe"),
            TEXT("geometry.create_ramp"),
            TEXT("geometry.revolve"),
            TEXT("geometry.create_procedural_mesh")
        };
        return Verbs;
    }
}

// 1. Static: each geometry.create_* verb registers its `name` slot with the
//    `actorName` alias (the key its own operate-verb siblings demand).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateVerbsDeclareActorNameAliasTest,
    "PinWright.geometry.aliases.CreateVerbsDeclareActorNameAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateVerbsDeclareActorNameAliasTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : GeometryCreateVerbs())
    {
        const FParamSpec* Spec = FindParamSpec(Verb, TEXT("name"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares a 'name' param"), *Verb), Spec))
        {
            continue;
        }
        TestFalse(*FString::Printf(TEXT("%s 'name' is optional"), *Verb), Spec->bRequired);
        TestTrue(*FString::Printf(TEXT("%s 'name' carries the 'actorName' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("actorName")));
    }
    return true;
}

// 2. End-to-end: a geometry.create_* verb accepts `{actorName:...}` at the wire
//    level — the alias passes ValidateHandlerParams so the request is never
//    rejected as UNKNOWN_PARAMS, and the body reads the alias value into the
//    spawned actor's label.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateVerbAcceptsActorNameAliasOnWireTest,
    "PinWright.geometry.aliases.CreateVerbAcceptsActorNameAliasOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateVerbAcceptsActorNameAliasOnWireTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry create dispatch test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_GeomAliasProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Use the spelling every operate verb demands (`actorName`) on a create verb —
    // the repro key. create_procedural_mesh is the seed verb from the ticket.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_procedural_mesh"),
        TEXT("req-geom-actorname-alias"), Params, bSuccess, Result, ErrorCode);

    // The alias must satisfy the known-params set: the unknown-param error may not appear.
    TestNotEqual(TEXT("create_procedural_mesh does not reject 'actorName' as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    // It must reach the body and spawn — and the alias value must become the actor label.
    TestTrue(TEXT("create_procedural_mesh succeeds with the 'actorName' alias"), bSuccess);
    if (bSuccess && Result.IsValid())
    {
        TestEqual(TEXT("spawned actor label resolves from the 'actorName' alias value"),
            Result->GetStringField(TEXT("name")), Label);
    }

    DestroyActorsWithLabel(Label);
    return true;
}
