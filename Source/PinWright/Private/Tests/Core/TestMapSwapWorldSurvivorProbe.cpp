// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-dirty-world-guard-only-covers-requested-map and
// B-open-level-fatal-python-held-pie-world: the map-swap verbs handed the engine a swap
// while a DEAD world was still resident, UEditorEngine::CheckForWorldGCLeaks counted it
// after the cleanse, and the editor died on "World Memory Leaks" — Fatal by default
// (Editor.CheckForWorldGCLeaksAreFatal), taking every attached session with it.
//
// Two halves of one line. The engine's leak check reads world type and world-context
// ownership; it reads no dirty flag. So a contextless world of a type the editor does not
// keep, still reachable after the probe's own collect, MUST be refused, and an ordinary
// resident world package with unsaved changes MUST NOT be, no matter how firmly it is held
// — that false refusal is what made an earlier attempt at this guard unusable, because
// swapping away from any edited map got rejected.
//
// Neither test drives level.load: the condition under test is fatal by construction, so a
// test that reached FEditorFileUtils::LoadMap on the failing side would take the suite down
// with it.
//
// EXACTLY ONE test calls ProbeResidentWorldSurvivors to ASSERT on it, and it does so once. (Two
// callers use it for its actual purpose rather than to measure it: the suite-start blank-world
// step in Tests/Infra/TestSuiteStartBlankWorld.cpp and FScopedEditorWorldMapGuard's
// untitled-world fallback, both of which are about to call NewMap and must not hand the engine a
// swap the guard would refuse.) The probe runs a
// full-purge collect, and this suite deliberately keeps fixture reclamation deferred and
// centrally scheduled (pinwright.TestGcEvery) because an unscheduled collect frees other
// tests' detached fixtures and resurfaces as an access violation in FEngineLoop::Tick,
// blamed on an unrelated test. Everything that can be asserted against the classifier
// instead of the probe is, in the second test, which collects nothing.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/MapSwapDirtyWorldGuard.h"

#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// The end-to-end half, on one probe call and therefore one collect. Both fixtures exist at
// the same time so the single call answers both questions: the dead world is named, and the
// dirty resident world package beside it is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapSurvivorProbeHeldWorldTest,
	"PinWright.core.map_swap_guard.SurvivorProbeNamesStronglyHeldDeadWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapSurvivorProbeHeldWorldTest::RunTest(const FString& Parameters)
{
	using namespace PinWrightMapSwapGuard;

	// GUID-suffixed so no earlier fixture or real world can shadow either case.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

	// (1) The crash, reproduced without the crash: a world that belongs to no FWorldContext,
	// is not one of the three types the editor keeps across a swap, and is held alive past a
	// full collect. The strong pointer stands in for the Python plugin's wrapper registry,
	// which is the holder the shipped callstack named. Never initialized, so teardown is a
	// no-op (World.cpp:1579) and no physics scene or navigation system is left behind.
	TStrongObjectPtr<UWorld> Dead(NewObject<UWorld>(
		GetTransientPackage(), FName(*FString::Printf(TEXT("PwSurvivorProbe_%s"), *Suffix))));
	if (!TestNotNull(TEXT("created a transient probe world"), Dead.Get()))
	{
		return true;
	}
	Dead->WorldType = EWorldType::Editor;
	const FString DeadWorldPath = Dead->GetPathName();

	// Unconditional, because everything below can leave early. Reclamation in this suite is
	// deferred (pinwright.TestGcEvery), so dropping the pointer does not free the world - it
	// stays resident, contextless and Editor-typed, which is exactly what
	// UEditorEngine::CheckForWorldGCLeaks counts, and any map swap in a later test would then
	// die on it. Inactive is one of the types that check keeps unconditionally, so putting the
	// type back first makes the fixture harmless for however long it lingers.
	ON_SCOPE_EXIT
	{
		if (UWorld* DeadWorld = Dead.Get())
		{
			DeadWorld->WorldType = EWorldType::Inactive;
		}
		Dead.Reset();
	};

	// (2) The swap a caller makes away from a map they have been editing: a dirty resident
	// world package, held just as firmly. A world loaded from a package is
	// EWorldType::Inactive (World.cpp:1693), which the engine's leak check keeps
	// unconditionally, so this one must not appear in the refusal.
	const FString ResidentPackageName =
		FString::Printf(TEXT("/Game/_Test/L_SurvivorProbeDirty_%s"), *Suffix);
	UPackage* ResidentPackage = CreatePackage(*ResidentPackageName);
	if (!TestNotNull(TEXT("created an in-memory world package"), ResidentPackage))
	{
		return true;
	}
	// Transient so the fixture is reclaimable; the dirty flag is cleared before returning so
	// the run does not leave the editor with a dirty package.
	ResidentPackage->SetFlags(RF_Transient);
	TStrongObjectPtr<UWorld> Resident(
		NewObject<UWorld>(ResidentPackage, TEXT("L_SurvivorProbeResident")));
	if (!TestNotNull(TEXT("created the resident world"), Resident.Get()))
	{
		return true;
	}
	Resident->WorldType = EWorldType::Inactive;
	// Same reason as above: the dirty flag is set on purpose and must come off on every exit,
	// or the editor is left with a dirty package no later test asked for.
	ON_SCOPE_EXIT
	{
		ResidentPackage->SetDirtyFlag(false);
		Resident.Reset();
	};
	ResidentPackage->SetDirtyFlag(true);
	TestTrue(TEXT("the resident fixture package really is dirty"), ResidentPackage->IsDirty());

	// The one probe call in this file. True for the transaction-buffer argument, matching
	// level.load's Map_Load entry point.
	const FWorldSurvivorProbeResult Probe =
		ProbeResidentWorldSurvivors(FString(), /*bTransactionBufferWillBeCleared=*/true);

	if (Probe.bProbeUnavailable)
	{
		// The probe refuses to guess on a stack where collecting is illegal. That is correct
		// behaviour, not a pass: this run measured nothing.
		PinWrightTestSkip::SkipAssertions(*this, TEXT("map-swap-probe-unavailable"),
			Probe.UnavailableReason);
		return true;
	}

	TestTrue(TEXT("a held dead world blocks the map swap"), Probe.IsBlocked());
	TestTrue(TEXT("the probe collected once it had something to purge"), Probe.bRanCollect);
	TestTrue(TEXT("the probe asked the holders to release before collecting"),
		Probe.PurgedWorldCount > 0);

	bool bNamedDeadWorld = false;
	for (const FResidentWorldSurvivor& Survivor : Probe.Survivors)
	{
		if (Survivor.WorldPath == DeadWorldPath)
		{
			bNamedDeadWorld = true;
			TestEqual(TEXT("the survivor carries its world type"),
				Survivor.WorldType, FString(TEXT("Editor")));
		}
		TestNotEqual(TEXT("the dirty resident world package must never be refused"),
			Survivor.PackageName, ResidentPackageName);
	}
	TestTrue(TEXT("the refusal names the surviving world by path"), bNamedDeadWorld);

	// A refusal that does not name the world is not actionable.
	TestTrue(TEXT("the refusal message names the surviving world"),
		DescribeSurvivorRefusal(Probe).Contains(DeadWorldPath));

	// A refusal the caller cannot act on is the defect this probe was written after: name the
	// holder, or say in words why the search could not. A TStrongObjectPtr holds by ref count,
	// not by a UObject reference, so this fixture is the no-chain case by construction.
	const FResidentWorldSurvivor* DeadSurvivor = Probe.Survivors.FindByPredicate(
		[&DeadWorldPath](const FResidentWorldSurvivor& Candidate)
		{
			return Candidate.WorldPath == DeadWorldPath;
		});
	if (DeadSurvivor)
	{
		TestTrue(TEXT("the survivor says who or what is holding the world"),
			!DeadSurvivor->ReferencedBy.IsEmpty());
	}

	return true;
}

// The classifier, which is where the discrimination actually lives — asserted directly so
// the whole table costs no garbage collection. Every case is a world held for the duration
// of the test, so "would be reclaimed anyway" can never be what makes a case pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMapSwapLeakCheckClassifierTest,
	"PinWright.core.map_swap_guard.LeakCheckClassifierIgnoresDirtyAndKeptWorlds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMapSwapLeakCheckClassifierTest::RunTest(const FString& Parameters)
{
	using namespace PinWrightMapSwapGuard;

	const FString PackageName = FString::Printf(TEXT("/Game/_Test/L_LeakClassifier_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("created an in-memory world package"), Package))
	{
		return true;
	}
	Package->SetFlags(RF_Transient);

	TStrongObjectPtr<UWorld> World(NewObject<UWorld>(Package, TEXT("L_LeakClassifier")));
	if (!TestNotNull(TEXT("created the classifier fixture world"), World.Get()))
	{
		return true;
	}
	// This test walks the fixture through the counted types and would otherwise leave it on
	// the last one it set. Reclamation is deferred, so a counted world outlives the test and a
	// later map swap's CheckForWorldGCLeaks is fatal on it; the type goes back to a kept one on
	// every exit, including the early returns below.
	ON_SCOPE_EXIT
	{
		if (UWorld* FixtureWorld = World.Get())
		{
			FixtureWorld->WorldType = EWorldType::Inactive;
		}
		Package->SetDirtyFlag(false);
		World.Reset();
	};
	Package->SetDirtyFlag(true);
	TestTrue(TEXT("the fixture package really is dirty"), Package->IsDirty());

	// The kept types (EditorServer.cpp:1918). A world loaded from a package defaults to
	// Inactive (World.cpp:1693), so this is the ordinary resident map, dirty and firmly held,
	// that the returned earlier patch refused.
	World->WorldType = EWorldType::Inactive;
	TestFalse(TEXT("a dirty, held, Inactive world is not a leak-check survivor"),
		IsWorldCountedByLeakCheck(World.Get(), FString()));
	World->WorldType = EWorldType::EditorPreview;
	TestFalse(TEXT("an EditorPreview world is kept"),
		IsWorldCountedByLeakCheck(World.Get(), FString()));
	World->WorldType = EWorldType::GamePreview;
	TestFalse(TEXT("a GamePreview world is kept"),
		IsWorldCountedByLeakCheck(World.Get(), FString()));

	// Flip ONLY the type and the same held, dirty world is counted: the discrimination is
	// world type and context ownership, never the dirty flag. This is the line the returned
	// earlier patch had backwards.
	World->WorldType = EWorldType::Editor;
	TestTrue(TEXT("a contextless Editor-typed world IS counted"),
		IsWorldCountedByLeakCheck(World.Get(), FString()));
	World->WorldType = EWorldType::PIE;
	TestTrue(TEXT("a contextless PIE world IS counted — the dead-PIE-world case"),
		IsWorldCountedByLeakCheck(World.Get(), FString()));

	// ...unless it lives in the map being opened, which is Map_Load's own business: it may
	// keep an uninitialized copy as NewWorld, and WouldMapLoadFatal refuses the dirty case
	// ahead of this. Counting it here would make level.create -> level.load refuse itself.
	TestFalse(TEXT("a world in the incoming target package is exempt"),
		IsWorldCountedByLeakCheck(World.Get(), PackageName));

	TestFalse(TEXT("a null world is never counted"),
		IsWorldCountedByLeakCheck(nullptr, FString()));

	return true;
}
