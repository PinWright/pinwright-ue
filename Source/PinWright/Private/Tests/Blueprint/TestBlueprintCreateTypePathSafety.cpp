// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBlueprintCreateTypePathSafety.cpp - regression coverage for the
// BlueprintTypeDefinitionHandler site of B-createpackage-unvalidated-paths-plugin-wide
// (blueprint.create_enum / blueprint.create_struct, both routed through one CreateAssetPackage).
//
// THE HAZARD. CreatePackage (UObjectGlobals.cpp:1094-1096) logs at **Fatal** on a name containing
// "//", and Fatal is not compiled out in any configuration: the call does not fail, the editor
// PROCESS dies with every unsaved package in it. Both verbs here take one whole caller-supplied
// `path` and hand it to CreatePackage.
//
// WHAT THIS FILE ASSERTS, AND WHAT IT IS NOT. Unlike the foliage and landscape members of this
// board ticket, these two verbs were NOT reachable-lethal: `path` goes through ParseAssetPath ->
// NormalizeAssetPath, which returns bIsValid only for a path FPackageName::IsValidLongPackageName
// has already accepted, so a "//" path has always been refused INVALID_ASSET_PATH here. The
// ticket's enumeration classified the site as unguarded because the guard sits two call frames
// above the CreatePackage and nothing at that line said so. The change this file accompanies adds
// the check at the composition point as a backstop. So these assertions LOCK a contract rather
// than discriminate a fix, and that is deliberate: the only way this site becomes lethal again is
// somebody dropping the upstream check, which is exactly what these cases catch.
//
// SINCE THE DISPATCH-BOUNDARY WAVE, the "//" case is refused one layer higher still: `path` is a
// declared param type carrying the "//" rule, so the gate answers INVALID_ARGUMENT and the handler
// never runs. Every other malformed shape still reaches ParseAssetPath and answers
// INVALID_ASSET_PATH. Each case below asserts its own code, so removing EITHER layer goes red.
//
// WHY DRIVING A "//" PATH IS SAFE HERE, ON EVERY BUILD. It is refused above the concatenation
// today by ParseAssetPath, and if that upstream check is ever lost the new backstop inside
// CreateAssetPackage refuses it there instead and the verb answers CREATE_FAILED - so this case
// goes red on the wrong error code with the process intact rather than taking the suite host down.
// CreatePackage is unreachable from these payloads in both directions.
//
// Every refusal path carries a fresh GUID. That USED to be load-bearing: NormalizeAssetPath fell
// back to retrying the leaf segment under /Game, /Engine and /Script and ACCEPTED the rewrite if
// such a package happened to exist, so a fixed leaf name could silently turn a refusal case into a
// success on some host. That fallback has been deleted (AssetUtils.cpp - it returned bIsValid=true
// naming a DIFFERENT package, and it laundered a "//" input into a valid path), so a GUID leaf is
// no longer what makes these cases deterministic. It is kept because it costs nothing and still
// rules out collision with real host content.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace BlueprintCreateTypePathSafetyHelpers
{
    // The control's fixture. Package-path spelling; the handlers append the object suffix
    // themselves through ToObjectPath.
    constexpr const TCHAR* ControlPackagePath = TEXT("/Engine/BasicShapes/Cube");
    constexpr const TCHAR* ControlObjectPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    inline FString UniqueLeaf(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Drives one refusal case and reports the two facts that separate a guarded verb from an
    // unguarded one: it is refused, and it is refused as a PATH error rather than as anything
    // that happened after a package was made.
    //
    // ExpectedCode differs by shape because the refusal is raised at two different layers. A
    // doubled slash is refused by the DISPATCH GATE (INVALID_ARGUMENT), above the handler
    // entirely - `path`-typed params carry the `//` rule, so the verb body never runs. Every
    // other malformed shape reaches ParseAssetPath inside the handler (INVALID_ASSET_PATH).
    // Both are "refused before any package is created"; asserting the exact code per shape is
    // what keeps this test able to tell the two layers apart if either one is removed.
    inline void ExpectPathRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method, const FString& BadPath,
        const TCHAR* Label, const TCHAR* ExpectedCode)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), BadPath);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-blueprint-create-type-path-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("%s: a %s path ('%s') is refused"), Method, Label,
            *BadPath), bSuccess);
        // Either expected code IS the "nothing was created" assertion: INVALID_ARGUMENT is raised
        // by the dispatch gate before the handler is entered, and INVALID_ASSET_PATH by
        // ParseAssetPath before the create helpers are called. A readback with
        // UEditorAssetLibrary::DoesAssetExist on the malformed path is deliberately NOT done: the
        // engine logs `DoesPackageExist called on PackageName that will always return false` at
        // Warning for such input (PackageName.cpp:2415), and a UE_LOG warning raised inside a
        // running automation test is elevated to an error whenever
        // UAutomationControllerSettings::bElevateLogWarningsToErrors is set, which it is by
        // default - so the readback would fail the test it is meant to strengthen.
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a %s path is refused as a path error, before any package is created"),
            Method, Label), ErrorCode, FString(ExpectedCode));
    }

    // The control. Driven with a path that names an EXISTING asset, so a verb that got past the
    // path check stops at the already-exists branch instead of creating and saving anything.
    inline void ExpectControlReachesExistenceCheck(FAutomationTestBase& Test,
        FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), ControlPackagePath);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-blueprint-create-type-path-control"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("%s: the control still fails - the asset is there"),
            Method), bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a well-formed path passes the path check and reaches the existence check"),
            Method), ErrorCode, FString(TEXT("ALREADY_EXISTS")));
    }

    // Shared by both tests: the control is only meaningful, and only safe, while the fixture is
    // present. Without it a well-formed path would run on into a real create inside Engine content.
    inline bool ControlFixtureIsPresent(FAutomationTestBase& Test)
    {
        if (UEditorAssetLibrary::DoesAssetExist(ControlObjectPath))
        {
            return true;
        }
        PinWrightTestSkip::SkipAssertions(Test, TEXT("basic-shapes-cube-fixture-absent"),
            FString::Printf(TEXT("DoesAssetExist('%s') is false, so the control would create an "
                                 "asset instead of being answered ALREADY_EXISTS"),
                ControlObjectPath));
        return false;
    }
}

// ============================================================================
// blueprint.create_enum refuses a malformed path before a package can be created
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateEnumPathCheckedBeforePackageCreationTest,
    "PinWright.blueprint.create_enum.PathIsCheckedBeforePackageCreation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateEnumPathCheckedBeforePackageCreationTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintCreateTypePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // The member of the class that killed a shared editor on foliage.add_type.
    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_enum"),
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(TEXT("E_DoubleSlash"))),
        TEXT("double slash"), TEXT("INVALID_ARGUMENT"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this is the case that proves the composed path is checked
    // against the engine's package rules and not merely against object-name rules.
    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_enum"),
        FString::Printf(TEXT("/Game/PinWrightTests/%s\\Sub"), *UniqueLeaf(TEXT("E_Backslash"))),
        TEXT("backslash"), TEXT("INVALID_ASSET_PATH"));

    // An unmounted root. Left unchecked this is the shape that composes a double slash at verbs
    // that prepend "/Game/" to anything IsValidMountPoint turns away.
    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_enum"),
        FString::Printf(TEXT("/PinWrightNotAMountPoint/%s"), *UniqueLeaf(TEXT("E_Unmounted"))),
        TEXT("unmounted root"), TEXT("INVALID_ASSET_PATH"));

    // CONTROL. Without this, a handler that refused every path would satisfy every case above.
    if (ControlFixtureIsPresent(*this))
    {
        ExpectControlReachesExistenceCheck(*this, Dispatcher, Sink, TEXT("blueprint.create_enum"));
    }

    return true;
}

// ============================================================================
// blueprint.create_struct shares CreateAssetPackage, so it must refuse the same paths
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCreateStructPathCheckedBeforePackageCreationTest,
    "PinWright.blueprint.create_struct.PathIsCheckedBeforePackageCreation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCreateStructPathCheckedBeforePackageCreationTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintCreateTypePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_struct"),
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(TEXT("S_DoubleSlash"))),
        TEXT("double slash"), TEXT("INVALID_ARGUMENT"));

    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_struct"),
        FString::Printf(TEXT("/Game/PinWrightTests/%s\\Sub"), *UniqueLeaf(TEXT("S_Backslash"))),
        TEXT("backslash"), TEXT("INVALID_ASSET_PATH"));

    ExpectPathRefused(*this, Dispatcher, Sink, TEXT("blueprint.create_struct"),
        FString::Printf(TEXT("/PinWrightNotAMountPoint/%s"), *UniqueLeaf(TEXT("S_Unmounted"))),
        TEXT("unmounted root"), TEXT("INVALID_ASSET_PATH"));

    if (ControlFixtureIsPresent(*this))
    {
        ExpectControlReachesExistenceCheck(*this, Dispatcher, Sink, TEXT("blueprint.create_struct"));
    }

    return true;
}
