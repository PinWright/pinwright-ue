// Copyright (c) 2026 Alexander Penkin. MIT License.

// Host-safety coverage for asset.bulk_delete's automatic redirector fixup.
//
// The defect (board B-tests-destroy-host-assets, docs/defect-backlog.md D-72): the
// fixup built an FARFilter carrying ClassPaths only, so GetAssets returned EVERY
// ObjectRedirector in the project and the fixup deleted all of them and re-saved their
// referencers. `fixupRedirectors` defaults to true and there was no scope parameter, so
// deleting a handful of assets in one folder performed a project-wide redirector purge.
// In this host it irreversibly deleted 4 pre-existing ObjectRedirector packages under
// Content/ExampleContent and rewrote 13 packages that referenced them; only git got them
// back. The caller's response said nothing about any of it.
//
// COUNTERFACTUAL, per test:
//   * OutsideRedirectorsSurviveTheDefaultScope - revert BuildSweepFilter to the class-only
//     filter and the outside-folder redirector is swept in and deleted, so the survival
//     assertion fails. This is the data-loss test.
//   * ProjectScopeStillReachesOutsideRedirectors - drop ESweepScope::Project (or make it
//     narrow like the default) and the wide filter stops returning the outside redirector.
//   * EmptyPathSetMatchesNothing - delete the sentinel path in BuildSweepFilter and an
//     empty PackagePaths array silently means "every path" again, which is the original
//     bug reintroduced through the narrow door.
//   * UnknownFixupScopeIsRejected - accept an unknown scope by falling back to a default
//     and the error assertion fails; §3 of docs/rpc-design.md is what forbids the
//     fall-back, because the wrong guess here is a project-wide purge.
//   * FixupScopeIsDeclaredWithPathsDefault - drop the FParamSpec and the dispatcher's
//     unknown-parameter gate rejects `fixupScope` before any handler body sees it, so the
//     opt-in would be undialable even though the body reads it.
//
// WHY NO TEST DISPATCHES asset.bulk_delete WITH fixupScope:"project": "project" means
// project. Dispatching it would fix up and delete every ObjectRedirector in the HOST, which
// is precisely the damage this ticket exists to stop - a host-safety test that destroys
// host content proves nothing and costs everything. The opt-in is therefore asserted at the
// seam it is implemented at: the FARFilter that the handler hands to GetAssets, run against
// the live registry as a READ. That still measures the thing that matters (does the wide
// scope reach a redirector the narrow scope excludes) without deleting anything.
//
// Every fixture lives under /Game/PinWrightTests/BulkDeleteScope/, in two SIBLING folders -
// "Inside" (what the deletion names) and "Outside" (what must survive). Siblings, not
// parent/child, because that is the pair a string-prefix scope check gets wrong.
// Never-saved in-memory packages announced with FAssetRegistryModule::AssetCreated: the
// registry indexes them exactly as it does a freshly created real asset (FARFilter's
// bIncludeOnlyOnDiskAssets defaults to false), and nothing is ever written to the host's
// Content directory.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CoreGlobals.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerRegistration.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSettings.h"
#include "Tests/TestUtils.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "Utils/RedirectorFixupPolicy.h"

// File-scope helper names carry the BulkDeleteScopeTest_ prefix: bUseUnity merges
// translation units, so an unprefixed helper collides with a same-named static in a
// neighbouring test file (see CLAUDE.md > Building).

namespace BulkDeleteScopeTest
{
    // Two SIBLING folders. "InsideFolder" is where the deleted asset and its redirector
    // live; "OutsideFolder" is unrelated host-shaped content that must be untouched.
    static const TCHAR* InsideFolder  = TEXT("/Game/PinWrightTests/BulkDeleteScope/Inside");
    static const TCHAR* OutsideFolder = TEXT("/Game/PinWrightTests/BulkDeleteScope/Outside");

    inline FString PackagePathFor(const TCHAR* Folder, const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *AssetName);
    }

    inline FString ObjectPathFor(const TCHAR* Folder, const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *AssetName, *AssetName);
    }

    inline UMaterial* MakeMaterial(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        UMaterial* Material = NewObject<UMaterial>(
            Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone);
        if (Material)
        {
            FAssetRegistryModule::AssetCreated(Material);
        }
        return Material;
    }

    // The shape asset.rename leaves behind: a package holding nothing but a redirector.
    inline UObjectRedirector* MakeRedirector(const FString& PackagePath, UObject* Destination)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        UObjectRedirector* Redirector = NewObject<UObjectRedirector>(
            Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone);
        if (Redirector)
        {
            Redirector->DestinationObject = Destination;
            FAssetRegistryModule::AssetCreated(Redirector);
        }
        return Redirector;
    }

    // Registry-visible redirector package names returned by a filter, so an assertion can
    // name what it did and did not see.
    inline TArray<FString> RedirectorPackagesMatching(const FARFilter& Filter)
    {
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);

        TArray<FString> Names;
        for (const FAssetData& Asset : Assets)
        {
            Names.AddUnique(Asset.PackageName.ToString());
        }
        return Names;
    }

    // Forces modal suppression on for the body and restores both it and the process
    // global afterwards. InvokeHandler calls the registered function directly, so it never
    // crosses FRpcDispatcher's own FScopedUnattendedRpc - without this the handler's
    // ObjectTools::DeleteObjects would run unguarded. Mirrors the guard in
    // TestRedirectorFixupPolicy.cpp.
    struct FSuppression
    {
        UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
        bool bSavedSetting = false;
        bool bSavedGlobal = GIsRunningUnattendedScript;

        FSuppression()
        {
            PinWrightAutomationMode::ResetForTests();
            if (Settings)
            {
                bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
                Settings->bSuppressModalDialogsDuringRpc = true;
            }
            GIsRunningUnattendedScript = false;
        }

        ~FSuppression()
        {
            PinWrightAutomationMode::ResetForTests();
            if (Settings)
            {
                Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
            }
            GIsRunningUnattendedScript = bSavedGlobal;
        }
    };
}

// ============================================================================
// The data-loss test. A bulk_delete naming ONE asset in one folder must not remove a
// redirector sitting in a sibling folder it never mentioned.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteOutsideRedirectorsSurviveTest,
    "PinWright.asset.bulk_delete.OutsideRedirectorsSurviveTheDefaultScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteOutsideRedirectorsSurviveTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteScopeTest;

    FSuppression Suppression;

    const FString DoomedPath        = PackagePathFor(InsideFolder,  TEXT("M_Doomed"));
    const FString InsideTargetPath  = PackagePathFor(InsideFolder,  TEXT("M_InsideTarget"));
    const FString InsideOldPath     = PackagePathFor(InsideFolder,  TEXT("M_InsideOld"));
    const FString OutsideTargetPath = PackagePathFor(OutsideFolder, TEXT("M_OutsideTarget"));
    const FString OutsideOldPath    = PackagePathFor(OutsideFolder, TEXT("M_OutsideOld"));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InsideOldPath);
        CleanupTestAsset(InsideTargetPath);
        CleanupTestAsset(DoomedPath);
        CleanupTestAsset(OutsideOldPath);
        CleanupTestAsset(OutsideTargetPath);
    };

    UMaterial* Doomed = MakeMaterial(DoomedPath);
    if (!TestNotNull(TEXT("the asset to delete was created"), Doomed)) return false;
    UMaterial* InsideTarget = MakeMaterial(InsideTargetPath);
    if (!TestNotNull(TEXT("the in-folder redirect target was created"), InsideTarget)) return false;
    if (!TestNotNull(TEXT("the in-folder redirector was created"),
            MakeRedirector(InsideOldPath, InsideTarget))) return false;

    UMaterial* OutsideTarget = MakeMaterial(OutsideTargetPath);
    if (!TestNotNull(TEXT("the sibling-folder redirect target was created"), OutsideTarget)) return false;
    if (!TestNotNull(TEXT("the sibling-folder redirector was created"),
            MakeRedirector(OutsideOldPath, OutsideTarget))) return false;

    // The fixture has to be findable before the call, or "it survived" would be
    // indistinguishable from "it was never there" - a check that examined nothing.
    if (!TestNotNull(TEXT("the sibling-folder redirector is live before the delete"),
            FindObject<UObjectRedirector>(nullptr, *ObjectPathFor(OutsideFolder, TEXT("M_OutsideOld")))))
    {
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> PathsToDelete;
    PathsToDelete.Add(MakeShared<FJsonValueString>(DoomedPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assetPaths"), PathsToDelete);
    // fixupRedirectors and fixupScope both left at their defaults on purpose: the defect
    // was in the DEFAULT path, so the default is what has to be exercised.

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.bulk_delete handler found"),
            InvokeHandlerWithCapture(TEXT("asset.bulk_delete"), Payload, Capture));
    }

    TestTrue(TEXT("the delete responded"), Capture.bWasCalled);
    TestTrue(TEXT("the delete succeeded"), Capture.bSuccess);

    // THE assertion: the redirector in the folder the caller never named is still there.
    TestNotNull(TEXT("the sibling-folder redirector survived the delete"),
        FindObject<UObjectRedirector>(nullptr, *ObjectPathFor(OutsideFolder, TEXT("M_OutsideOld"))));
    TestNotNull(TEXT("the sibling-folder redirect target survived the delete"),
        FindObject<UMaterial>(nullptr, *ObjectPathFor(OutsideFolder, TEXT("M_OutsideTarget"))));

    if (Capture.Result.IsValid())
    {
        FString ReportedScope;
        TestTrue(TEXT("the response names the scope it used"),
            Capture.Result->TryGetStringField(TEXT("fixupScope"), ReportedScope));
        TestEqual(TEXT("the default scope is 'paths'"), ReportedScope, FString(TEXT("paths")));

        const TArray<TSharedPtr<FJsonValue>>* Outside = nullptr;
        if (TestTrue(TEXT("the response always carries redirectorsDeletedOutsideScope"),
                Capture.Result->TryGetArrayField(TEXT("redirectorsDeletedOutsideScope"), Outside)) &&
            Outside)
        {
            TestEqual(TEXT("a scoped delete reports no collateral"), Outside->Num(), 0);
        }
    }

    return true;
}

// ============================================================================
// The opt-in half. Asserted on the filter the handler builds, run against the live
// registry as a READ - see the file header for why nothing dispatches the verb with
// fixupScope:"project". A single test covers both directions on the same fixture so the
// two filters cannot be compared against different worlds.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteProjectScopeReachesOutsideTest,
    "PinWright.asset.bulk_delete.ProjectScopeStillReachesOutsideRedirectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteProjectScopeReachesOutsideTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteScopeTest;

    const FString DoomedPath        = PackagePathFor(InsideFolder,  TEXT("M_ScopeDoomed"));
    const FString InsideTargetPath  = PackagePathFor(InsideFolder,  TEXT("M_ScopeInsideTarget"));
    const FString InsideOldPath     = PackagePathFor(InsideFolder,  TEXT("M_ScopeInsideOld"));
    const FString OutsideTargetPath = PackagePathFor(OutsideFolder, TEXT("M_ScopeOutsideTarget"));
    const FString OutsideOldPath    = PackagePathFor(OutsideFolder, TEXT("M_ScopeOutsideOld"));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InsideOldPath);
        CleanupTestAsset(InsideTargetPath);
        CleanupTestAsset(DoomedPath);
        CleanupTestAsset(OutsideOldPath);
        CleanupTestAsset(OutsideTargetPath);
    };

    if (!TestNotNull(TEXT("the asset to delete was created"), MakeMaterial(DoomedPath))) return false;
    UMaterial* InsideTarget = MakeMaterial(InsideTargetPath);
    if (!TestNotNull(TEXT("the in-folder redirect target was created"), InsideTarget)) return false;
    if (!TestNotNull(TEXT("the in-folder redirector was created"),
            MakeRedirector(InsideOldPath, InsideTarget))) return false;
    UMaterial* OutsideTarget = MakeMaterial(OutsideTargetPath);
    if (!TestNotNull(TEXT("the sibling-folder redirect target was created"), OutsideTarget)) return false;
    if (!TestNotNull(TEXT("the sibling-folder redirector was created"),
            MakeRedirector(OutsideOldPath, OutsideTarget))) return false;

    const TArray<FString> DeletedAssets = { DoomedPath };

    const TArray<FString> NarrowMatches = RedirectorPackagesMatching(
        RedirectorFixupPolicy::BuildSweepFilter(
            DeletedAssets, RedirectorFixupPolicy::ESweepScope::RequestedPaths));

    const TArray<FString> WideMatches = RedirectorPackagesMatching(
        RedirectorFixupPolicy::BuildSweepFilter(
            DeletedAssets, RedirectorFixupPolicy::ESweepScope::Project));

    // Both directions on one fixture: the narrow scope reaches its own folder and stops
    // there; the wide scope reaches past it. Asserting only the second half would pass on
    // a filter that matches everything.
    TestTrue(TEXT("the default scope reaches the redirector beside the deleted asset"),
        NarrowMatches.Contains(InsideOldPath));
    TestFalse(TEXT("the default scope does NOT reach the sibling folder"),
        NarrowMatches.Contains(OutsideOldPath));

    TestTrue(TEXT("the project opt-in still reaches the sibling folder"),
        WideMatches.Contains(OutsideOldPath));
    TestTrue(TEXT("the project opt-in still reaches the deleted asset's own folder"),
        WideMatches.Contains(InsideOldPath));

    // The wide filter is wide because it constrains nothing but the class - that is the
    // property the opt-in buys, and the property the default must never have.
    const FARFilter WideFilter = RedirectorFixupPolicy::BuildSweepFilter(
        DeletedAssets, RedirectorFixupPolicy::ESweepScope::Project);
    TestEqual(TEXT("the project filter constrains no package path"),
        WideFilter.PackagePaths.Num(), 0);

    const FARFilter NarrowFilter = RedirectorFixupPolicy::BuildSweepFilter(
        DeletedAssets, RedirectorFixupPolicy::ESweepScope::RequestedPaths);
    TestEqual(TEXT("the default filter constrains exactly the deleted asset's folder"),
        NarrowFilter.PackagePaths.Num(), 1);
    TestFalse(TEXT("the default filter is not recursive"), NarrowFilter.bRecursivePaths);

    return true;
}

// ============================================================================
// An FARFilter with an empty PackagePaths array matches EVERY path, so "the request
// resolved to no folder" must not quietly become the project-wide sweep. This is the
// narrow door back into the original bug.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteEmptyScopeMatchesNothingTest,
    "PinWright.asset.bulk_delete.EmptyPathSetMatchesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteEmptyScopeMatchesNothingTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteScopeTest;

    const FString OutsideTargetPath = PackagePathFor(OutsideFolder, TEXT("M_EmptyScopeTarget"));
    const FString OutsideOldPath    = PackagePathFor(OutsideFolder, TEXT("M_EmptyScopeOld"));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(OutsideOldPath);
        CleanupTestAsset(OutsideTargetPath);
    };

    UMaterial* Target = MakeMaterial(OutsideTargetPath);
    if (!TestNotNull(TEXT("the redirect target was created"), Target)) return false;
    if (!TestNotNull(TEXT("the redirector was created"),
            MakeRedirector(OutsideOldPath, Target))) return false;

    const FARFilter EmptyScope = RedirectorFixupPolicy::BuildSweepFilter(
        TArray<FString>(), RedirectorFixupPolicy::ESweepScope::RequestedPaths);

    TestTrue(TEXT("an empty request still constrains the package path"),
        EmptyScope.PackagePaths.Num() > 0);

    const TArray<FString> Matches = RedirectorPackagesMatching(EmptyScope);
    TestFalse(TEXT("an empty request matches no redirector at all"),
        Matches.Contains(OutsideOldPath));
    TestEqual(TEXT("an empty request matches nothing in the project"), Matches.Num(), 0);

    // The folder resolver behind it, in the two spellings a caller may send.
    const TArray<FName> Folders = RedirectorFixupPolicy::PackageFoldersForAssets(
        { PackagePathFor(InsideFolder, TEXT("M_A")),
          ObjectPathFor(InsideFolder, TEXT("M_B")) });
    TestEqual(TEXT("both spellings collapse to one folder"), Folders.Num(), 1);
    TestEqual(TEXT("and it is the asset's own folder"),
        Folders.IsValidIndex(0) ? Folders[0].ToString() : FString(), FString(InsideFolder));

    // A sibling whose name merely starts with the scoped folder's name is OUTSIDE it.
    TestTrue(TEXT("a package in the scoped folder is inside it"),
        RedirectorFixupPolicy::IsInPackageFolders(PackagePathFor(InsideFolder, TEXT("M_C")), Folders));
    TestFalse(TEXT("a string-prefix sibling folder is NOT inside it"),
        RedirectorFixupPolicy::IsInPackageFolders(
            FString::Printf(TEXT("%sOther/M_D"), InsideFolder), Folders));
    TestFalse(TEXT("a child folder is NOT inside a non-recursive scope"),
        RedirectorFixupPolicy::IsInPackageFolders(
            FString::Printf(TEXT("%s/Nested/M_E"), InsideFolder), Folders));

    return true;
}

// ============================================================================
// An unrecognised fixupScope must be an error, and it must be raised BEFORE anything is
// deleted - a scope typo that costs the caller the irreversible half of the operation is
// worse than no parameter at all.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteUnknownScopeRejectedTest,
    "PinWright.asset.bulk_delete.UnknownFixupScopeIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteUnknownScopeRejectedTest::RunTest(const FString& /*Parameters*/)
{
    using namespace BulkDeleteScopeTest;

    FSuppression Suppression;

    const FString SurvivorPath = PackagePathFor(InsideFolder, TEXT("M_TypoSurvivor"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SurvivorPath);
    };

    if (!TestNotNull(TEXT("the asset named in the rejected call was created"),
            MakeMaterial(SurvivorPath))) return false;

    TArray<TSharedPtr<FJsonValue>> PathsToDelete;
    PathsToDelete.Add(MakeShared<FJsonValueString>(SurvivorPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assetPaths"), PathsToDelete);
    Payload->SetStringField(TEXT("fixupScope"), TEXT("everything"));

    FTestResponseCapture Capture;
    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("asset.bulk_delete handler found"),
            InvokeHandlerWithCapture(TEXT("asset.bulk_delete"), Payload, Capture));
    }

    TestTrue(TEXT("the call responded"), Capture.bWasCalled);
    TestFalse(TEXT("an unknown scope is refused, not defaulted"), Capture.bSuccess);
    TestEqual(TEXT("refused as bad caller input"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the message names the parameter"),
        Capture.Message.Contains(TEXT("fixupScope")));

    // Rejected before the delete: the named asset is untouched.
    TestNotNull(TEXT("nothing was deleted by the rejected call"),
        FindObject<UMaterial>(nullptr, *ObjectPathFor(InsideFolder, TEXT("M_TypoSurvivor"))));

    // Both accepted spellings parse, case-insensitively, and nothing else does.
    RedirectorFixupPolicy::ESweepScope Parsed = RedirectorFixupPolicy::ESweepScope::Project;
    TestTrue(TEXT("'paths' parses"), RedirectorFixupPolicy::ParseSweepScope(TEXT("paths"), Parsed));
    TestTrue(TEXT("'paths' is the narrow scope"),
        Parsed == RedirectorFixupPolicy::ESweepScope::RequestedPaths);
    TestTrue(TEXT("'Project' parses case-insensitively"),
        RedirectorFixupPolicy::ParseSweepScope(TEXT("Project"), Parsed));
    TestTrue(TEXT("'Project' is the wide scope"),
        Parsed == RedirectorFixupPolicy::ESweepScope::Project);
    TestFalse(TEXT("an empty string does not parse"),
        RedirectorFixupPolicy::ParseSweepScope(FString(), Parsed));
    TestFalse(TEXT("'all' does not parse"),
        RedirectorFixupPolicy::ParseSweepScope(TEXT("all"), Parsed));

    return true;
}

// ============================================================================
// Declaration half. The dispatcher's unknown-parameter gate runs before the handler body,
// so a parameter the body reads but the FParamSpec does not declare is rejected with
// UNKNOWN_PARAMS and can never be dialled - the defect shape docs/rpc-design.md §3
// catalogues. InvokeHandler bypasses that gate, so the body tests above cannot see it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBulkDeleteFixupScopeDeclaredTest,
    "PinWright.asset.bulk_delete.FixupScopeIsDeclaredWithPathsDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBulkDeleteFixupScopeDeclaredTest::RunTest(const FString& /*Parameters*/)
{
    const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("asset.bulk_delete"), TEXT("fixupScope"));
    if (!TestNotNull(TEXT("asset.bulk_delete declares fixupScope"), Spec)) return false;

    TestEqual(TEXT("declared as a string"), Spec->Type, FString(TEXT("string")));
    TestFalse(TEXT("optional, so existing callers keep working"), Spec->bRequired);
    TestEqual(TEXT("and it defaults to the narrow scope"), Spec->Default, FString(TEXT("paths")));
    TestTrue(TEXT("the description names the wide opt-in"),
        Spec->Description.Contains(TEXT("project")));

    return true;
}
