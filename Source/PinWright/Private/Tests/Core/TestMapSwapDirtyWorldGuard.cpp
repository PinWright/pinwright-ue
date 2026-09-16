// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-level-load-dirty-world-fatal /
// B-level-load-dirty-world-memory-leak-fatal: level.load handed
// FEditorFileUtils::LoadMap a map that was ALREADY resident and dirty, the engine could not
// unload that package, and UEditorEngine::Map_Load reached an unconditional Fatal
// ("World Memory Leaks") that killed the whole editor process and every agent session
// attached to it. The verb now probes the target package and refuses first.
//
// These tests deliberately do NOT drive level.load. Reproducing the defect end to end means
// reproducing the crash, and the whole point of the fix is that the condition is fatal — a
// test that reached LoadMap would take the suite down with it on the failing side. So the
// guard was split into a pure predicate (which branch of Map_Load a given state reaches) and
// a read-only probe (what the live state actually is), and both halves are asserted directly.
// The one part left unproven here is that LoadMap really does survive once the guard passes,
// which only a live map swap can show.
#include "Misc/AutomationTest.h"
#include "Utils/MapSwapDirtyWorldGuard.h"

#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// The state the shipped crash was in: the target map resident and dirty, holding an
// initialized world that carries RF_Standalone so it survives the collect Map_Load runs
// before its unload sweep. Every case below is this one with a single field flipped, which is
// what makes the table readable — the fields are near-synonymous booleans.
static PinWrightMapSwapGuard::FTargetWorldState MapSwapGuardTests_MakeShippedCrashState()
{
	PinWrightMapSwapGuard::FTargetWorldState State;
	State.PackageName = TEXT("/Game/Maps/L_Blocking");
	State.bPackageResident = true;
	State.bPackageDirty = true;
	State.bWorldFound = true;
	State.bWorldSurvivesEditorCollect = true;
	State.bWorldEverInitialized = true;
	State.bIsCurrentEditorWorld = false;
	return State;
}

// The decision table, one case per branch of UEditorEngine::Map_Load's leak check. Pure, so
// it needs neither a world nor a map on disk.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapGuardPredicateTest,
	"PinWright.core.map_swap_guard.PredicateMirrorsMapLoadReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapGuardPredicateTest::RunTest(const FString& Parameters)
{
	using namespace PinWrightMapSwapGuard;

	TestTrue(TEXT("resident + dirty + initialized world that survives GC -> fatal, must refuse"),
		WouldMapLoadFatal(MapSwapGuardTests_MakeShippedCrashState()));

	// Second fatal branch (ExistingPackage && !ExistingWorld): a dirty package with no world
	// in it defeats the targeted unload Map_Load tries for exactly that case.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bWorldFound = false;
		State.bWorldSurvivesEditorCollect = false;
		State.bWorldEverInitialized = false;
		TestTrue(TEXT("resident + dirty + no world in the package -> fatal, must refuse"),
			WouldMapLoadFatal(State));
	}

	// Not resident: there is nothing for UnloadPackages to refuse.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bPackageResident = false;
		TestFalse(TEXT("package not resident -> nothing to unload"), WouldMapLoadFatal(State));
	}

	// Clean: UnloadPackages takes it. It can still fail to GC when something holds a real
	// reference, which no flag predicts and this guard does not claim to cover.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bPackageDirty = false;
		TestFalse(TEXT("resident but clean -> UnloadPackages takes it"), WouldMapLoadFatal(State));
	}

	// The case that must NOT be refused, or level.create -> level.load breaks: an
	// uninitialized world is kept and reused by Map_Load, never unloaded, never leak-checked.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bWorldEverInitialized = false;
		TestFalse(TEXT("resident + dirty but the world was never initialized -> reused, not fatal"),
			WouldMapLoadFatal(State));
	}

	// Reachable through a world redirector whose destination is the live editor world:
	// EditorDestroyWorld strips that world's keep-flags itself, so dirty does not hold it.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bIsCurrentEditorWorld = true;
		TestFalse(TEXT("the resident world IS the current editor world -> EditorDestroyWorld path"),
			WouldMapLoadFatal(State));
	}

	// REGRESSION, and the reason this field exists. A verb that swaps the active world by hand
	// ends with UWorld::DestroyWorld, which unroots the previous world and clears its
	// RF_Standalone but runs NO collect (World.cpp:2792-2793) — level.structure.create_level is
	// the in-tree example, and the test harness's map-restore guard calls level.load right
	// afterwards. That world is resident, initialized and possibly dirty, yet completely
	// harmless: the next Map_Load's own CollectGarbage reclaims it before the leak check looks.
	// Refusing here strands the editor on a torn-down world and breaks every following test.
	{
		FTargetWorldState State = MapSwapGuardTests_MakeShippedCrashState();
		State.bWorldSurvivesEditorCollect = false;
		TestFalse(TEXT("garbage-in-waiting world (unrooted, no RF_Standalone) must NOT be refused"),
			WouldMapLoadFatal(State));
	}
	return true;
}

// The probe against a REAL resident package, which is the half a pure predicate cannot cover:
// it has to agree with the engine about which package is at stake and read its live dirty
// flag. A package with no world in it is the reproducible shape here (creating an initialized
// UWorld would need a map), and it is a genuine fatal branch rather than a stand-in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapGuardProbeResidentTest,
	"PinWright.core.map_swap_guard.ProbeReadsResidentDirtyPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapGuardProbeResidentTest::RunTest(const FString& Parameters)
{
	using namespace PinWrightMapSwapGuard;

	// GUID-suffixed so no real or previously-created asset can shadow the case.
	const FString PackageName = FString::Printf(TEXT("/Game/_Test/L_MapSwapGuard_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("created an in-memory package to probe"), Package))
	{
		return true;
	}
	// Transient so the package is collectable once this test is done with it; the dirty flag
	// is cleared explicitly below so the run does not leave the editor with a dirty package.
	Package->SetFlags(RF_Transient);

	// Clean first: resident, but nothing UnloadPackages would refuse.
	Package->SetDirtyFlag(false);
	FTargetWorldState Clean = ProbeTargetWorld(PackageName);
	TestTrue(TEXT("probe sees the live package"), Clean.bPackageResident);
	TestEqual(TEXT("probe reports the package it read"), Clean.PackageName, PackageName);
	TestFalse(TEXT("clean package is not reported dirty"), Clean.bPackageDirty);
	TestFalse(TEXT("a clean resident package does not block the swap"), WouldMapLoadFatal(Clean));

	// Dirty: this is the state that kills the editor if it reaches Map_Load.
	Package->SetDirtyFlag(true);
	FTargetWorldState Dirty = ProbeTargetWorld(PackageName);
	TestTrue(TEXT("probe sees the dirty flag"), Dirty.bPackageDirty);
	TestFalse(TEXT("no world lives in this package"), Dirty.bWorldFound);
	// The no-world branch is evaluated BEFORE the survives-collect one: a package with no
	// world reports bWorldSurvivesEditorCollect false (the field is about the world), and it
	// must still block, because Map_Load's targeted unload of a world-less package is defeated
	// by the same dirty flag.
	TestFalse(TEXT("survives-collect is meaningless with no world"),
		Dirty.bWorldSurvivesEditorCollect);
	TestTrue(TEXT("a resident dirty target must block the swap"), WouldMapLoadFatal(Dirty));

	// The refusal has to name the package the caller must act on, or it is not actionable.
	const FString Refusal = DescribeRefusal(Dirty, FString());
	TestTrue(TEXT("refusal names the blocking package"), Refusal.Contains(PackageName));

	// level.load hands LoadMap a .umap FILENAME, not a package path. The probe resolves it
	// through the same conversion LoadMap performs, so both forms must reach the same package
	// — a divergence here is the guard watching a package the engine is not about to unload.
	FString MapFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, MapFilename, FPackageName::GetMapPackageExtension()))
	{
		const FTargetWorldState FromFilename = ProbeTargetWorld(MapFilename);
		TestEqual(TEXT("the .umap filename resolves to the same package"),
			FromFilename.PackageName, PackageName);
		TestTrue(TEXT("the .umap filename form blocks identically"),
			WouldMapLoadFatal(FromFilename));
	}

	Package->SetDirtyFlag(false);
	return true;
}

// The negative side: a path with nothing behind it must never block a load, and a path with
// no registered mount must not either — Map_Load's own conversion fails on that one too, so
// it never reaches the leak check and there is nothing to refuse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapGuardProbeAbsentTest,
	"PinWright.core.map_swap_guard.ProbeIsSilentForAbsentAndUnmountedPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapGuardProbeAbsentTest::RunTest(const FString& Parameters)
{
	using namespace PinWrightMapSwapGuard;

	const FString AbsentPackage = FString::Printf(TEXT("/Game/_Test/L_MapSwapGuardAbsent_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FTargetWorldState Absent = ProbeTargetWorld(AbsentPackage);
	TestEqual(TEXT("a mounted path still resolves to its package name"),
		Absent.PackageName, AbsentPackage);
	TestFalse(TEXT("nothing is resident at that path"), Absent.bPackageResident);
	TestFalse(TEXT("an absent target never blocks the swap"), WouldMapLoadFatal(Absent));

	const FTargetWorldState Unmounted = ProbeTargetWorld(TEXT("/NotAMountedRoot_pw/L_Nope"));
	TestTrue(TEXT("an unmounted path resolves to no package name"),
		Unmounted.PackageName.IsEmpty());
	TestFalse(TEXT("an unmounted path never blocks the swap"), WouldMapLoadFatal(Unmounted));

	const FTargetWorldState Empty = ProbeTargetWorld(FString());
	TestFalse(TEXT("an empty path never blocks the swap"), WouldMapLoadFatal(Empty));
	return true;
}
