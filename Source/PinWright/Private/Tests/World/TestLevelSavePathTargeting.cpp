// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the level-save path-targeting defects.
//
// B-level-save-package-path-as-filename: McpSafeLevelSave passed a LONG PACKAGE NAME
// (/Game/Maps/X) into FEditorFileUtils::SaveLevel's DefaultFilename parameter, which the
// engine forwards to SaveWorld as ForceFilename — a FILESYSTEM path. SaveWorld splits it
// into Path="/Game/Maps" + CleanFilename="X" (no extension) and, because
// FPackageName::TryConvertFilenameToLongPackageName passes an already-long-package-name
// straight through, never rejects it. Windows then resolves the leading '/' against the
// current drive, so the map bytes landed at <drive>:\Game\Maps\X — extensionless, outside
// the project — while McpSafeLevelSave's lenient ShouldTreatLevelSaveAsSuccess OR-policy
// still reported success off the now-clean package flag. level.create has the same shape
// (it hands the same package path to FEditorFileUtils::SaveMap, whose Filename parameter
// is likewise a filesystem path).
//
// B-level-save-verify-existence-not-freshness: the on-disk gate only asked "does a .umap
// exist here", so a stale .umap left by an earlier save satisfied it even when the current
// save wrote nothing (or wrote somewhere else entirely) — which is why the wrong-path write
// above stayed invisible.
//
// Post-fix contract asserted here:
//   * a level save writes its .umap under FPaths::ProjectContentDir(), at exactly the path
//     ResolveLevelPackageToMapFilename resolves for the package;
//   * nothing is ever created at the drive-relative interpretation of the package path;
//   * re-saving a dirty, already-saved map actually rewrites the file;
//   * level.create reports success only when a real .umap landed on disk.
//
// These drive the REAL handlers end-to-end (level.create for the create+save, level.save for
// the re-save), so a fix at either the handler layer or inside the shared McpSafeLevelSave
// helper satisfies them.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Uniquely-named namespace (not an anonymous one): TestLevelHandlers.cpp already defines an
// anonymous-namespace DiscardProbeMapPackage, and Unity merges both TUs into one anonymous
// namespace — an identically-named helper there would be a redefinition. Same reason the
// plugin consolidates shared test helpers into named namespaces (see PinWright CLAUDE.md).
namespace LevelSavePathTargetingTests
{
    // Unique GUID-suffixed probe package path, so no stale on-disk .umap from an earlier run
    // can shadow the probe and make an existence-only check pass for the wrong reason (that
    // stale-file blind spot is exactly B-level-save-verify-existence-not-freshness).
    inline FString MakeProbeLevelPath()
    {
        return FString::Printf(TEXT("/Game/Maps/__EARG_LevelSaveProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // The path the bug produced: the package path treated as a filesystem path. FPaths sees a
    // leading '/' as rooted, so ConvertRelativePathToFull returns it unchanged and the Windows
    // file API resolves it against the CURRENT DRIVE (<drive>:\Game\Maps\...) — the same
    // resolution SavePackage performed when it wrote there.
    inline FString DriveRelativeCandidate(const FString& LevelPath)
    {
        return FPaths::ConvertRelativePathToFull(LevelPath);
    }

    // Safely discards the probe map WITHOUT routing through CleanupTestAsset ->
    // UEditorAssetLibrary::DeleteAsset -> ObjectTools::ForceDeleteObjects. Force-delete's
    // GatherObjectReferencersForDeletion serializes the freshly-created, never-reloaded world
    // and broadcasts OnAssetsPendingDelete; for a brand-new map package that chain crashes the
    // editor under -unattended -RenderOffScreen (FUObjectArray::AllocateSerialNumber null-deref
    // via BlueprintActionDatabase::OnAssetsPendingDelete). Mirrors the discard protocol
    // TestLevelHandlers.cpp documents: detach from the roots, rename the package into the
    // transient package, GC, then drop the file and the registry entry.
    //
    // Additionally removes the drive-relative artifacts the pre-fix save path leaves at the
    // current drive's root, so a failing run does not litter the machine. Those names are
    // GUID-suffixed, so a delete here can only ever match this test's own probe.
    inline void DiscardLevelSaveProbeMap(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty())
        {
            return;
        }

        // Never detach/rename the package while its world is still the active editor world
        // (the map-guard restore may not have fired): renaming a live world's package to
        // transient corrupts the editor world context. In that case only the on-disk files are
        // dropped below; the in-memory world is reclaimed when the next map opens.
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
                    UPackage::StaticClass(), TEXT("EARG_DiscardedSaveProbeMap")).ToString(),
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

        const FString StrayBase = DriveRelativeCandidate(PackagePath);
        const TArray<FString> StrayPaths =
            { StrayBase, StrayBase + FPackageName::GetMapPackageExtension() };
        for (const FString& Stray : StrayPaths)
        {
            if (IFileManager::Get().FileExists(*Stray))
            {
                IFileManager::Get().Delete(*Stray, /*RequireExists*/false,
                    /*EvenReadOnly*/true, /*Quiet*/true);
            }
        }
    }

    // True when the active editor world is the probe map. Every step that mutates or re-saves
    // the ACTIVE world is gated on this: if level.create did not swap the world (no
    // LevelEditor subsystem, save guard, pre-existing package), spawning into it or invoking
    // level.save would touch the host project's real open map instead of the throwaway probe.
    inline bool IsProbeWorldActive(const FString& LevelPath)
    {
        UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return World && World->GetOutermost()
            && World->GetOutermost()->GetName().Equals(LevelPath, ESearchCase::IgnoreCase);
    }

    // Spawns a PERSISTENT (non-transient) static-mesh actor into the world's persistent level
    // and marks the level package dirty, so the next save has genuinely different bytes to
    // write. TestWorldUtils' SpawnTransientCubeActor deliberately sets RF_Transient, which
    // excludes the actor from the saved map — useless for proving a rewrite, hence this
    // variant. The engine cube is decorative: an actor entry alone changes the .umap.
    inline AActor* SpawnPersistentProbeActor(UWorld* World)
    {
        if (!World || !World->PersistentLevel)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.OverrideLevel = World->PersistentLevel;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator,
            SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
        {
            Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        }
        Actor->SetActorLabel(FString::Printf(TEXT("PW_SaveFreshnessProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        World->PersistentLevel->MarkPackageDirty();
        return Actor;
    }

    // Invokes level.save on the ACTIVE world and pumps the game thread until the job it starts
    // has actually performed the save. level.save schedules McpSafeLevelSave from StartJob's
    // bind through an always-deferred core-ticker safe-point continuation, so the handler call
    // returns a "running" ticket before the save body runs. Pumping the ticker here makes the
    // save deterministic on this stack (and stops a late continuation from saving after the
    // map-guard already restored the original map). Returns true once the ticket is terminal.
    inline bool DriveLevelSaveJobToCompletion(FAutomationTestBase& Test)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("level.save handler is registered"),
            InvokeHandlerWithCapture(TEXT("level.save"), MakeShared<FJsonObject>(), Capture));

        FString TicketId;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId)
            || TicketId.IsEmpty())
        {
            Test.AddInfo(TEXT("level.save returned no job ticket; the save body never started."));
            return false;
        }

        const double Deadline = FPlatformTime::Seconds() + 15.0;
        FJobTicket Ticket;
        while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
            && Ticket.Status == TEXT("running"))
        {
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddInfo(TEXT("level.save job did not reach a terminal status within 15s."));
                return false;
            }
            FTSTicker::GetCoreTicker().Tick(0.01f);
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            FPlatformProcess::Sleep(0.001f);
        }
        return true;
    }
}

// ============================================================================
// level.save — the .umap must land under the project content directory, and
// nothing may be created at the drive-relative reading of the package path.
// ============================================================================

// Regression for B-level-save-package-path-as-filename. Creates a throwaway probe map at a
// unique /Game/Maps path through the real level.create handler (which creates the world AND
// saves it), then re-saves the now-active probe world through the real level.save handler, and
// asserts the resulting on-disk state. Pre-fix, the package path travels into a filesystem
// filename parameter and the bytes land at <drive>:\Game\Maps\__EARG_LevelSaveProbe_<GUID>
// with no extension, so BOTH the "file exists at the resolved project-content path" and the
// "no package file written outside the project" assertions fail. Skipped gracefully with no
// GEditor (commandlet/unit context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveWritesToProjectContentNotDriveRootTest,
    "PinWright.level.save.WritesToProjectContentNotDriveRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveWritesToProjectContentNotDriveRootTest::RunTest(const FString& Parameters)
{
    using namespace LevelSavePathTargetingTests;

    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); skipping level-save path-targeting "
                "assertions."));
        return true;
    }

    const FString LevelPath = MakeProbeLevelPath();

    // Discard the probe (in-memory package + every on-disk artifact) on EVERY exit path via the
    // ForceDeleteObjects-free helper. Declared BEFORE MapGuard so LIFO destruction runs
    // MapGuard's original-map restore FIRST, leaving the probe package non-active when the
    // discard runs — reversed, the discard degrades to file-only cleanup and leaks the package.
    ON_SCOPE_EXIT { DiscardLevelSaveProbeMap(LevelPath); };

    // level.create calls GEditor->NewMap(), swapping the active editor world; restore the
    // originally-open map on scope exit so later tests in the run aren't polluted.
    FScopedEditorWorldMapGuard MapGuard;

    // On the pre-fix path level.save's honest re-gate logs an Error (no .umap landed), and a
    // save that cannot write at all makes McpSafeLevelSave log its all-attempts-failed Error.
    // UE's automation framework captures Error logs as failures, so declare both expected —
    // optional (negative occurrences), because post-fix the save succeeds and logs neither.
    // The explicit assertions below do the real verification.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.3: AddExpectedError rejects negative Occurrences (optional semantics and the IsRegex
    // parameter both arrived in 5.4), and Occurrences=0 would fail the clean post-fix branch.
    // Suppress log capture instead; the TestTrue/TestFalse assertions below bypass log capture.
    bSuppressLogs = true;
#else
    AddExpectedError(TEXT("level.save: save reported success but no .umap on disk"),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
    AddExpectedError(TEXT("McpSafeLevelSave: All "),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
#endif

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("levelPath"), LevelPath);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("level.create handler is registered"),
        InvokeHandlerWithCapture(TEXT("level.create"), CreatePayload, CreateCapture));
    if (!CreateCapture.bSuccess)
    {
        AddInfo(FString::Printf(TEXT("level.create reported failure (code='%s', message='%s'); the "
            "path-targeting assertions below still describe the required post-save state."),
            *CreateCapture.ErrorCode, *CreateCapture.Message));
    }

    // Re-save the probe world through the level.save verb itself, so the shared
    // McpSafeLevelSave path (the one named in the ticket) is exercised too. Gated on the probe
    // actually being the active world — otherwise level.save would save the host's real map.
    if (IsProbeWorldActive(LevelPath))
    {
        DriveLevelSaveJobToCompletion(*this);
    }
    else
    {
        AddInfo(TEXT("level.create did not leave the probe map as the active world; asserting "
            "against the create-time save only (level.save would target the host's open map)."));
    }

    // Mount-aware resolution of the package to its .umap filename. This is a pure path
    // transform and is expected to succeed both pre- and post-fix — it establishes WHERE the
    // file is contractually required to be.
    FString ResolvedMapFilename;
    const bool bResolved = ResolveLevelPackageToMapFilename(LevelPath, ResolvedMapFilename);
    TestTrue(TEXT("ResolveLevelPackageToMapFilename resolves the probe package"), bResolved);

    const FString ResolvedFull = FPaths::ConvertRelativePathToFull(ResolvedMapFilename);
    const FString ContentRootFull = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    TestTrue(*FString::Printf(
        TEXT("the resolved .umap path is inside the project content directory (resolved='%s', content='%s')"),
        *ResolvedFull, *ContentRootFull),
        !ResolvedFull.IsEmpty() && ResolvedFull.StartsWith(ContentRootFull));

    // The saved map must actually be at that resolved project-content path. Pre-fix nothing is
    // written there at all — the bytes went to the drive-relative path asserted against below.
    TestTrue(*FString::Printf(TEXT("the saved .umap exists at the resolved project-content path ('%s')"),
        *ResolvedFull),
        !ResolvedMapFilename.IsEmpty() && IFileManager::Get().FileExists(*ResolvedMapFilename));

    // Nothing may exist at the drive-relative reading of the package path. The pre-fix save
    // wrote an extensionless file there; the +.umap form is covered too in case a partial fix
    // only appends the extension without re-rooting the path.
    const FString DriveRelative = DriveRelativeCandidate(LevelPath);
    const FString DriveRelativeUmap = DriveRelative + FPackageName::GetMapPackageExtension();
    TestFalse(*FString::Printf(
        TEXT("no package file written outside the project — nothing at the drive-relative path '%s'"),
        *DriveRelative),
        IFileManager::Get().FileExists(*DriveRelative));
    TestFalse(*FString::Printf(
        TEXT("no package file written outside the project — nothing at the drive-relative path '%s'"),
        *DriveRelativeUmap),
        IFileManager::Get().FileExists(*DriveRelativeUmap));

    return true;
}

// ============================================================================
// level.save — a re-save of a dirty, already-saved map must rewrite the file.
// ============================================================================

// Regression for B-level-save-verify-existence-not-freshness: the save gate only asked
// "does a .umap exist at this path", which a stale file from an earlier save satisfies even
// when the current save wrote nothing — the blind spot that let the wrong-path write above go
// unnoticed. This proves the save is a real WRITE, not just a passing existence probe: save the
// probe map, snapshot the .umap's timestamp and size, add a persistent actor (so the correct
// bytes genuinely differ), re-save through level.save, and require the file to have changed.
// Pre-fix the very first assertion fails, because no .umap is written under the project at all.
// Skipped gracefully with no GEditor (commandlet/unit context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveRewritesExistingUmapTest,
    "PinWright.level.save.RewritesExistingUmap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveRewritesExistingUmapTest::RunTest(const FString& Parameters)
{
    using namespace LevelSavePathTargetingTests;

    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); skipping level-save freshness "
                "assertions."));
        return true;
    }

    const FString LevelPath = MakeProbeLevelPath();

    // Declared BEFORE MapGuard — see the ordering note on the sibling test above.
    ON_SCOPE_EXIT { DiscardLevelSaveProbeMap(LevelPath); };
    FScopedEditorWorldMapGuard MapGuard;

    // Same optional expected-error declarations as the sibling test: the pre-fix save path logs
    // Errors that the automation framework would otherwise turn into opaque failures.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    bSuppressLogs = true;
#else
    AddExpectedError(TEXT("level.save: save reported success but no .umap on disk"),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
    AddExpectedError(TEXT("McpSafeLevelSave: All "),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
#endif

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("levelPath"), LevelPath);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("level.create handler is registered"),
        InvokeHandlerWithCapture(TEXT("level.create"), CreatePayload, CreateCapture));

    FString MapFilename;
    const bool bResolved = ResolveLevelPackageToMapFilename(LevelPath, MapFilename);
    TestTrue(TEXT("ResolveLevelPackageToMapFilename resolves the probe package"), bResolved);

    // Precondition for a freshness comparison: the first save must have produced a real .umap.
    // Pre-fix this is where the test stops — the file only exists at the drive-relative path.
    const bool bFirstSaveOnDisk =
        bResolved && !MapFilename.IsEmpty() && IFileManager::Get().FileExists(*MapFilename);
    TestTrue(*FString::Printf(TEXT("the probe .umap is on disk after the first save ('%s')"),
        *MapFilename), bFirstSaveOnDisk);
    if (!bFirstSaveOnDisk)
    {
        return true;
    }

    // Every step below mutates and re-saves the ACTIVE world, so it must be the probe map.
    if (!IsProbeWorldActive(LevelPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("handler-call-failed"),
            TEXT("level.create did not leave the probe map as the active world; skipping the "
                "re-save comparison rather than dirtying and saving the host's open map."));
        return true;
    }
    UWorld* const ProbeWorld = GEditor->GetEditorWorldContext().World();

    const FDateTime FirstTimeStamp = IFileManager::Get().GetTimeStamp(*MapFilename);
    const int64 FirstSize = IFileManager::Get().FileSize(*MapFilename);

    // Dirty the world with content that is actually serialized into the map, so a genuine
    // rewrite changes the file's SIZE and does not depend on filesystem timestamp granularity.
    TestNotNull(TEXT("spawned a persistent actor to dirty the probe map"),
        SpawnPersistentProbeActor(ProbeWorld));

    TestTrue(TEXT("level.save job reached a terminal status"), DriveLevelSaveJobToCompletion(*this));

    const bool bStillOnDisk = IFileManager::Get().FileExists(*MapFilename);
    TestTrue(TEXT("the probe .umap is still on disk after the re-save"), bStillOnDisk);
    if (!bStillOnDisk)
    {
        return true;
    }

    const FDateTime SecondTimeStamp = IFileManager::Get().GetTimeStamp(*MapFilename);
    const int64 SecondSize = IFileManager::Get().FileSize(*MapFilename);

    // The load-bearing freshness assertion: an existence-only gate is satisfied by the stale
    // file from the first save, so only an observable change proves the second save wrote.
    TestTrue(*FString::Printf(
        TEXT("re-saving a dirty map rewrites the .umap (size %lld -> %lld, timestamp %s -> %s)"),
        FirstSize, SecondSize, *FirstTimeStamp.ToIso8601(), *SecondTimeStamp.ToIso8601()),
        SecondSize != FirstSize || SecondTimeStamp != FirstTimeStamp);

    // Companion signal: a save that really landed leaves the level package clean.
    if (UPackage* LevelPackage = ProbeWorld->PersistentLevel
        ? ProbeWorld->PersistentLevel->GetOutermost() : nullptr)
    {
        TestFalse(TEXT("the probe level package is clean after the re-save"),
            LevelPackage->IsDirty());
    }

    return true;
}

// ============================================================================
// level.create — reported success must agree with a .umap on disk.
// ============================================================================

// Regression for the level.create half of B-level-save-package-path-as-filename /
// B-level-save-verify-existence-not-freshness: level.create hands its package path to
// FEditorFileUtils::SaveMap, whose Filename parameter is a filesystem path, and reports success
// straight off that call's boolean. Pre-fix the bytes go to the drive-relative path, no .umap
// lands under the project, and the handler still answers success:true — a false success.
//
// Asserts the persistence-honesty INVARIANT (the same agreement shape
// lighting.create_lighting_enabled_level's regression test uses): reported success MUST equal
// .umap-on-disk reality. The invariant is robust to the environment — if the save genuinely
// lands the file, success:true is consistent and the test still passes. Skipped gracefully with
// no GEditor (commandlet/unit context).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelCreateVerifiesUmapOnDiskTest,
    "PinWright.level.create.VerifiesUmapOnDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelCreateVerifiesUmapOnDiskTest::RunTest(const FString& Parameters)
{
    using namespace LevelSavePathTargetingTests;

    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor (commandlet/unit context); skipping level.create "
                "persistence-honesty assertion."));
        return true;
    }

    const FString LevelPath = MakeProbeLevelPath();

    // Declared BEFORE MapGuard — see the ordering note on the first test in this file.
    ON_SCOPE_EXIT { DiscardLevelSaveProbeMap(LevelPath); };
    FScopedEditorWorldMapGuard MapGuard;

    // A save that cannot write at all makes McpSafeLevelSave log its all-attempts-failed Error,
    // which the automation framework would capture as an opaque failure. Optional occurrences:
    // the success branch logs nothing. Deliberately NOT declaring a broad "level.create" matcher
    // — that would swallow genuine unrelated Errors; the invariant assertion below is the check.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    bSuppressLogs = true;
#else
    AddExpectedError(TEXT("McpSafeLevelSave: All "),
        EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex=*/false);
#endif

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("levelPath"), LevelPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("level.create handler is registered"),
        InvokeHandlerWithCapture(TEXT("level.create"), Payload, Capture));
    TestTrue(TEXT("level.create responded"), Capture.bWasCalled);

    // Ground truth: did a .umap actually land on disk? (mount-aware resolve + file probe — the
    // same signal the shared VerifyLevelSavedToDisk re-gate uses.)
    FString MapFilename;
    const bool bResolved = ResolveLevelPackageToMapFilename(LevelPath, MapFilename);
    const bool bFileOnDisk =
        bResolved && !MapFilename.IsEmpty() && IFileManager::Get().FileExists(*MapFilename);

    // The persistence-honesty invariant. Pre-fix the handler reports success:true with no file
    // under the project, violating it — the exact false success this test guards.
    TestEqual(TEXT("reported success must agree with .umap-on-disk reality"),
        Capture.bSuccess, bFileOnDisk);

    if (Capture.bSuccess)
    {
        // A reported success must also be a success at the RIGHT place: inside the project's
        // content directory, at the path the package resolves to.
        const FString ResolvedFull = FPaths::ConvertRelativePathToFull(MapFilename);
        const FString ContentRootFull = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
        TestTrue(*FString::Printf(
            TEXT("a reported-success create resolves its .umap inside the project content directory (resolved='%s', content='%s')"),
            *ResolvedFull, *ContentRootFull),
            bResolved && !ResolvedFull.IsEmpty() && ResolvedFull.StartsWith(ContentRootFull));
        TestTrue(*FString::Printf(
            TEXT("a reported-success create leaves a real .umap on disk at '%s'"), *ResolvedFull),
            bFileOnDisk);
    }

    return true;
}
