// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-set-vertex-color-no-channel-mask.
//
// set_vertex_color used to write all four components as one element, so whoever coloured a mesh
// first owned R, G, B and A: the standard two-signal layout - RGB a per-part tint, A a mask - was
// not authorable at all, because writing alpha meant re-sending an RGB the caller does not know
// per vertex. The `channels` mask makes the second signal reachable, and the property that has to
// hold is the one this file measures: a masked write leaves the unnamed components at the value
// they already carried.
//
// Counterfactual: revert the mask (write Color whole in GeometryOps::SetVertexColor) and the
// second write below sets R/G/B to 0.9, so the "the tint survives an alpha-only write"
// assertions fail while the op still reports the same verticesModified success.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU when Unity merges them.
UDynamicMesh* VertexColorMaskTest_NewBoxMesh()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// Every colour element must equal Expected. Returns the first element that does not, or
// INDEX_NONE, plus what it actually held - so a failure names a value rather than only a count.
int32 VertexColorMaskTest_FindMismatch(
    const UE::Geometry::FDynamicMesh3& Mesh, const FVector4f& Expected, FVector4f& OutActual)
{
    const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay =
        Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
    if (ColorOverlay == nullptr)
    {
        return INDEX_NONE;
    }

    for (int32 ElementID : ColorOverlay->ElementIndicesItr())
    {
        const FVector4f Actual = ColorOverlay->GetElement(ElementID);
        if (!Actual.Equals(Expected, KINDA_SMALL_NUMBER))
        {
            OutActual = Actual;
            return ElementID;
        }
    }
    return INDEX_NONE;
}
}

// ============================================================================
// The mask leaves the channels it does not name alone
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySetVertexColorChannelMaskTest,
    "PinWright.geometry.set_vertex_color.ChannelMaskLeavesUnmaskedChannelsUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySetVertexColorChannelMaskTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(VertexColorMaskTest_NewBoxMesh());

    // 1. Lay down a tint through the default (unmasked) path. This is the existing contract and
    // must be unchanged by the mask's arrival.
    {
        GeometryOps::FSetVertexColorParams Params;
        Params.bSetAll = true;
        Params.Color = FLinearColor(0.2f, 0.4f, 0.6f, 1.0f);

        int32 VerticesModified = 0;
        const GeometryOps::FOpResult Op =
            GeometryOps::SetVertexColor(Mesh.Get(), Params, VerticesModified);
        TestTrue(TEXT("the unmasked tint write succeeds"), Op.bSuccess);
        TestTrue(TEXT("the unmasked tint write reaches vertices"), VerticesModified > 0);

        FVector4f Actual = FVector4f::Zero();
        const int32 Mismatch = VertexColorMaskTest_FindMismatch(
            Mesh->GetMeshRef(), FVector4f(0.2f, 0.4f, 0.6f, 1.0f), Actual);
        TestEqual(TEXT("an unmasked write still lands on every component of every element"),
            Mismatch, INDEX_NONE);
    }

    // 2. Write ONLY alpha, with an RGB deliberately different from the tint. If the mask is not
    // honoured, the 0.9s below overwrite the ladder - which is exactly the defect.
    {
        GeometryOps::FSetVertexColorParams Params;
        Params.bSetAll = true;
        Params.Color = FLinearColor(0.9f, 0.9f, 0.9f, 0.25f);
        Params.Channels = GeometryOps::EColorChannels::A;

        int32 VerticesModified = 0;
        const GeometryOps::FOpResult Op =
            GeometryOps::SetVertexColor(Mesh.Get(), Params, VerticesModified);
        TestTrue(TEXT("the alpha-only write succeeds"), Op.bSuccess);
        TestTrue(TEXT("the alpha-only write reaches vertices"), VerticesModified > 0);

        FVector4f Actual = FVector4f::Zero();
        const int32 Mismatch = VertexColorMaskTest_FindMismatch(
            Mesh->GetMeshRef(), FVector4f(0.2f, 0.4f, 0.6f, 0.25f), Actual);
        TestEqual(
            FString::Printf(TEXT("alpha moved to 0.25 and the RGB tint survived (first offender held %.3f, %.3f, %.3f, %.3f)"),
                Actual.X, Actual.Y, Actual.Z, Actual.W),
            Mismatch, INDEX_NONE);
    }

    // 3. The single-vertex path takes the mask too, and an empty mask is refused rather than
    // silently widened - a write with nowhere to land is not a success.
    {
        GeometryOps::FSetVertexColorParams Params;
        Params.VertexIndex = 0;
        Params.Color = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        Params.Channels = GeometryOps::EColorChannels::None;

        int32 VerticesModified = 0;
        const GeometryOps::FOpResult Op =
            GeometryOps::SetVertexColor(Mesh.Get(), Params, VerticesModified);
        TestFalse(TEXT("a write naming no channel is refused"), Op.bSuccess);
        TestEqual(TEXT("a write naming no channel reports INVALID_ARGUMENT"),
            Op.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

        FVector4f Actual = FVector4f::Zero();
        const int32 Mismatch = VertexColorMaskTest_FindMismatch(
            Mesh->GetMeshRef(), FVector4f(0.2f, 0.4f, 0.6f, 0.25f), Actual);
        TestEqual(TEXT("the refused write changed nothing"), Mismatch, INDEX_NONE);
    }

    return true;
}

// ============================================================================
// The spelling both front-ends compile against
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryColorChannelSpellingTest,
    "PinWright.Geometry.Ops.ColorChannels.SpellingIsOrderFreeAndRefusesNonsense",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryColorChannelSpellingTest::RunTest(const FString& Parameters)
{
    auto Accepts = [this](const TCHAR* Spec, GeometryOps::EColorChannels Expected)
    {
        GeometryOps::EColorChannels Parsed = GeometryOps::EColorChannels::None;
        FString Error;
        const bool bOk = GeometryOps::ParseColorChannels(Spec, Parsed, Error);
        TestTrue(FString::Printf(TEXT("'%s' is accepted (%s)"), Spec, *Error), bOk);
        TestTrue(FString::Printf(TEXT("'%s' parses to the expected mask"), Spec), Parsed == Expected);
    };

    auto Refuses = [this](const TCHAR* Spec)
    {
        GeometryOps::EColorChannels Parsed = GeometryOps::EColorChannels::All;
        FString Error;
        const bool bOk = GeometryOps::ParseColorChannels(Spec, Parsed, Error);
        TestFalse(FString::Printf(TEXT("'%s' is refused"), Spec), bOk);
        TestFalse(FString::Printf(TEXT("'%s' explains why"), Spec), Error.IsEmpty());
        // The out-parameter must be untouched on refusal, so a caller that ignores the bool
        // cannot end up writing a mask this function never produced.
        TestTrue(FString::Printf(TEXT("'%s' leaves the out mask untouched"), Spec),
            Parsed == GeometryOps::EColorChannels::All);
    };

    Accepts(TEXT("rgba"), GeometryOps::EColorChannels::All);
    Accepts(TEXT("a"), GeometryOps::EColorChannels::A);
    Accepts(TEXT("RGB"), GeometryOps::EColorChannels::R | GeometryOps::EColorChannels::G |
                         GeometryOps::EColorChannels::B);
    Accepts(TEXT("ar"), GeometryOps::EColorChannels::A | GeometryOps::EColorChannels::R);

    Refuses(TEXT(""));
    Refuses(TEXT("x"));
    Refuses(TEXT("rr"));
    Refuses(TEXT("rgbaa"));

    // The canonical echo is always r,g,b,a-ordered whatever order it was written in, so a caller
    // comparing responses is not comparing spellings.
    TestEqual(TEXT("the echo re-orders a mask canonically"),
        GeometryOps::ColorChannelsToString(
            GeometryOps::EColorChannels::A | GeometryOps::EColorChannels::R),
        FString(TEXT("ra")));

    return true;
}
