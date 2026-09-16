// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGuardedLoadPathSafety.cpp - the refusal contract of PinWrightGuardedLoad
// (Utils/GuardedLoad.h), the point-of-use guard behind two board tickets the dispatch-boundary
// type gate structurally cannot reach:
//
//   * B-nested-path-values-reach-createpackage-fatal - a path inside an array element
//     (`foliageTypes[].meshPath`, `nodes[].texturePath`, `stems[].assetPath`,
//     `captures[].attribute`) or in the VALUE half of a map (`texture: {ParamName: AssetPath}`).
//     The gate reads TOP-LEVEL params only, and FParamSpec::NestedKeys is an untyped allow-list of
//     KEYS, so on the map shape there is not even a key to hang a rule on.
//   * B-ir-source-class-refs-reach-createpackage-fatal - a class or asset reference that is a
//     SUBSTRING of the `text` parameter. `text` is an IR document and must stay typed `string`;
//     the reference is produced by the parser mid-compile, and no IR tokenizer filters '/' (the
//     comment character is '#', and Unquote returns its input verbatim).
//
// WHY THIS FILE TESTS A HELPER AND NOT THE VERBS. CreatePackage logs at Fatal for a name
// containing "//" (UObjectGlobals.cpp:1094-1096), which is not compiled out in any configuration:
// a test that drove the defect would END the suite host rather than report a red, and a suite that
// dies mid-queue is an ABSENCE of a signal, not a failure one. Every reacher was therefore
// converted to ONE guard, and this file locks that guard's contract directly. Which call sites go
// through it is asserted separately, by the source scan in
// Tests/Infra/TestIrAndNestedLoadGuard.cpp - the two files together are the claim "the class is
// closed"; either alone is only half of it.
//
// THE ACCEPT CASES ARE NOT DECORATION. A guard that refused everything would close the class and
// break every verb that uses it, and no response-shape test elsewhere discriminates a refusal from
// a not-found (both are nullptr). The shapes below - a bare short name, a package path, an object
// path, a subobject path, a generated-class `_C` path, a plugin mount - are exactly the ones
// CanReachCreatePackageFatal documents as legal, and a build that starts refusing one of them
// fails here.
//
// EVERY REFUSAL LOGS AT WARNING, so each refusing test declares the message with an exact
// occurrence count. That makes the suppression an assertion in two directions:
// bElevateLogWarningsToErrors turns an undeclared captured Warning into a failure, an unmet
// expectation is also a failure, and an accept case that wrongly refuses overshoots the count.

#include "Misc/AutomationTest.h"

#include "Utils/GuardedLoad.h"
#include "Utils/PathUtils.h"

#include "Engine/Blueprint.h"
#include "Misc/Guid.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace GuardedLoadPathSafetyHelpers
{
    // A fresh GUID leaf on every probe. A fixed leaf could collide with a real package on some
    // host and turn a refusal case into a resolution, which would then pass on one machine and
    // fail on another.
    inline FString UniqueLeaf(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWGuardedLoad_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // THE PRE-FLIGHT THAT MAKES THE ACCEPT CASES SAFE TO ATTEMPT. Every accept case below hands
    // LoadObjectChecked a string that ends the PROCESS if the underlying predicate has been broken
    // to answer false on a doubled slash. Refusing to make the call and failing loudly instead is
    // the only useful behaviour when that happens.
    inline bool PredicateIsSound(FAutomationTestBase& Test)
    {
        const bool bRefusesDoubled = CanReachCreatePackageFatal(TEXT("/Game//A/B"));
        const bool bAcceptsClean = !CanReachCreatePackageFatal(TEXT("/Game/A/B"));
        Test.TestTrue(TEXT("CanReachCreatePackageFatal flags a doubled slash"), bRefusesDoubled);
        Test.TestTrue(TEXT("CanReachCreatePackageFatal passes a clean path"), bAcceptsClean);
        if (!bRefusesDoubled || !bAcceptsClean)
        {
            Test.AddError(TEXT("Refusing to drive LoadObjectChecked: CanReachCreatePackageFatal no "
                               "longer implements the rule, so a load below would reach "
                               "CreatePackage's Fatal and end this process instead of failing."));
            return false;
        }
        return true;
    }
}

// ============================================================================
// The predicate half: IsRefused
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuardedLoadRefusesDoubledSlashTest,
    "PinWright.core.path.guarded_load.RefusesDoubledSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGuardedLoadRefusesDoubledSlashTest::RunTest(const FString& Parameters)
{
    using namespace GuardedLoadPathSafetyHelpers;

    // One per refused input below. Each must log; none of the accept cases in the sibling test
    // may, which is what an exact count buys over a bare suppression.
    AddExpectedMessagePlain(TEXT("PinWright refused a load"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 6);

    // Every input shape a nested value or an IR token can actually arrive in. The doubled slash is
    // placed differently in each on purpose: the rule is a substring test, not a prefix test, and
    // a "leading // only" implementation would pass four of these six.
    const TArray<FString> Refused = {
        // The repro on the IR ticket: blend_space `BS` class=/Game//X.X_C
        FString::Printf(TEXT("/Game//%s.%s_C"), *UniqueLeaf(TEXT("Cls")), *UniqueLeaf(TEXT("Cls"))),
        // foliageTypes[].meshPath / nodes[].texturePath shape.
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(TEXT("Mesh"))),
        // The map-VALUE shape, texture: {ParamName: AssetPath}.
        FString::Printf(TEXT("//Game/PinWrightTests/%s"), *UniqueLeaf(TEXT("Tex"))),
        // A doubled slash in the middle of a plugin mount.
        FString::Printf(TEXT("/PinWrightNoSuchPlugin/A//%s"), *UniqueLeaf(TEXT("Plugin"))),
        // NO LEADING SLASH AND NO DOT, and still lethal: StaticLoadObjectInternal re-enters itself
        // with InName + "." + GetShortName(InName), and the second pass reaches ResolveName2:1310.
        FString::Printf(TEXT("A//%s"), *UniqueLeaf(TEXT("Bare"))),
        // A subobject path carrying the sequence before the ':' delimiter.
        FString::Printf(TEXT("/Game/A//%s.%s:Comp"), *UniqueLeaf(TEXT("Sub")), *UniqueLeaf(TEXT("Sub"))),
    };

    for (const FString& Path : Refused)
    {
        FString Refusal;
        TestTrue(FString::Printf(TEXT("IsRefused('%s')"), *Path),
            PinWrightGuardedLoad::IsRefused(Path, &Refusal));

        // The message must QUOTE the value, so support can tell a refusal apart from a not-found
        // in a log, and must name the rule so the caller knows what to change.
        TestTrue(FString::Printf(TEXT("The refusal quotes '%s'"), *Path), Refusal.Contains(Path));
        TestTrue(TEXT("The refusal names the doubled-slash rule"), Refusal.Contains(TEXT("//")));
        TestTrue(TEXT("The refusal names CreatePackage"), Refusal.Contains(TEXT("CreatePackage")));
    }

    return true;
}

// ============================================================================
// The accept half - the guard is not "refuse everything"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuardedLoadAcceptsEveryLegalShapeTest,
    "PinWright.core.path.guarded_load.AcceptsEveryLegalShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGuardedLoadAcceptsEveryLegalShapeTest::RunTest(const FString& Parameters)
{
    using namespace GuardedLoadPathSafetyHelpers;

    // ZERO refusals expected: any occurrence of the refusal line here is a shape the guard started
    // turning away, and a captured undeclared Warning fails the test on its own.
    if (!PredicateIsSound(*this))
    {
        return false;
    }

    // Every shape CanReachCreatePackageFatal documents as legal. None of them names a real asset,
    // so each load misses - which is the point: the assertion is that the call was ATTEMPTED
    // (no refusal string) rather than turned away before it.
    const TArray<FString> Accepted = {
        UniqueLeaf(TEXT("ShortName")),                                            // bare short name
        FString::Printf(TEXT("/Game/PinWrightTests/%s"), *UniqueLeaf(TEXT("Pkg"))),
        FString::Printf(TEXT("/Game/PinWrightTests/%s.%s"),
            *UniqueLeaf(TEXT("Obj")), *UniqueLeaf(TEXT("Obj"))),                  // object path
        FString::Printf(TEXT("/Game/PinWrightTests/%s.%s:Comp"),
            *UniqueLeaf(TEXT("Sub")), *UniqueLeaf(TEXT("Sub"))),                  // subobject path
        FString::Printf(TEXT("/Game/PinWrightTests/%s.%s_C"),
            *UniqueLeaf(TEXT("Gen")), *UniqueLeaf(TEXT("Gen"))),                  // generated class
        FString::Printf(TEXT("/PinWrightNoSuchPlugin/A/%s"), *UniqueLeaf(TEXT("Mount"))),
    };

    for (const FString& Path : Accepted)
    {
        FString Refusal;
        TestFalse(FString::Printf(TEXT("IsRefused('%s') is false"), *Path),
            PinWrightGuardedLoad::IsRefused(Path, &Refusal));
        TestTrue(FString::Printf(TEXT("No refusal text for '%s'"), *Path), Refusal.IsEmpty());
    }

    return true;
}

// ============================================================================
// The load half - LoadObjectChecked never loads a refused path, and does load an accepted one
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuardedLoadObjectCheckedTest,
    "PinWright.core.path.guarded_load.LoadObjectCheckedShortCircuits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGuardedLoadObjectCheckedTest::RunTest(const FString& Parameters)
{
    using namespace GuardedLoadPathSafetyHelpers;

    AddExpectedMessagePlain(TEXT("PinWright refused a load"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 2);

    if (!PredicateIsSound(*this))
    {
        return false;
    }

    // Refused: nullptr, a filled OutRefusal, and - the property that matters - no load attempted.
    // There is no way to observe "did not call LoadObject" other than surviving this line: on a
    // build with the guard removed, the process ends here rather than reporting a red.
    {
        const FString Lethal =
            FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(TEXT("Lethal")));
        FString Refusal;
        UClass* Loaded = PinWrightGuardedLoad::LoadObjectChecked<UClass>(Lethal, &Refusal);
        TestNull(TEXT("A refused path loads nothing"), Loaded);
        TestFalse(TEXT("A refused path reports why"), Refusal.IsEmpty());
    }

    // OutRefusal is optional, and omitting it must not change the verdict - most call sites pass
    // nothing because they already have their own not-found message.
    {
        const FString Lethal =
            FString::Printf(TEXT("/Game/PinWrightTests//%s"), *UniqueLeaf(TEXT("NoOut")));
        TestNull(TEXT("A refused path loads nothing without an out-param"),
            PinWrightGuardedLoad::LoadObjectChecked<UBlueprint>(Lethal));
    }

    // Accepted: the call reaches the loader. The path names nothing, so nullptr is the right
    // answer - what is asserted is that OutRefusal stays EMPTY, i.e. the miss is a not-found and
    // not a refusal. Folding those two together is exactly the bug this out-param exists to avoid.
    {
        const FString Missing =
            FString::Printf(TEXT("/Game/PinWrightTests/%s"), *UniqueLeaf(TEXT("Missing")));
        FString Refusal;
        UObject* Loaded = PinWrightGuardedLoad::LoadObjectChecked<UObject>(
            Missing, &Refusal, LOAD_NoWarn | LOAD_Quiet);
        TestNull(TEXT("A well-formed path naming nothing still loads nothing"), Loaded);
        TestTrue(TEXT("...but reports no refusal"), Refusal.IsEmpty());
    }

    // A reference that always resolves, so the guard is shown not to break the case the call sites
    // exist for. A NATIVE class path is used rather than a content asset on purpose: it resolves
    // out of the loaded object graph on every host, with no dependency on which plugins, engine
    // content or cooked packages this build has - which is the only way this assertion means the
    // same thing on a developer box and on CI.
    {
        FString Refusal;
        UClass* ActorClass =
            PinWrightGuardedLoad::LoadObjectChecked<UClass>(TEXT("/Script/Engine.Actor"), &Refusal);
        TestNotNull(TEXT("A resolvable class reference still resolves through the guard"), ActorClass);
        TestTrue(TEXT("A successful load reports no refusal"), Refusal.IsEmpty());
    }

    return true;
}
