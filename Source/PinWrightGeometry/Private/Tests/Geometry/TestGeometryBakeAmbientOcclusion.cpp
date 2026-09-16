// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for geometry.bake_ambient_occlusion (F-no-ambient-occlusion-or-curvature-bake).
//
// The failure mode this file exists to catch is not "the verb errors" - it is "the verb succeeds
// and writes a flat map". Three successive hand-written occlusion producers shipped exactly that:
// each passed its author's own per-junction spot checks and each covered the mesh in white. So
// the first test does not assert that a bake happened; it measures the SHAPE of what was baked
// against a fixture whose answer is known by construction.
//
// Fixture: two interpenetrating boxes in one mesh, built through the production primitive op.
// A 200 x 200 x 20 slab centred on the origin (top face at z = +10) and a 20 x 200 x 200 wall
// through its middle, rising 90 units above that top face and occupying |x| <= 10. Every vertex
// on the slab's top face therefore has a known exposure: one just beside the wall sees a 90-unit
// cliff over most of its hemisphere, and one out at |x| > 80 sees nothing at all within the
// occlusion radius. That difference is what a real bake measures and what a flat one cannot.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU when Unity merges them.

// The half-extents the probe bands below are expressed in. Named rather than repeated so the
// fixture and the bands cannot drift apart.
constexpr double BakeAOTest_SlabHalfX = 100.0;
constexpr double BakeAOTest_SlabTopZ = 10.0;
constexpr double BakeAOTest_WallHalfX = 10.0;

// One mesh holding both boxes. Built with GeometryOps::GenerateBox rather than by hand so the
// fixture exercises the same primitive path a caller would use, and so the tessellation follows
// the generator instead of being asserted here.
//
// The normal overlay is EMPTIED on the way out, deliberately. AppendBox leaves one covering every
// triangle, and with that in place the bake's normal-repair step never runs - so the fixture would
// silently stop testing it. An overlay that exists but covers nothing is also the exact shape the
// repair exists for: FMeshBakerDynamicMeshSampler::TriBaryInterpolateNormal leaves its
// out-parameter untouched on a miss and FMeshOcclusionMapEvaluator::SampleFunction neither
// initializes that parameter nor checks the bool, so every reading below would come from
// uninitialized stack memory without the repair.
UDynamicMesh* BakeAOTest_NewInterpenetratingBoxes()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());

    GeometryOps::FBoxParams Slab;
    Slab.Size = FVector(2.0 * BakeAOTest_SlabHalfX, 200.0, 2.0 * BakeAOTest_SlabTopZ);
    Slab.Steps = FIntVector(24, 24, 2);
    GeometryOps::GenerateBox(Mesh, Slab, FTransform::Identity);

    GeometryOps::FBoxParams Wall;
    Wall.Size = FVector(2.0 * BakeAOTest_WallHalfX, 200.0, 200.0);
    Wall.Steps = FIntVector(2, 24, 24);
    GeometryOps::GenerateBox(Mesh, Wall, FTransform::Identity);

    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
    if (EditMesh.HasAttributes() && EditMesh.Attributes()->PrimaryNormals())
    {
        EditMesh.Attributes()->PrimaryNormals()->ClearElements();
    }

    return Mesh;
}

// Mean of the baked value over the colour elements whose parent vertex sits on the slab's top
// face with |x| inside [MinAbsX, MaxAbsX]. OutCount reports how many contributed, so a band that
// matched nothing is a fixture failure rather than a silent zero.
double BakeAOTest_MeanOverTopFaceBand(
    const UE::Geometry::FDynamicMesh3& Mesh, double MinAbsX, double MaxAbsX, int32& OutCount)
{
    OutCount = 0;
    double Sum = 0.0;

    const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay =
        Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
    if (ColorOverlay == nullptr)
    {
        return 0.0;
    }

    for (int32 ElementID : ColorOverlay->ElementIndicesItr())
    {
        const int32 VertexID = ColorOverlay->GetParentVertex(ElementID);
        if (!Mesh.IsVertex(VertexID))
        {
            continue;
        }
        const FVector3d Position = Mesh.GetVertex(VertexID);

        // On the slab's top plane, and away from the slab's own y edges so an edge vertex's
        // averaged normal cannot be what the reading is measuring.
        if (FMath::Abs(Position.Z - BakeAOTest_SlabTopZ) > 0.5 || FMath::Abs(Position.Y) > 60.0)
        {
            continue;
        }
        const double AbsX = FMath::Abs(Position.X);
        if (AbsX < MinAbsX || AbsX > MaxAbsX)
        {
            continue;
        }

        Sum += ColorOverlay->GetElement(ElementID).X;
        ++OutCount;
    }

    return OutCount > 0 ? Sum / static_cast<double>(OutCount) : 0.0;
}
}

// ============================================================================
// The measurement: a junction darkens, an exposed face does not, and the two differ
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBakeAmbientOcclusionJunctionTest,
    "PinWright.geometry.bake_ambient_occlusion.JunctionIsDarkerThanTheExposedFace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBakeAmbientOcclusionJunctionTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(BakeAOTest_NewInterpenetratingBoxes());

    GeometryOps::FBakeAmbientOcclusionParams Params;
    // 60 units reaches the wall from the near band and cannot reach it from the far one, which
    // is the whole point of the radius: it is what makes this a CONTACT term rather than a
    // global bent-normal darkening.
    Params.OcclusionRadius = 60.0;
    Params.Samples = 64;
    Params.Channels = GeometryOps::EColorChannels::R | GeometryOps::EColorChannels::G |
                      GeometryOps::EColorChannels::B;

    GeometryOps::FBakeAmbientOcclusionOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::BakeAmbientOcclusion(Mesh.Get(), Params, Outputs);

    if (!TestTrue(TEXT("bake_ambient_occlusion succeeds on the two-box fixture"), Op.bSuccess))
    {
        AddError(FString::Printf(TEXT("bake failed with %s: %s"), *Op.ErrorCode, *Op.ErrorMessage));
        return true;
    }
    TestTrue(TEXT("the bake reports writing at least one colour element"), Outputs.ElementsWritten > 0);
    TestTrue(TEXT("the bake measured at least one corner"), Outputs.Occlusion.Count > 0);

    // The normal repair ran, on every triangle, and said so. Without it the readings below are
    // uninitialized stack memory that happens to look plausible - which is the failure mode this
    // whole file is built around, one level down.
    TestEqual(TEXT("every triangle of the stripped-normal fixture was given normals before baking"),
        Outputs.TrianglesGivenNormals, Mesh->GetTriangleCount());

    // 1. Not flat. A producer that ships white everywhere - the exact failure this verb exists to
    // make visible - has min == mean == max == 1 and fails both halves of this.
    TestTrue(FString::Printf(TEXT("some corner is well exposed (max %.3f > 0.95)"), Outputs.Occlusion.Max),
        Outputs.Occlusion.Max > 0.95);
    TestTrue(FString::Printf(TEXT("some corner is substantially occluded (min %.3f < 0.6)"), Outputs.Occlusion.Min),
        Outputs.Occlusion.Min < 0.6);

    // 2. The darkening is in the right PLACE. A bake can spread a range over a mesh and still put
    // it nowhere useful, so the band comparison is the assertion that actually pins the feature.
    int32 JunctionCount = 0;
    int32 ExposedCount = 0;
    const UE::Geometry::FDynamicMesh3& ReadMesh = Mesh->GetMeshRef();
    const double JunctionMean = BakeAOTest_MeanOverTopFaceBand(ReadMesh, 12.0, 32.0, JunctionCount);
    const double ExposedMean = BakeAOTest_MeanOverTopFaceBand(ReadMesh, 80.0, BakeAOTest_SlabHalfX, ExposedCount);

    if (!TestTrue(TEXT("the fixture put vertices beside the wall"), JunctionCount > 0) ||
        !TestTrue(TEXT("the fixture put vertices out on the open end of the slab"), ExposedCount > 0))
    {
        return true;
    }

    TestTrue(FString::Printf(TEXT("the exposed end of the slab stays open (mean %.3f > 0.9 over %d corners)"),
        ExposedMean, ExposedCount), ExposedMean > 0.9);
    TestTrue(FString::Printf(
        TEXT("the wall junction is darker than the exposed face (junction %.3f over %d corners vs exposed %.3f over %d)"),
        JunctionMean, JunctionCount, ExposedMean, ExposedCount),
        JunctionMean < ExposedMean - 0.15);

    return true;
}

// ============================================================================
// A colour element no triangle references is freed, not handed to the baker
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBakeAmbientOcclusionOrphanElementTest,
    "PinWright.geometry.bake_ambient_occlusion.OrphanColourElementsAreFreedNotBaked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBakeAmbientOcclusionOrphanElementTest::RunTest(const FString& Parameters)
{
    // An isolated vertex - live, but referenced by no triangle. The colour overlay's per-vertex
    // seed gives it an element anyway, and FMeshVertexBaker::SampleSurface reads that element's
    // triangle list at [0] with no empty check, so handing it to the baker terminates the editor.
    // Reachable in ordinary use after geometry.delete_triangle or a boolean.
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    GeometryOps::FBoxParams Box;
    Box.Size = FVector(100.0, 100.0, 100.0);
    GeometryOps::GenerateBox(Mesh.Get(), Box, FTransform::Identity);

    GeometryOps::FAppendVertexParams Loose;
    Loose.Position = FVector(0.0, 0.0, 500.0);
    int32 LooseIndex = INDEX_NONE;
    if (!TestTrue(TEXT("the isolated vertex appends"),
            GeometryOps::AppendVertex(Mesh.Get(), Loose, LooseIndex).bSuccess))
    {
        return true;
    }

    GeometryOps::FBakeAmbientOcclusionParams Params;
    Params.OcclusionRadius = 50.0;
    Params.Samples = 8;
    Params.Channels = GeometryOps::EColorChannels::A;

    GeometryOps::FBakeAmbientOcclusionOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::BakeAmbientOcclusion(Mesh.Get(), Params, Outputs);

    // The counterfactual is blunt: revert the FreeUnusedElements call and this line is never
    // reached, because the bake takes the editor down inside the engine baker.
    TestTrue(TEXT("a mesh carrying an isolated vertex bakes instead of crashing"), Op.bSuccess);
    TestTrue(FString::Printf(TEXT("the orphan colour element was freed and counted (freed %d)"),
        Outputs.OrphanElementsFreed), Outputs.OrphanElementsFreed >= 1);
    TestTrue(TEXT("the rest of the mesh still baked"), Outputs.ElementsWritten > 0);

    return true;
}

// ============================================================================
// The response contract, over the real dispatcher
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBakeAmbientOcclusionResponseTest,
    "PinWright.geometry.bake_ambient_occlusion.ResponseCarriesPerChannelStatistics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBakeAmbientOcclusionResponseTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping bake_ambient_occlusion response test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_BakeAOProbe_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-bakeao-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // The failure direction first: an unreadable channel spelling must be refused, not quietly
    // widened to all four - widening is precisely what would destroy the RGB the caller kept.
    {
        TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
        BadParams->SetStringField(TEXT("actorName"), Label);
        BadParams->SetNumberField(TEXT("occlusionRadius"), 50.0);
        BadParams->SetStringField(TEXT("channels"), TEXT("x"));
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> BadResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bake_ambient_occlusion"),
            TEXT("req-bakeao-badchannel"), BadParams, bSuccess, BadResult, ErrorCode);
        TestFalse(TEXT("an unreadable channel mask is refused"), bSuccess);
        TestEqual(TEXT("an unreadable channel mask reports INVALID_ARGUMENT"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // And a radius of zero, which would otherwise bake an unbounded global darkening under a
    // parameter name promising a local one.
    {
        TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
        BadParams->SetStringField(TEXT("actorName"), Label);
        BadParams->SetNumberField(TEXT("occlusionRadius"), 0.0);
        BadParams->SetStringField(TEXT("channels"), TEXT("rgb"));
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> BadResult;
        Dispatch(Dispatcher, Sink, TEXT("geometry.bake_ambient_occlusion"),
            TEXT("req-bakeao-zeroradius"), BadParams, bSuccess, BadResult, ErrorCode);
        TestFalse(TEXT("a non-positive occlusionRadius is refused"), bSuccess);
        TestEqual(TEXT("a non-positive occlusionRadius reports INVALID_ARGUMENT"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    TSharedPtr<FJsonObject> BakeParams = MakeShared<FJsonObject>();
    BakeParams->SetStringField(TEXT("actorName"), Label);
    BakeParams->SetNumberField(TEXT("occlusionRadius"), 50.0);
    BakeParams->SetStringField(TEXT("channels"), TEXT("rgb"));
    BakeParams->SetNumberField(TEXT("samples"), 16);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.bake_ambient_occlusion"),
        TEXT("req-bakeao-run"), BakeParams, bSuccess, Result, ErrorCode);

    if (!TestTrue(TEXT("geometry.bake_ambient_occlusion succeeded"), bSuccess) ||
        !TestTrue(TEXT("geometry.bake_ambient_occlusion carries a result object"), Result.IsValid()))
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        return true;
    }

    FString EchoedChannels;
    TestTrue(TEXT("the response echoes the channels it wrote"),
        Result->TryGetStringField(TEXT("channels"), EchoedChannels));
    TestEqual(TEXT("the echo is the canonical r,g,b,a-ordered mask"), EchoedChannels, FString(TEXT("rgb")));

    double VerticesModified = 0.0;
    TestTrue(TEXT("the response reports verticesModified"),
        Result->TryGetNumberField(TEXT("verticesModified"), VerticesModified));
    TestTrue(TEXT("the bake reached at least one vertex"), VerticesModified >= 1.0);

    // The flatness detector has to be present and populated, or the caller is back to opening
    // the mesh to find out whether the bake did anything.
    const TSharedPtr<FJsonObject>* Occlusion = nullptr;
    if (TestTrue(TEXT("the response carries an occlusion statistics block"),
            Result->TryGetObjectField(TEXT("occlusion"), Occlusion)) && Occlusion != nullptr)
    {
        double Min = 0.0, Mean = 0.0, Max = 0.0, Count = 0.0;
        TestTrue(TEXT("occlusion.min is present"), (*Occlusion)->TryGetNumberField(TEXT("min"), Min));
        TestTrue(TEXT("occlusion.mean is present"), (*Occlusion)->TryGetNumberField(TEXT("mean"), Mean));
        TestTrue(TEXT("occlusion.max is present"), (*Occlusion)->TryGetNumberField(TEXT("max"), Max));
        TestTrue(TEXT("occlusion.count is present"), (*Occlusion)->TryGetNumberField(TEXT("count"), Count));
        TestTrue(TEXT("occlusion.count says corners were measured"), Count >= 1.0);
        TestTrue(TEXT("occlusion.min <= occlusion.mean <= occlusion.max"), Min <= Mean && Mean <= Max);
    }

    const TSharedPtr<FJsonObject>* Written = nullptr;
    if (TestTrue(TEXT("the response carries a per-channel written block"),
            Result->TryGetObjectField(TEXT("written"), Written)) && Written != nullptr)
    {
        TestTrue(TEXT("the written block covers the r channel the mask named"),
            (*Written)->HasField(TEXT("r")));
        TestTrue(TEXT("the written block covers the b channel the mask named"),
            (*Written)->HasField(TEXT("b")));
        TestFalse(TEXT("the written block omits the alpha channel the mask did not name"),
            (*Written)->HasField(TEXT("a")));
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Label);
    return true;
}
