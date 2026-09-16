// Copyright (c) 2026 Alexander Penkin. MIT License.

// Coverage for the PIE-blocked save report (board B-asset-save-pie-failure-reports-pendingflush
// and its duplicate B-asset-save-omits-savestate-pie-block).
//
// The defect: while any agent held PIE, the editor refused every single-asset write in the
// process, and asset.save answered {saved:false, sizeBytes:<stale>, pendingFlush:true} with NO
// saveState - the exact payload the throttle emits, whose documented remedy is the retry that
// cannot possibly work. The handler had already computed state=failed and logged it. This file
// guards the four parts of the corrected contract:
//
//   1. the response shape - asset.save must always carry saveState/saveDetail, so the documented
//      "read saveState before retrying" procedure has a field to read;
//   2. the blocker - a PIE block must be published as pieActive/editorMode/pieWorlds, which is
//      the only way a caller learns that somebody ELSE's play session is the cause;
//   3. sizeBytes - a non-durable save reports the file already on disk, which reads as proof of
//      a write, so it must be labelled.
//   4. refusal - a measured PIE block is PIE_ACTIVE with pendingFlush:false, not success with
//      work that appears queued.
//
// What is NOT tested here, deliberately: a live PIE session. Starting PIE under -unattended
// walks dirty transient Blueprints left by sibling tests and can crash the suite (see
// TestUtils.h's PIE-request cancel helper). Payload-only tests drive the guard data directly;
// the handler regression scopes GIsPlayInEditorWorld, one half of the exact two-global engine
// predicate, and exercises the real asset.save path without creating a live PIE world.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "State/PluginState.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"
#include "Utils/PieSaveBlockGuard.h"

#include "Tests/TestUtils.h"

// Prefixed and file-static: Unity merges this TU with its neighbours, so an unprefixed helper
// collides (see CLAUDE.md > Building).
static FString PieSaveBlockTest_UniquePackagePath()
{
    return FString::Printf(TEXT("/Game/PinWrightTests/AssetSavePieBlock/M_PieBlock_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

static UMaterial* PieSaveBlockTest_MakeMaterial(const FString& PackagePath)
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
        Material->MarkPackageDirty();
        // UEditorAssetSubsystem::SaveLoadedAsset refuses an asset the registry has never heard
        // of, so a fixture that skips this exercises the wrong failure branch.
        FAssetRegistryModule::AssetCreated(Material);
    }
    return Material;
}

static FString PieSaveBlockTest_Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    return (Object.IsValid() && Object->TryGetStringField(Field, Value)) ? Value : FString(TEXT("<absent>"));
}

// ============================================================================
// The blocker payload
// ============================================================================

// A measured block must publish enough for a caller to act WITHOUT filesystem access to the
// editor log - which is the whole point, because an agent driving the editor purely over MCP
// has none. Named world, named map, and the same pieActive/editorMode spelling editor.save_all
// already uses, so the two save verbs answer the environmental question in one vocabulary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieSaveBlockJsonNamesTheSessionTest,
    "PinWright.assets.PieSaveBlock.ActiveBlockNamesTheSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieSaveBlockJsonNamesTheSessionTest::RunTest(const FString& /*Parameters*/)
{
    PinWrightPieSaveBlock::FPieSaveBlock Block;
    Block.bActive = true;
    PinWrightPieSaveBlock::FPieWorldIdentity World;
    World.PieInstance = 0;
    World.MapName = TEXT("T_UI");
    World.WorldPath = TEXT("/Game/Test/UEDPIE_0_T_UI.T_UI");
    Block.Worlds.Add(World);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWrightPieSaveBlock::AddPieSaveBlockJson(Result, Block);

    bool bPieActive = false;
    TestTrue(TEXT("pieActive is present"), Result->TryGetBoolField(TEXT("pieActive"), bPieActive));
    TestTrue(TEXT("pieActive is true"), bPieActive);
    TestEqual(TEXT("editorMode matches editor.save_all's spelling"),
        PieSaveBlockTest_Str(Result, TEXT("editorMode")), FString(TEXT("PIE")));

    const TArray<TSharedPtr<FJsonValue>>* Worlds = nullptr;
    if (TestTrue(TEXT("pieWorlds is present"), Result->TryGetArrayField(TEXT("pieWorlds"), Worlds))
        && Worlds && Worlds->Num() == 1)
    {
        const TSharedPtr<FJsonObject> Entry = (*Worlds)[0]->AsObject();
        if (TestTrue(TEXT("pieWorlds entry is an object"), Entry.IsValid()))
        {
            TestEqual(TEXT("pieWorlds names the map"),
                PieSaveBlockTest_Str(Entry, TEXT("mapName")), FString(TEXT("T_UI")));
            TestEqual(TEXT("pieWorlds names the world path"),
                PieSaveBlockTest_Str(Entry, TEXT("worldPath")),
                FString(TEXT("/Game/Test/UEDPIE_0_T_UI.T_UI")));
            double Instance = -1.0;
            TestTrue(TEXT("pieWorlds carries the instance"),
                Entry->TryGetNumberField(TEXT("pieInstance"), Instance));
            TestEqual(TEXT("pieWorlds instance is the gathered one"), static_cast<int32>(Instance), 0);
        }
    }

    // The description feeds the log line and any refusal message; it must name the world rather
    // than saying "a save failed".
    const FString Description = PinWrightPieSaveBlock::DescribePieSaveBlock(Block);
    TestTrue(*FString::Printf(TEXT("description names play mode: '%s'"), *Description),
        Description.Contains(TEXT("play mode")));
    TestTrue(*FString::Printf(TEXT("description names the map: '%s'"), *Description),
        Description.Contains(TEXT("T_UI")));
    return true;
}

// The absence contract, and it is load-bearing: safe-mutation-save.md tells callers that on a
// requested-but-not-durable save, NO pieActive means PIE was not running. An inactive block that
// wrote anything - even pieActive:false - would make every ordinary save report noisier, and an
// inactive block that wrote pieWorlds would let an empty list read as a measurement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPieSaveBlockInactiveWritesNothingTest,
    "PinWright.assets.PieSaveBlock.InactiveBlockWritesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPieSaveBlockInactiveWritesNothingTest::RunTest(const FString& /*Parameters*/)
{
    PinWrightPieSaveBlock::FPieSaveBlock Block; // bActive defaults false
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWrightPieSaveBlock::AddPieSaveBlockJson(Result, Block);

    TestEqual(TEXT("an inactive block adds no fields at all"), Result->Values.Num(), 0);
    TestTrue(TEXT("an inactive block has no description"),
        PinWrightPieSaveBlock::DescribePieSaveBlock(Block).IsEmpty());
    return true;
}

// ============================================================================
// sizeBytes honesty
// ============================================================================

// The field that did the most damage: on a blocked write over an existing asset it reported the
// STALE on-disk size, so the payload read as a real write of a plausibly-sized package, and a
// caller comparing it against a known-good earlier save saw an identical number. The number is a
// true measurement of the file and stays; the label is what was missing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveSizeReportLabelsStaleBytesTest,
    "PinWright.assets.AssetSaveReport.StaleSizeBytesAreLabelled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveSizeReportLabelsStaleBytesTest::RunTest(const FString& /*Parameters*/)
{
    // Durable: the count describes what this call wrote, so no caveat.
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetSaveSizeReport(Result, 108204, /*bSavedToDisk=*/true);
        double Size = 0.0;
        TestTrue(TEXT("sizeBytes is present on a durable save"),
            Result->TryGetNumberField(TEXT("sizeBytes"), Size));
        TestEqual(TEXT("sizeBytes is the measured size"), static_cast<int64>(Size), (int64)108204);
        TestFalse(TEXT("a durable save carries no staleness caveat"),
            Result->HasField(TEXT("sizeBytesIsStale")));
    }

    // Not durable over an existing file: the exact case the board reported, twice, on two
    // different assets. The number survives, the claim it implied does not.
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetSaveSizeReport(Result, 108204, /*bSavedToDisk=*/false);
        double Size = 0.0;
        TestTrue(TEXT("sizeBytes is still reported"),
            Result->TryGetNumberField(TEXT("sizeBytes"), Size));
        TestEqual(TEXT("sizeBytes is still the measured on-disk size"),
            static_cast<int64>(Size), (int64)108204);
        bool bStale = false;
        TestTrue(TEXT("a non-durable save over an existing file labels the size stale"),
            Result->TryGetBoolField(TEXT("sizeBytesIsStale"), bStale));
        TestTrue(TEXT("the staleness label is true"), bStale);
    }

    // Not durable with no file behind it: zero is not a stale revision, and saying it is would
    // claim a previous save that never happened.
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddAssetSaveSizeReport(Result, 0, /*bSavedToDisk=*/false);
        TestFalse(TEXT("a zero size carries no staleness claim"),
            Result->HasField(TEXT("sizeBytesIsStale")));
    }
    return true;
}

// ============================================================================
// asset.save's response shape
// ============================================================================

// The regression guard for the ticket itself, driven through the real handler.
//
// asset.save used to publish {saved, sizeBytes} plus a bare pendingFlush and drop the state it
// had already measured, so the documented decision procedure had no field to read. Both branches
// are asserted here, on one real package: a forced write (durable) and a throttled write over a
// dirty package (not durable). Pre-fix the second branch is exactly the payload the board
// quotes - saved:false, pendingFlush:true, no saveState - and the state assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSaveAlwaysReportsSaveStateTest,
    // NOT "PinWright.asset.save.<...>": that id is a complete leaf (TestAssetSaveHandler.cpp),
    // and a dotted suffix under it would adopt it as a branch node and silently drop it from
    // the queue (CLAUDE.md > Testing, PinWright.infra.automation_registry.NoPrefixCollisions).
    "PinWright.assets.AssetSaveHandler.ResponseAlwaysCarriesSaveState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveAlwaysReportsSaveStateTest::RunTest(const FString& /*Parameters*/)
{
    const FString PackagePath = PieSaveBlockTest_UniquePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterial* Material = PieSaveBlockTest_MakeMaterial(PackagePath);
    if (!TestNotNull(TEXT("probe material created"), Material))
    {
        return false;
    }

    // LoadObject inside the handler resolves the ObjectPath form against the in-memory asset.
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath,
        *FPackageName::GetLongPackageAssetName(PackagePath));

    // Widen the throttle so the second call is reliably inside one window on any machine.
    // Process-wide state, so it is restored on the way out.
    double& ThrottleSeconds = FPluginState::Get().SaveThrottle().ThrottleSecondsRef();
    const double PreviousThrottleSeconds = ThrottleSeconds;
    ThrottleSeconds = 30.0;
    ON_SCOPE_EXIT { ThrottleSeconds = PreviousThrottleSeconds; };

    // --- 1. A forced save that writes. ---
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), ObjectPath);
        Params->SetBoolField(TEXT("force"), true);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);
        if (!TestTrue(TEXT("asset.save responded"), Capture.bWasCalled)
            || !TestTrue(TEXT("asset.save succeeded"), Capture.bSuccess)
            || !TestTrue(TEXT("asset.save returned a result"), Capture.Result.IsValid()))
        {
            return false;
        }

        bool bSaved = false;
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
        if (!TestTrue(TEXT("the forced save reached disk"), bSaved))
        {
            // Without a write there is no durable branch to assert; the deferred branch below
            // depends on this one having cleared the dirty flag.
            return false;
        }
        TestEqual(TEXT("a durable save reports written"),
            PieSaveBlockTest_Str(Capture.Result, TEXT("saveState")), FString(TEXT("written")));
        TestFalse(TEXT("a durable save owes no flush"),
            Capture.Result->HasField(TEXT("pendingFlush")));
        TestFalse(TEXT("a durable save's size is not stale"),
            Capture.Result->HasField(TEXT("sizeBytesIsStale")));
        bool bSaveRequested = false;
        TestTrue(TEXT("asset.save echoes saveRequested like the rest of the save family"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
        TestTrue(TEXT("asset.save always requests a save"), bSaveRequested);
    }

    // --- 2. Dirty again, unforced, inside the throttle window: not durable. ---
    //
    // THE assertion of this branch: a throttled asset.save must name the deferred state rather
    // than returning the pre-fix payload with no state.
    {
        Material->MarkPackageDirty();

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), ObjectPath);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);
        if (!TestTrue(TEXT("throttled asset.save responded"), Capture.bWasCalled)
            || !TestTrue(TEXT("throttled asset.save succeeded"), Capture.bSuccess)
            || !TestTrue(TEXT("throttled asset.save returned a result"), Capture.Result.IsValid()))
        {
            return false;
        }

        bool bSaved = true;
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
        TestFalse(TEXT("a throttled save over a dirty package is not durable"), bSaved);
        bool bPendingFlush = false;
        TestTrue(TEXT("pendingFlush is still emitted, so no existing caller moves"),
            Capture.Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush) && bPendingFlush);

        const FString State = PieSaveBlockTest_Str(Capture.Result, TEXT("saveState"));
        TestNotEqual(TEXT("a not-durable asset.save carries a saveState"),
            State, FString(TEXT("<absent>")));
        TestFalse(TEXT("a not-durable asset.save carries a saveDetail"),
            PieSaveBlockTest_Str(Capture.Result, TEXT("saveDetail")).IsEmpty());
        TestEqual(TEXT("a throttled save reports deferred"), State, FString(TEXT("deferred")));

        // The stale-size label rides the same response. The file from step 1 is on disk, so the
        // count is non-zero and is the previous revision's.
        double Size = 0.0;
        Capture.Result->TryGetNumberField(TEXT("sizeBytes"), Size);
        if (Size > 0.0)
        {
            bool bStale = false;
            TestTrue(TEXT("the stale on-disk size is labelled on a not-durable save"),
                Capture.Result->TryGetBoolField(TEXT("sizeBytesIsStale"), bStale) && bStale);
        }

        TestFalse(TEXT("a throttle-only response does not claim PIE is active"),
            Capture.Result->HasField(TEXT("pieActive")));
    }

    // Leave the fixture clean so teardown does not trip over a dirty package.
    SaveAssetToDiskReportingPresence(Material, /*bForce=*/true);
    return true;
}

// Simulate the exact engine PIE predicate without starting a play session. Reverting either the
// handler refusal or the BlockedByPie pendingFlush rule makes this test fail on the real
// asset.save production path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSavePieBlockIsTypedFailureTest,
    "PinWright.assets.AssetSaveHandler.PieBlockIsTypedFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSavePieBlockIsTypedFailureTest::RunTest(const FString& /*Parameters*/)
{
    const FString PackagePath = PieSaveBlockTest_UniquePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterial* Material = PieSaveBlockTest_MakeMaterial(PackagePath);
    if (!TestNotNull(TEXT("PIE-block fixture material created"), Material))
    {
        return false;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath,
        *FPackageName::GetLongPackageAssetName(PackagePath));
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), ObjectPath);
    Params->SetBoolField(TEXT("force"), true);

    TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.save handler found"),
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture));
    if (!TestTrue(TEXT("asset.save responded"), Capture.bWasCalled)
        || !TestTrue(TEXT("PIE refusal carries result data"), Capture.Result.IsValid()))
    {
        return false;
    }

    TestFalse(TEXT("PIE-blocked asset.save is an error"), Capture.bSuccess);
    TestEqual(TEXT("PIE-blocked asset.save carries a typed error"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_PIE_ACTIVE));

    bool bSaveRequested = false;
    bool bSaved = true;
    bool bPendingFlush = true;
    bool bPieActive = false;
    TestTrue(TEXT("PIE refusal reports saveRequested"),
        Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested));
    TestTrue(TEXT("PIE refusal records that a save was requested"), bSaveRequested);
    TestTrue(TEXT("PIE refusal reports saved"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestFalse(TEXT("PIE refusal reports saved:false"), bSaved);
    TestTrue(TEXT("PIE refusal reports pendingFlush"),
        Capture.Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush));
    TestFalse(TEXT("PIE refusal is not queued flush work"), bPendingFlush);
    TestEqual(TEXT("PIE refusal reports blockedByPie"),
        PieSaveBlockTest_Str(Capture.Result, TEXT("saveState")),
        FString(TEXT("blockedByPie")));
    TestFalse(TEXT("PIE refusal carries actionable saveDetail"),
        PieSaveBlockTest_Str(Capture.Result, TEXT("saveDetail")).IsEmpty());
    TestTrue(TEXT("PIE refusal reports pieActive"),
        Capture.Result->TryGetBoolField(TEXT("pieActive"), bPieActive));
    TestTrue(TEXT("PIE refusal reports pieActive:true"), bPieActive);
    TestEqual(TEXT("PIE refusal reports editor mode"),
        PieSaveBlockTest_Str(Capture.Result, TEXT("editorMode")), FString(TEXT("PIE")));
    TestTrue(TEXT("PIE refusal leaves the fixture dirty"), Material->GetPackage()->IsDirty());
    TestFalse(TEXT("PIE refusal writes no package file"),
        DoesPackageFileExistOnDisk(PackagePath));
    return true;
}
