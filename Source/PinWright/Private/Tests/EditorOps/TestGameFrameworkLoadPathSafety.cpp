// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGameFrameworkLoadPathSafety.cpp - regression coverage for the two load chokepoints of
// B-createpackage-unvalidated-paths-plugin-wide that carry no CreatePackage call of their own:
// GameFrameworkHelpers::LoadClassFromPath and the shared PinWrightBlueprintPathLoad::
// LoadBlueprintFromPath (one body, reached from game_framework.* and networking.* alike).
//
// WHAT WAS WRONG, AND WHY IT IS EASY TO MISS. Neither function calls CreatePackage, so neither
// showed up in a sweep for it - and both end the editor PROCESS on a caller string containing "//".
// A load is the same door: StaticLoadObjectInternal calls ResolveName2(..., Create=true)
// (UObjectGlobals.cpp:1427), which calls CreatePackage on the partial name (:1310), and
// CreatePackage logs a "//" at **Fatal** (:1094-1096) - not compiled out in any configuration.
// LoadClassFromPath reaches it through LoadClass<UObject> -> StaticLoadClass -> StaticLoadObject,
// and it appends "_C" to the caller's text first, which preserves any "//" the text carried.
// LoadBlueprintFromPath reaches it through StaticLoadObject directly. Neither can be defended by a
// null check at a call site, because nothing after the call runs.
//
// THE REACHABLE SURFACE IS LIVE. LoadClassFromPath is called from
// game_framework.configure_spectating (spectatorClass, read with a raw Ctx.GetString) and from
// every verb the CREATE_GF_BP_HANDLER macro generates (parentClass); LoadBlueprintFromPath has 29
// call sites across the two handler files. That is why the guards sit inside the two shared
// functions rather than at the call sites: 29 places to remember is 29 places to forget.
//
// WHY THESE ARE DIRECT CALLS AND NOT VERB DRIVES. Both guards are exactly Contains("//") - nothing
// wider is correct over inputs that legitimately include a bare class name, "/Script/Engine.X",
// "/Game/A/BP_X.BP_X_C" and a plugin mount (see CanReachCreatePackageFatal in Utils/PathUtils.h).
// No non-lethal payload discriminates a build with the guard from one without, so these assert the
// post-fix CONTRACT and are not reproductions: on a reverted build the "//" cases end the process
// instead of going red. Driving them through a verb would add the dispatcher and the verb's other
// unguarded arguments to the crash surface and add no coverage.
//
// EACH REFUSAL IS PAIRED WITH A CONTROL, and the control is the load-bearing half here: refusal and
// not-found are the same nullptr, so only an input that RESOLVES proves the guard did not swallow
// the function whole.
#include "Misc/AutomationTest.h"
#include "Handlers/Blueprint/BlueprintPathLoad.h"
#include "Tests/TestSkipReporting.h"

#include "Engine/Blueprint.h"
#include "Templates/Function.h"
#include "UObject/Class.h"

// GameFrameworkHelpers::LoadClassFromPath is defined in Handlers/Systems/GameFrameworkHandler.cpp
// and has no header - it is a handler-local shared helper, not a published utility. It has external
// linkage (a named namespace, not `static`), so declaring the one signature here is what lets this
// test call it without promoting it to a public API it has no other reason to be.
namespace GameFrameworkHelpers
{
    UClass* LoadClassFromPath(const FString& ClassPath);
}

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace GameFrameworkLoadPathSafetyHelpers
{
    // Every shape these resolvers accept, each carrying the one lethal property. The bare-name case
    // has no leading slash and no dot and is still a kill: ResolveName2 returns early with no
    // delimiter, and StaticLoadObjectInternal re-enters itself with InName + "." +
    // GetShortName(InName), so the second pass reaches CreatePackage.
    inline void ForEachLethalPath(TFunctionRef<void(const TCHAR*)> Visit)
    {
        const TCHAR* LethalPaths[] = {
            TEXT("/Game//PinWrightMissing/BP_LoadPathSafety"),
            TEXT("/Game/PinWrightMissing//BP_LoadPathSafety"),
            TEXT("/Game//PinWrightMissing/BP_LoadPathSafety.BP_LoadPathSafety"),
            TEXT("PinWrightMissing//BP_LoadPathSafety"),
        };
        for (const TCHAR* LethalPath : LethalPaths)
        {
            Visit(LethalPath);
        }
    }
}

// ============================================================================
// GameFrameworkHelpers::LoadClassFromPath refuses a "//" class path before LoadClass
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkLoadClassFromPathRefusesDoubleSlashTest,
    "PinWright.game_framework.load_class_from_path.RefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkLoadClassFromPathRefusesDoubleSlashTest::RunTest(const FString& Parameters)
{
    using namespace GameFrameworkLoadPathSafetyHelpers;

    ForEachLethalPath([this](const TCHAR* LethalPath)
    {
        TestNull(*FString::Printf(TEXT("'%s' is refused before LoadClass"), LethalPath),
            GameFrameworkHelpers::LoadClassFromPath(LethalPath));
    });

    // CONTROL. A native class path must still resolve. This one is answered by the FindObject
    // directly below the guard, so it also witnesses that the guard was inserted ABOVE that
    // resolution rather than in place of it. Host-independent: APointLight is core engine.
    TestNotNull(TEXT("a native class path still resolves"),
        GameFrameworkHelpers::LoadClassFromPath(TEXT("/Script/Engine.PointLight")));

    // CONTROL. A well-formed path that names nothing must still return nullptr rather than be
    // treated as an error - the function's contract is one failure shape, and the guard did not
    // change it.
    TestNull(TEXT("a well-formed path naming no class still returns null"),
        GameFrameworkHelpers::LoadClassFromPath(
            TEXT("/Game/PinWrightMissing/BP_AbsentSpectatorProbe")));

    // ORDERING WITNESS. The empty check the guard was placed beside must still fire.
    TestNull(TEXT("an empty class path is still refused"),
        GameFrameworkHelpers::LoadClassFromPath(FString()));

    return true;
}

// ============================================================================
// The unified blueprint loader refuses a "//" path before StaticLoadObject
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintPathLoadRefusesDoubleSlashTest,
    "PinWright.blueprint.path_load.RefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintPathLoadRefusesDoubleSlashTest::RunTest(const FString& Parameters)
{
    using namespace GameFrameworkLoadPathSafetyHelpers;

    ForEachLethalPath([this](const TCHAR* LethalPath)
    {
        TestNull(*FString::Printf(TEXT("'%s' is refused before StaticLoadObject"), LethalPath),
            PinWrightBlueprintPathLoad::LoadBlueprintFromPath(LethalPath));
    });

    // ORDERING WITNESS. The pre-existing "_C" rejection must still fire; the guard was added above
    // it, not in place of it.
    TestNull(TEXT("a generated-class path is still rejected"),
        PinWrightBlueprintPathLoad::LoadBlueprintFromPath(
            TEXT("/Game/PinWrightMissing/BP_LoadPathSafety.BP_LoadPathSafety_C")));

    // CONTROL, and it has to be a REAL asset: refusal and not-found are the same nullptr, so only a
    // load that SUCCEEDS proves the guard did not swallow the function. StandardMacros is engine
    // content that ships with every install, but a host that has it unavailable would otherwise
    // turn this into a false red, so its absence is reported as a skip rather than a failure - a
    // silent early return here would leave the run counting a pass that measured nothing.
    {
        const TCHAR* ControlPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
        UBlueprint* Loaded = PinWrightBlueprintPathLoad::LoadBlueprintFromPath(ControlPath);
        if (Loaded)
        {
            TestEqual(TEXT("the control loads the blueprint that was asked for"),
                Loaded->GetPathName(), FString(ControlPath));
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-blueprint-fixture-absent"),
                FString::Printf(
                    TEXT("'%s' did not load on this host, so the over-refusal control - the only "
                         "case that distinguishes the guard from a blanket refusal - did not run."),
                    ControlPath));
        }
    }

    return true;
}
