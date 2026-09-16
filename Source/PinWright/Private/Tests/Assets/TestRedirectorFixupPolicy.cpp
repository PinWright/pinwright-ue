// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the asset.bulk_delete / asset.fixup_redirectors editor kill
// reproduced 2026-08-19 16:05:49.
//
// IAssetTools::FixupReferencers ends in an unconditional modal report
// (AssetFixUpRedirectors.cpp:938-939) whose answer is read with an unchecked
// TOptional::GetValue() (Dialogs.h:280). Under GIsRunningUnattendedScript Slate cancels
// the window, the OnFinished delegate never fires, and GetValue() hard-asserts at
// Optional.h:372 - the editor process dies. RedirectorFixupPolicy is the non-interactive
// replacement.
//
// COUNTERFACTUAL, and the reason these tests are worth writing: swap
// RedirectorFixupPolicy::FixupReferencers for IAssetTools::FixupReferencers in
// FRedirectorFixupPolicyNoReferencersTest and the test does not fail - it takes the whole
// automation process down with it. A single live redirector is enough: the
// `!bAnyRefs && !bMayDeleteRedirectors` early-out at AssetFixUpRedirectors.cpp:602 cannot
// fire, because the default ERedirectFixupMode::DeleteFixedUpRedirectors forces
// bMayDeleteRedirectors true at :584. So "the suite finished" IS the assertion, and it is
// one no amount of registration-checking could have made.
//
// What is NOT asserted, because it is not observable: that no dialog was raised. The
// harness suppresses dialogs before any delegate fires, so the machine-checkable proxy is
// the same one TestAssetCreatePolicy.cpp uses - GIsRunningUnattendedScript held for the
// duration of the call.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSettings.h"
#include "Tests/TestUtils.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "Utils/RedirectorFixupPolicy.h"

// File-scope helper names carry the RedirectorFixupPolicyTest_ prefix: Unity merges
// translation units, so a plain UniquePath() would collide with a same-named static in
// another test file.

static FString RedirectorFixupPolicyTest_UniquePackagePath(const TCHAR* Prefix)
{
    return FString::Printf(TEXT("/Game/PinWrightTests/RedirectorFixupPolicy/%s_%s"),
        Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

static UMaterial* RedirectorFixupPolicyTest_MakeMaterial(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    return NewObject<UMaterial>(
        Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
        RF_Public | RF_Standalone);
}

// The shape asset.rename leaves behind: a package holding nothing but a redirector that
// points at the moved asset. Built in memory - the crash needs only a live redirector,
// and keeping it off disk keeps the test from depending on registry scan timing.
static UObjectRedirector* RedirectorFixupPolicyTest_MakeRedirector(
    const FString& PackagePath, UObject* Destination)
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
    }
    return Redirector;
}

// Forces the modal-suppression kill switch on for a test body and restores both it and
// the process-global afterwards, so an interactively-configured editor cannot make an
// assertion pass or fail for the wrong reason.
struct FRedirectorFixupPolicyTestSuppression
{
    UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
    bool bSavedSetting = false;
    bool bSavedGlobal = GIsRunningUnattendedScript;

    FRedirectorFixupPolicyTestSuppression()
    {
        PinWrightAutomationMode::ResetForTests();
        if (Settings)
        {
            bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
            Settings->bSuppressModalDialogsDuringRpc = true;
        }
        GIsRunningUnattendedScript = false;
    }

    ~FRedirectorFixupPolicyTestSuppression()
    {
        PinWrightAutomationMode::ResetForTests();
        if (Settings)
        {
            Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
        }
        GIsRunningUnattendedScript = bSavedGlobal;
    }
};

// -----------------------------------------------------------------------------
// One live redirector, nothing referencing it. This is the minimum reproduction of the
// editor kill: the engine call reaches the modal report here too. The policy completes,
// reports the redirector as considered and fully fixed, and deletes it.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRedirectorFixupPolicyNoReferencersTest,
    "PinWright.assets.RedirectorFixupPolicy.LiveRedirectorDoesNotKillTheEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRedirectorFixupPolicyNoReferencersTest::RunTest(const FString& /*Parameters*/)
{
    FRedirectorFixupPolicyTestSuppression Suppression;

    const FString TargetPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("M_Target"));
    const FString RedirectorPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("M_Old"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(RedirectorPath);
        CleanupTestAsset(TargetPath);
    };

    UMaterial* Target = RedirectorFixupPolicyTest_MakeMaterial(TargetPath);
    if (!TestNotNull(TEXT("redirect target created"), Target)) return false;
    UObjectRedirector* Redirector = RedirectorFixupPolicyTest_MakeRedirector(RedirectorPath, Target);
    if (!TestNotNull(TEXT("redirector created"), Redirector)) return false;

    RedirectorFixupPolicy::FResult Fixup;
    {
        FScopedUnattendedRpc UnattendedScope;
        TestTrue(TEXT("GIsRunningUnattendedScript held for the whole fixup"),
            GIsRunningUnattendedScript);

        Fixup = RedirectorFixupPolicy::FixupReferencers(
            { Redirector }, /*bDeleteFixedUpRedirectors=*/true);
    }

    TestEqual(TEXT("the redirector was considered"), Fixup.RedirectorsConsidered, 1);
    TestEqual(TEXT("nothing was skipped"), Fixup.SkippedRedirectors, 0);
    TestEqual(TEXT("no referencing packages found"), Fixup.ReferencingPackagesFound, 0);
    TestTrue(TEXT("no referencer failed"), Fixup.AllReferencersSaved());
    TestTrue(TEXT("the fully fixed-up redirector was deleted"), Fixup.RedirectorsDeleted >= 1);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    RedirectorFixupPolicy::AddReport(Report, Fixup);
    double Considered = 0.0;
    TestTrue(TEXT("report carries redirectorsConsidered"),
        Report->TryGetNumberField(TEXT("redirectorsConsidered"), Considered));
    TestEqual(TEXT("report agrees with the result"), static_cast<int32>(Considered), 1);
    TestFalse(TEXT("a clean run reports no failedPackages array"),
        Report->HasField(TEXT("failedPackages")));

    return true;
}

// -----------------------------------------------------------------------------
// A redirector pointing nowhere must be reported as skipped, never as fixed. Deleting it
// would be indistinguishable from deleting a repaired one, and it is the case a future
// "just treat it as done" simplification would get wrong.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRedirectorFixupPolicyNullDestinationTest,
    "PinWright.assets.RedirectorFixupPolicy.NullDestinationIsSkippedNotDeleted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRedirectorFixupPolicyNullDestinationTest::RunTest(const FString& /*Parameters*/)
{
    FRedirectorFixupPolicyTestSuppression Suppression;

    const FString RedirectorPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("M_Dangling"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(RedirectorPath);
    };

    UObjectRedirector* Redirector =
        RedirectorFixupPolicyTest_MakeRedirector(RedirectorPath, nullptr);
    if (!TestNotNull(TEXT("dangling redirector created"), Redirector)) return false;

    RedirectorFixupPolicy::FResult Fixup;
    {
        FScopedUnattendedRpc UnattendedScope;
        Fixup = RedirectorFixupPolicy::FixupReferencers(
            { Redirector }, /*bDeleteFixedUpRedirectors=*/true);
    }

    TestEqual(TEXT("nothing was considered"), Fixup.RedirectorsConsidered, 0);
    TestEqual(TEXT("the dangling redirector was skipped"), Fixup.SkippedRedirectors, 1);
    TestEqual(TEXT("nothing was deleted"), Fixup.RedirectorsDeleted, 0);
    TestTrue(TEXT("the dangling redirector still exists"), IsValid(Redirector));

    return true;
}

// -----------------------------------------------------------------------------
// A Blueprint destination needs three rewrite entries, not one: soft references hold the
// asset path, the generated class path (`_C`) and the CDO path. Dropping the last two is
// silent - the reference resolves to nothing only once the redirector is gone.
// Mirrors AssetFixUpRedirectors.cpp:855-860.
// -----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRedirectorFixupPolicyBlueprintPathsTest,
    "PinWright.assets.RedirectorFixupPolicy.BlueprintDestinationGetsClassAndCdoSpellings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRedirectorFixupPolicyBlueprintPathsTest::RunTest(const FString& /*Parameters*/)
{
    const FString MaterialTargetPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("M_Target"));
    const FString MaterialOldPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("M_Old"));
    const FString BlueprintTargetPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("BP_Target"));
    const FString BlueprintOldPath = RedirectorFixupPolicyTest_UniquePackagePath(TEXT("BP_Old"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MaterialOldPath);
        CleanupTestAsset(MaterialTargetPath);
        CleanupTestAsset(BlueprintOldPath);
        CleanupTestAsset(BlueprintTargetPath);
    };

    UMaterial* MaterialTarget = RedirectorFixupPolicyTest_MakeMaterial(MaterialTargetPath);
    if (!TestNotNull(TEXT("material target created"), MaterialTarget)) return false;
    UObjectRedirector* MaterialRedirector =
        RedirectorFixupPolicyTest_MakeRedirector(MaterialOldPath, MaterialTarget);
    if (!TestNotNull(TEXT("material redirector created"), MaterialRedirector)) return false;

    UPackage* BlueprintPackage = CreatePackage(*BlueprintTargetPath);
    if (!TestNotNull(TEXT("blueprint package created"), BlueprintPackage)) return false;
    const FString BlueprintName = FPackageName::GetLongPackageAssetName(BlueprintTargetPath);
    UBlueprint* BlueprintTarget = NewObject<UBlueprint>(
        BlueprintPackage, FName(*BlueprintName), RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("blueprint target created"), BlueprintTarget)) return false;
    UObjectRedirector* BlueprintRedirector =
        RedirectorFixupPolicyTest_MakeRedirector(BlueprintOldPath, BlueprintTarget);
    if (!TestNotNull(TEXT("blueprint redirector created"), BlueprintRedirector)) return false;

    const TMap<FSoftObjectPath, FSoftObjectPath> Map =
        RedirectorFixupPolicy::BuildRedirectorMap({ MaterialRedirector, BlueprintRedirector });

    TestEqual(TEXT("one entry for the material, three for the blueprint"), Map.Num(), 4);

    const FSoftObjectPath MaterialOld(MaterialRedirector);
    const FSoftObjectPath* MaterialNew = Map.Find(MaterialOld);
    if (TestNotNull(TEXT("the material old path is mapped"), MaterialNew))
    {
        TestEqual(TEXT("the material maps to its destination"),
            MaterialNew->ToString(), FSoftObjectPath(MaterialTarget).ToString());
    }

    const FSoftObjectPath BlueprintOld(BlueprintRedirector);
    const FSoftObjectPath BlueprintNew(BlueprintTarget);
    TestTrue(TEXT("the blueprint asset path is mapped"), Map.Contains(BlueprintOld));

    const FSoftObjectPath* ClassEntry =
        Map.Find(FSoftObjectPath(FString::Printf(TEXT("%s_C"), *BlueprintOld.ToString())));
    if (TestNotNull(TEXT("the blueprint generated class path is mapped"), ClassEntry))
    {
        TestEqual(TEXT("the class path maps to the destination class path"),
            ClassEntry->ToString(), FString::Printf(TEXT("%s_C"), *BlueprintNew.ToString()));
    }

    const FSoftObjectPath* CdoEntry = Map.Find(FSoftObjectPath(FString::Printf(
        TEXT("%s.Default__%s_C"), *BlueprintOld.GetLongPackageName(), *BlueprintOld.GetAssetName())));
    if (TestNotNull(TEXT("the blueprint CDO path is mapped"), CdoEntry))
    {
        TestEqual(TEXT("the CDO path maps to the destination CDO path"),
            CdoEntry->ToString(),
            FString::Printf(TEXT("%s.Default__%s_C"),
                *BlueprintNew.GetLongPackageName(), *BlueprintNew.GetAssetName()));
    }

    return true;
}
