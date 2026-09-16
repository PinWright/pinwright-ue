// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-create-procedural-ignores-scale-and-normal-fields.
//
// Symptom, in two halves. foliage.create_procedural ACCEPTED per-entry minScale /
// maxScale / alignToNormal on foliageTypes[] and wrote none of them: the generated
// UFoliageType got SetStaticMesh + Density + ReapplyDensity and nothing else, so every
// plant came out the same size and vertical while the call reported success. (That half
// was at least echoed back in ignoredFields, which is what made the ticket cheap to
// substantiate.) The spawner's TileSize and NumUniqueTiles were worse: hardcoded to
// 1000 cm / 10 tiles, accepted as no parameter and reported in no response field, so the
// grid the simulation actually ran on was neither settable nor observable.
//
// The fix honours the three per-type keys through the same read + validation
// foliage.add_type uses (ReadFoliageScaleAndAlign / ApplyFoliageScaleAndAlign, promoted
// to file scope in FoliageHandler.cpp so the two verbs cannot drift), accepts tileSize /
// numUniqueTiles, and echoes the effective tiling as tile_size / num_unique_tiles.
//
// WHY THIS TEST READS ASSETS AND NOT JUST THE RESPONSE. The defect was invisible from
// the response alone — success:true, an accurate foliage_types_count and an honest
// ignoredFields were all present while the values were being dropped. So the load-bearing
// assertions below open the two assets the handler built and read the properties back:
// ScaleX/Y/Z and AlignToNormal off the generated UFoliageType_InstancedStaticMesh, and
// TileSize / NumUniqueTiles off the UProceduralFoliageSpawner. Against the pre-fix
// handler those read 1.0 / true / 1000 / 10 and the comparisons fail.
//
// The request routes through the real dispatcher (FRpcDispatcher::ProcessRequest), not
// TestUtils' InvokeHandler, because half the fix is the PARAM DECLARATION: the
// dispatcher's unknown-param gate refuses tileSize / numUniqueTiles with UNKNOWN_PARAMS
// unless RPC_PARAMS declares them, and InvokeHandler never runs that gate.
//
// One call exercises both entry outcomes: entry 0 carries a valid non-default range and
// alignToNormal:false (plus a randomYaw the verb still does not read, so ignoredFields
// stays exercised), entry 1 an inverted range that must be reported in skipped[] rather
// than silently clamped or allowed to fail the whole batch.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "ProceduralFoliageSpawner.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity
// builds enabled, where same-named anonymous-namespace helpers collide across merged
// translation units.
namespace FoliageCreateProceduralTypeConfigTestHelpers
{
    constexpr const TCHAR* MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Non-default on every axis so nothing here can pass by matching a CDO value:
    // the interval is neither 1.0 at either end, alignToNormal inverts the default
    // true, and the tiling pair differs from the old hardcoded 1000 / 10.
    constexpr double MinScale = 0.4;
    constexpr double MaxScale = 2.5;
    constexpr double TileSize = 2500.0;
    constexpr int32 NumUniqueTiles = 3;

    // One foliageTypes[] entry.
    inline TSharedPtr<FJsonValue> MakeTypeEntry(double InMinScale, double InMaxScale,
        bool bAlignToNormal, bool bWithRandomYaw)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("meshPath"), MeshPath);
        Entry->SetNumberField(TEXT("density"), 50.0);
        Entry->SetNumberField(TEXT("minScale"), InMinScale);
        Entry->SetNumberField(TEXT("maxScale"), InMaxScale);
        Entry->SetBoolField(TEXT("alignToNormal"), bAlignToNormal);
        if (bWithRandomYaw)
        {
            // Still unread by this verb, so it must keep showing up in ignoredFields.
            Entry->SetBoolField(TEXT("randomYaw"), false);
        }
        return MakeShared<FJsonValueObject>(Entry);
    }

    inline TSharedPtr<FJsonObject> MakeBounds()
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

    // The handler hardcodes /Game/ProceduralFoliage and only marks its packages dirty
    // (McpSafeAssetSave is mark-dirty-only), so an editor-wide save-all later in the
    // suite would otherwise flush them into the host Content tree. Delete the object and
    // clear the dirty flag any emptied package is left carrying. B-tests-leak-host-content.
    inline void CleanFoliagePackage(const FString& PackagePath)
    {
        CleanupTestAsset(PackagePath);
        if (UPackage* Remaining = FindPackage(nullptr, *PackagePath))
        {
            Remaining->SetDirtyFlag(false);
        }
    }

    // Reads back an asset the handler created, by the package/object naming the handler
    // itself uses. FindObject rather than LoadObject: the packages are dirty-only, never
    // written to disk, so the freshly built object is the only copy there is.
    template <typename T>
    inline T* FindGeneratedAsset(const FString& PackagePath)
    {
        return FindObject<T>(nullptr, *ToObjectPath(PackagePath));
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens FoliageCreateProceduralTypeConfigTestHelpers inside its own
// body rather than at file scope: a file-scope using-directive would leak into every
// other test .cpp that Unity merges after this one into the same translation unit.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralAppliesTypeConfigTest,
    "PinWright.foliage.create_procedural.AppliesScaleAlignAndTiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralAppliesTypeConfigTest::RunTest(const FString& Parameters)
{
    using namespace FoliageCreateProceduralTypeConfigTestHelpers;

    // Destroys the AProceduralFoliageVolume the handler spawns and restores the level's
    // dirty flag, so the open map is left as found.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString VolumeName = FString::Printf(TEXT("PW_ProcFoliageCfg_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SpawnerPackage =
        FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName);
    const FString FirstTypePackage = SpawnerPackage + TEXT("_FT_0");
    const FString SecondTypePackage = SpawnerPackage + TEXT("_FT_1");

    ON_SCOPE_EXIT
    {
        CleanFoliagePackage(FirstTypePackage);
        // Only exists if the skipped entry wrongly consumed a type index.
        CleanFoliagePackage(SecondTypePackage);
        CleanFoliagePackage(SpawnerPackage);
    };

    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeTypeEntry(MinScale, MaxScale, /*bAlignToNormal=*/false,
        /*bWithRandomYaw=*/true));
    // Inverted range: the same condition foliage.add_type rejects outright.
    Types.Add(MakeTypeEntry(3.0, 1.0, /*bAlignToNormal=*/true, /*bWithRandomYaw=*/false));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), VolumeName);
    Params->SetObjectField(TEXT("bounds"), MakeBounds());
    Params->SetArrayField(TEXT("foliageTypes"), Types);
    Params->SetNumberField(TEXT("seed"), 12345.0);
    Params->SetNumberField(TEXT("tileSize"), TileSize);
    Params->SetNumberField(TEXT("numUniqueTiles"), NumUniqueTiles);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.create_procedural"),
        TEXT("req-foliage-create-procedural-type-config"), Params, bSuccess, Result, ErrorCode);

    // Before the fix this is where the run stops: tileSize / numUniqueTiles are
    // undeclared, so the dispatcher's unknown-param gate answers UNKNOWN_PARAMS.
    TestTrue(*FString::Printf(
        TEXT("foliage.create_procedural accepts tileSize/numUniqueTiles (errorCode '%s')"),
        *ErrorCode), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return false;
    }

    // ---- The per-type writes, read back off the generated asset ----
    UFoliageType_InstancedStaticMesh* GeneratedType =
        FindGeneratedAsset<UFoliageType_InstancedStaticMesh>(FirstTypePackage);
    if (!TestNotNull(TEXT("the handler generated a foliage type for the valid entry"),
            GeneratedType))
    {
        // Nothing was built (e.g. the engine cube did not load on this host), so every
        // property comparison below would be vacuous rather than discriminating.
        return false;
    }

    // Uniform scaling: one interval driving all three axes, exactly what
    // foliage.add_type writes. Pre-fix these are the CDO's 1.0 / 1.0.
    TestEqual(TEXT("ScaleX.Min is the supplied minScale"),
        GeneratedType->ScaleX.Min, static_cast<float>(MinScale));
    TestEqual(TEXT("ScaleX.Max is the supplied maxScale"),
        GeneratedType->ScaleX.Max, static_cast<float>(MaxScale));
    TestEqual(TEXT("ScaleY.Min is the supplied minScale"),
        GeneratedType->ScaleY.Min, static_cast<float>(MinScale));
    TestEqual(TEXT("ScaleY.Max is the supplied maxScale"),
        GeneratedType->ScaleY.Max, static_cast<float>(MaxScale));
    TestEqual(TEXT("ScaleZ.Min is the supplied minScale"),
        GeneratedType->ScaleZ.Min, static_cast<float>(MinScale));
    TestEqual(TEXT("ScaleZ.Max is the supplied maxScale"),
        GeneratedType->ScaleZ.Max, static_cast<float>(MaxScale));
    TestTrue(TEXT("Scaling is Uniform, matching foliage.add_type"),
        GeneratedType->Scaling == EFoliageScaling::Uniform);

    // alignToNormal:false inverts the property default, so a dropped write reads true.
    TestFalse(TEXT("AlignToNormal is the supplied false, not the default true"),
        static_cast<bool>(GeneratedType->AlignToNormal));

    // ---- The spawner tiling, read back off the spawner asset ----
    UProceduralFoliageSpawner* Spawner =
        FindGeneratedAsset<UProceduralFoliageSpawner>(SpawnerPackage);
    if (TestNotNull(TEXT("the handler generated the procedural foliage spawner"), Spawner))
    {
        TestEqual(TEXT("TileSize is the supplied tileSize, not the hardcoded 1000"),
            Spawner->TileSize, static_cast<float>(TileSize));
        TestEqual(TEXT("NumUniqueTiles is the supplied numUniqueTiles, not the hardcoded 10"),
            Spawner->NumUniqueTiles, NumUniqueTiles);
    }

    // ---- The response tells the caller what the grid was ----
    double ReportedTileSize = 0.0;
    TestTrue(TEXT("response echoes tile_size"),
        Result->TryGetNumberField(TEXT("tile_size"), ReportedTileSize));
    TestEqual(TEXT("the echoed tile_size is the effective value"), ReportedTileSize, TileSize);

    double ReportedNumUniqueTiles = 0.0;
    TestTrue(TEXT("response echoes num_unique_tiles"),
        Result->TryGetNumberField(TEXT("num_unique_tiles"), ReportedNumUniqueTiles));
    TestEqual(TEXT("the echoed num_unique_tiles is the effective value"),
        static_cast<int32>(ReportedNumUniqueTiles), NumUniqueTiles);

    // ---- ignoredFields must stop naming keys that are now applied ----
    TArray<FString> IgnoredFields;
    const TArray<TSharedPtr<FJsonValue>>* IgnoredArray = nullptr;
    if (Result->TryGetArrayField(TEXT("ignoredFields"), IgnoredArray) && IgnoredArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *IgnoredArray)
        {
            IgnoredFields.Add(Value->AsString());
        }
    }
    TestFalse(TEXT("minScale is no longer reported as ignored"),
        IgnoredFields.Contains(TEXT("minScale")));
    TestFalse(TEXT("maxScale is no longer reported as ignored"),
        IgnoredFields.Contains(TEXT("maxScale")));
    TestFalse(TEXT("alignToNormal is no longer reported as ignored"),
        IgnoredFields.Contains(TEXT("alignToNormal")));
    // The mechanism itself must survive: randomYaw really is unread here.
    TestTrue(TEXT("randomYaw is still reported in ignoredFields"),
        IgnoredFields.Contains(TEXT("randomYaw")));

    // ---- The inverted range is refused per entry, not clamped and not fatal ----
    double TypesCount = -1.0;
    TestTrue(TEXT("response carries foliage_types_count"),
        Result->TryGetNumberField(TEXT("foliage_types_count"), TypesCount));
    TestEqual(TEXT("only the valid entry produced a foliage type"),
        static_cast<int32>(TypesCount), 1);

    double SkippedCount = -1.0;
    TestTrue(TEXT("response carries skippedCount"),
        Result->TryGetNumberField(TEXT("skippedCount"), SkippedCount));
    TestEqual(TEXT("the inverted-range entry is counted as skipped"),
        static_cast<int32>(SkippedCount), 1);

    const TArray<TSharedPtr<FJsonValue>>* SkippedArray = nullptr;
    if (TestTrue(TEXT("response carries the skipped[] detail array"),
            Result->TryGetArrayField(TEXT("skipped"), SkippedArray) && SkippedArray))
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (SkippedArray->Num() > 0 && (*SkippedArray)[0]->TryGetObject(Row) && Row)
        {
            double Index = -1.0;
            (*Row)->TryGetNumberField(TEXT("index"), Index);
            TestEqual(TEXT("the skipped row names entry 1"), static_cast<int32>(Index), 1);

            FString Reason;
            (*Row)->TryGetStringField(TEXT("reason"), Reason);
            TestTrue(TEXT("the skipped reason names the inverted scale range"),
                Reason.Contains(TEXT("maxScale")));
        }
    }

    // The skipped entry must not consume a type index, or the generated _FT_<n> names
    // would gap and the next valid entry would be misnamed.
    TestNull(TEXT("the skipped entry generated no foliage type asset"),
        FindGeneratedAsset<UFoliageType_InstancedStaticMesh>(SecondTypePackage));

    return true;
}
