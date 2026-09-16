// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the SkeletalMesh <-> DynamicMesh round trip and the skin binding that makes it work.
//
// Before geometry.create_from_skeletal_mesh / bind_skin_weights / convert_to_skeletal_mesh, the
// ~90-verb geometry namespace had ZERO skeletal support (grepping SkeletalMesh|BoneWeight across
// Source/PinWrightGeometry/ returned nothing) and NO verb in any namespace created a
// USkeletalMesh. Characters could only be authored through untyped python.execute.
//
// GROUND TRUTH: these tests do not settle for the response echo. They read the triangle count off
// the live UDynamicMeshComponent, they re-dispatch the verbs to observe the mesh's measured skin
// coverage change, and on the rejection paths they assert that NO asset was left behind.
//
// THE TEST THAT MATTERS is FGeometrySkeletalOrderingTrapTest. Everything else guards a normal
// path; that one guards the defect class this whole feature exists to eliminate - a bind that
// silently stops covering the mesh after later geometry edits, producing an asset that the engine
// accepts and the renderer draws wrong.
//
// Counterfactuals:
//  - reverting the three verbs leaves them unregistered and every test fails at the Dispatch;
//  - reverting ONLY the per-vertex coverage scan in convert_to_skeletal_mesh (SkeletalIOScanWeight
//    Coverage + the NO_SKIN_WEIGHTS / SKIN_WEIGHTS_INCOMPLETE gates) makes
//    FGeometrySkeletalRejectsUnskinnedMeshTest and FGeometrySkeletalOrderingTrapTest go green-to-
//    red in the worst way: the calls start SUCCEEDING and writing assets, which is exactly the
//    silent failure being prevented;
//  - reverting the ASSET_EXISTS guard makes FGeometrySkeletalCreateRefusesOccupiedPathTest report
//    created:true while the engine wipes the target asset's LODs, materials and reference
//    skeleton in place.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Animation/Skeleton.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UDynamicMesh.h"
#include "UObject/Linker.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::FindActorByLabel;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
    // Present on every UE 5.3-5.8 install (verified on disk at
    // C:\UE_5.3\Engine\Content\EngineMeshes\ and C:\UE_5.8\Engine\Content\EngineMeshes\). The
    // skeletal counterpart of the /Engine/BasicShapes/Cube fixture the StaticMesh round-trip
    // tests lean on. /Engine paths pass SanitizeProjectRelativePath (it validates mount points,
    // and /Engine is one) so they are usable as a read source; they are refused only as a WRITE
    // target, which is a separate guard.
    const TCHAR* const SkeletalSourceAsset =
        TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
    const TCHAR* const SkeletalSourceSkeleton =
        TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton");

    FString SkeletalLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // True only when the engine actually ships the fixture on this host. Every test bails with a
    // warning rather than a failure when it does not, matching how the sibling geometry tests
    // treat a missing editor world - a stripped content install is an environment fact, not a
    // regression in this code.
    bool SkeletalFixturesAvailable()
    {
        return LoadObject<USkeletalMesh>(nullptr, SkeletalSourceAsset) != nullptr
            && LoadObject<USkeleton>(nullptr, SkeletalSourceSkeleton) != nullptr;
    }

    // Triangle count read straight off the live DynamicMeshComponent, not off the response.
    int32 SkeletalLiveTriangleCount(const FString& Label)
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

    // Read a bool field, defaulting to the value that would FAIL the assertion so a missing field
    // never reads as a pass.
    bool ReadBool(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, bool DefaultOnMissing)
    {
        bool Value = DefaultOnMissing;
        return (Result.IsValid() && Result->TryGetBoolField(Field, Value)) ? Value : DefaultOnMissing;
    }

    double ReadNumber(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, double DefaultOnMissing)
    {
        double Value = DefaultOnMissing;
        return (Result.IsValid() && Result->TryGetNumberField(Field, Value)) ? Value : DefaultOnMissing;
    }

    // Nothing at all at this package path - neither on disk nor loaded in memory. The rejection
    // tests use this rather than trusting the error code alone: a guard that returns the right
    // code AFTER creating the asset would still have shipped the defect.
    bool NothingAtAssetPath(const FString& PackagePath)
    {
        // FindObject first (in-memory, no load side effect), then a quiet cold load in case the
        // .uasset landed on disk without the object staying resident. Same ObjectPath form
        // TestUtils.h:486 uses for its in-memory probe.
        return FindObject<UObject>(nullptr, *ToObjectPath(PackagePath)) == nullptr
            && LoadObject<UObject>(nullptr, *PackagePath, nullptr, LOAD_NoWarn | LOAD_Quiet) == nullptr;
    }

    UStaticMesh* SkeletalMaterialSlotsTest_CreateMultiSectionFixture(
        const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        UStaticMesh* Mesh = Package
            ? NewObject<UStaticMesh>(
                Package,
                FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
                RF_Public | RF_Standalone)
            : nullptr;
        if (!Mesh)
        {
            return nullptr;
        }

        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotA"))));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotB"))));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotC"))));

        FMeshDescription MeshDescription;
        FStaticMeshAttributes Attributes(MeshDescription);
        Attributes.Register();
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        UVs.SetNumChannels(2);

        const FPolygonGroupID GroupA = MeshDescription.CreatePolygonGroup();
        const FPolygonGroupID GroupC = MeshDescription.CreatePolygonGroup();
        TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
        SlotNames[GroupA] = FName(TEXT("SlotA"));
        // Leave SlotB unused so the baked sections exercise a sparse material index (0 and 2).
        SlotNames[GroupC] = FName(TEXT("SlotC"));

        const auto AddTriangle = [&](FPolygonGroupID Group, float X)
        {
            FVertexID Vertices[3];
            FVertexInstanceID Instances[3];
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                Vertices[Corner] = MeshDescription.CreateVertex();
                Instances[Corner] = MeshDescription.CreateVertexInstance(Vertices[Corner]);
                Normals[Instances[Corner]] = FVector3f(0.0f, 0.0f, 1.0f);
                Tangents[Instances[Corner]] = FVector3f(1.0f, 0.0f, 0.0f);
                BinormalSigns[Instances[Corner]] = 1.0f;
            }
            Positions[Vertices[0]] = FVector3f(X, 0.0f, 0.0f);
            Positions[Vertices[1]] = FVector3f(X + 1.0f, 0.0f, 0.0f);
            Positions[Vertices[2]] = FVector3f(X, 1.0f, 0.0f);
            UVs.Set(Instances[0], 0, FVector2f(0.0f, 0.0f));
            UVs.Set(Instances[1], 0, FVector2f(1.0f, 0.0f));
            UVs.Set(Instances[2], 0, FVector2f(0.0f, 1.0f));
            UVs.Set(Instances[0], 1, FVector2f(0.0f, 0.0f));
            UVs.Set(Instances[1], 1, FVector2f(0.5f, 0.0f));
            UVs.Set(Instances[2], 1, FVector2f(0.0f, 0.5f));
            MeshDescription.CreateTriangle(Group, Instances);
        };

        AddTriangle(GroupA, 0.0f);
        AddTriangle(GroupC, 2.0f);
        AddTriangle(GroupC, 4.0f);

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bFastBuild = true;
        BuildParams.bAllowCpuAccess = true;
        if (!Mesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams))
        {
            return nullptr;
        }

        // BuildFromMeshDescriptions assigns compact material indices to this transient mesh. Keep
        // the intentionally unused middle slot in the fixture by restoring the authored section
        // mapping before resolving the render sections.
        const FMeshSectionInfo SparseSectionInfo(2);
        Mesh->GetSectionInfoMap().Set(0, 1, SparseSectionInfo);
        Mesh->GetOriginalSectionInfoMap().Set(0, 1, SparseSectionInfo);
        Mesh->GetRenderData()->ResolveSectionInfo(Mesh);
        Mesh->SetLightMapCoordinateIndex(1);
        return Mesh;
    }
}

// ============================================================================
// The asset -> dynamic mesh direction brings the geometry AND the skinning across.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateFromSkeletalMeshLoadsSkinnedGeometryTest,
    "PinWright.geometry.create_from_skeletal_mesh.LoadsSkinnedGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateFromSkeletalMeshLoadsSkinnedGeometryTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping create_from_skeletal_mesh load test"));
        return true;
    }
    if (!SkeletalFixturesAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube unavailable; skipping create_from_skeletal_mesh load test"));
        return true;
    }

    const FString Label = SkeletalLabel(TEXT("PW_SkelLoadProbe"));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), SkeletalSourceAsset);
    Params->SetStringField(TEXT("name"), Label);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_skeletal_mesh"),
        TEXT("req-skel-load"), Params, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.create_from_skeletal_mesh loaded the engine skeletal cube"), bSuccess) ||
        !TestTrue(TEXT("load success carries a result object"), Result.IsValid()))
    {
        AddError(FString::Printf(TEXT("create_from_skeletal_mesh failed with code '%s'"), *ErrorCode));
        DestroyActorsWithLabel(Label);
        return true;
    }

    const double Triangles = ReadNumber(Result, TEXT("triangleCount"), 0.0);
    TestTrue(TEXT("the loaded skeletal cube carries triangles"), Triangles >= 12.0);

    TestFalse(TEXT("a first load is not a reuse"), ReadBool(Result, TEXT("reused"), true));

    // The whole point of using CopyMeshFromSkeletalMesh rather than the StaticMesh path: the skin
    // weights and the bone hierarchy come across with the geometry, so the mesh is immediately
    // re-writable as a skeletal asset without a re-bind.
    TestTrue(TEXT("the source asset's skin weights survived the copy"),
        ReadBool(Result, TEXT("hasBoneWeights"), false));
    TestTrue(TEXT("every vertex of the source asset arrives weighted"),
        ReadBool(Result, TEXT("fullyWeighted"), false));
    TestEqual(TEXT("no vertex arrives unweighted"),
        (int32)ReadNumber(Result, TEXT("verticesUnweighted"), -1.0), 0);
    TestTrue(TEXT("the bone hierarchy came across too"),
        ReadNumber(Result, TEXT("boneCount"), 0.0) >= 1.0);

    FString SkeletonPath;
    TestTrue(TEXT("response names the source asset's skeleton"),
        Result->TryGetStringField(TEXT("skeletonPath"), SkeletonPath) && !SkeletonPath.IsEmpty());

    // A SkeletalMesh has no HiRes source model, so the engine collapses MaxAvailable and
    // HiResSourceModel to SourceModel. The response has to say what was actually read.
    FString EffectiveLodType;
    TestTrue(TEXT("response reports the LOD type the engine actually used"),
        Result->TryGetStringField(TEXT("effectiveLodType"), EffectiveLodType));
    TestEqual(TEXT("the default read resolves to SourceModel"), EffectiveLodType, FString(TEXT("SourceModel")));

    // GROUND TRUTH: a real ADynamicMeshActor with real geometry, not just an echo.
    TestEqual(TEXT("the live DynamicMeshComponent carries the loaded triangles"),
        SkeletalLiveTriangleCount(Label), (int32)Triangles);

    DestroyActorsWithLabel(Label);
    return true;
}

// ============================================================================
// A mesh that was never bound cannot be written. The engine would not stop this: the create
// path reports its refusal only into the discarded UGeometryScriptDebug object, and the
// overwrite path does not refuse at all.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalRejectsUnskinnedMeshTest,
    "PinWright.geometry.convert_to_skeletal_mesh.RejectsUnskinnedMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalRejectsUnskinnedMeshTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping unskinned-mesh rejection test"));
        return true;
    }
    if (!SkeletalFixturesAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube unavailable; skipping unskinned-mesh rejection test"));
        return true;
    }

    bSuppressLogErrors = true;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_SkelUnskinned_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/PW_SkelUnskinned_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto Cleanup = [&]()
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(Label);
    };

    // A plain box: real geometry, no bones, no weights.
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-skel-unskinned-seed"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box seeded the unskinned probe"), bCreated))
        {
            Cleanup();
            return true;
        }
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-unskinned-write"), Params, bSuccess, Result, ErrorCode);

        TestFalse(TEXT("baking a mesh with no skin weights is not a success"), bSuccess);
        TestEqual(TEXT("baking a mesh with no skin weights is NO_SKIN_WEIGHTS"),
            ErrorCode, FString(TEXT("NO_SKIN_WEIGHTS")));
    }

    // GROUND TRUTH: the refusal happened BEFORE anything was written. A guard that returns the
    // right code after creating the asset would have shipped the defect anyway.
    TestTrue(TEXT("no SkeletalMesh asset was left behind by the refused bake"),
        NothingAtAssetPath(AssetPath));

    // The same verb must also refuse a bind with no skeleton to bind to, rather than inventing one.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bind_skin_weights"),
            TEXT("req-skel-bind-noskeleton"), Params, bSuccess, ErrorCode);
        TestFalse(TEXT("binding without a skeleton is not a success"), bSuccess);
        TestEqual(TEXT("binding without a skeleton is SKELETON_NOT_FOUND"),
            ErrorCode, FString(TEXT("SKELETON_NOT_FOUND")));
    }

    Cleanup();
    return true;
}

// ============================================================================
// THE ORDERING TRAP.
//
// mesh_create_bone_weights has to run after all geometry is appended. The skin-weight attribute
// store DOES grow with the mesh, but it pads new vertices with EMPTY influences - so geometry
// appended after a bind is silently unskinned, and both engine write paths accept it (the create
// path only checks that the attribute exists; the overwrite path, under the default
// BoneHierarchyMismatchHandling, writes zero-filled weights and reports Success).
//
// This test drives exactly that sequence and asserts the write is REJECTED, then that re-binding
// is the way out.
//
// The geometry op is geometry.append_triangle, chosen because its mechanism is directly readable:
// it calls FDynamicMesh3::AppendVertex three times (MeshInfoHandler.cpp:350-352), and the skin
// weight layer's only hook for that is OnNewVertex -> resize-with-EMPTY
// (DynamicVertexSkinWeightsAttribute.h:461-464). geometry.subdivide was deliberately NOT used:
// the same layer implements OnSplitEdge and OnPokeTriangle with real interpolation
// (DynamicVertexSkinWeightsAttribute.h:430-434, :467-472), so whether a tessellation verb leaves
// vertices unweighted depends on which primitives its implementation happens to use - an
// unverified detail that would make this test's premise conditional on somebody else's code.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalOrderingTrapTest,
    "PinWright.geometry.convert_to_skeletal_mesh.RejectsStaleSkinBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalOrderingTrapTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping skin-binding ordering test"));
        return true;
    }
    if (!SkeletalFixturesAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube unavailable; skipping skin-binding ordering test"));
        return true;
    }

    bSuppressLogErrors = true;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_SkelOrdering_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/PW_SkelOrdering_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto Cleanup = [&]()
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(Label);
    };

    auto BindOnce = [&](const TCHAR* RequestId, bool& bOutSuccess, TSharedPtr<FJsonObject>& OutResult)
    {
        TSharedPtr<FJsonObject> BindParams = MakeShared<FJsonObject>();
        BindParams->SetStringField(TEXT("actorName"), Label);
        BindParams->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        BindParams->SetNumberField(TEXT("maxInfluences"), 4);
        FString BindErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bind_skin_weights"),
            RequestId, BindParams, bOutSuccess, OutResult, BindErr);
    };

    // 1. Load a skinned mesh into an editable actor.
    {
        TSharedPtr<FJsonObject> LoadParams = MakeShared<FJsonObject>();
        LoadParams->SetStringField(TEXT("assetPath"), SkeletalSourceAsset);
        LoadParams->SetStringField(TEXT("name"), Label);
        bool bLoaded = false;
        FString LoadErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_skeletal_mesh"),
            TEXT("req-skel-order-load"), LoadParams, bLoaded, LoadErr);
        if (!TestTrue(TEXT("the skeletal cube loaded into an editable DynamicMeshActor"), bLoaded))
        {
            Cleanup();
            return true;
        }
    }

    // 2. Bind. This is the correct thing to do at this instant - the mesh is complete.
    {
        bool bBound = false;
        TSharedPtr<FJsonObject> BindResult;
        BindOnce(TEXT("req-skel-order-bind1"), bBound, BindResult);
        if (!TestTrue(TEXT("geometry.bind_skin_weights bound the loaded mesh"), bBound))
        {
            Cleanup();
            return true;
        }
        TestTrue(TEXT("the first bind covers every vertex"),
            ReadBool(BindResult, TEXT("fullyWeighted"), false));
        TestEqual(TEXT("the first bind leaves no vertex unweighted"),
            (int32)ReadNumber(BindResult, TEXT("verticesUnweighted"), -1.0), 0);
        // The base profile, never a named alternate one - that is the whole difference from the
        // skeleton.* weight verbs, and a caller has to be able to see which was written.
        FString Profile;
        TestTrue(TEXT("the bind reports which weight profile it wrote"),
            BindResult.IsValid() && BindResult->TryGetStringField(TEXT("profile"), Profile));
        TestEqual(TEXT("the bind writes the BASE skin weight profile"), Profile, FString(TEXT("Default")));
    }

    // 3. Edit the geometry AFTER binding. This is the mistake being trapped.
    {
        TSharedPtr<FJsonObject> AppendParams = MakeShared<FJsonObject>();
        AppendParams->SetStringField(TEXT("actorName"), Label);
        bool bAppended = false;
        FString AppendErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.append_triangle"),
            TEXT("req-skel-order-append"), AppendParams, bAppended, AppendErr);
        if (!TestTrue(TEXT("geometry.append_triangle added post-bind geometry"), bAppended))
        {
            Cleanup();
            return true;
        }
    }

    // 4. The write must now be REFUSED. Without the coverage scan this call SUCCEEDS and produces
    //    an asset whose three newest vertices have no influences at all.
    {
        TSharedPtr<FJsonObject> WriteParams = MakeShared<FJsonObject>();
        WriteParams->SetStringField(TEXT("actorName"), Label);
        WriteParams->SetStringField(TEXT("assetPath"), AssetPath);
        WriteParams->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-order-write"), WriteParams, bSuccess, Result, ErrorCode);

        TestFalse(TEXT("baking a mesh edited after its bind is not a success"), bSuccess);
        TestEqual(TEXT("baking a mesh edited after its bind is SKIN_WEIGHTS_INCOMPLETE"),
            ErrorCode, FString(TEXT("SKIN_WEIGHTS_INCOMPLETE")));
    }

    // GROUND TRUTH for the whole feature: nothing was written. The defect this replaces was a
    // successful-looking bake of a partly-unskinned asset.
    TestTrue(TEXT("no SkeletalMesh asset was left behind by the refused bake"),
        NothingAtAssetPath(AssetPath));

    // 5. Re-binding is the documented way out, and it has to actually restore full coverage -
    //    otherwise the error message sends callers into a loop they cannot exit.
    {
        bool bRebound = false;
        TSharedPtr<FJsonObject> RebindResult;
        BindOnce(TEXT("req-skel-order-bind2"), bRebound, RebindResult);
        TestTrue(TEXT("geometry.bind_skin_weights re-binds after a geometry edit"), bRebound);
        TestTrue(TEXT("the re-bind reports that it replaced an existing binding"),
            ReadBool(RebindResult, TEXT("rebound"), false));
        TestTrue(TEXT("the re-bind restores full coverage over the edited mesh"),
            ReadBool(RebindResult, TEXT("fullyWeighted"), false));
        TestEqual(TEXT("the re-bind leaves no vertex unweighted"),
            (int32)ReadNumber(RebindResult, TEXT("verticesUnweighted"), -1.0), 0);
    }

    Cleanup();
    return true;
}

// ============================================================================
// The full write round trip, plus the collision guard.
//
// CreateNewSkeletalMeshAssetFromMesh performs no collision check of its own; the check happens
// inside UE::AssetUtils::CreateSkeletalMeshAsset, which REUSES an existing asset at the path and
// empties its LOD models, source models, materials, reference skeleton and physics asset in
// place. Silently. So `overwrite` has to be a real gate, not just a branch selector.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalCreateRefusesOccupiedPathTest,
    "PinWright.geometry.convert_to_skeletal_mesh.CreatesThenRefusesOccupiedPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalCreateRefusesOccupiedPathTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping skeletal bake/collision test"));
        return true;
    }
    if (!SkeletalFixturesAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube unavailable; skipping skeletal bake/collision test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_SkelBake_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/PW_SkelBake_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto Cleanup = [&]()
    {
        CleanupTestAsset(AssetPath);
        DestroyActorsWithLabel(Label);
    };

    // Load -> bind -> bake. No geometry edit in between, so this is the clean happy path.
    {
        TSharedPtr<FJsonObject> LoadParams = MakeShared<FJsonObject>();
        LoadParams->SetStringField(TEXT("assetPath"), SkeletalSourceAsset);
        LoadParams->SetStringField(TEXT("name"), Label);
        bool bLoaded = false;
        FString LoadErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_skeletal_mesh"),
            TEXT("req-skel-bake-load"), LoadParams, bLoaded, LoadErr);
        if (!TestTrue(TEXT("the skeletal cube loaded into an editable DynamicMeshActor"), bLoaded))
        {
            Cleanup();
            return true;
        }
    }
    {
        TSharedPtr<FJsonObject> BindParams = MakeShared<FJsonObject>();
        BindParams->SetStringField(TEXT("actorName"), Label);
        BindParams->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        bool bBound = false;
        FString BindErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bind_skin_weights"),
            TEXT("req-skel-bake-bind"), BindParams, bBound, BindErr);
        if (!TestTrue(TEXT("geometry.bind_skin_weights bound the mesh before the bake"), bBound))
        {
            Cleanup();
            return true;
        }
    }

    {
        TSharedPtr<FJsonObject> BakeParams = MakeShared<FJsonObject>();
        BakeParams->SetStringField(TEXT("actorName"), Label);
        BakeParams->SetStringField(TEXT("assetPath"), AssetPath);
        BakeParams->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        bool bBaked = false;
        FString BakeErr;
        TSharedPtr<FJsonObject> BakeResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-bake-create"), BakeParams, bBaked, BakeResult, BakeErr);
        if (!TestTrue(TEXT("a bound mesh bakes into a SkeletalMesh asset"), bBaked))
        {
            AddError(FString::Printf(TEXT("convert_to_skeletal_mesh failed with code '%s'"), *BakeErr));
            Cleanup();
            return true;
        }
        TestTrue(TEXT("the create branch reports created"), ReadBool(BakeResult, TEXT("created"), false));
        TestFalse(TEXT("the create branch does not claim updated"), ReadBool(BakeResult, TEXT("updated"), true));
        // Neither engine entry point writes the .uasset itself, so a saved:true that is not
        // backed by a file on disk is precisely the regression AddAssetSaveReport exists to make
        // impossible.
        TestTrue(TEXT("the bake reports it saved"), ReadBool(BakeResult, TEXT("saved"), false));
        TestTrue(TEXT("the bake reports a non-zero on-disk size"),
            ReadNumber(BakeResult, TEXT("sizeBytes"), 0.0) > 0.0);
        FString SkeletonPackage;
        TestTrue(TEXT("the bake names the referenced skeleton package"),
            BakeResult->TryGetStringField(TEXT("skeletonPackage"), SkeletonPackage)
            && !SkeletonPackage.IsEmpty());
        FString SkeletonSaveState;
        TestTrue(TEXT("the bake reports the referenced skeleton save state"),
            BakeResult->TryGetStringField(TEXT("skeletonSaveState"), SkeletonSaveState)
            && !SkeletonSaveState.IsEmpty());
    }

    // GROUND TRUTH: a loadable USkeletalMesh, bound to the right skeleton.
    if (USkeletalMesh* Baked = LoadObject<USkeletalMesh>(nullptr, *ToObjectPath(AssetPath)))
    {
        TestNotNull(TEXT("the baked SkeletalMesh carries a skeleton"), Baked->GetSkeleton());
        // Not merely non-null: the identity is the property. A bake that bound the asset to
        // a stale, default or arbitrarily-resident USkeleton still yields a non-null
        // GetSkeleton() and passes a bare TestNotNull, while the asset animates against the
        // wrong hierarchy. (This fixture cannot separate "honored skeletonPath" from "fell
        // back to the source mesh's own skeleton" - SkeletalCube's skeleton IS
        // SkeletalSourceSkeleton. Separating those needs a second USkeleton fixture with a
        // different bone hierarchy, which the engine content does not ship.)
        TestTrue(TEXT("the baked SkeletalMesh is bound to the skeleton the request named"),
            Baked->GetSkeleton() == LoadObject<USkeleton>(nullptr, SkeletalSourceSkeleton));
        TestTrue(TEXT("the baked SkeletalMesh has at least one material slot"),
            Baked->GetMaterials().Num() >= 1);
    }
    else
    {
        AddError(TEXT("the baked SkeletalMesh asset is not loadable from disk"));
    }

    // Re-baking WITHOUT overwrite must be refused. The engine would have silently emptied the
    // asset's LODs, materials, reference skeleton and physics asset and reported success.
    {
        bSuppressLogErrors = true;
        TSharedPtr<FJsonObject> BakeParams = MakeShared<FJsonObject>();
        BakeParams->SetStringField(TEXT("actorName"), Label);
        BakeParams->SetStringField(TEXT("assetPath"), AssetPath);
        BakeParams->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-bake-collide"), BakeParams, bSuccess, ErrorCode);
        TestFalse(TEXT("creating over an occupied assetPath is not a success"), bSuccess);
        TestEqual(TEXT("creating over an occupied assetPath is ASSET_EXISTS"),
            ErrorCode, FString(TEXT("ASSET_EXISTS")));
    }

    // And the /Engine refusal on the overwrite branch, which the engine also enforces but only
    // into the discarded Debug object.
    {
        bSuppressLogErrors = true;
        TSharedPtr<FJsonObject> BakeParams = MakeShared<FJsonObject>();
        BakeParams->SetStringField(TEXT("actorName"), Label);
        BakeParams->SetStringField(TEXT("assetPath"), TEXT("/Engine/EngineMeshes/SkeletalCube"));
        BakeParams->SetBoolField(TEXT("overwrite"), true);
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-bake-engine"), BakeParams, bSuccess, ErrorCode);
        TestFalse(TEXT("overwriting an /Engine skeletal asset is not a success"), bSuccess);
        // INVALID_ASSET_PATH is also acceptable - SanitizeProjectRelativePath may reject /Engine
        // before the handler's own guard, depending on the configured mount points.
        TestTrue(TEXT("overwriting an /Engine skeletal asset is refused with a typed code"),
            ErrorCode == TEXT("SECURITY_VIOLATION") || ErrorCode == TEXT("INVALID_ASSET_PATH"));
    }

    Cleanup();
    return true;
}

// ============================================================================
// A multi-section static source exercises the handler path that previously created one null
// skeletal material slot regardless of the imported sections. The unused middle source slot also
// keeps material IDs sparse (0 and 2), so padding by section count alone is not sufficient.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalMaterialSlotsCoverSectionsTest,
    "PinWright.geometry.convert_to_skeletal_mesh.MaterialSlotsCoverSections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalMaterialSlotsCoverSectionsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping skeletal material-slot coverage test"));
        return true;
    }
    if (!SkeletalFixturesAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube unavailable; skipping skeletal material-slot coverage test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/SM_SkeletalSlots_%s"), *Suffix);
    const FString AssetPath = FString::Printf(
        TEXT("/Game/GeneratedMeshes/SKM_SkeletalSlots_%s"), *Suffix);
    const FString Label = FString::Printf(TEXT("PW_SkelSlots_%s"), *Suffix);

    TStrongObjectPtr<UStaticMesh> Source(
        SkeletalMaterialSlotsTest_CreateMultiSectionFixture(SourcePackagePath));
    ON_SCOPE_EXIT
    {
        DestroyActorsWithLabel(Label);
        Source.Reset();
        CleanupTestAsset(AssetPath);
        CleanupTestAsset(SourcePackagePath);
    };
    if (!TestNotNull(TEXT("the multi-section static source was built"), Source.Get()))
    {
        return true;
    }

    const FStaticMeshRenderData* RenderData = Source->GetRenderData();
    if (!TestNotNull(TEXT("the static source has render data"), RenderData)
        || !TestTrue(TEXT("the static source has LOD0"),
            RenderData && RenderData->LODResources.Num() > 0))
    {
        return true;
    }
    const FStaticMeshLODResources& SourceLOD0 = RenderData->LODResources[0];
    int32 MaxSourceMaterialIndex = -1;
    for (const FStaticMeshSection& Section : SourceLOD0.Sections)
    {
        MaxSourceMaterialIndex = FMath::Max(MaxSourceMaterialIndex, Section.MaterialIndex);
    }
    TestTrue(TEXT("the static source has at least two render sections"),
        SourceLOD0.Sections.Num() >= 2);
    TestTrue(TEXT("the static source preserves a sparse material index"),
        MaxSourceMaterialIndex >= 2);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), Source->GetPathName());
        Params->SetStringField(TEXT("name"), Label);
        Params->SetStringField(TEXT("lodType"), TEXT("RenderData"));
        bool bLoaded = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_from_static_mesh"),
            TEXT("req-skel-slots-load"), Params, bLoaded, ErrorCode);
        if (!TestTrue(TEXT("the multi-section static source loaded through the handler"), bLoaded))
        {
            return true;
        }
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        Params->SetNumberField(TEXT("maxInfluences"), 4);
        bool bBound = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bind_skin_weights"),
            TEXT("req-skel-slots-bind"), Params, bBound, ErrorCode);
        if (!TestTrue(TEXT("the handler bound the imported mesh before baking"), bBound))
        {
            return true;
        }
    }

    TSharedPtr<FJsonObject> BakeResult;
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("skeletonPath"), SkeletalSourceSkeleton);
        Params->SetBoolField(TEXT("save"), true);
        bool bBaked = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_skeletal_mesh"),
            TEXT("req-skel-slots-bake"), Params, bBaked, BakeResult, ErrorCode);
        if (!TestTrue(TEXT("the multi-section mesh baked into a skeletal asset"), bBaked)
            || !TestTrue(TEXT("the bake returned a result object"), BakeResult.IsValid()))
        {
            return true;
        }
    }

    double ResponseSlots = 0.0;
    TestTrue(TEXT("the bake reports repaired materialSlots"),
        BakeResult->TryGetNumberField(TEXT("materialSlots"), ResponseSlots));
    TestTrue(TEXT("the response material slots cover every referenced material index"),
        ResponseSlots >= MaxSourceMaterialIndex + 1);

    bool bExistsOnDisk = false;
    TestTrue(TEXT("the bake reports a durable asset"),
        BakeResult->TryGetBoolField(TEXT("exists"), bExistsOnDisk));
    TestTrue(TEXT("the bake saved the skeletal asset to disk"), bExistsOnDisk);

    // GROUND TRUTH: reset the resident package loader, then invoke the production asset.reload
    // handler. Its package eviction guarantees the following reacquire reads persisted bytes
    // instead of returning the object that was already resident after the convert.
    UPackage* BakedPackage = FindPackage(
        nullptr, *FPackageName::ObjectPathToPackageName(AssetPath));
    if (!TestNotNull(TEXT("the saved SkeletalMesh package is resident before reload"), BakedPackage))
    {
        return true;
    }

    ResetLoaders(BakedPackage);
    TSharedPtr<FJsonObject> ReloadParams = MakeShared<FJsonObject>();
    ReloadParams->SetStringField(TEXT("assetPath"), ToObjectPath(AssetPath));
    FTestResponseCapture ReloadCapture;
    TestTrue(TEXT("asset.reload handler is registered"),
        InvokeHandlerWithCapture(TEXT("asset.reload"), ReloadParams, ReloadCapture));
    TestTrue(TEXT("asset.reload succeeded"), ReloadCapture.bSuccess);
    bool bReloaded = false;
    TestTrue(TEXT("asset.reload reports reloaded:true"),
        ReloadCapture.Result.IsValid()
            && ReloadCapture.Result->TryGetBoolField(TEXT("reloaded"), bReloaded));
    TestTrue(TEXT("asset.reload actually reloaded the SkeletalMesh"), bReloaded);
    if (!ReloadCapture.bSuccess || !bReloaded)
    {
        return true;
    }

    USkeletalMesh* Baked = LoadObject<USkeletalMesh>(nullptr, *ToObjectPath(AssetPath));
    if (TestNotNull(TEXT("the saved SkeletalMesh reloaded after loader reset"), Baked))
    {
        // The load hands the mesh's build to FSkinnedAssetCompilingManager's thread pool, and the
        // section arrays read below are the build's output: sampling them while it is still
        // running reports whatever partial LOD state exists at that instant (observed as a
        // one-section LOD0 on an otherwise identical run). Block until the build is published.
        FSkinnedAssetCompilingManager::Get().FinishCompilation({ Baked });

        const FSkeletalMeshModel* ImportedModel = Baked->GetImportedModel();
        if (TestNotNull(TEXT("the reloaded SkeletalMesh has imported model data"), ImportedModel)
            && TestTrue(TEXT("the reloaded SkeletalMesh has LOD0"),
                ImportedModel->LODModels.IsValidIndex(0)))
        {
            const FSkeletalMeshLODModel& LOD0 = ImportedModel->LODModels[0];
            const TArray<FSkeletalMaterial>& Materials = Baked->GetMaterials();
            TestTrue(TEXT("the reloaded LOD0 retains multiple sections"), LOD0.Sections.Num() >= 2);
            TestTrue(TEXT("the reloaded material array covers sparse section indices"),
                Materials.Num() >= MaxSourceMaterialIndex + 1);
            for (int32 Index = 0; Index < Materials.Num(); ++Index)
            {
                TestNotNull(*FString::Printf(TEXT("reloaded material slot %d is non-null"), Index),
                    Materials[Index].MaterialInterface.Get());
            }
            for (const FSkelMeshSection& Section : LOD0.Sections)
            {
                TestTrue(TEXT("every reloaded LOD0 section material index is in range"),
                    Materials.IsValidIndex(Section.MaterialIndex));
                if (Materials.IsValidIndex(Section.MaterialIndex))
                {
                    TestNotNull(TEXT("every reloaded LOD0 section resolves to a material"),
                        Materials[Section.MaterialIndex].MaterialInterface.Get());
                }
            }

            const FSkeletalMeshRenderData* SkeletalRenderData = Baked->GetResourceForRendering();
            if (TestNotNull(TEXT("the reloaded SkeletalMesh has render data"), SkeletalRenderData)
                && TestTrue(TEXT("the reloaded SkeletalMesh has render LOD0"),
                    SkeletalRenderData->LODRenderData.IsValidIndex(0)))
            {
                const FSkeletalMeshLODRenderData& RenderLOD0 = SkeletalRenderData->LODRenderData[0];
                TestTrue(TEXT("the reloaded render LOD0 retains multiple sections"),
                    RenderLOD0.RenderSections.Num() >= 2);
                for (const FSkelMeshRenderSection& Section : RenderLOD0.RenderSections)
                {
                    TestTrue(TEXT("every reloaded render section material index is in range"),
                        Materials.IsValidIndex(Section.MaterialIndex));
                    if (Materials.IsValidIndex(Section.MaterialIndex))
                    {
                        TestNotNull(TEXT("every reloaded render section resolves to a material"),
                            Materials[Section.MaterialIndex].MaterialInterface.Get());
                    }
                }
            }
        }
    }
    else
    {
        AddError(TEXT("the saved SkeletalMesh was not reacquirable after asset.reload"));
    }

    return true;
}
