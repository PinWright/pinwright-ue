// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for CreateStaticMesh - the single-build DynamicMesh -> UStaticMesh path that
// replaced CreateNewStaticMeshAssetFromMesh.
//
// These call the function DIRECTLY, with no actor and no FHandlerContext. The dispatcher-level
// geometry.convert_to_static_mesh tests remain the regression net for that verb's response
// contract; this file covers what only the pre-build path can express, each of which reverting
// would silently break:
//
//   1. Material slots exist and carry the caller's NAMES. The engine util hides AssetMaterials
//      and NumMaterialSlots, so the old path always produced exactly one default slot and
//      static_mesh.set_material had to patch it afterwards ("slots were never populated",
//      StaticMeshSetMaterialHandler.cpp:7). Nothing else in the plugin can observe this.
//   2. Simple collision and the lightmap index are INPUTS, written into a UBodySetup and a
//      FMeshBuildSettings that have never been built, rather than patched onto a built asset.
//   3. The provenance stamp derives the overwrite rule. A same-source recompile overwrites with
//      no flag; a different source, or an asset this path never generated, refuses. Drop the
//      stamp and the destructive case becomes invisible - the state one level had to
//      reconcile by hand across 231 placed actors.
//   4. Higher UV channels survive the bake's crash guard. The guard's older form called bare
//      SetNumUVSets(Mesh, 1), which sets the count EXACTLY and so silently truncated an
//      authored lightmap channel right before the lightmap index was pointed at it.
//   5. A compiled model that is actually IN USE still recompiles. The overwrite flag used to be
//      hardcoded true into AssetCreatePolicy::Resolve, which put every recompile on the
//      delete-then-recreate branch - so placing the asset anywhere made every later recompile
//      fail ASSET_IN_USE, and the rejection's own advice ("re-run without overwrite") named a
//      remedy no caller could ask for.
//   6. An asset that is BOTH referenced AND not ours still migrates, with the flag. (5) and (3)
//      each covered one half and their intersection was unreachable: overwrite=true was refused
//      ASSET_IN_USE and omitting it was refused ASSET_ALREADY_EXISTS "pass overwrite=true", so
//      the two gates named each other and the FIRST compile of every migration deadlocked.
//      overwrite is now permission only; the mechanism is always the in-place rebuild.
//
// The last test in this file is the exception to "these call the function directly": it goes
// through the dispatcher, because the behaviour it pins is a shipped verb's response contract.
#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif
#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshUVFunctions.h"

namespace
{
    // Prefixed because Unity merges this TU with the sibling geometry test files.
    const TCHAR* AssetCreateProbeMaterial = TEXT("/Engine/EngineMaterials/WorldGridMaterial");

    FString AssetCreateUniquePath()
    {
        return FString::Printf(TEXT("/Game/GeneratedMeshes/PW_AssetCreate_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Box-project UVSetIndex over the whole mesh, framed on its bounds. Same deterministic
    // projection the bake's own UV guard uses, so a test mesh matches what the guard would make.
    void AssetCreateProjectUVs(UDynamicMesh* Mesh, int32 UVSetIndex)
    {
        GeometryUtils::EnsureMeshHasUVChannel(Mesh, UVSetIndex);

        const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh->GetMeshRef().GetBounds();
        FVector BoxSize = Bounds.Diagonal();
        BoxSize.X = FMath::Max(FMath::Abs(BoxSize.X), 1.0);
        BoxSize.Y = FMath::Max(FMath::Abs(BoxSize.Y), 1.0);
        BoxSize.Z = FMath::Max(FMath::Abs(BoxSize.Z), 1.0);

        const FTransform BoxFrame(FQuat::Identity, Bounds.Center(), BoxSize);
        UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
            Mesh, UVSetIndex, BoxFrame, FGeometryScriptMeshSelection(), /*MinIslandTriCount=*/2, nullptr);
    }

    // A transient 100^3 box with UV channel 0 projected, i.e. the shape every test starts from.
    UDynamicMesh* AssetCreateBoxMesh()
    {
        UDynamicMesh* Mesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
        GeometryOps::FBoxParams Params;
        Params.Size = FVector(100.0, 100.0, 100.0);
        GeometryOps::GenerateBox(Mesh, Params, FTransform::Identity);
        AssetCreateProjectUVs(Mesh, 0);
        return Mesh;
    }

    // A spec for a test that reads IN-MEMORY state only.
    //
    // FStaticMeshCreateSpec::bSave defaults to true, which is right for the verb and wrong for a
    // suite: every test below except BoxCreatesAssetOnDisk asserts against the returned object,
    // yet the default had a dozen of them force a real .uasset write and a matching delete for
    // nothing. Nothing is lost by turning it off - Resolve's occupant lookup is FindObject-first,
    // so an unsaved asset is still found at its path by the provenance tests.
    FStaticMeshCreateSpec AssetCreateInMemorySpec(const FString& AssetPath)
    {
        FStaticMeshCreateSpec Spec;
        Spec.AssetPath = AssetPath;
        Spec.bSave = false;
        return Spec;
    }

    // The lightmap index is clamped during the build, so it can only be read once the build has
    // actually finished.
    void AssetCreateFinishBuild(UStaticMesh* Mesh)
    {
        if (Mesh && Mesh->IsCompiling())
        {
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        }
    }

    struct FAssetCreateReferencedFixture
    {
        UStaticMesh* Target = nullptr;
        UStaticMesh* Referencer = nullptr;
        // Asserted, never assumed: with no registry edge a "referenced asset still compiles"
        // test passes for the wrong reason and pins nothing.
        bool bRegistryEdge = false;
    };

    // "An asset something else really points at." ReferencedAssetStillRecompilesFromTheSameSource
    // builds this inline; it is factored out here because the overwrite tests below need the same
    // fixture on a target that is ALSO unstamped or foreign-stamped.
    //
    // TargetSourcePath empty leaves the target UNSTAMPED - what hand-authored content and
    // geometry.convert_to_static_mesh output both look like.
    //
    // Both assets must reach DISK (so neither spec turns bSave off): the asset registry learns a
    // dependency from a saved package's import table, never from an in-memory pointer.
    // UStaticMesh::ComplexCollisionMesh is a plain serialised UPROPERTY holding another
    // UStaticMesh, so it writes the same hard package dependency a placed StaticMeshActor would,
    // with none of a level fixture's cost.
    FAssetCreateReferencedFixture AssetCreateReferencedFixture(
        const FString& TargetPath, const FString& ReferencerPath, const FString& TargetSourcePath)
    {
        FAssetCreateReferencedFixture Fixture;

        FStaticMeshCreateSpec TargetSpec;
        TargetSpec.AssetPath = TargetPath;
        TargetSpec.SourcePath = TargetSourcePath;
        const FStaticMeshCreateResult TargetResult = CreateStaticMesh(AssetCreateBoxMesh(), TargetSpec);
        if (!TargetResult.bSuccess || !TargetResult.Asset)
        {
            return Fixture;
        }
        Fixture.Target = TargetResult.Asset;

        FStaticMeshCreateSpec ReferencerSpec;
        ReferencerSpec.AssetPath = ReferencerPath;
        const FStaticMeshCreateResult ReferencerResult =
            CreateStaticMesh(AssetCreateBoxMesh(), ReferencerSpec);
        if (!ReferencerResult.bSuccess || !ReferencerResult.Asset)
        {
            return Fixture;
        }
        Fixture.Referencer = ReferencerResult.Asset;
        Fixture.Referencer->ComplexCollisionMesh = Fixture.Target;
        Fixture.Referencer->MarkPackageDirty();
        SaveAssetToDiskReportingPresence(Fixture.Referencer, /*bForce=*/true);

        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous(
            {FPackageName::LongPackageNameToFilename(ReferencerPath, FPackageName::GetAssetPackageExtension())},
            /*bForceRescan=*/true);

        TArray<FName> Referencers;
        Registry.GetReferencers(FName(*TargetPath), Referencers,
            UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery());
        Fixture.bRegistryEdge = Referencers.Contains(FName(*ReferencerPath));

        return Fixture;
    }
}

// ============================================================================
// Creation and persistence
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateBoxSavesTest,
    "PinWright.Geometry.AssetCreate.BoxCreatesAssetOnDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateBoxSavesTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    // The one test that leaves bSave at its true default: persistence is what it asserts.
    FStaticMeshCreateSpec Spec;
    Spec.AssetPath = AssetPath;
    Spec.bRecomputeNormals = true;
    Spec.bRecomputeTangents = true;

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);

    TestTrue(TEXT("a box creates a StaticMesh asset"), Result.bSuccess);
    TestTrue(TEXT("the created asset is returned"), Result.Asset != nullptr);
    TestTrue(TEXT("the source triangle count is reported"), Result.TriangleCount > 0);
    TestTrue(TEXT("the asset reports itself saved to disk"), Result.bSavedToDisk);
    TestFalse(TEXT("a completed save owes no flush"), Result.bPendingFlush);

    // The in-memory object proves nothing about persistence: probe the file.
    FString Filename;
    if (TestTrue(TEXT("the package name resolves to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                Result.PackageName, Filename, FPackageName::GetAssetPackageExtension())))
    {
        TestTrue(TEXT("the .uasset is on disk"), IFileManager::Get().FileSize(*Filename) > 0);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// Material slots
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateNamedSlotsTest,
    "PinWright.Geometry.AssetCreate.NamedSlotsAreCreatedInOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateNamedSlotsTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AssetCreateProbeMaterial);

    const FString AssetPath = AssetCreateUniquePath();

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.MaterialSlots = {TEXT("Shell"), TEXT("Trim")};
    Spec.MaterialBindings.Add(TEXT("Shell"), AssetCreateProbeMaterial);
    Spec.MaterialBindings.Add(TEXT("Trim"), AssetCreateProbeMaterial);

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);

    if (!TestTrue(TEXT("two named slots still create the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const TArray<FStaticMaterial>& Materials = Result.Asset->GetStaticMaterials();
    if (TestTrue(TEXT("two slots produce at least two FStaticMaterial entries"), Materials.Num() >= 2))
    {
        TestEqual(TEXT("slot 0 carries the first document name"),
            Materials[0].MaterialSlotName, FName(TEXT("Shell")));
        TestEqual(TEXT("slot 1 carries the second document name"),
            Materials[1].MaterialSlotName, FName(TEXT("Trim")));

        // Identity, not merely non-null. CreateStaticMeshAsset populates every slot it makes,
        // so a binding that silently fell back to the default surface material - the exact
        // "slots were never populated" state this path replaced - still leaves
        // MaterialInterface != nullptr and passes a bare null check. Compare against the
        // material the spec asked for, on BOTH slots: NumMaterialSlots and AssetMaterials are
        // parallel arrays (GeometryAssetCreate.cpp), so a length/index slip shows up here.
        UMaterialInterface* const Requested =
            LoadObject<UMaterialInterface>(nullptr, AssetCreateProbeMaterial);
        if (TestNotNull(TEXT("the probe material loads"), Requested))
        {
            TestTrue(TEXT("slot 0 holds the material the spec bound to it"),
                Materials[0].MaterialInterface == Requested);
            TestTrue(TEXT("slot 1 holds the material the spec bound to it"),
                Materials[1].MaterialInterface == Requested);
        }
    }
    TestEqual(TEXT("every slot was bound"), Result.UnboundSlots.Num(), 0);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateUnboundSlotTest,
    "PinWright.Geometry.AssetCreate.UnboundSlotIsReportedAndStillCreates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateUnboundSlotTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.MaterialSlots = {TEXT("Shell")};
    // No binding for "Shell": an unassigned slot is a normal authoring state, not an error.

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);

    TestTrue(TEXT("an unbound slot still creates the asset"), Result.bSuccess);
    if (TestEqual(TEXT("the unbound slot is reported"), Result.UnboundSlots.Num(), 1))
    {
        TestEqual(TEXT("the reported slot is the unbound one"), Result.UnboundSlots[0], FString(TEXT("Shell")));
    }
    if (Result.Asset)
    {
        const TArray<FStaticMaterial>& Materials = Result.Asset->GetStaticMaterials();
        if (TestTrue(TEXT("the unbound slot still exists on the asset"), Materials.Num() >= 1))
        {
            TestEqual(TEXT("the unbound slot keeps its document name"),
                Materials[0].MaterialSlotName, FName(TEXT("Shell")));
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// UV channels and the lightmap index
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateKeepsUVChannelsTest,
    "PinWright.Geometry.AssetCreate.TwoUVChannelMeshKeepsBoth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateKeepsUVChannelsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    UDynamicMesh* Mesh = AssetCreateBoxMesh();
    AssetCreateProjectUVs(Mesh, 1);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);

    const FStaticMeshCreateResult Result = CreateStaticMesh(Mesh, Spec);
    if (!TestTrue(TEXT("a two-UV-channel mesh creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // The truncating form of the crash guard (bare SetNumUVSets(Mesh, 1)) would have dropped
    // channel 1 here, and nothing downstream would have reported it.
    const FMeshDescription* Baked = Result.Asset->GetMeshDescription(0);
    if (TestNotNull(TEXT("the baked asset has a LOD0 mesh description"), Baked))
    {
        const FStaticMeshConstAttributes Attributes(*Baked);
        TestTrue(TEXT("both UV channels survived the bake"),
            Attributes.GetVertexInstanceUVs().GetNumChannels() >= 2);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateLightmapChannelTest,
    "PinWright.Geometry.AssetCreate.LightmapChannelSurvivesOnATwoChannelMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateLightmapChannelTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    UDynamicMesh* Mesh = AssetCreateBoxMesh();
    AssetCreateProjectUVs(Mesh, 1);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.LightMapChannel = 1;
    // A resolution as well, so this stays the fully-specified request that warns about nothing.
    // A channel with no resolution is now itself a reportable case and has its own test below.
    Spec.LightMapResolution = 128;

    const FStaticMeshCreateResult Result = CreateStaticMesh(Mesh, Spec);
    if (!TestTrue(TEXT("a lightmap channel request creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);
    TestEqual(TEXT("the requested lightmap channel survived EnforceLightmapRestrictions"),
        Result.Asset->GetLightMapCoordinateIndex(), 1);
    TestEqual(TEXT("an honoured lightmap request warns about nothing"), Result.Warnings.Num(), 0);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateLightmapResolutionTest,
    "PinWright.Geometry.AssetCreate.LightmapResolutionReachesTheBuiltAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateLightmapResolutionTest::RunTest(const FString& Parameters)
{
    // Read off the BUILT asset, not off the spec: PostEditChange runs the build, and
    // EnforceLightmapRestrictions is free to move the value (it raises anything under 4 to 4,
    // StaticMesh.cpp:9732). A spec-struct assertion would pass on a path that wrote nothing.
    const FString AssetPath = AssetCreateUniquePath();

    UDynamicMesh* Mesh = AssetCreateBoxMesh();
    AssetCreateProjectUVs(Mesh, 1);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.LightMapChannel = 1;
    Spec.LightMapResolution = 256;

    const FStaticMeshCreateResult Result = CreateStaticMesh(Mesh, Spec);
    if (!TestTrue(TEXT("a lightmap resolution request creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);
    TestEqual(TEXT("the requested lightmap resolution is on the built asset"),
        Result.Asset->GetLightMapResolution(), 256);
    TestEqual(TEXT("and on the source model's build settings"),
        Result.Asset->GetSourceModel(0).BuildSettings.MinLightmapResolution, 256);
    TestEqual(TEXT("an honoured request warns about nothing"), Result.Warnings.Num(), 0);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateLightmapNoResolutionWarnsTest,
    "PinWright.Geometry.AssetCreate.LightmapChannelWithoutResolutionWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateLightmapNoResolutionWarnsTest::RunTest(const FString& Parameters)
{
    // The silent-waste case. Everything about this request succeeds - the channel is real, the
    // index survives the build, nothing is clamped - and the asset still bakes the authored
    // lightmap UVs at the bare UStaticMesh default of 4x4. Only a warning can surface it.
    const FString AssetPath = AssetCreateUniquePath();

    UDynamicMesh* Mesh = AssetCreateBoxMesh();
    AssetCreateProjectUVs(Mesh, 1);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.LightMapChannel = 1;

    const FStaticMeshCreateResult Result = CreateStaticMesh(Mesh, Spec);
    if (!TestTrue(TEXT("a resolution-less lightmap request still creates the asset"), Result.bSuccess)
        || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);
    TestEqual(TEXT("the asset keeps the UStaticMesh default of 4"),
        Result.Asset->GetLightMapResolution(), 4);

    // Matched on text, not on count: another warning appearing later must not be able to stand
    // in for this one, and deleting the message has to fail the test.
    bool bWarned = false;
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.Contains(TEXT("no lightmap resolution")) && Warning.Contains(TEXT("resolution=")))
        {
            bWarned = true;
            break;
        }
    }
    TestTrue(*FString::Printf(TEXT("the wasted lightmap UVs are reported, naming the fix. Warnings: [%s]"),
        *FString::Join(Result.Warnings, TEXT("; "))), bWarned);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateNoLightmapIsSilentTest,
    "PinWright.Geometry.AssetCreate.NoLightmapRequestIsNotNagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateNoLightmapIsSilentTest::RunTest(const FString& Parameters)
{
    // The other half of the rule above. geometry.convert_to_static_mesh callers who never
    // mentioned a lightmap must not be told their non-existent lightmap UVs are wasted -
    // a warning on every bake is a warning nobody reads.
    const FString AssetPath = AssetCreateUniquePath();

    const FStaticMeshCreateResult Result =
        CreateStaticMesh(AssetCreateBoxMesh(), AssetCreateInMemorySpec(AssetPath));
    if (!TestTrue(TEXT("a bake with no lightmap request succeeds"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);
    TestEqual(*FString::Printf(TEXT("and says nothing about lightmaps. Warnings: [%s]"),
        *FString::Join(Result.Warnings, TEXT("; "))), Result.Warnings.Num(), 0);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateLightmapClampWarnsTest,
    "PinWright.Geometry.AssetCreate.LightmapChannelWarnsWhenClamped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateLightmapClampWarnsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    // One UV channel only. EnforceLightmapRestrictions clamps the index to [0, NumUVs - 1], so
    // channel 1 cannot survive - and reporting success would be a lie the caller acts on.
    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.LightMapChannel = 1;

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);
    if (!TestTrue(TEXT("an unsatisfiable lightmap request still creates the asset"), Result.bSuccess)
        || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);
    TestTrue(TEXT("the lightmap index was clamped away from the request"),
        Result.Asset->GetLightMapCoordinateIndex() != 1);

    // Matched on text rather than on a nonzero count: this spec now also trips the
    // channel-without-resolution warning, so a bare count would let the clamp report be deleted
    // and still pass on the other warning.
    bool bClampReported = false;
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.Contains(TEXT("does not carry that UV channel")))
        {
            bClampReported = true;
            break;
        }
    }
    TestTrue(*FString::Printf(TEXT("the clamp is reported rather than claimed as success. Warnings: [%s]"),
        *FString::Join(Result.Warnings, TEXT("; "))), bClampReported);

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// Collision and vertex colors
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateCollisionTest,
    "PinWright.Geometry.AssetCreate.SimpleCollisionLandsInAggGeom",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateCollisionTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    FKAggregateGeom Geom;
    FKBoxElem Box;
    Box.X = 100.0f;
    Box.Y = 100.0f;
    Box.Z = 100.0f;
    Geom.BoxElems.Add(Box);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.SimpleCollision = Geom;
    Spec.CollisionTrace = ECollisionTraceFlag::CTF_UseSimpleAndComplex;

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);
    if (!TestTrue(TEXT("simple collision creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    TestEqual(TEXT("the collision element count is reported"), Result.CollisionElements, 1);
    UBodySetup* BodySetup = Result.Asset->GetBodySetup();
    if (TestNotNull(TEXT("the created asset owns a body setup"), BodySetup))
    {
        TestEqual(TEXT("the box element reached AggGeom"), BodySetup->AggGeom.GetElementCount(), 1);
        // The element's TYPE and EXTENTS, not just the count. A count of 1 is satisfied by a
        // default-constructed element of any kind - the spec's FKBoxElem carries the only
        // information a caller can act on, and CreateStaticMesh's job here is to move it
        // verbatim into the body setup. bCreatePhysicsBody:true also makes the engine build a
        // UBodySetup of its own, so "there is one element" says nothing about whose it is.
        if (TestEqual(TEXT("the element is a BOX, not some other primitive"),
                BodySetup->AggGeom.BoxElems.Num(), 1))
        {
            const FKBoxElem& Landed = BodySetup->AggGeom.BoxElems[0];
            TestEqual(TEXT("the box keeps its authored X extent"), (double)Landed.X, 100.0);
            TestEqual(TEXT("the box keeps its authored Y extent"), (double)Landed.Y, 100.0);
            TestEqual(TEXT("the box keeps its authored Z extent"), (double)Landed.Z, 100.0);
        }
        TestEqual(TEXT("the requested trace flag reached the body setup"),
            static_cast<int32>(BodySetup->CollisionTraceFlag),
            static_cast<int32>(ECollisionTraceFlag::CTF_UseSimpleAndComplex));
    }

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateVertexColorsTest,
    "PinWright.Geometry.AssetCreate.VertexColorsReachTheBakedMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateVertexColorsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    UDynamicMesh* Mesh = AssetCreateBoxMesh();
    GeometryOps::FSetVertexColorParams ColorParams;
    ColorParams.bSetAll = true;
    ColorParams.Color = FLinearColor(0.25f, 0.5f, 0.75f, 1.0f);
    int32 VerticesModified = 0;
    GeometryOps::SetVertexColor(Mesh, ColorParams, VerticesModified);

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);

    const FStaticMeshCreateResult Result = CreateStaticMesh(Mesh, Spec);
    if (!TestTrue(TEXT("a coloured mesh creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    AssetCreateFinishBuild(Result.Asset);

    const FMeshDescription* Baked = Result.Asset->GetMeshDescription(0);
    if (TestNotNull(TEXT("the baked asset has a LOD0 mesh description"), Baked))
    {
        const FStaticMeshConstAttributes Attributes(*Baked);
        const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
        TestTrue(TEXT("the bake carried a vertex colour attribute"),
            Colors.IsValid() && Colors.GetNumElements() > 0);

        // ...and it carried the VALUES, not merely an allocated channel. The attribute existing
        // is satisfied by a bake that allocates the colour channel and writes nothing (or writes
        // flat white) into it - which is the silent-untextured shape this file exists to guard
        // elsewhere. The authored colour (0.25, 0.5, 0.75) is strictly increasing across R/G/B,
        // and every colour-space conversion the bake can apply is monotonic, so R < G < B
        // survives whatever encoding lands while a dropped or flat colour (R == G == B) does not.
        if (Colors.IsValid())
        {
            bool bCheckedAColour = false;
            for (const FVertexInstanceID InstanceID : Baked->VertexInstances().GetElementIDs())
            {
                const FVector4f Colour = Colors[InstanceID];
                TestTrue(*FString::Printf(
                    TEXT("the baked vertex colour keeps the authored R<G<B ordering (got %.3f, %.3f, %.3f)"),
                    Colour.X, Colour.Y, Colour.Z),
                    Colour.X < Colour.Y && Colour.Y < Colour.Z);
                bCheckedAColour = true;
                break;
            }
            TestTrue(TEXT("the baked mesh description has a vertex instance to read a colour from"),
                bCheckedAColour);
        }
    }

    if (const FStaticMeshRenderData* RenderData = Result.Asset->GetRenderData())
    {
        if (RenderData->LODResources.Num() > 0)
        {
            TestTrue(TEXT("the built LOD carries a non-empty FColorVertexBuffer"),
                RenderData->LODResources[0].VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0);
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// Provenance: all four cases the overwrite rule is derived from.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateProvenanceAbsentTest,
    "PinWright.Geometry.AssetCreate.ProvenanceAbsentPathCreatesAndStamps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateProvenanceAbsentTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    FStaticMeshCreateSpec Spec = AssetCreateInMemorySpec(AssetPath);
    Spec.SourcePath = TEXT("Content/Models/bracket.pwmodel");
    Spec.SourceHash = TEXT("deadbeef");

    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Spec);
    if (!TestTrue(TEXT("an empty path creates the asset"), Result.bSuccess) || !Result.Asset)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const UPwModelAssetUserData* Stamp = Cast<UPwModelAssetUserData>(
        Result.Asset->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
    if (TestNotNull(TEXT("the created asset carries a provenance stamp"), Stamp))
    {
        TestEqual(TEXT("the stamp records the source path"),
            Stamp->SourcePath, FString(TEXT("Content/Models/bracket.pwmodel")));
        TestEqual(TEXT("the stamp records the source hash"), Stamp->SourceHash, FString(TEXT("deadbeef")));
    }

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateProvenanceSameSourceTest,
    "PinWright.Geometry.AssetCreate.ProvenanceSameSourceOverwritesWithoutAFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateProvenanceSameSourceTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();
    const FString SourcePath = TEXT("Content/Models/bracket.pwmodel");

    FStaticMeshCreateSpec First = AssetCreateInMemorySpec(AssetPath);
    First.SourcePath = SourcePath;
    const FStaticMeshCreateResult FirstResult = CreateStaticMesh(AssetCreateBoxMesh(), First);
    if (!TestTrue(TEXT("the first compile creates the asset"), FirstResult.bSuccess))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    TestFalse(TEXT("the first compile is a create, not an update"), FirstResult.bUpdatedInPlace);

    // Recompiling a source onto its own output IS the iteration loop. Needing a flag for it
    // would make overwrite=true the habitual argument, at which point it stops carrying
    // information about the destructive case.
    FStaticMeshCreateSpec Second = AssetCreateInMemorySpec(AssetPath);
    Second.SourcePath = SourcePath;
    Second.bOverwrite = false;
    const FStaticMeshCreateResult SecondResult = CreateStaticMesh(AssetCreateBoxMesh(), Second);

    TestTrue(TEXT("the same source recompiles onto its own output with no flag"), SecondResult.bSuccess);
    TestEqual(TEXT("a permitted recompile reports no error code"), SecondResult.ErrorCode, FString());
    // ...by REBUILDING the occupant, not by deleting it and creating a new one. The address is
    // the discriminator: StaticAllocateObject replaces a same-class occupant in place, so a
    // recompile that took the delete-then-create branch would hand back a different object.
    TestTrue(TEXT("the recompile reports itself as an update in place"), SecondResult.bUpdatedInPlace);
    TestTrue(TEXT("the recompile rebuilds the SAME UStaticMesh object"),
        SecondResult.Asset == FirstResult.Asset);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateProvenanceDifferentSourceTest,
    "PinWright.Geometry.AssetCreate.ProvenanceDifferentSourceRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateProvenanceDifferentSourceTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    FStaticMeshCreateSpec First = AssetCreateInMemorySpec(AssetPath);
    First.SourcePath = TEXT("Content/Models/bracket.pwmodel");
    const FStaticMeshCreateResult FirstResult =
        CreateStaticMesh(AssetCreateBoxMesh(), First);
    if (!TestTrue(TEXT("the first compile creates the asset"), FirstResult.bSuccess)
        || !TestNotNull(TEXT("the original static mesh is available"), FirstResult.Asset))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(FirstResult.Asset);
    Socket->SocketName = FName(TEXT("Attachment"));
    FirstResult.Asset->Sockets.Add(Socket);

    FStaticMeshCreateSpec Second = AssetCreateInMemorySpec(AssetPath);
    Second.SourcePath = TEXT("Content/Models/hinge.pwmodel");
    const FStaticMeshCreateResult SecondResult = CreateStaticMesh(AssetCreateBoxMesh(), Second);

    TestFalse(TEXT("a different source does not silently overwrite"), SecondResult.bSuccess);
    TestEqual(TEXT("a different source refuses with ASSET_ALREADY_EXISTS"),
        SecondResult.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));
    const FPwDiagnostic* RefusalDiagnostic = SecondResult.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    TestNotNull(TEXT("the static takeover refusal carries the guard code"), RefusalDiagnostic);
    if (RefusalDiagnostic)
    {
        TestEqual(TEXT("the blocked static takeover is an error"),
            RefusalDiagnostic->Severity, EPwSeverity::Error);
        TestTrue(TEXT("the static takeover refusal names the socket"),
            RefusalDiagnostic->Message.Contains(TEXT("socket[Attachment]")));
        TestTrue(TEXT("the ownership refusal includes the state that would be lost"),
            SecondResult.ErrorMessage.Contains(TEXT("socket[Attachment]")));
    }

    // ...and the flag is the documented way through.
    Second.bOverwrite = true;
    const FStaticMeshCreateResult Allowed = CreateStaticMesh(AssetCreateBoxMesh(), Second);
    TestTrue(TEXT("overwrite=true is the way through"), Allowed.bSuccess);
    const FPwDiagnostic* WarningDiagnostic = Allowed.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    TestNotNull(TEXT("the permitted static takeover keeps the guard code visible"),
        WarningDiagnostic);
    if (WarningDiagnostic)
    {
        TestEqual(TEXT("the permitted static takeover is a warning"),
            WarningDiagnostic->Severity, EPwSeverity::Warning);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateProvenanceUnstampedTest,
    "PinWright.Geometry.AssetCreate.ProvenanceUnstampedRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateProvenanceUnstampedTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = AssetCreateUniquePath();

    // No SourcePath: this is what geometry.convert_to_static_mesh produces, and it is exactly
    // the asset a later compile must not assume it owns.
    FStaticMeshCreateSpec First = AssetCreateInMemorySpec(AssetPath);
    if (!TestTrue(TEXT("an unstamped asset is created"),
            CreateStaticMesh(AssetCreateBoxMesh(), First).bSuccess))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FStaticMeshCreateSpec Second = AssetCreateInMemorySpec(AssetPath);
    Second.SourcePath = TEXT("Content/Models/bracket.pwmodel");
    const FStaticMeshCreateResult SecondResult = CreateStaticMesh(AssetCreateBoxMesh(), Second);

    TestFalse(TEXT("an unstamped asset is not silently overwritten"), SecondResult.bSuccess);
    TestEqual(TEXT("an unstamped asset refuses with ASSET_ALREADY_EXISTS"),
        SecondResult.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// One asset, two spellings. The live side of a material slot is read with
// GetPathName(), i.e. the OBJECT path /Pkg.Object; a .pwmodel binds a material
// by the PACKAGE path /Pkg, and there is no source spelling that produces the
// object form. Comparing the two raw made the guard fire on an asset whose
// source ALREADY named the live material, so the remedy the diagnostic prints -
// "put the state in the source" - could not be carried out and overwrite=true
// was the only way past. That turns the flag into a habit and the guard's signal
// stops distinguishing a real conflict from a reconciled one.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateMaterialPathShapeTest,
    "PinWright.Geometry.AssetCreate.ReconciledMaterialPathIsNotUnmanagedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateMaterialPathShapeTest::RunTest(const FString& Parameters)
{
    // A second material, so the out-of-band write is a real change rather than a no-op.
    const TCHAR* OutOfBandMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AssetCreateProbeMaterial);
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(OutOfBandMaterialPath);

    const FString AssetPath = AssetCreateUniquePath();
    const FString SourcePath = TEXT("Content/Models/material-path.pwmodel");
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    FStaticMeshCreateSpec First = AssetCreateInMemorySpec(AssetPath);
    First.SourcePath = SourcePath;
    First.MaterialSlots = {TEXT("Shell")};
    First.MaterialBindings.Add(TEXT("Shell"), AssetCreateProbeMaterial);
    const FStaticMeshCreateResult FirstResult = CreateStaticMesh(AssetCreateBoxMesh(), First);
    if (!TestTrue(TEXT("the first compile creates the asset"), FirstResult.bSuccess)
        || !TestNotNull(TEXT("the compiled mesh is available"), FirstResult.Asset))
    {
        return true;
    }

    // The out-of-band write the guard exists for, made the way static_mesh.set_material makes it.
    UMaterialInterface* OutOfBandMaterial =
        LoadObject<UMaterialInterface>(nullptr, OutOfBandMaterialPath);
    if (!TestNotNull(TEXT("the out-of-band material loads"), OutOfBandMaterial))
    {
        return true;
    }
    FirstResult.Asset->SetMaterial(0, OutOfBandMaterial);
    // SetMaterial's PostEditChangeProperty starts a rebuild; let it land so the guard below reads
    // settled state rather than a mesh mid-compile.
    AssetCreateFinishBuild(FirstResult.Asset);

    // Half one - the guard still has to fire. The source names a DIFFERENT material from the one
    // now on the asset, so recompiling really would discard someone else's deliberate choice.
    FStaticMeshCreateSpec Stale = AssetCreateInMemorySpec(AssetPath);
    Stale.SourcePath = SourcePath;
    Stale.MaterialSlots = {TEXT("Shell")};
    Stale.MaterialBindings.Add(TEXT("Shell"), AssetCreateProbeMaterial);
    const FStaticMeshCreateResult StaleResult = CreateStaticMesh(AssetCreateBoxMesh(), Stale);
    TestFalse(TEXT("a recompile that would discard the out-of-band material is refused"),
        StaleResult.bSuccess);
    const FPwDiagnostic* RefusalDiagnostic = StaleResult.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    if (TestNotNull(TEXT("genuinely unmanaged material state carries the guard code"),
            RefusalDiagnostic))
    {
        TestEqual(TEXT("the blocked recompile is an error"),
            RefusalDiagnostic->Severity, EPwSeverity::Error);
        TestTrue(TEXT("the refusal names the material slot"),
            RefusalDiagnostic->Message.Contains(TEXT("material[Shell]")));
    }

    // Half two - the same asset, after doing exactly what that diagnostic asks. The source now
    // names the live material; only the SPELLING differs, so there is no state left to discard.
    FStaticMeshCreateSpec Reconciled = AssetCreateInMemorySpec(AssetPath);
    Reconciled.SourcePath = SourcePath;
    Reconciled.MaterialSlots = {TEXT("Shell")};
    Reconciled.MaterialBindings.Add(TEXT("Shell"), OutOfBandMaterialPath);
    const FStaticMeshCreateResult ReconciledResult =
        CreateStaticMesh(AssetCreateBoxMesh(), Reconciled);

    const FPwDiagnostic* SpuriousDiagnostic = ReconciledResult.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    TestNull(TEXT("a package path and an object path naming ONE material are not a loss"),
        SpuriousDiagnostic);
    TestTrue(TEXT("the reconciled recompile succeeds without overwrite"),
        ReconciledResult.bSuccess);
    TestEqual(TEXT("the reconciled recompile reports no error code"),
        ReconciledResult.ErrorCode, FString());

    return true;
}

// ============================================================================
// Iteration on an asset that is actually in use - the case the headline claim
// ("same source overwrites freely, that is iteration") is about.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateReferencedRecompileTest,
    "PinWright.Geometry.AssetCreate.ReferencedAssetStillRecompilesFromTheSameSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateReferencedRecompileTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = AssetCreateUniquePath();
    const FString ReferencerPath = AssetCreateUniquePath();
    const FString SourcePath = TEXT("Content/Models/bracket.pwmodel");

    UStaticMesh* Referencer = nullptr;
    ON_SCOPE_EXIT
    {
        // The referencing asset goes first: deleting the target while something still points at
        // it is the situation this test exists to avoid producing.
        if (Referencer)
        {
            Referencer->ComplexCollisionMesh = nullptr;
        }
        CleanupTestAsset(ReferencerPath);
        CleanupTestAsset(TargetPath);
    };

    // 1. Compile the model. It has to reach disk: the asset registry learns a dependency from a
    //    SAVED package's import table, never from an in-memory pointer.
    FStaticMeshCreateSpec First;
    First.AssetPath = TargetPath;
    First.SourcePath = SourcePath;
    const FStaticMeshCreateResult FirstResult = CreateStaticMesh(AssetCreateBoxMesh(), First);
    if (!TestTrue(TEXT("the first compile creates the asset"), FirstResult.bSuccess)
        || !TestNotNull(TEXT("the first compile returns the asset"), FirstResult.Asset))
    {
        return true;
    }

    // 2. Put it to use. UStaticMesh::ComplexCollisionMesh is a plain serialised UPROPERTY holding
    //    another UStaticMesh, so saving the holder writes a hard package dependency onto the
    //    target - the same registry edge a placed StaticMeshActor produces, with none of a level
    //    fixture's cost.
    FStaticMeshCreateSpec ReferencerSpec;
    ReferencerSpec.AssetPath = ReferencerPath;
    const FStaticMeshCreateResult ReferencerResult = CreateStaticMesh(AssetCreateBoxMesh(), ReferencerSpec);
    if (!TestTrue(TEXT("the referencing probe asset is created"), ReferencerResult.bSuccess)
        || !TestNotNull(TEXT("the referencing probe asset is returned"), ReferencerResult.Asset))
    {
        return true;
    }
    Referencer = ReferencerResult.Asset;
    Referencer->ComplexCollisionMesh = FirstResult.Asset;
    Referencer->MarkPackageDirty();
    SaveAssetToDiskReportingPresence(Referencer, /*bForce=*/true);

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanFilesSynchronous(
        {FPackageName::LongPackageNameToFilename(ReferencerPath, FPackageName::GetAssetPackageExtension())},
        /*bForceRescan=*/true);

    // Assert the fixture rather than assume it: with no registry edge the recompile below would
    // pass for the wrong reason, and this test would pin nothing.
    TArray<FName> Referencers;
    Registry.GetReferencers(FName(*TargetPath), Referencers,
        UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery());
    if (!TestTrue(TEXT("the asset registry reports the compiled asset as referenced"),
            Referencers.Contains(FName(*ReferencerPath))))
    {
        return true;
    }

    // 3. Recompile from the SAME source, with no flag. This is the whole loop: edit the .pwmodel,
    //    compile again. Hardcoding overwrite into Resolve made this fail ASSET_IN_USE from the
    //    moment step 2 happened - i.e. from the moment the model was used for anything.
    FStaticMeshCreateSpec Second = AssetCreateInMemorySpec(TargetPath);
    Second.SourcePath = SourcePath;
    const FStaticMeshCreateResult SecondResult = CreateStaticMesh(AssetCreateBoxMesh(), Second);

    TestNotEqual(TEXT("a referenced same-source recompile is not refused as in-use"),
        SecondResult.ErrorCode, FString(TEXT("ASSET_IN_USE")));
    TestEqual(TEXT("a referenced same-source recompile reports no error at all"),
        SecondResult.ErrorCode, FString());
    if (!TestTrue(TEXT("a referenced same-source recompile succeeds"), SecondResult.bSuccess))
    {
        return true;
    }

    // 4. And it kept the referencer whole, which is what UpdateInPlace buys over delete-and-create.
    TestTrue(TEXT("the recompile reports itself as an update in place"), SecondResult.bUpdatedInPlace);
    TestTrue(TEXT("the recompile rebuilds the SAME UStaticMesh object"),
        SecondResult.Asset == FirstResult.Asset);
    TestTrue(TEXT("the referencing asset still resolves to the recompiled mesh"),
        Referencer->ComplexCollisionMesh == SecondResult.Asset);

    return true;
}

// ============================================================================
// The intersection none of the tests above reached: an asset that is BOTH referenced AND not
// ours. That combination is what deadlocked.
//
// ReferencedAssetStillRecompilesFromTheSameSource, directly above, is the closest neighbour and
// is the test whose gap let this ship. It covers referenced + SAME source, so it only ever
// exercises the branch that needs no flag. The three provenance tests cover unstamped and
// foreign-stamped targets, but always UNREFERENCED ones, so their `overwrite=true` succeeded on
// Resolve's delete-then-recreate branch. Neither family put an asset in the state where both
// gates fire at once - which is not an edge case but the FIRST compile of every migration of
// existing, already-placed content onto .pwmodel:
//
//   overwrite=true    -> ASSET_IN_USE      "drop overwrite ... to update the existing asset in place"
//   overwrite omitted -> ASSET_ALREADY_EXISTS  "pass overwrite=true"
//
// Each rejection named the other as the remedy, so no argument worked. Both messages were
// individually accurate, and neither could detect the other firing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateReferencedUnstampedOverwriteTest,
    "PinWright.Geometry.AssetCreate.ReferencedUnstampedAssetRebuildsInPlaceWithOverwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateReferencedUnstampedOverwriteTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = AssetCreateUniquePath();
    const FString ReferencerPath = AssetCreateUniquePath();
    const FString MigratingSource = TEXT("Content/Models/bracket.pwmodel");

    UStaticMesh* Referencer = nullptr;
    ON_SCOPE_EXIT
    {
        // The referencing asset goes first, for the same reason as above: deleting the target
        // while something still points at it is what this test exists to avoid producing.
        if (Referencer)
        {
            Referencer->ComplexCollisionMesh = nullptr;
        }
        CleanupTestAsset(ReferencerPath);
        CleanupTestAsset(TargetPath);
    };

    const FAssetCreateReferencedFixture Fixture =
        AssetCreateReferencedFixture(TargetPath, ReferencerPath, /*TargetSourcePath=*/FString());
    Referencer = Fixture.Referencer;
    if (!TestNotNull(TEXT("the unstamped target asset is created"), Fixture.Target)
        || !TestNotNull(TEXT("the referencing probe asset is created"), Fixture.Referencer)
        || !TestTrue(TEXT("the asset registry reports the target as referenced"), Fixture.bRegistryEdge))
    {
        return true;
    }
    TestNull(TEXT("the fixture target really carries no provenance stamp"),
        Fixture.Target->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));

    // Permission is still REQUIRED. The fix separates permission from mechanism; it does not
    // remove the gate, and a test that only proved the flag works would not notice if it had.
    FStaticMeshCreateSpec NoFlag = AssetCreateInMemorySpec(TargetPath);
    NoFlag.SourcePath = MigratingSource;
    const FStaticMeshCreateResult NoFlagResult = CreateStaticMesh(AssetCreateBoxMesh(), NoFlag);
    TestFalse(TEXT("a referenced unstamped asset is still not taken without the flag"),
        NoFlagResult.bSuccess);
    TestEqual(TEXT("the refusal is the provenance one, not ASSET_IN_USE"),
        NoFlagResult.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));
    TestTrue(TEXT("the refusal names overwrite=true - the remedy that must now work"),
        NoFlagResult.ErrorMessage.Contains(TEXT("overwrite=true")));

    // ...and taking that remedy on THIS asset - referenced, which is the normal case - must not
    // hand back the other gate's rejection.
    FStaticMeshCreateSpec Migration = AssetCreateInMemorySpec(TargetPath);
    Migration.SourcePath = MigratingSource;
    Migration.bOverwrite = true;
    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Migration);

    TestNotEqual(TEXT("overwrite on a referenced asset is not refused as in-use"),
        Result.ErrorCode, FString(TEXT("ASSET_IN_USE")));
    TestEqual(TEXT("the migrating compile reports no error at all"), Result.ErrorCode, FString());
    if (!TestTrue(TEXT("overwrite=true migrates a referenced unstamped asset"), Result.bSuccess))
    {
        return true;
    }

    // By the in-place mechanism, which is what makes granting permission safe here. The object
    // address is the discriminator: delete-then-create would return a different UStaticMesh and
    // orphan the referencer.
    TestTrue(TEXT("overwrite still reports itself as an update in place"), Result.bUpdatedInPlace);
    TestTrue(TEXT("overwrite rebuilds the SAME UStaticMesh object"), Result.Asset == Fixture.Target);
    TestTrue(TEXT("the referencing asset still resolves to the rebuilt mesh"),
        Referencer->ComplexCollisionMesh == Result.Asset);

    // And the asset is now stamped, so the next recompile of this source needs no flag - which
    // is what keeps overwrite from becoming the habitual argument.
    const UPwModelAssetUserData* Stamp = Cast<UPwModelAssetUserData>(
        Result.Asset->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
    if (TestNotNull(TEXT("the migrated asset carries a provenance stamp"), Stamp))
    {
        TestEqual(TEXT("the stamp records the migrating source"), Stamp->SourcePath, MigratingSource);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryAssetCreateReferencedForeignSourceOverwriteTest,
    "PinWright.Geometry.AssetCreate.ReferencedForeignSourceAssetRebuildsInPlaceWithOverwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryAssetCreateReferencedForeignSourceOverwriteTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = AssetCreateUniquePath();
    const FString ReferencerPath = AssetCreateUniquePath();
    const FString OriginalSource = TEXT("Content/Models/hinge.pwmodel");
    const FString ClaimingSource = TEXT("Content/Models/bracket.pwmodel");

    UStaticMesh* Referencer = nullptr;
    ON_SCOPE_EXIT
    {
        if (Referencer)
        {
            Referencer->ComplexCollisionMesh = nullptr;
        }
        CleanupTestAsset(ReferencerPath);
        CleanupTestAsset(TargetPath);
    };

    // The other half of the deadlock: a stamped occupant, just not stamped with OUR source.
    // ProvenanceDifferentSourceRefuses proves overwrite=true is the way through, but only on an
    // unreferenced asset, where the delete branch was still available.
    const FAssetCreateReferencedFixture Fixture =
        AssetCreateReferencedFixture(TargetPath, ReferencerPath, OriginalSource);
    Referencer = Fixture.Referencer;
    if (!TestNotNull(TEXT("the foreign-stamped target asset is created"), Fixture.Target)
        || !TestNotNull(TEXT("the referencing probe asset is created"), Fixture.Referencer)
        || !TestTrue(TEXT("the asset registry reports the target as referenced"), Fixture.bRegistryEdge))
    {
        return true;
    }

    FStaticMeshCreateSpec Claim = AssetCreateInMemorySpec(TargetPath);
    Claim.SourcePath = ClaimingSource;
    Claim.bOverwrite = true;
    const FStaticMeshCreateResult Result = CreateStaticMesh(AssetCreateBoxMesh(), Claim);

    TestNotEqual(TEXT("overwrite on a referenced asset is not refused as in-use"),
        Result.ErrorCode, FString(TEXT("ASSET_IN_USE")));
    TestEqual(TEXT("the claiming compile reports no error at all"), Result.ErrorCode, FString());
    if (!TestTrue(TEXT("overwrite=true claims a referenced foreign-stamped asset"), Result.bSuccess))
    {
        return true;
    }

    TestTrue(TEXT("overwrite still reports itself as an update in place"), Result.bUpdatedInPlace);
    TestTrue(TEXT("overwrite rebuilds the SAME UStaticMesh object"), Result.Asset == Fixture.Target);
    TestTrue(TEXT("the referencing asset still resolves to the rebuilt mesh"),
        Referencer->ComplexCollisionMesh == Result.Asset);

    // The stamp must be REPLACED, not appended to: a second stamp would leave the old source
    // matching first and hand ownership back to a document that no longer produces this asset.
    const UPwModelAssetUserData* Stamp = Cast<UPwModelAssetUserData>(
        Result.Asset->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
    if (TestNotNull(TEXT("the claimed asset carries a provenance stamp"), Stamp))
    {
        TestEqual(TEXT("the stamp now records the claiming source"), Stamp->SourcePath, ClaimingSource);
    }

    return true;
}

// ============================================================================
// Dispatcher level: the shipped verb's refusal on an occupied path.
//
// geometry.convert_to_static_mesh's create branch routes through CreateStaticMesh, so it now
// refuses an occupied path with ASSET_ALREADY_EXISTS where it previously wrote straight through
// whatever was there. Every other dispatcher test for this verb bakes to a GUID-unique path or
// passes overwrite:true, so none of them would notice that contract changing back.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertOccupiedPathRefusesTest,
    "PinWright.geometry.convert_to_static_mesh.OccupiedPathRefusesWithoutOverwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertOccupiedPathRefusesTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert_to_static_mesh occupied-path test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_ConvertOccupied_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/GeneratedMeshes/%s"), *Label);

    ON_SCOPE_EXIT
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        CleanupTestAsset(AssetPath);
    };

    // Occupy the target path with an asset this path did not generate - no provenance stamp, which
    // is exactly what a hand-authored or previously converted mesh looks like.
    const FStaticMeshCreateResult Squatter =
        CreateStaticMesh(AssetCreateBoxMesh(), AssetCreateInMemorySpec(AssetPath));
    if (!TestTrue(TEXT("the squatting StaticMesh is created"), Squatter.bSuccess)
        || !TestNotNull(TEXT("the squatting StaticMesh is returned"), Squatter.Asset))
    {
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("name"), Label);
    bool bCreated = false;
    FString CreateErr;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
        TEXT("req-convert-occupied-create"), CreateParams, bCreated, CreateErr);
    if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
    {
        return true;
    }

    // No overwrite: the verb documents that the create path refuses an occupied path rather than
    // replacing an asset it did not generate.
    TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
    ConvertParams->SetStringField(TEXT("actorName"), Label);
    ConvertParams->SetStringField(TEXT("assetPath"), AssetPath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"),
        TEXT("req-convert-occupied"), ConvertParams, bSuccess, Result, ErrorCode);

    TestFalse(TEXT("baking onto an occupied path without overwrite fails"), bSuccess);
    TestEqual(TEXT("the refusal is ASSET_ALREADY_EXISTS"),
        ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));

    // A refusal that still replaced the asset would be the worst of both: the occupant must be
    // the same object it was before the call.
    TestTrue(TEXT("the refused bake left the occupying asset in place"),
        FindObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath)) == Squatter.Asset);

    return true;
}
