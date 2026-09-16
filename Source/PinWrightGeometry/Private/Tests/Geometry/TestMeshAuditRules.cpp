// Copyright (c) 2026 Alexander Penkin. MIT License.

// Rule net for MeshAudit - the static-mesh health sweep behind geometry.audit_static_meshes.
//
// EVERY assertion here runs over SYNTHETIC meshes built in this file. Nothing reads host
// content, so the rules stay covered on a fresh clone and on a machine whose art has been
// fixed. The live acceptance run against the project's tree meshes proves the sweep sees a
// real defect; these prove it cannot stop seeing one.
//
// The seam that makes that possible is MeshAudit::EvaluateAsset, which is pure - it takes a
// FAssetMeasurement and produces findings, with no package load, no world and no UObject
// access. So a test can hand it a measurement of a mesh it built, or a measurement that says
// "this asset could not be read at all", and assert the verdict without an asset existing.
//
// WHY THESE TESTS MEASURE VOLUME AND NOT NORMALS, restated because it is the reason the defect
// shipped: recomputing normals from an inverted winding cements the inversion into the
// attribute set and leaves the mesh perfectly self-consistent and still inside out. Signed
// volume reads the winding itself. The sign convention is the engine's - facing normal
// (V2-V0) x (V1-V0), the NEGATION of the right-hand rule, because Unreal is left-handed - and
// it is not re-derived here: these tests build a mesh, reverse it, and assert the two disagree.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/MeshAuditUtils.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "Model/PwModelCompiler.h"

#include "Tests/TestUtils.h" // InvokeHandlerWithCapture, for the handler-level argument tests

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "GPUSkinPublicDefs.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshTypes.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* MeshAuditTest_NewBox()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    GeometryOps::FBoxParams Params;
    Params.Size = FVector(100.0, 100.0, 100.0);
    GeometryOps::GenerateBox(Mesh, Params, FTransform::Identity);
    return Mesh;
}

UDynamicMesh* MeshAuditTest_NewBoxAssembly(const TArray<FVector>& Centers)
{
    UDynamicMesh* Mesh = nullptr;
    for (const FVector& Center : Centers)
    {
        UDynamicMesh* Box = MeshAuditTest_NewBox();
        if (!Mesh)
        {
            Mesh = Box;
            if (!Center.IsNearlyZero())
            {
                Box->EditMesh([&Center](UE::Geometry::FDynamicMesh3& Edit)
                {
                    for (const int32 VertexID : Edit.VertexIndicesItr())
                    {
                        Edit.SetVertex(VertexID, Edit.GetVertex(VertexID) + FVector3d(Center));
                    }
                });
            }
        }
        else
        {
            UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
                Mesh, Box, FTransform(FQuat::Identity, Center, FVector::OneVector));
        }
    }
    return Mesh ? Mesh : NewObject<UDynamicMesh>(GetTransientPackage());
}

UDynamicMesh* MeshAuditTest_NewHalfInvertedBoxes()
{
    UDynamicMesh* Mesh = MeshAuditTest_NewBox();
    UDynamicMesh* InvertedBox = MeshAuditTest_NewBox();
    InvertedBox->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
    {
        Edit.ReverseOrientation(true);
    });

    // Keep the shells separate: this is a general multi-component fixture, not a named asset
    // or a host-specific reproduction. AppendMesh does not weld the two boxes, so the component
    // walk must see one correctly wound shell and one inverted shell.
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
        Mesh, InvertedBox, FTransform(FVector(90.0, 0.0, 0.0)));
    return Mesh;
}

UDynamicMesh* MeshAuditTest_NewDegenerateBox()
{
    UDynamicMesh* Mesh = MeshAuditTest_NewBox();
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
    {
        for (const int32 TriangleID : Edit.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Triangle = Edit.GetTriangle(TriangleID);
            Edit.SetVertex(Triangle.B, Edit.GetVertex(Triangle.A));
            break;
        }
    });
    return Mesh;
}

void MeshAuditTest_FillAnimationBoxSection(FSkelMeshSection& Section, int32 BoneIndex,
    const FVector3f& Min, const FVector3f& Max,
                                           TArray<uint32>& IndexBuffer)
{
    Section.BoneMap.Add(static_cast<FBoneIndexType>(BoneIndex));
    Section.BaseIndex = IndexBuffer.Num();
    Section.NumTriangles = 12;
    Section.SoftVertices.SetNum(8);
    Section.NumVertices = Section.SoftVertices.Num();

    const FVector3f Positions[8] = {
        FVector3f(Min.X, Min.Y, Min.Z), FVector3f(Max.X, Min.Y, Min.Z),
        FVector3f(Max.X, Max.Y, Min.Z), FVector3f(Min.X, Max.Y, Min.Z),
        FVector3f(Min.X, Min.Y, Max.Z), FVector3f(Max.X, Min.Y, Max.Z),
        FVector3f(Max.X, Max.Y, Max.Z), FVector3f(Min.X, Max.Y, Max.Z),
    };
    const uint32 Triangles[36] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
    };
    for (int32 VertexIndex = 0; VertexIndex < 8; ++VertexIndex)
    {
        FSoftSkinVertex& Vertex = Section.SoftVertices[VertexIndex];
        FMemory::Memzero(&Vertex, sizeof(FSoftSkinVertex));
        Vertex.Position = Positions[VertexIndex];
        Vertex.InfluenceBones[0] = 0;
        Vertex.InfluenceWeights[0] = 65535;
    }
    for (const uint32 Index : Triangles)
    {
        IndexBuffer.Add(Index + Section.BaseVertexIndex);
    }
}

struct FMeshAuditAnimationFixture
{
    USkeletalMesh* Mesh = nullptr;
    UAnimSequence* Sequence = nullptr;
};

FMeshAuditAnimationFixture MeshAuditTest_NewTwoBoneAnimationFixture(
    bool bBindFloating = false, bool bUseRetargetSource = false,
    bool bAddOverlappingDetachedPart = false, bool bChildBoneRollOnly = false)
{
    FMeshAuditAnimationFixture Fixture;
    USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage());
    Fixture.Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
    UPackage* FixturePackage = CreatePackage(*FString::Printf(
        TEXT("/Engine/Transient/PinWrightMeshAudit_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    Fixture.Sequence = FixturePackage
        ? NewObject<UAnimSequence>(FixturePackage, TEXT("Sequence"), RF_Transient)
        : nullptr;
    if (!Skeleton || !Fixture.Mesh || !Fixture.Sequence)
    {
        return Fixture;
    }
    const FVector PartReferenceTranslation = bChildBoneRollOnly
        ? FVector(100.0, 50.0, 0.0)
        : FVector(100.0, 0.0, 0.0);

    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                     FTransform::Identity, true);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("part")), TEXT("part"), 0),
                     FTransform(PartReferenceTranslation));
    }
    Fixture.Mesh->SetSkeleton(Skeleton);
    {
        FReferenceSkeletonModifier Modifier(Fixture.Mesh->GetRefSkeleton(), Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                     FTransform::Identity, true);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("part")), TEXT("part"), 0),
                     FTransform(PartReferenceTranslation));
    }

    // The engine's raw data-model evaluator uses the skeleton bone tree to resolve track indices;
    // a transient skeleton does not populate it just by editing its reference skeleton.
    // The bShowProgress parameter arrived in UE 5.4. 5.3's one-argument overload shows no
    // progress at all, so dropping the argument there is exactly the false being asked for.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    Skeleton->MergeAllBonesToBoneTree(Fixture.Mesh, false);
#else
    Skeleton->MergeAllBonesToBoneTree(Fixture.Mesh);
#endif

    const FName RetargetSourceName(TEXT("SyntheticRetargetSource"));
    if (bUseRetargetSource)
    {
        FReferencePose RetargetPose;
        RetargetPose.PoseName = RetargetSourceName;
        RetargetPose.ReferencePose = Skeleton->GetReferenceSkeleton().GetRefBonePose();
        if (RetargetPose.ReferencePose.IsValidIndex(1))
        {
            RetargetPose.ReferencePose[1].SetTranslation(FVector(200.0, 0.0, 0.0));
        }
        Skeleton->AnimRetargetSources.Add(RetargetSourceName, MoveTemp(RetargetPose));
        Skeleton->SetBoneTranslationRetargetingMode(
            1, EBoneTranslationRetargetingMode::AnimationRelative);
    }

    Fixture.Mesh->AddLODInfo();
    FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();
    FSkelMeshSection& RootSection = LODModel->Sections.AddDefaulted_GetRef();
    TArray<uint32>& IndexBuffer = LODModel->IndexBuffer;
    const FVector3f RootMin = bChildBoneRollOnly
        ? FVector3f(-50.0f, -25.0f, -5.0f)
        : FVector3f(-50.0f, -25.0f, -25.0f);
    const FVector3f RootMax = bChildBoneRollOnly
        ? FVector3f(50.0f, 25.0f, 5.0f)
        : FVector3f(50.0f, 25.0f, 25.0f);
    MeshAuditTest_FillAnimationBoxSection(
        RootSection, 0, RootMin, RootMax, IndexBuffer);
    FSkelMeshSection& PartSection = LODModel->Sections.AddDefaulted_GetRef();
    PartSection.BaseVertexIndex = RootSection.SoftVertices.Num();
    const float PartMinX = bBindFloating ? 300.0f : 0.0f;
    const FVector3f PartMin = bChildBoneRollOnly
        ? FVector3f(0.0f, 0.0f, 0.0f)
        : FVector3f(PartMinX, -25.0f, -25.0f);
    const FVector3f PartMax = bChildBoneRollOnly
        ? FVector3f(100.0f, 2.0f, 2.0f)
        : FVector3f(PartMinX + 100.0f, 25.0f, 25.0f);
    MeshAuditTest_FillAnimationBoxSection(
        PartSection, 1, PartMin, PartMax, IndexBuffer);
    if (bAddOverlappingDetachedPart)
    {
        FSkelMeshSection& Part2Section = LODModel->Sections.AddDefaulted_GetRef();
        Part2Section.BaseVertexIndex =
            RootSection.SoftVertices.Num() + PartSection.SoftVertices.Num();
        MeshAuditTest_FillAnimationBoxSection(
            Part2Section, 1, FVector3f(PartMinX, -25.0f, -25.0f),
            FVector3f(PartMinX + 100.0f, 25.0f, 25.0f),
            IndexBuffer);
    }
    // FillAnimationBoxSection writes the second section's local indices with its base vertex
    // offset, while its BaseIndex points at the second section's slice of the shared buffer.
    LODModel->NumVertices = RootSection.SoftVertices.Num() + PartSection.SoftVertices.Num()
        + (bAddOverlappingDetachedPart ? 8 : 0);
    Fixture.Mesh->GetImportedModel()->LODModels.Add(LODModel);

    Fixture.Sequence->SetSkeleton(Skeleton);
    if (bUseRetargetSource)
    {
        Fixture.Sequence->RetargetSource = RetargetSourceName;
    }
    IAnimationDataController& Controller = Fixture.Sequence->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("floating geometry fixture")), false);
    Controller.InitializeModel();
    Controller.UpdateWithSkeleton(Skeleton, false);
    Controller.SetFrameRate(FFrameRate(30, 1), false);
    Controller.SetNumberOfFrames(FFrameNumber(bChildBoneRollOnly ? 1 : 2), false);
    const FName PartBone(TEXT("part"));
    Controller.AddBoneCurve(PartBone, false);
    TArray<FVector> Positions;
    TArray<FQuat> Rotations;
    TArray<FVector> Scales;
    if (bChildBoneRollOnly)
    {
        // The child stays at its off-axis reference translation in both keys. Only a roll around
        // its longitudinal +X axis changes. The offset pivot makes reference/pose order visible:
        // correct skinning swings the thin section clear, while the old order leaves it attached.
        Positions.Add(PartReferenceTranslation);
        Positions.Add(PartReferenceTranslation);
        Rotations.Add(FQuat::Identity);
        Rotations.Add(FQuat(FVector::ForwardVector, FMath::DegreesToRadians(90.0f)));
        Scales.Init(FVector::OneVector, 2);
    }
    else
    {
        const double MiddlePartX = bUseRetargetSource ? 200.0 : 1000.0;
        Positions.Add(FVector(100.0, 0.0, 0.0));
        Positions.Add(FVector(MiddlePartX, 0.0, 0.0));
        Positions.Add(FVector(100.0, 0.0, 0.0));
        Rotations.Init(FQuat::Identity, 3);
        Scales.Init(FVector::OneVector, 3);
    }
    Controller.SetBoneTrackKeys(PartBone, Positions, Rotations, Scales, false);
    Controller.NotifyPopulated();
    Controller.CloseBracket(false);
    return Fixture;
}

bool MeshAuditTest_SetBoneTrackState(
    UAnimSequence* Sequence, const FName& TrackName, int32 BoneTreeIndex, FName ReplacementName)
{
    const IAnimationDataModel* DataModel = Sequence ? Sequence->GetDataModel() : nullptr;
    if (!DataModel)
    {
        return false;
    }

    TArray<FName> TrackNames;
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    DataModel->GetBoneTrackNames(TrackNames);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
    FName ExistingTrackName = TrackName;
    if (!TrackNames.Contains(ExistingTrackName) && TrackNames.Num() == 1)
    {
        // This transient fixture can expose its single controller-created track with a placeholder
        // name until the model validation pass; it is still the track under test.
        ExistingTrackName = TrackNames[0];
    }
    if (!TrackNames.Contains(ExistingTrackName))
    {
        return false;
    }
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    FBoneAnimationTrack& Track =
        const_cast<FBoneAnimationTrack&>(DataModel->GetBoneTrackByName(ExistingTrackName));
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
    if (Track.Name == NAME_None)
    {
        Track.Name = TrackName;
    }
    Track.BoneTreeIndex = BoneTreeIndex;
    if (ReplacementName != NAME_None)
    {
        Track.Name = ReplacementName;
    }
    return true;
}

// Measure a dynamic mesh into the shape EvaluateAsset consumes, exactly as MeasureStaticMesh
// does after its copy - two walks, because only the orientation form carries SurfaceArea.
MeshAudit::FAssetMeasurement MeshAuditTest_Measure(UDynamicMesh* Mesh)
{
    MeshAudit::FAssetMeasurement M;
    M.bMeasured = true;
    M.bHasSourceModel = true;
    M.SourceLodCount = 1;
    M.Health = GeometryUtils::MeasureMeshHealth(Mesh);
    M.SurfaceArea = GeometryUtils::MeasureMeshOrientation(Mesh).SurfaceArea;
    MeshAudit::MeasureComponents(Mesh, M.Components, M.ZFightTriangles);
    MeshAudit::MeasureSpatialProximity(Mesh, M.Spatial, M.Components);
    M.Health.ComponentCount = M.Components.Num();
    return M;
}

MeshAudit::FConfig MeshAuditTest_DefaultConfig()
{
    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::DefaultCheckMask();
    return Config;
}

// Flagged count for one check in a finished report.
int32 MeshAuditTest_Flagged(const MeshAudit::FReport& R, MeshAudit::ECheck Check)
{
    return R.Tallies[static_cast<int32>(Check)].Flagged;
}

int32 MeshAuditTest_DiagnosticCount(const FPwModelCompileResult& Result, const TCHAR* Code)
{
    int32 Count = 0;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        Count += Diagnostic.Code == Code ? 1 : 0;
    }
    return Count;
}

int32 MeshAuditTest_NotApplicable(const MeshAudit::FReport& R, MeshAudit::ECheck Check)
{
    return R.Tallies[static_cast<int32>(Check)].NotApplicable;
}
}

// ---------------------------------------------------------------------------------------------
// The control: a correct closed box must produce nothing at all. Without this, every test below
// could pass on a rule that flags everything.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditHealthyBoxTest,
    "PinWright.Geometry.MeshAudit.HealthyClosedBoxProducesNoFinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditHealthyBoxTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = MeshAuditTest_NewBox();
    const MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(Box);

    TestTrue(TEXT("the probe box is closed"), M.Health.IsClosed());
    TestTrue(TEXT("the probe box encloses positive volume"), M.Health.SignedVolume > 0.0);

    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Box"), TEXT("Box"), M, MeshAuditTest_DefaultConfig(), R);

    TestEqual(TEXT("a correct closed box produces no findings"), R.Findings.Num(), 0);
    TestEqual(TEXT("and no errors"), R.ErrorCount, 0);
    TestEqual(TEXT("and no warnings"), R.WarningCount, 0);
    TestEqual(TEXT("and nothing unrunnable"), R.UnrunnableCount, 0);
    TestTrue(TEXT("so the sweep passes"), R.DerivePass(TEXT("error")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The defect that shipped. A uniformly reversed box is closed, consistent, non-degenerate and
// bowtie-free - identical to the control on every other measurement - and only the volume sign
// separates them. The test also asserts that inconsistent_winding stays CLEAN here, because the
// two checks are independent and a rule that conflated them would hide the uniform case.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditInvertedBoxTest,
    "PinWright.Geometry.MeshAudit.UniformlyInvertedBoxIsFlaggedInverted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditInvertedBoxTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = MeshAuditTest_NewBox();
    const MeshAudit::FAssetMeasurement Correct = MeshAuditTest_Measure(Box);

    Box->EditMesh([](UE::Geometry::FDynamicMesh3& Edit) { Edit.ReverseOrientation(true); });
    const MeshAudit::FAssetMeasurement Inverted = MeshAuditTest_Measure(Box);

    // The inversion is invisible to everything except the sign.
    TestEqual(TEXT("reversing changes no triangle count"),
        Inverted.Health.TriangleCount, Correct.Health.TriangleCount);
    TestEqual(TEXT("reversing leaves it closed"), Inverted.Health.BoundaryEdges, 0);
    TestEqual(TEXT("reversing leaves winding self-consistent"),
        Inverted.Health.InconsistentEdges, 0);
    TestTrue(TEXT("the two disagree on the sign of the enclosed volume"),
        (Correct.Health.SignedVolume > 0.0) != (Inverted.Health.SignedVolume > 0.0));

    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Inv"), TEXT("Inv"), Inverted,
                             MeshAuditTest_DefaultConfig(), R);

    TestEqual(TEXT("inverted is flagged"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::Inverted), 1);
    TestEqual(TEXT("inconsistent_winding is NOT - a uniform inversion is perfectly consistent"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::InconsistentWinding), 0);
    TestEqual(TEXT("not_closed is not flagged either"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::NotClosed), 0);
    TestFalse(TEXT("so the sweep goes RED on a mesh every other measurement calls healthy"),
        R.DerivePass(TEXT("error")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The shipped regression: two equal, disconnected shells cancel in the asset-level sum. The
// component evidence must still flag exactly the reversed shell and expose both signs to the
// caller. This is deliberately selected with `inverted` alone so the assertions isolate the
// check whose verdict used to be CLEAN.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditHalfInvertedComponentsTest,
    "PinWright.Geometry.MeshAudit.BalancedInvertedComponentIsNotCancelled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditHalfInvertedComponentsTest::RunTest(const FString& Parameters)
{
    const MeshAudit::FAssetMeasurement M =
        MeshAuditTest_Measure(MeshAuditTest_NewHalfInvertedBoxes());

    TestEqual(TEXT("the fixture contains two edge-connected components"),
        M.Components.Num(), 2);
    TestEqual(TEXT("the health count uses the same component walk"),
        M.Health.ComponentCount, 2);
    TestTrue(TEXT("the whole-mesh signed volumes cancel"),
        FMath::IsNearlyZero(M.Health.SignedVolume, 1.0e-3));

    int32 PositiveComponents = 0;
    int32 NegativeComponents = 0;
    for (const MeshAudit::FComponentMeasurement& Component : M.Components)
    {
        PositiveComponents += Component.SignedVolume > 0.0 ? 1 : 0;
        NegativeComponents += Component.SignedVolume < 0.0 ? 1 : 0;
    }
    TestEqual(TEXT("one shell remains correctly wound"), PositiveComponents, 1);
    TestEqual(TEXT("one shell is reversed"), NegativeComponents, 1);

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::Inverted);
    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Engine/BasicShapes/TwoBoxes"), TEXT("TwoBoxes"), M, Config, R);

    const MeshAudit::FCheckTally& Tally =
        R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)];
    TestEqual(TEXT("the selected component check is applicable once"), Tally.Applicable, 1);
    TestEqual(TEXT("the cancellation fixture is flagged"), Tally.Flagged, 1);
    TestEqual(TEXT("no component is unknown"), Tally.Unrunnable, 0);
    TestEqual(TEXT("the flagged asset is not also counted clean"), Tally.Clean, 0);
    TestEqual(TEXT("the check is not not-applicable"), Tally.NotApplicable, 0);
    TestFalse(TEXT("the old whole-mesh cancellation cannot produce pass"),
        R.DerivePass(TEXT("error")));

    TestEqual(TEXT("one finding carries the component evidence"), R.Findings.Num(), 1);
    if (R.Findings.Num() == 1 && R.Findings[0].Measurements.IsValid())
    {
        const TSharedPtr<FJsonObject>& Measurements = R.Findings[0].Measurements;
        double Answered = -1.0;
        double Unknown = -1.0;
        double Inverted = -1.0;
        double Clean = -1.0;
        TestTrue(TEXT("measurements report answered components"),
            Measurements->TryGetNumberField(TEXT("answeredComponents"), Answered));
        TestTrue(TEXT("measurements report unknown components"),
            Measurements->TryGetNumberField(TEXT("unknownComponents"), Unknown));
        TestTrue(TEXT("measurements report inverted components"),
            Measurements->TryGetNumberField(TEXT("invertedComponents"), Inverted));
        TestTrue(TEXT("measurements report clean components"),
            Measurements->TryGetNumberField(TEXT("cleanComponents"), Clean));
        TestEqual(TEXT("both components were answered"), Answered, 2.0);
        TestEqual(TEXT("neither component was unknown"), Unknown, 0.0);
        TestEqual(TEXT("one component was inverted"), Inverted, 1.0);
        TestEqual(TEXT("one component was clean"), Clean, 1.0);

        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        TestTrue(TEXT("measurements include the component rows"),
            Measurements->TryGetArrayField(TEXT("components"), Components));
        TestTrue(TEXT("component rows are present"), Components && Components->Num() == 2);
        int32 InvertedRows = 0;
        int32 CleanRows = 0;
        if (Components)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Components)
            {
                const TSharedPtr<FJsonObject>* Row = nullptr;
                if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row || !Row->IsValid())
                {
                    continue;
                }
                FString Status;
                double SignedVolume = 0.0;
                TestTrue(TEXT("each component row has a status"),
                    (*Row)->TryGetStringField(TEXT("status"), Status));
                TestTrue(TEXT("each component row has a signed volume"),
                    (*Row)->TryGetNumberField(TEXT("signedVolume"), SignedVolume));
                if (Status == TEXT("inverted"))
                {
                    ++InvertedRows;
                    TestTrue(TEXT("an inverted row carries a negative volume"),
                        SignedVolume < 0.0);
                }
                else if (Status == TEXT("clean"))
                {
                    ++CleanRows;
                    TestTrue(TEXT("a clean row carries a positive volume"),
                        SignedVolume > 0.0);
                }
            }
        }
        TestEqual(TEXT("one row is explicitly inverted"), InvertedRows, 1);
        TestEqual(TEXT("one row is explicitly clean"), CleanRows, 1);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// A zero-volume component is not evidence of a correctly wound solid. This keeps the tolerance
// path honest even when another component in the same asset remains answerable.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditZeroVolumeComponentTest,
    "PinWright.Geometry.MeshAudit.ZeroVolumeComponentIsUnrunnableNotClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditZeroVolumeComponentTest::RunTest(const FString& Parameters)
{
    MeshAudit::FAssetMeasurement M =
        MeshAuditTest_Measure(MeshAuditTest_NewHalfInvertedBoxes());
    TestEqual(TEXT("the fixture has two components before making one degenerate"),
        M.Components.Num(), 2);
    if (M.Components.Num() != 2)
    {
        return true;
    }
    M.Components[0].SignedVolume = 0.0;

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::Inverted);
    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Engine/BasicShapes/ZeroComponent"), TEXT("ZeroComponent"), M, Config, R);

    const MeshAudit::FCheckTally& Tally =
        R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)];
    TestEqual(TEXT("the zero-volume component remains applicable to the check"),
        Tally.Applicable, 1);
    TestEqual(TEXT("the zero-volume component is unrunnable"), Tally.Unrunnable, 1);
    TestEqual(TEXT("the zero-volume component is not clean"), Tally.Clean, 0);
    TestEqual(TEXT("the zero-volume component is not not-applicable"), Tally.NotApplicable, 0);
    TestFalse(TEXT("UNKNOWN cannot pass even when failOn is none"),
        R.DerivePass(TEXT("none")));
    TestEqual(TEXT("the finding uses the component-unknown code"), R.Findings.Num(), 1);
    if (R.Findings.Num() == 1)
    {
        TestEqual(TEXT("the finding is unrunnable"),
            R.Findings[0].Status, MeshAudit::EFindingStatus::Unrunnable);
        TestEqual(TEXT("the reason is component uncertainty"), R.Findings[0].Code,
            FString(ErrorCodes::ERR_MESH_AUDIT_COMPONENT_UNKNOWN));
        double Answered = -1.0;
        double Unknown = -1.0;
        TestTrue(TEXT("the finding reports the answered count"),
            R.Findings[0].Measurements->TryGetNumberField(TEXT("answeredComponents"), Answered));
        TestTrue(TEXT("the finding reports the unknown count"),
            R.Findings[0].Measurements->TryGetNumberField(TEXT("unknownComponents"), Unknown));
        TestEqual(TEXT("one component remains answerable"), Answered, 1.0);
        TestEqual(TEXT("one component is unknown"), Unknown, 1.0);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// A component can retain a useful signed volume while containing a degenerate triangle. The
// separate degenerate check will also flag the asset, but `inverted` alone must not call that
// component a clean solid.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditDegenerateComponentTest,
    "PinWright.Geometry.MeshAudit.DegenerateComponentIsUnrunnableNotClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditDegenerateComponentTest::RunTest(const FString& Parameters)
{
    const MeshAudit::FAssetMeasurement M =
        MeshAuditTest_Measure(MeshAuditTest_NewDegenerateBox());
    TestTrue(TEXT("the synthetic box has a degenerate triangle"),
        M.Health.DegenerateTriangles > 0);
    TestEqual(TEXT("the degenerate box remains one component"), M.Components.Num(), 1);
    if (M.Components.Num() != 1)
    {
        return true;
    }
    TestTrue(TEXT("the component measurement attributes the degenerate triangle"),
        M.Components[0].DegenerateTriangles > 0);

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::Inverted);
    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Engine/BasicShapes/Degenerate"), TEXT("Degenerate"), M, Config, R);

    const MeshAudit::FCheckTally& Tally =
        R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)];
    TestEqual(TEXT("the degenerate component is applicable"), Tally.Applicable, 1);
    TestEqual(TEXT("the degenerate component is unrunnable"), Tally.Unrunnable, 1);
    TestEqual(TEXT("the degenerate component is not clean"), Tally.Clean, 0);
    TestFalse(TEXT("a degenerate component cannot pass the inverted-only audit"),
        R.DerivePass(TEXT("none")));
    TestEqual(TEXT("the component evidence is returned in one finding"), R.Findings.Num(), 1);
    if (R.Findings.Num() == 1 && R.Findings[0].Measurements.IsValid())
    {
        double Unknown = -1.0;
        TestTrue(TEXT("the finding counts the component as unknown"),
            R.Findings[0].Measurements->TryGetNumberField(TEXT("unknownComponents"), Unknown));
        TestEqual(TEXT("the degenerate component is the unknown component"), Unknown, 1.0);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The other half of the pair. Reversing ONE triangle leaves the volume overwhelmingly positive,
// so `inverted` says nothing; only the edge-adjacency signal sees it. This is the test that
// stops the two checks from being collapsed into one.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditPartialInversionTest,
    "PinWright.Geometry.MeshAudit.PartiallyInvertedBoxIsFlaggedInconsistent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditPartialInversionTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = MeshAuditTest_NewBox();
    Box->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
    {
        for (const int32 TriangleId : Edit.TriangleIndicesItr())
        {
            Edit.ReverseTriOrientation(TriangleId);
            break;
        }
    });

    const MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(Box);
    TestTrue(TEXT("one reversed triangle leaves the volume positive"), M.Health.SignedVolume > 0.0);
    TestTrue(TEXT("but it disagrees with its neighbours"), M.Health.InconsistentEdges > 0);

    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Part"), TEXT("Part"), M, MeshAuditTest_DefaultConfig(), R);

    TestEqual(TEXT("inconsistent_winding is flagged"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::InconsistentWinding), 1);
    TestEqual(TEXT("inverted is NOT - the volume never went negative"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::Inverted), 0);
    TestFalse(TEXT("the sweep still goes red"), R.DerivePass(TEXT("error")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// An OPEN component cannot answer `inverted`. It must be UNRUNNABLE, never not-applicable or
// clean: the selected check had a mesh component to inspect, but its winding is not meaningful.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditOpenMeshTest,
    "PinWright.Geometry.MeshAudit.OpenMeshMakesInvertedUnrunnableNotClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditOpenMeshTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = MeshAuditTest_NewBox();
    Box->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
    {
        for (const int32 TriangleId : Edit.TriangleIndicesItr())
        {
            Edit.RemoveTriangle(TriangleId);
            break;
        }
    });

    const MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(Box);
    TestTrue(TEXT("removing a triangle opens the mesh"), M.Health.BoundaryEdges > 0);

    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Open"), TEXT("Open"), M, MeshAuditTest_DefaultConfig(), R);

    TestEqual(TEXT("not_closed is flagged"),
        MeshAuditTest_Flagged(R, MeshAudit::ECheck::NotClosed), 1);
    TestEqual(TEXT("inverted reports one applicable but UNKNOWN component"),
        R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)].Unrunnable, 1);
    TestEqual(TEXT("the unknown component is not put in not-applicable"),
        MeshAuditTest_NotApplicable(R, MeshAudit::ECheck::Inverted), 0);
    TestEqual(TEXT("and is never counted clean there"),
        R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)].Clean, 0);
    TestFalse(TEXT("an unknown component makes the shared verdict fail even at failOn=none"),
        R.DerivePass(TEXT("none")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The rule the whole verb is shaped around: an asset that could not be read must never read as
// a pass, and no failOn value may buy its way past that.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditUnrunnableTest,
    "PinWright.Geometry.MeshAudit.UnreadableAssetNeverReadsAsAPass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditUnrunnableTest::RunTest(const FString& Parameters)
{
    MeshAudit::FAssetMeasurement Missing;
    Missing.bMeasured = false;
    Missing.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_UNLOADABLE;
    Missing.UnrunnableReason = TEXT("probe");

    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Gone"), TEXT("Gone"), Missing,
                             MeshAuditTest_DefaultConfig(), R);

    TestEqual(TEXT("the asset counts as unmeasured"), R.AssetsUnmeasured, 1);
    TestTrue(TEXT("every selected check reported unrunnable"), R.UnrunnableCount > 0);
    TestEqual(TEXT("zero errors and zero warnings were raised"),
        R.ErrorCount + R.WarningCount, 0);
    TestTrue(TEXT("every finding row carries status unrunnable"),
        R.Findings.Num() > 0 && R.Findings[0].Status == MeshAudit::EFindingStatus::Unrunnable);

    // The point: with no error and no warning, the severity term is satisfied under EVERY
    // failOn. Only the unrunnable term keeps this from reporting a clean sweep.
    TestFalse(TEXT("failOn=error cannot pass"), R.DerivePass(TEXT("error")));
    TestFalse(TEXT("failOn=any cannot pass"), R.DerivePass(TEXT("any")));
    TestFalse(TEXT("failOn=none cannot pass either - this is the whole rule"),
        R.DerivePass(TEXT("none")));
    return true;
}

// Spatial isolation is the requested distinction: unwelded shells that overlap are normal,
// while the same shells moved beyond the model-derived tolerance are a warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditFloatingComponentsTest,
    "PinWright.Geometry.MeshAudit.FloatingComponentsUseProximityIslands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditFloatingComponentsTest::RunTest(const FString& Parameters)
{
    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::FloatingComponents);

    {
        const MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(
            MeshAuditTest_NewBoxAssembly({FVector::ZeroVector, FVector(90.0, 0.0, 0.0)}));
        MeshAudit::FReport R;
        MeshAudit::EvaluateAsset(TEXT("/Game/Test/Overlapping"), TEXT("Overlapping"), M,
                                 Config, R);
        TestEqual(TEXT("overlapping unwelded boxes form one proximity island"),
            MeshAuditTest_Flagged(R, MeshAudit::ECheck::FloatingComponents), 0);
        TestTrue(TEXT("the accepting direction passes even when warnings are selected"),
            R.DerivePass(TEXT("any")));
    }

    {
        const MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(
            MeshAuditTest_NewBoxAssembly({FVector::ZeroVector, FVector(300.0, 0.0, 0.0)}));
        MeshAudit::FReport R;
        MeshAudit::EvaluateAsset(TEXT("/Game/Test/Separated"), TEXT("Separated"), M,
                                 Config, R);
        TestEqual(TEXT("moved-apart boxes are flagged"),
            MeshAuditTest_Flagged(R, MeshAudit::ECheck::FloatingComponents), 1);
        TestFalse(TEXT("the failure direction is visible under failOn any"),
            R.DerivePass(TEXT("any")));
        TestEqual(TEXT("one component is reported as floating"), R.Findings.Num(), 1);
        if (R.Findings.Num() == 1 && R.Findings[0].Measurements.IsValid())
        {
            double Components = 0.0;
            double Islands = 0.0;
            double Floating = 0.0;
            double Suppressed = 0.0;
            double Unsuppressed = 0.0;
            double MainTriangles = 0.0;
            TestTrue(TEXT("the finding reports total component count"),
                R.Findings[0].Measurements->TryGetNumberField(TEXT("componentCount"), Components));
            TestTrue(TEXT("the finding reports island count"),
                R.Findings[0].Measurements->TryGetNumberField(TEXT("islandCount"), Islands));
            TestTrue(TEXT("the finding reports floating count"),
                R.Findings[0].Measurements->TryGetNumberField(TEXT("floatingCount"), Floating));
            TestTrue(TEXT("the finding reports suppressed count"),
                R.Findings[0].Measurements->TryGetNumberField(TEXT("suppressedCount"), Suppressed));
            TestTrue(TEXT("the finding reports unsuppressed count"),
                R.Findings[0].Measurements->TryGetNumberField(TEXT("unsuppressedCount"), Unsuppressed));
            TestTrue(TEXT("the finding reports the main island triangle count"),
                R.Findings[0].Measurements->TryGetNumberField(
                    TEXT("largestIslandTriangleCount"), MainTriangles));
            TestEqual(TEXT("the assembly has two edge-connected components"), Components, 2.0);
            TestEqual(TEXT("two boxes make two islands"), Islands, 2.0);
            TestEqual(TEXT("one box is outside the largest island"), Floating, 1.0);
            TestEqual(TEXT("static audit has no suppression surface"), Suppressed, 0.0);
            TestEqual(TEXT("the static floater remains unsuppressed"), Unsuppressed, 1.0);
            TestEqual(TEXT("suppressed and unsuppressed buckets sum to floating"),
                Suppressed + Unsuppressed, Floating);
            TestEqual(TEXT("the selected main island contains one box"), MainTriangles, 12.0);
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            TestTrue(TEXT("the finding carries per-component rows"),
                R.Findings[0].Measurements->TryGetArrayField(TEXT("floatingComponents"), Rows));
            if (Rows)
            {
                TestEqual(TEXT("one floater row is present"), Rows->Num(), 1);
                if (Rows->Num() == 1)
                {
                    const TSharedPtr<FJsonObject>* Row = nullptr;
                    if (TestTrue(TEXT("the floater row is an object"),
                                 (*Rows)[0]->TryGetObject(Row)) && Row && Row->IsValid())
                    {
                        double Distance = 0.0;
                        double TriangleCount = 0.0;
                        double SignedVolume = 0.0;
                        double NearestComponent = -1.0;
                        bool bSuppressed = true;
                        TestTrue(TEXT("the floater reports nearest distance"),
                            (*Row)->TryGetNumberField(TEXT("nearestDistance"), Distance));
                        TestTrue(TEXT("the floater reports triangle count"),
                            (*Row)->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
                        TestTrue(TEXT("the floater reports signed volume"),
                            (*Row)->TryGetNumberField(TEXT("signedVolume"), SignedVolume));
                        TestTrue(TEXT("the floater reports its nearest component"),
                            (*Row)->TryGetNumberField(
                                TEXT("nearestComponentIndex"), NearestComponent));
                        TestTrue(TEXT("the floater reports suppression state"),
                            (*Row)->TryGetBoolField(TEXT("suppressed"), bSuppressed));
                        const TSharedPtr<FJsonObject>* Center = nullptr;
                        TestTrue(TEXT("the floater reports its centre"),
                            (*Row)->TryGetObjectField(TEXT("center"), Center)
                            && Center && Center->IsValid());
                        TestTrue(TEXT("the far nearest distance is finite and exact"),
                            FMath::IsFinite(Distance)
                            && FMath::IsNearlyEqual(Distance, 200.0, 1.0e-6));
                        TestEqual(TEXT("the floater is the second twelve-triangle box"),
                            TriangleCount, 12.0);
                        TestTrue(TEXT("the closed box has non-zero volume"),
                            FMath::Abs(SignedVolume) > 0.0);
                        TestEqual(TEXT("the nearest component is the main box"),
                            NearestComponent, 0.0);
                        TestFalse(TEXT("the static row is not suppressed"), bSuppressed);
                    }
                }
            }
        }
    }

    {
        TArray<MeshAudit::FComponentMeasurement> Components;
        Components.SetNum(3);
        Components[0].Index = 0;
        Components[0].TriangleCount = 10;
        Components[1].Index = 1;
        Components[1].TriangleCount = 6;
        Components[2].Index = 2;
        Components[2].TriangleCount = 6;

        MeshAudit::FSpatialMeasurement Spatial;
        Spatial.bMeasured = true;
        Spatial.BoundingSphereRadius = 100.0;
        Spatial.PairDistances = {
            {0, 1, 10.0},
            {0, 2, 10.0},
            {1, 2, 0.0},
        };
        MeshAudit::FFloatingReport LargestComponentReport;
        MeshAudit::ClassifyFloatingComponents(
            Components, Spatial, 0.01, LargestComponentReport);
        TestEqual(TEXT("the largest individual component anchors the main island"),
            LargestComponentReport.LargestComponentIndex, 0);
        TestEqual(TEXT("the linked smaller components remain one other island"),
            LargestComponentReport.IslandCount, 2);
        TestEqual(TEXT("every component in the other island is reported"),
            LargestComponentReport.Components.Num(), 2);
        if (LargestComponentReport.Components.Num() == 2)
        {
            TestEqual(TEXT("the first smaller component is reported"),
                LargestComponentReport.Components[0].ComponentIndex, 1);
            TestEqual(TEXT("the second smaller component is reported"),
                LargestComponentReport.Components[1].ComponentIndex, 2);
        }
    }

    {
        const MeshAudit::FAssetMeasurement Chain = MeshAuditTest_Measure(
            MeshAuditTest_NewBoxAssembly({FVector::ZeroVector, FVector(90.0, 0.0, 0.0),
                                          FVector(180.0, 0.0, 0.0)}));
        MeshAudit::FFloatingReport ChainReport;
        MeshAudit::ClassifyFloatingComponents(Chain.Components, Chain.Spatial,
                                               Config.Thresholds.FloatingToleranceFraction,
                                               ChainReport);
        TestEqual(TEXT("the three-box chain is one transitive island"),
            ChainReport.IslandCount, 1);
        TestEqual(TEXT("the intact chain has no floater"), ChainReport.FloatingCount, 0);

        const MeshAudit::FAssetMeasurement RemovedMiddle = MeshAuditTest_Measure(
            MeshAuditTest_NewBoxAssembly({FVector::ZeroVector, FVector(180.0, 0.0, 0.0)}));
        MeshAudit::FFloatingReport RemovedReport;
        MeshAudit::ClassifyFloatingComponents(RemovedMiddle.Components, RemovedMiddle.Spatial,
                                               Config.Thresholds.FloatingToleranceFraction,
                                               RemovedReport);
        TestEqual(TEXT("removing the middle link splits the chain"),
            RemovedReport.IslandCount, 2);
        TestEqual(TEXT("the orphan side is flagged, not the main side"),
            RemovedReport.FloatingCount, 1);
        if (RemovedReport.Components.Num() == 1)
        {
            TestEqual(TEXT("the first/largest side remains the main component"),
                RemovedReport.Components[0].ComponentIndex, 1);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditPwModelFloatingSuppressionTest,
    "PinWright.Geometry.MeshAudit.PwModelFloatingSuppressionIsPartSpecificAndCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditPwModelFloatingSuppressionTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Warned = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n")
        TEXT("part ornament at=(300, 0, 0) {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n"),
        Options);
    TestTrue(TEXT("an unsuppressed floating model still validates"), Warned.bSuccess);
    TestEqual(TEXT("the model has two components"), Warned.FloatingGeometry.ComponentCount, 2);
    TestEqual(TEXT("one component is floating"), Warned.FloatingGeometry.FloatingCount, 1);
    TestEqual(TEXT("no component is suppressed"), Warned.FloatingGeometry.SuppressedCount, 0);
    TestEqual(TEXT("the detached ornament is unsuppressed"),
        Warned.FloatingGeometry.UnsuppressedCount, 1);
    TestEqual(TEXT("the unsuppressed model emits one floating warning"),
        MeshAuditTest_DiagnosticCount(
            Warned, PwModelDiagnosticCodes::PWMODEL_FLOATING_COMPONENT), 1);

    const FPwModelCompileResult Suppressed = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n")
        TEXT("part ornament allow_floating=true at=(300, 0, 0) {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n"),
        Options);
    TestTrue(TEXT("the explicitly allowed floating model validates"), Suppressed.bSuccess);
    TestEqual(TEXT("the measured floater remains in the report"),
        Suppressed.FloatingGeometry.FloatingCount, 1);
    TestEqual(TEXT("the named ornament is the one suppressed component"),
        Suppressed.FloatingGeometry.SuppressedCount, 1);
    TestEqual(TEXT("no unsuppressed floater remains"),
        Suppressed.FloatingGeometry.UnsuppressedCount, 0);
    TestEqual(TEXT("the two buckets sum to the measured floating count"),
        Suppressed.FloatingGeometry.SuppressedCount
            + Suppressed.FloatingGeometry.UnsuppressedCount,
        Suppressed.FloatingGeometry.FloatingCount);
    TestEqual(TEXT("suppression removes only the warning diagnostic"),
        MeshAuditTest_DiagnosticCount(
            Suppressed, PwModelDiagnosticCodes::PWMODEL_FLOATING_COMPONENT), 0);
    TestEqual(TEXT("both named parts remain in the result"), Suppressed.Parts.Num(), 2);
    if (Suppressed.Parts.Num() == 2)
    {
        TestFalse(TEXT("the main body did not inherit suppression"),
            Suppressed.Parts[0].bAllowFloating);
        TestTrue(TEXT("only the ornament parsed allow_floating=true"),
            Suppressed.Parts[1].bAllowFloating);
    }
    TestEqual(TEXT("one floating component row remains visible"),
        Suppressed.FloatingGeometry.Components.Num(), 1);
    if (Suppressed.FloatingGeometry.Components.Num() == 1)
    {
        TestTrue(TEXT("the visible measurement row is marked suppressed"),
            Suppressed.FloatingGeometry.Components[0].bSuppressed);
        TestTrue(TEXT("the suppressed row retains exact nearest distance"),
            Suppressed.FloatingGeometry.Components[0].NearestDistance > 0.0);
    }
    return true;
}

// The animation fixture is a real imported-LOD-shaped mesh plus a real two-bone UAnimSequence,
// all created in memory. The child part overlaps the root part in bind pose, rotates 180 degrees
// around its own bone at frame 1, then returns. Full sampling must catch only that middle frame;
// stride 2 deliberately demonstrates why the response reports sparse coverage instead of
// pretending the animation passed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditAnimatedFloatingComponentsTest,
    "PinWright.Geometry.MeshAudit.AnimationReportsTheFirstAndWorstSeparatingFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditAnimatedFloatingComponentsTest::RunTest(const FString& Parameters)
{
    FMeshAuditAnimationFixture Fixture = MeshAuditTest_NewTwoBoneAnimationFixture();
    TestNotNull(TEXT("synthetic skeletal mesh was created"), Fixture.Mesh);
    TestNotNull(TEXT("synthetic animation sequence was created"), Fixture.Sequence);
    if (!Fixture.Mesh || !Fixture.Sequence)
    {
        return true;
    }

    MeshAudit::FAnimationFloatingReport Full;
    MeshAudit::MeasureSkeletalAnimationFloating(
        Fixture.Mesh, Fixture.Sequence, 1, MeshAudit::DefaultFloatingToleranceFraction, Full);
    TestTrue(TEXT("full frame scan is measurable"), Full.bMeasured);
    TestEqual(TEXT("the sequence timeline spans two frames"), Full.FrameCount, 2);
    TestEqual(TEXT("the full scan reports unit stride"), Full.SampleStride, 1);
    TestEqual(TEXT("full scan samples every timeline position"), Full.SampledFrameCount, 3);
    TestTrue(TEXT("timeline span is not confused with stored key count"),
        Full.FrameCount != Full.SampledFrameCount);
    TestEqual(TEXT("full scan publishes every sampled frame id"), Full.SampledFrameIds.Num(), 3);
    if (Full.SampledFrameIds.Num() == 3)
    {
        TestEqual(TEXT("full scan starts at frame zero"), Full.SampledFrameIds[0], 0);
        TestEqual(TEXT("full scan includes the planted middle frame"), Full.SampledFrameIds[1], 1);
        TestEqual(TEXT("full scan includes the final frame"), Full.SampledFrameIds[2], 2);
    }
    TestEqual(TEXT("bind pose has no floater"), Full.BindFloatingCount, 0);
    TestEqual(TEXT("one component separates during the animation"), Full.Components.Num(), 1);
    if (Full.Components.Num() == 1)
    {
        const MeshAudit::FAnimationFloatingComponent& Row = Full.Components[0];
        TestFalse(TEXT("the separated part was connected in bind pose"),
                  Row.bAlreadySeparatedAtBindPose);
        TestEqual(TEXT("the first separation is the planted middle frame"),
                  Row.FirstSeparatedFrame, 1);
        TestEqual(TEXT("the worst frame is the same middle frame"), Row.WorstFrame, 1);
        TestTrue(TEXT("the first measured separation is positive"), Row.FirstSeparation > 0.0);
        TestTrue(TEXT("the measured opening is greater than the model-derived tolerance"),
                 Row.WorstSeparation > Full.Tolerance);
    }

    MeshAudit::FAnimationFloatingReport FixedScale;
    constexpr double LargeToleranceFraction = 1.9;
    MeshAudit::MeasureSkeletalAnimationFloating(
        Fixture.Mesh, Fixture.Sequence, 1, LargeToleranceFraction, FixedScale);
    TestTrue(TEXT("fixed-scale animation scan is measurable"), FixedScale.bMeasured);
    TestTrue(TEXT("animation tolerance is derived once from bind-pose radius"),
        FMath::IsNearlyEqual(
            FixedScale.Tolerance,
            FixedScale.BoundingSphereRadius * LargeToleranceFraction,
            1.0e-6));
    TestEqual(TEXT("expanded posed bounds do not enlarge the link tolerance"),
        FixedScale.Components.Num(), 1);
    if (FixedScale.Components.Num() == 1)
    {
        TestTrue(TEXT("the excursion remains beyond the fixed bind-pose tolerance"),
            FixedScale.Components[0].WorstSeparation > FixedScale.Tolerance);
    }

    const TSharedPtr<FJsonObject> FullResult = MeshAudit::SerializeAnimationFloatingResult(
        TEXT("/Game/Test/SK_TwoBone"), TEXT("/Game/Test/A_TwoBone"), Full,
        PinWrightAudit::EFailOn::Error);
    TestTrue(TEXT("the full report serializes to a public result"), FullResult.IsValid());
    if (FullResult.IsValid())
    {
        double ModelFloatingCount = -1.0;
        double AnimationFloatingCount = -1.0;
        TestTrue(TEXT("public result reports the model-owned count"),
            FullResult->TryGetNumberField(TEXT("modelFloatingCount"), ModelFloatingCount));
        TestTrue(TEXT("public result reports the animation-caused count"),
            FullResult->TryGetNumberField(TEXT("animationFloatingCount"), AnimationFloatingCount));
        TestEqual(TEXT("the connected bind pose contributes no model-owned row"),
            ModelFloatingCount, 0.0);
        TestEqual(TEXT("the middle-frame separation is animation-caused"),
            AnimationFloatingCount, 1.0);

        const TSharedPtr<FJsonObject>* Sampling = nullptr;
        TestTrue(TEXT("public result contains sampling metadata"),
            FullResult->TryGetObjectField(TEXT("sampling"), Sampling)
            && Sampling && Sampling->IsValid());
        if (Sampling && Sampling->IsValid())
        {
            double FrameCount = -1.0;
            double SampleStride = -1.0;
            double SampledFrameCount = -1.0;
            bool bComplete = false;
            TestTrue(TEXT("sampling reports frameCount"),
                (*Sampling)->TryGetNumberField(TEXT("frameCount"), FrameCount));
            TestTrue(TEXT("sampling reports sampleStride"),
                (*Sampling)->TryGetNumberField(TEXT("sampleStride"), SampleStride));
            TestTrue(TEXT("sampling reports sampledFrameCount"),
                (*Sampling)->TryGetNumberField(TEXT("sampledFrameCount"), SampledFrameCount));
            TestTrue(TEXT("sampling reports complete"),
                (*Sampling)->TryGetBoolField(TEXT("complete"), bComplete));
            TestEqual(TEXT("public frame count matches the measured report"), FrameCount, 2.0);
            TestEqual(TEXT("public stride matches the measured report"), SampleStride, 1.0);
            TestEqual(TEXT("public sampled count matches the measured report"),
                SampledFrameCount, 3.0);
            TestTrue(TEXT("sampling every key is marked complete"), bComplete);

            const TArray<TSharedPtr<FJsonValue>>* SampledFrameIds = nullptr;
            TestTrue(TEXT("sampling publishes sampledFrameIds"),
                (*Sampling)->TryGetArrayField(TEXT("sampledFrameIds"), SampledFrameIds));
            TestTrue(TEXT("public sampledFrameIds has one entry per sampled key"),
                SampledFrameIds && SampledFrameIds->Num() == 3);
            if (SampledFrameIds && SampledFrameIds->Num() == 3)
            {
                TestEqual(TEXT("public first sampled frame id"), (*SampledFrameIds)[0]->AsNumber(), 0.0);
                TestEqual(TEXT("public middle sampled frame id"), (*SampledFrameIds)[1]->AsNumber(), 1.0);
                TestEqual(TEXT("public final sampled frame id"), (*SampledFrameIds)[2]->AsNumber(), 2.0);
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestTrue(TEXT("public result contains component rows"),
            FullResult->TryGetArrayField(TEXT("components"), Rows));
        TestTrue(TEXT("public result contains the one animation-caused row"),
            Rows && Rows->Num() == 1);
        if (Rows && Rows->Num() == 1 && Full.Components.Num() == 1)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            TestTrue(TEXT("public component row is an object"), (*Rows)[0]->TryGetObject(Row));
            if (Row && Row->IsValid())
            {
                const MeshAudit::FAnimationFloatingComponent& Measured = Full.Components[0];
                double ComponentIndex = -1.0;
                double TriangleCount = -1.0;
                double SignedVolume = 0.0;
                double NearestComponentIndex = -1.0;
                double FirstSeparation = -1.0;
                double FirstSeparatedFrame = -1.0;
                double WorstFrame = -1.0;
                double WorstSeparation = -1.0;
                bool bAlreadySeparatedAtBindPose = true;
                TestTrue(TEXT("row publishes componentIndex"),
                    (*Row)->TryGetNumberField(TEXT("componentIndex"), ComponentIndex));
                TestTrue(TEXT("row publishes triangleCount"),
                    (*Row)->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
                TestTrue(TEXT("row publishes signedVolume"),
                    (*Row)->TryGetNumberField(TEXT("signedVolume"), SignedVolume));
                TestTrue(TEXT("row publishes nearestComponentIndex"),
                    (*Row)->TryGetNumberField(TEXT("nearestComponentIndex"), NearestComponentIndex));
                TestTrue(TEXT("row publishes firstSeparation"),
                    (*Row)->TryGetNumberField(TEXT("firstSeparation"), FirstSeparation));
                TestTrue(TEXT("row publishes firstSeparatedFrame"),
                    (*Row)->TryGetNumberField(TEXT("firstSeparatedFrame"), FirstSeparatedFrame));
                TestTrue(TEXT("row publishes worstFrame"),
                    (*Row)->TryGetNumberField(TEXT("worstFrame"), WorstFrame));
                TestTrue(TEXT("row publishes worstSeparation"),
                    (*Row)->TryGetNumberField(TEXT("worstSeparation"), WorstSeparation));
                TestTrue(TEXT("row publishes bind-pose attribution"),
                    (*Row)->TryGetBoolField(
                        TEXT("alreadySeparatedAtBindPose"), bAlreadySeparatedAtBindPose));
                TestEqual(TEXT("public component index matches measurement"),
                    ComponentIndex, static_cast<double>(Measured.ComponentIndex));
                TestEqual(TEXT("public triangle count matches measurement"),
                    TriangleCount, static_cast<double>(Measured.TriangleCount));
                TestEqual(TEXT("public signed volume matches measurement"),
                    SignedVolume, Measured.SignedVolume);
                TestEqual(TEXT("public nearest component matches measurement"),
                    NearestComponentIndex, static_cast<double>(Measured.NearestComponentIndex));
                TestEqual(TEXT("public first separation matches measurement"),
                    FirstSeparation, Measured.FirstSeparation);
                TestEqual(TEXT("public first frame matches measurement"),
                    FirstSeparatedFrame, static_cast<double>(Measured.FirstSeparatedFrame));
                TestEqual(TEXT("public worst frame matches measurement"),
                    WorstFrame, static_cast<double>(Measured.WorstFrame));
                TestEqual(TEXT("public worst distance matches measurement"),
                    WorstSeparation, Measured.WorstSeparation);
                TestFalse(TEXT("animation-caused row is not bind-pose-owned"),
                    bAlreadySeparatedAtBindPose);

                const TSharedPtr<FJsonObject>* Center = nullptr;
                TestTrue(TEXT("row publishes center"),
                    (*Row)->TryGetObjectField(TEXT("center"), Center)
                    && Center && Center->IsValid());
                if (Center && Center->IsValid())
                {
                    double X = 0.0;
                    double Y = 0.0;
                    double Z = 0.0;
                    TestTrue(TEXT("center publishes x"), (*Center)->TryGetNumberField(TEXT("x"), X));
                    TestTrue(TEXT("center publishes y"), (*Center)->TryGetNumberField(TEXT("y"), Y));
                    TestTrue(TEXT("center publishes z"), (*Center)->TryGetNumberField(TEXT("z"), Z));
                    TestEqual(TEXT("public center x matches measurement"), X, Measured.Center.X);
                    TestEqual(TEXT("public center y matches measurement"), Y, Measured.Center.Y);
                    TestEqual(TEXT("public center z matches measurement"), Z, Measured.Center.Z);
                }
            }
        }
    }

    MeshAudit::FAnimationFloatingReport Sparse;
    MeshAudit::MeasureSkeletalAnimationFloating(
        Fixture.Mesh, Fixture.Sequence, 2, MeshAudit::DefaultFloatingToleranceFraction, Sparse);
    TestTrue(TEXT("the sparse scan is still measurable"), Sparse.bMeasured);
    TestEqual(TEXT("the sparse scan reports the requested stride"), Sparse.SampleStride, 2);
    TestEqual(TEXT("the final frame is always included"), Sparse.SampledFrameCount, 2);
    TestEqual(TEXT("the sparse scan publishes two sampled frame ids"),
        Sparse.SampledFrameIds.Num(), 2);
    if (Sparse.SampledFrameIds.Num() == 2)
    {
        TestEqual(TEXT("the sparse scan starts at frame zero"), Sparse.SampledFrameIds[0], 0);
        TestEqual(TEXT("the sparse scan ends at the final frame"), Sparse.SampledFrameIds[1], 2);
    }
    TestEqual(TEXT("the sparse scan does not see the unsampled separation"),
              Sparse.Components.Num(), 0);
    TestTrue(TEXT("a sparse scan is visibly incomplete"),
        Sparse.SampledFrameCount < Sparse.FrameCount + 1);

    const TSharedPtr<FJsonObject> SparseResult = MeshAudit::SerializeAnimationFloatingResult(
        TEXT("/Game/Test/SK_TwoBone"), TEXT("/Game/Test/A_TwoBone"), Sparse,
        PinWrightAudit::EFailOn::Error);
    const TSharedPtr<FJsonObject>* SparseSampling = nullptr;
    TestTrue(TEXT("the sparse public result contains sampling metadata"),
        SparseResult.IsValid()
        && SparseResult->TryGetObjectField(TEXT("sampling"), SparseSampling)
        && SparseSampling && SparseSampling->IsValid());
    if (SparseSampling && SparseSampling->IsValid())
    {
        double SampleStride = -1.0;
        double SampledFrameCount = -1.0;
        bool bComplete = true;
        TestTrue(TEXT("sparse public result reports stride"),
            (*SparseSampling)->TryGetNumberField(TEXT("sampleStride"), SampleStride));
        TestTrue(TEXT("sparse public result reports sampled count"),
            (*SparseSampling)->TryGetNumberField(TEXT("sampledFrameCount"), SampledFrameCount));
        TestTrue(TEXT("sparse public result reports completeness"),
            (*SparseSampling)->TryGetBoolField(TEXT("complete"), bComplete));
        TestEqual(TEXT("sparse public stride is two"), SampleStride, 2.0);
        TestEqual(TEXT("sparse public sampled count is two"), SampledFrameCount, 2.0);
        TestFalse(TEXT("sparse public result is explicitly incomplete"), bComplete);
        const TArray<TSharedPtr<FJsonValue>>* SampledFrameIds = nullptr;
        TestTrue(TEXT("sparse public result publishes sampledFrameIds"),
            (*SparseSampling)->TryGetArrayField(TEXT("sampledFrameIds"), SampledFrameIds));
        TestTrue(TEXT("sparse public frame ids are exactly the measured endpoints"),
            SampledFrameIds && SampledFrameIds->Num() == 2
            && (*SampledFrameIds)[0]->AsNumber() == 0.0
            && (*SampledFrameIds)[1]->AsNumber() == 2.0);
    }

    const FMeshAuditAnimationFixture BindFixture =
        MeshAuditTest_NewTwoBoneAnimationFixture(/*bBindFloating=*/true);
    TestNotNull(TEXT("synthetic bind-floater mesh was created"), BindFixture.Mesh);
    TestNotNull(TEXT("synthetic bind-floater sequence was created"), BindFixture.Sequence);
    if (BindFixture.Mesh && BindFixture.Sequence)
    {
        MeshAudit::FAnimationFloatingReport BindFloating;
        MeshAudit::MeasureSkeletalAnimationFloating(
            BindFixture.Mesh, BindFixture.Sequence, 1,
            MeshAudit::DefaultFloatingToleranceFraction, BindFloating);
        TestTrue(TEXT("bind-pose floater is measurable"), BindFloating.bMeasured);
        TestEqual(TEXT("bind pose contains one floater"), BindFloating.BindFloatingCount, 1);
        TestEqual(TEXT("bind-pose floater remains one visible component row"),
            BindFloating.Components.Num(), 1);
        if (BindFloating.Components.Num() == 1)
        {
            const MeshAudit::FAnimationFloatingComponent& Row = BindFloating.Components[0];
            TestTrue(TEXT("synthetic bind-pose floater is marked model-owned"),
                Row.bAlreadySeparatedAtBindPose);
            TestEqual(TEXT("bind-pose measurement is attributed to frame zero"),
                Row.FirstSeparatedFrame, 0);
            TestEqual(TEXT("bind-pose floater keeps model attribution while later frames update"),
                Row.WorstFrame, 1);
            TestTrue(TEXT("bind-pose floater records a larger later-frame separation"),
                Row.FirstSeparation > 0.0 && Row.WorstSeparation > Row.FirstSeparation);
        }

        const TSharedPtr<FJsonObject> BindResult = MeshAudit::SerializeAnimationFloatingResult(
            TEXT("/Game/Test/SK_BindFloater"), TEXT("/Game/Test/A_BindFloater"), BindFloating,
            PinWrightAudit::EFailOn::Error);
        TestTrue(TEXT("bind-pose report serializes to a public result"), BindResult.IsValid());
        if (BindResult.IsValid())
        {
            double ModelFloatingCount = -1.0;
            double AnimationFloatingCount = -1.0;
            TestTrue(TEXT("bind result reports modelFloatingCount"),
                BindResult->TryGetNumberField(TEXT("modelFloatingCount"), ModelFloatingCount));
            TestTrue(TEXT("bind result reports animationFloatingCount"),
                BindResult->TryGetNumberField(
                    TEXT("animationFloatingCount"), AnimationFloatingCount));
            TestEqual(TEXT("the bind-pose floater is model-owned"), ModelFloatingCount, 1.0);
            TestEqual(TEXT("the model-owned row is excluded from animation-caused rows"),
                AnimationFloatingCount, 0.0);

            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            TestTrue(TEXT("bind result keeps the model-owned component row visible"),
                BindResult->TryGetArrayField(TEXT("components"), Rows)
                && Rows && Rows->Num() == 1);
            if (Rows && Rows->Num() == 1)
            {
                const TSharedPtr<FJsonObject>* Row = nullptr;
                bool bAlreadySeparatedAtBindPose = false;
                TestTrue(TEXT("bind result component row is an object"),
                    (*Rows)[0]->TryGetObject(Row));
                TestTrue(TEXT("public bind row carries model attribution"),
                    Row && Row->IsValid()
                    && (*Row)->TryGetBoolField(
                        TEXT("alreadySeparatedAtBindPose"), bAlreadySeparatedAtBindPose)
                    && bAlreadySeparatedAtBindPose);
            }
        }
    }

    {
        const FMeshAuditAnimationFixture RetargetedFixture =
            MeshAuditTest_NewTwoBoneAnimationFixture(false, true);
        TestTrue(TEXT("the retargeted fixture has a valid track mapping"),
            MeshAuditTest_SetBoneTrackState(
                RetargetedFixture.Sequence, FName(TEXT("part")), 1, NAME_None));
        MeshAudit::FAnimationFloatingReport Retargeted;
        MeshAudit::MeasureSkeletalAnimationFloating(
            RetargetedFixture.Mesh, RetargetedFixture.Sequence, 1,
            MeshAudit::DefaultFloatingToleranceFraction, Retargeted);
        TestTrue(TEXT("a normally mapped retargeted sequence is measurable"), Retargeted.bMeasured);
        TestEqual(TEXT("engine retargeting remains authoritative for a valid track mapping"),
            Retargeted.Components.Num(), 0);
    }

    {
        FMeshAuditAnimationFixture TransientFixture =
            MeshAuditTest_NewTwoBoneAnimationFixture();
        TestTrue(TEXT("the transient fallback fixture has a mutable track"),
            MeshAuditTest_SetBoneTrackState(
                TransientFixture.Sequence, FName(TEXT("part")), INDEX_NONE, NAME_None));
        MeshAudit::FAnimationFloatingReport Transient;
        MeshAudit::MeasureSkeletalAnimationFloating(
            TransientFixture.Mesh, TransientFixture.Sequence, 1,
            MeshAudit::DefaultFloatingToleranceFraction, Transient);
        TestTrue(TEXT("a missing track mapping is still measurable through the fallback"),
            Transient.bMeasured);
        TestEqual(TEXT("the fallback evaluates the animated separation"),
            Transient.Components.Num(), 1);
    }

    {
        FMeshAuditAnimationFixture UnknownTrackFixture =
            MeshAuditTest_NewTwoBoneAnimationFixture();
        TestTrue(TEXT("the unknown-track fixture has a mutable track"),
            MeshAuditTest_SetBoneTrackState(
                UnknownTrackFixture.Sequence, FName(TEXT("part")), 1,
                FName(TEXT("missing_bone"))));
        MeshAudit::FAnimationFloatingReport UnknownTrack;
        MeshAudit::MeasureSkeletalAnimationFloating(
            UnknownTrackFixture.Mesh, UnknownTrackFixture.Sequence, 1,
            MeshAudit::DefaultFloatingToleranceFraction, UnknownTrack);
        TestFalse(TEXT("a track absent from the mesh skeleton is unrunnable"),
            UnknownTrack.bMeasured);
        TestTrue(TEXT("the unrunnable result names the missing track"),
            UnknownTrack.UnrunnableReason.Contains(TEXT("missing_bone")));
        // GetBoneTrackByName can return its shared invalid-track sentinel for these transient
        // models. The test deliberately mutates that object, so restore it before another
        // fixture evaluates a track or the next test inherits the synthetic missing name.
        TestTrue(TEXT("the unknown-track fixture restores the shared track state"),
            MeshAuditTest_SetBoneTrackState(
                UnknownTrackFixture.Sequence, FName(TEXT("part")), 1,
                FName(TEXT("part"))));
    }

    if (Full.Components.Num() == 1)
    {
        MeshAudit::FAnimationFloatingReport Suppressed = Full;
        Suppressed.AllowedComponentIndices.Add(Full.Components[0].ComponentIndex);
        Suppressed.Components[0].bSuppressed = true;
        Suppressed.SuppressedCount = 1;
        Suppressed.UnsuppressedCount = 0;
        const TSharedPtr<FJsonObject> SuppressedResult =
            MeshAudit::SerializeAnimationFloatingResult(
                TEXT("/Game/Test/SK_TwoBone"), TEXT("/Game/Test/A_TwoBone"), Suppressed,
                PinWrightAudit::EFailOn::Error);
        TestTrue(TEXT("suppressed animation result remains serializable"), SuppressedResult.IsValid());
        if (SuppressedResult.IsValid())
        {
            double AnimationFloatingCount = -1.0;
            double SuppressedCount = -1.0;
            TestTrue(TEXT("suppression result reports animation count"),
                SuppressedResult->TryGetNumberField(
                    TEXT("animationFloatingCount"), AnimationFloatingCount));
            TestTrue(TEXT("suppression result reports suppressed count"),
                SuppressedResult->TryGetNumberField(TEXT("suppressedCount"), SuppressedCount));
            TestEqual(TEXT("an allowed row does not count as an animation warning"),
                AnimationFloatingCount, 0.0);
            TestEqual(TEXT("an allowed row is counted as suppressed"), SuppressedCount, 1.0);
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            TestTrue(TEXT("suppression keeps the measured row visible"),
                SuppressedResult->TryGetArrayField(TEXT("components"), Rows)
                && Rows && Rows->Num() == 1);
            if (Rows && Rows->Num() == 1)
            {
                const TSharedPtr<FJsonObject>* Row = nullptr;
                bool bSuppressed = false;
                TestTrue(TEXT("suppressed row is an object"), (*Rows)[0]->TryGetObject(Row));
                TestTrue(TEXT("suppressed row carries its suppression state"),
                    Row && Row->IsValid()
                    && (*Row)->TryGetBoolField(TEXT("suppressed"), bSuppressed)
                    && bSuppressed);
            }
        }
    }

    {
        MeshAudit::FAnimationFloatingReport Unrunnable;
        Unrunnable.UnrunnableCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
        Unrunnable.UnrunnableReason = TEXT("synthetic missing asset");
        const TSharedPtr<FJsonObject> UnrunnableResult =
            MeshAudit::SerializeAnimationFloatingResult(
                TEXT("/Game/Test/MissingMesh"), TEXT("/Game/Test/MissingAnimation"),
                Unrunnable, PinWrightAudit::EFailOn::Error);
        TestTrue(TEXT("missing-asset result is a structured response"), UnrunnableResult.IsValid());
        if (UnrunnableResult.IsValid())
        {
            bool bPass = true;
            FString Code;
            FString Status;
            TestTrue(TEXT("missing-asset result fails the shared verdict"),
                UnrunnableResult->TryGetBoolField(TEXT("pass"), bPass) && !bPass);
            TestTrue(TEXT("missing-asset result preserves its error code"),
                UnrunnableResult->TryGetStringField(TEXT("code"), Code)
                && Code == ErrorCodes::ERR_ASSET_NOT_FOUND);
            TestTrue(TEXT("missing-asset result is marked unrunnable"),
                UnrunnableResult->TryGetStringField(TEXT("status"), Status)
                && Status == TEXT("unrunnable"));
        }
    }
    return true;
}

// A constant-position child can still separate when its bone rolls around its own axis. This is
// deliberately a two-frame sequence: frame zero is connected, and frame one contains only roll.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditAnimatedChildBoneRollOnlyTest,
    "PinWright.Geometry.MeshAudit.AnimationRollOnlyReportsFrameOneSeparation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditAnimatedChildBoneRollOnlyTest::RunTest(const FString& Parameters)
{
    const FMeshAuditAnimationFixture Fixture =
        MeshAuditTest_NewTwoBoneAnimationFixture(false, false, false, true);
    TestNotNull(TEXT("roll-only skeletal mesh was created"), Fixture.Mesh);
    TestNotNull(TEXT("roll-only animation sequence was created"), Fixture.Sequence);
    if (!Fixture.Mesh || !Fixture.Sequence)
    {
        return true;
    }

    MeshAudit::FAnimationFloatingReport Report;
    MeshAudit::MeasureSkeletalAnimationFloating(
        Fixture.Mesh, Fixture.Sequence, 1, MeshAudit::DefaultFloatingToleranceFraction, Report);
    const FString MeasurabilityLabel = FString::Printf(
        TEXT("roll-only scan is measurable (code='%s', reason='%s')"),
        *Report.UnrunnableCode, *Report.UnrunnableReason);
    TestTrue(*MeasurabilityLabel, Report.bMeasured);
    TestEqual(TEXT("roll-only animation spans two frame positions"), Report.FrameCount, 1);
    TestEqual(TEXT("roll-only scan samples both frame positions"), Report.SampledFrameCount, 2);
    TestEqual(TEXT("roll-only scan publishes two frame ids"), Report.SampledFrameIds.Num(), 2);
    if (Report.SampledFrameIds.Num() == 2)
    {
        TestEqual(TEXT("roll-only scan starts at frame zero"), Report.SampledFrameIds[0], 0);
        TestEqual(TEXT("roll-only scan ends at frame one"), Report.SampledFrameIds[1], 1);
    }
    TestEqual(TEXT("the roll-only child is connected at frame zero"), Report.BindFloatingCount, 0);
    TestEqual(TEXT("one child component separates at frame one"), Report.Components.Num(), 1);
    if (Report.Components.Num() == 1)
    {
        const MeshAudit::FAnimationFloatingComponent& Row = Report.Components[0];
        TestFalse(TEXT("the roll-only child was not separated at frame zero"),
            Row.bAlreadySeparatedAtBindPose);
        TestTrue(TEXT("the roll-only child has positive frame-one separation"),
            Row.FirstSeparation > 0.0);
        TestEqual(TEXT("the roll-only child first separates at frame one"),
            Row.FirstSeparatedFrame, 1);
        TestEqual(TEXT("the roll-only child worst separation is at frame one"),
            Row.WorstFrame, 1);
    }
    return true;
}

// Two detached source sections move the same known distance and overlap each other while they
// are detached from the body. The old nearest-any-component metric reports zero for both rows,
// because each detached section is nearest to its detached sibling instead of to the body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditAnimatedFloatingIslandDistanceTest,
    "PinWright.Geometry.MeshAudit.AnimationFloatingReportsDistanceToLargestIsland",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditAnimatedFloatingIslandDistanceTest::RunTest(const FString& Parameters)
{
    const FMeshAuditAnimationFixture Fixture =
        MeshAuditTest_NewTwoBoneAnimationFixture(false, false, true);
    TestNotNull(TEXT("known-separation skeletal mesh was created"), Fixture.Mesh);
    TestNotNull(TEXT("known-separation animation was created"), Fixture.Sequence);
    if (!Fixture.Mesh || !Fixture.Sequence)
    {
        return true;
    }

    MeshAudit::FAnimationFloatingReport Report;
    MeshAudit::MeasureSkeletalAnimationFloating(
        Fixture.Mesh, Fixture.Sequence, 1, MeshAudit::DefaultFloatingToleranceFraction, Report);
    TestTrue(TEXT("known-separation scan is measurable"), Report.bMeasured);
    TestEqual(TEXT("the bind pose is one proximity island"), Report.BindIslandCount, 1);
    TestEqual(TEXT("two detached components produce two rows"), Report.Components.Num(), 2);

    // The root box ends at x=50 and both child boxes begin at x=900 in frame 1: the exact
    // triangle-to-triangle separation from either detached box to the body is 850 Unreal units.
    constexpr double KnownSeparation = 850.0;
    for (const MeshAudit::FAnimationFloatingComponent& Row : Report.Components)
    {
        TestEqual(TEXT("each detached row is attributed to frame 1"), Row.FirstSeparatedFrame, 1);
        TestEqual(TEXT("each detached row has frame 1 as its worst frame"), Row.WorstFrame, 1);
        TestTrue(TEXT("first separation is the known non-zero body gap"),
            FMath::IsNearlyEqual(Row.FirstSeparation, KnownSeparation, 1.0e-3));
        TestTrue(TEXT("worst separation is the known non-zero body gap"),
            FMath::IsNearlyEqual(Row.WorstSeparation, KnownSeparation, 1.0e-3));
        TestEqual(TEXT("each detached row attributes its nearest component to the body"),
            Row.NearestComponentIndex, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditAnimationFloatingBadArgumentsTest,
    "PinWright.Geometry.MeshAudit.AnimationFloatingRejectsMalformedArgumentsBeforeWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditAnimationFloatingBadArgumentsTest::RunTest(const FString& Parameters)
{
    const FString Method = TEXT("geometry.audit_skeletal_animation_floating");
    auto MakeBasePayload = []()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        // These paths are deliberately well-formed but do not exist. INVALID_ARGUMENT therefore
        // proves each malformed option was rejected before either asset lookup or job creation.
        Payload->SetStringField(TEXT("skeletalMeshPath"), TEXT("/Game/Test/SK_NotLoaded"));
        Payload->SetStringField(TEXT("animationPath"), TEXT("/Game/Test/A_NotLoaded"));
        return Payload;
    };
    auto AssertRejectedBeforeWork = [this, &Method](
        const TCHAR* Label, const TSharedPtr<FJsonObject>& Payload)
    {
        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s handler is registered"), Label),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestTrue(*FString::Printf(TEXT("%s returns an immediate response"), Label),
            Capture.bWasCalled);
        TestFalse(*FString::Printf(TEXT("%s does not start work"), Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s is INVALID_ARGUMENT"), Label),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestFalse(*FString::Printf(TEXT("%s returns no job/result payload"), Label),
            Capture.Result.IsValid());
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetNumberField(TEXT("sampleStride"), 0);
        AssertRejectedBeforeWork(TEXT("zero sampleStride"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetNumberField(TEXT("floatingToleranceFraction"), -0.25);
        AssertRejectedBeforeWork(TEXT("negative floatingToleranceFraction"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetStringField(TEXT("failOn"), TEXT("sometimes"));
        AssertRejectedBeforeWork(TEXT("unknown failOn"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetStringField(TEXT("animationPath"), TEXT("A_NotAContentPath"));
        AssertRejectedBeforeWork(TEXT("malformed animationPath"), Payload);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        TArray<TSharedPtr<FJsonValue>> Allowed;
        Allowed.Add(MakeShared<FJsonValueString>(TEXT("not-an-index")));
        Payload->SetArrayField(TEXT("allowFloatingComponents"), Allowed);
        AssertRejectedBeforeWork(TEXT("malformed allowFloatingComponents"), Payload);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// A page that stopped short of the match set has not audited the set. Half a folder reported
// clean is how check_actors once exited 0 having examined nothing.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditTruncationTest,
    "PinWright.Geometry.MeshAudit.TruncatedSweepCannotReportPass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditTruncationTest::RunTest(const FString& Parameters)
{
    MeshAudit::FReport Clean;
    TestTrue(TEXT("an empty, untruncated report passes"), Clean.DerivePass(TEXT("error")));

    MeshAudit::FReport Paged;
    Paged.bPageTruncated = true;
    TestFalse(TEXT("a page short of the set cannot pass"), Paged.DerivePass(TEXT("none")));

    MeshAudit::FReport Clipped;
    Clipped.bFindingsTruncated = true;
    TestFalse(TEXT("a findings list clipped by maxFindings cannot pass"),
        Clipped.DerivePass(TEXT("none")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// ParseCheckId must REJECT an unknown id. A `checks` array that silently ran nothing would be
// indistinguishable from a folder of correct meshes.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditCheckTableTest,
    "PinWright.Geometry.MeshAudit.UnknownCheckIdIsRejectedAndTheTableIsWellFormed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditCheckTableTest::RunTest(const FString& Parameters)
{
    MeshAudit::ECheck Parsed = MeshAudit::ECheck::Inverted;
    TestFalse(TEXT("an unknown id does not parse"),
        MeshAudit::ParseCheckId(TEXT("no_such_check"), Parsed));
    TestTrue(TEXT("a known id does"), MeshAudit::ParseCheckId(TEXT("inverted"), Parsed));
    TestTrue(TEXT("and ids are case-insensitive"),
        MeshAudit::ParseCheckId(TEXT("INVERTED"), Parsed));

    const TArray<MeshAudit::FCheckInfo>& Checks = MeshAudit::AllChecks();
    TestEqual(TEXT("the table covers every ECheck"), Checks.Num(), MeshAudit::CheckCount);
    for (int32 Index = 0; Index < Checks.Num(); ++Index)
    {
        TestEqual(TEXT("table index matches ECheck order"),
            static_cast<int32>(Checks[Index].Check), Index);
        TestTrue(TEXT("every check carries a wire id"), FCString::Strlen(Checks[Index].Id) > 0);
        TestTrue(TEXT("every check carries an ERR_* code"),
            FCString::Strlen(Checks[Index].Code) > 0);
    }
    // thin_shell needs a threshold and trips on legitimately flat content, so it is the one
    // check that must be asked for.
    TestFalse(TEXT("thin_shell is off by default"),
        MeshAudit::HasCheck(MeshAudit::DefaultCheckMask(), MeshAudit::ECheck::ThinShell));
    TestTrue(TEXT("inverted is on by default"),
        MeshAudit::HasCheck(MeshAudit::DefaultCheckMask(), MeshAudit::ECheck::Inverted));
    return true;
}

// ---------------------------------------------------------------------------------------------
// A mirroring Build Scale is a cause the sweep can name, and an EMPTY mesh must not be the
// cleanest asset in the folder.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditBuildScaleAndEmptyTest,
    "PinWright.Geometry.MeshAudit.MirroredBuildScaleAndEmptyMeshAreBothFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditBuildScaleAndEmptyTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Box = MeshAuditTest_NewBox();

    {
        MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(Box);
        M.BuildScale = FVector(-1.0, 1.0, 1.0);
        TestTrue(TEXT("one negative axis is an odd count, so the build mirrors"),
            M.IsMirroredByBuildScale());

        MeshAudit::FReport R;
        MeshAudit::EvaluateAsset(TEXT("/Game/T/Mir"), TEXT("Mir"), M,
                                 MeshAuditTest_DefaultConfig(), R);
        TestEqual(TEXT("mirrored_build_scale is flagged"),
            MeshAuditTest_Flagged(R, MeshAudit::ECheck::MirroredBuildScale), 1);
    }

    {
        // Two negative axes is a rotation, not a mirror.
        MeshAudit::FAssetMeasurement M = MeshAuditTest_Measure(Box);
        M.BuildScale = FVector(-1.0, -1.0, 1.0);
        TestFalse(TEXT("two negative axes are a rotation, not a mirror"),
            M.IsMirroredByBuildScale());
    }

    {
        // An empty mesh is trivially closed, consistent, non-degenerate and bowtie-free: every
        // other check in the table would call it clean.
        MeshAudit::FAssetMeasurement Empty;
        Empty.bMeasured = true;
        Empty.bHasSourceModel = true;

        MeshAudit::FReport R;
        MeshAudit::EvaluateAsset(TEXT("/Game/T/Void"), TEXT("Void"), Empty,
                                 MeshAuditTest_DefaultConfig(), R);
        TestEqual(TEXT("empty is flagged"),
            MeshAuditTest_Flagged(R, MeshAudit::ECheck::EmptyMesh), 1);
        TestEqual(TEXT("and no other check claims it as clean"),
            R.Tallies[static_cast<int32>(MeshAudit::ECheck::Inverted)].Clean, 0);
        TestFalse(TEXT("an empty mesh cannot pass"), R.DerivePass(TEXT("error")));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// B-mesh-audit-package-path-reads-as-broken-asset. An `assets` entry that is not a content path
// at all is a CALLER error and must be rejected before the sweep runs - it must never enter the
// findings structure, where it would arrive as Unrunnable rows against an asset path and a
// pass:false verdict, i.e. a bad argument wearing a content defect's costume.
//
// The two halves this asserts are the two the caller has to be able to tell apart:
//   * the response is an ERROR, not a success carrying findings;
//   * its code is INVALID_ARGUMENT, not any MESH_AUDIT_* code, because nothing about a mesh was
//     measured or could have been.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditBadAssetArgumentTest,
    "PinWright.Geometry.MeshAudit.MalformedAssetPathIsRejectedNotReportedAsABrokenMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditBadAssetArgumentTest::RunTest(const FString& Parameters)
{
    // Not asset paths at all: a bare name, a folder, and a dot in a folder component.
    const TArray<FString> Malformed = {
        TEXT("SM_Tree_Oak"),
        TEXT("/Game/Props/Meshes/Trees/"),
        TEXT("/Game/Props.Meshes/Trees/SM_X"),
    };

    for (const FString& Entry : Malformed)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Assets;
        Assets.Add(MakeShared<FJsonValueString>(Entry));
        Payload->SetArrayField(TEXT("assets"), Assets);

        FTestResponseCapture Capture;
        TestTrue(TEXT("geometry.audit_static_meshes is registered"),
            InvokeHandlerWithCapture(TEXT("geometry.audit_static_meshes"), Payload, Capture));
        TestFalse(*FString::Printf(TEXT("'%s' does not start a sweep"), *Entry), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("'%s' is INVALID_ARGUMENT"), *Entry),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        // The distinction the ticket is about: a caller error must not borrow the vocabulary of
        // a content finding, or the caller re-authors a mesh that was fine.
        TestFalse(*FString::Printf(TEXT("'%s' is not reported as a mesh defect"), *Entry),
            Capture.ErrorCode.StartsWith(TEXT("MESH_AUDIT")));
        TestTrue(*FString::Printf(TEXT("'%s' is told the accepted forms"), *Entry),
            Capture.Message.Contains(TEXT("/Game/Folder/SM_Name")));
    }
    return true;
}
