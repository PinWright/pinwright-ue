// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-geometry-convert-static-mesh-drops-collision.
//
// "Give it collision, then bake it into a StaticMesh I can drop into a level" is the
// canonical tail of a procedural-prop chain. geometry.convert_to_static_mesh bakes via
// CreateNewStaticMeshAssetFromMesh(Target.Mesh, ...) — geometry only. A prior
// geometry.generate_collision applies its simple shapes to the source
// DynamicMeshComponent's BodySetup (not to the mesh), so the bake used to silently drop
// them: static_mesh.describe reported box:0 despite generate_collision returning
// shapeCount:1, with no in-band signal on the convert response (created:true regardless).
//
// The fix carries the source component's simple collision (AggGeom + trace flag) into the
// baked StaticMesh's own BodySetup before the save, and echoes a collisionElements count
// so the transfer is verifiable in one call.
// (GeometryUtils::TransferSimpleCollisionToStaticMesh in CollisionHelpers.h, wired into
// MeshOpsHandler.cpp geometry.convert_to_static_mesh.)
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box, generate box collision
// on it through the real dispatcher, bake it, then (a) assert the convert response reports
// collisionElements >= 1 and (b) load the baked UStaticMesh and assert its BodySetup's
// AggGeom actually holds the box primitive. Counterfactual: reverting the fix (no
// TransferSimpleCollisionToStaticMesh call) leaves the baked BodySetup empty, so both the
// collisionElements echo and the loaded-asset box count are zero and this test fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// A simple collision generated on the DynamicMeshActor must survive the bake into the
// StaticMesh asset — the baked mesh's BodySetup carries the box, and the convert response
// reports how many collision primitives it carried over.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertToStaticMeshCarriesCollisionTest,
    "PinWright.geometry.convert_to_static_mesh.CarriesCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertToStaticMeshCarriesCollisionTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert_to_static_mesh collision-carry test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_ConvertCollisionProbe_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *Label);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1. Spawn a real DynamicMeshActor whose label is the actorName convert resolves.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-convert-collision-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 2. Generate simple box collision ON THE DYNAMIC MESH ACTOR (the component's BodySetup).
    //    This is the shape that the bake used to drop.
    {
        TSharedPtr<FJsonObject> CollParams = MakeShared<FJsonObject>();
        CollParams->SetStringField(TEXT("actorName"), Label);
        CollParams->SetStringField(TEXT("collisionType"), TEXT("box"));
        bool bCollSuccess = false;
        FString CollErr;
        TSharedPtr<FJsonObject> CollResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.generate_collision"),
            TEXT("req-convert-collision-gen"), CollParams, bCollSuccess, CollResult, CollErr);

        double ShapeCount = 0.0;
        const bool bHasShapes = bCollSuccess && CollResult.IsValid() &&
            CollResult->TryGetNumberField(TEXT("shapeCount"), ShapeCount) && ShapeCount >= 1.0;
        // Precondition: without a simple shape on the source there is nothing to carry, so
        // the test would be vacuous. A box mesh must yield at least one box body.
        if (!TestTrue(TEXT("generate_collision applied a simple box shape to the source actor"), bHasShapes))
        {
            CleanupTestAsset(AssetPath);
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 3. Bake it and capture the success payload.
    TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
    ConvertParams->SetStringField(TEXT("actorName"), Label);
    ConvertParams->SetStringField(TEXT("assetPath"), AssetPath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
        TEXT("req-convert-collision"), ConvertParams, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.convert_to_static_mesh baked the StaticMesh"), bSuccess) ||
        !TestTrue(TEXT("convert success carries a result object"), Result.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(Label);
        return true;
    }

    // 4. In-band signal: the convert response reports the carried collision element count.
    //    Reverting the fix drops this field (and the transfer), so it reads 0 / absent.
    double CollisionElements = 0.0;
    TestTrue(TEXT("convert response echoes a collisionElements field"),
        Result->TryGetNumberField(TEXT("collisionElements"), CollisionElements));
    TestTrue(TEXT("convert reports at least one collision primitive carried into the bake"),
        CollisionElements >= 1.0);

    // 5. GROUND TRUTH: load the baked StaticMesh and assert its BodySetup actually holds the
    //    box. This is exactly what static_mesh.describe's collision.elements reads back, and
    //    what the board repro observed as box:0 before the fix.
    FString EchoedAssetPath;
    Result->TryGetStringField(TEXT("assetPath"), EchoedAssetPath);
    if (EchoedAssetPath.IsEmpty())
    {
        EchoedAssetPath = AssetPath;
    }
    UStaticMesh* BakedMesh = LoadObject<UStaticMesh>(nullptr, *ToObjectPath(EchoedAssetPath));
    if (TestNotNull(TEXT("the baked StaticMesh asset is loadable"), BakedMesh))
    {
        UBodySetup* BodySetup = BakedMesh->GetBodySetup();
        if (TestNotNull(TEXT("the baked StaticMesh has a BodySetup"), BodySetup))
        {
            TestTrue(TEXT("the baked StaticMesh's BodySetup carries at least one simple collision primitive"),
                BodySetup->AggGeom.GetElementCount() >= 1);
            TestTrue(TEXT("the generated box primitive specifically survived the bake"),
                BodySetup->AggGeom.BoxElems.Num() >= 1);
        }
    }

    // Cleanup: drop the baked asset (registry + disk) and the probe actor.
    CleanupTestAsset(AssetPath);
    DestroyActorsWithLabel(Label);
    return true;
}
