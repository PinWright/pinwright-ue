// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-asset-path-vs-assetpath-list-drift.
// The asset.* read-back verbs require `assetPath` while asset.list declares `path`;
// a caller who reuses `path` from asset.list hard-failed MISSING_REQUIRED_PARAM
// 'assetPath'. The fix annotates each read-back verb's `assetPath` spec with a `path`
// alias (AssetPathParamUtils) and reads the value via GetStringFirstOf so it resolves
// end-to-end.
//
// Two layers of coverage:
//  - Static registration check: every read-back verb's required assetPath spec carries
//    the `path` alias. This exercises the production FHandlerRegistration/FParamSpec set
//    and covers the async asset.validate verb without relying on its AsyncTask completing.
//  - End-to-end dispatch: route `{path: <missing asset>}` through the real dispatcher for
//    the synchronous verbs and assert it is NOT rejected with MISSING_REQUIRED_PARAM —
//    i.e. the alias passed ValidateHandlerParams and the body read the value, yielding a
//    domain outcome (exists=false / ASSET_NOT_FOUND) instead of the param error.
// Counterfactual: reverting the alias makes the dispatcher reject `{path:...}` with
// MISSING_REQUIRED_PARAM 'assetPath' and the static check fails (Aliases is empty).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using ParamSpecTestHelpers::FindParamSpec;

namespace
{
    // The read-back verbs that share the assetPath slot taught as `path` by asset.list.
    const TArray<FString>& AssetReadbackVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("asset.exists"),
            TEXT("asset.get"),
            TEXT("asset.validate"),
            TEXT("asset.dump")
        };
        return Verbs;
    }
}

// 1. Static: every asset.* read-back verb registers its assetPath slot with a `path` alias.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReadbackVerbsDeclarePathAliasTest,
    "PinWright.asset.aliases.ReadbackVerbsDeclarePathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetReadbackVerbsDeclarePathAliasTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : AssetReadbackVerbs())
    {
        const FParamSpec* Spec = FindParamSpec(Verb, TEXT("assetPath"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares an assetPath param"), *Verb), Spec))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s assetPath is required"), *Verb), Spec->bRequired);
        TestTrue(*FString::Printf(TEXT("%s assetPath carries the 'path' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("path")));
    }
    return true;
}

// 2. End-to-end: the synchronous read-back verbs accept `{path:...}` at the wire level —
//    the alias passes ValidateHandlerParams so the request is never rejected as
//    MISSING_REQUIRED_PARAM (it reaches the body, which reports the domain outcome).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReadbackVerbsAcceptPathAliasOnWireTest,
    "PinWright.asset.aliases.ReadbackVerbsAcceptPathAliasOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetReadbackVerbsAcceptPathAliasOnWireTest::RunTest(const FString& Parameters)
{
    // A path that does not resolve to a real asset: the body returns a domain result
    // (exists=false for asset.exists, ASSET_NOT_FOUND for asset.get/asset.dump), never
    // the param error — that is the discriminating signal the alias was honored.
    const FString MissingPath = TEXT("/Game/__PW_GatewayTests/DoesNotExist_AssetPathAlias");

    // asset.validate is async (AsyncTask to the game thread) and its captured response
    // may not settle synchronously in the test harness, so it is covered by test 1 only.
    const TArray<FString> SyncVerbs = {
        TEXT("asset.exists"),
        TEXT("asset.get"),
        TEXT("asset.dump")
    };

    for (const FString& Verb : SyncVerbs)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), MissingPath);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, Verb,
            FString::Printf(TEXT("req-%s-path-alias"), *Verb), Params, bSuccess, ErrorCode);

        // The alias must satisfy the required-param check: the param error must not appear.
        TestNotEqual(*FString::Printf(TEXT("%s does not reject 'path' as MISSING_REQUIRED_PARAM"), *Verb),
            ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
        TestNotEqual(*FString::Printf(TEXT("%s does not reject 'path' as UNKNOWN_PARAMS"), *Verb),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    return true;
}
