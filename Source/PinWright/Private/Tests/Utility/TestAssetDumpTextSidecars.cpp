// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/TestUtils.h"

#include "AssetDumpFixtureHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "PixelFormat.h"
#include "UObject/Package.h"

namespace
{
    using AssetDumpFixtureHelpers::WrittenPathsContains;

    // Mirrors AssetTextEmitterHelpers::FormatNumber so the tests below can build the exact
    // string the emitter would write for a value read off the live UObject. Kept as a copy
    // rather than a call into the production helper on purpose: the emitter's number
    // formatting is part of the contract under test, so the expectation must be stated
    // independently instead of being derived from the code that produced the file.
    FString FormatSidecarNumberForTest(double Value)
    {
        const double Rounded = FMath::RoundToDouble(Value);
        if (FMath::IsNearlyEqual(Value, Rounded, KINDA_SMALL_NUMBER))
        {
            return FString::Printf(TEXT("%lld"), static_cast<int64>(Rounded));
        }
        return FString::SanitizeFloat(Value);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpStaticMeshTextSidecarTest,
    "PinWright.asset.dump.StaticMeshTextSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpStaticMeshTextSidecarTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpStaticMeshText") / Suffix;

    const FString FixturePath = TEXT("/Engine/EditorMeshes/EditorCube.EditorCube");
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FixturePath);
    if (!TestNotNull(TEXT("EditorCube static mesh fixture loads"), Mesh))
    {
        return true;
    }

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(FixturePath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains static_mesh.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::StaticMesh));
    TestTrue(TEXT("WrittenPaths contains static_mesh.txt"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::StaticMeshTxt));

    const FString StaticMeshJsonPath = Result.DumpDir / DumpFileNames::StaticMesh;
    const FString StaticMeshTextPath = Result.DumpDir / DumpFileNames::StaticMeshTxt;
    TestTrue(TEXT("static_mesh.json exists on disk"),
        IFileManager::Get().FileExists(*StaticMeshJsonPath));
    TestTrue(TEXT("static_mesh.txt exists on disk"),
        IFileManager::Get().FileExists(*StaticMeshTextPath));

    FString Text;
    if (TestTrue(TEXT("static_mesh.txt loads"),
        FFileHelper::LoadFileToString(Text, *StaticMeshTextPath)))
    {
        TestTrue(TEXT("static_mesh.txt has DSL root"), Text.Contains(TEXT("static_mesh {")));
        TestTrue(TEXT("static_mesh.txt has bounds token"), Text.Contains(TEXT("bounds {")));
        TestTrue(TEXT("static_mesh.txt has materials token"), Text.Contains(TEXT("materials {")));
        TestTrue(TEXT("static_mesh.txt has trianglesByLod token"), Text.Contains(TEXT("trianglesByLod:")));
        TestTrue(TEXT("static_mesh.txt has verticesByLod token"), Text.Contains(TEXT("verticesByLod:")));
        TestTrue(TEXT("static_mesh.txt has collisionTraceFlag token"), Text.Contains(TEXT("collisionTraceFlag:")));
        TestFalse(TEXT("static_mesh.txt does not use JSON key quoting"), Text.Contains(TEXT("\"bounds\"")));

        // ---- Projected VALUES, derived from the live UStaticMesh ----
        // The token checks above cannot fail on a broken projection: every formatter in
        // StaticMeshTextEmitter.cpp emits its KEY unconditionally and degrades the VALUE
        // silently. FormatNumberArray skips non-Number elements and still writes
        // `trianglesByLod: []`; FormatVectorObject substitutes "0" for each component it
        // cannot read, so `origin: (x=0, y=0, z=0)` is exactly what total failure looks
        // like; AppendOptionalString writes `None` for an empty string. Emitted formats
        // taken from StaticMeshTextEmitter.cpp: "[%s]" joined with ", " for number arrays,
        // "(x=%s, y=%s, z=%s)" for vectors, "<field>: <value>" for scalars.
        const int32 NumLODs = Mesh->GetNumLODs();
        TestTrue(TEXT("static_mesh.txt lods matches the mesh's LOD count"),
            Text.Contains(FString::Printf(TEXT("lods: %s"),
                *FormatSidecarNumberForTest(NumLODs))));

        TArray<FString> ExpectedTriangles;
        TArray<FString> ExpectedVertices;
        for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
        {
            ExpectedTriangles.Add(FormatSidecarNumberForTest(Mesh->GetNumTriangles(LODIndex)));
            ExpectedVertices.Add(FormatSidecarNumberForTest(Mesh->GetNumVertices(LODIndex)));
        }
        TestTrue(TEXT("static_mesh.txt trianglesByLod carries the mesh's per-LOD triangle counts"),
            Text.Contains(FString::Printf(TEXT("trianglesByLod: [%s]"),
                *FString::Join(ExpectedTriangles, TEXT(", ")))));
        TestTrue(TEXT("static_mesh.txt verticesByLod carries the mesh's per-LOD vertex counts"),
            Text.Contains(FString::Printf(TEXT("verticesByLod: [%s]"),
                *FString::Join(ExpectedVertices, TEXT(", ")))));

        const FBoxSphereBounds MeshBounds = Mesh->GetExtendedBounds();
        TestTrue(TEXT("static_mesh.txt bounds origin carries the mesh's origin"),
            Text.Contains(FString::Printf(TEXT("origin: (x=%s, y=%s, z=%s)"),
                *FormatSidecarNumberForTest(MeshBounds.Origin.X),
                *FormatSidecarNumberForTest(MeshBounds.Origin.Y),
                *FormatSidecarNumberForTest(MeshBounds.Origin.Z))));
        TestTrue(TEXT("static_mesh.txt bounds extent carries the mesh's box extent"),
            Text.Contains(FString::Printf(TEXT("extent: (x=%s, y=%s, z=%s)"),
                *FormatSidecarNumberForTest(MeshBounds.BoxExtent.X),
                *FormatSidecarNumberForTest(MeshBounds.BoxExtent.Y),
                *FormatSidecarNumberForTest(MeshBounds.BoxExtent.Z))));

        FString ExpectedTraceFlag;
        if (const UBodySetup* BodySetup = Mesh->GetBodySetup())
        {
            if (const UEnum* TraceFlagEnum = StaticEnum<ECollisionTraceFlag>())
            {
                ExpectedTraceFlag = TraceFlagEnum->GetNameStringByValue(
                    static_cast<int64>(BodySetup->CollisionTraceFlag));
            }
        }
        TestTrue(TEXT("static_mesh.txt collisionTraceFlag carries the body setup's trace flag"),
            Text.Contains(FString::Printf(TEXT("collisionTraceFlag: %s"),
                ExpectedTraceFlag.IsEmpty() ? TEXT("None") : *ExpectedTraceFlag)));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpTextureTextSidecarTest,
    "PinWright.asset.dump.TextureTextSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpTextureTextSidecarTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpTextureText") / Suffix;

    const FString FixturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *FixturePath);
    if (!TestNotNull(TEXT("DefaultTexture fixture loads"), Texture))
    {
        return true;
    }

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(FixturePath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains texture.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::Texture));
    TestTrue(TEXT("WrittenPaths contains texture.txt"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::TextureTxt));

    const FString TextureJsonPath = Result.DumpDir / DumpFileNames::Texture;
    const FString TextureTextPath = Result.DumpDir / DumpFileNames::TextureTxt;
    TestTrue(TEXT("texture.json exists on disk"),
        IFileManager::Get().FileExists(*TextureJsonPath));
    TestTrue(TEXT("texture.txt exists on disk"),
        IFileManager::Get().FileExists(*TextureTextPath));

    FString Text;
    if (TestTrue(TEXT("texture.txt loads"),
        FFileHelper::LoadFileToString(Text, *TextureTextPath)))
    {
        TestTrue(TEXT("texture.txt has DSL root"), Text.Contains(TEXT("texture {")));
        TestTrue(TEXT("texture.txt has kind token"), Text.Contains(TEXT("kind: Texture2D")));
        TestTrue(TEXT("texture.txt has size token"), Text.Contains(TEXT("size:")));
        TestTrue(TEXT("texture.txt has arraySize token"), Text.Contains(TEXT("arraySize:")));
        TestFalse(TEXT("texture.txt does not use JSON key quoting"), Text.Contains(TEXT("\"kind\"")));

        // ---- Projected VALUES, derived from the live UTexture2D ----
        // `size:` is written whenever the JSON carries a size object at all:
        // FormatDimensionObject (TextureTextEmitter.cpp) appends only the components it can
        // read and returns "()" when it reads none, so the token check above passes on a
        // fully degraded projection. The root size object is built by TextureDumpBuilder
        // from GetSurfaceWidth/Height/Depth (x/y/z, no slices), and the emitted format is
        // "(" + comma-joined "<field>=<value>" + ")".
        TestTrue(TEXT("texture.txt size carries the texture's surface dimensions"),
            Text.Contains(FString::Printf(TEXT("size: (x=%s, y=%s, z=%s)"),
                *FormatSidecarNumberForTest(Texture->GetSurfaceWidth()),
                *FormatSidecarNumberForTest(Texture->GetSurfaceHeight()),
                *FormatSidecarNumberForTest(Texture->GetSurfaceDepth()))));
        TestTrue(TEXT("texture.txt arraySize carries the texture's surface array size"),
            Text.Contains(FString::Printf(TEXT("arraySize: %s"),
                *FormatSidecarNumberForTest(Texture->GetSurfaceArraySize()))));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpRenderTargetTextureTextSidecarTest,
    "PinWright.asset.dump.RenderTargetTextureTextSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpRenderTargetTextureTextSidecarTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpRenderTargetTextureText") / Suffix;

    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/AssetDumpTextureTextCube_%s"), *Suffix);
    // RF_Standalone survives the periodic suite GC; detach so a later /Engine/Transient
    // asset-registry rescan cannot see the fixture.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    const FString AssetName = FString::Printf(TEXT("AssetDumpTextureTextCube_%s"), *Suffix);
    UTextureRenderTargetCube* RenderTarget = NewObject<UTextureRenderTargetCube>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("TextureRenderTargetCube created"), RenderTarget))
    {
        return true;
    }
    RenderTarget->Init(64, PF_FloatRGBA);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains texture.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::Texture));
    TestTrue(TEXT("WrittenPaths contains texture.txt"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::TextureTxt));

    FString Text;
    const FString TextureTextPath = Result.DumpDir / DumpFileNames::TextureTxt;
    if (TestTrue(TEXT("texture.txt loads"),
        FFileHelper::LoadFileToString(Text, *TextureTextPath)))
    {
        TestTrue(TEXT("texture.txt has render-target source token"),
            Text.Contains(TEXT("sourceKind: renderTarget")));
        TestTrue(TEXT("texture.txt has TSF source format"),
            Text.Contains(TEXT("format: TSF_")));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}
