// Copyright (c) 2026 Alexander Penkin. MIT License.

// End-to-end guard for the actor.duplicate dynamic-mesh integrity check (B2).
//
// UDynamicMesh::ImportCustomProperties — the T3D-paste half of
// UEditorActorSubsystem::DuplicateActor — emits a 12-triangle 50-unit box when it cannot
// recover the source mesh, and returns SUCCESS
// (Engine/Source/Runtime/GeometryFramework/Private/UDynamicMesh.cpp:568 on 5.8, :529 on
// 5.3). Two cvars govern the two triggers, both declared at UDynamicMesh.cpp:31-39:
// geometry.DynamicMesh.TextBasedDupeTriThreshold (default 200000) gates whether the
// Base64 text copy is written at all, and geometry.DynamicMesh.DupeStashTimeout (default
// 300 s) governs the FDynamicMeshCopyHelper pointer stash the fast path uses. This test
// drives BOTH to 0 to make the failure reachable on a small mesh.
//
// The assertion is deliberately a DISJUNCTION — "success with matching triangle counts,
// OR error MESH_DUPLICATE_SUBSTITUTED". Whether the copy stash still serves the mesh under
// a zero timeout is engine-internal and version-sensitive, so pinning one branch would
// flake across 5.3-5.8. The invariant this patch actually guarantees is that a SILENT
// success with mismatched counts can never happen again, and that is what is asserted.
//
// This file lives in the geometry sub-module because only that tree links
// GeometryFramework and can spawn a real dynamic mesh actor; the pure classifier and the
// probe's fail-safe paths are covered in the main module at
// Source/PinWright/Private/Tests/World/TestActorDuplicateMeshIntegrity.cpp.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "LevelUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Removes the probe actor and every duplicate UE auto-suffixed from its label
    // (<Label>2, <Label>3, ...) so the mutated editor world is left clean.
    void DestroyDuplicateMeshProbeActors(const FString& LabelPrefix)
    {
        if (!GEditor)
        {
            return;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!IsValid(World))
        {
            return;
        }
        TArray<AActor*> ToDestroy;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().StartsWith(LabelPrefix))
            {
                ToDestroy.Add(*It);
            }
        }
        for (AActor* Actor : ToDestroy)
        {
            World->DestroyActor(Actor);
        }
    }

    // Sets an int cvar and hands back its previous value, or MIN_int32 when the cvar does
    // not exist in this host (nothing to restore).
    int32 DupMeshSetIntCVarReturningPrevious(const TCHAR* Name, int32 NewValue)
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
        if (!CVar)
        {
            return MIN_int32;
        }
        const int32 Previous = CVar->GetInt();
        CVar->Set(NewValue);
        return Previous;
    }

    void DupMeshRestoreIntCVar(const TCHAR* Name, int32 Value)
    {
        if (Value == MIN_int32)
        {
            return;
        }
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            CVar->Set(Value);
        }
    }

    const TCHAR* const GDupMeshTriThresholdCVarName = TEXT("geometry.DynamicMesh.TextBasedDupeTriThreshold");
    const TCHAR* const GDupMeshStashTimeoutCVarName = TEXT("geometry.DynamicMesh.DupeStashTimeout");
}

// actor.duplicate of a dynamic mesh actor never returns a silent success whose copy holds
// different geometry than the source, even with both engine fallbacks disarmed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateNeverSilentlySubstitutesMeshTest,
    "PinWright.actor.duplicate.NeverSilentlySubstitutesDynamicMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateNeverSilentlySubstitutesMeshTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping duplicate mesh integrity test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_DupMeshIntegrity_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { DestroyDuplicateMeshProbeActors(Label); };

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-dup-mesh-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            return true;
        }
    }

    // FIXTURE PRECONDITION — unlock the source level. The engine's
    // DuplicateActorsToLevel early-returns with ZERO new actors when the source actor's
    // level is locked, and actor.duplicate correctly reports that up front as
    // LEVEL_LOCKED (LifecycleHandler.cpp:130-141). That is a THIRD outcome which never
    // reaches the substitution path this test exists to pin, so the fixture has to clear
    // it rather than the test anticipate it. In the headless automation editor the level
    // can be locked either because its package is read-only on disk (gated by
    // GEngine->bLockReadOnlyLevels) or because ULevel::bLocked is set, so clear BOTH and
    // restore them on every exit path. Spawning never consults IsLevelLocked, which is
    // why geometry.create_box above succeeds on a locked level.
    // Same treatment as TestGeometryDuplicateAlongSplineScaleVariation.cpp, which reaches
    // the same engine call.
    AActor* Probe = GeometryTestHelpers::FindActorByLabel(Label);
    if (!TestNotNull(TEXT("the probe DynamicMeshActor is findable by label"), Probe))
    {
        return true;
    }
    ULevel* ProbeLevel = Probe->GetLevel();
    const bool bPrevLockReadOnlyLevels = GEngine ? (bool)GEngine->bLockReadOnlyLevels : false;
    if (GEngine)
    {
        GEngine->bLockReadOnlyLevels = false;
    }
    const bool bLevelWasLocked = ProbeLevel != nullptr && FLevelUtils::IsLevelLocked(ProbeLevel);
    if (bLevelWasLocked)
    {
        FLevelUtils::ToggleLevelLock(ProbeLevel);
    }
    ON_SCOPE_EXIT
    {
        // Restore bLocked BEFORE the read-only flag: ToggleLevelLock is a pure flip, so
        // the IsLevelLocked read that guards it must still see bLockReadOnlyLevels false.
        if (bLevelWasLocked && ProbeLevel && !FLevelUtils::IsLevelLocked(ProbeLevel))
        {
            FLevelUtils::ToggleLevelLock(ProbeLevel);
        }
        if (GEngine)
        {
            GEngine->bLockReadOnlyLevels = bPrevLockReadOnlyLevels;
        }
    };
    if (ProbeLevel == nullptr || FLevelUtils::IsLevelLocked(ProbeLevel))
    {
        // LOUD SELF-DISABLE, never a silent pass. Everything below — the disjunction
        // "success with matching triangle counts OR MESH_DUPLICATE_SUBSTITUTED", the
        // allowPlaceholderMesh escape hatch, and the allowSlowLargeMeshCopy cvar
        // raise/restore — needs a duplicate to actually happen. On a source level this
        // fixture cannot unlock, actor.duplicate refuses with LEVEL_LOCKED before any of
        // that runs, so none of it can be asserted here.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("source-level-locked"),
            TEXT("Source level is still locked after clearing "
                 "GEngine->bLockReadOnlyLevels and ULevel::bLocked; "
                 "actor.duplicate returns LEVEL_LOCKED before the dynamic-mesh "
                 "substitution check runs, so the mesh-integrity invariant cannot "
                 "be exercised on this host."));
        return true;
    }

    // Disarm both engine fallbacks for the duration of the duplicate. Zero on the
    // threshold means "never write the Base64 text copy"; zero on the stash timeout
    // expires the FDynamicMeshCopyHelper reference immediately.
    const int32 PrevThreshold = DupMeshSetIntCVarReturningPrevious(GDupMeshTriThresholdCVarName, 0);
    const int32 PrevStashTimeout = DupMeshSetIntCVarReturningPrevious(GDupMeshStashTimeoutCVarName, 0);
    ON_SCOPE_EXIT
    {
        DupMeshRestoreIntCVar(GDupMeshTriThresholdCVarName, PrevThreshold);
        DupMeshRestoreIntCVar(GDupMeshStashTimeoutCVarName, PrevStashTimeout);
    };
    if (!TestTrue(TEXT("engine exposes the dynamic mesh dupe cvars"),
            PrevThreshold != MIN_int32 && PrevStashTimeout != MIN_int32))
    {
        return true;
    }

    TSharedPtr<FJsonObject> DupParams = MakeShared<FJsonObject>();
    DupParams->SetStringField(TEXT("actorName"), Label);
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("actor.duplicate"),
        TEXT("req-dup-mesh-duplicate"), DupParams, bSuccess, Result, ErrorCode);

    if (bSuccess)
    {
        // Branch A: the engine served the mesh anyway. The response must PROVE it by
        // carrying both counts, and they must agree — this is the assertion that fails if
        // the integrity check is ever removed and a placeholder slips through.
        if (!TestTrue(TEXT("success carries a result object"), Result.IsValid()))
        {
            return true;
        }
        double SourceTriangles = 0.0;
        double DuplicateTriangles = 0.0;
        const bool bHasSource =
            Result->TryGetNumberField(TEXT("sourceTriangles"), SourceTriangles);
        const bool bHasDuplicate =
            Result->TryGetNumberField(TEXT("duplicateTriangles"), DuplicateTriangles);
        TestTrue(TEXT("duplicating a dynamic mesh actor echoes sourceTriangles"), bHasSource);
        TestTrue(TEXT("duplicating a dynamic mesh actor echoes duplicateTriangles"), bHasDuplicate);
        TestTrue(TEXT("a successful duplicate never carries mismatched triangle counts"),
            bHasSource && bHasDuplicate && SourceTriangles == DuplicateTriangles);
        TestFalse(TEXT("a default-flagged success is never a kept placeholder"),
            Result->HasField(TEXT("meshPlaceholderSubstituted")));
        return true;
    }

    // Defensive third branch for a lock the fixture above could not have cleared (one
    // applied by a concurrently-mutating host between the check and this call). The
    // duplicate never ran, so refusing here says nothing about mesh substitution: warn
    // loudly instead of failing on an unrelated cause or, worse, passing vacuously.
    if (ErrorCode == TEXT("LEVEL_LOCKED"))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("source-level-locked"),
            TEXT("actor.duplicate refused with LEVEL_LOCKED even though the fixture "
                 "unlocked the source level; the dynamic-mesh substitution path was "
                 "not exercised."));
        return true;
    }

    // Branch B: the engine substituted, and the handler refused the copy.
    TestEqual(TEXT("a refused duplicate uses the typed substitution error code"),
        ErrorCode, FString(ErrorCodes::ERR_MESH_DUPLICATE_SUBSTITUTED));
    if (ErrorCode != ErrorCodes::ERR_MESH_DUPLICATE_SUBSTITUTED)
    {
        return true;
    }
    if (TestTrue(TEXT("the error carries diagnostic data"), Result.IsValid()))
    {
        double SourceTriangles = 0.0;
        double DuplicateTriangles = 0.0;
        TestTrue(TEXT("error data carries sourceTriangles"),
            Result->TryGetNumberField(TEXT("sourceTriangles"), SourceTriangles));
        TestTrue(TEXT("error data carries duplicateTriangles"),
            Result->TryGetNumberField(TEXT("duplicateTriangles"), DuplicateTriangles));
        TestTrue(TEXT("error data names the source actor"),
            Result->HasField(TEXT("actorName")));
        TestTrue(TEXT("error data carries a remedy"), Result->HasField(TEXT("remedy")));
        TestTrue(TEXT("the counts that triggered the refusal actually differ"),
            SourceTriangles != DuplicateTriangles);
    }

    // The opt-in escape hatch: allowPlaceholderMesh:true must return the cube instead of
    // failing, flagged and warned about rather than silent.
    {
        TSharedPtr<FJsonObject> AllowParams = MakeShared<FJsonObject>();
        AllowParams->SetStringField(TEXT("actorName"), Label);
        AllowParams->SetBoolField(TEXT("allowPlaceholderMesh"), true);
        bool bAllowSuccess = false;
        FString AllowError;
        TSharedPtr<FJsonObject> AllowResult;
        Dispatch(Dispatcher, Sink, TEXT("actor.duplicate"),
            TEXT("req-dup-mesh-allow-placeholder"), AllowParams,
            bAllowSuccess, AllowResult, AllowError);

        if (TestTrue(TEXT("allowPlaceholderMesh:true keeps the substituted copy"), bAllowSuccess) &&
            TestTrue(TEXT("the kept copy carries a result object"), AllowResult.IsValid()))
        {
            bool bFlag = false;
            TestTrue(TEXT("the kept copy is flagged meshPlaceholderSubstituted"),
                AllowResult->TryGetBoolField(TEXT("meshPlaceholderSubstituted"), bFlag) && bFlag);
            const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
            TestTrue(TEXT("the kept copy carries a warnings entry"),
                AllowResult->TryGetArrayField(TEXT("warnings"), Warnings) &&
                    Warnings != nullptr && Warnings->Num() > 0);
        }
    }

    // The other opt-in: allowSlowLargeMeshCopy:true raises the threshold cvar for exactly
    // one call, so the duplicate succeeds with matching counts AND the forced 0 is back in
    // place afterwards (the scope guard's restore is the risky half of that feature).
    {
        TSharedPtr<FJsonObject> SlowParams = MakeShared<FJsonObject>();
        SlowParams->SetStringField(TEXT("actorName"), Label);
        SlowParams->SetBoolField(TEXT("allowSlowLargeMeshCopy"), true);
        bool bSlowSuccess = false;
        FString SlowError;
        TSharedPtr<FJsonObject> SlowResult;
        Dispatch(Dispatcher, Sink, TEXT("actor.duplicate"),
            TEXT("req-dup-mesh-allow-slow-copy"), SlowParams,
            bSlowSuccess, SlowResult, SlowError);

        if (TestTrue(TEXT("allowSlowLargeMeshCopy:true recovers the real mesh"), bSlowSuccess) &&
            TestTrue(TEXT("the recovered copy carries a result object"), SlowResult.IsValid()))
        {
            bool bApplied = false;
            TestTrue(TEXT("the response reports the cvar raise was applied"),
                SlowResult->TryGetBoolField(TEXT("slowLargeMeshCopyApplied"), bApplied) && bApplied);
            double SourceTriangles = 0.0;
            double DuplicateTriangles = 0.0;
            TestTrue(TEXT("the recovered copy matches the source triangle count"),
                SlowResult->TryGetNumberField(TEXT("sourceTriangles"), SourceTriangles) &&
                    SlowResult->TryGetNumberField(TEXT("duplicateTriangles"), DuplicateTriangles) &&
                    SourceTriangles == DuplicateTriangles);
        }

        IConsoleVariable* ThresholdCVar =
            IConsoleManager::Get().FindConsoleVariable(GDupMeshTriThresholdCVarName);
        if (TestNotNull(TEXT("threshold cvar still resolvable after the raise"), ThresholdCVar))
        {
            TestEqual(TEXT("the scope guard restored the threshold cvar"),
                ThresholdCVar->GetInt(), 0);
        }
    }

    return true;
}
