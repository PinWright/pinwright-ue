// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-spawned-volumes-have-no-brush-geometry.
//
// THE DEFECT. An AVolume keeps its shape in a UModel brush, not in its transform. Two
// verbs spawned AVolume subclasses through a raw World->SpawnActor and never built that
// model: actor.spawn (measured on APCGVolume) and foliage.create_procedural
// (AProceduralFoliageVolume). Both reported success. Both produced an actor with bounds
// extent (0,0,0), no collision, and no containment at all — and nothing downstream
// noticed, because UEditorBrushBuilder::EndBrush early-returns SUCCESS on a null model
// (EditorBrushBuilder.cpp:82-86). foliage.create_procedural additionally wrote the
// requested size as SetActorScale3D(Size / 200) against a "default extent of 100 units"
// the never-built brush does not have, so the scatter sampled an empty region and
// reported instances_spawned: 0 as if the ground were bare.
//
// WHY THESE ASSERT CONTAINMENT AND NOT EXISTENCE. A test that asserts the actor spawned
// passes against the broken handlers — the actor always spawned, that was the whole
// trap. The load-bearing assertion is AVolume::EncompassesPoint, which reaches
// UBrushComponent::GetSquaredDistanceToCollision -> FBodyInstance::GetSquaredDistanceToBody
// (Volume.cpp:84-116). With no brush there are no convex hulls, GetSquaredDistanceToBody
// returns false with no valid body, and EncompassesPoint answers false for a point at the
// volume's own centre. So every "inside" assertion below fails against the pre-fix
// handlers and cannot pass vacuously.
//
// The second foliage assertion is the actor scale. Building the brush is necessary and
// not sufficient: a leftover SetActorScale3D(Size / 200) multiplies the new geometry, and
// a volume ~100x too large samples the whole level instead of nothing — the worse of the
// two failures for a scatter host.
//
// THE FOURTH SITE, B-lightmass-volume-no-brush-geometry, is covered by the two
// lighting.create_lightmass_volume tests at the bottom of this file. Same mechanism, but
// there a phantom is worse than no volume at all rather than merely inert — the engine
// gates its scene-bounds fallback on the importance-volume COUNT, so an extent-less volume
// suppresses a correct auto-synthesized region. That verb therefore also has to REFUSE
// rather than leave one standing, which the second test asserts.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Components/BrushComponent.h"
#include "Editor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Volume.h"
#include "Lightmass/LightmassImportanceVolume.h"
#include "Model.h"
#include "ProceduralFoliageVolume.h"
#include "UObject/Package.h"

#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Volume/VolumeBrushGeometry.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity
// builds enabled, where same-named anonymous-namespace helpers collide across merged
// translation units.
namespace SpawnedVolumeBrushGeometryTestHelpers
{
    // Well clear of the host project's open map on every axis, so no other actor's
    // geometry can be what a containment query answers about.
    const FVector ProbeOrigin(120000.0, 120000.0, 60000.0);

    // Asserts the containment contract that a zero-extent phantom cannot satisfy:
    // the volume's own centre is inside it, and a point beyond its stated half-extent
    // is not. Pre-fix, EncompassesPoint answers false for BOTH and the first fails.
    inline void TestContainment(FAutomationTestBase& Test, const TCHAR* What,
        AVolume* Volume, const FVector& Centre, const FVector& HalfExtent)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s: the volume's own centre is inside it"), What),
            Volume->EncompassesPoint(Centre));

        // 90% of the way to each face — inside by a margin wider than any BSP rounding.
        Test.TestTrue(*FString::Printf(TEXT("%s: a point near the corner is inside it"), What),
            Volume->EncompassesPoint(Centre + HalfExtent * 0.9));

        // Two half-extents out on Z: outside by the volume's own full height.
        Test.TestFalse(*FString::Printf(TEXT("%s: a point beyond the top face is outside it"), What),
            Volume->EncompassesPoint(Centre + FVector(0.0, 0.0, HalfExtent.Z * 2.0)));
    }

    // The brush model itself, so a containment failure separates "no geometry was built"
    // from "geometry was built and the physics body did not follow".
    inline void TestBrushModel(FAutomationTestBase& Test, const TCHAR* What, ABrush* Volume)
    {
        Test.TestNotNull(*FString::Printf(TEXT("%s: Brush UModel is initialized"), What),
            Volume->Brush.Get());
        if (UBrushComponent* BrushComp = Volume->GetBrushComponent())
        {
            Test.TestNotNull(*FString::Printf(TEXT("%s: BrushComponent->Brush is wired"), What),
                BrushComp->Brush.Get());
        }
        if (Volume->Brush && Volume->Brush->Polys)
        {
            Test.TestTrue(*FString::Printf(TEXT("%s: the brush has polys"), What),
                Volume->Brush->Polys->Element.Num() > 0);
        }
        else
        {
            Test.AddError(FString::Printf(
                TEXT("%s: Brush or Brush->Polys is null — geometry was never built"), What));
        }
    }

    // Finds a spawned volume by its editor label. The handlers set the label from the
    // caller's name, and both tests pass a GUID-suffixed one, so this is exact.
    template <typename T>
    inline T* FindVolumeByLabel(UWorld* World, const FString& Label)
    {
        for (TActorIterator<T> It(World); It; ++It)
        {
            if (It->GetActorLabel() == Label)
            {
                return *It;
            }
        }
        return nullptr;
    }

    template <typename T>
    inline int32 CountVolumesOfClass(UWorld* World)
    {
        int32 Count = 0;
        for (TActorIterator<T> It(World); It; ++It)
        {
            ++Count;
        }
        return Count;
    }

    // Reads an {x, y, z} object out of a response, recording a failure when the field is
    // absent. Absence is the pre-fix state and is the assertion that matters: the old
    // lighting.create_lightmass_volume response carried no bounds field of any kind, so a
    // phantom volume was indistinguishable from a working one from the wire alone.
    inline bool ReadResponseVector(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Response, const TCHAR* Field, FVector& OutValue)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Response.IsValid() || !Response->TryGetObjectField(Field, Obj)
            || Obj == nullptr || !Obj->IsValid())
        {
            Test.AddError(FString::Printf(
                TEXT("response carries no '%s' object field"), Field));
            return false;
        }
        OutValue = FVector(
            (*Obj)->GetNumberField(TEXT("x")),
            (*Obj)->GetNumberField(TEXT("y")),
            (*Obj)->GetNumberField(TEXT("z")));
        return true;
    }
}

// ============================================================================
// actor.spawn — an AVolume subclass gets brush geometry, not a phantom
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnVolumeBuildsBrushGeometryTest,
    "PinWright.actor.spawn.VolumeSpawnBuildsBrushGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnVolumeBuildsBrushGeometryTest::RunTest(const FString& Parameters)
{
    using namespace SpawnedVolumeBrushGeometryTestHelpers;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world — skipping"));
        return true;
    }

    // Destroys the volume this test spawns and restores the level's dirty flag.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString Label = FString::Printf(TEXT("PW_SpawnVolumeBrush_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), ProbeOrigin.X);
    LocationJson->SetNumberField(TEXT("y"), ProbeOrigin.Y);
    LocationJson->SetNumberField(TEXT("z"), ProbeOrigin.Z);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // BlockingVolume rather than the measured PCGVolume: the defect and the fix are in
    // actor.spawn's ABrush handling, not in any one subclass, and an engine-core class
    // keeps the test off the PCG plugin's availability.
    Payload->SetStringField(TEXT("classPath"), TEXT("BlockingVolume"));
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetObjectField(TEXT("location"), LocationJson);

    TestTrue(TEXT("actor.spawn handler found"), InvokeHandler(TEXT("actor.spawn"), Payload));

    ABlockingVolume* Spawned = FindVolumeByLabel<ABlockingVolume>(World, Label);
    if (!TestNotNull(TEXT("actor.spawn produced the volume"), Spawned))
    {
        return false;
    }

    TestBrushModel(*this, TEXT("actor.spawn"), Spawned);

    // The default box actor.spawn builds is the editor's own: UActorFactoryBoxVolume
    // uses a default-constructed UCubeBuilder, 200 uu FULL size on each axis.
    const FVector ExpectedHalfExtent(VolumeBrushGeometry::DefaultBoxSize * 0.5f);
    const FBoxSphereBounds Bounds = Spawned->GetBounds();
    TestTrue(TEXT("actor.spawn: the volume's bounds are not the zero extent of a phantom"),
        Bounds.BoxExtent.GetMin() > 1.0);
    TestTrue(TEXT("actor.spawn: the box is the engine's default 100 uu half-extent"),
        Bounds.BoxExtent.Equals(ExpectedHalfExtent, 1.0));

    TestContainment(*this, TEXT("actor.spawn"), Spawned, ProbeOrigin, ExpectedHalfExtent);

    return true;
}

// ============================================================================
// foliage.create_procedural — the volume the scatter samples has real geometry
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralVolumeBuildsBrushGeometryTest,
    "PinWright.foliage.create_procedural.VolumeBuildsBrushGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralVolumeBuildsBrushGeometryTest::RunTest(const FString& Parameters)
{
    using namespace SpawnedVolumeBrushGeometryTestHelpers;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world — skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString VolumeName = FString::Printf(TEXT("PW_ProcFoliageBrush_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SpawnerPackage =
        FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName);

    // The handler's packages are mark-dirty-only, so an editor-wide save later in the
    // suite would otherwise flush them into the host Content tree. B-tests-leak-host-content.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SpawnerPackage);
        if (UPackage* Remaining = FindPackage(nullptr, *SpawnerPackage))
        {
            Remaining->SetDirtyFlag(false);
        }
    };

    const FVector RequestedSize(2000.0, 2000.0, 500.0);

    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), ProbeOrigin.X);
    LocationJson->SetNumberField(TEXT("y"), ProbeOrigin.Y);
    LocationJson->SetNumberField(TEXT("z"), ProbeOrigin.Z);

    TSharedPtr<FJsonObject> SizeJson = MakeShared<FJsonObject>();
    SizeJson->SetNumberField(TEXT("x"), RequestedSize.X);
    SizeJson->SetNumberField(TEXT("y"), RequestedSize.Y);
    SizeJson->SetNumberField(TEXT("z"), RequestedSize.Z);

    TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
    BoundsJson->SetObjectField(TEXT("location"), LocationJson);
    BoundsJson->SetObjectField(TEXT("size"), SizeJson);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), VolumeName);
    Params->SetObjectField(TEXT("bounds"), BoundsJson);
    // Empty on purpose: the question here is the VOLUME's geometry, and no foliage type
    // means no generated assets to clean up and no scatter to wait for.
    Params->SetArrayField(TEXT("foliageTypes"), TArray<TSharedPtr<FJsonValue>>());

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.create_procedural"),
        TEXT("req-procedural-foliage-volume-brush"), Params, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("foliage.create_procedural succeeded (errorCode '%s')"),
        *ErrorCode), bSuccess);
    if (!bSuccess)
    {
        return false;
    }

    AProceduralFoliageVolume* Volume =
        FindVolumeByLabel<AProceduralFoliageVolume>(World, VolumeName);
    if (!TestNotNull(TEXT("the procedural foliage volume was spawned"), Volume))
    {
        return false;
    }

    TestBrushModel(*this, TEXT("foliage.create_procedural"), Volume);

    // The requested size must reach the BRUSH, at identity actor scale. Pre-fix the
    // brush did not exist and the size was written as scale (2000/200 = 10 on X/Y),
    // so both halves of this pair fail for the right reason.
    TestTrue(TEXT("foliage.create_procedural: the volume is left at identity scale"),
        Volume->GetActorScale3D().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER));

    const FVector ExpectedHalfExtent = RequestedSize * 0.5;
    const FBoxSphereBounds Bounds = Volume->GetBounds();
    TestTrue(TEXT("foliage.create_procedural: bounds are not the zero extent of a phantom"),
        Bounds.BoxExtent.GetMin() > 1.0);
    TestTrue(TEXT("foliage.create_procedural: the brush half-extent is the requested size / 2"),
        Bounds.BoxExtent.Equals(ExpectedHalfExtent, 1.0));

    TestContainment(*this, TEXT("foliage.create_procedural"), Volume,
        ProbeOrigin, ExpectedHalfExtent);

    return true;
}

// ============================================================================
// lighting.create_lightmass_volume - the importance region is a box, not a point
// ============================================================================
//
// The fourth site of the same defect, and the one where the phantom is WORSE than the
// absence. FStaticLightingSystem::GatherScene (Engine:
// Editor/UnrealEd/Private/StaticLightingSystem/StaticLightingSystem.cpp) synthesizes an
// importance region from the scene bounds only
// `if (LightmassExporter->GetImportanceVolumes().Num() == 0)` - a COUNT, with no extent test
// anywhere on the path - and FLightmassExporter::AddImportanceVolume (Lightmass.h) stores
// GetComponentsBoundingBox(true) unchecked. An extent-less volume therefore still counts as
// one, suppresses that fallback AND the "No importance volume found" warning inside it, and
// the level bakes against a point. So this verb has to produce real geometry or produce
// nothing. B-lightmass-volume-no-brush-geometry.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingCreateLightmassVolumeBuildsBrushGeometryTest,
    "PinWright.lighting.create_lightmass_volume.VolumeBuildsBrushGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingCreateLightmassVolumeBuildsBrushGeometryTest::RunTest(const FString& Parameters)
{
    using namespace SpawnedVolumeBrushGeometryTestHelpers;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world - skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString Label = FString::Printf(TEXT("PW_LightmassVolumeBrush_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Deliberately NOT the verb's own (1000, 1000, 1000) default and deliberately unequal
    // per axis: a response field that merely echoes the request is indistinguishable from a
    // measured one when the two coincide by construction, and an axis swap hides behind a
    // cube.
    const FVector RequestedSize(3000.0, 2000.0, 800.0);
    const FVector ExpectedHalfExtent = RequestedSize * 0.5;

    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), ProbeOrigin.X);
    LocationJson->SetNumberField(TEXT("y"), ProbeOrigin.Y);
    LocationJson->SetNumberField(TEXT("z"), ProbeOrigin.Z);

    TSharedPtr<FJsonObject> SizeJson = MakeShared<FJsonObject>();
    SizeJson->SetNumberField(TEXT("x"), RequestedSize.X);
    SizeJson->SetNumberField(TEXT("y"), RequestedSize.Y);
    SizeJson->SetNumberField(TEXT("z"), RequestedSize.Z);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Label);
    Payload->SetObjectField(TEXT("location"), LocationJson);
    Payload->SetObjectField(TEXT("size"), SizeJson);

    FTestResponseCapture Capture;
    TestTrue(TEXT("lighting.create_lightmass_volume handler found"),
        InvokeHandlerWithCapture(TEXT("lighting.create_lightmass_volume"), Payload, Capture));
    TestTrue(TEXT("a response was sent"), Capture.bWasCalled);
    if (!TestTrue(*FString::Printf(
        TEXT("lighting.create_lightmass_volume succeeded (errorCode '%s': %s)"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }

    ALightmassImportanceVolume* Volume =
        FindVolumeByLabel<ALightmassImportanceVolume>(World, Label);
    if (!TestNotNull(TEXT("the importance volume was spawned"), Volume))
    {
        return false;
    }

    TestBrushModel(*this, TEXT("lighting.create_lightmass_volume"), Volume);

    // The requested size must reach the BRUSH at identity actor scale. Pre-fix the brush did
    // not exist and the size was written as SetActorScale3D(Size / 200) - 15 / 10 / 4 here -
    // against a zero extent no scale can multiply, so both halves fail for the right reason.
    TestTrue(TEXT("the volume is left at identity scale"),
        Volume->GetActorScale3D().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER));

    const FBoxSphereBounds Bounds = Volume->GetBounds();
    TestTrue(TEXT("bounds are not the zero extent of a phantom"),
        Bounds.BoxExtent.GetMin() > 1.0);
    TestTrue(TEXT("the brush half-extent is the requested size / 2"),
        Bounds.BoxExtent.Equals(ExpectedHalfExtent, 1.0));
    TestTrue(TEXT("the brush is centred on the requested location"),
        Bounds.Origin.Equals(ProbeOrigin, 1.0));

    TestContainment(*this, TEXT("lighting.create_lightmass_volume"), Volume,
        ProbeOrigin, ExpectedHalfExtent);

    // Response honesty: the extent has to be READABLE from the reply, because the caller
    // that runs lighting.build_lighting next has nothing else to check. Pre-fix the response
    // was success + AddActorVerification's existsAfter:true and no bounds field at all.
    FVector ReportedSize = FVector::ZeroVector;
    if (ReadResponseVector(*this, Capture.Result, TEXT("measuredSize"), ReportedSize))
    {
        TestTrue(TEXT("measuredSize is not the zero size of a phantom"),
            ReportedSize.GetMin() > 1.0);
        TestTrue(TEXT("measuredSize matches the brush actually built"),
            ReportedSize.Equals(Bounds.BoxExtent * 2.0, 1.0));
    }

    FVector ReportedExtent = FVector::ZeroVector;
    if (ReadResponseVector(*this, Capture.Result, TEXT("measuredExtent"), ReportedExtent))
    {
        TestTrue(TEXT("measuredExtent is read back off the actor, not echoed"),
            ReportedExtent.Equals(Bounds.BoxExtent, 1.0));
    }

    // The request is reported too, under its own name, so the two can never be confused.
    FVector ReportedRequest = FVector::ZeroVector;
    if (ReadResponseVector(*this, Capture.Result, TEXT("requestedSize"), ReportedRequest))
    {
        TestTrue(TEXT("requestedSize names the caller's value separately"),
            ReportedRequest.Equals(RequestedSize, UE_KINDA_SMALL_NUMBER));
    }

    return true;
}

// ============================================================================
// lighting.create_lightmass_volume - a volume it cannot give an extent is not spawned
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingCreateLightmassVolumeRefusesDegenerateSizeTest,
    "PinWright.lighting.create_lightmass_volume.DegenerateSizeIsRefusedNotSpawned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingCreateLightmassVolumeRefusesDegenerateSizeTest::RunTest(const FString& Parameters)
{
    using namespace SpawnedVolumeBrushGeometryTestHelpers;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world - skipping"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const int32 VolumesBefore = CountVolumesOfClass<ALightmassImportanceVolume>(World);

    TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
    LocationJson->SetNumberField(TEXT("x"), ProbeOrigin.X);
    LocationJson->SetNumberField(TEXT("y"), ProbeOrigin.Y);
    LocationJson->SetNumberField(TEXT("z"), ProbeOrigin.Z);

    // Flat on Z: the brush would build, and its importance region would still enclose zero
    // volume. Lightmass takes GetExtent() straight into FullGridSize, so a flat box is the
    // same point-shaped failure with a nicer-looking bounds readout.
    TSharedPtr<FJsonObject> SizeJson = MakeShared<FJsonObject>();
    SizeJson->SetNumberField(TEXT("x"), 3000.0);
    SizeJson->SetNumberField(TEXT("y"), 2000.0);
    SizeJson->SetNumberField(TEXT("z"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"),
        FString::Printf(TEXT("PW_LightmassVolumeFlat_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    Payload->SetObjectField(TEXT("location"), LocationJson);
    Payload->SetObjectField(TEXT("size"), SizeJson);

    FTestResponseCapture Capture;
    TestTrue(TEXT("lighting.create_lightmass_volume handler found"),
        InvokeHandlerWithCapture(TEXT("lighting.create_lightmass_volume"), Payload, Capture));
    TestTrue(TEXT("a response was sent"), Capture.bWasCalled);

    // Pre-fix this answered success, wrote SetActorScale3D on a brushless actor and left it
    // in the level - which is the state that suppresses the engine's own fallback.
    TestFalse(TEXT("a size that cannot produce an importance region is refused"),
        Capture.bSuccess);

    // The refusal has to explain the count gate, or the caller reads it as a fussy validator
    // and reaches for a workaround that spawns the phantom by another route.
    TestTrue(TEXT("the refusal explains why an empty volume is worse than none"),
        Capture.Message.Contains(TEXT("COUNT is zero")));

    TestEqual(TEXT("no importance volume was left behind by the refusal"),
        CountVolumesOfClass<ALightmassImportanceVolume>(World), VolumesBefore);

    return true;
}
