// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for three tickets that all land on foliage.create_procedural's per-type
// property writes:
//
//   F-procedural-foliage-simulation-knobs-unreachable - the verb wrote six properties in
//     total and none of the fourteen Category=Procedural ones the tile simulation reads, so
//     ProceduralScale, InitialSeedDensity, the clustering radii, the age/growth set and
//     OverlapPriority were unreachable through the only verb that runs the simulation.
//   B-create-procedural-density-writes-paint-density - the one per-type knob the verb DID
//     advertise, `density`, wrote UFoliageType::Density (the Foliage-editor brush knob) while
//     FProceduralFoliageTile::Simulate seeds from InitialSeedDensity, which the handler never
//     wrote. Every generated type therefore simulated at the CDO 1.0 - one seed per 10 m tile
//     - whatever the caller asked for, and the response reported success.
//   B-create-procedural-ignores-scale-and-normal-fields (its reopened `#3`) - the scale write
//     landed on ScaleX/Y/Z, which FPotentialInstance::PlaceInstance reads only on the
//     NON-procedural branch; the procedural branch takes GetScaleForAge, which interpolates
//     ProceduralScale.
//
// WHY THE ASSERTIONS OPEN THE GENERATED ASSET. Both defects were invisible from the response:
// success:true, an accurate foliage_types_count and an ignoredFields array that (correctly,
// by the handler's own contract) did not list `density`, all while the number that decided
// the scatter was a CDO value the caller never saw. The load-bearing assertions below read
// the properties back off the UFoliageType_InstancedStaticMesh the handler built.
//
// THE ORDERING-DEPENDENT PAIR IS THE POINT OF THE FIRST TEST. MaxInitialAge and
// ProceduralScale interact: GetInitAge returns MaxInitialAge * random, so at the engine
// default of 0 every seed starts at age exactly 0, ages advance by integer 1 per step, and
// GetScaleForAge evaluates the curve at Age/MaxAge - collapsing any requested range to
// NumSteps+1 discrete sizes. A correct proceduralScale therefore reads as a no-op. The
// handler resolves MaxInitialAge AFTER MaxAge and ProceduralScale for exactly that reason and
// raises it to the effective MaxAge when the caller asked for a range and did not pin the age
// themselves; `MaxInitialAgeIsRaisedSoProceduralScaleIsNotANoOp` is the assertion on that.
//
// The requests route through the real dispatcher (FRpcDispatcher::ProcessRequest), not
// TestUtils' InvokeHandler, because part of the fix is the PARAM DECLARATION: the dispatcher's
// unknown-param gate answers UNKNOWN_PARAMS for tileOverlap / savePath unless RPC_PARAMS
// declares them, and InvokeHandler never runs that gate. Against the pre-fix handler these
// tests stop at that first assertion.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds
// enabled, where same-named anonymous-namespace helpers collide across merged translation
// units.
namespace FoliageCreateProceduralSimConfigTestHelpers
{
    constexpr const TCHAR* SimMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Every value below differs from the engine CDO on that property, so no assertion can
    // pass by accidentally matching a default: InitialSeedDensity 1, CollisionRadius 100,
    // ShadeRadius 100, NumSteps 3, SeedsPerStep 3, AverageSpreadDistance 50, MaxAge 10,
    // MaxInitialAge 0, OverlapPriority 0, bCanGrowInShade/bSpawnsInShade false,
    // RandomPitchAngle 0, ProceduralScale (1, 3), Height (-262144, 262144),
    // GroundSlopeAngle (0, 45).
    constexpr double CanopyDensity = 100.0;          // -> InitialSeedDensity sqrt(100) = 10
    constexpr double CanopyDerivedSeedDensity = 10.0;
    constexpr double CanopyCollisionRadius = 300.0;
    constexpr double CanopyShadeRadius = 450.0;
    constexpr double CanopyNumSteps = 5.0;
    constexpr double CanopySeedsPerStep = 7.0;
    constexpr double CanopySpreadDistance = 250.0;
    constexpr double CanopyMaxAge = 20.0;
    constexpr double CanopyOverlapPriority = 10.0;
    constexpr double CanopyRandomPitchAngle = 12.0;
    constexpr double CanopyProceduralScaleMin = 0.5;
    constexpr double CanopyProceduralScaleMax = 2.5;
    constexpr double CanopyHeightMin = -1000.0;
    constexpr double CanopyHeightMax = 5000.0;
    constexpr double CanopySlopeMin = 0.0;
    constexpr double CanopySlopeMax = 30.0;
    constexpr double CanopyMinScale = 0.4;
    constexpr double CanopyMaxScale = 2.5;

    constexpr double GroundCoverDensity = 400.0;
    constexpr double GroundCoverSeedDensity = 7.0; // supplied, so NOT sqrt(400) = 20
    constexpr double GroundCoverCollisionRadius = 60.0;
    constexpr double GroundCoverOverlapPriority = 1.0;

    constexpr double SimTileSize = 2500.0;
    constexpr int32 SimNumUniqueTiles = 3;
    constexpr double SimTileOverlap = 100.0;

    inline TSharedPtr<FJsonObject> MakeSimInterval(double Min, double Max)
    {
        TSharedPtr<FJsonObject> Interval = MakeShared<FJsonObject>();
        Interval->SetNumberField(TEXT("min"), Min);
        Interval->SetNumberField(TEXT("max"), Max);
        return Interval;
    }

    inline TSharedPtr<FJsonObject> MakeSimBounds()
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 0.0);
        Location->SetNumberField(TEXT("y"), 0.0);
        Location->SetNumberField(TEXT("z"), 0.0);

        TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
        Size->SetNumberField(TEXT("x"), 2000.0);
        Size->SetNumberField(TEXT("y"), 2000.0);
        Size->SetNumberField(TEXT("z"), 500.0);

        TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
        Bounds->SetObjectField(TEXT("location"), Location);
        Bounds->SetObjectField(TEXT("size"), Size);
        return Bounds;
    }

    // The handler only marks its packages dirty (McpSafeAssetSave is mark-dirty-only), so an
    // editor-wide save-all later in the suite would otherwise flush them into the host Content
    // tree. Delete the object and clear the dirty flag any emptied package is left carrying.
    // B-tests-leak-host-content.
    inline void CleanSimFoliagePackage(const FString& PackagePath)
    {
        CleanupTestAsset(PackagePath);
        if (UPackage* Remaining = FindPackage(nullptr, *PackagePath))
        {
            Remaining->SetDirtyFlag(false);
        }
    }

    // FindObject rather than LoadObject: the packages are dirty-only, never written to disk,
    // so the freshly built object is the only copy there is.
    inline UFoliageType_InstancedStaticMesh* FindGeneratedSimType(const FString& PackagePath)
    {
        return FindObject<UFoliageType_InstancedStaticMesh>(nullptr, *ToObjectPath(PackagePath));
    }

    inline TArray<FString> CollectWarnings(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("warnings"), Array) && Array)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                Out.Add(Value->AsString());
            }
        }
        return Out;
    }

    inline bool AnyWarningContains(const TArray<FString>& Warnings, const TCHAR* Needle)
    {
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }

    // One row of the response's foliage_types[] echo, by its `index` field.
    inline TSharedPtr<FJsonObject> FindTypeEchoRow(const TSharedPtr<FJsonObject>& Result,
        int32 EntryIndex)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("foliage_types"), Rows) || !Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            double Index = -1.0;
            if (Value->TryGetObject(Row) && Row && (*Row)->TryGetNumberField(TEXT("index"), Index)
                && static_cast<int32>(Index) == EntryIndex)
            {
                return *Row;
            }
        }
        return nullptr;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens FoliageCreateProceduralSimConfigTestHelpers inside its own body
// rather than at file scope: a file-scope using-directive would leak into every other test
// .cpp that Unity merges after this one into the same translation unit.

// ============================================================================
// The simulation properties reach the generated type, `density` stops being the only knob,
// and the MaxInitialAge / ProceduralScale ordering produces the caller's intent.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralAppliesSimConfigTest,
    "PinWright.foliage.create_procedural.AppliesSimulationConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralAppliesSimConfigTest::RunTest(const FString& Parameters)
{
    using namespace FoliageCreateProceduralSimConfigTestHelpers;

    // Destroys the AProceduralFoliageVolume the handler spawns and restores the level's
    // dirty flag, so the open map is left as found.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString VolumeName = FString::Printf(TEXT("PW_ProcFoliageSim_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SpawnerPackage =
        FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName);
    const FString CanopyPackage = SpawnerPackage + TEXT("_FT_0");
    const FString GroundCoverPackage = SpawnerPackage + TEXT("_FT_1");

    ON_SCOPE_EXIT
    {
        CleanSimFoliagePackage(CanopyPackage);
        CleanSimFoliagePackage(GroundCoverPackage);
        CleanSimFoliagePackage(SpawnerPackage);
    };

    // Entry 0: the large species. Supplies a proceduralScale range and NO maxInitialAge, so
    // the ordering rule must raise the age; supplies `density` and NO initialSeedDensity, so
    // the seed density must be derived rather than left at the CDO 1.0.
    TSharedPtr<FJsonObject> Canopy = MakeShared<FJsonObject>();
    Canopy->SetStringField(TEXT("meshPath"), SimMeshPath);
    Canopy->SetNumberField(TEXT("density"), CanopyDensity);
    Canopy->SetNumberField(TEXT("minScale"), CanopyMinScale);
    Canopy->SetNumberField(TEXT("maxScale"), CanopyMaxScale);
    Canopy->SetBoolField(TEXT("alignToNormal"), false);
    Canopy->SetNumberField(TEXT("collisionRadius"), CanopyCollisionRadius);
    Canopy->SetNumberField(TEXT("shadeRadius"), CanopyShadeRadius);
    Canopy->SetNumberField(TEXT("numSteps"), CanopyNumSteps);
    Canopy->SetNumberField(TEXT("seedsPerStep"), CanopySeedsPerStep);
    Canopy->SetNumberField(TEXT("averageSpreadDistance"), CanopySpreadDistance);
    Canopy->SetNumberField(TEXT("maxAge"), CanopyMaxAge);
    Canopy->SetNumberField(TEXT("overlapPriority"), CanopyOverlapPriority);
    Canopy->SetBoolField(TEXT("canGrowInShade"), true);
    Canopy->SetBoolField(TEXT("spawnsInShade"), true);
    Canopy->SetNumberField(TEXT("randomPitchAngle"), CanopyRandomPitchAngle);
    Canopy->SetObjectField(TEXT("proceduralScale"),
        MakeSimInterval(CanopyProceduralScaleMin, CanopyProceduralScaleMax));
    Canopy->SetObjectField(TEXT("height"), MakeSimInterval(CanopyHeightMin, CanopyHeightMax));
    Canopy->SetObjectField(TEXT("groundSlopeAngle"),
        MakeSimInterval(CanopySlopeMin, CanopySlopeMax));

    // Entry 1: the small species. Supplies initialSeedDensity explicitly (so it must NOT be
    // derived from its density) and pins maxInitialAge to 0 alongside a proceduralScale range
    // (so the raise must NOT happen and the collapse must be warned about instead).
    TSharedPtr<FJsonObject> GroundCover = MakeShared<FJsonObject>();
    GroundCover->SetStringField(TEXT("meshPath"), SimMeshPath);
    GroundCover->SetNumberField(TEXT("density"), GroundCoverDensity);
    GroundCover->SetNumberField(TEXT("initialSeedDensity"), GroundCoverSeedDensity);
    GroundCover->SetNumberField(TEXT("collisionRadius"), GroundCoverCollisionRadius);
    GroundCover->SetNumberField(TEXT("shadeRadius"), GroundCoverCollisionRadius);
    GroundCover->SetNumberField(TEXT("overlapPriority"), GroundCoverOverlapPriority);
    GroundCover->SetNumberField(TEXT("maxInitialAge"), 0.0);
    GroundCover->SetObjectField(TEXT("proceduralScale"), MakeSimInterval(0.9, 1.6));

    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueObject>(Canopy));
    Types.Add(MakeShared<FJsonValueObject>(GroundCover));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), VolumeName);
    Params->SetObjectField(TEXT("bounds"), MakeSimBounds());
    Params->SetArrayField(TEXT("foliageTypes"), Types);
    Params->SetNumberField(TEXT("seed"), 4242.0);
    Params->SetNumberField(TEXT("tileSize"), SimTileSize);
    Params->SetNumberField(TEXT("numUniqueTiles"), SimNumUniqueTiles);
    Params->SetNumberField(TEXT("tileOverlap"), SimTileOverlap);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.create_procedural"),
        TEXT("req-foliage-create-procedural-sim-config"), Params, bSuccess, Result, ErrorCode);

    // Before the fix the run stops here: tileOverlap is undeclared, so the dispatcher's
    // unknown-param gate answers UNKNOWN_PARAMS.
    TestTrue(*FString::Printf(
        TEXT("foliage.create_procedural accepts the simulation params (errorCode '%s')"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return false;
    }

    UFoliageType_InstancedStaticMesh* CanopyType = FindGeneratedSimType(CanopyPackage);
    if (!TestNotNull(TEXT("the handler generated a foliage type for the large species"),
            CanopyType))
    {
        // Nothing was built (e.g. the engine cube did not load on this host), so every
        // property comparison below would be vacuous rather than discriminating.
        return false;
    }

    // ---- B-create-procedural-density-writes-paint-density ----
    // Density (the brush knob) keeps the caller's number, because foliage.add_type's painting
    // path and any later paint pass on this asset genuinely read it. InitialSeedDensity - the
    // ONLY input to FProceduralFoliageTile::Simulate's seed count - is derived from it with
    // the sqrt the differing units require. Pre-fix this reads the CDO 1.0.
    TestEqual(TEXT("Density still carries the requested paint density"),
        CanopyType->Density, static_cast<float>(CanopyDensity));
    TestEqual(TEXT("InitialSeedDensity is derived from density, not left at the CDO 1.0"),
        CanopyType->InitialSeedDensity, static_cast<float>(CanopyDerivedSeedDensity));

    // ---- B-create-procedural-ignores-scale-and-normal-fields #3 ----
    // ProceduralScale is what the procedural placement branch interpolates. ScaleX/Y/Z and
    // AlignToNormal must still land alongside it, not instead of it.
    TestEqual(TEXT("ProceduralScale.Min is the supplied minimum, not the CDO 1.0"),
        CanopyType->ProceduralScale.Min, static_cast<float>(CanopyProceduralScaleMin));
    TestEqual(TEXT("ProceduralScale.Max is the supplied maximum, not the CDO 3.0"),
        CanopyType->ProceduralScale.Max, static_cast<float>(CanopyProceduralScaleMax));
    TestEqual(TEXT("ScaleX.Min still carries minScale for the painting path"),
        CanopyType->ScaleX.Min, static_cast<float>(CanopyMinScale));
    TestEqual(TEXT("ScaleX.Max still carries maxScale for the painting path"),
        CanopyType->ScaleX.Max, static_cast<float>(CanopyMaxScale));
    TestFalse(TEXT("AlignToNormal is the supplied false, not the default true"),
        static_cast<bool>(CanopyType->AlignToNormal));

    // ---- The ordering-dependent pair ----
    // MaxInitialAge is resolved after MaxAge and ProceduralScale, and raised to the effective
    // MaxAge because the caller asked for a range without pinning the age. Without the raise
    // GetInitAge returns 0 for every seed and the requested range collapses to NumSteps+1
    // discrete sizes - the caller's correct input reading as a no-op.
    TestEqual(TEXT("MaxAge is the supplied value"),
        CanopyType->MaxAge, static_cast<float>(CanopyMaxAge));
    TestEqual(TEXT("MaxInitialAge was raised to MaxAge so proceduralScale is not a no-op"),
        CanopyType->MaxInitialAge, static_cast<float>(CanopyMaxAge));

    // ---- The remaining simulation knobs ----
    TestEqual(TEXT("CollisionRadius is the supplied value"),
        CanopyType->CollisionRadius, static_cast<float>(CanopyCollisionRadius));
    TestEqual(TEXT("ShadeRadius is the supplied value"),
        CanopyType->ShadeRadius, static_cast<float>(CanopyShadeRadius));
    TestEqual(TEXT("NumSteps is the supplied value"),
        CanopyType->NumSteps, static_cast<int32>(CanopyNumSteps));
    TestEqual(TEXT("SeedsPerStep is the supplied value"),
        CanopyType->SeedsPerStep, static_cast<int32>(CanopySeedsPerStep));
    TestEqual(TEXT("AverageSpreadDistance is the supplied value"),
        CanopyType->AverageSpreadDistance, static_cast<float>(CanopySpreadDistance));
    TestEqual(TEXT("OverlapPriority is the supplied value"),
        CanopyType->OverlapPriority, static_cast<float>(CanopyOverlapPriority));
    TestTrue(TEXT("bCanGrowInShade is the supplied true"), CanopyType->bCanGrowInShade);
    TestTrue(TEXT("bSpawnsInShade is the supplied true"), CanopyType->bSpawnsInShade);
    TestEqual(TEXT("RandomPitchAngle is the supplied value"),
        CanopyType->RandomPitchAngle, static_cast<float>(CanopyRandomPitchAngle));
    TestEqual(TEXT("Height.Min is the supplied value"),
        CanopyType->Height.Min, static_cast<float>(CanopyHeightMin));
    TestEqual(TEXT("Height.Max is the supplied value"),
        CanopyType->Height.Max, static_cast<float>(CanopyHeightMax));
    TestEqual(TEXT("GroundSlopeAngle.Max is the supplied value, not the CDO 45"),
        CanopyType->GroundSlopeAngle.Max, static_cast<float>(CanopySlopeMax));

    // ---- The second entry: supplied wins over derived, and an explicit 0 is honoured ----
    UFoliageType_InstancedStaticMesh* GroundCoverType =
        FindGeneratedSimType(GroundCoverPackage);
    if (TestNotNull(TEXT("the handler generated a foliage type for the small species"),
            GroundCoverType))
    {
        TestEqual(TEXT("an explicit initialSeedDensity is taken verbatim, not derived"),
            GroundCoverType->InitialSeedDensity, static_cast<float>(GroundCoverSeedDensity));
        TestEqual(TEXT("an explicit maxInitialAge of 0 is honoured rather than raised"),
            GroundCoverType->MaxInitialAge, 0.0f);
    }

    // ---- The response says where the two derived numbers came from ----
    TSharedPtr<FJsonObject> CanopyEcho = FindTypeEchoRow(Result, 0);
    if (TestTrue(TEXT("response carries a foliage_types[] row for entry 0"),
            CanopyEcho.IsValid()))
    {
        FString SeedSource;
        CanopyEcho->TryGetStringField(TEXT("initial_seed_density_source"), SeedSource);
        TestEqual(TEXT("entry 0's seed density is reported as derived from density"),
            SeedSource, FString(TEXT("derived_from_density")));

        double EchoedSeedDensity = 0.0;
        CanopyEcho->TryGetNumberField(TEXT("initial_seed_density"), EchoedSeedDensity);
        TestEqual(TEXT("the echoed seed density is the value actually written"),
            EchoedSeedDensity, CanopyDerivedSeedDensity);

        FString AgeSource;
        CanopyEcho->TryGetStringField(TEXT("max_initial_age_source"), AgeSource);
        TestEqual(TEXT("entry 0's initial age is reported as raised for proceduralScale"),
            AgeSource, FString(TEXT("raised_for_procedural_scale")));

        double EffectiveRadius = 0.0;
        CanopyEcho->TryGetNumberField(TEXT("effective_radius"), EffectiveRadius);
        TestEqual(TEXT("effective_radius is max(collisionRadius, shadeRadius)"),
            EffectiveRadius, CanopyShadeRadius);
    }

    TSharedPtr<FJsonObject> GroundCoverEcho = FindTypeEchoRow(Result, 1);
    if (TestTrue(TEXT("response carries a foliage_types[] row for entry 1"),
            GroundCoverEcho.IsValid()))
    {
        FString SeedSource;
        GroundCoverEcho->TryGetStringField(TEXT("initial_seed_density_source"), SeedSource);
        TestEqual(TEXT("entry 1's seed density is reported as supplied"),
            SeedSource, FString(TEXT("supplied")));

        FString AgeSource;
        GroundCoverEcho->TryGetStringField(TEXT("max_initial_age_source"), AgeSource);
        TestEqual(TEXT("entry 1's initial age is reported as supplied"),
            AgeSource, FString(TEXT("supplied")));
    }

    // ---- The third grid number and the output folder are now observable ----
    double EchoedTileOverlap = -1.0;
    TestTrue(TEXT("response echoes tile_overlap"),
        Result->TryGetNumberField(TEXT("tile_overlap"), EchoedTileOverlap));
    TestEqual(TEXT("the echoed tile_overlap is the supplied value, not the hardcoded 0"),
        EchoedTileOverlap, SimTileOverlap);

    FString EchoedSavePath;
    TestTrue(TEXT("response echoes save_path"),
        Result->TryGetStringField(TEXT("save_path"), EchoedSavePath));
    TestEqual(TEXT("the echoed save_path is the effective default folder"),
        EchoedSavePath, FString(TEXT("/Game/ProceduralFoliage")));

    // ---- Warnings: the collapse is named, the (correct) priority order is not ----
    const TArray<FString> Warnings = CollectWarnings(Result);
    // "maxInitialAge 0" is the pinned-age warning's phrasing specifically; the raise warning
    // on entry 0 also says "maxInitialAge", so a bare word would not discriminate.
    TestTrue(TEXT("the pinned maxInitialAge 0 alongside a proceduralScale range is warned about"),
        AnyWarningContains(Warnings, TEXT("maxInitialAge 0")));
    TestTrue(TEXT("the raise on entry 0 is disclosed rather than applied silently"),
        AnyWarningContains(Warnings, TEXT("maxInitialAge was raised to maxAge")));
    TestTrue(TEXT("height / groundSlopeAngle are disclosed as filters, not niches"),
        AnyWarningContains(Warnings, TEXT("groundSlopeAngle")));
    // Priorities here run in the same order as effective radius (450/10 vs 60/1), so the
    // ordering check must stay quiet - a warning that always fires teaches nothing.
    TestFalse(TEXT("a correctly ordered overlapPriority produces no inversion warning"),
        AnyWarningContains(Warnings, TEXT("inverts the radius order")));

    return true;
}

// ============================================================================
// The two ordering rules the verb is uniquely placed to check: overlapPriority against
// effective radius across entries, and spawnsInShade's dependency on canGrowInShade.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralWarnsOnBadOrderingTest,
    "PinWright.foliage.create_procedural.WarnsOnInvertedOverlapPriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralWarnsOnBadOrderingTest::RunTest(const FString& Parameters)
{
    using namespace FoliageCreateProceduralSimConfigTestHelpers;

    FScopedEditorWorldActorGuard WorldGuard;

    const FString VolumeName = FString::Printf(TEXT("PW_ProcFoliageOrder_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SpawnerPackage =
        FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName);

    ON_SCOPE_EXIT
    {
        CleanSimFoliagePackage(SpawnerPackage + TEXT("_FT_0"));
        CleanSimFoliagePackage(SpawnerPackage + TEXT("_FT_1"));
        CleanSimFoliagePackage(SpawnerPackage);
    };

    // Entry 0 is the LARGE species and is given the LOWER priority - the exact inversion that
    // took a live level's trees from 46 placed to 0.
    TSharedPtr<FJsonObject> Large = MakeShared<FJsonObject>();
    Large->SetStringField(TEXT("meshPath"), SimMeshPath);
    Large->SetNumberField(TEXT("collisionRadius"), 400.0);
    Large->SetNumberField(TEXT("shadeRadius"), 400.0);
    Large->SetNumberField(TEXT("overlapPriority"), 1.0);

    // Entry 1 is the SMALL species at the higher priority, and asks to spawn in shade without
    // being able to grow there - which the engine reads as false.
    TSharedPtr<FJsonObject> Small = MakeShared<FJsonObject>();
    Small->SetStringField(TEXT("meshPath"), SimMeshPath);
    Small->SetNumberField(TEXT("collisionRadius"), 50.0);
    Small->SetNumberField(TEXT("shadeRadius"), 50.0);
    Small->SetNumberField(TEXT("overlapPriority"), 5.0);
    Small->SetBoolField(TEXT("spawnsInShade"), true);

    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueObject>(Large));
    Types.Add(MakeShared<FJsonValueObject>(Small));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), VolumeName);
    Params->SetObjectField(TEXT("bounds"), MakeSimBounds());
    Params->SetArrayField(TEXT("foliageTypes"), Types);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.create_procedural"),
        TEXT("req-foliage-create-procedural-order"), Params, bSuccess, Result, ErrorCode);

    // Pre-fix: overlapPriority / collisionRadius / shadeRadius are nested keys the handler
    // never reads, so nothing is written and no warning can exist. The dispatcher's gate does
    // not see nested keys, so this call succeeds either way - the warnings are the assertion.
    TestTrue(*FString::Printf(TEXT("foliage.create_procedural succeeded (errorCode '%s')"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return false;
    }

    const TArray<FString> Warnings = CollectWarnings(Result);
    TestTrue(TEXT("the inverted overlapPriority order is reported"),
        AnyWarningContains(Warnings, TEXT("inverts the radius order")));
    TestTrue(TEXT("the inversion warning names the large species' entry index"),
        AnyWarningContains(Warnings, TEXT("foliageTypes[0]")));
    TestTrue(TEXT("spawnsInShade without canGrowInShade is reported as a no-op"),
        AnyWarningContains(Warnings, TEXT("spawnsInShade without canGrowInShade")));

    // The no-op is also visible as a field, not only as prose: the echo reports what the
    // engine will actually read (bCanGrowInShade && bSpawnsInShade) beside what was stored.
    TSharedPtr<FJsonObject> SmallEcho = FindTypeEchoRow(Result, 1);
    if (TestTrue(TEXT("response carries a foliage_types[] row for the small species"),
            SmallEcho.IsValid()))
    {
        bool bStored = false;
        bool bEffective = true;
        SmallEcho->TryGetBoolField(TEXT("spawns_in_shade"), bStored);
        SmallEcho->TryGetBoolField(TEXT("spawns_in_shade_effective"), bEffective);
        TestTrue(TEXT("spawns_in_shade echoes what was stored"), bStored);
        TestFalse(TEXT("spawns_in_shade_effective reports the AND the engine performs"),
            bEffective);
    }

    return true;
}
