// Copyright (c) 2026 Alexander Penkin. MIT License.

// Dirty-flag contract for the main-module Environment handlers that mutate live
// level actors.
//
// The bug guarded here: UWorld::SpawnActor dirties the level only under a
// transaction (LevelActor.cpp:735-739) and the engine light/fog setters push
// render state through a fast path that never marks the package dirty
// (SkyLightComponent.cpp:971-980). Handlers open no transaction, so a spawn or a
// property write updated the viewport, left the package clean, and `level.save`
// no-opped — the edit vanished when the editor closed.
//
// Verb choice is deliberate. `lighting.spawn_light` looks like the natural spawn
// guard and is a BAD one: its unconditional SetActorLabel(LightClassStr) dirties
// the actor package by accident, and on a classic (non-World-Partition) map the
// actor package IS the level package, so the assertion passes with the fix
// reverted. Every verb below has no accidental-dirty path.
//
// Run-mode note: the suite runs under -unattended, so SpawnActorInActiveWorld
// takes the SpawnDirectInWorld fork and does NOT get UEditorEngine::AddActor's
// incidental MarkPackageDirty. These tests therefore observe the real defect;
// running the same assertions by hand in an interactive editor shows false
// greens on the spawn cases.
//
// Every test saves and restores the package dirty flag it clears. These run
// against the host project's real open level, and leaving a forced-clean flag
// behind would make a later `level.save` lose work — the exact failure this
// contract exists to prevent.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SphereReflectionCapture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Package.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Package of the world's persistent level — the package `level.save` writes.
    UPackage* GetPersistentLevelPackage(UWorld* World)
    {
        if (!World || !World->PersistentLevel) return nullptr;
        return World->PersistentLevel->GetPackage();
    }

    AExponentialHeightFog* FindHeightFog(UWorld* World)
    {
        if (!World) return nullptr;
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
        {
            return *It;
        }
        return nullptr;
    }
}

// ---- lighting.setup_volumetric_fog — spawn and modify paths both dirty ----
// Exercises MarkLevelActorSpawned (spawn sub-case) and MarkLevelActorModified +
// MarkComponentRenderStateDirty (modify sub-case). The verb sets no actor label
// anywhere and both of its writes are raw component fields, so nothing dirties by
// accident — a revert of the ceremony fails these assertions outright.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupVolumetricFogMarksPackageDirtyTest,
    "PinWright.lighting.setup_volumetric_fog.MarksPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupVolumetricFogMarksPackageDirtyTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping setup_volumetric_fog dirty test"));
        return true;
    }

    UPackage* LevelPkg = GetPersistentLevelPackage(World);
    if (!LevelPkg)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-package-unavailable"),
            TEXT("Persistent level package unavailable — skipping setup_volumetric_fog dirty test"));
        return true;
    }

    // Sub-case 1 — spawn path. Only meaningful when the level has no fog actor yet;
    // destroying an existing one to force the path would mutate the host level.
    if (FindHeightFog(World) == nullptr)
    {
        const bool bLevelWasDirty = LevelPkg->IsDirty();
        LevelPkg->SetDirtyFlag(false);
        ON_SCOPE_EXIT
        {
            LevelPkg->SetDirtyFlag(bLevelWasDirty);
        };

        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        TestTrue(TEXT("lighting.setup_volumetric_fog handler found (spawn)"),
            InvokeHandler(TEXT("lighting.setup_volumetric_fog"), SpawnPayload));

        TestTrue(TEXT("spawning the fog actor dirties the level package"),
            LevelPkg->IsDirty());
    }
    else
    {
        AddWarning(TEXT("Level already has an AExponentialHeightFog — skipping the spawn sub-case"));
    }

    // Sub-case 2 — modify path. Always runs; this is the assertion that fails
    // without MarkLevelActorModified.
    AExponentialHeightFog* FogActor = FindHeightFog(World);
    TestNotNull(TEXT("AExponentialHeightFog present after setup_volumetric_fog"), FogActor);
    if (!FogActor) return false;

    UPackage* FogPkg = FogActor->GetPackage();
    TestNotNull(TEXT("fog actor has a package"), FogPkg);
    if (!FogPkg) return false;

    const bool bFogPkgWasDirty = FogPkg->IsDirty();
    FogPkg->SetDirtyFlag(false);
    ON_SCOPE_EXIT
    {
        FogPkg->SetDirtyFlag(bFogPkgWasDirty);
    };

    TSharedPtr<FJsonObject> ModifyPayload = MakeShared<FJsonObject>();
    ModifyPayload->SetNumberField(TEXT("viewDistance"), 12345.0);

    TestTrue(TEXT("lighting.setup_volumetric_fog handler found (modify)"),
        InvokeHandler(TEXT("lighting.setup_volumetric_fog"), ModifyPayload));

    TestTrue(TEXT("writing volumetric fog properties dirties the actor package"),
        FogPkg->IsDirty());

    if (UExponentialHeightFogComponent* FogComp = FogActor->GetComponent())
    {
        TestEqual(TEXT("VolumetricFogDistance round-trips"),
            FogComp->VolumetricFogDistance, 12345.0f);
        TestTrue(TEXT("bEnableVolumetricFog was set"), FogComp->bEnableVolumetricFog);
    }

    return true;
}

// ---- post_process.set_bloom — the Settings write dirties the volume's package ----
// Isolates MarkLevelActorModified from MarkLevelActorSpawned by invoking once to
// guarantee the unbound PPV exists, then clearing the flag before the real call.
// FindOrSpawnUnboundPPV passes no OptionalLabel, so nothing dirties by accident.
// The override assertion rides along so a future refactor cannot trade the
// bOverride_ flip for the dirty flag or vice versa.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostProcessSetBloomMarksPackageDirtyTest,
    "PinWright.post_process.set_bloom.MarksPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPostProcessSetBloomMarksPackageDirtyTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping post_process.set_bloom dirty test"));
        return true;
    }

    TSharedPtr<FJsonObject> SeedPayload = MakeShared<FJsonObject>();
    SeedPayload->SetNumberField(TEXT("intensity"), 1.0);
    TestTrue(TEXT("post_process.set_bloom handler found (seed)"),
        InvokeHandler(TEXT("post_process.set_bloom"), SeedPayload));

    APostProcessVolume* PPV = FindUnboundPPV(World);
    TestNotNull(TEXT("Unbound APostProcessVolume present after set_bloom"), PPV);
    if (!PPV) return false;

    UPackage* PPVPkg = PPV->GetPackage();
    TestNotNull(TEXT("PPV has a package"), PPVPkg);
    if (!PPVPkg) return false;

    const bool bPkgWasDirty = PPVPkg->IsDirty();
    PPVPkg->SetDirtyFlag(false);

    // Restore the flag we cleared. WorldGuard owns teardown of a PPV this test spawned.
    ON_SCOPE_EXIT
    {
        PPVPkg->SetDirtyFlag(bPkgWasDirty);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 2.5);
    TestTrue(TEXT("post_process.set_bloom handler found"),
        InvokeHandler(TEXT("post_process.set_bloom"), Payload));

    TestTrue(TEXT("set_bloom dirties the PostProcessVolume package"), PPVPkg->IsDirty());
    TestEqual(TEXT("BloomIntensity round-trips"), PPV->Settings.BloomIntensity, 2.5f);
    TestTrue(TEXT("bOverride_BloomIntensity was flipped to true"),
        PPV->Settings.bOverride_BloomIntensity != 0);

    return true;
}

// ---- lighting.list_light_types — read-only verb must leave the level clean ----
// Guards against an over-broad "dirty everything on every call" fix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingListLightTypesLeavesPackageCleanTest,
    "PinWright.lighting.list_light_types.LeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingListLightTypesLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    UPackage* LevelPkg = GetPersistentLevelPackage(World);
    if (!LevelPkg)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping list_light_types clean test"));
        return true;
    }

    const bool bWasDirty = LevelPkg->IsDirty();
    LevelPkg->SetDirtyFlag(false);
    ON_SCOPE_EXIT
    {
        LevelPkg->SetDirtyFlag(bWasDirty);
    };

    TestTrue(TEXT("lighting.list_light_types handler found"),
        InvokeHandler(TEXT("lighting.list_light_types"), MakeShared<FJsonObject>()));

    TestFalse(TEXT("a read-only verb leaves the level package clean"), LevelPkg->IsDirty());

    return true;
}

// ---- post_process.set_anti_aliasing — CVar-only verb must leave the level clean ----
// This verb lives among the PPV setters but writes only r.AntiAliasingMethod /
// r.ScreenPercentage and touches no actor. It must NOT gain the actor ceremony its
// five file-mates carry — i.e. the ceremony was applied exactly 5 times, not 6.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostProcessSetAntiAliasingLeavesPackageCleanTest,
    "PinWright.post_process.set_anti_aliasing.LeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPostProcessSetAntiAliasingLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    UPackage* LevelPkg = GetPersistentLevelPackage(World);
    if (!LevelPkg)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_anti_aliasing clean test"));
        return true;
    }

    IConsoleVariable* AACVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod"));
    IConsoleVariable* ScreenPctCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));

    const int32 OriginalAA = AACVar ? AACVar->GetInt() : 0;
    const float OriginalScreenPct = ScreenPctCVar ? ScreenPctCVar->GetFloat() : 0.0f;

    const bool bWasDirty = LevelPkg->IsDirty();
    LevelPkg->SetDirtyFlag(false);

    ON_SCOPE_EXIT
    {
        // Restore both CVars so this test does not pollute the others, then the
        // dirty flag we cleared.
        if (AACVar)
        {
            AACVar->Set(OriginalAA, ECVF_SetByCode);
        }
        if (ScreenPctCVar)
        {
            ScreenPctCVar->Set(OriginalScreenPct, ECVF_SetByCode);
        }
        LevelPkg->SetDirtyFlag(bWasDirty);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("method"), TEXT("TSR"));
    Payload->SetNumberField(TEXT("screenPercentage"), 71.0);

    TestTrue(TEXT("post_process.set_anti_aliasing handler found"),
        InvokeHandler(TEXT("post_process.set_anti_aliasing"), Payload));

    TestFalse(TEXT("a CVar-only verb leaves the level package clean"), LevelPkg->IsDirty());

    return true;
}

// ---- environment.spawn_reflection_capture — the spawn dirties the level package ----
// The strongest spawn guard available anywhere in this cluster, and the reason is the
// verb's OPTIONAL `name`. SpawnActorInActiveWorld only calls SetActorLabel when a
// label was supplied (AssetUtils.h:426-429), so invoking with no `name` removes the
// accidental actor-package dirty entirely and leaves MarkLevelActorSpawned as the only
// thing that can dirty anything. Compare the volume spawn verbs, which always pass a
// non-empty label and are therefore untestable this way (see
// Tests/World/TestVolumeHandlers.cpp).
//
// `properties` is omitted too, so the MarkRenderStateDirty at the end of the handler
// (which marks no package) never runs either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnReflectionCaptureMarksLevelPackageDirtyTest,
    "PinWright.environment.spawn_reflection_capture.MarksLevelPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnReflectionCaptureMarksLevelPackageDirtyTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    UPackage* LevelPkg = GetPersistentLevelPackage(World);
    if (!LevelPkg)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn_reflection_capture dirty test"));
        return true;
    }

    // Snapshot the existing captures: with no `name` the new actor cannot be looked up
    // by label, and passing a label is exactly what would invalidate this test.
    TSet<ASphereReflectionCapture*> PreExisting;
    for (TActorIterator<ASphereReflectionCapture> It(World); It; ++It)
    {
        PreExisting.Add(*It);
    }

    ASphereReflectionCapture* Spawned = nullptr;

    LevelPkg->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("Sphere"));

    TestTrue(TEXT("environment.spawn_reflection_capture handler found"),
        InvokeHandler(TEXT("environment.spawn_reflection_capture"), Payload));

    for (TActorIterator<ASphereReflectionCapture> It(World); It; ++It)
    {
        if (!PreExisting.Contains(*It))
        {
            Spawned = *It;
            break;
        }
    }
    TestNotNull(TEXT("a new ASphereReflectionCapture was spawned"), Spawned);

    TestTrue(TEXT("Level package dirtied by spawn_reflection_capture"), LevelPkg->IsDirty());

    return true;
}
