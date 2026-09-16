// Copyright (c) 2026 Alexander Penkin. MIT License.

// The suite-start half of the host-content containment pair whose other half is the
// suite-end scratch-root gate in TestSuiteScratchRootHygiene.cpp.
//
// WHAT IT IS FOR. UE automation runs against whatever map the host project opens at editor
// startup (`EditorStartupMap`), so the world every test spawns into, edits, dirties and
// occasionally saves IS a shipped host asset. FScopedForeignDirtyPackageSuspension scopes the
// one verb that flushes everything (`editor.save_all`) to the fixture scratch root, and
// FScopedEditorWorldActorGuard puts back what a spawn test changed - but both are opt-in per
// call site, so a test that dirties the world without reaching for either still leaves the
// host's startup map modified, and anything that saves it writes into host content. This step
// removes the target instead of guarding each path to it: the world the rest of the suite runs
// against is a brand-new UNTITLED world under the read-only `/Temp/` mount, which has no
// package on disk and is not part of the host's content tree at all. An unguarded world edit
// then has nothing host-owned to reach.
//
// WHY THE ID STARTS WITH "aa_". Same mechanism as the "zz_" gate, in the other direction. The
// automation controller sorts the whole batch by display name before inserting it into the
// report tree (AutomationControllerManager.cpp:1042-1051) and executes leaves in that order;
// FString::operator< is a case-insensitive compare, so an "aa_" second segment sorts ahead of
// every other `PinWright.*` second segment in the tree (the earliest today is "actor", and
// "aa" < "ac"). Rename it into a later namespace and the swap still happens, but only after
// the tests it was supposed to protect have already run against the host's map.
//
// WHY GEditor->NewMap AND NOT CreateNewMapForEditing. NewMap (EditorServer.cpp:2187) creates
// the world and nothing else. CreateNewMapForEditing (:2145) wraps it in
// FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, ...) at :2158, which is exactly the
// "save the map you are leaving" step this file must never take: under -unattended the modal
// is auto-answered rather than declined, so the prompting form could write the host's startup
// map to disk on its way out. NewMap is the non-prompting form, and it is what `level.create`
// and `lighting.create_lighting_enabled_level` already call, so the swap here shares their
// path rather than inventing a second one.
//
// THE PRE-SWAP PROBE IS NOT OPTIONAL. NewMap reaches EditorDestroyWorld -> Cleanse ->
// CheckForWorldGCLeaks, which is Fatal by default on any dead world still resident after the
// cleanse; that is why every map-swapping verb runs ProbeResidentWorldSurvivors first
// (Utils/MapSwapDirtyWorldGuard.h). Being the first test in the run does not make this one
// exempt, so it runs the same probe and DECLINES the swap - visibly, through the skip marker -
// rather than handing the engine a swap the guard would have refused. Its cost here is the
// documented fast path: at suite start no world is counted, so the probe iterates worlds once
// and runs no collect, and it runs before any fixture exists that an unscheduled collect could
// take (the constraint TestMapSwapWorldSurvivorProbe.cpp is written around).

#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/MapSwapDirtyWorldGuard.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"

#if WITH_AUTOMATION_TESTS

// Named namespace, not anonymous: with bUseUnity=true an anonymous-namespace helper becomes
// visible at global scope once Unity merges this TU with its neighbours (see TestAssetTeardown.h).
namespace PinWrightSuiteStartBlankWorld
{
    // The two spellings of "this world cannot be written into host content". NewMap builds its
    // package with CreatePackage(nullptr) (EditorServer.cpp:2215), and an outerless package with
    // no name is named `/Temp/Untitled_<N>` by MakeUniqueObjectName (UObjectGlobals.cpp:2669);
    // `/Temp/` is a READ-ONLY mount point rooted at <Project>/Saved (PackageName.cpp:904), so
    // nothing under it resolves into the host's Content tree. The RF_Transient arm covers a host
    // whose editor world came from somewhere other than NewMap.
    inline bool IsUntitledOrTransientWorldPackage(const UPackage* Package)
    {
        return Package != nullptr
            && (FPackageName::IsTempPackage(Package->GetName())
                || Package->HasAnyFlags(RF_Transient));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSuiteStartOpenBlankTransientWorldTest,
    "PinWright.aa_suite_start.OpenBlankTransientWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuiteStartOpenBlankTransientWorldTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    UWorld* const StartupWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (StartupWorld == nullptr)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is loaded (a commandlet host without an editor), so there is "
                 "no host startup map for the rest of the suite to reach and nothing to swap."));
        return true;
    }

    const UPackage* const StartupPackage = StartupWorld->GetOutermost();
    const FString StartupPackageName =
        StartupPackage != nullptr ? StartupPackage->GetName() : FString();
    // On the record because it names the asset every later test would otherwise have been
    // editing: a run that fails downstream needs to know which map it started on.
    AddInfo(FString::Printf(
        TEXT("Editor world at suite start: '%s'. Swapping it out for a blank untitled world."),
        *StartupPackageName));

    const PinWrightMapSwapGuard::FWorldSurvivorProbeResult Probe =
        PinWrightMapSwapGuard::ProbeResidentWorldSurvivors(
            FString(), /*bTransactionBufferWillBeCleared=*/false);
    if (Probe.bProbeUnavailable || Probe.IsBlocked())
    {
        const FString Reason = Probe.bProbeUnavailable
            ? Probe.UnavailableReason
            : PinWrightMapSwapGuard::DescribeSurvivorRefusal(Probe);
        // Refusing is the guard working, not the test passing: the rest of the run stays on the
        // host's startup map, which is the exposure this step exists to remove, so it is
        // reported as a skipped assertion rather than swallowed.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("map-swap-refused"),
            FString::Printf(
                TEXT("The shared pre-swap guard would not clear a NewMap right now, so the suite "
                     "keeps running against '%s': %s"),
                *StartupPackageName, *Reason));
        return true;
    }

    UWorld* const BlankWorld = GEditor->NewMap(/*bIsPartitionedWorld=*/false);
    if (!TestNotNull(TEXT("GEditor->NewMap returned a world for the suite to run against"),
            BlankWorld))
    {
        return false;
    }
    // NewMap already sets it, and level.create re-asserts it the same way; the editor world
    // context is what every later test reads, so it is what this test measures.
    GEditor->GetEditorWorldContext().SetCurrentWorld(BlankWorld);

    UWorld* const ActiveWorld = GEditor->GetEditorWorldContext().World();
    if (!TestTrue(TEXT("the blank world is the active editor world"), ActiveWorld == BlankWorld))
    {
        return false;
    }

    UPackage* const ActivePackage = ActiveWorld->GetOutermost();
    const FString ActivePackageName =
        ActivePackage != nullptr ? ActivePackage->GetName() : FString();

    TestTrue(
        *FString::Printf(
            TEXT("the editor world the suite will run against is untitled or transient, so a "
                 "test that dirties it cannot persist anything into host content (package is "
                 "'%s')"),
            *ActivePackageName),
        PinWrightSuiteStartBlankWorld::IsUntitledOrTransientWorldPackage(ActivePackage));
    TestTrue(
        TEXT("the blank world's package is PKG_NewlyCreated, i.e. it has never been written to "
             "disk and has no file for a save to overwrite"),
        ActivePackage != nullptr && ActivePackage->HasAnyPackageFlags(PKG_NewlyCreated));
    TestNotEqual(
        TEXT("the editor world is no longer the host's startup map"),
        ActivePackageName, StartupPackageName);
    return true;
}

#endif
