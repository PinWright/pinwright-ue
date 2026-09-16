// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPoseSearchCreateAssetPathSafety.cpp - regression coverage for the two pose_search sites of
// B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT THIS LOCKS DOWN. pose_search.create_schema and pose_search.create_database each hand a
// caller-composed package path to CreatePackage (HandleCreateSchema / HandleCreateDatabase). That
// call logs at **Fatal** - a verbosity not compiled out in any configuration, which ends the
// PROCESS and every unsaved package in it - for a name containing "//" (UObjectGlobals.cpp:
// 1094-1096) and for a name that resolves to empty (:1118). No `if (!Package)` check downstream
// can catch it, because nothing after the call is reached.
//
// Unlike the folder+name sites in the same sweep, both of these take ONE whole caller-supplied
// path (`assetPath`), and the guard the ticket prescribes for that shape was already in place:
// BuildCreatePaths stacks THREE independent checks before it publishes OutPackagePath, and none of
// them is below the CreatePackage call -
//
//   1. NormalizeAssetPath (Utils/AssetUtils.cpp:51) - engine IsValidLongPackageName on the
//      normalized path, with a rescue pass that only fires for a leaf naming a package that
//      actually exists;
//   2. SanitizeProjectRelativePath (Utils/PathUtils.cpp:34-89) - collapses "//" in a loop,
//      rejects "..", a drive letter and an unmounted root;
//   3. FPackageName::IsValidLongPackageName (PackageName.cpp:1758) - rejects "//", the
//      empty/too-short name, a missing leading slash, a trailing slash and
//      INVALID_LONGPACKAGE_CHARACTERS.
//
// So the sweep's verdict on these two sites was wrong, and this file exists to keep it wrong: the
// handlers were NOT changed, and what is asserted below is the contract those three checks
// already provide. Delete any one of them and two remain; delete the block and this test goes red.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is an absence of a signal, not a failure one (the
// DID_NOT_COMPLETE state in the plugin's testing notes). Three properties keep CreatePackage
// unreachable here:
//
//   * ON THE CURRENT BUILD every MALFORMED `assetPath` below is refused INVALID_PATH inside
//     BuildCreatePaths, which sets OutObjectPath only on its LAST line - so a rejected path never
//     even reaches the ALREADY_EXISTS lookup, let alone CreatePackage. The one exception is the
//     bare mount root, which the guard ACCEPTS and the companion-asset load refuses instead; it is
//     driven through ExpectAssetPathRefusedDownstream and is not a Fatal vector. Measured
//     2026-08-31, after the first version of this file asserted INVALID_PATH there and went red.
//   * ON A BUILD MISSING ONE OR TWO of the three checks the remaining ones still refuse: every
//     malformed value below carries "//", ".." or a drive letter, and no single check owns all
//     three.
//   * ON A BUILD MISSING ALL THREE every refusal case still pairs its bad `assetPath` with a
//     COMPANION required argument - `skeleton` for create_schema, `schema` for create_database -
//     that is a well-formed long package name naming NO asset (a fresh GUID). Both handlers
//     resolve that companion ABOVE their CreatePackage call, so the call is refused
//     SKELETON_NOT_FOUND / SCHEMA_NOT_FOUND there and the TestEqual on INVALID_PATH goes red while
//     the process lives.
//
// ONE CONSTRAINT THE ABOVE MAKES EXPLICIT, because it is not obvious and it is easy to break. The
// guard may not be relaxed to "validate later": the ALREADY_EXISTS step between BuildCreatePaths
// and CreatePackage calls LoadObject on the composed object path, and StaticLoadObject resolves
// through ResolveName2 with Create=true, which itself calls CreatePackage on the raw package name
// (UObjectGlobals.cpp:1310). A "//" that got past BuildCreatePaths would die at the existence
// check rather than at the create - one line earlier, equally fatal. Keep the validation where it
// is, and do NOT rewrite these cases into a crash expectation: that hands a live editor a string
// that ends it.
//
// THE FOLDER IS A KILL VECTOR IN ITS OWN RIGHT, and for these two verbs it is the ONLY one. Both
// take a single whole path, so there is no separate name argument a check could be attached to: a
// "//" between folder segments is fatal no matter how impeccable the leaf after it is. That is why
// the double-slash cases below put the "//" in the folder portion and follow it with a bare GUID
// leaf, and why they are spelled twice - mid-path ("/Game/PinWrightTests//<leaf>") and at the root
// ("//Game/PinWrightTests/<leaf>"), the second being the shape a hand-rolled "starts with a mount
// point" check waves through. A name-only character filter would have caught neither.
//
// The FindObject assertions below are safe on any input, including a "//" one: StaticFindObject
// resolves with Create=false (UObjectGlobals.cpp:620) and never reaches CreatePackage.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// The same three-header condition PoseSearchHandler.cpp compiles its own MCP_HAS_POSESEARCH
// from. UE 5.4 ships PoseSearchFeatureChannel_Position.h under the plugin's PRIVATE source
// directory, so the condition is false there, every pose_search verb answers PLUGIN_DISABLED
// out of EnsurePoseSearchAvailable before BuildCreatePaths runs, and the INVALID_PATH contract
// asserted below simply does not exist on that engine. The name is file-local because the two
// sibling Gameplay TUs define MCP_TEST_HAS_POSESEARCH and Unity merges all three.
#if __has_include("PoseSearch/PoseSearchSchema.h") && __has_include("PoseSearch/PoseSearchDatabase.h") && __has_include("PoseSearch/PoseSearchFeatureChannel_Position.h")
#define PWPS_PATHSAFETY_HAS_POSESEARCH 1
#else
#define PWPS_PATHSAFETY_HAS_POSESEARCH 0
#endif

// Named (not anonymous) namespace: the plugin's tests share one module per sub-module with Unity
// builds enabled, where same-named anonymous-namespace helpers collide across merged TUs. These
// names are also distinct from the helpers in the two sibling Gameplay TUs.
namespace PoseSearchCreateAssetPathSafetyHelpers
{
    // Every bad path below ends in a fresh GUID leaf. The original reason is gone:
    // NormalizeAssetPath's last resort used to be retrying the leaf under /Game, /Engine and
    // /Script and accepting the first that named an EXISTING package, which made a fixed leaf's
    // refusal dependent on host content. That fallback has been deleted (Utils/AssetUtils.cpp - it
    // returned bIsValid=true naming a DIFFERENT package). The GUID leaf stays because it still
    // rules out collision with real content, but it is no longer what makes these deterministic.
    inline FString SafetyUniqueLeaf(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWPoseSearchPathSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee in the file header: it makes a build with the path guard gone bail at
    // the companion-asset resolution, above CreatePackage.
    inline FString SafetyAbsentAssetPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/%s_Absent_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyObjectPath(const FString& PackagePath)
    {
        FString Leaf = PackagePath;
        int32 LastSlash = INDEX_NONE;
        if (PackagePath.FindLastChar(TEXT('/'), LastSlash))
        {
            Leaf = PackagePath.RightChop(LastSlash + 1);
        }
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *Leaf);
    }

    // create_schema needs skeleton + channels alongside assetPath; both are required and both are
    // read AFTER BuildCreatePaths, so they must be well-formed for the reverted-build argument to
    // hold.
    inline TSharedPtr<FJsonObject> MakeSchemaPayload()
    {
        TSharedPtr<FJsonObject> PositionChannel = MakeShared<FJsonObject>();
        PositionChannel->SetStringField(TEXT("type"), TEXT("Position"));
        PositionChannel->SetStringField(TEXT("bone"), TEXT("root"));

        TArray<TSharedPtr<FJsonValue>> Channels;
        Channels.Add(MakeShared<FJsonValueObject>(PositionChannel));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("skeleton"), SafetyAbsentAssetPath(TEXT("SK")));
        Params->SetArrayField(TEXT("channels"), Channels);
        Params->SetBoolField(TEXT("save"), false);
        return Params;
    }

    // create_database needs schema alongside assetPath, read after BuildCreatePaths for the same
    // reason.
    inline TSharedPtr<FJsonObject> MakeDatabasePayload()
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("schema"), SafetyAbsentAssetPath(TEXT("PSSchema")));
        Params->SetBoolField(TEXT("save"), false);
        return Params;
    }

    // Drives one refusal case and reports on the three facts that separate the guarded contract
    // from an unguarded handler: it is refused, it is refused as a PATH error rather than as a
    // missing companion asset, and the message quotes the offending value so the caller can act.
    // For an input that IS refused but NOT by the path guard, and is not a CreatePackage Fatal
    // vector either. A bare mount root is the case: it carries no "//" and does not resolve empty,
    // so both Fatal branches are unreachable, and FPackageName::IsValidLongPackageName accepts it
    // -- BuildCreatePaths returns true and the companion-asset load below refuses it instead.
    // Asserting INVALID_PATH here measured a contract the handler never had. What is worth pinning
    // is that the call is refused at all and the process survives; the seven genuinely malformed
    // values above are what discriminate a guarded build from an unguarded one.
    inline void ExpectAssetPathRefusedDownstream(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Params, const FString& BadPath, const TCHAR* Label)
    {
        Params->SetStringField(TEXT("assetPath"), BadPath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Params, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s handler is registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s answered the %s path"), Method, Label),
            Capture.bWasCalled);
        Test.TestFalse(*FString::Printf(TEXT("%s: a %s assetPath ('%s') is refused"), Method,
            Label, *BadPath), Capture.bSuccess);
        Test.TestFalse(*FString::Printf(
            TEXT("%s: a %s assetPath does not create the asset"), Method, Label),
            Capture.bSuccess && Capture.ErrorCode.IsEmpty());
    }

    inline void ExpectAssetPathRefused(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Params, const FString& BadPath, const TCHAR* Label)
    {
        Params->SetStringField(TEXT("assetPath"), BadPath);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Params, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s handler is registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s answered the %s path"), Method, Label),
            Capture.bWasCalled);

        Test.TestFalse(*FString::Printf(TEXT("%s: a %s assetPath ('%s') is refused"), Method,
            Label, *BadPath), Capture.bSuccess);
        // The discriminator against a build whose path guard is gone: without it this same
        // payload answers SKELETON_NOT_FOUND / SCHEMA_NOT_FOUND from the companion asset, never
        // INVALID_PATH.
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a %s assetPath is refused as a path error, not as a missing companion asset"),
            Method, Label), Capture.ErrorCode, FString(TEXT("INVALID_PATH")));
        Test.TestTrue(*FString::Printf(TEXT("%s: the %s refusal quotes the offending path"),
            Method, Label), Capture.Message.Contains(BadPath));
    }

    // Skips rather than fails on a host where the engine plugin is absent. This sub-module is
    // only loaded when IPluginManager reports PoseSearch enabled, so the usual case is that the
    // whole file never runs; this covers the narrower case of the module being present but not
    // loaded, which the handler answers PLUGIN_DISABLED.
    inline bool SafetySkipIfPoseSearchUnavailable(FAutomationTestBase& Test)
    {
#if PWPS_PATHSAFETY_HAS_POSESEARCH
        if (FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(Test, TEXT("optional-plugin-not-shipped"),
            TEXT("PoseSearch module is not loaded; skipping the pose_search assetPath safety "
                 "assertions."));
        return true;
#else
        // Header-gated, not module-gated: the module loads on UE 5.4 and the handlers still
        // refuse every call PLUGIN_DISABLED, because they were compiled without the channel
        // header. Checking IsModuleLoaded alone let this file run there and measure that
        // refusal against an INVALID_PATH the build cannot produce.
        PinWrightTestSkip::SkipAssertions(Test, TEXT("optional-plugin-not-shipped"),
            TEXT("PoseSearch public headers are absent from this engine (UE 5.4 keeps "
                 "PoseSearchFeatureChannel_Position.h private), so pose_search.* is compiled out "
                 "and answers PLUGIN_DISABLED ahead of the path guard; skipping the assetPath "
                 "safety assertions."));
        return true;
#endif
    }
}

// ============================================================================
// pose_search.create_schema refuses a malformed assetPath instead of composing it into
// CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchCreateSchemaMalformedAssetPathIsRefusedTest,
    "PinWright.pose_search.CreateSchemaMalformedAssetPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchCreateSchemaMalformedAssetPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PoseSearchCreateAssetPathSafetyHelpers;

    if (SafetySkipIfPoseSearchUnavailable(*this))
    {
        return true;
    }

    // The ticket's one-argument kill, and note WHERE the "//" sits: in the FOLDER portion, with a
    // perfectly bare GUID leaf after it. The folder is a kill vector in its own right - a legal
    // asset name cannot save a malformed destination - and for a whole-path verb like this one
    // there is no separate name argument to check, so the guard has to be on the composed string.
    // A lenient handler would collapse this to /Game/PinWrightTests/<leaf> and write there
    // silently; an unguarded one dies at CreatePackage. Neither is the contract: NormalizeAssetPath
    // refuses it outright (its rescue pass cannot fire, see SafetyUniqueLeaf).
    const FString DoubleSlashLeaf = SafetyUniqueLeaf(TEXT("Schema"));
    const FString DoubleSlashPath =
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *DoubleSlashLeaf);
    ExpectAssetPathRefused(*this, TEXT("pose_search.create_schema"), MakeSchemaPayload(),
        DoubleSlashPath, TEXT("double slash"));

    // The same kill spelled at the ROOT rather than mid-path - a different string shape that a
    // hand-rolled "starts with a mount point" check would wave through.
    ExpectAssetPathRefused(*this, TEXT("pose_search.create_schema"), MakeSchemaPayload(),
        FString::Printf(TEXT("//Game/PinWrightTests/%s"), *SafetyUniqueLeaf(TEXT("Schema"))),
        TEXT("leading double slash"));
    // The collapse a lenient implementation would have performed. Asserting nothing landed there
    // is what separates "refused" from "sanitized and written somewhere else".
    TestNull(TEXT("a refused double-slash assetPath writes nothing at the collapsed path"),
        FindObject<UObject>(nullptr, *SafetyObjectPath(
            FString::Printf(TEXT("/Game/PinWrightTests/%s"), *DoubleSlashLeaf))));

    ExpectAssetPathRefused(*this, TEXT("pose_search.create_schema"), MakeSchemaPayload(),
        FString::Printf(TEXT("/Game/../../Engine/Content/%s"), *SafetyUniqueLeaf(TEXT("Schema"))),
        TEXT("traversal"));

    ExpectAssetPathRefused(*this, TEXT("pose_search.create_schema"), MakeSchemaPayload(),
        FString::Printf(TEXT("C:/Temp/%s"), *SafetyUniqueLeaf(TEXT("Schema"))),
        TEXT("windows absolute"));

    // A bare mount root is NOT a Fatal vector and is NOT refused by the path guard: it carries no
    // "//", does not resolve empty, and IsValidLongPackageName accepts it, so BuildCreatePaths
    // passes it through and the companion-asset load refuses it. Measured 2026-08-31 -- the
    // original expectation of INVALID_PATH here was wrong about the handler and about the engine.
    ExpectAssetPathRefusedDownstream(*this, TEXT("pose_search.create_schema"), MakeSchemaPayload(),
        TEXT("/Game"), TEXT("bare mount root"));

    // CONTROL. Without this, a handler that refused every assetPath would satisfy every case
    // above. Driven with the same absent skeleton, so it stops at SKELETON_NOT_FOUND - which is
    // exactly the proof that a well-formed path got PAST the path checks.
    {
        const FString ControlPath =
            FString::Printf(TEXT("/Game/PinWrightTests/%s"), *SafetyUniqueLeaf(TEXT("SchemaCtl")));
        // The ALREADY_EXISTS lookup ahead of the skeleton resolution leaves an empty in-memory
        // package behind for any path it fails to load; tear it down rather than leak it.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ControlPath);
        };

        TSharedPtr<FJsonObject> Params = MakeSchemaPayload();
        Params->SetStringField(TEXT("assetPath"), ControlPath);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("pose_search.create_schema"), Params, Capture);

        TestFalse(TEXT("the control call still fails - its skeleton does not exist"),
            Capture.bSuccess);
        TestEqual(TEXT("a well-formed assetPath passes the path checks and is refused on the "
                       "skeleton instead"),
            Capture.ErrorCode, FString(TEXT("SKELETON_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// pose_search.create_database refuses a malformed assetPath the same way, through the same
// BuildCreatePaths guard
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchCreateDatabaseMalformedAssetPathIsRefusedTest,
    "PinWright.pose_search.CreateDatabaseMalformedAssetPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchCreateDatabaseMalformedAssetPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PoseSearchCreateAssetPathSafetyHelpers;

    if (SafetySkipIfPoseSearchUnavailable(*this))
    {
        return true;
    }

    // Asserted separately from create_schema rather than assumed: the two verbs share
    // BuildCreatePaths today, and this case is what would catch one of them growing its own
    // composition later.
    const FString DoubleSlashLeaf = SafetyUniqueLeaf(TEXT("Database"));
    const FString DoubleSlashPath =
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *DoubleSlashLeaf);
    ExpectAssetPathRefused(*this, TEXT("pose_search.create_database"), MakeDatabasePayload(),
        DoubleSlashPath, TEXT("double slash"));
    TestNull(TEXT("a refused double-slash assetPath writes no database at the collapsed path"),
        FindObject<UObject>(nullptr, *SafetyObjectPath(
            FString::Printf(TEXT("/Game/PinWrightTests/%s"), *DoubleSlashLeaf))));

    // Root-spelled folder kill, as on create_schema.
    ExpectAssetPathRefused(*this, TEXT("pose_search.create_database"), MakeDatabasePayload(),
        FString::Printf(TEXT("//Game/PinWrightTests/%s"), *SafetyUniqueLeaf(TEXT("Database"))),
        TEXT("leading double slash"));

    ExpectAssetPathRefused(*this, TEXT("pose_search.create_database"), MakeDatabasePayload(),
        FString::Printf(TEXT("/Game/../../Engine/Content/%s"), *SafetyUniqueLeaf(TEXT("Database"))),
        TEXT("traversal"));

    ExpectAssetPathRefused(*this, TEXT("pose_search.create_database"), MakeDatabasePayload(),
        FString::Printf(TEXT("C:/Temp/%s"), *SafetyUniqueLeaf(TEXT("Database"))),
        TEXT("windows absolute"));

    ExpectAssetPathRefusedDownstream(*this, TEXT("pose_search.create_database"), MakeDatabasePayload(),
        TEXT("/Game"), TEXT("bare mount root"));

    // CONTROL, as above: a well-formed path reaches the schema resolution.
    {
        const FString ControlPath = FString::Printf(TEXT("/Game/PinWrightTests/%s"),
            *SafetyUniqueLeaf(TEXT("DatabaseCtl")));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ControlPath);
        };

        TSharedPtr<FJsonObject> Params = MakeDatabasePayload();
        Params->SetStringField(TEXT("assetPath"), ControlPath);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("pose_search.create_database"), Params, Capture);

        TestFalse(TEXT("the control call still fails - its schema does not exist"),
            Capture.bSuccess);
        TestEqual(TEXT("a well-formed assetPath passes the path checks and is refused on the "
                       "schema instead"),
            Capture.ErrorCode, FString(TEXT("SCHEMA_NOT_FOUND")));
    }

    return true;
}
