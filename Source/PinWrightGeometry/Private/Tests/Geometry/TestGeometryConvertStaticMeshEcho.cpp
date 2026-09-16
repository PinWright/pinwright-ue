// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-convert-static-mesh-no-asset-echo.
//
// geometry.convert_to_static_mesh — the terminal "bake the DynamicMesh into a
// real /Game/... StaticMesh asset" verb — used to return only {actorName,
// assetPath} + the prose "StaticMesh created from DynamicMesh" on success. The
// assetPath it echoes is just the input/derived path, NOT a confirmation that
// anything exists there, so the canonical "bake AND confirm it landed" intent
// cost two extra readbacks (asset.exists + asset.get_metadata) to re-derive
// facts the handler already held at Outcome == Success.
//
// The fix echoes a machine-readable confirmation block inline on the success
// response — exists/created (the success branch IS the proof), class:"StaticMesh",
// triangleCount/vertexCount (read off the held Target.Mesh), and the CreateOptions
// flags (nanite/recomputeNormals/recomputeTangents) — mirroring how
// simplify_mesh/subdivide already carry their post-op topology inline.
// (MeshOpsHandler.cpp geometry.convert_to_static_mesh success branch.)
//
// Strategy: spawn a real DynamicMeshActor via geometry.create_box, then route
// geometry.convert_to_static_mesh through the real dispatcher and assert the
// success result carries the confirmation block with the expected values.
// Counterfactual: reverting the fix (success result back to {actorName,
// assetPath} only) drops every confirmation field and fails this test.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ObjectTools.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
    // Delete the baked StaticMesh asset so the test leaves no /Game/... residue.
    void DeleteAssetIfPresent(const FString& PackagePath)
    {
        FAssetRegistryModule& AssetRegistry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Assets;
        AssetRegistry.Get().GetAssetsByPackageName(FName(*PackagePath), Assets);
        if (Assets.Num() > 0)
        {
            ObjectTools::DeleteAssets(Assets, /*bShowConfirmation=*/false);
        }
    }
}

// The convert success response carries the inline confirmation block so the bake
// is verifiable in one call (no asset.exists + asset.get_metadata round-trip).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertToStaticMeshEchoesConfirmationTest,
    "PinWright.geometry.convert_to_static_mesh.EchoesConfirmation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertToStaticMeshEchoesConfirmationTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert_to_static_mesh echo test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_ConvertEchoProbe_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *Label);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1. Spawn a real DynamicMeshActor (a box has a known non-zero tri/vert count)
    //    whose label is the actorName convert will resolve.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-convert-echo-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 2. Bake it and capture the success payload.
    TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
    ConvertParams->SetStringField(TEXT("actorName"), Label);
    ConvertParams->SetStringField(TEXT("assetPath"), AssetPath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
        TEXT("req-convert-echo"), ConvertParams, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.convert_to_static_mesh baked the StaticMesh"), bSuccess) ||
        !TestTrue(TEXT("convert success carries a result object"), Result.IsValid()))
    {
        DeleteAssetIfPresent(AssetPath);
        DestroyActorsWithLabel(Label);
        return true;
    }

    // 3. The confirmation block — the heart of the fix. Each field would be absent
    //    if the success response reverted to {actorName, assetPath} only.
    bool bExists = false;
    TestTrue(TEXT("result echoes exists:true"),
        Result->TryGetBoolField(TEXT("exists"), bExists) && bExists);

    bool bCreated = false;
    TestTrue(TEXT("result echoes created:true"),
        Result->TryGetBoolField(TEXT("created"), bCreated) && bCreated);

    FString AssetClass;
    TestTrue(TEXT("result echoes class:StaticMesh"),
        Result->TryGetStringField(TEXT("class"), AssetClass) && AssetClass.Equals(TEXT("StaticMesh")));

    double TriangleCount = 0.0;
    TestTrue(TEXT("result echoes a triangleCount field"),
        Result->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
    TestTrue(TEXT("baked box has a non-zero triangleCount"), TriangleCount > 0.0);

    double VertexCount = 0.0;
    TestTrue(TEXT("result echoes a vertexCount field"),
        Result->TryGetNumberField(TEXT("vertexCount"), VertexCount));
    TestTrue(TEXT("baked box has a non-zero vertexCount"), VertexCount > 0.0);

    // 4. The CreateOptions flags it configured (so the caller confirms bake
    //    settings without a metadata re-read) — Nanite off, normals/tangents on.
    bool bNanite = true;
    TestTrue(TEXT("result echoes the nanite flag"),
        Result->TryGetBoolField(TEXT("nanite"), bNanite));
    TestFalse(TEXT("baked StaticMesh has Nanite disabled"), bNanite);

    bool bRecomputeNormals = false;
    TestTrue(TEXT("result echoes recomputeNormals:true"),
        Result->TryGetBoolField(TEXT("recomputeNormals"), bRecomputeNormals) && bRecomputeNormals);

    bool bRecomputeTangents = false;
    TestTrue(TEXT("result echoes recomputeTangents:true"),
        Result->TryGetBoolField(TEXT("recomputeTangents"), bRecomputeTangents) && bRecomputeTangents);

    // Cleanup: drop the baked asset and the probe actor so the host stays clean.
    DeleteAssetIfPresent(AssetPath);
    DestroyActorsWithLabel(Label);
    return true;
}
