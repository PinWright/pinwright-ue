// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshIOHandler.cpp - Mesh import/export (OBJ/STL) round-trip for DynamicMeshActors.
//
// Lets an AI read a working DynamicMesh out as LLM-editable OBJ text (or STL),
// edit the text, and re-import it into a fresh DynamicMeshActor. Emits/parses the
// text directly from the FDynamicMesh3 vertex/triangle buffers - no StaticMesh
// bake, no Interchange round-trip.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryScriptDebugSink.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Utils/AtomicFileWriter.h"
#include "Utils/PathUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"
#include "Editor.h"
#include "Engine/World.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"

// File-local OBJ/STL parse and serialize helpers, uniquely named so a Unity build merging the
// geometry handlers' anonymous namespaces into one translation unit cannot hit an ODR
// redefinition (see GeometryTarget.h).
namespace
{
    // Replace filesystem-hostile characters in an actor label used as a default filename.
    FString SanitizeMeshIOFileToken(const FString& In)
    {
        if (In.IsEmpty())
        {
            return TEXT("mesh");
        }
        FString Out;
        Out.Reserve(In.Len());
        const TCHAR* const Invalid = TEXT("\\/:*?\"<>| ");
        for (int32 i = 0; i < In.Len(); ++i)
        {
            const TCHAR Ch = In[i];
            bool bBad = FChar::IsControl(Ch);
            for (const TCHAR* P = Invalid; !bBad && *P != TEXT('\0'); ++P)
            {
                bBad = (*P == Ch);
            }
            Out.AppendChar(bBad ? TEXT('_') : Ch);
        }
        return Out;
    }

    // Resolve the on-disk output path for an export. When FilePathParam is set it is
    // sanitized as a project-relative file path (Windows-absolute / traversal paths are
    // rejected); when omitted the mesh is written under Saved/PinWright/Meshes/<actor>.<ext>.
    // Returns false (error already sent) on a rejected filePath.
    bool ResolveMeshIOExportPath(const FHandlerContext& Ctx, const FString& FilePathParam,
        const FString& ActorName, const TCHAR* Extension, FString& OutFullPath)
    {
        if (!FilePathParam.IsEmpty())
        {
            FString Safe = SanitizeProjectFilePath(FilePathParam);
            if (Safe.IsEmpty())
            {
                Ctx.SendError(TEXT("SECURITY_VIOLATION"),
                    FString::Printf(TEXT("Invalid or unsafe filePath: %s. Path must be project-relative (e.g. /Saved/PinWright/Meshes/mesh.%s)"),
                        *FilePathParam, Extension));
                return false;
            }
            Safe.RemoveFromStart(TEXT("/"));
            OutFullPath = FPaths::ProjectDir() / Safe;
        }
        else
        {
            const FString FileName = FString::Printf(TEXT("%s.%s"), *SanitizeMeshIOFileToken(ActorName), Extension);
            OutFullPath = FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("Meshes") / FileName;
        }
        OutFullPath = FPaths::ConvertRelativePathToFull(OutFullPath);
        return true;
    }

    // Resolve + read an import source file into raw bytes. Returns false (error already
    // sent) on a rejected path or a missing/unreadable file.
    bool LoadMeshIOImportFile(const FHandlerContext& Ctx, const FString& FilePathParam, TArray<uint8>& OutBytes)
    {
        FString Safe = SanitizeProjectFilePath(FilePathParam);
        if (Safe.IsEmpty())
        {
            Ctx.SendError(TEXT("SECURITY_VIOLATION"),
                FString::Printf(TEXT("Invalid or unsafe filePath: %s. Path must be project-relative."), *FilePathParam));
            return false;
        }
        Safe.RemoveFromStart(TEXT("/"));
        const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Safe);
        if (!FFileHelper::LoadFileToArray(OutBytes, *FullPath))
        {
            Ctx.SendError(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("File not found or unreadable: %s"), *FullPath));
            return false;
        }
        return true;
    }

    // ---- OBJ serialization -----------------------------------------------------
    // Emit `v x y z` for each (compacted) vertex, plus per-vertex `vt`/`vn` when the
    // mesh carries a UV0 / primary-normal overlay, then `f` faces. Vertex IDs in a
    // FDynamicMesh3 can be sparse after deletes, so a compaction map assigns dense
    // 1-based OBJ indices in vertex-iteration order. v / vt / vn share that single
    // per-vertex index, which round-trips cleanly.
    FString MeshIOBuildObjText(const UE::Geometry::FDynamicMesh3& Mesh)
    {
        const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = nullptr;
        const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = nullptr;
        if (Mesh.HasAttributes())
        {
            NormalOverlay = Mesh.Attributes()->PrimaryNormals();
            if (Mesh.Attributes()->NumUVLayers() > 0)
            {
                UVOverlay = Mesh.Attributes()->GetUVLayer(0);
            }
        }
        const bool bHasNormals = NormalOverlay != nullptr && NormalOverlay->ElementCount() > 0;
        const bool bHasUVs = UVOverlay != nullptr && UVOverlay->ElementCount() > 0;

        FString Obj;
        Obj.Reserve(Mesh.VertexCount() * 28 + Mesh.TriangleCount() * 24 + 128);
        Obj += TEXT("# Exported by PinWright geometry.export_obj\n");

        // Compact sparse vertex ID -> dense 1-based OBJ index (emission order).
        TMap<int32, int32> VidToIndex;
        VidToIndex.Reserve(Mesh.VertexCount());
        int32 NextIndex = 1;
        for (int32 Vid : Mesh.VertexIndicesItr())
        {
            VidToIndex.Add(Vid, NextIndex++);
            const FVector3d P = Mesh.GetVertex(Vid);
            Obj += FString::Printf(TEXT("v %.6f %.6f %.6f\n"), P.X, P.Y, P.Z);
        }

        if (bHasUVs)
        {
            for (int32 Vid : Mesh.VertexIndicesItr())
            {
                FVector2f UV(0.0f, 0.0f);
                TArray<int32> Elems;
                UVOverlay->GetVertexElements(Vid, Elems);
                if (Elems.Num() > 0)
                {
                    UV = UVOverlay->GetElement(Elems[0]);
                }
                Obj += FString::Printf(TEXT("vt %.6f %.6f\n"), UV.X, UV.Y);
            }
        }

        if (bHasNormals)
        {
            for (int32 Vid : Mesh.VertexIndicesItr())
            {
                FVector3f N(0.0f, 0.0f, 1.0f);
                TArray<int32> Elems;
                NormalOverlay->GetVertexElements(Vid, Elems);
                if (Elems.Num() > 0)
                {
                    N = NormalOverlay->GetElement(Elems[0]);
                }
                Obj += FString::Printf(TEXT("vn %.6f %.6f %.6f\n"), N.X, N.Y, N.Z);
            }
        }

        for (int32 Tid : Mesh.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(Tid);
            const int32 A = VidToIndex[Tri.A];
            const int32 B = VidToIndex[Tri.B];
            const int32 C = VidToIndex[Tri.C];
            if (bHasUVs && bHasNormals)
            {
                Obj += FString::Printf(TEXT("f %d/%d/%d %d/%d/%d %d/%d/%d\n"), A, A, A, B, B, B, C, C, C);
            }
            else if (bHasNormals)
            {
                Obj += FString::Printf(TEXT("f %d//%d %d//%d %d//%d\n"), A, A, B, B, C, C);
            }
            else if (bHasUVs)
            {
                Obj += FString::Printf(TEXT("f %d/%d %d/%d %d/%d\n"), A, A, B, B, C, C);
            }
            else
            {
                Obj += FString::Printf(TEXT("f %d %d %d\n"), A, B, C);
            }
        }

        return Obj;
    }

    // ---- OBJ parsing -----------------------------------------------------------
    // Parse `v`/`f` (and skip `vt`/`vn`/`o`/`g`/`s`/`usemtl`/`mtllib`). Faces accept
    // `f v`, `f v/vt`, `f v/vt/vn`, `f v//vn`; only the position index is used to
    // build the buffer. Polygons with >3 vertices are fan-triangulated. 1-based and
    // negative (relative) position indices are both resolved. Returns false when the
    // text yields no vertices or no triangles (malformed / not an OBJ).
    bool MeshIOParseObjText(const FString& Text, TArray<FVector>& OutVertices, TArray<FIntVector>& OutTriangles)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);

        TArray<FVector> Positions;
        TArray<FIntVector> Triangles;
        for (const FString& RawLine : Lines)
        {
            const FString Line = RawLine.TrimStartAndEnd();
            if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
            {
                continue;
            }
            TArray<FString> Tok;
            Line.ParseIntoArrayWS(Tok);
            if (Tok.Num() == 0)
            {
                continue;
            }

            if (Tok[0] == TEXT("v") && Tok.Num() >= 4)
            {
                Positions.Add(FVector(
                    FCString::Atod(*Tok[1]), FCString::Atod(*Tok[2]), FCString::Atod(*Tok[3])));
            }
            else if (Tok[0] == TEXT("f") && Tok.Num() >= 4)
            {
                TArray<int32> FaceIdx;
                for (int32 k = 1; k < Tok.Num(); ++k)
                {
                    FString VStr = Tok[k];
                    int32 Slash = INDEX_NONE;
                    if (VStr.FindChar(TEXT('/'), Slash))
                    {
                        VStr.LeftInline(Slash);
                    }
                    if (VStr.IsEmpty())
                    {
                        FaceIdx.Reset();
                        break;
                    }
                    const int32 Idx = FCString::Atoi(*VStr);
                    int32 Zero;
                    if (Idx > 0)
                    {
                        Zero = Idx - 1;
                    }
                    else if (Idx < 0)
                    {
                        Zero = Positions.Num() + Idx; // relative to current vertex count
                    }
                    else
                    {
                        FaceIdx.Reset();
                        break; // 0 is not a valid OBJ index
                    }
                    if (Zero < 0 || Zero >= Positions.Num())
                    {
                        FaceIdx.Reset();
                        break;
                    }
                    FaceIdx.Add(Zero);
                }
                for (int32 k = 1; k + 1 < FaceIdx.Num(); ++k)
                {
                    Triangles.Add(FIntVector(FaceIdx[0], FaceIdx[k], FaceIdx[k + 1]));
                }
            }
            // vt / vn / o / g / s / usemtl / mtllib deliberately ignored.
        }

        if (Positions.Num() == 0 || Triangles.Num() == 0)
        {
            return false;
        }
        OutVertices = MoveTemp(Positions);
        OutTriangles = MoveTemp(Triangles);
        return true;
    }

    // ---- STL serialization -----------------------------------------------------
    // Per-face normal from the CCW winding of the triangle's three vertices.
    FVector3d MeshIOFaceNormal(const FVector3d& A, const FVector3d& B, const FVector3d& C)
    {
        return ((B - A) ^ (C - A)).GetSafeNormal();
    }

    FString MeshIOBuildStlAscii(const UE::Geometry::FDynamicMesh3& Mesh, const FString& SolidName)
    {
        FString Stl;
        Stl.Reserve(Mesh.TriangleCount() * 180 + 64);
        Stl += FString::Printf(TEXT("solid %s\n"), *SolidName);
        for (int32 Tid : Mesh.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(Tid);
            const FVector3d A = Mesh.GetVertex(Tri.A);
            const FVector3d B = Mesh.GetVertex(Tri.B);
            const FVector3d C = Mesh.GetVertex(Tri.C);
            const FVector3d N = MeshIOFaceNormal(A, B, C);
            Stl += FString::Printf(TEXT("  facet normal %.6f %.6f %.6f\n"), N.X, N.Y, N.Z);
            Stl += TEXT("    outer loop\n");
            Stl += FString::Printf(TEXT("      vertex %.6f %.6f %.6f\n"), A.X, A.Y, A.Z);
            Stl += FString::Printf(TEXT("      vertex %.6f %.6f %.6f\n"), B.X, B.Y, B.Z);
            Stl += FString::Printf(TEXT("      vertex %.6f %.6f %.6f\n"), C.X, C.Y, C.Z);
            Stl += TEXT("    endloop\n");
            Stl += TEXT("  endfacet\n");
        }
        Stl += FString::Printf(TEXT("endsolid %s\n"), *SolidName);
        return Stl;
    }

    void MeshIOAppendFloat(TArray<uint8>& Out, float V)
    {
        Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(float));
    }

    void MeshIOBuildStlBinary(const UE::Geometry::FDynamicMesh3& Mesh, TArray<uint8>& Out)
    {
        Out.Reset();
        // 80-byte header. Must NOT start with "solid" so it is not mistaken for ASCII.
        uint8 Header[80];
        FMemory::Memzero(Header, sizeof(Header));
        const ANSICHAR* Label = "PinWright binary STL";
        FMemory::Memcpy(Header, Label, FCStringAnsi::Strlen(Label));
        Out.Append(Header, sizeof(Header));

        const uint32 TriCount = static_cast<uint32>(Mesh.TriangleCount());
        Out.Append(reinterpret_cast<const uint8*>(&TriCount), sizeof(uint32));

        for (int32 Tid : Mesh.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(Tid);
            const FVector3d A = Mesh.GetVertex(Tri.A);
            const FVector3d B = Mesh.GetVertex(Tri.B);
            const FVector3d C = Mesh.GetVertex(Tri.C);
            const FVector3d N = MeshIOFaceNormal(A, B, C);
            MeshIOAppendFloat(Out, (float)N.X); MeshIOAppendFloat(Out, (float)N.Y); MeshIOAppendFloat(Out, (float)N.Z);
            MeshIOAppendFloat(Out, (float)A.X); MeshIOAppendFloat(Out, (float)A.Y); MeshIOAppendFloat(Out, (float)A.Z);
            MeshIOAppendFloat(Out, (float)B.X); MeshIOAppendFloat(Out, (float)B.Y); MeshIOAppendFloat(Out, (float)B.Z);
            MeshIOAppendFloat(Out, (float)C.X); MeshIOAppendFloat(Out, (float)C.Y); MeshIOAppendFloat(Out, (float)C.Z);
            const uint16 Attr = 0;
            Out.Append(reinterpret_cast<const uint8*>(&Attr), sizeof(uint16));
        }
    }

    // ---- STL parsing -----------------------------------------------------------
    // Binary STL layout: 80-byte header + uint32 triangle count + N * 50 bytes. The
    // size-match test (84 + 50*N == fileSize) is the standard robust binary/ASCII
    // discriminator (a binary header can itself begin with "solid").
    bool MeshIOIsBinaryStl(const TArray<uint8>& Bytes)
    {
        if (Bytes.Num() < 84)
        {
            return false;
        }
        uint32 TriCount = 0;
        FMemory::Memcpy(&TriCount, Bytes.GetData() + 80, sizeof(uint32));
        const int64 Expected = 84 + static_cast<int64>(TriCount) * 50;
        return Expected == static_cast<int64>(Bytes.Num());
    }

    // No vertex welding (MVP): STL has no shared-vertex indices, so each triangle
    // emits 3 fresh vertices. Triangle count round-trips exactly; vertex count is
    // 3 * triangleCount.
    bool MeshIOParseStlBinary(const TArray<uint8>& Bytes, TArray<FVector>& OutVertices, TArray<FIntVector>& OutTriangles)
    {
        if (!MeshIOIsBinaryStl(Bytes))
        {
            return false;
        }
        uint32 TriCount = 0;
        FMemory::Memcpy(&TriCount, Bytes.GetData() + 80, sizeof(uint32));
        OutVertices.Reserve(TriCount * 3);
        OutTriangles.Reserve(TriCount);

        int64 Off = 84;
        for (uint32 t = 0; t < TriCount; ++t)
        {
            Off += 12; // skip the stored per-face normal
            FVector V[3];
            for (int32 v = 0; v < 3; ++v)
            {
                float XYZ[3];
                FMemory::Memcpy(XYZ, Bytes.GetData() + Off, sizeof(XYZ));
                V[v] = FVector(XYZ[0], XYZ[1], XYZ[2]);
                Off += 12;
            }
            Off += 2; // attribute byte count
            const int32 Base = OutVertices.Num();
            OutVertices.Add(V[0]);
            OutVertices.Add(V[1]);
            OutVertices.Add(V[2]);
            OutTriangles.Add(FIntVector(Base, Base + 1, Base + 2));
        }
        return OutTriangles.Num() > 0;
    }

    bool MeshIOParseStlAscii(const FString& Text, TArray<FVector>& OutVertices, TArray<FIntVector>& OutTriangles)
    {
        TArray<FString> Tok;
        Text.ParseIntoArrayWS(Tok);

        TArray<FVector> Verts;
        for (int32 i = 0; i < Tok.Num(); ++i)
        {
            if (Tok[i].Equals(TEXT("vertex"), ESearchCase::IgnoreCase) && i + 3 < Tok.Num())
            {
                Verts.Add(FVector(
                    FCString::Atod(*Tok[i + 1]), FCString::Atod(*Tok[i + 2]), FCString::Atod(*Tok[i + 3])));
                i += 3;
            }
        }
        if (Verts.Num() < 3 || (Verts.Num() % 3) != 0)
        {
            return false;
        }
        for (int32 i = 0; i < Verts.Num(); i += 3)
        {
            OutTriangles.Add(FIntVector(i, i + 1, i + 2));
        }
        OutVertices = MoveTemp(Verts);
        return true;
    }

    // The two spellings of allowPartial, in ONE place. Both the declaration and the read go
    // through this list, because they are not independent: the dispatcher rejects any top-level
    // field that is neither a declared Name nor a declared Alias (RpcDispatcher.cpp's
    // unknown-param gate), so a snake_case spelling the body reads but the spec does not declare
    // is rejected before the handler ever runs. Canonical first.
    const TArray<FString>& MeshIOAllowPartialKeys()
    {
        static const TArray<FString> Keys = { TEXT("allowPartial"), TEXT("allow_partial") };
        return Keys;
    }

    // RPC_PARAM_DEF cannot carry aliases and ParamAliasUtils::MakeAliasParamSpec cannot carry a
    // default, so the two are combined here - the same shape LevelAuditHandler.cpp uses.
    FParamSpec MeshIOAllowPartialParam()
    {
        FParamSpec Spec = ParamAliasUtils::MakeAliasParamSpec(
            TEXT("allowPartial"), TEXT("bool"),
            TEXT("Import the triangles the engine accepts even when it refuses some (non-manifold, "
                 "duplicate or invalid). Default false: a refused triangle fails the import rather "
                 "than silently producing a mesh with missing geometry"),
            /*bRequired=*/false, MeshIOAllowPartialKeys());
        Spec.Default = TEXT("false");
        return Spec;
    }

    bool ReadMeshIOAllowPartial(const FHandlerContext& Ctx)
    {
        return Ctx.GetBoolFirstOf(MeshIOAllowPartialKeys(), false);
    }

    // Shared tail for import verbs: build a DynamicMesh from parsed buffers, spawn a
    // new DynamicMeshActor, and send the {actorName, class, path, format, vertexCount,
    // triangleCount} + AddActorVerification success. FilePathEcho is the resolved source path
    // echoed back ("" for text input).
    //
    // import_obj and import_stl spawn an actor, so they owe the caller the same response shape
    // the create_* family speaks (PrimitiveHandler.cpp) - `class` plus the verification block.
    // Both were missed by that sweep for the same reason geometry.revolve was: neither name
    // starts with create_. Fixing the shared tail fixes both verbs at once.
    void FinishMeshIOImport(const FHandlerContext& Ctx, TArray<FVector>&& Vertices, TArray<FIntVector>&& Triangles,
        const FTransform& Transform, const FString& ActorName, const FString& FilePathEcho, const FString& FormatEcho,
        bool bAllowPartial)
    {
        UDynamicMesh* DynMesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

        FGeometryScriptSimpleMeshBuffers Buffers;
        Buffers.Vertices = MoveTemp(Vertices);
        Buffers.Triangles = MoveTemp(Triangles);

        // Called RAW rather than through GeometryOps::AppendBuffers, and that is safe HERE only.
        // The engine's AppendBuffersToMesh does not merge UV layers, it sets the TARGET's layer
        // count to whatever the incoming buffers carry (MeshBasicEditFunctions.cpp:973-983), so
        // appending UV-less buffers to a UV'd mesh DELETES the UVs off the geometry that was
        // already there. GeometryOps::AppendBuffers pads the incoming buffers to stop that.
        //
        // That mitigation cannot apply at this call site, and not because it was skipped: DynMesh
        // is one statement old and empty. GetOrCreateDynamicMesh is a bare NewObject<UDynamicMesh>
        // (GeometryUtils.cpp), so the target has no triangles and no UV elements, the preserve
        // count GeometryOps::AppendBuffers computes would be 0, and the padding branch would not
        // run. There is nothing on this mesh to lose.
        //
        // What the import DOES leave behind is a mesh with ZERO UV layers (the buffers carry no
        // UV0, so SetNumUVLayers(0) removes the default layer). That is wholly-unset, never the
        // partially-set shape, because a single append into an empty mesh cannot produce a mix.
        // The wholly-unset shape is already guarded everywhere it is dangerous: the bake box-
        // projects through GeometryUtils::EnsureMeshHasUVs, and bevel/shell refuse it by name.
        // The OBJ parser also drops `vt` outright, so even a UV-carrying .obj arrives UV-less.
        // Read before the append: Buffers owns the array now, and the engine does not report how
        // many triangles it was ASKED for - only, one message at a time, the ones it refused.
        const int32 RequestedTriangles = Buffers.Triangles.Num();

        // THE FAILURE CHANNEL FOR AN IMPORT, and the reason import_obj / import_stl could lose
        // geometry without saying anything. AppendBuffersToMesh validates each triangle as it
        // goes and, for one it will not take, calls AppendError and continues - it never returns
        // a failure, never stops, and returns TargetMesh regardless (MeshBasicEditFunctions.cpp,
        // inside the AppendBuffersToMesh EditMesh lambda). Three of its four rejections are
        // reachable from a file a user hands us:
        //
        //   "Triangle cannot be added because it would create invalid Non-Manifold Mesh
        //    Topology"    - a third face on an edge two faces already share
        //   "...because it is a duplicate of an existing Triangle"
        //                 - the same face listed twice, or two f-lines that triangulate to the
        //                   same three vertices
        //   "...because it had invalid indices"
        //                 - NOT reachable through import_obj: MeshIOParseObjText range-checks OBJ
        //                   indices itself and drops the offending face before we get here (which
        //                   is its own silent loss - see the parser). Wired anyway, because the
        //                   STL paths build indices arithmetically and a parser change reaches it.
        //
        // The vertices are appended BEFORE any triangle is validated and are never rolled back, so
        // a refused triangle also leaves its three vertices behind as isolated points. That is why
        // the counts this verb echoes could not be used to detect the loss either: vertexCount
        // still matches the file while triangleCount quietly does not.
        //
        // AppendBuffersToMesh has no AppendWarning call on any path, so there is deliberately no
        // DrainWarningsInto here - nothing could arrive on that channel.
        GeometryOps::FGeometryScriptDebugSink Debug;

        FGeometryScriptIndexList NewTriangleIndices;
        UGeometryScriptLibrary_MeshBasicEditFunctions::AppendBuffersToMesh(
            DynMesh, Buffers, NewTriangleIndices, /*MaterialID=*/0, /*bDeferChangeNotifications=*/false, Debug.Get());

        const int32 VertexCount = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(DynMesh);
        const int32 TriangleCount = DynMesh->GetTriangleCount();
        const int32 RefusedTriangles = Debug.ErrorCount();

        // BEFORE GeometryTarget::Spawn, and that ordering IS the side-effect audit for this site
        // (rpc-design.md 12, "reviving a dead error path exposes every side effect sequenced
        // before the failure check"). Spawning is this verb's COMMIT: it is what the caller is
        // left holding, it takes a label in the level, and it would carry the truncated mesh under
        // an error response. Nothing else has committed at this point - DynMesh is a bare
        // NewObject in the transient package, so abandoning it needs no cleanup beyond letting GC
        // take it, and the source file was only ever read.
        //
        // ErrorSummary, not ErrorText: the engine appends one message PER REFUSED TRIANGLE, so a
        // 40k-face file that is non-manifold throughout would otherwise put 40k copies of one
        // sentence into a single JSON error field.
        if (Debug.HasError() && !bAllowPartial)
        {
            UE_LOG(LogMcpGeometryHandlersNew, Warning, TEXT("Mesh import refused %d of %d triangles: %s"),
                RefusedTriangles, RequestedTriangles, *Debug.ErrorSummary());

            Ctx.SendError(ErrorCodes::ERR_IMPORT_FAILED, FString::Printf(
                TEXT("Import refused %d of %d triangle(s). The mesh would have been created with "
                     "%d triangle(s) and silently missing geometry, so no actor was spawned. The "
                     "engine reported: %s. Pass allowPartial=true to import the %d triangle(s) it "
                     "did accept."),
                RefusedTriangles, RequestedTriangles, TriangleCount, *Debug.ErrorSummary(),
                TriangleCount));

            // Cleanup, not a commit - the distinction rpc-design.md 12 draws. DynMesh is this
            // function's own allocation and nothing outside it has seen it, so releasing it on the
            // failure path is symmetric with GeometryTarget::Spawn doing the same on its own
            // failures, rather than leaving a mesh for GC that no longer has a purpose.
            DynMesh->MarkAsGarbage();
            return;
        }

        AActor* NewActor = GeometryTarget::Spawn(Ctx, DynMesh, Transform, ActorName);
        if (!NewActor)
        {
            return; // error already sent; DynMesh marked garbage inside the spawn helper
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actorName"), NewActor->GetActorLabel());
        Result->SetStringField(TEXT("class"), TEXT("DynamicMeshActor"));
        Result->SetStringField(TEXT("path"), FilePathEcho);
        Result->SetStringField(TEXT("format"), FormatEcho);
        Result->SetNumberField(TEXT("vertexCount"), VertexCount);
        Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
        // Published on EVERY import, not only the partial one, so a caller can assert "nothing was
        // dropped" without having to know which flag it passed. requestedTriangles is what the
        // file described; triangleCount is what the mesh actually got.
        Result->SetNumberField(TEXT("requestedTriangles"), RequestedTriangles);
        Result->SetNumberField(TEXT("droppedTriangles"), RequestedTriangles - TriangleCount);
        if (Debug.HasError())
        {
            // Only reachable with allowPartial=true. The success is real - an actor exists and
            // carries geometry - but it is a success over LESS geometry than the file described,
            // and the caller asked for that outcome rather than stumbling into it.
            TArray<TSharedPtr<FJsonValue>> Warnings;
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("allowPartial: the engine refused %d of %d triangle(s), and they are NOT in "
                     "the imported mesh (their vertices are, as isolated points). %s"),
                RefusedTriangles, RequestedTriangles, *Debug.ErrorSummary())));
            Result->SetArrayField(TEXT("warnings"), Warnings);
        }
        // Overwrites the actorName set above with the same GetActorLabel() value and adds the
        // rest of the identity block; ordered after the echoes for the same reason the create
        // verbs order it there.
        AddActorVerification(Result, NewActor);
        Ctx.SendSuccess(TEXT("Mesh imported"), Result);
    }
}

// ============================================================================
// export_obj
// ============================================================================
REGISTER_RPC_HANDLER("geometry.export_obj", "geometry",
    "Export a DynamicMeshActor's mesh as Wavefront OBJ text (v/vt/vn/f). Writes to filePath or Saved/PinWright/Meshes/<actor>.obj; refuses an existing file unless overwrite=true; set returnText to also get the OBJ inline for LLM editing",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the source DynamicMeshActor"),
        RPC_PARAM_OPT("filePath", "filepath", "Project-relative output path (default Saved/PinWright/Meshes/<actor>.obj)"),
        RPC_PARAM_DEF("overwrite", "boolean", "Replace an existing output file atomically instead of returning ALREADY_EXISTS", "false"),
        RPC_PARAM_OPT("returnText", "boolean", "If true, also return the full OBJ text inline in a 'text' field (default false)")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    const FString FilePathParam = Ctx.GetString(TEXT("filePath"));
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bReturnText = Ctx.GetBool(TEXT("returnText"), false);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
    {
        return true;
    }

    const UE::Geometry::FDynamicMesh3& Mesh = Target.Mesh->GetMeshRef();
    const int32 VertexCount = Mesh.VertexCount();
    const int32 TriangleCount = Mesh.TriangleCount();

    FString FullPath;
    if (!ResolveMeshIOExportPath(Ctx, FilePathParam, ActorName, TEXT("obj"), FullPath))
    {
        return true;
    }
    // This fast path only avoids serialization work. FailIfExists below remains the
    // race-safe authority if another writer creates the destination after this check.
    if (!bOverwrite && IFileManager::Get().FileExists(*FullPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("OBJ output already exists: %s. Pass overwrite=true to replace it."),
                *FullPath));
        return true;
    }

    const FString ObjText = MeshIOBuildObjText(Mesh);

    const AtomicFileWriter::FResult WriteResult = AtomicFileWriter::WriteUtf8(
        FullPath, ObjText, bOverwrite
            ? AtomicFileWriter::EExistingFilePolicy::ReplaceExisting
            : AtomicFileWriter::EExistingFilePolicy::FailIfExists);
    if (!WriteResult.IsSuccess())
    {
        if (WriteResult.Status == AtomicFileWriter::EStatus::AlreadyExists)
        {
            Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
                FString::Printf(TEXT("OBJ output already exists: %s. Pass overwrite=true to replace it."),
                    *FullPath));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_WRITE_FAILED, FString::Printf(
                TEXT("Failed to write OBJ file: %s. %s"), *FullPath, *WriteResult.Error));
        }
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("path"), FullPath);
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
    Result->SetBoolField(TEXT("replaced"), WriteResult.bReplaced);
    if (bReturnText)
    {
        Result->SetStringField(TEXT("text"), ObjText);
    }
    Ctx.SendSuccess(TEXT("Mesh exported to OBJ"), Result);
    return true;
}

// ============================================================================
// export_stl
// ============================================================================
REGISTER_RPC_HANDLER("geometry.export_stl", "geometry",
    "Export a DynamicMeshActor's mesh as STL (ASCII by default, or binary). Writes to filePath or Saved/PinWright/Meshes/<actor>.stl; refuses an existing file unless overwrite=true; returnText returns ASCII inline (binary is never inlined)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the source DynamicMeshActor"),
        RPC_PARAM_OPT("filePath", "filepath", "Project-relative output path (default Saved/PinWright/Meshes/<actor>.stl)"),
        RPC_PARAM_OPT("binary", "boolean", "If true, write binary STL; otherwise ASCII (default false)"),
        RPC_PARAM_DEF("overwrite", "boolean", "Replace an existing output file atomically instead of returning ALREADY_EXISTS", "false"),
        RPC_PARAM_OPT("returnText", "boolean", "If true, also return the ASCII STL text inline in a 'text' field (ignored for binary; default false)")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    const FString FilePathParam = Ctx.GetString(TEXT("filePath"));
    const bool bBinary = Ctx.GetBool(TEXT("binary"), false);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bReturnText = Ctx.GetBool(TEXT("returnText"), false);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
    {
        return true;
    }

    const UE::Geometry::FDynamicMesh3& Mesh = Target.Mesh->GetMeshRef();
    const int32 VertexCount = Mesh.VertexCount();
    const int32 TriangleCount = Mesh.TriangleCount();

    FString FullPath;
    if (!ResolveMeshIOExportPath(Ctx, FilePathParam, ActorName, TEXT("stl"), FullPath))
    {
        return true;
    }
    // Non-authoritative optimization only; the atomic writer still enforces
    // FailIfExists at publication so a check/write race cannot replace a file.
    if (!bOverwrite && IFileManager::Get().FileExists(*FullPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("STL output already exists: %s. Pass overwrite=true to replace it."),
                *FullPath));
        return true;
    }

    FString AsciiText;
    AtomicFileWriter::FResult WriteResult;
    const AtomicFileWriter::EExistingFilePolicy ExistingFilePolicy = bOverwrite
        ? AtomicFileWriter::EExistingFilePolicy::ReplaceExisting
        : AtomicFileWriter::EExistingFilePolicy::FailIfExists;
    if (bBinary)
    {
        TArray<uint8> Bytes;
        MeshIOBuildStlBinary(Mesh, Bytes);
        WriteResult = AtomicFileWriter::WriteBytes(FullPath,
            TArrayView<const uint8>(Bytes.GetData(), Bytes.Num()), ExistingFilePolicy);
    }
    else
    {
        AsciiText = MeshIOBuildStlAscii(Mesh, SanitizeMeshIOFileToken(ActorName));
        WriteResult = AtomicFileWriter::WriteUtf8(FullPath, AsciiText, ExistingFilePolicy);
    }

    if (!WriteResult.IsSuccess())
    {
        if (WriteResult.Status == AtomicFileWriter::EStatus::AlreadyExists)
        {
            Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
                FString::Printf(TEXT("STL output already exists: %s. Pass overwrite=true to replace it."),
                    *FullPath));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_WRITE_FAILED, FString::Printf(
                TEXT("Failed to write STL file: %s. %s"), *FullPath, *WriteResult.Error));
        }
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("path"), FullPath);
    Result->SetStringField(TEXT("format"), bBinary ? TEXT("binary") : TEXT("ascii"));
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
    Result->SetBoolField(TEXT("replaced"), WriteResult.bReplaced);
    // Binary STL is not inlined (would need base64); only ASCII rides back in `text`.
    if (bReturnText && !bBinary)
    {
        Result->SetStringField(TEXT("text"), AsciiText);
    }
    Ctx.SendSuccess(TEXT("Mesh exported to STL"), Result);
    return true;
}

// ============================================================================
// import_obj
// ============================================================================
REGISTER_RPC_HANDLER("geometry.import_obj", "geometry",
    "Import Wavefront OBJ (from text or filePath) into a NEW DynamicMeshActor. Parses v/f (all face forms, fan-triangulating polygons); vt/vn are parsed but not applied to the mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Label for the new DynamicMeshActor to create"),
        RPC_PARAM_OPT("text", "string", "OBJ text to parse (provide this OR filePath)"),
        RPC_PARAM_OPT("filePath", "filepath", "Project-relative path to an OBJ file (provide this OR text)"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x, y, z}"),
        MeshIOAllowPartialParam()
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
        return true;
    }

    const FString InlineText = Ctx.GetString(TEXT("text"));
    const FString FilePathParam = Ctx.GetString(TEXT("filePath"));

    FString ObjText;
    FString FilePathEcho;
    if (!InlineText.IsEmpty())
    {
        ObjText = InlineText;
    }
    else if (!FilePathParam.IsEmpty())
    {
        TArray<uint8> Bytes;
        if (!LoadMeshIOImportFile(Ctx, FilePathParam, Bytes))
        {
            return true;
        }
        FFileHelper::BufferToString(ObjText, Bytes.GetData(), Bytes.Num());
        FString Safe = SanitizeProjectFilePath(FilePathParam);
        Safe.RemoveFromStart(TEXT("/"));
        FilePathEcho = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Safe);
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Provide either 'text' or 'filePath'"));
        return true;
    }

    TArray<FVector> Vertices;
    TArray<FIntVector> Triangles;
    if (!MeshIOParseObjText(ObjText, Vertices, Triangles))
    {
        Ctx.SendError(TEXT("PARSE_FAILED"), TEXT("Malformed OBJ: no vertices/triangles parsed"));
        return true;
    }

    const FTransform Transform = GeometryUtils::ReadTransformFromPayload(Ctx.GetRawPayload());
    FinishMeshIOImport(Ctx, MoveTemp(Vertices), MoveTemp(Triangles), Transform, ActorName, FilePathEcho, TEXT("obj"),
        ReadMeshIOAllowPartial(Ctx));
    return true;
}

// ============================================================================
// import_stl
// ============================================================================
REGISTER_RPC_HANDLER("geometry.import_stl", "geometry",
    "Import STL (ASCII from text or filePath; binary from filePath only) into a NEW DynamicMeshActor. Binary/ASCII is auto-detected. STL has no shared vertices, so each triangle emits 3 vertices (no welding)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Label for the new DynamicMeshActor to create"),
        RPC_PARAM_OPT("text", "string", "ASCII STL text to parse (provide this OR filePath; binary must come from filePath)"),
        RPC_PARAM_OPT("filePath", "filepath", "Project-relative path to an STL file, ASCII or binary (provide this OR text)"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch, yaw, roll}"),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x, y, z}"),
        MeshIOAllowPartialParam()
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
        return true;
    }

    const FString InlineText = Ctx.GetString(TEXT("text"));
    const FString FilePathParam = Ctx.GetString(TEXT("filePath"));

    TArray<FVector> Vertices;
    TArray<FIntVector> Triangles;
    FString FilePathEcho;
    FString FormatEcho;

    if (!InlineText.IsEmpty())
    {
        // Inline text can only carry ASCII STL (binary would need raw bytes).
        if (!MeshIOParseStlAscii(InlineText, Vertices, Triangles))
        {
            Ctx.SendError(TEXT("PARSE_FAILED"), TEXT("Malformed ASCII STL: no valid vertices parsed"));
            return true;
        }
        FormatEcho = TEXT("ascii");
    }
    else if (!FilePathParam.IsEmpty())
    {
        TArray<uint8> Bytes;
        if (!LoadMeshIOImportFile(Ctx, FilePathParam, Bytes))
        {
            return true;
        }
        bool bParsed = false;
        if (MeshIOIsBinaryStl(Bytes))
        {
            bParsed = MeshIOParseStlBinary(Bytes, Vertices, Triangles);
            FormatEcho = TEXT("binary");
        }
        else
        {
            FString AsciiText;
            FFileHelper::BufferToString(AsciiText, Bytes.GetData(), Bytes.Num());
            bParsed = MeshIOParseStlAscii(AsciiText, Vertices, Triangles);
            FormatEcho = TEXT("ascii");
        }
        if (!bParsed)
        {
            Ctx.SendError(TEXT("PARSE_FAILED"), TEXT("Malformed STL: no valid triangles parsed"));
            return true;
        }
        FString Safe = SanitizeProjectFilePath(FilePathParam);
        Safe.RemoveFromStart(TEXT("/"));
        FilePathEcho = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Safe);
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Provide either 'text' or 'filePath'"));
        return true;
    }

    const FTransform Transform = GeometryUtils::ReadTransformFromPayload(Ctx.GetRawPayload());
    FinishMeshIOImport(Ctx, MoveTemp(Vertices), MoveTemp(Triangles), Transform, ActorName, FilePathEcho, FormatEcho,
        ReadMeshIOAllowPartial(Ctx));
    return true;
}
