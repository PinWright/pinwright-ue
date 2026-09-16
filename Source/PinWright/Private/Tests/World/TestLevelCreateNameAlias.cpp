// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-level-create-name-path-alias.
//
// level.create and level.structure.create_level named the new level's leaf/short-
// name slot `levelName` (RPC_PARAM_OPT / RPC_PARAM_REQ) with NO aliases, so a
// caller who reached for the obvious generic `name` (the spelling task briefs and
// many sibling create RPCs use) hard-failed at the wire level: the spec carried no
// alias, so ValidateHandlerParams rejected the payload as UNKNOWN_PARAMS (or, for
// the required slot, MISSING_REQUIRED_PARAM) before the body ran.
//
// The fix annotates each create verb's `levelName` slot via LevelNameParamUtils
// (built on the generic ParamAliasUtils) so `name` is an accepted alias of
// `levelName`, and the body reads the value via GetStringFirstOf({levelName, name})
// so the alias resolves end-to-end. Per the ticket's reword, only the `name`
// synonym is aliased — the destination-path slot (`levelPath`) is left unaliased
// because on level.create it is a full package path, conflicting with the settled
// `path`=folder create-verb convention.
//
// Two layers of coverage, mirroring TestGeometryCreateNameParamAlias:
//  - Static registration check: each create verb's `levelName` spec carries the
//    `name` alias and `levelPath` does NOT carry a `path`/`assetPath` alias.
//    Exercises the production FParamSpec set.
//  - End-to-end dispatch: route `{name: ""}` through the real dispatcher for the
//    required-slot verb (level.structure.create_level) and assert it is NOT
//    rejected with UNKNOWN_PARAMS or MISSING_REQUIRED_PARAM — i.e. the alias
//    satisfied both the known-params set and the required-param check, reaching the
//    body, which then rejects the blank value with INVALID_ARGUMENT. This proves
//    alias resolution without creating any level asset.
// Counterfactual: reverting the alias makes the dispatcher reject `{name:...}` with
// UNKNOWN_PARAMS / MISSING_REQUIRED_PARAM and the static check fails (Aliases is empty).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/TestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // The level create verbs whose `levelName` slot now accepts the generic `name`
    // spelling as an alias.
    const TArray<FString>& LevelCreateVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("level.create"),
            TEXT("level.structure.create_level")
        };
        return Verbs;
    }
}

// 1. Static: each level create verb registers its `levelName` slot with the `name`
//    alias, and the `levelPath` slot is left WITHOUT a `path`/`assetPath` alias
//    (the ticket's deliberate scope — `levelPath` is a full package path, not the
//    folder slot the material create convention reserves `path` for).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelCreateVerbsDeclareNameAliasTest,
    "PinWright.level.aliases.CreateVerbsDeclareNameAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelCreateVerbsDeclareNameAliasTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : LevelCreateVerbs())
    {
        const FParamSpec* NameSpec = GetRegisteredParamSpec(Verb, TEXT("levelName"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares a 'levelName' param"), *Verb), NameSpec))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s 'levelName' carries the 'name' alias"), *Verb),
            NameSpec->Aliases.Contains(TEXT("name")));

        // levelPath must NOT pick up a path/assetPath alias (would conflict with the
        // create-verb `path`=folder convention).
        const FParamSpec* PathSpec = GetRegisteredParamSpec(Verb, TEXT("levelPath"));
        if (PathSpec)
        {
            TestFalse(*FString::Printf(TEXT("%s 'levelPath' does NOT alias 'path'"), *Verb),
                PathSpec->Aliases.Contains(TEXT("path")));
            TestFalse(*FString::Printf(TEXT("%s 'levelPath' does NOT alias 'assetPath'"), *Verb),
                PathSpec->Aliases.Contains(TEXT("assetPath")));
        }
    }
    return true;
}

// 2. End-to-end: level.structure.create_level (the required-slot verb) accepts the
//    `name` alias at the wire level. Passing `{name: ""}` must pass both the
//    known-params set (no UNKNOWN_PARAMS) and the required-param check (no
//    MISSING_REQUIRED_PARAM), reaching the body, which then rejects the blank value
//    with INVALID_ARGUMENT. No level asset is created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelCreateAcceptsNameAliasOnWireTest,
    "PinWright.level.aliases.CreateAcceptsNameAliasOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelCreateAcceptsNameAliasOnWireTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Use the generic `name` spelling on a create verb whose canonical slot is the
    // REQUIRED `levelName`. A blank value reaches the body without creating anything.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TEXT(""));

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("level.structure.create_level"),
        TEXT("req-level-name-alias"), Params, bSuccess, ErrorCode);

    // The alias must satisfy the known-params set: UNKNOWN_PARAMS may not appear.
    TestNotEqual(TEXT("create_level does not reject 'name' as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    // The alias must satisfy the required-param check: MISSING_REQUIRED_PARAM may not appear.
    TestNotEqual(TEXT("create_level does not reject 'name' as MISSING_REQUIRED_PARAM"),
        ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    // The body ran and rejected the blank alias value with the level-specific error,
    // proving the alias resolved end-to-end into the body's LevelName read.
    TestEqual(TEXT("create_level body rejects the blank 'name' value with INVALID_ARGUMENT"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestFalse(TEXT("create_level with a blank 'name' does not succeed"), bSuccess);

    return true;
}
