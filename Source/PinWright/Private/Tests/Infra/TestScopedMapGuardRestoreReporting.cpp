// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-map-guard-drops-restore-result: FScopedEditorWorldMapGuard restored
// the editor map through level.load and discarded the result, so a refused restore left the run
// on the wrong world, the guard's own test passed, and the red landed on unrelated tests later.
//
// 1. A refused restore must red the test that owns the guard. The refusal is forced with the
//    same fixture TestMapSwapWorldSurvivorProbe.cpp uses: a contextless Editor-typed world held
//    past a collect, which the shared survivor probe refuses on both restore branches (level.load
//    when the original map is on disk, the blank-world fallback when it is untitled). The guard's
//    error is declared expected, so the test FAILS if the guard stays silent.
// 2. A restore that level.load defers to a safe point must still be complete when the guard's
//    destructor returns. Pre-fix the destructor returned with the swap parked on the core ticker.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Dispatch/SafePoint.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Templates/UniquePtr.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace TestScopedMapGuardRestoreHelpers
{
    struct FScopedForcedUnsafe
    {
        FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(true); }
        ~FScopedForcedUnsafe() { PinWrightSafePoint::SetForcedUnsafeForTests(false); }
    };

    inline FString ActiveMapPath()
    {
        const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        return (World && World->GetOutermost()) ? World->GetOutermost()->GetName() : FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedMapGuardReportsRefusedRestoreTest,
    "PinWright.infra.map_guard.RefusedRestoreRedsOwningTest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScopedMapGuardReportsRefusedRestoreTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the map-guard restore-refusal assertions."));
        return true;
    }

    // THE COUNTERFACTUAL. Exactly one: a guard that swallows the refusal fails this test at the
    // end of RunTest, and so does a second failure (OuterGuard's own restore) instead of being
    // absorbed by the expectation.
    AddExpectedError(TEXT("FScopedEditorWorldMapGuard failed to restore"),
        EAutomationExpectedErrorFlags::Contains, 1);

    // Puts the editor back once the fixture is gone, so this test does not strand the run it
    // is testing for. Declared first, so it destructs last.
    FScopedEditorWorldMapGuard OuterGuard(*this);
    {
        TUniquePtr<FScopedEditorWorldMapGuard> InnerGuard =
            MakeUnique<FScopedEditorWorldMapGuard>(*this);

        // A real swap, before the dead world exists, so the swap itself is not refused.
        if (!TestNotNull(TEXT("NewMap produced a world to swap to"),
                GEditor->NewMap(/*bIsPartitionedWorld=*/false)))
        {
            return true;
        }

        TStrongObjectPtr<UWorld> Dead(NewObject<UWorld>(GetTransientPackage(),
            FName(*FString::Printf(TEXT("PwMapGuardRefusal_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)))));
        if (!TestNotNull(TEXT("created the blocking dead world"), Dead.Get()))
        {
            return true;
        }
        Dead->WorldType = EWorldType::Editor;
        // Inactive is a type the engine's leak check keeps, so the fixture is harmless for as
        // long as deferred reclamation leaves it resident (see TestMapSwapWorldSurvivorProbe).
        ON_SCOPE_EXIT
        {
            if (UWorld* DeadWorld = Dead.Get())
            {
                DeadWorld->WorldType = EWorldType::Inactive;
            }
            Dead.Reset();
        };

        const FString SwappedInMap = TestScopedMapGuardRestoreHelpers::ActiveMapPath();
        // Runs the restore with the dead world still held: must be refused and reported.
        InnerGuard.Reset();

        TestEqual(TEXT("the refused restore really did not swap the world"),
            TestScopedMapGuardRestoreHelpers::ActiveMapPath(), SwappedInMap);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedMapGuardPumpsDeferredRestoreTest,
    "PinWright.infra.map_guard.DeferredRestoreCompletesBeforeGuardReturns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScopedMapGuardPumpsDeferredRestoreTest::RunTest(const FString& Parameters)
{
    using namespace TestScopedMapGuardRestoreHelpers;

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the map-guard deferred-restore assertions."));
        return true;
    }

    // Only the level.load branch can defer, so the guard must start on a map with a .umap.
    const FString StartMap = ActiveMapPath();
    const FString TargetMap = FPackageName::DoesPackageExist(StartMap)
        ? StartMap
        : FindAlternateOnDiskMap(StartMap);
    if (TargetMap.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("FIXTURE-SKIP: this host has no World asset with a .umap on disk; skipping."));
        return true;
    }

    FScopedEditorWorldMapGuard OuterGuard(*this);

    if (TargetMap != StartMap)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("levelPath"), TargetMap);
        FTestResponseCapture LoadCapture;
        InvokeHandlerWithCapture(TEXT("level.load"), Payload, LoadCapture);
        if (!TestTrue(FString::Printf(TEXT("fixture: level.load '%s' succeeded (%s %s)"),
                *TargetMap, *LoadCapture.ErrorCode, *LoadCapture.Message), LoadCapture.bSuccess))
        {
            return true;
        }
    }
    if (!TestEqual(TEXT("fixture: the guard starts on the on-disk map"), ActiveMapPath(), TargetMap))
    {
        return true;
    }

    TUniquePtr<FScopedEditorWorldMapGuard> InnerGuard = MakeUnique<FScopedEditorWorldMapGuard>(*this);
    if (!TestNotNull(TEXT("NewMap produced a world to swap to"),
            GEditor->NewMap(/*bIsPartitionedWorld=*/false)))
    {
        return true;
    }
    TestNotEqual(TEXT("fixture: the world really swapped"), ActiveMapPath(), TargetMap);

    {
        // Forces level.load's RunAtSafePoint onto its deferred branch for the restore.
        FScopedForcedUnsafe ForcedUnsafe;
        InnerGuard.Reset();
    }

    // THE COUNTERFACTUAL. Pre-fix the swap was still parked on the core ticker here.
    TestEqual(TEXT("the deferred restore is complete when the guard's destructor returns"),
        ActiveMapPath(), TargetMap);
    return true;
}
