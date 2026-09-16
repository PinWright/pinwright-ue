// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestResolverPathSafety.cpp - regression coverage for the SHARED-RESOLVER cluster of
// B-createpackage-unvalidated-paths-plugin-wide: the six chokepoints in Utils/ClassUtils.cpp and
// Utils/AssetUtils.cpp that ~145 call sites funnel caller-supplied text through, plus the removal
// of NormalizeAssetPath's re-rooting fallback.
//
// WHAT WAS WRONG. CreatePackage logs at Fatal for a package name containing "//"
// (UObjectGlobals.cpp:1094-1096), a verbosity that is not compiled out in any configuration: the
// editor PROCESS ends and every unsaved package in it is lost. CreatePackage is not only reached
// by direct-create sites - every LOAD gets there too. StaticLoadObjectInternal calls
// ResolveName2(..., Create=true) (:1427) and ResolveName2 calls CreatePackage on the partial name
// (:1310). A dot-free string still arrives: ResolveName2 returns immediately at :1241 with no
// delimiter, and StaticLoadObjectInternal re-enters itself with InName + "." + GetShortName(InName)
// (:1474-1482), so the second pass has the dot. FindObject is safe (Create=false, :620), which is
// why in every function below the guard could be placed above the whole body rather than split.
//
// WHAT THESE TESTS ASSERT, AND WHAT THEY CANNOT. They call each chokepoint DIRECTLY. There is no
// verb and no dispatcher here on purpose: a "//" payload driven through a verb would, on a build
// with the guard removed, reach the Fatal and take the suite host down mid-queue - which is an
// ABSENCE of a signal rather than a red (the DID_NOT_COMPLETE state in the plugin's testing notes).
//
// BE HONEST ABOUT THE LIMIT: unlike Tests/Media/TestAudioCreatePackagePathSafety.cpp, these cases
// cannot be built so that a build WITHOUT the fix takes a harmless alternate path. That file could
// pair its bad name with a second bad argument the handler validated FIRST; here the guard IS the
// first statement of each function and there is nothing above it to bail on. So these tests LOCK
// the post-fix refusal contract; they do not discriminate a reverted build without cost. Anyone
// deleting a guard below should expect this file to end the process rather than report a red, and
// that is the correct trade: the alternative is no coverage of a contract 145 call sites depend on.
// The one mitigation that IS available is applied - the predicate itself is asserted first, and the
// lethal calls are skipped entirely if it is broken.
//
// WHY EVERY REFUSAL IS ALSO AN EXPECTED LOG WARNING. The four ClassUtils resolvers return only
// nullptr across their 56/12/9/19 call sites and have no error channel, so the UE_LOG line is their
// whole diagnosis. It is logged at Warning, never Error, because
// UAutomationControllerSettings::bElevateLogWarningsToErrors turns a captured Error into a test
// failure - but that same setting ELEVATES a captured Warning to an Error too
// (AutomationTest.cpp:128-137), so a test that provokes one must declare it. Declaring it with an
// exact occurrence count turns the suppression into an assertion in two directions: the refusal
// must log (an unmet expected message fails the test, AutomationTest.cpp:1376) and the valid-input
// controls in the same test must NOT log (a second occurrence overshoots the count and fails too).
// That is what makes "the guard does not refuse everything" a checked property here rather than a
// hope.
#include "Misc/AutomationTest.h"

// AddExpectedMessagePlain is a UE 5.4+ member; on 5.3 this header supplies the escaping shim.
#include "Compat/EngineVersionCompat.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/PathUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
// UClass / UEnum / UScriptStruct as complete types: ClassUtils.h declares them only as return
// pointers and this TU must not depend on a Unity neighbour having pulled them in.
#include "UObject/Class.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace ResolverPathSafetyHelpers
{
    // Every probe below ends in a fresh GUID leaf. That is load-bearing rather than decoration:
    // a fixed leaf could collide with a real package on some host and turn a refusal case into a
    // resolution, and the refusal would then be reported as a pass on one machine and a failure on
    // another.
    inline FString UniqueLeaf(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWResolverSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A rooted path carrying the lethal byte sequence at the folder/leaf boundary.
    inline FString DoubledSlashPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(Prefix));
    }

    // THE ONE PRE-FLIGHT THAT MAKES THE LETHAL CALLS SAFE TO ATTEMPT. Every case in this file hands
    // a resolver a string that ends the process if the guard is not there, and the guard's whole
    // decision is this predicate. If it has been broken to return false, calling the resolvers
    // would be a suicide run that reports nothing; refusing to make the call and failing loudly
    // instead is the only useful behaviour. Returns false when the caller must stop.
    inline bool PredicateIsSound(FAutomationTestBase& Test)
    {
        const bool bRefusesDoubled = CanReachCreatePackageFatal(TEXT("/Game//A/B"));
        const bool bAcceptsClean = !CanReachCreatePackageFatal(TEXT("/Game/A/B"));
        Test.TestTrue(TEXT("CanReachCreatePackageFatal flags a doubled slash"), bRefusesDoubled);
        Test.TestTrue(TEXT("CanReachCreatePackageFatal passes a clean path"), bAcceptsClean);
        if (!bRefusesDoubled)
        {
            Test.AddError(TEXT("Refusing to drive the resolvers: CanReachCreatePackageFatal no "
                               "longer flags '//', so every probe below would reach CreatePackage's "
                               "Fatal and end this process instead of failing."));
        }
        return bRefusesDoubled && bAcceptsClean;
    }
}

// ============================================================================
// ResolveUClass - the highest-leverage guard in the plugin (~56 sites, >=45 verbs)
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyResolveUClassTest,
    "PinWright.core.resolver_path_safety.ResolveUClassRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyResolveUClassTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    // Exactly one: the single refused call below must log, and neither control may.
    AddExpectedMessagePlain(TEXT("ResolveUClass refused"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    TestNull(TEXT("a class reference containing '//' is refused"),
        ResolveUClass(DoubledSlashPath(TEXT("Cls"))));

    // CONTROLS. Without these the assertion above is satisfied by a guard that refuses
    // everything - which would break >=45 verbs while looking green. Both shapes are the ones the
    // sibling tests in Tests/Core/TestClassUtils.cpp already prove resolve, so a failure here is
    // unambiguously this guard and not a host difference.
    TestNotNull(TEXT("a bare short name still resolves"),
        ResolveUClass(TEXT("StaticMeshComponent")));
    TestNotNull(TEXT("a /Script/ object path - which IsValidLongPackageName would have refused "
                     "for its '.' - still resolves"),
        ResolveUClass(TEXT("/Script/Engine.CameraActor")));

    return true;
}

// ============================================================================
// ResolveUEnum - 12 sites
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyResolveUEnumTest,
    "PinWright.core.resolver_path_safety.ResolveUEnumRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyResolveUEnumTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    AddExpectedMessagePlain(TEXT("ResolveUEnum refused"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    // The short-name controls below go through FindFirstObjectSafe, whose ambiguity path logs at
    // Warning by default (StaticFindFirstObjectSafe's AmbiguousMessageVerbosity parameter,
    // UObjectGlobals.cpp:863) - and a captured Warning is elevated to a test error. The names used
    // are unique today, so this is declared with a NEGATIVE occurrence count: tolerated if a host's
    // loaded set makes one ambiguous, never required.
    AddExpectedMessagePlain(TEXT("StaticFindFirstObject: Ambiguous object name"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    // Carries a '.', so it clears the `Contains(".")` gate the lethal LoadObject sits behind -
    // which is the point: that gate is not a guard, it is the door.
    TestNull(TEXT("an enum reference containing '//' is refused"),
        ResolveUEnum(FString::Printf(TEXT("/Script//Engine.%s"), *UniqueLeaf(TEXT("Enum")))));

    // CONTROLS. ECollisionChannel is a UENUM in core Engine (EngineTypes.h:1096-1098), so neither
    // needs a plugin, an asset or the registry. The dotted form is the one that matters most here:
    // it is the shape that reaches the lethal LoadObject, so a guard that over-blocked would show
    // up on it first.
    TestNotNull(TEXT("a dotted enum path still resolves"),
        ResolveUEnum(TEXT("/Script/Engine.ECollisionChannel")));
    TestNotNull(TEXT("a bare enum short name still resolves"),
        ResolveUEnum(TEXT("ECollisionChannel")));

    return true;
}

// ============================================================================
// ResolveUScriptStruct - 9 sites
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyResolveUScriptStructTest,
    "PinWright.core.resolver_path_safety.ResolveUScriptStructRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyResolveUScriptStructTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    AddExpectedMessagePlain(TEXT("ResolveUScriptStruct refused"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    // The short-name controls below go through FindFirstObjectSafe, whose ambiguity path logs at
    // Warning by default (StaticFindFirstObjectSafe's AmbiguousMessageVerbosity parameter,
    // UObjectGlobals.cpp:863) - and a captured Warning is elevated to a test error. The names used
    // are unique today, so this is declared with a NEGATIVE occurrence count: tolerated if a host's
    // loaded set makes one ambiguous, never required.
    AddExpectedMessagePlain(TEXT("StaticFindFirstObject: Ambiguous object name"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    TestNull(TEXT("a struct reference containing '//' is refused"),
        ResolveUScriptStruct(
            FString::Printf(TEXT("/Script//CoreUObject.%s"), *UniqueLeaf(TEXT("Struct")))));

    // CONTROLS - the bare form and the F-prefixed form, so a guard that swallowed the tier-3
    // prefix strip would be visible.
    TestNotNull(TEXT("a bare struct short name still resolves"),
        ResolveUScriptStruct(TEXT("Vector")));
    TestNotNull(TEXT("an F-prefixed struct short name still resolves"),
        ResolveUScriptStruct(TEXT("FVector")));

    return true;
}

// ============================================================================
// ResolveClassByName - 19 sites
//
// This one has no lethal load TODAY: UEditorAssetLibrary::LoadAsset collapses "//" through
// FPaths::RemoveDuplicateSlashes (EditorScriptingHelpers.cpp:71) before resolving anything, and
// the one raw LoadObject in the function is reached only for an input containing no '/' at all.
// The guard is here because that safety is an engine-internal routing detail of a third-party
// helper rather than a property of this function, and this test is what makes the refusal a
// contract instead of an accident.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyResolveClassByNameTest,
    "PinWright.core.resolver_path_safety.ResolveClassByNameRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyResolveClassByNameTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    AddExpectedMessagePlain(TEXT("ResolveClassByName refused"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    TestNull(TEXT("a class reference containing '//' is refused"),
        ResolveClassByName(DoubledSlashPath(TEXT("ByName"))));

    // CONTROL.
    TestNotNull(TEXT("a bare short name still resolves"),
        ResolveClassByName(TEXT("StaticMeshComponent")));

    return true;
}

// ============================================================================
// LoadBlueprintAsset - ~53 sites, and the site with the anti-guard
//
// This function HAS an error channel, so the refusal is asserted on OutError rather than on a log
// line. The message deliberately names "//" so support can grep it apart from the
// "Blueprint asset not found" miss, which the second half of this test pins.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyLoadBlueprintAssetTest,
    "PinWright.core.resolver_path_safety.LoadBlueprintAssetRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyLoadBlueprintAssetTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    {
        // Pre-seeded so "left untouched" cannot masquerade as "cleared".
        FString Normalized = TEXT("/Game/Stale/Leftover");
        FString Error = TEXT("stale");
        const FString Bad = DoubledSlashPath(TEXT("Bp"));

        TestNull(TEXT("a blueprint path containing '//' is refused"),
            LoadBlueprintAsset(Bad, Normalized, Error));
        TestTrue(TEXT("the refusal message names the '//' rule"), Error.Contains(TEXT("//")));
        TestTrue(TEXT("the refusal message quotes the caller's raw argument"),
            Error.Contains(Bad));
        TestTrue(TEXT("the normalized out-parameter is cleared on refusal"),
            Normalized.IsEmpty());
    }

    // CONTROL, and the reason the message wording above matters. A well-formed path naming nothing
    // must still reach the ordinary miss - proving the guard did not simply refuse everything AND
    // that the two verdicts are distinguishable by their text, which is the whole point of giving
    // this refusal its own wording.
    {
        FString Normalized = TEXT("/Game/Stale/Leftover");
        FString Error;
        const FString Missing =
            FString::Printf(TEXT("/Game/PinWrightTests/%s"), *UniqueLeaf(TEXT("BpMiss")));

        TestNull(TEXT("a well-formed path naming nothing still misses"),
            LoadBlueprintAsset(Missing, Normalized, Error));
        TestTrue(TEXT("the miss reports 'not found'"), Error.Contains(TEXT("not found")));
        TestFalse(TEXT("the miss is NOT reported as a '//' refusal"),
            Error.Contains(TEXT("'//'")));
    }

    return true;
}

// ============================================================================
// ResolveUObjectByPath - 5 sites
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyResolveUObjectByPathTest,
    "PinWright.core.resolver_path_safety.ResolveUObjectByPathRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyResolveUObjectByPathTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    {
        FString Error;
        const FString Bad = DoubledSlashPath(TEXT("Obj"));
        TestNull(TEXT("an object path containing '//' is refused"),
            ResolveUObjectByPath(Bad, Error));
        TestTrue(TEXT("the refusal message names the '//' rule"), Error.Contains(TEXT("//")));
    }

    // CONTROL - an object path that resolves, and one that misses with the ordinary verdict.
    {
        FString Error;
        TestNotNull(TEXT("a /Script/ object path still resolves"),
            ResolveUObjectByPath(TEXT("/Script/Engine.CameraActor"), Error));
    }
    {
        // DELIBERATELY A FIXED NAME, not a GUID leaf like everything else in this file. A
        // well-formed path that names nothing still reaches StaticLoadObject, and that call
        // CREATES the empty package on its way to failing (ResolveName2 with Create=true,
        // UObjectGlobals.cpp:1310) - harmless, but a GUID leaf would leave one more empty /Game
        // package in the editor's memory on every run. A fixed reserved name reuses the same one,
        // and if somebody ever creates an asset there this test goes red rather than quiet.
        FString Error;
        const FString Missing = TEXT("/Game/PinWrightTests/PWResolverSafety_DoesNotExist");
        TestNull(TEXT("a well-formed path naming nothing still misses"),
            ResolveUObjectByPath(Missing, Error));
        TestTrue(TEXT("the miss reports 'not found'"), Error.Contains(TEXT("not found")));
        TestFalse(TEXT("the miss is NOT reported as a '//' refusal"),
            Error.Contains(TEXT("'//'")));
    }

    return true;
}

// ============================================================================
// NormalizeAssetPath no longer re-roots - the data-loss half of this change
//
// The deleted fallback threw away the caller's folder chain, kept the leaf, retried it under
// /Game/, /Engine/ and /Script/, and returned bIsValid=true naming a DIFFERENT PACKAGE with
// nothing in FNormalizedAssetPath recording the substitution. sequencer.export_anim_sequence pairs
// this function with `overwrite`, so that result named an asset the caller never asked for and the
// export rewrote it. The fallback's own DoesPackageExist test made this worse rather than safer: it
// only ever returned a path when the substitute package genuinely EXISTED, i.e. only when there was
// something real to clobber.
//
// Nothing here calls a loader, so this test carries none of the lethality caveat in the file
// header.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolverPathSafetyNormalizeAssetPathTest,
    "PinWright.core.resolver_path_safety.NormalizeAssetPathNeverReRoots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolverPathSafetyNormalizeAssetPathTest::RunTest(const FString& Parameters)
{
    using namespace ResolverPathSafetyHelpers;

    // WHAT MUST KEEP WORKING. The /Game/ prepend for a bare name happens BEFORE the validity
    // check, so it never depended on the fallback and is unaffected by its removal. This is the
    // case a reader will worry about, so it is asserted first.
    {
        const FNormalizedAssetPath Bare = NormalizeAssetPath(TEXT("MyMesh"));
        TestTrue(TEXT("a bare name is still accepted"), Bare.bIsValid);
        TestEqual(TEXT("a bare name is still rooted under /Game/"),
            Bare.Path, FString(TEXT("/Game/MyMesh")));
    }
    {
        const FNormalizedAssetPath Rooted = NormalizeAssetPath(TEXT("/Game/A/B"));
        TestTrue(TEXT("an ordinary rooted path is still accepted"), Rooted.bIsValid);
        TestEqual(TEXT("an ordinary rooted path is returned unchanged"),
            Rooted.Path, FString(TEXT("/Game/A/B")));
    }
    {
        // The object-path strip and the trailing-slash trim are also above the validity check.
        const FNormalizedAssetPath Obj = NormalizeAssetPath(TEXT("/Game/A/B.B"));
        TestTrue(TEXT("an object path is still accepted"), Obj.bIsValid);
        TestEqual(TEXT("an object path still collapses to its package"),
            Obj.Path, FString(TEXT("/Game/A/B")));
    }

    // WHAT MUST NOW BE REFUSED. IsValidLongPackageName rejects "//" outright
    // (PackageName.cpp:1701-1705, LongPackageNames_PathWithDoubleSlash), so with the fallback gone
    // there is no second chance for these.
    {
        const FString Doubled =
            FString::Printf(TEXT("/Game//PinWrightTests/%s"), *UniqueLeaf(TEXT("Norm")));
        const FNormalizedAssetPath Result = NormalizeAssetPath(Doubled);
        TestFalse(TEXT("a path containing '//' is refused"), Result.bIsValid);
        TestFalse(TEXT("the refusal carries an explanation"), Result.ErrorMessage.IsEmpty());
    }
    {
        const FString Unmounted = FString::Printf(TEXT("/%s/Sub/%s"),
            *UniqueLeaf(TEXT("Root")), *UniqueLeaf(TEXT("Leaf")));
        const FNormalizedAssetPath Result = NormalizeAssetPath(Unmounted);
        TestFalse(TEXT("a path under an unmounted root is refused"), Result.bIsValid);
    }

    // THE DISCRIMINATING CASE - the only shape that behaves differently before and after this
    // change, and the one the data loss came from. It needs a package sitting DIRECTLY under
    // /Game/ (the fallback only ever retried the bare leaf, never the folder chain), so the leaf
    // is discovered from the asset registry rather than hardcoded, and the case is skipped with
    // the wire marker on a host that has none.
    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> TopLevelAssets;
    AssetRegistry.GetAssetsByPath(FName(TEXT("/Game")), TopLevelAssets, /*bRecursive=*/false);

    FString ExistingTopLevelPackage;
    for (const FAssetData& Asset : TopLevelAssets)
    {
        const FString PackageName = Asset.PackageName.ToString();
        if (FPackageName::DoesPackageExist(PackageName))
        {
            ExistingTopLevelPackage = PackageName;
            break;
        }
    }

    if (ExistingTopLevelPackage.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_top_level_game_package"),
            FString::Printf(
                TEXT("The re-rooting cases need a package directly under /Game/; the registry "
                     "reported %d non-recursive rows there and none existed on disk."),
                TopLevelAssets.Num()));
        return true;
    }

    const FString ExistingLeaf = FPackageName::GetShortName(ExistingTopLevelPackage);

    {
        // Before the fix this returned bIsValid=true naming ExistingTopLevelPackage - a package the
        // caller never mentioned, reachable for overwrite.
        const FString Foreign = FString::Printf(TEXT("/%s/Sub/%s"),
            *UniqueLeaf(TEXT("Root")), *ExistingLeaf);
        const FNormalizedAssetPath Result = NormalizeAssetPath(Foreign);
        TestFalse(TEXT("an unmounted root is NOT re-rooted onto an existing /Game/ package"),
            Result.bIsValid);
        TestNotEqual(TEXT("the result does not name the substitute package"),
            Result.Path, ExistingTopLevelPackage);
    }
    {
        // The laundering case: a lethal input coming back as a valid, "//"-free, different path.
        const FString Doubled = FString::Printf(TEXT("/Game//Sub/%s"), *ExistingLeaf);
        const FNormalizedAssetPath Result = NormalizeAssetPath(Doubled);
        TestFalse(TEXT("a '//' input is NOT laundered into a valid different path"),
            Result.bIsValid);
        TestNotEqual(TEXT("the result does not name the substitute package"),
            Result.Path, ExistingTopLevelPackage);
    }

    return true;
}
