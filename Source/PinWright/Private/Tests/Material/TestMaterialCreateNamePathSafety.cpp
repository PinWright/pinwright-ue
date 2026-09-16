// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestMaterialCreateNamePathSafety.cpp - regression coverage for the material/texture cluster of
// B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. Twelve direct CreatePackage sites across MaterialAuthoringHandler.cpp,
// MaterialParameterCollectionHandler.cpp and TextureHandler.cpp composed "<folder>/<caller name>"
// and handed the result to CreatePackage with no engine validation. CreatePackage
// (UObjectGlobals.cpp:1087-1120) logs at **Fatal** - a verbosity that is not compiled out in any
// configuration - for a name containing "//" (:1094-1096) and for one that resolves to empty
// (:1118). So `name: "a//b"` did not fail the call: the editor PROCESS died, taking every unsaved
// package in it, in editors PinWright shares between agents. Each handler's own `if (!Package)`
// could never fire, because nothing after CreatePackage was reached.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is not a failure signal, it is an absence of one
// (the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on the
// post-fix CONTRACT - the refusal - and the fixture is built so a build WITHOUT the fix takes a
// different, harmless path instead of the fatal one:
//
//   every refusal case pairs its bad `name` with a `parentMaterial` that is a well-formed long
//   package name naming NO asset (a fresh GUID). The pre-fix handler validated parentMaterial and
//   nothing else, so on a reverted build the call is refused ASSET_NOT_FOUND at
//   LoadObject<UMaterial> - which sits ABOVE the concatenation - and the TestEqual on
//   INVALID_ARGUMENT below goes red while the process lives. CreatePackage is unreachable on both
//   the fixed and the reverted build.
//
// That property depends on the fix keeping its name/path check ABOVE the parentMaterial
// resolution, and both the handler and MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath
// carry comments saying so. Do NOT "improve" these cases by supplying a parent that exists, and do
// NOT rewrite them into a crash expectation: either change hands a live editor a string that ends
// it.
//
// AND THE parentMaterial FIXTURE ITSELF MUST STAY WELL-FORMED, for a reason that is not obvious.
// CreatePackage is not the only door to the Fatal: StaticLoadObjectInternal calls
// ResolveName2(..., Create=true) (UObjectGlobals.cpp:1427), which calls CreatePackage on the
// partial package name (:1310). So the LoadObject<UMaterial> that this fixture relies on to bail
// would ITSELF kill the host if handed a "//"-bearing or dot-bearing path. MatSafetyAbsentParentPath
// therefore composes a mounted, single-slashed, hex-only path. Never point it at a malformed one.
//
// THE FOLDER IS A SECOND ONE-ARGUMENT KILL, and the ticket's per-site notes do not say so: a
// perfectly bare `name` with path:"/Game//Materials" composes the same fatal string. Nothing in
// this cluster collapses an interior double slash - the material verbs run no folder sanitiser at
// all (TextureHandler's sites route through the shared NormalizeContentAssetPath, which does
// collapse one, but nothing here calls it). What covers it is that the guard validates the COMPOSED
// path with FPackageName::IsValidLongPackageName, not just the bare name; ExpectInstanceFolderRefused
// pins that, and a name-only character filter would fail it.
//
// WHY create_material_instance IS THE VERB DRIVEN, AND WHAT THAT LEAVES UNPROVEN. It is the only
// verb in this cluster with a second required argument whose resolution is provably above the
// concatenation, so it is the only one whose reverted build can be shown not to reach the Fatal.
// The other eleven sites take a name and a folder and nothing else - for them the caller's path IS
// the only meaningful input, and there is no argument that bails first - so driving a bad name at
// them would be a coin flip on the suite host, not a test. They are covered structurally instead:
//   * the seven other material.authoring.create_* verbs share ONE entry point with this one
//     (MaterialCreatePathParamUtils::ResolveCreateAssetPackagePath), so the refusal asserted here
//     is the same code path they all run;
//   * add_landscape_layer, create_parameter_collection and the three TextureHandler sites call
//     PinWrightComposeAssetPackagePath directly, whose own two engine rules are already asserted
//     by TestFoliageAddTypeNamePathSafety / TestLandscapeCreateGrassTypeNamePathSafety.
// A behavioural test for those would need a second pre-concatenation bail added to each verb,
// which is a bigger change than the guard; it is deliberately not attempted here.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace MaterialCreateNamePathSafetyHelpers
{
    // The verb's default destination, which every refusal case must leave untouched.
    constexpr const TCHAR* MatSafetyDefaultFolder = TEXT("/Game/Materials");

    // A folder that is NOT the default, so the positive control cannot pass by accident.
    constexpr const TCHAR* MatSafetyScratchFolder =
        TEXT("/Game/PinWrightTests/MaterialCreateNameSafety");

    inline FString MatSafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWMatSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee described in the file header: it makes the PRE-FIX handler bail at its
    // parentMaterial load, above the concatenation, instead of reaching CreatePackage. It must
    // stay mounted, single-slashed and dot-free (EGuidFormats::Digits is hex only) because the
    // LoadObject it feeds reaches CreatePackage itself through ResolveName2.
    inline FString MatSafetyAbsentParentPath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/M_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString MatSafetyPackagePath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *Name);
    }

    inline FString MatSafetyObjectPath(const FString& PackagePath, const FString& Name)
    {
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
    }

    // Drives one refusal case and reports on all three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather
    // than as a missing parent material, and the message names the offending value.
    inline void ExpectInstanceNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadName, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("parentMaterial"), MatSafetyAbsentParentPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("material.authoring.create_material_instance"),
            TEXT("req-mat-create-instance-name-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("a %s name ('%s') is refused"), Label, *BadName),
            bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // ASSET_NOT_FOUND from the parentMaterial load, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s name is refused as a caller argument error, not as a missing parent"),
            Label), ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name or composed path"), Label),
            Sink->Message.Contains(BadName));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal creates no object"), Label),
            FindObject<UObject>(nullptr,
                *MatSafetyObjectPath(MatSafetyPackagePath(MatSafetyDefaultFolder, BadName),
                                     BadName)) == nullptr);
    }

    // The FOLDER half is an equally live one-argument kill and the ticket's per-site notes do not
    // say so: a perfectly bare `name` with `path: "/Game//Materials"` composes a "//" package path
    // just as surely. Nothing in this cluster collapses an interior double slash - the material
    // verbs run no folder sanitiser at all (TextureHandler's sites route through the shared
    // NormalizeContentAssetPath, which does collapse one, but nothing here calls it). What covers it is that
    // the guard validates the COMPOSED path (FPackageName::IsValidLongPackageName), not just the
    // bare name, so a name-only character check would not have been enough.
    //
    // Safe to drive for exactly the reason the bad-name cases are: the absent parentMaterial is
    // resolved above the concatenation on a reverted build.
    inline void ExpectInstanceFolderRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadFolder, const TCHAR* Label)
    {
        const FString BareName = MatSafetyUniqueName(TEXT("Folder"));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BareName);
        Params->SetStringField(TEXT("path"), BadFolder);
        Params->SetStringField(TEXT("parentMaterial"), MatSafetyAbsentParentPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("material.authoring.create_material_instance"),
            TEXT("req-mat-create-instance-folder-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("a %s folder ('%s') is refused"), Label, *BadFolder),
            bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("a %s folder is refused as a caller argument error, not as a missing parent"),
            Label), ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s folder refusal quotes the composed path"), Label),
            Sink->Message.Contains(BadFolder));
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// A `name` carrying a path is refused instead of composing "//" into CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateInstanceNameCarryingAPathIsRefusedTest,
    "PinWright.material.authoring.create_material_instance.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateInstanceNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace MaterialCreateNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // The literal one-argument kill: "//" inside the name reaches CreatePackage's first Fatal
    // whichever way the folder and the name are joined, because neither FString::operator/ nor
    // FString::Printf touches the interior of the right-hand side.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink, TEXT("MI_a//b"), TEXT("double slash"));

    // The shape measured on B-foliage-add-type-name-with-slash-kills-the-editor. Note it is NOT
    // fatal through this verb's old composition: `Folder / Name` is PathAppend
    // (Core/Private/Containers/String.cpp.inl:855-885), which does not double a leading slash on
    // the right, so pre-fix this wrote the asset silently to a destination the caller never named
    // rather than killing the editor. Refused either way - the destination is `path`'s job.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink,
        TEXT("/Game/PinWrightScratch/MI_Rooted"), TEXT("rooted path"));

    // The whole class, not just the fatal members. An interior slash composes a nested package
    // rather than "//", so it does not fatal - but it silently writes somewhere the caller did not
    // name, which is the same argument confusion one step short of the crash.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink, TEXT("Sub/Leaf"), TEXT("interior slash"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
    // against the engine's package rules as well as the name against its object rules.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that has no sanitizer of its own.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink, TEXT("../Escape"), TEXT("traversal"));

    // A trailing slash composes "<folder>/Name/", which CreatePackage would resolve to something
    // other than the caller's name.
    ExpectInstanceNameRefused(*this, Dispatcher, Sink, TEXT("Trailing/"), TEXT("trailing slash"));

    // THE FOLDER IS THE OTHER ONE-ARGUMENT KILL. A perfectly bare name with a "//" in `path`
    // composes exactly the same fatal package path, and nothing in this cluster collapses an
    // interior double slash. This is what proves the guard validates the COMPOSED path and not
    // merely the bare name - a name-only character filter passes this payload straight through.
    ExpectInstanceFolderRefused(*this, Dispatcher, Sink, TEXT("/Game//Materials"),
        TEXT("double slash"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same absent parent, so it stops at ASSET_NOT_FOUND - which is exactly the
    // proof that a bare name got PAST the new check and reached the parent resolution.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), MatSafetyUniqueName(TEXT("Control")));
        Params->SetStringField(TEXT("parentMaterial"), MatSafetyAbsentParentPath());

        bool bSuccess = true;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material_instance"),
            TEXT("req-mat-create-instance-name-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its parent material does not exist"),
            bSuccess);
        TestEqual(TEXT("a BARE name passes the name check and is refused on the parent instead"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// VALID-INPUT CONTROL: the guard did not break the happy path it was added in front of
// ============================================================================
// The refusal test above can be satisfied by a handler that refuses everything, and the
// ASSET_NOT_FOUND control only proves a bare name reaches the NEXT check. This one drives a
// complete, successful create through the rewritten composition and reads the asset back from the
// engine at the path the caller can construct - so a guard that mangled the composed path (or a
// verb that echoed the requested folder while writing somewhere else) fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateInstanceValidNameStillCreatesTest,
    "PinWright.material.authoring.create_material_instance.ValidNameStillCreates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateInstanceValidNameStillCreatesTest::RunTest(const FString& Parameters)
{
    using namespace MaterialCreateNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString ParentName = MatSafetyUniqueName(TEXT("Parent"));
    const FString InstanceName = MatSafetyUniqueName(TEXT("Instance"));
    const FString ParentPath = MatSafetyPackagePath(MatSafetyScratchFolder, ParentName);
    const FString InstancePath = MatSafetyPackagePath(MatSafetyScratchFolder, InstanceName);

    // Both destinations are cleaned up, and so is the default folder: if the folder slot were
    // ignored the instance lands under /Game/Materials, and this test must not leak it either way.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(ParentPath);
        CleanupTestAsset(MatSafetyPackagePath(MatSafetyDefaultFolder, InstanceName));
    };

    // The parent is built through the already-guarded create_material verb rather than a raw
    // CreatePackage, so this test never composes a package path of its own.
    {
        TSharedPtr<FJsonObject> ParentParams = MakeShared<FJsonObject>();
        ParentParams->SetStringField(TEXT("name"), ParentName);
        ParentParams->SetStringField(TEXT("path"), MatSafetyScratchFolder);
        ParentParams->SetBoolField(TEXT("save"), false);

        bool bParentCreated = false;
        FString ParentErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material"),
            TEXT("req-mat-create-instance-valid-parent"), ParentParams, bParentCreated,
            ParentErrorCode);
        if (!TestTrue(*FString::Printf(TEXT("fixture parent material created (error=%s: %s)"),
                *ParentErrorCode, *Sink->Message), bParentCreated))
        {
            return true;
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), InstanceName);
    Params->SetStringField(TEXT("parentMaterial"), ParentPath);
    Params->SetStringField(TEXT("path"), MatSafetyScratchFolder);
    Params->SetBoolField(TEXT("save"), false);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material_instance"),
        TEXT("req-mat-create-instance-valid"), Params, bSuccess, Result, ErrorCode);

    if (!TestTrue(*FString::Printf(
            TEXT("a bare name with an existing parent still creates (error=%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess))
    {
        return true;
    }

    // Re-read from the ENGINE at the path the caller can construct, not from the response.
    UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(
        nullptr, *MatSafetyObjectPath(InstancePath, InstanceName));
    TestNotNull(TEXT("the material instance is addressable at <path>/<name>"), Instance);
    TestFalse(TEXT("nothing was written to the default /Game/Materials destination"),
        UEditorAssetLibrary::DoesAssetExist(
            MatSafetyPackagePath(MatSafetyDefaultFolder, InstanceName)));

    // A FOLDER ENDING IN '/' MUST STILL WORK. This is the specific way the guard could narrow
    // what the verb accepts rather than only refusing what kills the editor: the composition it
    // replaced was `Folder / Name`, and FString::operator/ pops a trailing separator (PathAppend,
    // Core/Private/Containers/String.cpp.inl:855-885) while the validator joins with Printf and
    // does not - so an untrimmed folder would compose "//" and be refused for a rule the caller
    // never broke. Without this case, TrimTrailingFolderSeparator could be deleted and every
    // other assertion in this file would still pass.
    {
        const FString TrailingName = MatSafetyUniqueName(TEXT("Trailing"));
        const FString TrailingPath = MatSafetyPackagePath(MatSafetyScratchFolder, TrailingName);
        ON_SCOPE_EXIT { CleanupTestAsset(TrailingPath); };

        TSharedPtr<FJsonObject> TrailingParams = MakeShared<FJsonObject>();
        TrailingParams->SetStringField(TEXT("name"), TrailingName);
        TrailingParams->SetStringField(TEXT("parentMaterial"), ParentPath);
        TrailingParams->SetStringField(TEXT("path"),
            FString(MatSafetyScratchFolder) + TEXT("/"));
        TrailingParams->SetBoolField(TEXT("save"), false);

        bool bTrailingSuccess = false;
        FString TrailingErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material_instance"),
            TEXT("req-mat-create-instance-trailing-folder"), TrailingParams, bTrailingSuccess,
            TrailingErrorCode);

        TestTrue(*FString::Printf(
            TEXT("a folder ending in '/' is still accepted (error=%s: %s)"),
            *TrailingErrorCode, *Sink->Message), bTrailingSuccess);
        TestNotNull(TEXT("the trailing-slash folder resolves to the same destination"),
            LoadObject<UMaterialInstanceConstant>(
                nullptr, *MatSafetyObjectPath(TrailingPath, TrailingName)));
    }

    return true;
}
