// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps.cpp - the scaffolding declared in GeometryOps.h, shared by all five op families.
//
// It lives in its own translation unit rather than in one family's .cpp so that no family owns
// the contract the other four depend on.
#include "Handlers/Geometry/GeometryOps.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"

namespace GeometryOps
{

FOpResult NullMeshFailure()
{
    return FOpResult::Fail(ErrorCodes::ERR_MESH_NOT_FOUND, TEXT("DynamicMesh not available"));
}

void CaptureBefore(UDynamicMesh* Mesh, FOpResult& Result)
{
    // GeometryUtils owns the vertex accessor choice, and the same accessor feeds the
    // vertexCount/triangleCount fields these verbs echo - so a wrapper reporting FOpResult's
    // counts reports byte-identical numbers to the pre-extraction code. Triangles come off
    // UDynamicMesh directly: the MeshQueryFunctions library has no triangle-count accessor.
    Result.VerticesBefore = GeometryUtils::GetMeshVertexCount(Mesh);
    Result.TrianglesBefore = Mesh ? Mesh->GetTriangleCount() : 0;
}

bool BeginOp(UDynamicMesh* Mesh, FOpResult& Result)
{
    if (!Mesh)
    {
        Result = NullMeshFailure();
        return false;
    }

    CaptureBefore(Mesh, Result);
    return true;
}

void FinishOp(UDynamicMesh* Mesh, FOpResult& Result, bool bForceChanged)
{
    Result.bSuccess = true;
    Result.VerticesAfter = GeometryUtils::GetMeshVertexCount(Mesh);
    Result.TrianglesAfter = Mesh ? Mesh->GetTriangleCount() : 0;
    Result.bChanged = bForceChanged
        || (Result.VerticesAfter != Result.VerticesBefore)
        || (Result.TrianglesAfter != Result.TrianglesBefore);
}

bool PrepareHoleFillAttributes(UDynamicMesh* Mesh, const TCHAR* OpName, FOpResult& Result,
                               bool& bAttributesChanged)
{
    bAttributesChanged = false;
    if (!Mesh)
    {
        FOpResult::FailIn(Result, ErrorCodes::ERR_MESH_NOT_FOUND,
            TEXT("DynamicMesh not available"));
        return false;
    }

    // FHoleFillOp skips its normal/UV writes entirely when attributes are disabled. Preserve
    // that valid representation instead of adding an attribute set only for the fill.
    if (!Mesh->GetMeshRef().HasAttributes())
    {
        return true;
    }

    // With no boundary edge there is no loop and therefore no new triangle for the engine to
    // decorate. Do not turn a closed-mesh no-op into an attribute edit.
    bool bHasBoundaryEdge = false;
    for (const int32 EdgeID : Mesh->GetMeshRef().BoundaryEdgeIndicesItr())
    {
        (void)EdgeID;
        bHasBoundaryEdge = true;
        break;
    }
    if (!bHasBoundaryEdge)
    {
        return true;
    }

    const UE::Geometry::FDynamicMeshAttributeSet* AttributesBefore =
        Mesh->GetMeshRef().Attributes();
    const bool bHadUVLayer = AttributesBefore->NumUVLayers() > 0
        && AttributesBefore->PrimaryUV() != nullptr;
    const bool bHadNormalLayer = AttributesBefore->NumNormalLayers() > 0
        && AttributesBefore->PrimaryNormals() != nullptr;

    if (!GeometryUtils::EnsureMeshHasUVChannel(Mesh, 0))
    {
        FOpResult::FailIn(Result, ErrorCodes::ERR_NO_UV_ELEMENTS, FString::Printf(
            TEXT("%s could not create UV channel 0 required by the engine hole filler"), OpName));
        return false;
    }

    if (Mesh->GetMeshRef().Attributes()->PrimaryNormals() == nullptr)
    {
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            if (EditMesh.HasAttributes() && EditMesh.Attributes()->PrimaryNormals() == nullptr)
            {
                EditMesh.Attributes()->SetNumNormalLayers(1);
            }
        }, EDynamicMeshChangeType::AttributeEdit,
        EDynamicMeshAttributeChangeFlags::NormalsTangents, /*bDeferChangeEvents=*/false);
    }

    const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh->GetMeshRef().Attributes();
    if (!Attributes || Attributes->NumUVLayers() == 0 || Attributes->PrimaryUV() == nullptr)
    {
        FOpResult::FailIn(Result, ErrorCodes::ERR_NO_UV_ELEMENTS, FString::Printf(
            TEXT("%s could not create UV channel 0 required by the engine hole filler"), OpName));
        return false;
    }
    if (Attributes->NumNormalLayers() == 0 || Attributes->PrimaryNormals() == nullptr)
    {
        FOpResult::FailIn(Result, ErrorCodes::ERR_INVALID_NORMAL_OVERLAY, FString::Printf(
            TEXT("%s could not create the primary normal layer required by the engine hole filler"),
            OpName));
        return false;
    }

    bAttributesChanged = !bHadUVLayer || !bHadNormalLayer;
    return true;
}

double ClampDimensionWarn(double Value, const TCHAR* Label, FOpResult& Result)
{
    const double Clamped = GeometryUtils::ClampDimension(Value);
    if (Clamped != Value)
    {
        Result.Warnings.Add(FString::Printf(TEXT("%s clamped from %s to %s"),
            Label, *FString::SanitizeFloat(Value), *FString::SanitizeFloat(Clamped)));
    }
    return Clamped;
}

int32 ClampSegmentsWarn(int32 Value, int32 Default, const TCHAR* Label, FOpResult& Result)
{
    const int32 Clamped = GeometryUtils::ClampSegments(Value, Default);
    if (Clamped != Value)
    {
        Result.Warnings.Add(FString::Printf(TEXT("%s clamped from %d to %d"), Label, Value, Clamped));
    }
    return Clamped;
}

int32 ClampCountWarn(int32 Value, int32 Default, int32 Max, const TCHAR* Label, FOpResult& Result)
{
    // Deliberately the same expression GeometryUtils::ClampSegments uses, with Max supplied
    // instead of fixed at GEOM_MAX_SEGMENTS, and deliberately the same warning text as
    // ClampSegmentsWarn above - a count that moves off the segment ceiling must not also change
    // the wording a caller greps for.
    const int32 Clamped = FMath::Clamp(Value <= 0 ? Default : Value, 1, Max);
    if (Clamped != Value)
    {
        Result.Warnings.Add(FString::Printf(TEXT("%s clamped from %d to %d"), Label, Value, Clamped));
    }
    return Clamped;
}

double ClampFactorWarn(double Value, const TCHAR* Label, FOpResult& Result)
{
    const double Clamped = FMath::Clamp(Value, 0.0, 1.0);
    if (Clamped != Value)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%s clamped from %g to %g (valid range 0-1)"), Label, Value, Clamped));
    }
    return Clamped;
}

int32 ClampRangeWarn(int32 Value, int32 Min, int32 Max, const TCHAR* Label, FOpResult& Result)
{
    const int32 Clamped = FMath::Clamp(Value, Min, Max);
    if (Clamped != Value)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("%s clamped from %d to %d (valid range %d-%d)"), Label, Value, Clamped, Min, Max));
    }
    return Clamped;
}

bool ParseColorChannels(const FString& Spec, EColorChannels& OutChannels, FString& OutError)
{
    if (Spec.IsEmpty())
    {
        OutError = TEXT("channel mask is empty; write any combination of r, g, b and a (e.g. \"a\", \"rgb\", \"rgba\")");
        return false;
    }

    EColorChannels Parsed = EColorChannels::None;
    for (const TCHAR Ch : Spec)
    {
        EColorChannels Bit = EColorChannels::None;
        switch (FChar::ToLower(Ch))
        {
        case TEXT('r'): Bit = EColorChannels::R; break;
        case TEXT('g'): Bit = EColorChannels::G; break;
        case TEXT('b'): Bit = EColorChannels::B; break;
        case TEXT('a'): Bit = EColorChannels::A; break;
        default:
            OutError = FString::Printf(
                TEXT("channel mask '%s' contains '%c', which is not one of r, g, b, a"), *Spec, Ch);
            return false;
        }

        // A repeat is refused rather than absorbed: "rr" is a typo for something, and silently
        // reading it as "r" hides which channel the caller believed they were naming.
        if (EnumHasAnyFlags(Parsed, Bit))
        {
            OutError = FString::Printf(
                TEXT("channel mask '%s' names '%c' more than once"), *Spec, Ch);
            return false;
        }
        Parsed |= Bit;
    }

    OutChannels = Parsed;
    return true;
}

FString ColorChannelsToString(EColorChannels Channels)
{
    FString Out;
    if (EnumHasAnyFlags(Channels, EColorChannels::R)) Out += TEXT("r");
    if (EnumHasAnyFlags(Channels, EColorChannels::G)) Out += TEXT("g");
    if (EnumHasAnyFlags(Channels, EColorChannels::B)) Out += TEXT("b");
    if (EnumHasAnyFlags(Channels, EColorChannels::A)) Out += TEXT("a");
    return Out;
}

FVector4f ApplyColorChannels(const FVector4f& Existing, const FVector4f& Incoming, EColorChannels Channels)
{
    FVector4f Out = Existing;
    if (EnumHasAnyFlags(Channels, EColorChannels::R)) Out.X = Incoming.X;
    if (EnumHasAnyFlags(Channels, EColorChannels::G)) Out.Y = Incoming.Y;
    if (EnumHasAnyFlags(Channels, EColorChannels::B)) Out.Z = Incoming.Z;
    if (EnumHasAnyFlags(Channels, EColorChannels::A)) Out.W = Incoming.W;
    return Out;
}

} // namespace GeometryOps
