// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-geometry-mesh-material-assign.
//
// The in-editor modeling -> bake flow (geometry.create_box -> geometry.convert_to_static_mesh)
// produced a /Game StaticMesh whose material slots were never populated —
// CreateNewStaticMeshAssetFromMesh bakes geometry only, so the baked asset the user
// drags into levels always came up on the default material, and no verb assigned a
// material to a StaticMesh asset slot (only the LIVE component was reachable via
// actor.set_component_properties {OverrideMaterials:[...]}, which does not carry into
// the baked asset). static_mesh.set_material closes that gap: it binds a material to a
// StaticMesh's StaticMaterials slot by index (UStaticMesh::SetMaterial) and persists
// the asset. (StaticMeshSetMaterialHandler.cpp.)
//
// Strategy (all fixtures built IN-CODE through the real registered handlers, so the
// test depends on no external /Game content): bake a StaticMesh via create_box ->
// convert_to_static_mesh, create a real Material via material.authoring.create_material,
// then dispatch static_mesh.set_material and assert (a) the response echoes the bound
// material read off the asset slot, and (b) GROUND TRUTH — the baked UStaticMesh's
// slot-0 MaterialInterface is the created material. Reverting the fix (no SetMaterial /
// no verb) leaves slot 0 on the default material, so the ground-truth assertion fails.
// (The mesh is still resident in memory from the steps above, so this is an in-memory
// regression guard; the saved:true check proves the .uasset was written, not this read.)
// A final case asserts an out-of-range slot index errors (INVALID_MATERIAL_INDEX)
// rather than silently no-op'ing into a fake success (UStaticMesh::SetMaterial no-ops
// on an invalid index).
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
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// static_mesh.set_material must bind a material into the StaticMesh asset's slot and
// persist it — the baked asset a user drags into levels carries the material.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshSetMaterialAssignsSlotTest,
    "PinWright.static_mesh.set_material.AssignsSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshSetMaterialAssignsSlotTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping static_mesh.set_material test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_SetMatProbe_%s"), *Suffix);
    const FString MeshAssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *Label);
    const FString MatName = FString::Printf(TEXT("M_SetMatProbe_%s"), *Suffix);
    const FString MatFolder = TEXT("/Game/PinWrightTests/StaticMeshSetMaterial");
    const FString MatPackagePath = FString::Printf(TEXT("%s/%s"), *MatFolder, *MatName);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Always tear the fixtures down, whichever assertion path returns.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MeshAssetPath);
        CleanupTestAsset(MatPackagePath);
        DestroyActorsWithLabel(Label);
    };

    // 1. Bake a real StaticMesh in-code: create_box -> convert_to_static_mesh.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-setmat-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            return true;
        }
    }
    {
        TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
        ConvertParams->SetStringField(TEXT("actorName"), Label);
        ConvertParams->SetStringField(TEXT("assetPath"), MeshAssetPath);
        bool bBaked = false;
        FString BakeErr;
        TSharedPtr<FJsonObject> BakeResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
            TEXT("req-setmat-bake"), ConvertParams, bBaked, BakeResult, BakeErr);
        if (!TestTrue(TEXT("geometry.convert_to_static_mesh baked the StaticMesh"), bBaked))
        {
            return true;
        }
    }

    // 2. Create a real Material fixture in-code (no external content dependency).
    {
        TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
        MatParams->SetStringField(TEXT("name"), MatName);
        MatParams->SetStringField(TEXT("path"), MatFolder);
        MatParams->SetBoolField(TEXT("save"), true);
        bool bMat = false;
        FString MatErr;
        TSharedPtr<FJsonObject> MatResult;
        Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material"),
            TEXT("req-setmat-mat"), MatParams, bMat, MatResult, MatErr);
        if (!TestTrue(TEXT("material.authoring.create_material created the probe material"), bMat))
        {
            return true;
        }
    }

    // 3. THE FIX: assign the material to slot 0 of the baked StaticMesh.
    {
        TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
        SetParams->SetStringField(TEXT("assetPath"), MeshAssetPath);
        SetParams->SetStringField(TEXT("materialPath"), MatPackagePath);
        SetParams->SetNumberField(TEXT("materialIndex"), 0);
        bool bSet = false;
        FString SetErr;
        TSharedPtr<FJsonObject> SetResult;
        Dispatch(Dispatcher, Sink, TEXT("static_mesh.set_material"),
            TEXT("req-setmat-set"), SetParams, bSet, SetResult, SetErr);

        TestTrue(TEXT("static_mesh.set_material succeeded"), bSet);
        if (bSet && SetResult.IsValid())
        {
            // The response echoes the bound material path read off the asset slot.
            FString EchoedMat;
            SetResult->TryGetStringField(TEXT("materialPath"), EchoedMat);
            TestTrue(TEXT("response echoes a non-empty bound materialPath"), !EchoedMat.IsEmpty());
            TestTrue(TEXT("bound materialPath references the created probe material"),
                EchoedMat.Contains(MatName));
            // Honest persistence verdict: saved:true once the .uasset is durable.
            bool bSaved = false;
            TestTrue(TEXT("static_mesh.set_material reports saved:true"),
                SetResult->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
        }
    }

    // 4. GROUND TRUTH: read the baked StaticMesh's slot 0 and assert it now carries the
    //    created material. Reverting the fix leaves slot 0 on the default material.
    //    (LoadObject returns the still-resident in-memory mesh, not a fresh disk read —
    //    this is an in-memory regression guard; the saved:true check above covers disk.)
    UStaticMesh* BakedMesh = LoadObject<UStaticMesh>(nullptr, *ToObjectPath(MeshAssetPath));
    UMaterialInterface* ExpectedMat = LoadObject<UMaterialInterface>(nullptr, *ToObjectPath(MatPackagePath));
    if (TestNotNull(TEXT("baked StaticMesh is loadable"), BakedMesh) &&
        TestNotNull(TEXT("probe material is loadable"), ExpectedMat))
    {
        const TArray<FStaticMaterial>& Slots = BakedMesh->GetStaticMaterials();
        if (TestTrue(TEXT("baked StaticMesh has at least one material slot"), Slots.Num() >= 1))
        {
            // TObjectPtr == raw-pointer comparison (avoids TestEqual template deduction
            // across the two pointer kinds).
            TestTrue(TEXT("slot 0 MaterialInterface is the assigned material"),
                Slots[0].MaterialInterface == ExpectedMat);
        }
    }

    // 5. Out-of-range slot index must error, not fake-succeed on an unchanged asset
    //    (UStaticMesh::SetMaterial silently no-ops on an invalid index).
    {
        TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
        BadParams->SetStringField(TEXT("assetPath"), MeshAssetPath);
        BadParams->SetStringField(TEXT("materialPath"), MatPackagePath);
        BadParams->SetNumberField(TEXT("materialIndex"), 99);
        bool bBad = false;
        FString BadErr;
        Dispatch(Dispatcher, Sink, TEXT("static_mesh.set_material"),
            TEXT("req-setmat-bad"), BadParams, bBad, BadErr);
        TestFalse(TEXT("out-of-range materialIndex is rejected, not a fake success"), bBad);
        TestEqual(TEXT("out-of-range slot returns INVALID_MATERIAL_INDEX"),
            BadErr, FString(TEXT("INVALID_MATERIAL_INDEX")));
    }

    return true;
}
