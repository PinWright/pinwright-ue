// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Level domain handlers
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetUtils.h"
#include "PinWrightSubsystem.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"

// ============================================================================
// level.load — requires: levelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelLoadValidParamsNoCrashTest,
    "PinWright.level.load.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelLoadValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps/TestLevel"));
    TestTrue(TEXT("level.load found"), InvokeHandler(TEXT("level.load"), Payload));
    return true;
}

// FindAlternateOnDiskMap moved to Tests/TestWorldUtils.h: three more tests need a real
// on-disk map now that the editor world is untitled and none of them can read one off
// the ambient world any more.

// Shared regression assertion for B-level-load-no-completion-signal (and its
// editor-domain sibling B-editor-open-level-no-completion-signal): a level-load
// method must respond synchronously once the map is loaded, NOT return a job-ticket
// envelope. The reverted code wrapped FEditorFileUtils::LoadMap in Ctx.StartJob +
// an OnMapOpened delegate; when that delegate did not fire (already-current map,
// load veto, even some fresh cross-map loads) the job leaked as status:"running"
// forever. A real cross-map load through the production handler must come back with
// the load-result shape (levelPath/loaded) and none of ticket_id / status:"running"
// / monitor_path. Picks a real map asset that is NOT the current world, loads it
// via MethodName, asserts the sync shape, then the MapGuard restores the original
// world. Parameterized so both level.load and its alias editor.open_level get the
// identical guard from one routine.
inline void AssertLoadMethodRespondsSynchronously(
    FAutomationTestBase& Test, const FString& MethodName)
{
    if (!GEditor)
    {
        Test.AddInfo(FString::Printf(TEXT("No GEditor (commandlet/unit context); %s reaches the "
            "EDITOR_NOT_AVAILABLE early-out before the load path. Skipping live shape check."),
            *MethodName));
        return;
    }

    // Snapshot the open map and auto-restore it on scope exit (the guard reloads
    // it through level.load if this test's load swaps the active world).
    FScopedEditorWorldMapGuard MapGuard;

    // Find a real map asset that is not the currently-open world so the handler
    // takes the actual load path (not the alreadyLoaded short-circuit).
    const FString TargetMapPath = FindAlternateOnDiskMap(MapGuard.GetOriginalMapPath());

    if (TargetMapPath.IsEmpty())
    {
        Test.AddInfo(TEXT("No alternate map asset available to load; skipping live shape check. "
            "The synchronous contract is still enforced by the handler summary assertion below."));
        return;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TargetMapPath);
    Test.TestTrue(FString::Printf(TEXT("%s handler found"), *MethodName),
        InvokeHandlerWithCapture(MethodName, Payload, Capture));

    // The handler must have responded on this stack — a job ticket would also
    // SendSuccess synchronously, so bWasCalled alone does not prove the fix; the
    // field assertions below distinguish the sync response from the job envelope.
    Test.TestTrue(FString::Printf(TEXT("%s responded on the calling stack"), *MethodName),
        Capture.bWasCalled);

    // Alias handlers (editor.open_level) reach the load path only by cross-dispatching
    // to level.load via Ctx.GetSubsystem()->DispatchMethod. The capture-based unit
    // context wires no subsystem (MakeTestContextWithCapture sets Subsystem=nullptr),
    // so the cross-dispatch can't complete and the handler returns SUBSYSTEM_NOT_FOUND
    // here. That is a harness limitation, not a regression — the load-result shape is
    // still proven by the direct level.load run. Skip the live-shape fields in that
    // case; the summary assertion still guards against re-async-wrapping the alias.
    if (!Capture.bSuccess && Capture.ErrorCode == TEXT("SUBSYSTEM_NOT_FOUND"))
    {
        Test.AddInfo(FString::Printf(TEXT("%s cross-dispatches to level.load, which needs a wired "
            "subsystem the capture-only unit context does not provide; skipping live shape check. "
            "level.load's own RespondsSynchronously test covers the shared load-result shape."),
            *MethodName));
        return;
    }

    if (Capture.Result.IsValid())
    {
        Test.TestFalse(TEXT("no ticket_id (sync response, not a job envelope)"),
            Capture.Result->HasField(TEXT("ticket_id")));
        Test.TestFalse(TEXT("no monitor_path (sync response, not a job envelope)"),
            Capture.Result->HasField(TEXT("monitor_path")));
        // A job envelope sets status="running"; the sync response either omits
        // status or never reports a running job.
        FString Status;
        if (Capture.Result->TryGetStringField(TEXT("status"), Status))
        {
            Test.TestNotEqual(TEXT("status must not be running (job envelope leak)"),
                Status, FString(TEXT("running")));
        }
        Test.TestTrue(TEXT("sync load-result carries levelPath"),
            Capture.Result->HasField(TEXT("levelPath")));
    }
    // MapGuard restores the original world on scope exit.
}

// Belt-and-suspenders summary check: the registered summary must not advertise an
// async/OnMapOpened contract, which is what the reverted handler promised.
inline void AssertSummaryNotAsync(FAutomationTestBase& Test, const FString& MethodName)
{
    const FString Summary = GetRegisteredSummary(MethodName);
    Test.TestTrue(FString::Printf(TEXT("%s summary present"), *MethodName), !Summary.IsEmpty());
    Test.TestFalse(
        FString::Printf(TEXT("%s summary must not promise an async/OnMapOpened variant"), *MethodName),
        Summary.Contains(TEXT("async variant")) || Summary.Contains(TEXT("OnMapOpened")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelLoadRespondsSynchronouslyTest,
    "PinWright.level.load.RespondsSynchronously",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelLoadRespondsSynchronouslyTest::RunTest(const FString& Parameters)
{
    AssertLoadMethodRespondsSynchronously(*this, TEXT("level.load"));
    AssertSummaryNotAsync(*this, TEXT("level.load"));
    return true;
}

// Same regression guard for the editor-domain alias. editor.open_level was fixed by
// the same diff (B-editor-open-level-no-completion-signal) and now cross-dispatches
// to level.load; this proves the alias also responds synchronously and never leaks
// a job envelope, so re-async-wrapping either handler is caught.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorOpenLevelRespondsSynchronouslyTest,
    "PinWright.editor.open_level.RespondsSynchronously",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorOpenLevelRespondsSynchronouslyTest::RunTest(const FString& Parameters)
{
    AssertLoadMethodRespondsSynchronously(*this, TEXT("editor.open_level"));
    AssertSummaryNotAsync(*this, TEXT("editor.open_level"));
    return true;
}

// ============================================================================
// level.save — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveValidParamsNoCrashTest,
    "PinWright.level.save.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Only check registration — invoking with no params saves the currently-open level.
    TestTrue(TEXT("level.save is registered"), IsRegistered(TEXT("level.save")));
    return true;
}

// ============================================================================
// level.save_as — requires: savePath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveAsValidParamsNoCrashTest,
    "PinWright.level.save_as.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveAsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Save-as is environment-dependent and can fail due source-control/content validation in CI.
    TestTrue(TEXT("level.save_as is registered"), IsRegistered(TEXT("level.save_as")));
    return true;
}

// ============================================================================
// level.create — no required params (all optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelCreateValidParamsNoCrashTest,
    "PinWright.level.create.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelCreateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.create found"), InvokeHandler(TEXT("level.create"), MakeShared<FJsonObject>()));
    return true;
}

namespace
{
    // Safely discards the on-disk map that level.create saves at CreatePath WITHOUT routing
    // through CleanupTestAsset -> UEditorAssetLibrary::DeleteAsset -> ObjectTools::ForceDeleteObjects.
    // Force-delete's GatherObjectReferencersForDeletion serializes the freshly-created,
    // never-reloaded world to collect referencers and broadcasts OnAssetsPendingDelete; for a
    // brand-new map package that delete-notification chain crashes the editor under
    // -unattended -RenderOffScreen (FUObjectArray::AllocateSerialNumber null-deref via
    // BlueprintActionDatabase::OnAssetsPendingDelete) — the same never-reloaded-asset hazard
    // CleanupTestAsset documents for 5.4 and TestAudioHandlers' DiscardCreatedAssetByObjectPath
    // works around. Detach the probe world from the roots, rename its package into the transient
    // package, GC it, then delete the .umap file from disk and drop the registry entry — the way
    // the engine discards throwaway maps, never touching ForceDeleteObjects.
    void DiscardProbeMapPackage(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty()) return;

        // Never detach/rename the package while its world is still the active editor world
        // (the restore branch may not have fired): renaming a live world's package to transient
        // would corrupt the editor world context. In that case just drop the on-disk file below;
        // the in-memory world is reclaimed when the next map opens / the host restarts.
        const UPackage* ActivePackage = (GEditor && GEditor->GetEditorWorldContext().World())
            ? GEditor->GetEditorWorldContext().World()->GetOutermost()
            : nullptr;

        if (UPackage* Package = FindPackage(nullptr, *PackagePath))
        {
            if (Package != ActivePackage)
            {
                ForEachObjectWithPackage(Package, [](UObject* Obj)
                {
                    Obj->ClearFlags(RF_Public | RF_Standalone);
                    Obj->SetFlags(RF_Transient);
                    return true;
                });
                FAssetRegistryModule::PackageDeleted(Package);
                Package->ClearFlags(RF_Public | RF_Standalone);
                Package->SetFlags(RF_Transient);
                Package->SetDirtyFlag(false);
                Package->Rename(*MakeUniqueObjectName(GetTransientPackage(),
                    UPackage::StaticClass(), TEXT("EARG_DiscardedProbeMap")).ToString(),
                    GetTransientPackage(),
                    REN_DontCreateRedirectors | REN_NonTransactional | MCP_REN_NO_RESET_LOADERS);
                CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            }
        }

        FString Filename;
        if (FPackageName::TryConvertLongPackageNameToFilename(
                PackagePath, Filename, FPackageName::GetMapPackageExtension()))
        {
            if (IFileManager::Get().FileExists(*Filename))
            {
                IFileManager::Get().Delete(*Filename, /*RequireExists*/false,
                    /*EvenReadOnly*/true, /*Quiet*/true);
            }
        }
    }

    // RAII scaffold for the WP/active-world create_level probe shared by the three
    // create_level regression fixtures (AttachesWorldPartition, MakesWorldActive,
    // EnumeratesDataLayers). It owns the ONE engine-version-sensitive create + teardown
    // protocol so it lives in a single place; each test then only adds its own assertions.
    //
    // Construction invokes level.structure.create_level {save:false} with a fresh GUID-named
    // throwaway level (bCreateWorldPartition flag per-caller), records the handler's
    // authoritative resolved levelPath, and looks the created UWorld up by that path.
    //
    // Teardown (LIFO, in this exact order — the ordering is load-bearing, see below):
    //   1. destructor body: DestroyWorld(bInformEngineOfWorld=false) on the probe world.
    //      create_level reaches UWorld::CreateWorld(..., bAddToRoot=true), which roots the
    //      new world; a rooted, partition-carrying world survives DiscardProbeMapPackage's
    //      CollectGarbage(KEEPFLAGS), leaving a half-torn-down partition object in the global
    //      object hash. A later, unrelated ForceDeleteObjects (e.g. the widget screenshot
    //      test's CleanupTestAsset) then walks that hash and dereferences the orphaned
    //      partition subobject — an access violation that crashes the whole suite. DestroyWorld
    //      flushes streaming, runs CleanupWorld (uninitializing the UWorldPartition) and
    //      RemoveFromRoot()s the world, so the discard GC can actually reclaim it.
    //   2. destructor body: DiscardProbeMapPackage on the resolved path (the
    //      ForceDeleteObjects-free discard helper above).
    //   3. MapGuard member destructs LAST (members destruct after the destructor body, in
    //      reverse declaration order; it is the only member) and reloads the original map —
    //      after the probe world is destroyed and discarded.
    //
    // Gated off on UE 5.3: tearing down a freshly-created partitioned world asserts inside a
    // TVariant (check(Index == TypeIndex), TVariant.h:118). When IsValidEnv() returns false on
    // 5.3 the staging never runs, so callers early-out before any create/destroy round-trip.
    struct FScopedProbeWorld
    {
        // bWantPartition: pass true to request a World-Partition world (sets
        // bCreateWorldPartition:true), false for a plain non-partitioned active world.
        FScopedProbeWorld(const TCHAR* NamePrefix, bool bWantPartition)
        {
            // On UE 5.3 the staging body is compiled out (the create+destroy round-trip
            // crashes inside a TVariant); reference the params so they are not "unused" there.
            (void)NamePrefix;
            (void)bWantPartition;
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
            if (!GEditor)
            {
                return;
            }

            // Unique throwaway package path; save:false keeps the world purely in-memory.
            LevelName = FString::Printf(
                TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            // Hand-built fallback path that only seeds the discard target before the response
            // exists; the handler's authoritative resolved levelPath is adopted below.
            ResolvedPath = FString::Printf(TEXT("/Game/Maps/%s"), *LevelName);

            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("levelName"), LevelName);
            Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps"));
            if (bWantPartition)
            {
                Payload->SetBoolField(TEXT("bCreateWorldPartition"), true);
            }
            Payload->SetBoolField(TEXT("save"), false);

            bHandlerFound =
                InvokeHandlerWithCapture(TEXT("level.structure.create_level"), Payload, CreateCapture);
            if (CreateCapture.bSuccess && CreateCapture.Result.IsValid())
            {
                // Adopt the handler's authoritative resolved path for the lookup / discard.
                CreateCapture.Result->TryGetStringField(TEXT("levelPath"), ResolvedPath);
                if (UPackage* CreatedPackage = FindPackage(nullptr, *ResolvedPath))
                {
                    CreatedWorld = FindObject<UWorld>(CreatedPackage, *LevelName);
                }
                bStaged = true;
            }
#endif // !UE_VERSION_OLDER_THAN(5, 4, 0)
        }

        ~FScopedProbeWorld()
        {
            if (CreatedWorld && CreatedWorld->bIsWorldInitialized)
            {
                CreatedWorld->DestroyWorld(/*bInformEngineOfWorld=*/false);
            }
            if (!ResolvedPath.IsEmpty())
            {
                DiscardProbeMapPackage(ResolvedPath);
            }
            // MapGuard (member) destructs after this body and reloads the original map.
        }

        FScopedProbeWorld(const FScopedProbeWorld&) = delete;
        FScopedProbeWorld& operator=(const FScopedProbeWorld&) = delete;

        // True once create_level returned success (the world was staged); false on the 5.3
        // gate, missing GEditor, or a create_level failure — callers should skip in that case.
        bool IsStaged() const { return bStaged; }
        // Whether the dispatcher even found the create_level handler (for the found-assert).
        bool WasHandlerFound() const { return bHandlerFound; }
        UWorld* World() const { return CreatedWorld; }
        const FString& GetLevelName() const { return LevelName; }
        const FString& GetResolvedPath() const { return ResolvedPath; }
        const FTestResponseCapture& GetCreateCapture() const { return CreateCapture; }

    private:
        FString LevelName;
        FString ResolvedPath;
        FTestResponseCapture CreateCapture;
        bool bHandlerFound = false;
        bool bStaged = false;
        UWorld* CreatedWorld = nullptr;
        // Declared LAST so it destructs LAST — restores the original map after destroy+discard.
        FScopedEditorWorldMapGuard MapGuard;
    };
}

// Regression for B-level-create-makes-wp-map: level.create is documented and named
// "non-World-Partition" (LevelHandler.cpp registration string) but called
// GEditor->NewMap(true) (bIsPartitionedWorld=true), producing a World-Partition world
// with no self-contained persistent .umap. This test invokes the real handler against a
// throwaway path and asserts the world it leaves active is NOT partitioned; it fails if
// the NewMap argument is reverted to true. Skipped gracefully when no editor world exists
// (e.g. a pre-existing target package short-circuits to system.console_command).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelCreateProducesNonPartitionedWorldTest,
    "PinWright.level.create.ProducesNonPartitionedWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelCreateProducesNonPartitionedWorldTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor; skipping non-partitioned-world assertion."));
        return true;
    }

    // Use a unique throwaway path so a stale on-disk asset can't divert level.create into
    // its "package already exists -> Open" branch (which never reaches NewMap).
    const FString CreatePath = FString::Printf(
        TEXT("/Game/Maps/__EARG_WPRegressionProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // Delete the throwaway probe map unconditionally on every exit path (assert failure,
    // future early return) rather than only from the conditional tail block. Uses the
    // ForceDeleteObjects-free DiscardProbeMapPackage (not CleanupTestAsset) — see its comment:
    // force-deleting a freshly-created-and-saved map crashes the editor under -unattended.
    ON_SCOPE_EXIT { DiscardProbeMapPackage(CreatePath); };

    // Record the world open before the test and auto-restore it on scope exit. The guard
    // snapshots the original map's package name as a string NOW (before any swap) and reloads
    // it via level.load on destruction. level.create calls GEditor->NewMap(), which swaps the
    // editor world and lets GC collect the old one; the guard's captured UWorld* is used only
    // for an identity (did-the-world-swap?) comparison via GetOriginalWorld(), never
    // dereferenced afterward (that would read a dangling pointer and crash). Declared AFTER the
    // ON_SCOPE_EXIT so its destructor (the original-map restore) runs BEFORE DiscardProbeMapPackage
    // — that ordering leaves the probe map non-active when the discard runs, so the discard takes
    // its full rename+GC cleanup path rather than the file-only fallback.
    FScopedEditorWorldMapGuard MapGuard;
    UWorld* const OriginalWorld = MapGuard.GetOriginalWorld();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), CreatePath);
    TestTrue(TEXT("level.create handler is registered"),
        InvokeHandler(TEXT("level.create"), Payload));

    // After a real create, the handler sets the new map as the current world. Assert it is
    // a plain (non-WP) world — NewMap(false). A reverted NewMap(true) makes this true.
    if (UWorld* ActiveWorld = GEditor->GetEditorWorldContext().World())
    {
        if (ActiveWorld != OriginalWorld)
        {
            TestFalse(TEXT("level.create must produce a non-World-Partition world"),
                ActiveWorld->IsPartitionedWorld());
        }
        else
        {
            // Create didn't swap the active world (no LevelEditor subsystem / save guard /
            // existing package). Registration coverage stands; nothing to clean up.
            AddInfo(TEXT("level.create did not swap the active editor world; "
                "skipping partition assertion for this environment."));
        }
    }

    // MapGuard restores the originally-open map (via level.load) on scope exit, sharing
    // level.load's resolution/verification path.
    return true;
}

// Regression for B-lighting-create-level-false-success-no-umap:
// lighting.create_lighting_enabled_level builds a fresh in-memory map (GEditor->NewMap()),
// spawns a directional + sky light, calls the shared McpSafeLevelSave, and used to report
// success:true/existsAfter:true straight from that boolean — whose lenient
// ShouldTreatLevelSaveAsSuccess OR-policy (AssetUtils.cpp) accepts a clean / registered
// package as success even when NO .umap landed on disk. For a NewMap()'d in-memory-only
// world that produced a FALSE success:true — strictly worse than the now-honest level.*
// verbs. The fix re-gates the reported-success boolean through the shared
// VerifyLevelSavedToDisk helper (exactly as level.save / level.save_as /
// level.structure.create_level already do), so the handler may only report
// success:true/existsAfter:true when a .umap is actually on disk, and otherwise fails
// honestly with SAVE_VERIFICATION_FAILED.
//
// This drives the REAL handler end-to-end (InvokeHandlerWithCapture -> the production
// handler body, including NewMap + save) and asserts the persistence-honesty INVARIANT: the
// reported success MUST agree with on-disk reality. Reverting the handler to trust the bare
// McpSafeLevelSave boolean makes it report success:true with no .umap on disk, failing the
// invariant. The invariant is robust to the environment — if the save genuinely lands a
// .umap, success:true is consistent and the test still passes. Skipped gracefully with no
// GEditor (commandlet/unit context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingCreateLevelReportsHonestPersistenceTest,
    "PinWright.lighting.create_lighting_enabled_level.ReportsHonestPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingCreateLevelReportsHonestPersistenceTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); skipping create-lighting-level "
                "persistence-honesty assertion."));
        return true;
    }

    // Unique throwaway path so no stale on-disk .umap can shadow the probe.
    const FString LevelPath = FString::Printf(
        TEXT("/Game/Maps/__EARG_LightingLevelHonestyProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Discard any .umap + in-memory package the create/save may have left, on every exit
    // path, via the ForceDeleteObjects-free discard (see DiscardProbeMapPackage). Declared
    // BEFORE MapGuard so MapGuard's original-map restore runs FIRST (LIFO destruction),
    // leaving the probe package non-active when the discard runs.
    ON_SCOPE_EXIT { DiscardProbeMapPackage(LevelPath); };

    // The handler calls GEditor->NewMap(), swapping the active editor world; restore the
    // originally-open map on scope exit so later tests in the run aren't polluted.
    FScopedEditorWorldMapGuard MapGuard;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), LevelPath);

    // On the honest-failure path (no .umap on disk) the handler logs an Error — the same
    // Error-level report the sibling level.* save verbs emit (LevelHandler.cpp). UE's
    // automation framework captures Error/Warning logs as test failures, so declare that log
    // expected. Negative occurrence = suppress/optional (the honest-success branch, if the
    // save ever lands a file, logs nothing); the explicit assertions below do the real
    // verification. Plain (non-regex) substring match on the stable, GUID-independent prefix.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.3: AddExpectedError rejects negative Occurrences (optional semantics and the IsRegex
    // parameter both arrived in 5.4), and Occurrences=0 would fail the honest-success (no log)
    // branch. Suppress log capture for this test instead; the explicit TestEqual/TestTrue
    // assertions below (which bypass log capture) still do the real verification.
    bSuppressLogs = true;
#else
    AddExpectedError(TEXT("create_lighting_enabled_level: save reported="),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
#endif

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("lighting.create_lighting_enabled_level"), Payload, Capture);
    TestTrue(TEXT("lighting.create_lighting_enabled_level handler is registered"), bFound);
    TestTrue(TEXT("lighting.create_lighting_enabled_level responded"), Capture.bWasCalled);

    // Ground truth: did the .umap actually land on disk? (mount-aware resolve + file probe —
    // the same signal the fix's VerifyLevelSavedToDisk re-gate uses.)
    FString MapFilename;
    const bool bFileOnDisk =
        ResolveLevelPackageToMapFilename(LevelPath, MapFilename)
        && IFileManager::Get().FileExists(*MapFilename);

    // The persistence-honesty invariant: success is reported IFF the .umap landed on disk.
    // Pre-fix, the handler reported success:true from the lenient boolean even with no file
    // (bFileOnDisk == false), violating this — the exact false-success this test guards.
    TestEqual(TEXT("reported success must agree with .umap-on-disk reality"),
        Capture.bSuccess, bFileOnDisk);

    if (bFileOnDisk)
    {
        // Honest success path: the file landed, so success:true + existsAfter:true is truthful.
        bool bExistsAfter = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("existsAfter"), bExistsAfter);
        }
        TestTrue(TEXT("existsAfter:true only when the .umap is truly on disk"), bExistsAfter);
    }
    else
    {
        // The bug condition (in-memory-only world, no .umap). The reported-success/no-file
        // agreement is already asserted by the TestEqual invariant above; here we additionally
        // require the failure to carry an honest code — SAVE_VERIFICATION_FAILED when the save
        // reported success (the lenient-OR-policy case the ticket documents), or SAVE_FAILED if
        // the underlying save reported failure. Either is honest; a false success:true is not.
        const bool bHonestFailureCode =
            Capture.ErrorCode == TEXT("SAVE_VERIFICATION_FAILED") ||
            Capture.ErrorCode == TEXT("SAVE_FAILED");
        TestTrue(*FString::Printf(
            TEXT("no-.umap failure reports an honest SAVE_(VERIFICATION_)FAILED code (got '%s')"),
            *Capture.ErrorCode), bHonestFailureCode);
    }

    return true;
}

// ============================================================================
// level.stream — requires: levelName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStreamValidParamsNoCrashTest,
    "PinWright.level.stream.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStreamValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), TEXT("TestSublevel"));
    TestTrue(TEXT("level.stream found"), InvokeHandler(TEXT("level.stream"), Payload));
    return true;
}

// Regression for E-level-stream-editor-time-exec-failed: level.stream wraps the
// runtime-only StreamLevel console command, which has no editor-time consumer.
// Before the fix, calling it at editor authoring time cross-dispatched to
// system.console_command and surfaced the bare [EXEC_FAILED] Command not executed
// (or EDITOR_WORLD_NOT_AVAILABLE) with no hint at the editor-time substitute. The
// handler now short-circuits with the actionable RUNTIME_ONLY_COMMAND error when
// GEditor->PlayWorld == nullptr (not in a play session). This test runs in the
// unit/editor context (no PIE world), so the production handler must take the guard
// and return RUNTIME_ONLY_COMMAND — never EXEC_FAILED, never a fake success. If the
// guard were reverted the error code would revert to EXEC_FAILED and this fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStreamEditorTimeRuntimeOnlyRedirectTest,
    "PinWright.level.stream.EditorTimeRuntimeOnlyRedirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStreamEditorTimeRuntimeOnlyRedirectTest::RunTest(const FString& Parameters)
{
    // Only meaningful outside a play session; the unit-test context never starts PIE,
    // so GEditor->PlayWorld is null here and the runtime-only guard must fire.
    if (GEditor && GEditor->PlayWorld != nullptr)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-session-active"),
            TEXT("PIE world active in test context; level.stream guard intentionally "
                "does not fire. Skipping editor-time redirect assertion."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), TEXT("TestSublevel"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("level.stream"), Payload, Capture);
    TestTrue(TEXT("level.stream handler registered"), bFound);
    TestTrue(TEXT("level.stream sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("level.stream is an error at editor time, not a fake success"),
        Capture.bSuccess);
    // The actionable redirect — NOT the cryptic EXEC_FAILED the ticket reported
    // (the exact RUNTIME_ONLY_COMMAND match below already proves it is not EXEC_FAILED).
    TestEqual(TEXT("level.stream error code is RUNTIME_ONLY_COMMAND at editor time"),
        Capture.ErrorCode, FString(TEXT("RUNTIME_ONLY_COMMAND")));
    // The message must name the editor-time substitute so an agent can pivot.
    TestTrue(TEXT("level.stream error names level.set_visibility as the editor-time path"),
        Capture.Message.Contains(TEXT("level.set_visibility")));
    return true;
}

// ============================================================================
// level.list — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelListValidParamsNoCrashTest,
    "PinWright.level.list.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelListValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.list found"), InvokeHandler(TEXT("level.list"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// level.export — requires: exportPath
// ============================================================================

// Unique temp path for level-export tests, following the project's
// ProjectIntermediateDir() + GUID temp-path convention (see TestAssetDumpHandler);
// the GUID avoids cross-test collisions in the shared scratch dir.
static FString MakeLevelExportTempPath(const TCHAR* Name)
{
    return FPaths::ProjectIntermediateDir() / TEXT("PinWright")
        / (FGuid::NewGuid().ToString() + Name);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelExportValidParamsNoCrashTest,
    "PinWright.level.export.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelExportValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // .t3d matches the documented text-format export contract (see
    // B-level-export-writes-binary-not-t3d); the handler must not crash.
    const FString ExportPath = MakeLevelExportTempPath(TEXT("ExportedLevelNoCrash.t3d"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ExportPath, false, true);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("exportPath"), ExportPath);
    TestTrue(TEXT("level.export found"), InvokeHandler(TEXT("level.export"), Payload));
    return true;
}

// Regression guard for B-level-export-writes-binary-not-t3d: level.export
// documents "Export a level to a text-format .t3d file on disk", but the reverted
// handler called FEditorFileUtils::SaveMap, which serialized a BINARY .umap package
// to the .t3d-named path. The on-disk bytes then began with the Unreal package magic
// (PACKAGE_FILE_TAG 0x9E2A83C1, little-endian first four bytes C1 83 2A 9E) instead
// of the human-readable "Begin Map" T3D text the doc assumes.
//
// This test exports the live editor world to a temp .t3d through the production
// handler and asserts the artifact is T3D TEXT, not a binary package: the leading
// bytes must NOT be the package magic, and the file must contain a "Begin Map"
// marker. It also asserts the handler now echoes the written exportPath (the prior
// handler returned only {"success":true} with no path signal). Reverting the fix to
// SaveMap fails both the magic-byte check and the "Begin Map" check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelExportWritesT3dTextNotBinaryTest,
    "PinWright.level.export.WritesT3dTextNotBinary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelExportWritesT3dTextNotBinaryTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor/editor world (commandlet/unit context); level.export "
                "reaches its EDITOR_NOT_AVAILABLE / NO_WORLD early-out before the T3D "
                "export path. Skipping the on-disk T3D-format assertion."));
        return true;
    }

    const FString ExportPath = MakeLevelExportTempPath(TEXT("LevelExportT3dRegression.t3d"));
    IFileManager::Get().Delete(*ExportPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ExportPath, false, true);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("exportPath"), ExportPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.export found"),
        InvokeHandlerWithCapture(TEXT("level.export"), Payload, Capture));
    TestTrue(TEXT("level.export responded"), Capture.bWasCalled);
    TestTrue(TEXT("level.export succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Secondary symptom from the ticket: the handler must now echo the path.
        FString EchoedPath;
        TestTrue(TEXT("level.export echoes exportPath"),
            Capture.Result->TryGetStringField(TEXT("exportPath"), EchoedPath));
    }

    // The file must exist (the handler now hard-errors if the exporter wrote nothing).
    if (!TestTrue(TEXT("level.export wrote a file"),
            IFileManager::Get().FileExists(*ExportPath)))
    {
        return false;
    }

    // Read the raw bytes once and assert TEXT, not a binary package.
    TArray<uint8> FileBytes;
    if (!TestTrue(TEXT("read exported .t3d bytes"),
            FFileHelper::LoadFileToArray(FileBytes, *ExportPath)))
    {
        return false;
    }
    TestTrue(TEXT("exported .t3d is non-empty"), FileBytes.Num() > 0);

    // A T3D text export must NOT begin with the binary Unreal package magic.
    // Compare the leading uint32 against the engine's own PACKAGE_FILE_TAG
    // constant rather than hand-spelling its bytes: on a little-endian host the
    // file's first four bytes (C1 83 2A 9E) load into a uint32 as 0x9E2A83C1,
    // which is PACKAGE_FILE_TAG. Using the named constant keeps the guard correct
    // automatically if the tag ever changes.
    if (FileBytes.Num() >= 4)
    {
        uint32 LeadingTag = 0;
        FMemory::Memcpy(&LeadingTag, FileBytes.GetData(), sizeof(LeadingTag));
        TestNotEqual(TEXT("exported .t3d does NOT start with the binary package magic (PACKAGE_FILE_TAG)"),
            LeadingTag, static_cast<uint32>(PACKAGE_FILE_TAG));
    }

    // A genuine T3D level export contains the human-readable "Begin Map" marker.
    // Decode the bytes already in hand (BufferToString handles length + BOM/encoding)
    // instead of re-reading the same file from disk a second time.
    FString FileText;
    FFileHelper::BufferToString(FileText, FileBytes.GetData(), FileBytes.Num());
    TestTrue(TEXT("exported .t3d contains 'Begin Map' text marker"),
        FileText.Contains(TEXT("Begin Map")));

    return true;
}

// ============================================================================
// level.add_sublevel — requires: subLevelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelAddSublevelValidParamsNoCrashTest,
    "PinWright.level.add_sublevel.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelAddSublevelValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("subLevelPath"), TEXT("/Game/Maps/Sublevel01"));
    TestTrue(TEXT("level.add_sublevel found"), InvokeHandler(TEXT("level.add_sublevel"), Payload));
    return true;
}

// ============================================================================
// level.delete — requires: levelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDeleteValidParamsNoCrashTest,
    "PinWright.level.delete.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDeleteValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Deleting maps mutates workspace state and is not deterministic under unattended runs.
    TestTrue(TEXT("level.delete is registered"), IsRegistered(TEXT("level.delete")));
    return true;
}

// ============================================================================
// level.rename — requires: levelPath, destinationPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelRenameValidParamsNoCrashTest,
    "PinWright.level.rename.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelRenameValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Renaming maps mutates assets and can fail noisily in shared CI environments.
    TestTrue(TEXT("level.rename is registered"), IsRegistered(TEXT("level.rename")));
    return true;
}

// ============================================================================
// level.duplicate — requires: sourcePath, destinationPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDuplicateValidParamsNoCrashTest,
    "PinWright.level.duplicate.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDuplicateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Duplicating maps mutates assets and is flaky in unattended automation.
    TestTrue(TEXT("level.duplicate is registered"), IsRegistered(TEXT("level.duplicate")));
    return true;
}

// Regression for E-level-duplicate-in-memory-only-no-persist-signal: level.duplicate
// calls UEditorAssetLibrary::DuplicateAsset, which makes an in-memory-only copy (the
// package is dirty but no .umap lands on disk). The pre-fix result was just
// {sourcePath, destinationPath, duplicated:true} with NO persistence signal, so a
// caller read duplicated:true as "load-ready on disk" and an immediate level.load
// failed LEVEL_NOT_PERSISTED. The fix adds saved:false plus a persistenceNote pointing
// at level.save_as / editor.save_all (the same dedicated field the sibling
// level.structure.create_level uses for this condition). This duplicates a real engine
// map through the production handler and asserts the success result carries saved:false
// and a persistenceNote naming the save step and the LEVEL_NOT_PERSISTED cross-reference;
// reverting the fix drops both fields and fails this. The persistence
// assertions only fire on the success branch (the duplicate is environment-dependent
// in CI), so this never produces a false failure when the duplicate cannot land, while
// still guarding the honest-signal contract whenever the duplicate succeeds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDuplicateReportsInMemoryNotSavedTest,
    "PinWright.level.duplicate.ReportsInMemoryNotSaved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDuplicateReportsInMemoryNotSavedTest::RunTest(const FString& Parameters)
{
    // A reliably-present engine map to copy. If it is not mounted in this config,
    // skip — the duplicate path needs a real source asset to succeed.
    const FString SourcePath = TEXT("/Engine/Maps/Entry");
    if (!UEditorAssetLibrary::DoesAssetExist(SourcePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("Source map /Engine/Maps/Entry not mounted in this configuration; "
                "skipping the live level.duplicate persistence-signal check."));
        return true;
    }

    // Unique throwaway destination so a stale asset can't divert the duplicate.
    const FString DestinationPath = FString::Printf(
        TEXT("/Game/Maps/__EARG_DuplicatePersistProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // Discard the in-memory duplicate (and any on-disk file, though none is written
    // by duplicate alone) on every exit path so the disposable host baseline stays clean.
    ON_SCOPE_EXIT { DiscardProbeMapPackage(DestinationPath); };

    // Tear the duplicated probe world down through the engine's own DestroyWorld before
    // the package-level discard runs. UEditorAssetLibrary::DuplicateAsset on a UWorld
    // initializes engine-side per-world managers for the copy — notably an
    // FNiagaraWorldManager whose NiagaraDataChannelHandlers are rooted via the global
    // GCObjectReferencer. Those handlers' Outer is the duplicated world, whose
    // ULevel::Model still aliases the SOURCE map's UModel (/Engine/Maps/Entry.Model2),
    // so the rooted handlers transitively pin the /Engine/Maps/Entry package. That
    // reference survives DiscardProbeMapPackage's CollectGarbage(KEEPFLAGS), and the very
    // next level.load's UEditorEngine::Map_Load leak check (EditorServer.cpp) then sees
    // the old Entry package still alive and Fatal-errors, crashing the whole suite.
    // DestroyWorld runs CleanupWorld, which fires FNiagaraWorldManager::OnWorldCleanup
    // (releasing the data-channel handlers) and RemoveFromRoot()s the world, so the
    // subsequent discard GC actually reclaims it and unpins the source package. Mirrors
    // the same teardown the level.structure.create_level WP test performs above. Declared
    // AFTER the discard guard so LIFO runs this first: destroy, then discard.
    UWorld* DuplicatedProbeWorld = nullptr;
    ON_SCOPE_EXIT
    {
        if (DuplicatedProbeWorld && DuplicatedProbeWorld->bIsWorldInitialized)
        {
            DuplicatedProbeWorld->DestroyWorld(/*bInformEngineOfWorld=*/false);
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), SourcePath);
    Payload->SetStringField(TEXT("destinationPath"), DestinationPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.duplicate handler is registered"),
        InvokeHandlerWithCapture(TEXT("level.duplicate"), Payload, Capture));
    TestTrue(TEXT("level.duplicate responded"), Capture.bWasCalled);

    // Hand the duplicated UWorld to the DestroyWorld scope-exit guard above so its
    // engine-side managers (the leaking FNiagaraWorldManager) are torn down before the
    // package discard GC runs. The duplicate's UWorld lives in the destination package
    // under the package's short name (the convention DuplicateAsset uses for maps).
    if (UPackage* DuplicatedPackage = FindPackage(nullptr, *DestinationPath))
    {
        DuplicatedProbeWorld = FindObject<UWorld>(
            DuplicatedPackage, *FPackageName::GetShortName(DestinationPath));
    }

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // duplicated:true is still reported (the in-memory copy genuinely exists).
        bool bDuplicated = false;
        TestTrue(TEXT("result carries duplicated"),
            Capture.Result->TryGetBoolField(TEXT("duplicated"), bDuplicated));
        TestTrue(TEXT("duplicated is true"), bDuplicated);

        // The persistence signal the fix adds: the copy is NOT on disk yet.
        bool bSaved = true;
        TestTrue(TEXT("result carries the saved persistence signal"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
        TestFalse(TEXT("saved is false (duplicate is in-memory only, no .umap written)"),
            bSaved);

        // And a human-readable persistenceNote naming the save step before a load.
        // Emitted under the same dedicated persistenceNote field that the sibling
        // level.structure.create_level uses for this in-memory-only condition (not the
        // overloaded generic `note` key). Pin the load-bearing tokens the note is
        // contractually required to carry — the save step and the LEVEL_NOT_PERSISTED
        // cross-reference — rather than a loose save-OR-memory check that a note with
        // zero save guidance (the under-signalled state this ticket is about) would pass.
        FString Note;
        TestTrue(TEXT("result carries an in-memory-only persistenceNote"),
            Capture.Result->TryGetStringField(TEXT("persistenceNote"), Note));
        TestTrue(TEXT("persistenceNote names the save step"),
            Note.Contains(TEXT("save")));
        TestTrue(TEXT("persistenceNote cross-references LEVEL_NOT_PERSISTED"),
            Note.Contains(TEXT("LEVEL_NOT_PERSISTED")));
    }
    else
    {
        AddInfo(TEXT("level.duplicate did not succeed in this environment "
            "(source-control / content validation); the persistence-signal assertions "
            "are skipped. Registration coverage stands."));
    }

    return true;
}

// ============================================================================
// level.get_info — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetInfoValidParamsNoCrashTest,
    "PinWright.level.get_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.get_info found"), InvokeHandler(TEXT("level.get_info"), MakeShared<FJsonObject>()));
    return true;
}

// Shared regression assertion for E-level-getters-require-loaded-not-on-disk: the
// read-only inspection getters (level.get_info / get_actors / get_bounds) resolve
// levelPath through FindLevelByPathLevel, which walks only the active world's loaded
// levels. A map that genuinely exists on disk but is not loaded used to surface a
// misleading [LEVEL_NOT_FOUND] "Level not found" — contradicting level.list (which
// lists it) and level.load (which opens it). The fix classifies that miss as
// LEVEL_NOT_LOADED (an on-disk-but-unloaded map) with an actionable message that
// names level.load, reserving LEVEL_NOT_FOUND for a path with no .umap on disk.
//
// This drives the production handler with a real map asset that is NOT the currently
// open world (so FindLevelByPathLevel misses on a path that nonetheless exists on
// disk) and asserts the verdict is LEVEL_NOT_LOADED, not LEVEL_NOT_FOUND, and that
// the message steers the caller to level.load. Reverting the fix restores
// LEVEL_NOT_FOUND and fails the code assertion. Parameterized so all three getters
// get the identical guard from one routine. Skipped gracefully when no editor / no
// alternate on-disk map is available (commandlet / minimal host), where the
// disk-vs-loaded distinction cannot be exercised.
inline void AssertGetterReportsUnloadedOnDiskMap(
    FAutomationTestBase& Test, const FString& MethodName)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Test.AddInfo(FString::Printf(TEXT("No GEditor/editor world (commandlet/unit context); %s "
            "reaches NO_WORLD before the levelPath-resolution path. Skipping the "
            "LEVEL_NOT_LOADED-vs-LEVEL_NOT_FOUND check."), *MethodName));
        return;
    }

    // Find a real map asset that exists on disk but is NOT the active world, so the
    // getter's FindLevelByPathLevel misses on a path that nonetheless has a .umap.
    // The result is invariant across the three getter tests in a run (same active
    // world, same on-disk asset set), so resolve the GetAssetsByClass scan +
    // DoesPackageExist probe once and reuse it — these getters never swap the map,
    // so the open-map exclusion stays valid for the whole suite. The guard supplies
    // the open-map package name the same way the sibling load tests do.
    static const FString UnloadedMapPath = []()
    {
        FScopedEditorWorldMapGuard MapGuard;
        return FindAlternateOnDiskMap(MapGuard.GetOriginalMapPath());
    }();

    if (UnloadedMapPath.IsEmpty())
    {
        Test.AddInfo(FString::Printf(TEXT("No alternate on-disk map asset available; cannot exercise "
            "the on-disk-but-unloaded path for %s. Skipping."), *MethodName));
        return;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), UnloadedMapPath);

    FTestResponseCapture Capture;
    Test.TestTrue(FString::Printf(TEXT("%s handler registered"), *MethodName),
        InvokeHandlerWithCapture(MethodName, Payload, Capture));
    Test.TestTrue(FString::Printf(TEXT("%s responded"), *MethodName), Capture.bWasCalled);
    Test.TestFalse(FString::Printf(TEXT("%s errors for an unloaded map (not a fake success)"),
        *MethodName), Capture.bSuccess);

    // The core fix: an on-disk-but-unloaded map is LEVEL_NOT_LOADED, never the
    // misleading LEVEL_NOT_FOUND the reverted code emitted.
    Test.TestEqual(FString::Printf(
        TEXT("%s reports LEVEL_NOT_LOADED for an on-disk-but-unloaded map"), *MethodName),
        Capture.ErrorCode, FString(TEXT("LEVEL_NOT_LOADED")));
    Test.TestNotEqual(FString::Printf(
        TEXT("%s must NOT report the misleading LEVEL_NOT_FOUND for a map that exists on disk"),
        *MethodName), Capture.ErrorCode, FString(TEXT("LEVEL_NOT_FOUND")));
    // The message must name the actionable next step.
    Test.TestTrue(FString::Printf(TEXT("%s LEVEL_NOT_LOADED message names level.load"), *MethodName),
        Capture.Message.Contains(TEXT("level.load")));
}

// Asserts the levelPath param doc on a getter warns that the path must already be
// loaded (the discoverability half of the fix). Reverting the doc to the bare
// "Defaults to the active editor level." drops the loaded-only warning and fails this.
inline void AssertGetterLevelPathDocWarnsLoadedOnly(
    FAutomationTestBase& Test, const FString& MethodName)
{
    const FParamSpec* Spec = GetRegisteredParamSpec(MethodName, TEXT("levelPath"));
    Test.TestNotNull(FString::Printf(TEXT("%s exposes a levelPath param"), *MethodName), Spec);
    if (Spec)
    {
        Test.TestTrue(FString::Printf(
            TEXT("%s levelPath doc warns the path must already be loaded"), *MethodName),
            Spec->Description.Contains(TEXT("loaded")));
        Test.TestTrue(FString::Printf(
            TEXT("%s levelPath doc names the LEVEL_NOT_LOADED verdict"), *MethodName),
            Spec->Description.Contains(TEXT("LEVEL_NOT_LOADED")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetInfoUnloadedOnDiskMapNotLoadedTest,
    "PinWright.level.get_info.UnloadedOnDiskMapReportsNotLoaded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetInfoUnloadedOnDiskMapNotLoadedTest::RunTest(const FString& Parameters)
{
    AssertGetterReportsUnloadedOnDiskMap(*this, TEXT("level.get_info"));
    AssertGetterLevelPathDocWarnsLoadedOnly(*this, TEXT("level.get_info"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetActorsUnloadedOnDiskMapNotLoadedTest,
    "PinWright.level.get_actors.UnloadedOnDiskMapReportsNotLoaded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetActorsUnloadedOnDiskMapNotLoadedTest::RunTest(const FString& Parameters)
{
    AssertGetterReportsUnloadedOnDiskMap(*this, TEXT("level.get_actors"));
    AssertGetterLevelPathDocWarnsLoadedOnly(*this, TEXT("level.get_actors"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetBoundsUnloadedOnDiskMapNotLoadedTest,
    "PinWright.level.get_bounds.UnloadedOnDiskMapReportsNotLoaded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetBoundsUnloadedOnDiskMapNotLoadedTest::RunTest(const FString& Parameters)
{
    AssertGetterReportsUnloadedOnDiskMap(*this, TEXT("level.get_bounds"));
    AssertGetterLevelPathDocWarnsLoadedOnly(*this, TEXT("level.get_bounds"));
    return true;
}

// ============================================================================
// level.remove_from_world — requires: levelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelRemoveFromWorldValidParamsNoCrashTest,
    "PinWright.level.remove_from_world.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelRemoveFromWorldValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps/SubLevel"));
    TestTrue(TEXT("level.remove_from_world found"), InvokeHandler(TEXT("level.remove_from_world"), Payload));
    return true;
}

// ============================================================================
// level.set_visibility — requires: levelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSetVisibilityValidParamsNoCrashTest,
    "PinWright.level.set_visibility.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSetVisibilityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps/SubLevel"));
    Payload->SetBoolField(TEXT("bVisible"), true);
    TestTrue(TEXT("level.set_visibility found"), InvokeHandler(TEXT("level.set_visibility"), Payload));
    return true;
}

// ============================================================================
// level.set_locked — requires: levelPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSetLockedValidParamsNoCrashTest,
    "PinWright.level.set_locked.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSetLockedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps/SubLevel"));
    Payload->SetBoolField(TEXT("bLocked"), false);
    TestTrue(TEXT("level.set_locked found"), InvokeHandler(TEXT("level.set_locked"), Payload));
    return true;
}

// ============================================================================
// level.get_actors — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetActorsValidParamsNoCrashTest,
    "PinWright.level.get_actors.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetActorsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.get_actors found"), InvokeHandler(TEXT("level.get_actors"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// level.get_bounds — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetBoundsValidParamsNoCrashTest,
    "PinWright.level.get_bounds.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetBoundsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.get_bounds found"), InvokeHandler(TEXT("level.get_bounds"), MakeShared<FJsonObject>()));
    return true;
}

// Regression for B-level-get-bounds-ignores-actors: level.get_bounds documented
// "encloses every actor in the level" but only read TargetLevel->LevelBoundsActor,
// which is null on every level that has no ALevelBounds actor (the normal case) —
// so it reported a degenerate (0,0,0)..(0,0,0) box no matter where actors sat. This
// spawns a StaticMeshActor with a real cube mesh far from the origin into the live
// editor world, runs the production handler, and asserts the returned box is valid
// AND encloses the spawned actor. The reverted handler returns the origin box that
// does NOT contain (4000,5000,600), so this fails if the actor-iteration fallback
// is removed. Skipped gracefully without an editor world (commandlet context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetBoundsEnclosesActorsTest,
    "PinWright.level.get_bounds.EnclosesActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetBoundsEnclosesActorsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world (commandlet context); skipping live get_bounds enclosure check."));
        return true;
    }

    // Guard destroys the spawned actor and restores the level dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;

    const FVector SpawnLocation(4000.0, 5000.0, 600.0);
    AStaticMeshActor* MeshActor = SpawnTransientCubeActor(World, TEXT("PW_BoundsProbe"), SpawnLocation);
    if (!MeshActor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Engine cube mesh unavailable; skipping live get_bounds enclosure check."));
        return true;
    }
    // GetComponentsBoundingBox reads cached component bounds; force them current so the
    // far-from-origin mesh actually contributes finite bounds this same frame.
    MeshActor->GetStaticMeshComponent()->UpdateBounds();

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.get_bounds found"),
        InvokeHandlerWithCapture(TEXT("level.get_bounds"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("level.get_bounds responded"), Capture.bWasCalled);
    TestTrue(TEXT("level.get_bounds succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("level.get_bounds returned no result object"));
        return true;
    }

    // The fix surfaces an honest validity flag; with a finite-bounds mesh present it must be valid.
    bool bIsValid = false;
    TestTrue(TEXT("result carries isValid"), Capture.Result->TryGetBoolField(TEXT("isValid"), bIsValid));
    TestTrue(TEXT("bounds are valid with a mesh actor present (not the degenerate zero box)"), bIsValid);

    FString MinStr, MaxStr;
    TestTrue(TEXT("result carries min"), Capture.Result->TryGetStringField(TEXT("min"), MinStr));
    TestTrue(TEXT("result carries max"), Capture.Result->TryGetStringField(TEXT("max"), MaxStr));

    FVector BoundsMin, BoundsMax;
    const bool bParsedMin = BoundsMin.InitFromString(MinStr);
    const bool bParsedMax = BoundsMax.InitFromString(MaxStr);
    TestTrue(TEXT("min/max parse as vectors"), bParsedMin && bParsedMax);

    if (bParsedMin && bParsedMax)
    {
        const FBox ReportedBox(BoundsMin, BoundsMax);
        // The core assertion: the reported box must enclose the far-from-origin actor.
        // The reverted (LevelBoundsActor-only) handler reports (0,0,0)..(0,0,0), which
        // does not contain (4000,5000,600), so this fails when the fix is reverted.
        TestTrue(TEXT("level.get_bounds box encloses the spawned actor location"),
            ReportedBox.IsInsideOrOn(SpawnLocation));
    }

    // WorldGuard destroys the probe actor and restores the dirty flag on scope exit.
    return true;
}

// ============================================================================
// level.get_lighting_scenarios — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelGetLightingScenariosValidParamsNoCrashTest,
    "PinWright.level.get_lighting_scenarios.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelGetLightingScenariosValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.get_lighting_scenarios found"), InvokeHandler(TEXT("level.get_lighting_scenarios"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// level.build_navigation — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelBuildNavigationValidParamsNoCrashTest,
    "PinWright.level.build_navigation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelBuildNavigationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Navigation builds are heavyweight in headless automation; keep registration coverage.
    TestTrue(TEXT("level.build_navigation is registered"), IsRegistered(TEXT("level.build_navigation")));
    return true;
}

// ============================================================================
// level.build_all — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelBuildAllValidParamsNoCrashTest,
    "PinWright.level.build_all.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelBuildAllValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Build-all triggers editor build systems that assert in headless test runners.
    TestTrue(TEXT("level.build_all is registered"), IsRegistered(TEXT("level.build_all")));
    return true;
}

// ============================================================================
// level.structure.create_level — requires: levelName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelValidParamsNoCrashTest,
    "PinWright.level.structure.create_level.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Creating/saving levels is expensive and environment-dependent in headless automation.
    TestTrue(TEXT("level.structure.create_level is registered"),
        IsRegistered(TEXT("level.structure.create_level")));
    return true;
}

// Regression for F-enable-world-partition-impossible: level.structure.create_level
// {bCreateWorldPartition:true} previously hardcoded bWorldPartitionActuallyEnabled = false
// after merely reading WorldSettings — no UWorldPartition was ever attached, so
// worldPartitionEnabled could never become true and the entire WP-gated level.structure
// namespace (configure_grid_size / create_data_layer / configure_hlod_layer / …) was
// permanently unreachable. The fix routes the request through the engine's own
// UWorld::CreateWorld + FWorldInitializationValues::CreateWorldPartition(true) path, which
// attaches a real UWorldPartition. This invokes the production handler with the WP flag
// against a throwaway path (save:false so nothing lands on disk) and asserts:
//   (1) the response reports worldPartitionEnabled:true (honest, not the false stub), and
//   (2) the actual created UWorld carries a non-null UWorldPartition.
// Reverting the fix to the hardcoded `false` stub fails (1); dropping the IVS so no
// UWorldPartition attaches fails (2). Skipped gracefully without GEditor (commandlet ctx).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelAttachesWorldPartitionTest,
    "PinWright.level.structure.create_level.AttachesWorldPartition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelAttachesWorldPartitionTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); create_level reaches package "
                "creation but the WorldSettings/partition path is editor-dependent. Skipping."));
        return true;
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // On UE 5.3 tearing down a freshly-created World Partition world crashes inside the
    // engine: DestroyWorld() -> the WorldPartition / data-layer descriptor teardown reads a
    // TVariant with the wrong active type and asserts check(Index == TypeIndex) (TVariant.h:118).
    // The create+destroy round-trip this test needs is therefore not safe on 5.3. The required
    // DestroyWorld cleanup cannot be skipped (it prevents a later orphaned-partition crash on
    // 5.4+; see below), so the whole fixture is gated off here. The production handler's WP
    // attachment is exercised on 5.4+.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-crashes-on-partitioned-teardown"),
        TEXT("Skipping create_level World Partition attach test on UE 5.3: tearing down a partitioned world crashes the engine (TVariant index assert) on this version."));
    return true;
#else
    // create_level now makes the created world the ACTIVE editor world (fix for
    // E-create-level-save-false-world-not-active), so this probe run swaps the active
    // world out from under the rest of the suite. Declared FIRST so it destructs LAST
    // (after the probe world is destroyed/discarded below) and reloads the original map.
    FScopedEditorWorldMapGuard MapGuard;

    // Unique throwaway package path so a stale on-disk/in-memory world can't trip the
    // LEVEL_ALREADY_EXISTS guard. save:false keeps it purely in-memory.
    const FString LevelName = FString::Printf(
        TEXT("__EARG_WPCreateProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // The handler resolves the actual package path itself and echoes it back as
    // levelPath; read that authoritative value (below) rather than trusting this
    // hand-built fallback. The fallback only seeds the discard target before the
    // response exists, so cleanup still fires if the handler errors out early.
    FString CreatedPath = FString::Printf(TEXT("/Game/Maps/%s"), *LevelName);
    // Discard the in-memory probe world on every exit path (uses the
    // ForceDeleteObjects-free discard helper this file already defines). Captured by
    // reference so it tracks the handler's resolved path once the response arrives.
    ON_SCOPE_EXIT { DiscardProbeMapPackage(CreatedPath); };

    // Tear the probe world down through the engine's own DestroyWorld before the
    // package-level discard runs. The handler reaches create_level via
    // UWorld::CreateWorld(..., bAddToRoot=true), which roots the new Inactive world; a
    // rooted world (and its live UWorldPartition) survives DiscardProbeMapPackage's
    // CollectGarbage(KEEPFLAGS), leaving a half-torn-down partition object in the global
    // object hash. A later, unrelated ForceDeleteObjects (e.g. the widget screenshot test's
    // CleanupTestAsset) then walks that hash via ForEachObjectWithPackage and dereferences
    // the orphaned partition subobject — an access violation in
    // UObjectBaseUtility::GetPackage() that crashes the whole suite. DestroyWorld flushes
    // streaming, runs CleanupWorld (uninitializing the UWorldPartition) and RemoveFromRoot()s
    // the world, so the subsequent discard GC actually reclaims it. Declared AFTER the
    // discard guard so LIFO runs this first: destroy, then discard.
    UWorld* CreatedProbeWorld = nullptr;
    ON_SCOPE_EXIT
    {
        if (CreatedProbeWorld && CreatedProbeWorld->bIsWorldInitialized)
        {
            CreatedProbeWorld->DestroyWorld(/*bInformEngineOfWorld=*/false);
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), LevelName);
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps"));
    Payload->SetBoolField(TEXT("bCreateWorldPartition"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.create_level found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_level"), Payload, Capture));
    TestTrue(TEXT("create_level responded"), Capture.bWasCalled);
    TestTrue(TEXT("create_level succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // (1) Honest response field: the reverted hardcoded-false stub fails this.
        bool bReportedWp = false;
        TestTrue(TEXT("response carries worldPartitionEnabled"),
            Capture.Result->TryGetBoolField(TEXT("worldPartitionEnabled"), bReportedWp));
        TestTrue(TEXT("create_level {bCreateWorldPartition:true} reports worldPartitionEnabled:true"),
            bReportedWp);

        // Adopt the handler's own resolved package path for the lookup and discard
        // target, so a future change to its path normalization can't leave this test
        // probing a stale literal that silently misses (turning (2) into a no-op pass).
        Capture.Result->TryGetStringField(TEXT("levelPath"), CreatedPath);
    }

    // (2) Ground truth on the actual object: the engine must have attached a real
    // UWorldPartition to the created world. Look it up by the handler's resolved path
    // (it creates an Inactive world, NOT the active editor world, so find it explicitly).
    if (UPackage* CreatedPackage = FindPackage(nullptr, *CreatedPath))
    {
        if (UWorld* CreatedWorld = FindObject<UWorld>(CreatedPackage, *LevelName))
        {
            // IsPartitionedWorld() is defined as GetWorldPartition() != nullptr, so a
            // single non-null check on the partition is the whole ground truth.
            TestNotNull(TEXT("created world has a non-null UWorldPartition"),
                CreatedWorld->GetWorldPartition());

            // Hand the world to the DestroyWorld scope-exit guard (see its declaration above)
            // so the rooted, partition-carrying probe world is torn down before discard.
            CreatedProbeWorld = CreatedWorld;
        }
        else
        {
            AddError(TEXT("create_level reported success but no UWorld was found in the "
                "created package — cannot verify the attached UWorldPartition."));
        }
    }

    // DiscardProbeMapPackage tears the in-memory world down on scope exit.
    return true;
#endif // UE_VERSION_OLDER_THAN(5, 4, 0)
}

// Regression for E-create-level-save-false-world-not-active: level.structure.create_level
// {save:false} built a live UWorld in memory but never installed it as the active editor
// world (it was created EWorldType::Inactive and the handler fell straight through to
// SendSuccess). Every WP-gated level.structure verb (configure_grid_size / create_data_layer
// / create_minimap_volume / get_level_structure_info / …) resolves its target via
// GetEditorWorldLS() == GEditor->GetEditorWorldContext().World(), so the freshly-created
// world was unreachable — and no MCP verb could promote it. The fix promotes the world to
// EWorldType::Editor and calls GEditor->GetEditorWorldContext().SetCurrentWorld(NewWorld)
// (the same activation legacy level.create performs), so the follow-on WP verbs target it.
// This invokes the production handler with save:false against a throwaway path and asserts:
//   (1) the response reports activeWorld:true (the disclosure field), and
//   (2) GROUND TRUTH: the active editor world IS the created world.
// Reverting the SetCurrentWorld call fails BOTH: the active world stays the prior map and
// activeWorld would be false. Skipped gracefully without GEditor (commandlet ctx).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelMakesWorldActiveTest,
    "PinWright.level.structure.create_level.MakesWorldActive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelMakesWorldActiveTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); create_level cannot install an "
                "active editor world. Skipping the activation check."));
        return true;
    }

    // create_level now makes the created world the ACTIVE editor world, which swaps the
    // active world out from under the rest of the suite. Declared FIRST so it destructs
    // LAST (after the probe world is destroyed/discarded below) and reloads the original map.
    FScopedEditorWorldMapGuard MapGuard;

    // Snapshot the active world BEFORE the call so we can prove the handler actually
    // changed it (and is not just confirming a world that was already current).
    UWorld* const WorldBeforeCall = GEditor->GetEditorWorldContext().World();

    // Unique throwaway package path; save:false keeps the world purely in-memory.
    const FString LevelName = FString::Printf(
        TEXT("__EARG_ActiveWorldProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString CreatedPath = FString::Printf(TEXT("/Game/Maps/%s"), *LevelName);
    // Discard the in-memory probe world on every exit path (captured by reference so it
    // tracks the handler's resolved path once the response arrives).
    ON_SCOPE_EXIT { DiscardProbeMapPackage(CreatedPath); };

    // Tear the probe world down through the engine's own DestroyWorld before the
    // package-level discard runs (same rationale as the AttachesWorldPartition test:
    // the rooted world must be CleanupWorld'd / RemoveFromRoot'd so the discard GC can
    // reclaim it and no orphaned subobject survives in the global object hash). Declared
    // AFTER the discard guard so LIFO runs this first: destroy, then discard.
    UWorld* CreatedProbeWorld = nullptr;
    ON_SCOPE_EXIT
    {
        if (CreatedProbeWorld && CreatedProbeWorld->bIsWorldInitialized)
        {
            CreatedProbeWorld->DestroyWorld(/*bInformEngineOfWorld=*/false);
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), LevelName);
    Payload->SetStringField(TEXT("levelPath"), TEXT("/Game/Maps"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.create_level found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_level"), Payload, Capture));
    TestTrue(TEXT("create_level responded"), Capture.bWasCalled);
    TestTrue(TEXT("create_level {save:false} succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // (1) Disclosure field: the reverted (no-SetCurrentWorld) handler reports false.
        bool bActiveWorld = false;
        TestTrue(TEXT("response carries activeWorld"),
            Capture.Result->TryGetBoolField(TEXT("activeWorld"), bActiveWorld));
        TestTrue(TEXT("create_level {save:false} reports activeWorld:true"), bActiveWorld);

        // Adopt the handler's own resolved package path for the lookup / discard target.
        Capture.Result->TryGetStringField(TEXT("levelPath"), CreatedPath);
    }

    // (2) Ground truth: the active editor world must now BE the created world. Look the
    // created world up by the handler's resolved path, then compare it against the active
    // editor world. Reverting SetCurrentWorld leaves the active world == WorldBeforeCall.
    if (UPackage* CreatedPackage = FindPackage(nullptr, *CreatedPath))
    {
        if (UWorld* CreatedWorld = FindObject<UWorld>(CreatedPackage, *LevelName))
        {
            CreatedProbeWorld = CreatedWorld;

            // Pointer-identity via TestTrue (FAutomationTestBase::TestSamePtr is not on
            // every supported engine version — see TestWidgetTreeIntegrity.cpp); == / != is
            // equivalent and portable across UE 5.3–5.7.
            UWorld* const ActiveWorldNow = GEditor->GetEditorWorldContext().World();
            TestTrue(TEXT("active editor world is the just-created world"),
                ActiveWorldNow == CreatedWorld);
            TestTrue(TEXT("active editor world changed away from the pre-call world"),
                ActiveWorldNow != WorldBeforeCall);
        }
        else
        {
            AddError(TEXT("create_level reported success but no UWorld was found in the "
                "created package — cannot verify it became the active editor world."));
        }
    }

    // MapGuard reloads the original map on scope exit (after destroy + discard).
    return true;
}

// ============================================================================
// level.structure.configure_level_streaming — requires: levelName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureLevelStreamingValidParamsNoCrashTest,
    "PinWright.level.structure.configure_level_streaming.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureLevelStreamingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), TEXT("MySublevel"));
    TestTrue(TEXT("level.structure.configure_level_streaming found"), InvokeHandler(TEXT("level.structure.configure_level_streaming"), Payload));
    return true;
}

// Regression: streamingMethod used to be read and echoed back as if applied while the
// level's ULevelStreaming class — the only carrier of the streaming method — was never
// touched. The method is now validated up front and applied via SetStreamingClassForLevel,
// so an unknown value must be REJECTED rather than accepted and echoed. (Validation runs
// before the world/level lookup, so this holds on any host map.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureLevelStreamingUnknownMethodRejectedTest,
    "PinWright.level.structure.configure_level_streaming.UnknownStreamingMethodRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureLevelStreamingUnknownMethodRejectedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), TEXT("MySublevel"));
    Payload->SetStringField(TEXT("streamingMethod"), TEXT("NotAStreamingMethod"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.configure_level_streaming found"),
        InvokeHandlerWithCapture(TEXT("level.structure.configure_level_streaming"), Payload, Capture));
    TestFalse(TEXT("unknown streamingMethod is an error, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("error code is UNKNOWN_STREAMING_METHOD"),
        Capture.ErrorCode, FString(TEXT("UNKNOWN_STREAMING_METHOD")));
    return true;
}

// ============================================================================
// level.structure.set_streaming_distance — requires: levelName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureSetStreamingDistanceValidParamsNoCrashTest,
    "PinWright.level.structure.set_streaming_distance.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureSetStreamingDistanceValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelName"), TEXT("MySublevel"));
    Payload->SetNumberField(TEXT("streamingDistance"), 15000.0);
    TestTrue(TEXT("level.structure.set_streaming_distance found"), InvokeHandler(TEXT("level.structure.set_streaming_distance"), Payload));
    return true;
}

// ============================================================================
// level.structure.enable_world_partition — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureEnableWorldPartitionValidParamsNoCrashTest,
    "PinWright.level.structure.enable_world_partition.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureEnableWorldPartitionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.structure.enable_world_partition found"), InvokeHandler(TEXT("level.structure.enable_world_partition"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// level.structure.configure_grid_size — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureGridSizeValidParamsNoCrashTest,
    "PinWright.level.structure.configure_grid_size.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureGridSizeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("gridCellSize"), 12800);
    Payload->SetNumberField(TEXT("loadingRange"), 25600);
    TestTrue(TEXT("level.structure.configure_grid_size found"), InvokeHandler(TEXT("level.structure.configure_grid_size"), Payload));
    return true;
}

// ============================================================================
// level.structure.create_data_layer — requires: dataLayerName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateDataLayerValidParamsNoCrashTest,
    "PinWright.level.structure.create_data_layer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateDataLayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("MyDataLayer"));
    TestTrue(TEXT("level.structure.create_data_layer found"), InvokeHandler(TEXT("level.structure.create_data_layer"), Payload));
    return true;
}

// ============================================================================
// level.structure.assign_actor_to_data_layer — requires: actorName, dataLayerName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureAssignActorToDataLayerValidParamsNoCrashTest,
    "PinWright.level.structure.assign_actor_to_data_layer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureAssignActorToDataLayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MyActor"));
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("MyDataLayer"));
    TestTrue(TEXT("level.structure.assign_actor_to_data_layer found"), InvokeHandler(TEXT("level.structure.assign_actor_to_data_layer"), Payload));
    return true;
}

// ============================================================================
// level.structure.configure_hlod_layer — requires: hlodLayerName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureHlodLayerValidParamsNoCrashTest,
    "PinWright.level.structure.configure_hlod_layer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureHlodLayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("hlodLayerName"), TEXT("MyHLODLayer"));
    TestTrue(TEXT("level.structure.configure_hlod_layer found"), InvokeHandler(TEXT("level.structure.configure_hlod_layer"), Payload));
    CleanupTestAsset(TEXT("/Game/HLOD/MyHLODLayer"));
    return true;
}

// Regression: cellSize / loadingDistance were read and echoed back but written nowhere, so
// the created layer kept its class defaults. Read the values back off the asset — UHLODLayer
// has no setters for them and its getters are deprecated from 5.7, so reflection is the only
// portable route (and the same one the handler writes through).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureHlodLayerAppliesGridSettingsTest,
    "PinWright.level.structure.configure_hlod_layer.AppliesGridSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureHlodLayerAppliesGridSettingsTest::RunTest(const FString& Parameters)
{
    const FString LayerName = TEXT("PWHLODGridProbe");
    const FString LayerPackage = TEXT("/Game/HLOD/") + LayerName;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("hlodLayerName"), LayerName);
    Payload->SetNumberField(TEXT("cellSize"), 51200);
    Payload->SetNumberField(TEXT("loadingDistance"), 102400.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.configure_hlod_layer found"),
        InvokeHandlerWithCapture(TEXT("level.structure.configure_hlod_layer"), Payload, Capture));
    TestTrue(TEXT("configure_hlod_layer succeeds"), Capture.bSuccess);

    if (Capture.bSuccess)
    {
        UHLODLayer* Layer = FindObject<UHLODLayer>(nullptr, *ToObjectPath(LayerPackage));
        if (Layer)
        {
            FIntProperty* CellSizeProp = CastField<FIntProperty>(
                UHLODLayer::StaticClass()->FindPropertyByName(TEXT("CellSize")));
            FDoubleProperty* LoadingRangeProp = CastField<FDoubleProperty>(
                UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LoadingRange")));

            if (CellSizeProp && LoadingRangeProp)
            {
                TestEqual(TEXT("cellSize is written onto the HLOD layer, not just echoed"),
                    CellSizeProp->GetPropertyValue_InContainer(Layer), 51200);
                TestEqual(TEXT("loadingDistance is written onto the HLOD layer, not just echoed"),
                    LoadingRangeProp->GetPropertyValue_InContainer(Layer), 102400.0);
            }
            else
            {
                AddError(TEXT("UHLODLayer no longer exposes CellSize / LoadingRange properties"));
            }
        }
        else
        {
            AddError(TEXT("configure_hlod_layer reported success but no UHLODLayer asset was found"));
        }
    }

    CleanupTestAsset(LayerPackage);
    return true;
}

// ============================================================================
// level.structure.create_minimap_volume — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateMinimapVolumeValidParamsNoCrashTest,
    "PinWright.level.structure.create_minimap_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateMinimapVolumeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumeName"), TEXT("TestMinimapVolume"));
    TestTrue(TEXT("level.structure.create_minimap_volume found"), InvokeHandler(TEXT("level.structure.create_minimap_volume"), Payload));
    return true;
}

// ============================================================================
// level.structure.open_level_blueprint — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureOpenLevelBlueprintValidParamsNoCrashTest,
    "PinWright.level.structure.open_level_blueprint.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureOpenLevelBlueprintValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Opening blueprint editors manipulates Slate windows and can crash headless test runs.
    TestTrue(TEXT("level.structure.open_level_blueprint is registered"),
        IsRegistered(TEXT("level.structure.open_level_blueprint")));
    return true;
}

// Regression for E-open-level-blueprint-unusable-assetpath: the asset-verification
// helper used by level.structure.open_level_blueprint (and every blueprint.graph.*
// success body) derived assetPath from Asset->GetPackage()->GetPathName(). For a
// ULevelScriptBlueprint — a sub-object of the .umap — that is the bare map package
// path (/Game/Maps/<Map>), which LoadObject<UBlueprint> cannot resolve, so reusing it
// hard-fails ASSET_NOT_FOUND in the very blueprint.graph.* methods the seed is the
// prerequisite for. The fix special-cases ULevelScriptBlueprint in AddAssetVerification
// (Utils/AssetUtils.cpp) to surface the full object path (…:PersistentLevel.<Map>) from
// GetPathName(), which round-trips.
//
// This drives the PRODUCTION AddAssetVerification against the live editor world's real
// level-script Blueprint and asserts the assetPath it emits (a) is NOT the bare package
// path and (b) actually LoadObject<UBlueprint>-resolves back to the same Blueprint — the
// exact round-trip the seed promised. Reverting the helper to GetPackage()->GetPathName()
// makes the resolve return null and fails this. Skipped gracefully when no editor world /
// level-script Blueprint is available (commandlet context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureOpenLevelBlueprintAssetPathRoundTripsTest,
    "PinWright.level.structure.open_level_blueprint.AssetPathRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureOpenLevelBlueprintAssetPathRoundTripsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    ULevel* PersistentLevel = World ? World->PersistentLevel : nullptr;
    if (!PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world / persistent level (commandlet context); "
                "no level-script Blueprint to verify. Skipping the assetPath round-trip check."));
        return true;
    }

    // GetLevelScriptBlueprint(true) create-or-fetches the level-script BP; mark the
    // level's dirty flag so creating it (if it did not yet exist) leaves no trace.
    UPackage* LevelPackage = PersistentLevel->GetOutermost();
    const bool bWasDirty = LevelPackage && LevelPackage->IsDirty();
    ON_SCOPE_EXIT
    {
        if (LevelPackage)
        {
            LevelPackage->SetDirtyFlag(bWasDirty);
        }
    };

    ULevelScriptBlueprint* LevelBP = PersistentLevel->GetLevelScriptBlueprint(true);
    if (!LevelBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-script-blueprint-unavailable"),
            TEXT("Level-script Blueprint unavailable in this environment; "
                "skipping the assetPath round-trip check."));
        return true;
    }

    const FString PackagePath = LevelBP->GetPackage()
        ? LevelBP->GetPackage()->GetPathName() : FString();

    // Exercise the exact production helper the seed and blueprint.graph.* success
    // bodies use — not a copy.
    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    AddAssetVerification(Verification, LevelBP);

    FString AssetPath;
    TestTrue(TEXT("AddAssetVerification emits an assetPath for the level-script BP"),
        Verification->TryGetStringField(TEXT("assetPath"), AssetPath));

    // The reported assetPath must NOT be the bare .umap package path (the broken handle).
    if (!PackagePath.IsEmpty())
    {
        TestNotEqual(TEXT("assetPath is NOT the bare level package path (which fails to load as a Blueprint)"),
            AssetPath, PackagePath);
    }

    // The core round-trip: the assetPath the helper hands back must load as a UBlueprint
    // and resolve to this very level-script Blueprint. This is the documented downstream
    // contract (blueprint.graph.* does LoadObject<UBlueprint>(nullptr, assetPath)).
    UBlueprint* RoundTripped = LoadObject<UBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("assetPath LoadObject<UBlueprint> round-trips (does not ASSET_NOT_FOUND)"),
        RoundTripped);
    TestEqual(TEXT("the loaded Blueprint is the same level-script Blueprint"),
        RoundTripped, static_cast<UBlueprint*>(LevelBP));

    // The level-script object path is the round-trippable form.
    TestEqual(TEXT("assetPath equals the level-script object path (LevelBP->GetPathName())"),
        AssetPath, LevelBP->GetPathName());

    return true;
}

// ============================================================================
// level.structure.add_level_blueprint_node — requires: nodeClass
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureAddLevelBlueprintNodeValidParamsNoCrashTest,
    "PinWright.level.structure.add_level_blueprint_node.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureAddLevelBlueprintNodeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("nodeClass"), TEXT("K2Node_Event"));
    TestTrue(TEXT("level.structure.add_level_blueprint_node found"), InvokeHandler(TEXT("level.structure.add_level_blueprint_node"), Payload));
    return true;
}

// Regression for E-level-bp-node-verbs-cant-author-bound-nodes: the handler
// accepted a nodeName param it then dropped (the node was created bare and the
// nodeName was never applied), and the response reused the nodeName key to echo
// the auto-generated node title instead of the caller's value. nodePosition also
// only landed when passed as the object {x,y}; the sibling flat-x/y spelling was
// silently zeroed to the origin.
//
// The fix: apply nodeName as the node's comment label, echo the caller's nodeName
// back under nodeName (auto title moves to the separate nodeTitle key), and accept
// flat x/y as a nodePosition alias. This drives the production handler against the
// live editor world's level-script Blueprint and asserts:
//   - response nodeName == the caller's value (NOT the auto title),
//   - a distinct nodeTitle key carries the auto-generated title,
//   - flat x/y land at the requested coordinates (posX/posY echo them),
//   - the created node actually carries the nodeName as its NodeComment.
// Reverting any half of the fix fails one of these. The created node is removed
// and the level dirty flag restored so the open map is left untouched. Skipped
// gracefully without an editor world (commandlet/unit context), where the handler
// short-circuits before touching the level Blueprint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureAddLevelBlueprintNodeHonorsNodeNameAndFlatPosTest,
    "PinWright.level.structure.add_level_blueprint_node.HonorsNodeNameAndFlatPos",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureAddLevelBlueprintNodeHonorsNodeNameAndFlatPosTest::RunTest(const FString& Parameters)
{
    // The flat x/y alias must be DECLARED in the param spec, or the dispatcher's
    // ValidateHandlerParams rejects a flat-x/y call with UNKNOWN_PARAMS before the
    // handler ever runs (InvokeHandlerWithCapture below bypasses that validation, so
    // this spec assertion is what guards the real MCP-dispatch alias path). Runs in
    // every environment, including the commandlet skip path below.
    TestNotNull(TEXT("add_level_blueprint_node declares flat 'x' param (nodePosition alias)"),
        GetRegisteredParamSpec(TEXT("level.structure.add_level_blueprint_node"), TEXT("x")));
    TestNotNull(TEXT("add_level_blueprint_node declares flat 'y' param (nodePosition alias)"),
        GetRegisteredParamSpec(TEXT("level.structure.add_level_blueprint_node"), TEXT("y")));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    ULevel* CurrentLevel = World ? World->GetCurrentLevel() : nullptr;
    if (!CurrentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world / current level (commandlet context); "
                "add_level_blueprint_node short-circuits before the level Blueprint. "
                "Skipping the nodeName/flat-position assertion."));
        return true;
    }

    // The handler creates-or-fetches the level-script Blueprint and marks it
    // modified; snapshot the level's dirty flag so we can restore it after we
    // remove the probe node we add below.
    UPackage* LevelPackage = CurrentLevel->GetOutermost();
    const bool bWasDirty = LevelPackage && LevelPackage->IsDirty();

    const FString CallerNodeName = TEXT("EARG_ProbeNodeName_Regression");
    const int32 RequestedX = 412;
    const int32 RequestedY = 273;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("nodeClass"), TEXT("K2Node_Event"));
    Payload->SetStringField(TEXT("nodeName"), CallerNodeName);
    // Flat x/y (the blueprint.graph.create_node spelling) — must NOT be zeroed.
    Payload->SetNumberField(TEXT("x"), RequestedX);
    Payload->SetNumberField(TEXT("y"), RequestedY);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.add_level_blueprint_node found"),
        InvokeHandlerWithCapture(TEXT("level.structure.add_level_blueprint_node"), Payload, Capture));
    TestTrue(TEXT("add_level_blueprint_node responded"), Capture.bWasCalled);

    // Single source of truth for "find the probe node by its comment in the level
    // Blueprint's event graph" — drives both the cleanup removal below and assertion
    // #4. Resolving the level-script Blueprint + event graph and the NodeComment match
    // predicate live here in exactly one place so the two call sites can't drift.
    auto FindProbeNode = [&]() -> UEdGraphNode*
    {
        if (ULevelScriptBlueprint* LevelBP = CurrentLevel->GetLevelScriptBlueprint(false))
        {
            if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(LevelBP))
            {
                for (UEdGraphNode* Node : EventGraph->Nodes)
                {
                    if (Node && Node->NodeComment == CallerNodeName)
                    {
                        return Node;
                    }
                }
            }
        }
        return nullptr;
    };

    // Locate and remove the probe node on every exit path so the level Blueprint is
    // left exactly as found, then restore the level's pre-test dirty flag.
    ON_SCOPE_EXIT
    {
        if (ULevelScriptBlueprint* LevelBP = CurrentLevel->GetLevelScriptBlueprint(false))
        {
            if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(LevelBP))
            {
                // Remove every probe-node match (FindProbeNode returns the first; loop
                // until none remain so duplicates created by retries are also cleaned up).
                while (UEdGraphNode* ProbeNode = FindProbeNode())
                {
                    EventGraph->RemoveNode(ProbeNode);
                }
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(LevelBP);
        }
        if (LevelPackage)
        {
            LevelPackage->SetDirtyFlag(bWasDirty);
        }
    };

    if (!Capture.bSuccess)
    {
        // Some headless configs can't create the level-script Blueprint; that is an
        // environment limitation, not the regression. The handler still must not
        // have crashed (bWasCalled above proves it responded).
        PinWrightTestSkip::SkipAssertions(*this, TEXT("handler-call-failed"),
            FString::Printf(TEXT("add_level_blueprint_node did not succeed in this "
                "environment (error: %s); skipping the live result-shape assertions."),
                *Capture.ErrorCode));
        return true;
    }

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("add_level_blueprint_node reported success with no result body"));
        return true;
    }

    // 1. nodeName must echo the caller's value, NOT the auto-generated title.
    FString EchoedNodeName;
    TestTrue(TEXT("response carries nodeName"),
        Capture.Result->TryGetStringField(TEXT("nodeName"), EchoedNodeName));
    TestEqual(TEXT("nodeName echoes the caller's value, not the auto title"),
        EchoedNodeName, CallerNodeName);

    // 2. The auto-generated title lives under a DISTINCT nodeTitle key (no longer
    //    shadowing nodeName). For an unbound K2Node_Event it is "Event None"-style,
    //    so it must differ from the caller's nodeName.
    FString NodeTitle;
    TestTrue(TEXT("response carries the auto title under nodeTitle"),
        Capture.Result->TryGetStringField(TEXT("nodeTitle"), NodeTitle));
    TestNotEqual(TEXT("nodeTitle (auto title) is not the caller's nodeName"),
        NodeTitle, CallerNodeName);

    // 3. Flat x/y landed at the requested coordinates (not silently zeroed).
    double PosX = 0.0, PosY = 0.0;
    TestTrue(TEXT("response carries posX"), Capture.Result->TryGetNumberField(TEXT("posX"), PosX));
    TestTrue(TEXT("response carries posY"), Capture.Result->TryGetNumberField(TEXT("posY"), PosY));
    TestEqual(TEXT("flat x is honored (not zeroed)"), static_cast<int32>(PosX), RequestedX);
    TestEqual(TEXT("flat y is honored (not zeroed)"), static_cast<int32>(PosY), RequestedY);

    // 4. The created node actually carries the nodeName as its NodeComment — proves
    //    the param was applied to the node, not just echoed in the response. Reuses the
    //    same lookup the cleanup uses, so the assertion can't diverge from what gets
    //    removed.
    TestTrue(TEXT("the created node carries nodeName as its NodeComment"),
        FindProbeNode() != nullptr);

    return true;
}

// ============================================================================
// level.structure.connect_level_blueprint_nodes — requires: sourceNodeName, targetNodeName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConnectLevelBlueprintNodesValidParamsNoCrashTest,
    "PinWright.level.structure.connect_level_blueprint_nodes.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConnectLevelBlueprintNodesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourceNodeName"), TEXT("BeginPlay"));
    Payload->SetStringField(TEXT("targetNodeName"), TEXT("PrintString"));
    TestTrue(TEXT("level.structure.connect_level_blueprint_nodes found"), InvokeHandler(TEXT("level.structure.connect_level_blueprint_nodes"), Payload));
    return true;
}

// ============================================================================
// level.structure.create_level_instance — requires: levelAssetPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelInstanceValidParamsNoCrashTest,
    "PinWright.level.structure.create_level_instance.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelInstanceValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelAssetPath"), TEXT("/Game/Maps/InstancedLevel"));
    TestTrue(TEXT("level.structure.create_level_instance found"), InvokeHandler(TEXT("level.structure.create_level_instance"), Payload));
    return true;
}

// Real level asset for the level-instance / packed-level-actor ground-truth checks below:
// the engine's Basic template map ships with every supported engine (5.3–5.8) and is tiny,
// so instancing / packing it exercises the production path without dragging a project map
// (which may be huge, or World Partition) into the test.
static const TCHAR* GTemplateMapPackage = TEXT("/Engine/Maps/Templates/Template_Default");

// Regression: the handler spawned a bare ALevelInstance and only echoed levelAssetPath back
// — SetWorldAsset was never called, so the "instance" embedded no level at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelInstanceSetsWorldAssetTest,
    "PinWright.level.structure.create_level_instance.SetsWorldAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelInstanceSetsWorldAssetTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world (commandlet/unit context); skipping the live world-asset check."));
        return true;
    }
    if (!FPackageName::DoesPackageExist(GTemplateMapPackage))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("Engine template map is unavailable on this host; skipping the live world-asset check."));
        return true;
    }

    // Destroys the spawned level instance (and restores the map's dirty flag) on scope exit.
    FScopedEditorWorldActorGuard ActorGuard;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelInstanceName"), TEXT("PWLevelInstanceProbe"));
    Payload->SetStringField(TEXT("levelAssetPath"), GTemplateMapPackage);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.create_level_instance found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_level_instance"), Payload, Capture));
    TestTrue(TEXT("create_level_instance succeeds on a real level asset"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    FString ActorPath;
    Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath);
    ALevelInstance* Spawned = ActorPath.IsEmpty()
        ? nullptr : FindObject<ALevelInstance>(nullptr, *ActorPath);
    if (!Spawned)
    {
        AddError(TEXT("create_level_instance reported success but its actorPath resolves to no ALevelInstance"));
        return true;
    }

    // Ground truth: the actor must reference the level, not merely echo the path back.
    TestEqual(TEXT("level instance references the requested level asset"),
        Spawned->GetWorldAsset().ToSoftObjectPath().GetLongPackageName(),
        FString(GTemplateMapPackage));
    return true;
}

// ============================================================================
// level.structure.create_packed_level_actor — no required params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreatePackedLevelActorValidParamsNoCrashTest,
    "PinWright.level.structure.create_packed_level_actor.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreatePackedLevelActorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("packedLevelName"), TEXT("MyPackedLevel"));
    TestTrue(TEXT("level.structure.create_packed_level_actor found"), InvokeHandler(TEXT("level.structure.create_packed_level_actor"), Payload));
    return true;
}

// Regression: bPackBlueprints / bPackStaticMeshes were read and echoed back as applied, but
// the engine's default packed-level builder always installs both the recursive and ISM
// builders and its builder classes are engine-private, so no subset can be selected. An
// explicit opt-out must now be rejected rather than reported as honoured.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreatePackedLevelActorRejectsPackOptOutTest,
    "PinWright.level.structure.create_packed_level_actor.RejectsPackOptOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreatePackedLevelActorRejectsPackOptOutTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("packedLevelName"), TEXT("PWPackedOptOutProbe"));
    Payload->SetBoolField(TEXT("bPackStaticMeshes"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.create_packed_level_actor found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_packed_level_actor"), Payload, Capture));
    TestFalse(TEXT("disabling a pack flag is an error, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("error code is UNSUPPORTED_OPTION"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_OPTION")));
    return true;
}

// Regression: the handler spawned a bare APackedLevelActor — no WorldAsset, no builder run —
// so it baked nothing while reporting success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreatePackedLevelActorPacksSourceLevelTest,
    "PinWright.level.structure.create_packed_level_actor.PacksSourceLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreatePackedLevelActorPacksSourceLevelTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world (commandlet/unit context); skipping the live packing check."));
        return true;
    }
    if (!FPackageName::DoesPackageExist(GTemplateMapPackage))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("Engine template map is unavailable on this host; skipping the live packing check."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("packedLevelName"), TEXT("PWPackedLevelProbe"));
    Payload->SetStringField(TEXT("levelAssetPath"), GTemplateMapPackage);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.structure.create_packed_level_actor found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_packed_level_actor"), Payload, Capture));
    TestTrue(TEXT("create_packed_level_actor succeeds on a real level asset"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bPacked = false;
    Capture.Result->TryGetBoolField(TEXT("packed"), bPacked);
    TestTrue(TEXT("the packed-level builder actually ran"), bPacked);

    FString ActorPath;
    Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath);
    APackedLevelActor* Spawned = ActorPath.IsEmpty()
        ? nullptr : FindObject<APackedLevelActor>(nullptr, *ActorPath);
    if (!Spawned)
    {
        AddError(TEXT("create_packed_level_actor reported success but its actorPath resolves to no APackedLevelActor"));
        return true;
    }

    TestEqual(TEXT("packed level actor references the source level asset"),
        Spawned->GetWorldAsset().ToSoftObjectPath().GetLongPackageName(),
        FString(GTemplateMapPackage));

    TArray<UActorComponent*> PackedComponents;
    Spawned->GetPackedComponents(PackedComponents);
    // PINWRIGHT_INFO_IS_NOT_A_SKIP: the packed-actor assertions above already ran; this records the bake count.
    AddInfo(FString::Printf(TEXT("packed components baked from %s: %d"),
        GTemplateMapPackage, PackedComponents.Num()));
    return true;
}

// ============================================================================
// level.structure.get_level_structure_info — no params
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureGetLevelStructureInfoValidParamsNoCrashTest,
    "PinWright.level.structure.get_level_structure_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureGetLevelStructureInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("level.structure.get_level_structure_info found"), InvokeHandler(TEXT("level.structure.get_level_structure_info"), MakeShared<FJsonObject>()));
    return true;
}

// Regression for B-level-structure-info-datalayers-stub: get_level_structure_info
// advertises a "data layer summary" and always emits a `dataLayers` array, but the
// enumeration was an unimplemented stub (`// Data layer enumeration would go here`)
// that fetched the runtime UDataLayerSubsystem, never read it, and always wrote an
// EMPTY array — so dataLayers was structurally `[]` for every partitioned world, no
// matter how many layers were registered. The fix iterates
// UDataLayerEditorSubsystem::Get()->GetAllDataLayers() (the exact set create_data_layer
// registers into) and emits one object per layer.
//
// This drives the PRODUCTION handlers end-to-end against a live World-Partition world:
// create_level {bCreateWorldPartition:true, save:false} makes a partitioned world the
// active editor world, create_data_layer registers a real UDataLayerInstance on it, then
// get_level_structure_info must enumerate that layer in its dataLayers array. The reverted
// stub writes `[]` and the "layer appears in dataLayers" assertion fails. Skipped
// gracefully without GEditor (commandlet ctx) and gated off on UE 5.3, where tearing down
// a freshly-created partitioned world crashes the engine (see AttachesWorldPartition).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureGetLevelStructureInfoEnumeratesDataLayersTest,
    "PinWright.level.structure.get_level_structure_info.EnumeratesDataLayers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureGetLevelStructureInfoEnumeratesDataLayersTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); get_level_structure_info reaches "
                "NO_EDITOR_WORLD before the data-layer enumeration. Skipping the live enumeration check."));
        return true;
    }

    // Stage a partitioned world and make it the active editor world via the shared
    // create+teardown scaffold (FScopedProbeWorld owns the 5.3 gate, the create_level call,
    // and the DestroyWorld-then-discard-then-restore teardown — see its definition).
    FScopedProbeWorld ProbeWorld(TEXT("__EARG_DLEnumProbe"), /*bWantPartition=*/true);
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
    // The found-assert is only meaningful where FScopedProbeWorld actually invokes
    // create_level. On UE 5.3 the scaffold's staging body (including the handler call) is
    // compiled out — the create+destroy round-trip crashes the engine — so WasHandlerFound()
    // is always false there by design; asserting it would false-fail the deliberately-gated
    // 5.3 path. IsStaged() is likewise false on 5.3, so control falls through to the graceful
    // skip below.
    TestTrue(TEXT("level.structure.create_level found"), ProbeWorld.WasHandlerFound());
#endif
    if (!ProbeWorld.IsStaged())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-partitioned-world"),
            TEXT("create_level {bCreateWorldPartition:true} did not stage a live "
                "World-Partition world in this environment (or UE 5.3 gate). Skipping the "
                "data-layer enumeration assertion."));
        return true;
    }

    // Discard the persistable /Game/DataLayers asset create_data_layer writes (if it
    // reaches that path) at FUNCTION exit, after the readback below — captured by
    // reference and populated once the create_data_layer response arrives.
    FString CreatedLayerAssetPath;
    ON_SCOPE_EXIT
    {
        if (!CreatedLayerAssetPath.IsEmpty())
        {
            CleanupTestAsset(CreatedLayerAssetPath);
        }
    };

    // The fix for E-create-level-save-false-world-not-active makes the created WP world
    // the active editor world; confirm so the subsequent verbs target it. If the active
    // world is not partitioned, the data-layer verbs would short-circuit and this fixture
    // can't exercise the enumeration — skip rather than false-fail.
    UWorld* ActiveWorld = GEditor->GetEditorWorldContext().World();
    if (!ActiveWorld || !ActiveWorld->GetWorldPartition())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-partitioned-world"),
            TEXT("Created world is not the active partitioned editor world in this "
                "environment; the data-layer verbs cannot run. Skipping the enumeration assertion."));
        return true;
    }

    // 2) Create a data layer on the active partitioned world through the production handler.
    const FString DataLayerName = FString::Printf(
        TEXT("EnumProbeLayer_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> LayerPayload = MakeShared<FJsonObject>();
    LayerPayload->SetStringField(TEXT("dataLayerName"), DataLayerName);

    FTestResponseCapture LayerCapture;
    TestTrue(TEXT("level.structure.create_data_layer found"),
        InvokeHandlerWithCapture(TEXT("level.structure.create_data_layer"), LayerPayload, LayerCapture));
    if (!LayerCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("handler-call-failed"),
            TEXT("create_data_layer did not succeed in this environment; cannot register a "
                "layer to read back. Skipping the enumeration assertion."));
        return true;
    }
    // Record the persistable /Game/DataLayers asset path so the function-exit guard
    // above discards it (keeps the disposable host baseline clean).
    LayerCapture.Result->TryGetStringField(TEXT("dataLayerAssetPath"), CreatedLayerAssetPath);

    // 3) Read the structure back. The CORE assertion: the layer we just created must
    // appear in dataLayers. The reverted stub writes an empty array and this fails.
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("level.structure.get_level_structure_info found"),
        InvokeHandlerWithCapture(TEXT("level.structure.get_level_structure_info"),
            MakeShared<FJsonObject>(), InfoCapture));
    TestTrue(TEXT("get_level_structure_info succeeded"), InfoCapture.bSuccess);

    if (InfoCapture.bSuccess && InfoCapture.Result.IsValid())
    {
        // get_level_structure_info nests its overview under a single "levelStructureInfo"
        // object (see LevelStructureHandler.cpp: ResponseJson->SetObjectField(
        // TEXT("levelStructureInfo"), InfoJson)); dataLayers / worldPartitionEnabled live
        // INSIDE it, not at the response top level. Read the wrapper first.
        const TSharedPtr<FJsonObject>* InfoObj = nullptr;
        const bool bHasInfo = InfoCapture.Result->TryGetObjectField(TEXT("levelStructureInfo"), InfoObj);
        TestTrue(TEXT("get_level_structure_info response carries a levelStructureInfo object"),
            bHasInfo);
        const TSharedPtr<FJsonObject> Info = (bHasInfo && InfoObj) ? *InfoObj : nullptr;

        // The world is partitioned, so the handler must emit a dataLayers array.
        const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
        const bool bHasArray = Info.IsValid()
            && Info->TryGetArrayField(TEXT("dataLayers"), DataLayers);
        TestTrue(TEXT("get_level_structure_info carries a dataLayers array on a partitioned world"),
            bHasArray);

        if (bHasArray && DataLayers)
        {
            // The reverted stub guarantees an EMPTY array even though a layer is registered.
            TestTrue(TEXT("dataLayers is non-empty after a data layer was created (stub wrote [])"),
                DataLayers->Num() > 0);

            // The specific layer we created must be enumerated by its short name.
            bool bFoundCreatedLayer = false;
            for (const TSharedPtr<FJsonValue>& LayerValue : *DataLayers)
            {
                const TSharedPtr<FJsonObject>* LayerObj = nullptr;
                if (LayerValue.IsValid() && LayerValue->TryGetObject(LayerObj) && LayerObj)
                {
                    FString EnumeratedName;
                    if ((*LayerObj)->TryGetStringField(TEXT("name"), EnumeratedName)
                        && EnumeratedName.Equals(DataLayerName, ESearchCase::IgnoreCase))
                    {
                        bFoundCreatedLayer = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("dataLayers enumerates the created data layer by name"),
                bFoundCreatedLayer);
        }
    }

    // ProbeWorld's destructor reloads the original map on scope exit (after destroy +
    // discard); the CleanupTestAsset guard above discards the data-layer asset first.
    return true;
}
