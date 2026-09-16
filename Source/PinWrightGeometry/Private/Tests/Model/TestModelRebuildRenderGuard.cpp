// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-model-compile-live-niagara-mesh-renderer-raytracing-assert and its
// duplicate B-static-mesh-rebuild-crashes-live-niagara-mesh-renderer - one defect, filed
// twice from the same editor kill.
//
// model.compile rebuilds the target UStaticMesh in place, which reallocates its
// FStaticMeshRenderData. The engine reregisters UStaticMeshComponents around that build and
// nothing else, so a live UNiagaraComponent whose mesh renderer had cached the old render
// data was left holding the freed block. One frame later
// FNiagaraRenderableStaticMesh::GetRayTraceLODModelData read RayTracingProxy->LODs from it,
// took NumLODs 0 with a first-LOD index of -1, and indexed the empty array:
// "Array index out of bounds: -1 into an array of size 0" on the render thread. That is an
// unrecoverable appError - the compile had already answered success, and the whole editor
// process died on the next redraw, taking every other session's unsaved work with it.
//
// The fix (Utils/MeshRebuildRenderGuard.h, wired into model.compile and the other StaticMesh
// mutators) scans live target-mesh components and Niagara mesh-renderer properties, destroys
// their render state before the rebuild, flushes so the render thread drops the proxies, and
// recreates it afterwards - refusing the compile outright when a candidate cannot be cycled.
//
// A crash ticket cannot be reproduced from an automation test: an appError on the render
// thread would kill the suite host. These tests assert the GUARD instead - that the scan
// finds live proxy-holding components, that the scope really does strip and restore their render
// state around the work, and that the Niagara matcher follows the renderer's mesh slots.
#include "Misc/AutomationTest.h"

#include "Utils/MeshRebuildRenderGuard.h"
#include "Tests/TestWorldUtils.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraTypes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

// The mechanism the fix adds: everything handed to FQuiesceScope loses its render state for
// the duration of the scope and gets it back on exit. That is the entire reason the rebuild
// can no longer strand a scene proxy, and none of it existed before the fix - the compiler
// call ran bare, so a live renderer kept its cached render data straight through the
// reallocation.
//
// UStaticMeshComponent is used as the probe deliberately: it is spawnable on every host, and
// the target-mesh scan proves the helper's component matching independently of Niagara's
// optional class table (which the sibling test pins).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelRebuildRenderGuardQuiescesConsumersTest,
    "PinWright.Model.RebuildRenderGuard.QuiescedConsumersLoseAndRegainRenderState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelRebuildRenderGuardQuiescesConsumersTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no editor world, so no registered "
                        "components exist to quiesce."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString Label = FString::Printf(TEXT("PW_RebuildGuardProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Probe = SpawnTransientCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Probe)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: the engine unit cube did not load, so no "
                        "probe component could be spawned."));
        return true;
    }

    UStaticMeshComponent* ProbeComponent = Probe->GetStaticMeshComponent();
    if (!ProbeComponent || !ProbeComponent->IsRegistered() || !ProbeComponent->IsRenderStateCreated())
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: the probe component carries no render state "
                        "on this host, so there is no proxy for the guard to cycle."));
        return true;
    }

    TArray<UStaticMesh*> TargetMeshes;
    TargetMeshes.Add(ProbeComponent->GetStaticMesh());
    const TArray<UActorComponent*> Candidates =
        PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(TargetMeshes);

    TestTrue(TEXT("the scan finds the live, registered, proxy-holding probe component"),
        Candidates.Contains(ProbeComponent));

    // Only the probe is quiesced, not everything the scan returned: the open map's own meshes
    // are the same class and cycling all of them would make this test cost the level.
    UActorComponent* const ProbeAsComponent = ProbeComponent;
    {
        PinWrightMeshRebuild::FQuiesceScope Quiesce(MakeArrayView(&ProbeAsComponent, 1));

        TestEqual(TEXT("the scope reports nothing left unquiesced, so the rebuild would be allowed "
                       "to proceed"),
            Quiesce.NotQuiesced().Num(), 0);
        TestEqual(TEXT("the scope quiesced exactly the one component it was given"), Quiesce.Num(), 1);
        TestFalse(TEXT("INSIDE the scope the probe holds no render state - this is the window the "
                       "rebuild runs in, and it is why no proxy can be left caching the old render data"),
            ProbeComponent->IsRenderStateCreated());
        TestTrue(TEXT("the component stays registered while quiesced; only its render state is cycled"),
            ProbeComponent->IsRegistered());
    }

    TestTrue(TEXT("leaving the scope recreates the probe's render state, so the guard is not a "
                  "one-way teardown of whatever the level was drawing"),
        ProbeComponent->IsRenderStateCreated());

    return true;
}

// The shared entry point must resolve the target mesh before invoking its work callback. This
// test duplicates the engine cube into a disposable test package, attaches it to a live component,
// then performs the actual Build/PostEditChange pair while the helper owns the guard scope.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshRebuildGuardSharedHelperTest,
    "PinWright.static_mesh.rebuild_guard.SharedHelperResolvesMeshAtSafePoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshRebuildGuardSharedHelperTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("A live UStaticMeshComponent is required for the shared guard plumbing test."));
        return true;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("The shared StaticMesh guard has no loaded mesh path to resolve."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_RebuildGuard_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* MeshPackage = CreatePackage(*PackagePath);
    UStaticMesh* RebuildMesh = MeshPackage
        ? DuplicateObject<UStaticMesh>(Cube, MeshPackage,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)))
        : nullptr;
    if (!TestNotNull(TEXT("disposable StaticMesh rebuild fixture created"), RebuildMesh))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    RebuildMesh->AddToRoot();
    UStaticMeshComponent* ProbeComponent = nullptr;
    ON_SCOPE_EXIT
    {
        if (ProbeComponent && IsValid(ProbeComponent))
        {
            ProbeComponent->SetStaticMesh(nullptr);
        }
        RebuildMesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    const FString Label = FString::Printf(TEXT("PW_SharedRebuildGuardProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Probe = SpawnTransientCubeActor(World, Label, FVector::ZeroVector);
    ProbeComponent = Probe ? Probe->GetStaticMeshComponent() : nullptr;
    if (!ProbeComponent || !ProbeComponent->IsRegistered() || !ProbeComponent->IsRenderStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("live-static-mesh-component-unavailable"),
            TEXT("The editor world did not provide a registered render-state probe."));
        return true;
    }
    ProbeComponent->SetStaticMesh(RebuildMesh);
    if (!ProbeComponent->IsRegistered() || !ProbeComponent->IsRenderStateCreated())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-render-state-unavailable"),
            TEXT("The duplicated StaticMesh could not be attached to a live render-state probe."));
        return true;
    }

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
        TEXT("test-id"), TEXT("static_mesh.bake_transform"), MakeShared<FJsonObject>(), Capture);
    TSharedRef<int32> WorkCalls = MakeShared<int32>(0);
    TSharedRef<int32> MeshCount = MakeShared<int32>(0);
    TSharedRef<int32> BuildCalls = MakeShared<int32>(0);
    TSharedRef<int32> PostEditChangeCalls = MakeShared<int32>(0);
    TSharedRef<bool> QuiescedDuringWork = MakeShared<bool>(false);
    TSharedRef<bool> ReferencesTarget = MakeShared<bool>(false);

    TArray<FString> Paths;
    Paths.Add(FPackageName::ObjectPathToPackageName(RebuildMesh->GetPathName()));
    PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx,
        TEXT("shared StaticMesh guard test"), Paths,
        [WorkCalls, MeshCount, BuildCalls, PostEditChangeCalls, QuiescedDuringWork,
         ReferencesTarget, ProbeComponent](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const TArray<UStaticMesh*>& Meshes)
        {
            ++(*WorkCalls);
            *MeshCount = Meshes.Num();
            *ReferencesTarget = Meshes.Num() == 1 && ProbeComponent->GetStaticMesh() == Meshes[0];
            if (*ReferencesTarget)
            {
                *QuiescedDuringWork = !ProbeComponent->IsRenderStateCreated();
                Meshes[0]->Build();
                ++(*BuildCalls);
                Meshes[0]->PostEditChange();
                ++(*PostEditChangeCalls);
                FStaticMeshCompilingManager::Get().FinishCompilation({Meshes[0]});
            }
            Responder.SendSuccess(MakeShared<FJsonObject>());
        });

    // Automation normally runs at a safe point, but keep the assertion valid if a host invokes
    // the test from a named-thread pump: RunAtSafePoint's one-shot continuation is deterministic.
    if (!PinWrightSafePoint::IsSafeNow())
    {
        FTSTicker::GetCoreTicker().Tick(0.0f);
    }

    TestTrue(TEXT("the shared helper emits a response"), Capture->bWasCalled);
    if (!Capture->bWasCalled)
    {
        return false;
    }

    TestTrue(TEXT("the guarded rebuild is not refused when the live component is quiesced"),
        Capture->bSuccess);
    TestTrue(TEXT("the shared helper restores the live component render state after the guarded rebuild"),
        ProbeComponent->IsRenderStateCreated());
    if (!Capture->bSuccess)
    {
        TestEqual(TEXT("a live consumer refusal uses the established error code"),
            Capture->ErrorCode, FString(ErrorCodes::ERR_MESH_REBUILD_CONSUMER_NOT_QUIESCABLE));
        return false;
    }

    TestEqual(TEXT("the helper resolves the requested StaticMesh before work"), *MeshCount, 1);
    TestEqual(TEXT("the rebuild callback runs exactly once"), *WorkCalls, 1);
    TestEqual(TEXT("the guarded callback performs the actual StaticMesh Build"), *BuildCalls, 1);
    TestEqual(TEXT("the guarded callback performs the actual PostEditChange rebuild"),
        *PostEditChangeCalls, 1);
    TestTrue(TEXT("the shared helper itself quiesces the live component before Build/PostEditChange"),
        *QuiescedDuringWork);
    TestTrue(TEXT("the live UStaticMeshComponent still references the resolved mesh during the guarded window"),
        *ReferencesTarget);
    return true;
}

// Renderer properties are the target-specific Niagara evidence. A class-wide Niagara walk would
// quiesce unrelated systems and still miss the system that explicitly holds this mesh, so keep
// the matcher pinned to the public renderer mesh slots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMeshRendererTargetMatcherTest,
    "PinWright.Model.RebuildRenderGuard.NiagaraMeshRendererMatchesTargetMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMeshRendererTargetMatcherTest::RunTest(const FString& Parameters)
{
    UStaticMesh* TargetMesh = NewObject<UStaticMesh>(GetTransientPackage());
    UStaticMesh* OtherMesh = NewObject<UStaticMesh>(GetTransientPackage());
    UNiagaraMeshRendererProperties* Renderer =
        NewObject<UNiagaraMeshRendererProperties>(GetTransientPackage());
    if (!TestNotNull(TEXT("target StaticMesh fixture created"), TargetMesh) ||
        !TestNotNull(TEXT("non-target StaticMesh fixture created"), OtherMesh) ||
        !TestNotNull(TEXT("Niagara mesh renderer fixture created"), Renderer))
    {
        return false;
    }

    FNiagaraMeshRendererMeshProperties& Slot = Renderer->Meshes.AddDefaulted_GetRef();
    Slot.Mesh = TargetMesh;

    TArray<UStaticMesh*> TargetMeshes;
    TargetMeshes.Add(TargetMesh);
    TArray<UStaticMesh*> OtherMeshes;
    OtherMeshes.Add(OtherMesh);

    TestTrue(TEXT("an explicit Niagara mesh slot referencing the target is matched"),
        PinWrightMeshRebuild::NiagaraMeshRendererMayReferenceTargetMesh(*Renderer, TargetMeshes));
    TestFalse(TEXT("an explicit Niagara mesh slot referencing another mesh is not matched"),
        PinWrightMeshRebuild::NiagaraMeshRendererMayReferenceTargetMesh(*Renderer, OtherMeshes));

    Slot.Mesh = nullptr;
    Slot.MeshParameterBinding.ResolvedParameter =
        FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), FName(TEXT("RuntimeMesh")));
    TestTrue(TEXT("a valid runtime mesh binding remains a conservative may-reference candidate"),
        PinWrightMeshRebuild::NiagaraMeshRendererMayReferenceTargetMesh(*Renderer, OtherMeshes));

    return true;
}

// The shipped class-path table is retained for static_mesh.describe diagnostics. The actual
// rebuild scan matches UStaticMeshComponents by target mesh and Niagara components through their
// assigned system's enabled mesh-renderer properties.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelRebuildRenderGuardNamesNiagaraTest,
    "PinWright.Model.RebuildRenderGuard.NiagaraMeshRendererIsAScannedCandidateClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelRebuildRenderGuardNamesNiagaraTest::RunTest(const FString& Parameters)
{
    bool bNamesNiagaraComponent = false;
    for (const TCHAR* ClassPath : PinWrightMeshRebuild::StaleRenderStateComponentClassPaths)
    {
        if (FString(ClassPath) == TEXT("/Script/Niagara.NiagaraComponent"))
        {
            bNamesNiagaraComponent = true;
        }
    }
    TestTrue(TEXT("the candidate table names UNiagaraComponent - the class whose mesh renderer "
                  "aborted the render thread after a rebuild"),
        bNamesNiagaraComponent);

    const TArray<UClass*> Resolved = PinWrightMeshRebuild::ResolveStaleRenderStateComponentClasses();

    TestFalse(TEXT("UStaticMeshComponent is not in the Niagara diagnostic class table: the shared "
                   "helper matches target UStaticMeshComponents separately"),
        Resolved.Contains(UStaticMeshComponent::StaticClass()));

    if (FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraComponent")) == nullptr)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: Niagara is not enabled on this host, so the "
                        "class path resolves to nothing and no candidate can be confirmed loaded."));
        return true;
    }

    TestTrue(TEXT("on a Niagara-enabled host the table resolves to at least one live UClass, so the "
                  "scan has something to match against"),
        Resolved.Num() >= 1);

    return true;
}
