// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the StaticMesh <-> DynamicMesh round trip.
//
// The geometry namespace used to be write-only with respect to assets: 85 ops could build and
// mutate a live ADynamicMeshActor and convert_to_static_mesh could bake one out, but nothing
// could read a UStaticMesh back - so a baked blockout mesh, or any pre-existing project mesh,
// was permanently unreachable. geometry.create_from_static_mesh closes that
// (CopyMeshFromStaticMesh), and convert_to_static_mesh {overwrite:true} closes the write half
// (CopyMeshToStaticMesh, which unlike CreateNewStaticMeshAssetFromMesh takes an EXISTING asset
// and preserves its materials).
//
// GROUND TRUTH: these tests do not settle for the response echo. They read the spawned
// UDynamicMeshComponent's triangle count off the live actor, and they read the material slot
// back off the loaded UStaticMesh asset after an overwrite - the things that actually have to
// hold for the round trip to be usable.
//
// Counterfactual: reverting create_from_static_mesh leaves the verb unregistered and the first
// two tests fail at the Dispatch; reverting the overwrite branch makes the third test's re-bake
// either create a second asset or reset slot 0 to the default material, and the fourth test's
// updated/materialsPreserved fields disappear.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "UDynamicMesh.h"
#include "UObject/UObjectGlobals.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::FindActorByLabel;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
    // Present on every UE install - the same fixture the sibling geometry tests lean on.
    const TCHAR* const RoundTripSourceAsset = TEXT("/Engine/BasicShapes/Cube.Cube");
    const TCHAR* const RoundTripProbeMaterial =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    FString RoundTripLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Triangle count read straight off the live DynamicMeshComponent, not off the response.
    int32 RoundTripLiveTriangleCount(const FString& Label)
    {
        ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(FindActorByLabel(Label));
        if (!Actor)
        {
            return -1;
        }
        UDynamicMeshComponent* Component = Actor->GetDynamicMeshComponent();
        UDynamicMesh* Mesh = Component ? Component->GetDynamicMesh() : nullptr;
        return Mesh ? Mesh->GetTriangleCount() : -1;
    }
}

// ============================================================================
// The asset -> dynamic mesh direction produces a real, editable DynamicMeshActor.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateFromStaticMeshLoadsGeometryTest,
    "PinWright.geometry.create_from_static_mesh.LoadsGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateFromStaticMeshLoadsGeometryTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping create_from_static_mesh load test"));
        return true;
    }

    const FString Label = RoundTripLabel(TEXT("PW_RoundTripLoadProbe"));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), RoundTripSourceAsset);
    Params->SetStringField(TEXT("name"), Label);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_static_mesh"),
        TEXT("req-roundtrip-load"), Params, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.create_from_static_mesh loaded the engine cube"), bSuccess) ||
        !TestTrue(TEXT("load success carries a result object"), Result.IsValid()))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    double Triangles = 0.0;
    TestTrue(TEXT("response echoes triangleCount"),
        Result->TryGetNumberField(TEXT("triangleCount"), Triangles));
    TestTrue(TEXT("the loaded cube carries triangles"), Triangles >= 12.0);

    bool bReused = true;
    TestTrue(TEXT("response echoes reused"), Result->TryGetBoolField(TEXT("reused"), bReused));
    TestFalse(TEXT("a first load is not a reuse"), bReused);

    bool bHasUVs = false;
    TestTrue(TEXT("response echoes hasUVs"), Result->TryGetBoolField(TEXT("hasUVs"), bHasUVs));
    TestTrue(TEXT("the engine cube's UVs survived the copy"), bHasUVs);

    double Slots = 0.0;
    TestTrue(TEXT("response echoes the source material slot count"),
        Result->TryGetNumberField(TEXT("materialSlots"), Slots));
    TestTrue(TEXT("the engine cube reports at least one material slot"), Slots >= 1.0);

    // GROUND TRUTH: a real ADynamicMeshActor with real geometry, not just an echo.
    TestEqual(TEXT("the live DynamicMeshComponent carries the loaded triangles"),
        RoundTripLiveTriangleCount(Label), (int32)Triangles);

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// reuseExisting: a second load reloads the SAME actor instead of leaving two actors
// sharing a label (which every later geometry verb would then resolve ambiguously).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateFromStaticMeshIsIdempotentTest,
    "PinWright.geometry.create_from_static_mesh.ReuseExistingIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateFromStaticMeshIsIdempotentTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping create_from_static_mesh idempotency test"));
        return true;
    }

    const FString Label = RoundTripLabel(TEXT("PW_RoundTripIdemProbe"));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto LoadOnce = [&](bool& bOutSuccess, TSharedPtr<FJsonObject>& OutResult)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), RoundTripSourceAsset);
        Params->SetStringField(TEXT("name"), Label);
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_static_mesh"),
            TEXT("req-roundtrip-idem"), Params, bOutSuccess, OutResult, ErrorCode);
    };

    bool bFirst = false;
    TSharedPtr<FJsonObject> FirstResult;
    LoadOnce(bFirst, FirstResult);
    if (!TestTrue(TEXT("the first load succeeded"), bFirst))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    bool bSecond = false;
    TSharedPtr<FJsonObject> SecondResult;
    LoadOnce(bSecond, SecondResult);
    TestTrue(TEXT("the second load succeeded"), bSecond);

    if (SecondResult.IsValid())
    {
        bool bReused = false;
        TestTrue(TEXT("the second load reports reused"),
            SecondResult->TryGetBoolField(TEXT("reused"), bReused) && bReused);
    }

    // GROUND TRUTH: exactly ONE actor carries the label. Without reuseExisting there would
    // be two, and every subsequent geometry verb would silently pick whichever the actor
    // iterator hit first.
    int32 MatchCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(Label))
        {
            ++MatchCount;
        }
    }
    TestEqual(TEXT("two loads with the same name leave exactly one actor"), MatchCount, 1);

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// The full loop: bake -> assign a material -> reload -> edit -> re-bake with overwrite.
// The material assigned in the middle must survive the re-bake.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertOverwritePreservesMaterialsTest,
    "PinWright.geometry.convert_to_static_mesh.OverwritePreservesMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertOverwritePreservesMaterialsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping overwrite material-preservation test"));
        return true;
    }
    UMaterialInterface* Probe = LoadObject<UMaterialInterface>(nullptr, RoundTripProbeMaterial);
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("WorldGridMaterial unavailable; skipping overwrite material-preservation test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SeedLabel = FString::Printf(TEXT("PW_RoundTripSeed_%s"), *Suffix);
    const FString EditLabel = FString::Printf(TEXT("PW_RoundTripEdit_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/PW_RoundTrip_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto Cleanup = [&]()
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(SeedLabel);
        DestroyActorsWithLabel(EditLabel);
    };

    // 1. Seed a dynamic mesh and bake it to a fresh asset.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), SeedLabel);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-roundtrip-seed"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box seeded the probe actor"), bCreated))
        {
            Cleanup();
            return true;
        }

        TSharedPtr<FJsonObject> BakeParams = MakeShared<FJsonObject>();
        BakeParams->SetStringField(TEXT("actorName"), SeedLabel);
        BakeParams->SetStringField(TEXT("assetPath"), AssetPath);
        bool bBaked = false;
        FString BakeErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
            TEXT("req-roundtrip-bake"), BakeParams, bBaked, BakeErr);
        if (!TestTrue(TEXT("the first bake created the asset"), bBaked))
        {
            Cleanup();
            return true;
        }
    }

    // 2. Assign a material to the baked asset. This is what the re-bake must not destroy.
    {
        TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
        MatParams->SetStringField(TEXT("assetPath"), AssetPath);
        MatParams->SetStringField(TEXT("materialPath"), RoundTripProbeMaterial);
        MatParams->SetNumberField(TEXT("materialIndex"), 0);
        bool bAssigned = false;
        FString MatErr;
        Dispatch(Dispatcher, Sink, TEXT("static_mesh.set_material"),
            TEXT("req-roundtrip-material"), MatParams, bAssigned, MatErr);
        if (!TestTrue(TEXT("static_mesh.set_material assigned the probe material"), bAssigned))
        {
            Cleanup();
            return true;
        }
    }

    // 2b. The asset's OWN LOD0 triangle count before the overwrite. Step 6 compares against
    //     this. Without it, the only overwrite evidence in this test is the material slot plus
    //     the updated / materialsPreserved response fields - and both of those are bookkeeping
    //     the handler appends AFTER the CopyMeshToStaticMesh call, so an overwrite branch that
    //     preserved the materials and never wrote the geometry would leave every other
    //     assertion here green while the asset silently kept its pre-edit mesh.
    int32 AssetTrianglesBeforeOverwrite = -1;
    {
        UStaticMesh* Baked = LoadObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath));
        if (TestNotNull(TEXT("the freshly baked StaticMesh asset is loadable"), Baked))
        {
            AssetTrianglesBeforeOverwrite = Baked->GetNumTriangles(0);
            TestTrue(TEXT("the baked asset carries LOD0 geometry before the overwrite"),
                AssetTrianglesBeforeOverwrite > 0);
        }
    }

    // 3. Reload the baked asset into a NEW editable actor - the direction that did not exist.
    {
        TSharedPtr<FJsonObject> LoadParams = MakeShared<FJsonObject>();
        LoadParams->SetStringField(TEXT("assetPath"), AssetPath);
        LoadParams->SetStringField(TEXT("name"), EditLabel);
        bool bLoaded = false;
        FString LoadErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_static_mesh"),
            TEXT("req-roundtrip-reload"), LoadParams, bLoaded, LoadErr);
        if (!TestTrue(TEXT("the baked asset reloaded into an editable DynamicMeshActor"), bLoaded))
        {
            Cleanup();
            return true;
        }
    }

    const int32 TrianglesBeforeEdit = RoundTripLiveTriangleCount(EditLabel);
    TestTrue(TEXT("the reloaded actor carries geometry"), TrianglesBeforeEdit > 0);

    // 4. Actually edit it, so the re-bake has something to write.
    {
        TSharedPtr<FJsonObject> EditParams = MakeShared<FJsonObject>();
        EditParams->SetStringField(TEXT("actorName"), EditLabel);
        bool bEdited = false;
        FString EditErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.subdivide"),
            TEXT("req-roundtrip-edit"), EditParams, bEdited, EditErr);
        TestTrue(TEXT("a geometry op applies to the reloaded mesh"), bEdited);
    }
    const int32 TrianglesAfterEdit = RoundTripLiveTriangleCount(EditLabel);
    TestTrue(TEXT("the edit changed the reloaded mesh"), TrianglesAfterEdit > TrianglesBeforeEdit);

    // 5. Re-bake IN PLACE.
    {
        TSharedPtr<FJsonObject> OverParams = MakeShared<FJsonObject>();
        OverParams->SetStringField(TEXT("actorName"), EditLabel);
        OverParams->SetStringField(TEXT("assetPath"), AssetPath);
        OverParams->SetBoolField(TEXT("overwrite"), true);
        bool bOverwritten = false;
        FString OverErr;
        TSharedPtr<FJsonObject> OverResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
            TEXT("req-roundtrip-overwrite"), OverParams, bOverwritten, OverResult, OverErr);
        if (!TestTrue(TEXT("the in-place re-bake succeeded"), bOverwritten) || !OverResult.IsValid())
        {
            Cleanup();
            return true;
        }

        bool bUpdated = false;
        TestTrue(TEXT("the overwrite branch reports updated"),
            OverResult->TryGetBoolField(TEXT("updated"), bUpdated) && bUpdated);
        bool bCreated = true;
        TestTrue(TEXT("the overwrite branch does not claim created"),
            OverResult->TryGetBoolField(TEXT("created"), bCreated) && !bCreated);
        bool bPreserved = false;
        TestTrue(TEXT("the overwrite branch reports materialsPreserved"),
            OverResult->TryGetBoolField(TEXT("materialsPreserved"), bPreserved) && bPreserved);
    }

    // 6. GROUND TRUTH: slot 0 of the asset still holds the probe material, and the geometry
    //    is the edited geometry. A create-new re-bake would have reset slot 0 to the default.
    UStaticMesh* Reloaded = LoadObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath));
    if (TestNotNull(TEXT("the overwritten StaticMesh asset is loadable"), Reloaded))
    {
        TestTrue(TEXT("the asset still has a material slot"),
            Reloaded->GetStaticMaterials().Num() >= 1);
        if (Reloaded->GetStaticMaterials().Num() >= 1)
        {
            TestTrue(TEXT("slot 0 still holds the material assigned before the re-bake"),
                Reloaded->GetStaticMaterials()[0].MaterialInterface == Probe);
        }

        // ...and the EDITED geometry actually landed. The subdivide in step 4 multiplies the
        // triangle count, so the asset's LOD0 must now hold strictly more triangles than the
        // pre-overwrite bake did.
        //
        // Nothing else in this test looks at the asset's geometry. `updated` and
        // `materialsPreserved` are not evidence: MeshOpsHandler.cpp emits them as
        // `bUpdatedInPlace && bSavedToDisk` and `bUpdatedInPlace` - branch flags set once the
        // overwrite path is taken, not measurements of what reached the asset. So an overwrite
        // that wrote the wrong LOD, or wrote only the HiRes source and left LOD0 stale
        // (WriteTarget.LODIndex / bWriteHiResSource, MeshOpsHandler.cpp), still returns
        // Outcome::Success with every response field and the material slot intact. This is the
        // assertion that sees it. (A DELETED CopyMeshToStaticMesh call is already caught
        // elsewhere: Outcome stays Failure and the verb errors CONVERSION_FAILED.)
        if (AssetTrianglesBeforeOverwrite > 0)
        {
            const int32 AssetTrianglesAfterOverwrite = Reloaded->GetNumTriangles(0);
            TestTrue(*FString::Printf(
                TEXT("the overwrite rewrote LOD0 with the edited geometry (%d tris before, %d after)"),
                AssetTrianglesBeforeOverwrite, AssetTrianglesAfterOverwrite),
                AssetTrianglesAfterOverwrite > AssetTrianglesBeforeOverwrite);
        }
    }

    Cleanup();
    return true;
}

// ============================================================================
// overwrite:true against a path with no asset is a typed ASSET_NOT_FOUND, and an
// /Engine/ target is refused before the engine's own (message-less) refusal.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertOverwriteGuardsTest,
    "PinWright.geometry.convert_to_static_mesh.OverwriteGuards",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertOverwriteGuardsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping overwrite guard test"));
        return true;
    }

    bSuppressLogErrors = true;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_RoundTripGuard_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-roundtrip-guard-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box seeded the guard probe"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // (a) overwrite onto nothing.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetStringField(TEXT("assetPath"),
            FString::Printf(TEXT("/Game/GeneratedMeshes/PW_NoSuchAsset_%s"), *Suffix));
        Params->SetBoolField(TEXT("overwrite"), true);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
            TEXT("req-roundtrip-guard-missing"), Params, bSuccess, Result, ErrorCode);
        TestFalse(TEXT("overwrite onto a missing asset is not a success"), bSuccess);
        TestEqual(TEXT("overwrite onto a missing asset is ASSET_NOT_FOUND"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // (b) overwrite onto a built-in engine asset. The engine refuses this itself, but only
    //     into the discarded Debug object - we must refuse it with a code and a fix.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube"));
        Params->SetBoolField(TEXT("overwrite"), true);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
            TEXT("req-roundtrip-guard-engine"), Params, bSuccess, Result, ErrorCode);
        TestFalse(TEXT("overwriting an /Engine asset is not a success"), bSuccess);
        // INVALID_ASSET_PATH is also acceptable here - SanitizeProjectRelativePath may
        // reject /Engine/ before the handler's own guard, depending on the configured roots.
        TestTrue(TEXT("overwriting an /Engine asset is refused with a typed code"),
            ErrorCode == TEXT("SECURITY_VIOLATION") || ErrorCode == TEXT("INVALID_ASSET_PATH"));
    }

    DestroyActorsWithLabel(Label);
    return true;
}
