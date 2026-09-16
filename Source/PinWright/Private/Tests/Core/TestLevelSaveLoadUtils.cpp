// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for level load/save decision helpers in AssetUtils.
#include "Misc/AutomationTest.h"
#include "PinWrightHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelMatchCurrentWorldExactPackageTest,
	"PinWright.core.level_save_load.does_requested_level_match_current_world.ExactPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelMatchCurrentWorldExactPackageTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Exact package name should match"),
		DoesRequestedLevelMatchCurrentWorld(
			TEXT("/Game/Maps/L_Test"),
			TEXT("/Game/Maps/L_Test"),
			TEXT("L_Test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelMatchCurrentWorldShortNameTest,
	"PinWright.core.level_save_load.does_requested_level_match_current_world.ShortName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelMatchCurrentWorldShortNameTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Short map name should match current world"),
		DoesRequestedLevelMatchCurrentWorld(
			TEXT("L_Test"),
			TEXT("/Game/Maps/L_Test"),
			TEXT("L_Test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelMatchCurrentWorldObjectPathTest,
	"PinWright.core.level_save_load.does_requested_level_match_current_world.ObjectPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelMatchCurrentWorldObjectPathTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Object path should match package"),
		DoesRequestedLevelMatchCurrentWorld(
			TEXT("/Game/Maps/L_Test.L_Test"),
			TEXT("/Game/Maps/L_Test"),
			TEXT("L_Test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelMatchCurrentWorldMismatchTest,
	"PinWright.core.level_save_load.does_requested_level_match_current_world.Mismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelMatchCurrentWorldMismatchTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Different level should not match"),
		DoesRequestedLevelMatchCurrentWorld(
			TEXT("/Game/Maps/L_Other"),
			TEXT("/Game/Maps/L_Test"),
			TEXT("L_Test")));
	return true;
}

// Regression coverage for B-open-level-engine-mount-mangled: editor.open_level
// resolved level paths with a hardcoded MapPath.RightChop(6) + ProjectContentDir()
// string-chop that assumed every package starts with the 6-char /Game/ prefix. For
// any non-/Game mount the chop stripped the wrong 6 chars and forced the project
// content dir, e.g. /Engine/Maps/Templates/OpenWorld -> <Project>/Content/e/Maps/...
// a fictional path that always 404s. The fix routes through the mount-aware
// ResolveLevelPackageToMapFilename (FPackageName::TryConvertLongPackageNameToFilename),
// the same resolver the sync sibling level.load uses. These assertions fail if the
// helper is reverted to the RightChop(6) chop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveLevelEngineMountTest,
	"PinWright.core.level_save_load.resolve_level_package_to_map_filename.EngineMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveLevelEngineMountTest::RunTest(const FString& Parameters)
{
	// /Engine/ must resolve into the engine content tree, never the project's
	// Content/e/... garbage the RightChop(6) bug produced.
	FString EngineMapFile;
	const bool bEngineResolved = ResolveLevelPackageToMapFilename(
		TEXT("/Engine/Maps/Templates/OpenWorld"), EngineMapFile);
	TestTrue(TEXT("/Engine/ level path resolves to a filename"), bEngineResolved);
	if (bEngineResolved)
	{
		const FString FullEngineMapFile = FPaths::ConvertRelativePathToFull(EngineMapFile);
		const FString EngineContentDir =
			FPaths::ConvertRelativePathToFull(FPaths::EngineContentDir());
		TestTrue(TEXT("Resolved /Engine/ path lands under the engine content dir"),
			FullEngineMapFile.StartsWith(EngineContentDir));
		// The RightChop(6) bug produced .../Content/e/Maps/...; assert that exact
		// mangling fragment never appears in the resolved path.
		TestFalse(TEXT("Resolved /Engine/ path has no mangled Content/e/ fragment"),
			FullEngineMapFile.Contains(TEXT("/Content/e/")));
		TestTrue(TEXT("Resolved /Engine/ path ends with .umap"),
			FullEngineMapFile.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase));
		// The chop bug stripped the package's leading directory; the real map name
		// must survive resolution.
		TestTrue(TEXT("Resolved /Engine/ path contains the OpenWorld map name"),
			FullEngineMapFile.Contains(TEXT("OpenWorld")));
	}

	// A trailing .umap must be tolerated and produce the same resolution.
	FString EngineMapFileWithExt;
	const bool bExtResolved = ResolveLevelPackageToMapFilename(
		TEXT("/Engine/Maps/Templates/OpenWorld.umap"), EngineMapFileWithExt);
	TestTrue(TEXT("/Engine/ path with .umap suffix resolves"), bExtResolved);
	if (bEngineResolved && bExtResolved)
	{
		TestEqual(TEXT(".umap suffix resolves identically to bare package"),
			EngineMapFileWithExt, EngineMapFile);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveLevelGameMountTest,
	"PinWright.core.level_save_load.resolve_level_package_to_map_filename.GameMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveLevelGameMountTest::RunTest(const FString& Parameters)
{
	// /Game/ must still resolve into the project content tree (the one case the
	// old RightChop(6) handled correctly), so the fix is not a regression for it.
	FString GameMapFile;
	const bool bGameResolved = ResolveLevelPackageToMapFilename(
		TEXT("/Game/Maps/MyLevel"), GameMapFile);
	TestTrue(TEXT("/Game/ level path resolves to a filename"), bGameResolved);
	if (bGameResolved)
	{
		const FString FullGameMapFile = FPaths::ConvertRelativePathToFull(GameMapFile);
		const FString ProjectContentDir =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		TestTrue(TEXT("Resolved /Game/ path lands under the project content dir"),
			FullGameMapFile.StartsWith(ProjectContentDir));
		TestTrue(TEXT("Resolved /Game/ path preserves the Maps/MyLevel package"),
			FullGameMapFile.Contains(TEXT("Maps/MyLevel")) ||
			FullGameMapFile.Contains(TEXT("Maps\\MyLevel")));
	}

	// An unregistered mount must report failure (clean rejection), not silent
	// corruption into the project content dir.
	FString BogusMapFile;
	const bool bBogusResolved = ResolveLevelPackageToMapFilename(
		TEXT("/NotARegisteredMount/Foo/Bar"), BogusMapFile);
	TestFalse(TEXT("Unregistered mount resolves to false, not a mangled path"),
		bBogusResolved);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSavePolicyRequiresReportedSuccessTest,
	"PinWright.core.level_save_load.should_treat_level_save_as_success.RequiresReportedSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSavePolicyRequiresReportedSuccessTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Policy must fail when SaveLevel returned false"),
		ShouldTreatLevelSaveAsSuccess(false, true, true, true, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSavePolicyAcceptsDiskFileTest,
	"PinWright.core.level_save_load.should_treat_level_save_as_success.DiskFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSavePolicyAcceptsDiskFileTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Disk file verification should pass policy"),
		ShouldTreatLevelSaveAsSuccess(true, true, false, false, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSavePolicyAcceptsPackageOrAssetTest,
	"PinWright.core.level_save_load.should_treat_level_save_as_success.PackageOrAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSavePolicyAcceptsPackageOrAssetTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Package existence should pass policy"),
		ShouldTreatLevelSaveAsSuccess(true, false, true, false, false));
	TestTrue(TEXT("Asset existence should pass policy"),
		ShouldTreatLevelSaveAsSuccess(true, false, false, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSavePolicyAcceptsCleanPackageTest,
	"PinWright.core.level_save_load.should_treat_level_save_as_success.CleanPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSavePolicyAcceptsCleanPackageTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Clean package fallback should pass policy"),
		ShouldTreatLevelSaveAsSuccess(true, false, false, false, true));
	TestFalse(TEXT("No verification signals should fail policy"),
		ShouldTreatLevelSaveAsSuccess(true, false, false, false, false));
	return true;
}

// Regression coverage for B-create-level-saved-true-no-umap: the create_level
// flow (create-to-a-new-/Game/-path) must NOT report saved:true when SaveLevel
// reported success but no .umap landed on disk. The general
// ShouldTreatLevelSaveAsSuccess OR-policy intentionally accepts the clean-package
// fallback (asserted above), so create_level uses the stricter
// ShouldTreatCreateLevelSaveAsSuccess that makes disk presence load-bearing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateLevelSavePolicyRequiresDiskFileTest,
	"PinWright.core.level_save_load.should_treat_create_level_save_as_success.RequiresDiskFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateLevelSavePolicyRequiresDiskFileTest::RunTest(const FString& Parameters)
{
	// The defect: SaveLevel returned true and the package went clean, but no file
	// is on disk. The general policy masks this; the create policy must reject it.
	TestTrue(TEXT("General policy still masks a missing disk file via clean fallback"),
		ShouldTreatLevelSaveAsSuccess(true, false, false, false, true));
	TestFalse(TEXT("Create policy must FAIL when save reported success but no .umap on disk"),
		ShouldTreatCreateLevelSaveAsSuccess(true, false));

	// Honest success path: save reported success and the file landed on disk.
	TestTrue(TEXT("Create policy passes when the .umap exists on disk"),
		ShouldTreatCreateLevelSaveAsSuccess(true, true));

	// SaveLevel itself failed — must fail regardless of any disk state.
	TestFalse(TEXT("Create policy fails when SaveLevel returned false"),
		ShouldTreatCreateLevelSaveAsSuccess(false, true));
	TestFalse(TEXT("Create policy fails when SaveLevel returned false and no disk file"),
		ShouldTreatCreateLevelSaveAsSuccess(false, false));
	return true;
}

// Regression coverage for B-niagara-save-no-disk-write: niagara.save /
// niagara.create_system / niagara.create_emitter promise disk persistence, but
// the shared mark-dirty helper McpSafeAssetSave returns true without writing a
// .uasset (deferred to dodge the bulkdata-corruption vector). The handlers now
// gate saved:true through ShouldTreatAssetSaveAsSuccess so a reported-success
// save that left no file on disk reports saved:false (+ pendingFlush) instead of
// a false saved:true with a self-contradicting sizeBytes:0. If a handler is
// reverted to `saved = McpSafeAssetSave(Asset)` (always true regardless of disk),
// this predicate's (true,false)->false contract is what catches it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSavePolicyRequiresDiskFileTest,
	"PinWright.core.level_save_load.should_treat_asset_save_as_success.RequiresDiskFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSavePolicyRequiresDiskFileTest::RunTest(const FString& Parameters)
{
	// The defect: the save helper reported success (true) but no .uasset is on
	// disk (the mark-dirty / throttled-skip / deferred case). Must NOT count as saved.
	TestFalse(TEXT("Asset save policy must FAIL when save reported success but no .uasset on disk"),
		ShouldTreatAssetSaveAsSuccess(true, false));

	// Honest success path: save reported success and the file landed on disk.
	TestTrue(TEXT("Asset save policy passes when the .uasset exists on disk"),
		ShouldTreatAssetSaveAsSuccess(true, true));

	// Save itself failed — must fail regardless of any disk state.
	TestFalse(TEXT("Asset save policy fails when save returned false (file present)"),
		ShouldTreatAssetSaveAsSuccess(false, true));
	TestFalse(TEXT("Asset save policy fails when save returned false and no disk file"),
		ShouldTreatAssetSaveAsSuccess(false, false));
	return true;
}

// Regression coverage for E-level-load-file-not-found-vs-in-memory-orphan:
// level.load / editor.open_level used to emit a bare FILE_NOT_FOUND whenever no
// .umap was on disk, even when a matching world/package was live in memory but
// simply never saved — a path-resolution-flavored error that drove the caller
// into path-form trial-and-error instead of telling them to save or discard the
// orphan. Both handlers now branch on IsLevelPackagePresentInMemoryOrRegistry so
// the in-memory-present-but-unsaved case maps to LEVEL_NOT_PERSISTED while a
// genuinely-absent path keeps FILE_NOT_FOUND. This drives that probe directly
// with a live (never-saved-to-disk) in-memory package — the exact orphaned-world
// signal the handlers rely on — plus its object-path / .umap normalisation forms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelPresentInMemoryProbeTest,
	"PinWright.core.level_save_load.is_level_present_in_memory_or_registry.InMemoryVsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelPresentInMemoryProbeTest::RunTest(const FString& Parameters)
{
	// A package name that is not in memory, the registry, or on disk must read as
	// absent (the FILE_NOT_FOUND branch). Use a GUID-suffixed path so no real or
	// previously-created asset can shadow the negative case.
	const FString MissingPackage = FString::Printf(TEXT("/Game/_Test/L_OrphanProbe_Missing_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestFalse(TEXT("Unknown package -> not present (FILE_NOT_FOUND branch)"),
		IsLevelPackagePresentInMemoryOrRegistry(MissingPackage));

	// Create a live, never-saved-to-disk package — the in-memory orphan the fix
	// targets. The probe must report it present (the LEVEL_NOT_PERSISTED branch).
	const FString PresentPackage = FString::Printf(TEXT("/Game/_Test/L_OrphanProbe_Present_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PresentPackage);
	if (!TestNotNull(TEXT("created transient in-memory package for the probe"), Package))
	{
		return true;
	}
	Package->SetFlags(RF_Transient);

	TestTrue(TEXT("Live in-memory package -> present (LEVEL_NOT_PERSISTED branch)"),
		IsLevelPackagePresentInMemoryOrRegistry(PresentPackage));

	// The probe normalises an object path (/Game/.../L.L) and a trailing .umap to
	// the bare package name before the lookup, so all three path forms agree.
	const FString ShortName = FPackageName::GetShortName(PresentPackage);
	TestTrue(TEXT("Object-path form resolves to the same in-memory package"),
		IsLevelPackagePresentInMemoryOrRegistry(PresentPackage + TEXT(".") + ShortName));
	TestTrue(TEXT(".umap-suffixed form resolves to the same in-memory package"),
		IsLevelPackagePresentInMemoryOrRegistry(PresentPackage + FPackageName::GetMapPackageExtension()));

	// Empty / whitespace input is never present.
	TestFalse(TEXT("Empty path -> not present"),
		IsLevelPackagePresentInMemoryOrRegistry(TEXT("   ")));
	return true;
}

// Regression coverage for B-level-save-saved-true-in-memory-no-umap: level.save /
// level.save_as wrap the shared McpSafeLevelSave, whose ShouldTreatLevelSaveAsSuccess
// OR-policy accepts a clean package / registry asset as success even when no .umap
// landed on disk. For a freshly-created in-memory-only active world (CreatePackage'd
// but never written) that produced a false saved:true. Both handlers now re-probe
// the .umap on disk (via ResolveLevelPackageToMapFilename) and gate saved:true
// through the stricter ShouldTreatCreateLevelSaveAsSuccess.
//
// Scope of this test: it locks the disk-presence-probe + predicate CONTRACT the
// handlers depend on — for a real in-memory-only package, the resolver-driven probe
// reports bFileOnDisk=false, the lenient OR-policy still masks it, and the strict
// create-level gate correctly reports saved:false. It does NOT invoke the handlers
// themselves, so it does not by itself catch a handler being rewired back to a bare
// McpSafeLevelSave; it guards the shared helpers those handlers must keep calling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveInMemoryWorldReportsNotPersistedTest,
	"PinWright.core.level_save_load.level_save_disk_probe.InMemoryWorldNotPersisted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveInMemoryWorldReportsNotPersistedTest::RunTest(const FString& Parameters)
{
	// Build the exact orphan the bug fires on: a live, never-saved-to-disk package
	// at a GUID-suffixed /Game/ path so nothing can shadow the negative case.
	const FString InMemoryPackage = FString::Printf(TEXT("/Game/_Test/L_SaveProbe_InMemory_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*InMemoryPackage);
	if (!TestNotNull(TEXT("created transient in-memory package for the save probe"), Package))
	{
		return true;
	}
	Package->SetFlags(RF_Transient);

	// Drive the same disk-presence probe the handlers run: the shared mount-aware
	// resolver ResolveLevelPackageToMapFilename -> IFileManager::FileExists. For a
	// CreatePackage'd-but-never-written world this MUST be false.
	FString LevelFilename;
	const bool bConverted = ResolveLevelPackageToMapFilename(InMemoryPackage, LevelFilename);
	TestTrue(TEXT("package name converts to a .umap filename"), bConverted);
	const bool bFileOnDisk = bConverted && IFileManager::Get().FileExists(*LevelFilename);
	TestFalse(TEXT("in-memory-only world has no .umap on disk"), bFileOnDisk);

	// The lenient shared OR-policy still masks this (clean-package fallback) — the
	// exact false-success the handlers must NOT inherit.
	TestTrue(TEXT("shared OR-policy still masks the missing .umap via clean fallback"),
		ShouldTreatLevelSaveAsSuccess(/*bSaveReported=*/true, bFileOnDisk,
			/*bPackageExists=*/false, /*bAssetExists=*/false, /*bPackageClean=*/true));

	// The gate level.save / level.save_as now apply: even when McpSafeLevelSave
	// reported success, no .umap on disk -> honest saved:false.
	TestFalse(TEXT("level.save disk-presence gate reports saved:false for the in-memory orphan"),
		ShouldTreatCreateLevelSaveAsSuccess(/*bSaveReported=*/true, bFileOnDisk));

	// And the honest on-disk re-save path is preserved: once the .umap exists,
	// the same gate reports saved:true.
	TestTrue(TEXT("gate reports saved:true when the .umap is on disk"),
		ShouldTreatCreateLevelSaveAsSuccess(/*bSaveReported=*/true, /*bFileOnDisk=*/true));

	return true;
}

// Companion regression coverage for B-level-save-saved-true-in-memory-no-umap that
// drives the shared VerifyLevelSavedToDisk helper end-to-end — the exact code path
// both level.save and level.save_as now run (resolve the package to its .umap
// filename, probe IFileManager::FileExists, re-gate the reported-success boolean,
// and classify the error code) — against a LIVE in-memory-only package that was
// never written. Where the test above locks the underlying predicate contract, this
// one locks the bundled helper + its error-code classification. If a handler is
// reverted to `saved = McpSafeLevelSave(...)`, the (reported=true, on-disk=false)->
// false contract this asserts is what catches it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveVerbRejectsInMemoryNoDiskFileTest,
	"PinWright.core.level_save_load.level_save_verb.RejectsInMemoryNoDiskFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveVerbRejectsInMemoryNoDiskFileTest::RunTest(const FString& Parameters)
{
	// A live, never-saved-to-disk package — the in-memory-only world the false
	// saved:true fires on. GUID-suffixed so nothing on disk can shadow it.
	const FString InMemoryPackage = FString::Printf(TEXT("/Game/_Test/L_SaveVerbProbe_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*InMemoryPackage);
	if (!TestNotNull(TEXT("created transient in-memory package for the probe"), Package))
	{
		return true;
	}
	Package->SetFlags(RF_Transient);

	// Sanity: the package resolves to a .umap path but nothing is on disk yet.
	FString MapFilename;
	const bool bResolved = ResolveLevelPackageToMapFilename(InMemoryPackage, MapFilename);
	TestTrue(TEXT("package resolves to a .umap filename"), bResolved);
	TestFalse(TEXT("in-memory-only package has no .umap on disk"),
		bResolved && IFileManager::Get().FileExists(*MapFilename));

	// The bug: McpSafeLevelSave can report success for this clean/registered
	// in-memory package (the lenient OR-policy). The save verbs now route through
	// VerifyLevelSavedToDisk, which re-gates that reported-success boolean on disk
	// presence — so even a reported-true save MUST resolve to saved:false when no
	// file landed, and classify the failure as SAVE_VERIFICATION_FAILED.
	FString ResolvedFilename;
	FString ErrorCode;
	TestFalse(TEXT("save verb must report saved:false for reported-success + no .umap on disk"),
		VerifyLevelSavedToDisk(InMemoryPackage, /*bSaveReportedSuccess=*/true, ResolvedFilename, ErrorCode));
	TestEqual(TEXT("reported-success-but-no-file classifies as SAVE_VERIFICATION_FAILED"),
		ErrorCode, FString(TEXT("SAVE_VERIFICATION_FAILED")));

	// A save that never reported success at all classifies as plain SAVE_FAILED.
	VerifyLevelSavedToDisk(InMemoryPackage, /*bSaveReportedSuccess=*/false, ResolvedFilename, ErrorCode);
	TestEqual(TEXT("no-reported-success classifies as SAVE_FAILED"),
		ErrorCode, FString(TEXT("SAVE_FAILED")));

	return true;
}
