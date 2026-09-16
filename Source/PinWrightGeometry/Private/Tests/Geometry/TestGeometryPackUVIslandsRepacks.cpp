// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction guard for B-pack-uv-islands-is-unwrap.
//
// geometry.pack_uv_islands used to run the same XAtlas auto-unwrap as unwrap_uv and auto_uv, so
// it RECOMPUTED the layer it claimed to be packing - silently discarding whatever project_uv or
// transform_uvs had seated - and echoed a textureResolution that reached no engine call. It now
// runs GeometryOps::LayoutUV in Repack mode.
//
// The hard part is finding a signal that separates "repacked" from "re-unwrapped", because both
// leave a mesh with plausible UVs. This uses one: a PLANAR projection collapses every face
// parallel to the projection axis to zero UV area, and a repack only translates and scales
// islands, so those degenerate triangles stay degenerate. An XAtlas unwrap gives every triangle
// real UV area and drives the count to zero. The control is measured on the same fixture rather
// than assumed - if the projection does not produce degenerate faces, the precondition fails and
// the test reports that instead of passing on a signal it never had.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;
using GeometryTestHelpers::DestroyActorsWithLabel;
using GeometryTestHelpers::FindActorByLabel;

namespace PackUVIslandsTest
{
    struct FUVStats
    {
        bool bRead = false;
        int32 ElementCount = 0;
        int32 DegenerateTriangles = 0;   // triangles whose UV area is ~0
        double TotalUVArea = 0.0;
    };

    // Reads UV channel 0 straight off the actor's dynamic mesh. Deliberately not through a verb:
    // the point is to measure what the mesh holds, not what a response claims about it.
    inline FUVStats ReadUVStats(const FString& Label)
    {
        FUVStats Stats;
        ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(FindActorByLabel(Label));
        if (!Actor || !Actor->GetDynamicMeshComponent() || !Actor->GetDynamicMeshComponent()->GetDynamicMesh())
        {
            return Stats;
        }

        const UE::Geometry::FDynamicMesh3& Mesh =
            Actor->GetDynamicMeshComponent()->GetDynamicMesh()->GetMeshRef();
        if (!Mesh.HasAttributes() || Mesh.Attributes()->NumUVLayers() < 1)
        {
            return Stats;
        }
        const UE::Geometry::FDynamicMeshUVOverlay* UV = Mesh.Attributes()->GetUVLayer(0);
        if (!UV)
        {
            return Stats;
        }

        Stats.bRead = true;
        Stats.ElementCount = UV->ElementCount();

        for (const int32 TriangleID : Mesh.TriangleIndicesItr())
        {
            if (!UV->IsSetTriangle(TriangleID))
            {
                continue;
            }
            FVector2f A, B, C;
            UV->GetTriElements(TriangleID, A, B, C);
            const double Area = 0.5 * FMath::Abs(
                (B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y));
            Stats.TotalUVArea += Area;
            if (Area < 1e-9)
            {
                ++Stats.DegenerateTriangles;
            }
        }
        return Stats;
    }

    // Spawns two disconnected boxes and seats a PLANAR projection on UV channel 0. Two islands
    // are required here: with one island, the packer can normalize the same chart to the unit
    // square at every resolution, so the texture-resolution differential would test nothing.
    // Returns false if any verb refused, so a broken fixture fails loudly instead of being read as
    // a result.
    inline bool SeedPlanarProjectedBox(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        FSinkPtr& Sink, const FString& Label)
    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bOk = false;
        FString Err;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"), TEXT("req-pack-create"),
            CreateParams, bOk, Err);
        if (!Test.TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bOk))
        {
            return false;
        }

        TSharedPtr<FJsonObject> ArrayParams = MakeShared<FJsonObject>();
        ArrayParams->SetStringField(TEXT("actorName"), Label);
        ArrayParams->SetNumberField(TEXT("count"), 2);
        TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
        Offset->SetNumberField(TEXT("x"), 300.0);
        Offset->SetNumberField(TEXT("y"), 0.0);
        Offset->SetNumberField(TEXT("z"), 0.0);
        ArrayParams->SetObjectField(TEXT("offset"), Offset);
        bOk = false;
        Dispatch(Dispatcher, Sink, TEXT("geometry.array_linear"), TEXT("req-pack-array"),
            ArrayParams, bOk, Err);
        if (!Test.TestTrue(TEXT("geometry.array_linear made a second disconnected UV island"), bOk))
        {
            return false;
        }

        TSharedPtr<FJsonObject> ProjParams = MakeShared<FJsonObject>();
        ProjParams->SetStringField(TEXT("actorName"), Label);
        ProjParams->SetStringField(TEXT("projectionType"), TEXT("planar"));
        bOk = false;
        Dispatch(Dispatcher, Sink, TEXT("geometry.project_uv"), TEXT("req-pack-project"),
            ProjParams, bOk, Err);
        return Test.TestTrue(TEXT("geometry.project_uv seated a planar layout to pack"), bOk);
    }
}

using PackUVIslandsTest::ReadUVStats;
using PackUVIslandsTest::SeedPlanarProjectedBox;

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryPackUVIslandsPreservesLayoutTest,
    "PinWright.geometry.pack_uv_islands.PackRearrangesTheLayoutInsteadOfReplacingIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryPackUVIslandsPreservesLayoutTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "pack_uv_islands layout-preservation assertions did not run"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_PackPreserveProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    if (!SeedPlanarProjectedBox(*this, Dispatcher, Sink, Label))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    const PackUVIslandsTest::FUVStats Before = ReadUVStats(Label);
    if (!TestTrue(TEXT("the probe's UV channel 0 could be read before packing"), Before.bRead))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // Precondition, and the known-bad control: a planar projection MUST leave the faces parallel
    // to the projection axis with zero UV area. If it does not, this fixture cannot tell a repack
    // from an unwrap and the test must say so rather than pass.
    if (!TestTrue(TEXT("planar projection left degenerate-UV faces for the assertion to track"),
            Before.DegenerateTriangles > 0))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    TSharedPtr<FJsonObject> PackParams = MakeShared<FJsonObject>();
    PackParams->SetStringField(TEXT("actorName"), Label);
    PackParams->SetNumberField(TEXT("textureResolution"), 1024);
    bool bPacked = false;
    FString PackErr;
    TSharedPtr<FJsonObject> PackResult;
    Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"), TEXT("req-pack"),
        PackParams, bPacked, PackResult, PackErr);

    const PackUVIslandsTest::FUVStats After = ReadUVStats(Label);
    DestroyActorsWithLabel(Label);

    if (!TestTrue(TEXT("geometry.pack_uv_islands succeeded"), bPacked) ||
        !TestTrue(TEXT("the probe's UV channel 0 could be read after packing"), After.bRead))
    {
        return false;
    }

    // The assertion the ticket is about. A repack moves islands; it does not re-solve the mesh
    // into new ones, so the projection's degenerate faces are still degenerate. An XAtlas unwrap
    // gives every triangle real UV area and drives this to zero.
    TestTrue(FString::Printf(
        TEXT("packing must rearrange the existing layout, not regenerate it: %d degenerate-UV "
             "faces before, %d after - zero after means the layer was re-unwrapped"),
        Before.DegenerateTriangles, After.DegenerateTriangles),
        After.DegenerateTriangles > 0);
    return true;
}

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryPackUVIslandsResolutionClampTest,
    "PinWright.geometry.pack_uv_islands.TextureResolutionClampsToPackerBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryPackUVIslandsResolutionClampTest::RunTest(const FString& Parameters)
{
    FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FHandlerRegistration* Registration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("geometry.pack_uv_islands"));
    const FParamSpec* ResolutionSpec = Registration
        ? Registration->Params.FindByPredicate([](const FParamSpec& Spec)
            {
                return Spec.Name == TEXT("textureResolution");
            })
        : nullptr;
    if (TestNotNull(TEXT("pack_uv_islands publishes textureResolution"), ResolutionSpec))
    {
        TestEqual(TEXT("published default uses the production layout default"),
            ResolutionSpec->Default,
            FString::FromInt(GeometryOps::LayoutUVTextureResolutionDefault));
        TestTrue(TEXT("published metadata names the exact production range"),
            ResolutionSpec->Description.Contains(FString::Printf(
                TEXT("engine range %d-%d"),
                GeometryOps::LayoutUVTextureResolutionMin,
                GeometryOps::LayoutUVTextureResolutionMax)));
        TestTrue(TEXT("published metadata says positive out-of-range values clamp"),
            ResolutionSpec->Description.Contains(TEXT("clamped with a warning")));
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), TEXT("PW_UnresolvedRangeProbe"));
        Params->SetNumberField(TEXT("textureResolution"), 0);
        bool bOk = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"),
            TEXT("req-pack-zero"), Params, bOk, ErrorCode);

        TestFalse(TEXT("zero resolution keeps the established refusal"), bOk);
        TestEqual(TEXT("zero resolution is an argument error"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    for (const int32 ClampedResolution : {
             GeometryOps::LayoutUVTextureResolutionMin - 1,
             GeometryOps::LayoutUVTextureResolutionMax + 1 })
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), TEXT("PW_UnresolvedRangeProbe"));
        Params->SetNumberField(TEXT("textureResolution"), ClampedResolution);
        bool bOk = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"),
            TEXT("req-pack-positive-outside-engine-range"), Params, bOk, ErrorCode);

        TestFalse(TEXT("the unresolved compatibility probe still fails actor lookup"), bOk);
        TestTrue(FString::Printf(
            TEXT("positive resolution %d reaches actor lookup instead of range rejection"),
            ClampedResolution), ErrorCode != TEXT("INVALID_ARGUMENT"));
    }

    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; pack_uv_islands clamp echo/warning assertions did not run"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_PackClampProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!SeedPlanarProjectedBox(*this, Dispatcher, Sink, Label))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetNumberField(TEXT("textureResolution"), 1);
    bool bOk = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"),
        TEXT("req-pack-clamp"), Params, bOk, Result, ErrorCode);
    DestroyActorsWithLabel(Label);

    TestTrue(TEXT("the formerly accepted positive resolution still succeeds"), bOk);
    if (TestTrue(TEXT("the clamped pack returns a result"), Result.IsValid()))
    {
        TestEqual(TEXT("the response echoes the resolution the packer used"),
            Result->GetIntegerField(TEXT("textureResolution")),
            GeometryOps::LayoutUVTextureResolutionMin);
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        const FString ExpectedWarning = FString::Printf(
            TEXT("textureResolution clamped from 1 to %d (valid range %d-%d)"),
            GeometryOps::LayoutUVTextureResolutionMin,
            GeometryOps::LayoutUVTextureResolutionMin,
            GeometryOps::LayoutUVTextureResolutionMax);
        TestTrue(TEXT("the response reports the exact shared compatibility clamp"),
            Result->TryGetArrayField(TEXT("warnings"), Warnings)
            && Warnings != nullptr && Warnings->Num() == 1
            && (*Warnings)[0]->AsString() == ExpectedWarning);
    }
    return true;
}

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryPackUVIslandsResolutionReachesEngineTest,
    "PinWright.geometry.pack_uv_islands.TextureResolutionReachesThePacker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryPackUVIslandsResolutionReachesEngineTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "pack_uv_islands resolution assertions did not run"));
        return true;
    }

    // The resolution sizes the gutter left between islands. Packing the same fixture at a very
    // low and a very high resolution therefore has to land the islands differently; if the value
    // reaches no engine call - the state this ticket is about, where it was echoed and dropped -
    // the two packings come out identical.
    auto PackAt = [this](int32 Resolution, double& OutTotalArea) -> bool
    {
        const FString Label = FString::Printf(TEXT("PW_PackResProbe%d_%s"), Resolution,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        if (!SeedPlanarProjectedBox(*this, Dispatcher, Sink, Label))
        {
            DestroyActorsWithLabel(Label);
            return false;
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), Label);
        Params->SetNumberField(TEXT("textureResolution"), Resolution);
        bool bOk = false;
        FString Err;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"), TEXT("req-pack-res"),
            Params, bOk, Result, Err);

        const PackUVIslandsTest::FUVStats Stats = ReadUVStats(Label);
        DestroyActorsWithLabel(Label);

        if (!TestTrue(FString::Printf(TEXT("pack at resolution %d succeeded"), Resolution), bOk) ||
            !TestTrue(TEXT("UVs readable after packing"), Stats.bRead))
        {
            return false;
        }
        OutTotalArea = Stats.TotalUVArea;
        return true;
    };

    double LowArea = 0.0, HighArea = 0.0;
    if (!PackAt(64, LowArea) || !PackAt(4096, HighArea))
    {
        return false;
    }

    TestTrue(TEXT("both packings produced UV area"), LowArea > 0.0 && HighArea > 0.0);

    // A 64px gutter eats a far larger fraction of the unit square than a 4096px one, so the
    // islands are packed smaller. Asserted as a strict difference rather than a ratio: the
    // packer's exact allocation is its own business, and equality is the one outcome that proves
    // the parameter reached nothing.
    TestTrue(FString::Printf(
        TEXT("textureResolution must change the packing (64 -> area %.6f, 4096 -> area %.6f); "
             "identical results mean the value reached no engine call"), LowArea, HighArea),
        !FMath::IsNearlyEqual(LowArea, HighArea, 1e-6));
    return true;
}

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryPackUVIslandsEmptyChannelRefusedTest,
    "PinWright.geometry.pack_uv_islands.EmptyChannelIsRefusedNotPackedAsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryPackUVIslandsEmptyChannelRefusedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world available; "
                        "pack_uv_islands empty-channel assertions did not run"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_PackEmptyProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("name"), Label);
    bool bCreated = false;
    FString CreateErr;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"), TEXT("req-pack-empty-create"),
        CreateParams, bCreated, CreateErr);
    if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
    {
        DestroyActorsWithLabel(Label);
        return false;
    }

    // Channel 7 exists nowhere on a fresh box. The old shared path would have CREATED it and
    // reported a successful pack of zero islands; a pack has nothing to do here and must say so.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetNumberField(TEXT("uvChannel"), 7);
    bool bOk = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.pack_uv_islands"), TEXT("req-pack-empty"),
        Params, bOk, Result, ErrorCode);
    DestroyActorsWithLabel(Label);

    TestFalse(TEXT("packing a channel with no islands must not report success"), bOk);
    TestEqual(TEXT("the refusal is an argument error naming the empty channel"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
