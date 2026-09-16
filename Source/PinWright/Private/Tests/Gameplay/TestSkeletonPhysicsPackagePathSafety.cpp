// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestSkeletonPhysicsPackagePathSafety.cpp - regression coverage for the skeleton / physics
// share of B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. CreatePackage (UObjectGlobals.cpp:1086-1120) logs at **Fatal** - a verbosity
// that is not compiled out in any configuration - for two inputs: a package name containing "//"
// (:1094-1096) and one that resolves to empty (:1118). Fatal ends the PROCESS, so an unvalidated
// caller string reaching CreatePackage does not fail the call: it kills the editor and every
// unsaved package in it, in editors PinWright shares between agents. The handler's own
// `if (!Package)` can never fire, because nothing after CreatePackage is reached. BOTH Fatals are
// covered below, not just the measured one: ".." is the input that reaches the second, because
// ResolveName2 collapses "<folder>/.." to empty. Three verbs composed their package path from
// caller text and handed it over unchecked:
//
//   * physics.setup_physics_simulation - savePath was checked, physicsAssetName never was, and
//     the two were joined with FString::Printf(TEXT("%s/%s"), ...). Printf doubles the separator
//     UNCONDITIONALLY, unlike FString::operator/ (PathAppend, String.cpp.inl:855-885, doubles
//     only when the LEFT already ends with '/'), so this site was reachable by a ROOTED path in
//     the name ("/Game/X" -> "/Game/Physics//Game/X") as well as by a name that carries "//"
//     itself. It is the most reachable of the three.
//   * skeleton.create_physics_asset - outputPath had no guard of any kind. "//Game/X" survives
//     the GetPath()/GetBaseFilename() split-and-rejoin as "//Game/X"; "/" splits into two empty
//     halves and composes to "".
//   * skeleton.create_skeleton - see the honesty note on its test below.
//
// WHY THE THREE FIXES ARE NOT THE SAME SHAPE. Only the physics verb takes a folder+name pair, so
// only it routes through Handlers/PackagePathCompose.h. The other two take ONE whole caller path
// and are checked with FPackageName::IsValidLongPackageName directly on the composed string. That
// is not stylistic: the shared helper joins with Printf("%s/%s") and so doubles the separator when
// the folder already ends in '/', which on an operator/ site would newly REFUSE a trailing-slash
// folder that composes correctly today (PathAppend, String.cpp.inl:855-885, pops the separator
// instead of doubling). At the physics site the pre-fix code was already Printf, so the helper is
// a strict improvement there, and the folder is trimmed before it is handed over.
//
// CREATEPACKAGE IS NOT THE ONLY DOOR, WHICH IS WHY THE GUARDS SIT AT THE TOP OF EACH HANDLER.
// LoadObject -> StaticLoadObjectInternal calls ResolveName2(..., Create=true)
// (UObjectGlobals.cpp:1427), and ResolveName2 calls CreatePackage(*PartialName) (:1310), so ANY
// load of an unvalidated composed path reaches the same Fatal. physics.setup_physics_simulation
// has exactly that shape: its "does it already exist?" branch runs
// UEditorAssetLibrary::DoesAssetExist and then LoadObject<UPhysicsAsset> on the COMPOSED path,
// both several lines ABOVE the CreatePackage the ticket enumerated. A guard placed immediately
// before CreatePackage would have been no guard at all. FindObject is the safe counterpart
// (Create=false, :620) and is what the assertions below use.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the
// test host down with it, so a test that reproduced the defect would abort the whole suite rather
// than report a red - and a suite that dies mid-queue is not a failure signal, it is an absence
// of one (the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore
// on the post-fix CONTRACT - the refusal - and the fixture is built so that a build WITHOUT the
// fix takes a different, harmless path instead of the fatal one. The construction is copied from
// Tests/Environment/TestFoliageAddTypeNamePathSafety.cpp:
//
//   every refusal case pairs its bad name/path with a well-formed long package name, naming NO
//   asset (a fresh GUID), in the OTHER argument the verb resolves first - meshPath for
//   physics.setup_physics_simulation, skeletalMeshPath for skeleton.create_physics_asset. Both
//   verbs resolve that source asset ABOVE the composition, so on a reverted build the call is
//   refused ASSET_NOT_FOUND / MESH_NOT_FOUND there and the TestEqual on INVALID_ARGUMENT below
//   goes red while the process lives. CreatePackage is unreachable on both the fixed and the
//   reverted build.
//
// That property depends on each fix keeping its path check ABOVE the source-asset resolution,
// and both handlers carry a comment saying so. Do NOT "improve" these cases by supplying a
// source asset that exists, and do NOT rewrite them into a crash expectation: either change
// hands a live editor a string that ends it.
//
// NO MODAL CAN OPEN FROM THIS FILE. Board B-physics-asset-factory-modal-hang records that
// UPhysicsAssetFactory's body-generation modal (CreatePhysicsAssetFromMesh -> OpenNewBodyDlg ->
// GEditor->EditorAddModalWindow) wedges the game thread and the suite host with it. Every case
// here is a REFUSAL or a source-asset miss, so no case reaches physics-asset creation at all -
// neither the factory nor the headless replacement. There is deliberately no valid-input control
// that creates a physics asset: Tests/Gameplay/TestPhysicsAssetFactoryModalHang.cpp already owns
// that coverage for both verbs, through the factory-free path. The controls below instead prove a
// WELL-FORMED name/path gets PAST the new check by asserting it is refused further down, on the
// absent source asset - which is exactly the discrimination needed and reaches no creation code.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace SkeletonPhysicsPackagePathSafetyHelpers
{
    // The verb's default destination, which every physics refusal case must leave untouched.
    constexpr const TCHAR* SafetyDefaultPhysicsFolder = TEXT("/Game/Physics");

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee described in the file header: it makes a PRE-FIX handler bail at its
    // source-asset resolution, above the composition, instead of reaching CreatePackage.
    inline FString SafetyAbsentSourcePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/SKM_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWPhysSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // The composition the pre-fix physics handler performed, reproduced so the assertions can
    // look for an object at the exact path a leaked half-built package would occupy.
    inline FString SafetyPrintfObjectPath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *Name, *Name);
    }

    // Drives one physics.setup_physics_simulation refusal case and reports the three facts that
    // separate the post-fix contract from the pre-fix behaviour: it is refused, it is refused as
    // a CALLER ARGUMENT error rather than as a missing mesh, and the message names the offending
    // value so the caller can act.
    inline void ExpectPhysicsAssetNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadName, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("physicsAssetName"), BadName);
        Params->SetStringField(TEXT("meshPath"), SafetyAbsentSourcePath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("physics.setup_physics_simulation"),
            TEXT("req-physics-setup-name-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(
            TEXT("a %s physicsAssetName ('%s') is refused"), Label, *BadName), bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // ASSET_NOT_FOUND from the meshPath resolution, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s physicsAssetName is refused as a caller argument error, not as a missing "
                 "mesh"), Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name"), Label),
            Sink->Message.Contains(BadName));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal creates no object"), Label),
            FindObject<UObject>(nullptr,
                *SafetyPrintfObjectPath(SafetyDefaultPhysicsFolder, BadName)) == nullptr);
    }

    // The FOLDER half of the same hazard, which the ticket's "folder guarded" classification
    // misses. savePath's guard is FPackageName::IsValidLongPackageName - a validator, which does
    // reject "//" - but on failure it falls back to TryConvertFilenameToLongPackageName and
    // assigns the result WITHOUT re-validating. That fallback is a normalizer, not a validator:
    // FPaths::NormalizeFilename runs with bRemoveDuplicateSlashes=false (Paths.cpp:1332-1339) and
    // InternalFilenameToLongPackageName's final `OutPackageName << Result` returns the last
    // attempted span, so "/Game//Physics" comes back out of it verbatim and reports success. The
    // "//" therefore survives the folder guard entirely, and pre-fix it composed a fatal package
    // path with a perfectly BARE name - or with no caller name at all, since the derived
    // "<MeshName>_Physics" default takes the same route. What stops it is the check on the
    // COMPOSED path inside PinWrightComposeAssetPackagePath, not anything the name check does.
    inline void ExpectSavePathRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadSavePath, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // A deliberately BARE, well-formed name: the refusal below must come from the folder.
        Params->SetStringField(TEXT("physicsAssetName"), SafetyUniqueName(TEXT("BareName")));
        Params->SetStringField(TEXT("savePath"), BadSavePath);
        Params->SetStringField(TEXT("meshPath"), SafetyAbsentSourcePath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("physics.setup_physics_simulation"),
            TEXT("req-physics-setup-savepath-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(
            TEXT("a %s savePath ('%s') is refused even with a bare name"), Label, *BadSavePath),
            bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("a %s savePath is refused as a caller argument error, not as a missing mesh"),
            Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending folder"), Label),
            Sink->Message.Contains(BadSavePath));
    }

    // The same, for skeleton.create_physics_asset's whole-path outputPath argument.
    inline void ExpectOutputPathRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadPath, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("skeletalMeshPath"), SafetyAbsentSourcePath());
        Params->SetStringField(TEXT("outputPath"), BadPath);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("skeleton.create_physics_asset"),
            TEXT("req-create-physics-asset-outputpath-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(
            TEXT("a %s outputPath ('%s') is refused"), Label, *BadPath), bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // MESH_NOT_FOUND from the source-asset resolution, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s outputPath is refused as a caller argument error, not as a missing source "
                 "asset"), Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal names outputPath"), Label),
            Sink->Message.Contains(TEXT("outputPath")));
    }
}

// ============================================================================
// physics.setup_physics_simulation - a physicsAssetName carrying a path or "//" is refused
// instead of being Printf-joined onto savePath and handed to CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetupSimulationNameCarryingAPathIsRefusedTest,
    "PinWright.physics.setup_physics_simulation.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsSetupSimulationNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace SkeletonPhysicsPackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // The shape the Printf composition makes reachable that operator/ does not: a ROOTED path in
    // the name composed "/Game/Physics//Game/..." unconditionally.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink,
        TEXT("/Game/PinWrightScratch/PA_NameSafety"), TEXT("rooted path"));

    // The one-argument kill that every unguarded site shares regardless of which composition it
    // uses - a name that carries "//" itself.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT("Sub//Leaf"),
        TEXT("embedded double slash"));

    // An interior slash composes a nested package rather than "//", so it does not fatal - but it
    // silently writes somewhere the caller did not name, one step short of the crash.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT("Sub/Leaf"),
        TEXT("interior slash"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
    // against the engine's package rules as well as the name against its object rules.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that had no sanitizer of its own.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT("../Escape"), TEXT("traversal"));

    // The SECOND Fatal, not the "//" one: ".." composes "/Game/Physics/..", which ResolveName2
    // collapses to empty, and CreatePackage logs an empty package name at Fatal too
    // (UObjectGlobals.cpp:1118). Both of CreatePackage's two process-ending inputs are therefore
    // covered by this verb's cases, not just the measured one.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT(".."), TEXT("bare traversal"));

    // A trailing slash composes "/Game/Physics/Name/", which CreatePackage would resolve to
    // something other than the caller's name.
    ExpectPhysicsAssetNameRefused(*this, Dispatcher, Sink, TEXT("Trailing/"),
        TEXT("trailing slash"));

    // FOLDER cases. A bare name cannot save a folder that carries "//" - see the note on
    // ExpectSavePathRefused for why savePath's own guard lets it through.
    ExpectSavePathRefused(*this, Dispatcher, Sink, TEXT("/Game//Physics"),
        TEXT("interior double slash"));
    ExpectSavePathRefused(*this, Dispatcher, Sink, TEXT("//Game/Physics"),
        TEXT("double-slash root"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same absent mesh, so it stops at ASSET_NOT_FOUND - which is exactly the
    // proof that a bare name got PAST the new check and reached the mesh resolution. It reaches
    // no creation code, so it cannot open the factory modal of
    // B-physics-asset-factory-modal-hang.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("physicsAssetName"), SafetyUniqueName(TEXT("Control")));
        Params->SetStringField(TEXT("meshPath"), SafetyAbsentSourcePath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("physics.setup_physics_simulation"),
            TEXT("req-physics-setup-name-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its mesh does not exist"), bSuccess);
        TestEqual(TEXT("a BARE name passes the name check and is refused on the mesh instead"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// skeleton.create_physics_asset - an outputPath that would compose a package name CreatePackage
// dies on is refused
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreatePhysicsAssetOutputPathIsValidatedTest,
    "PinWright.skeleton.create_physics_asset.OutputPathIsValidated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreatePhysicsAssetOutputPathIsValidatedTest::RunTest(const FString& Parameters)
{
    using namespace SkeletonPhysicsPackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // CreatePackage's first Fatal, verbatim: the leading "//" survives the split-and-rejoin.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("//Game/PinWrightScratch/PA_Safety"),
        TEXT("double-slash root"));

    // CreatePackage's second Fatal: "/" splits into two empty halves and composes to "", which
    // resolves to an empty package name.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("/"), TEXT("bare slash"));

    // Interior "//" - the one-argument kill this ticket's sweep is named for.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("//PinWrightScratch//PA_Safety"),
        TEXT("interior double slash"));

    // The FOLDER half, under a real mounted root and with a perfectly bare leaf name: the "//"
    // lives entirely in the directory part and still composes a package path CreatePackage dies
    // on. This is the shape a folder NORMALIZER (as opposed to a validator) lets through.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("/Game//Physics/PA_Safety"),
        TEXT("double slash in the folder half"));

    // CreatePackage's second Fatal again, reached through the split-and-rejoin rather than
    // directly: ".." composes to "." and "/Game/.." composes to "/Game/.", both of which resolve
    // to an empty package name.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT(".."), TEXT("bare traversal"));
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("/Game/.."), TEXT("rooted traversal"));

    // Not a Fatal, but the same argument confusion: with no leading slash CreatePackage builds a
    // rootless package the caller cannot address and never asked for.
    ExpectOutputPathRefused(*this, Dispatcher, Sink, TEXT("PinWrightScratch/PA_Safety"),
        TEXT("rootless"));

    // An unmounted root is refused rather than silently creating a package under a root that
    // cannot be saved.
    ExpectOutputPathRefused(*this, Dispatcher, Sink,
        TEXT("/PinWrightNotAMountPoint/PA_Safety"), TEXT("unmounted root"));

    // CONTROL. A well-formed outputPath must get PAST the new check and be refused further down,
    // on the absent source asset - proving the check is not simply refusing everything. It
    // reaches no creation code, so no factory modal is possible.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("skeletalMeshPath"), SafetyAbsentSourcePath());
        Params->SetStringField(TEXT("outputPath"),
            FString::Printf(TEXT("/Game/PinWrightScratch/%s"), *SafetyUniqueName(TEXT("Ctl"))));

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("skeleton.create_physics_asset"),
            TEXT("req-create-physics-asset-outputpath-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its source asset does not exist"),
            bSuccess);
        TestEqual(TEXT("a well-formed outputPath passes the path check and is refused on the "
                       "source asset instead"),
            ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// skeleton.create_skeleton - the composed package name is checked on the line that uses it
// ============================================================================
//
// HONESTY NOTE, so nobody reads more into a green here than it carries. Unlike the two tests
// above, this one does NOT discriminate a fixed build from a reverted one, and no input exists
// that would. skeleton.create_skeleton already rejected "..", "//", "\", a non-mount root and
// anything failing FPackageName::IsValidLongPackageName(path, /*readOnlyRoots=*/false) BEFORE
// this ticket, and it then handed CreatePackage a RECOMPOSITION of that path
// (GetPath() / GetBaseFilename()) rather than the string it had validated. Because the surviving
// path can contain no "//", no trailing slash and no '.' (which is in
// INVALID_LONGPACKAGE_CHARACTERS), the split-and-rejoin is provably the identity, so the fix -
// moving a check onto the composed string itself - can refuse nothing that works today. Its value
// is structural: the identity is a property of the three lines above CreatePackage, not of the
// call, and an edit to the composition would otherwise silently reopen the Fatal with no test
// noticing. This test pins the refusals so that a future weakening of ANY of those checks turns
// red here instead of turning an agent's editor off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateSkeletonHazardousPathsAreRefusedTest,
    "PinWright.skeleton.create_skeleton.HazardousPathsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateSkeletonHazardousPathsAreRefusedTest::RunTest(const FString& Parameters)
{
    using namespace SkeletonPhysicsPackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Each of these composes a package name CreatePackage logs Fatal on, or one the caller did
    // not name. None may reach it.
    const TCHAR* const HazardousPaths[] = {
        TEXT("/Game//PinWrightScratch/SK_Safety"),   // "//" - CreatePackage's first Fatal
        TEXT("//Game/PinWrightScratch/SK_Safety"),   // "//" at the root
        TEXT("/"),                                    // composes to "" - the second Fatal
        TEXT(".."),                                   // composes to "." - the second Fatal
        TEXT("/Game/.."),                             // composes to "/Game/." - likewise
        TEXT("/Game/../../Engine/Content/SK_Safety"), // traversal out of the mount
        TEXT("/Game/PinWrightScratch/SK_Safety/"),    // trailing slash
        TEXT("PinWrightScratch/SK_Safety"),           // rootless
        TEXT("/Game/PinWrightScratch\\SK_Safety"),    // backslash
    };

    for (const TCHAR* HazardousPath : HazardousPaths)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), HazardousPath);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("skeleton.create_skeleton"),
            TEXT("req-create-skeleton-path-safety"), Params, bSuccess, ErrorCode);

        TestFalse(*FString::Printf(TEXT("skeleton.create_skeleton refuses '%s'"), HazardousPath),
            bSuccess);
        // Refused as a caller-argument problem, not swallowed as an internal failure - a
        // PACKAGE_ERROR here would mean CreatePackage had been reached and had merely returned
        // null, which is not what happens: it does not return on these inputs.
        TestTrue(*FString::Printf(
            TEXT("'%s' is refused as a caller argument error (got %s)"), HazardousPath,
            *ErrorCode),
            ErrorCode == TEXT("INVALID_PATH") || ErrorCode == TEXT("INVALID_ARGUMENT"));
    }

    // NO POSITIVE CONTROL HERE, deliberately. The case a control would guard against - a handler
    // that refuses everything - is already covered: Tests/Gameplay/TestAnimationHandlers.cpp
    // drives skeleton.create_skeleton to SUCCESS as the fixture for its bare-skeleton physics
    // tests, and fails if the verb stops creating skeletons. Repeating it here would add a real
    // package write and a force-delete GC to a test whose whole job is to prove nothing is
    // written, for information the suite already carries.

    return true;
}
