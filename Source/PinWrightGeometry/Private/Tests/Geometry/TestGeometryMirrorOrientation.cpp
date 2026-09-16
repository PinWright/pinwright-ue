// Copyright (c) 2026 Alexander Penkin. MIT License.

// Orientation regression net for GeometryOps::Mirror, plus the `transform` control it has to
// stay consistent with. Separate from TestGeometryOpsBoolean.cpp because that file covers the
// verb's CONTRACT (counts, bChanged, axis fallback, null handling) and every one of its
// assertions passed while the geometry was inside out.
//
// THE DEFECT. Mirror clones the mesh, negates one coordinate on the clone, and appends the
// clone back. A reflection has determinant -1, so negating one axis turns every triangle of the
// clone inside out; unless the winding is reversed as well, the appended half is a back-faced
// shell. It renders black under backface culling, and inside a boolean it subtracts an
// anti-solid instead of a solid. Mirror reversed the winding only under
// `#if (UE_VERSION >= 5.4 && UE_VERSION < 5.5)` and hand-rolled the vertex negation with no
// reversal on every other version - so on 5.8, the only version this plugin builds, the branch
// that ran was the broken one.
//
// WHY THESE TESTS MEASURE VOLUME AND NOT NORMALS. The obvious check - does each triangle's
// stored normal agree with its winding? - PASSES on the broken mesh. Worse, a
// recalculate_normals after the mirror makes it pass BY CONSTRUCTION: recomputing normals from
// the inverted winding cements the inversion into the attribute set and leaves the mesh
// perfectly self-consistent and still inside out. That is why this shipped, and it is why no
// assertion here looks at a normal. Signed volume reads the winding itself:
//
//     V = sum over triangles of dot(A, cross(B, C)) / 6
//
// one sign for an outward-wound closed shell and the other for an inward-wound one. Two halves
// of opposite handedness therefore CANCEL EXACTLY, which is the 0.00 the defect measured.
//
// The probe is the .pwmodel one-liner the defect was found on:
//
//     part p { torus major_radius=10 minor_radius=4 at=(14,0,0)  mirror axis=x }
//
// Nothing below asserts a particular SIGN. The sign is a convention of the generator and the
// handedness of the space; what these tests assert is AGREEMENT - between the two halves, and
// between a half and the unmirrored source - so they stay true if a generator's winding
// convention ever flips wholesale.
//
// GeometryOps::Stretch carried the identical dead guard around the identical ScaleMesh call
// and was fixed in the same pass; its regressions live in TestGeometryStretchOrientation.cpp,
// which is the file docs/engine-version-support.md cites. Deliberately not duplicated here.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Selections/MeshConnectedComponents.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "GeometryScript/MeshTransformFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// The probe's numbers. The inner equator lands exactly on x = 0 (14 - 10 - 4), so the mirrored
// half meets this one at the origin - which is what lets a WELDED mirror fuse the two halves
// into a single connected component. Any check that assumes the halves stay separately
// countable is therefore blind on the shipped default, which is why the between-shell test
// below turns the weld off and the total-volume test leaves it on.
constexpr double MirrorOrientTest_MajorRadius = 10.0;
constexpr double MirrorOrientTest_MinorRadius = 4.0;

UDynamicMesh* MirrorOrientTest_NewTorus()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());

    GeometryOps::FTorusParams Params;
    Params.MajorRadius = MirrorOrientTest_MajorRadius;
    Params.MinorRadius = MirrorOrientTest_MinorRadius;
    // Denser than the 16x8 default so the shell is unambiguously closed. Every assertion here is
    // a RATIO between meshes of identical tessellation, so the step count cannot move a result
    // either way; it only makes the figures easier to read in a failure message.
    Params.MajorSteps = 32;
    Params.MinorSteps = 16;

    GeometryOps::GenerateTorus(Mesh, Params, FTransform(FVector(14.0, 0.0, 0.0)));
    return Mesh;
}

// The signed volume of the tetrahedron each triangle forms with the origin, summed. Positive for
// one winding and negative for the other, so the SIGN is the orientation - and two oppositely
// wound copies of the same shell sum to zero no matter where either sits in space.
double MirrorOrientTest_SignedVolume(
    const UE::Geometry::FDynamicMesh3& Mesh, const TArray<int32>* Triangles = nullptr)
{
    double Volume = 0.0;
    auto Accumulate = [&Mesh, &Volume](int32 TID)
    {
        FVector3d A, B, C;
        Mesh.GetTriVertices(TID, A, B, C);
        Volume += A.Dot(B.Cross(C));
    };

    if (Triangles)
    {
        for (int32 TID : *Triangles)
        {
            Accumulate(TID);
        }
    }
    else
    {
        for (int32 TID : Mesh.TriangleIndicesItr())
        {
            Accumulate(TID);
        }
    }
    return Volume / 6.0;
}

// Signed volume PER CONNECTED COMPONENT. This is the only form of the measurement that can state
// the fault as "these two shells disagree with each other" rather than "the total happened to
// come out at zero", and it is the form that survives a probe whose halves do not cancel exactly.
void MirrorOrientTest_ComponentVolumes(
    const UE::Geometry::FDynamicMesh3& Mesh, TArray<double>& OutVolumes)
{
    UE::Geometry::FMeshConnectedComponents Components(&Mesh);
    Components.FindConnectedTriangles();

    OutVolumes.Reset();
    for (int32 Index = 0; Index < Components.Num(); ++Index)
    {
        OutVolumes.Add(MirrorOrientTest_SignedVolume(Mesh, &Components.GetComponent(Index).Indices));
    }
}
}

// ============================================================================
// Mirror - between-shell orientation agreement
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMirrorHalvesAgreeTest,
    "PinWright.Geometry.Ops.Boolean.MirrorHalvesAgreeOnOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryMirrorHalvesAgreeTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = MirrorOrientTest_NewTorus();

    const double BareVolume = MirrorOrientTest_SignedVolume(Mesh->GetMeshRef());
    TestTrue(TEXT("the bare torus encloses a non-zero signed volume"), FMath::Abs(BareVolume) > 1.0);

    // Weld OFF deliberately. The two halves touch at the origin, so the shipped weld=true can
    // fuse them into one component - and then there are no two shells left to compare and the
    // disagreement can only be seen as a cancelling total. Unwelded, the fault has to be stated
    // as what it actually is: two shells of opposite handedness in one mesh.
    GeometryOps::FMirrorParams Params;
    Params.Axis = GeometryOps::EMeshAxis::X;
    Params.bWeld = false;

    const GeometryOps::FOpResult Op = GeometryOps::Mirror(Mesh, Params);
    TestTrue(TEXT("mirror succeeds"), Op.bSuccess);

    TArray<double> Volumes;
    MirrorOrientTest_ComponentVolumes(Mesh->GetMeshRef(), Volumes);

    TestEqual(TEXT("an unwelded mirror leaves exactly two shells"), Volumes.Num(), 2);
    if (Volumes.Num() != 2)
    {
        return false;
    }

    // THE ASSERTION. A correct mirror reflects the positions AND reverses the winding: the
    // reflection alone negates the signed volume (the reflection matrix has determinant -1) and
    // the winding reversal negates it back, so the mirrored half must come out at the SAME
    // signed volume as the source half - not merely the same magnitude.
    //
    // On the broken mirror these are equal and OPPOSITE, and every other observable property of
    // the mesh is byte-identical between the two cases: same triangle count, same vertex count,
    // same bounds, same normal-vs-winding consistency.
    const double Tolerance = FMath::Abs(BareVolume) * 1e-6;
    TestEqual(TEXT("the first shell matches the unmirrored torus"), Volumes[0], BareVolume, Tolerance);
    TestEqual(TEXT("the mirrored shell matches the unmirrored torus"), Volumes[1], BareVolume, Tolerance);

    // Stated a second way, independent of which shell the component search reported first, and
    // of the generator's winding convention: same sign means same handedness.
    TestTrue(TEXT("both shells are wound the same way round"), Volumes[0] * Volumes[1] > 0.0);

    return true;
}

// ============================================================================
// Mirror - the welded default, where the halves cancel into one component
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMirrorWeldedVolumeTest,
    "PinWright.Geometry.Ops.Boolean.MirrorWeldedVolumeDoesNotCancel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryMirrorWeldedVolumeTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = MirrorOrientTest_NewTorus();

    const double BareVolume = MirrorOrientTest_SignedVolume(Mesh->GetMeshRef());
    TestTrue(TEXT("the bare torus encloses a non-zero signed volume"), FMath::Abs(BareVolume) > 1.0);

    // Defaults: axis X, weld ON. This is the exact configuration the .pwmodel probe compiles to
    // and the one that shipped, so it is the one the regression has to cover directly rather
    // than by analogy with the unwelded case above.
    GeometryOps::FMirrorParams Params;
    const GeometryOps::FOpResult Op = GeometryOps::Mirror(Mesh, Params);
    TestTrue(TEXT("mirror succeeds"), Op.bSuccess);

    const double MirroredVolume = MirrorOrientTest_SignedVolume(Mesh->GetMeshRef());

    // The measured signature of the defect: EXACTLY 0.00, because the two halves are exact
    // reflections and their contributions cancel term for term.
    TestTrue(TEXT("a mirrored torus does not measure zero enclosed volume"),
        FMath::Abs(MirroredVolume) > FMath::Abs(BareVolume));

    // A mirror is a mirror-AND-MERGE, so the output is both halves and encloses twice the
    // source. The looser tolerance than the unwelded test is for the weld itself, which may
    // merge the coincident ring where the two halves meet at the origin.
    TestEqual(TEXT("a mirrored torus encloses twice the source volume"),
        MirroredVolume, 2.0 * BareVolume, FMath::Abs(BareVolume) * 1e-3);

    return true;
}

// ============================================================================
// Control - the `transform` path that was already correct
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNegativeScaleOrientationTest,
    "PinWright.Geometry.Ops.Boolean.NegativeScaleTransformPreservesOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryNegativeScaleOrientationTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = MirrorOrientTest_NewTorus();

    const double Before = MirrorOrientTest_SignedVolume(Mesh->GetMeshRef());
    TestTrue(TEXT("the bare torus encloses a non-zero signed volume"), FMath::Abs(Before) > 1.0);

    // EXACTLY the call the .pwmodel `transform` op makes, bFixOrientationForNegativeScale
    // included (PwModelCompiler.cpp). This path was ALREADY correct while mirror was not, and it
    // is correct precisely BECAUSE of that flag: MeshTransforms::ApplyTransform reverses the
    // winding when the transform's determinant is negative. The flag is not a workaround for a
    // broken engine path - it is how the engine path is asked to do the reversal at all.
    //
    // This test exists so that a "fix" which removes or inverts that flag - or which reverses
    // the winding a second time somewhere upstream - fails HERE rather than shipping a
    // regression on the one negative-scale path that always worked.
    UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(
        Mesh,
        FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(-1.0, 1.0, 1.0)),
        /*bFixOrientationForNegativeScale=*/ true,
        nullptr);

    const double After = MirrorOrientTest_SignedVolume(Mesh->GetMeshRef());

    TestEqual(TEXT("a negative-scale transform preserves the signed volume"),
        After, Before, FMath::Abs(Before) * 1e-6);

    // Belt and braces on the sign, so a change that scaled the volume to zero cannot pass the
    // tolerance check above by collapsing both sides.
    TestTrue(TEXT("a negative-scale transform does not invert the shell"), After * Before > 0.0);

    return true;
}
