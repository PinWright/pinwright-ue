// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-create-physics-asset-skeletonpath-alias-dead.
// skeleton.create_physics_asset's summary/wiki advertise that the target may be
// supplied under a `skeletonPath` alias, and the handler body reads it via
// GetStringFirstOf({skeletalMeshPath, skeletonPath}). But the required slot was
// registered as a bare RPC_PARAM_REQ("skeletalMeshPath") with no schema alias, so
// the dispatcher's ValidateHandlerParams rejected a skeletonPath-only payload with
// MISSING_REQUIRED_PARAM *before* the body ran — the documented alias was dead.
// The fix registers the slot via ParamAliasUtils::MakeAliasParamSpec with
// `skeletonPath` as an alias of the required `skeletalMeshPath`.
//
// Two layers of coverage:
//  - Static registration: the registered skeletalMeshPath spec is required and
//    carries the `skeletonPath` alias. Exercises the production
//    FHandlerRegistration/FParamSpec set (the REGISTER_RPC_HANDLER output).
//  - Dispatcher end-to-end: route a skeletonPath-only payload through the real
//    FRpcDispatcher::ProcessRequest (which runs ValidateHandlerParams — the exact
//    gate the bug lives at, and which InvokeHandler bypasses). The request must NOT
//    be rejected with MISSING_REQUIRED_PARAM; it reaches the handler body, which
//    returns MESH_NOT_FOUND on the deliberately bogus probe path. This proves the
//    alias is honored at the wire level, not just present in the registry.
// Counterfactual: reverting to the bare RPC_PARAM_REQ drops the alias, so (1) the
//   static Aliases.Contains check fails and (2) the dispatcher rejects the payload
//   with MISSING_REQUIRED_PARAM, failing both dispatcher assertions.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dispatch/RpcDispatcher.h"
#include "Misc/Guid.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using ParamSpecTestHelpers::FindParamSpec;
using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// 1. Static: skeleton.create_physics_asset registers `skeletalMeshPath` as a required
//    param carrying the `skeletonPath` alias (not a bare RPC_PARAM_REQ with no aliases).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreatePhysicsAssetDeclaresSkeletonPathAliasTest,
    "PinWright.skeleton.aliases.CreatePhysicsAssetDeclaresSkeletonPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreatePhysicsAssetDeclaresSkeletonPathAliasTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = FindParamSpec(TEXT("skeleton.create_physics_asset"), TEXT("skeletalMeshPath"));
    if (!TestNotNull(TEXT("skeleton.create_physics_asset declares a canonical skeletalMeshPath param"), Spec))
    {
        return false;
    }

    TestTrue(TEXT("skeletalMeshPath is required"), Spec->bRequired);
    TestTrue(TEXT("skeletalMeshPath carries the documented 'skeletonPath' schema alias"),
        Spec->Aliases.Contains(TEXT("skeletonPath")));

    return true;
}

// 2. Dispatcher end-to-end: a skeletonPath-only payload must pass ValidateHandlerParams
//    (NOT rejected MISSING_REQUIRED_PARAM) and reach the handler body, which returns
//    MESH_NOT_FOUND for the bogus probe path. Routes through the real dispatcher so it
//    exercises the required-param gate the bug lived at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreatePhysicsAssetDispatcherAcceptsSkeletonPathTest,
    "PinWright.skeleton.aliases.CreatePhysicsAssetDispatcherAcceptsSkeletonPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreatePhysicsAssetDispatcherAcceptsSkeletonPathTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // A deliberately non-existent skeleton path: the required-param gate must pass on
    // the alias, then the body's not-found branch fires. GUID-suffixed so it can never
    // collide with a real asset in the disposable host.
    const FString BogusSkeletonPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/DoesNotExist_SkeletonAliasProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("skeletonPath"), BogusSkeletonPath);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("skeleton.create_physics_asset"),
        TEXT("req-skeletonpath-alias"), Params, bSuccess, ErrorCode);

    // The alias passed schema validation (counterfactual: the bare RPC_PARAM_REQ would
    // reject here with MISSING_REQUIRED_PARAM before the body runs).
    TestNotEqual(TEXT("skeletonPath-only payload is NOT rejected as a missing required param"),
        ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    // ...and reached the handler body, which fails to load the bogus path with MESH_NOT_FOUND,
    // proving the alias resolved end-to-end into the body's GetStringFirstOf.
    TestEqual(TEXT("payload reaches the body, which reports MESH_NOT_FOUND on the bogus path"),
        ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    TestFalse(TEXT("the bogus-path call does not fake-succeed"), bSuccess);

    return true;
}
