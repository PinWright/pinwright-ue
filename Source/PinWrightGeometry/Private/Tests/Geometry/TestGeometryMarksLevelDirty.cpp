// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the geometry.* data-loss bug: mutating verbs edited a level
// ADynamicMeshActor's UDynamicMesh and called NotifyMeshUpdated(), but marked nothing
// dirty — so `level.save` no-opped and the edit was silently lost on editor close.
//
// Strategy (end-to-end through the real dispatcher, no external content asset):
//   1. geometry.create_box spawns a real DynamicMeshActor -> assert the level package
//      is dirty (SpawnActor alone does NOT dirty: it only calls ModifyLevel under
//      GUndo, LevelActor.cpp:735-739, and this namespace opens no transaction).
//   2. Clear the dirty flag, run a read-only verb (geometry.get_mesh_info) -> assert
//      the package is STILL CLEAN. Guards the inverse defect: a helper wired into the
//      target-resolution path instead of the commit path would dirty on every query.
//   3. Clear again, run a mutating verb (geometry.translate_mesh) -> assert dirty.
//   4. Clear again, run geometry.boolean_subtract -> assert dirty. The four boolean
//      verbs had NO commit step at all (not even NotifyMeshUpdated), so they are the
//      most likely site to be missed by a NotifyMeshUpdated-seam-only fix.
//
// Counterfactual: reverting GeometryUtils::MarkGeometryActorModified /
// MarkGeometryActorSpawned to a bare NotifyMeshUpdated() makes steps 1, 3 and 4 fail
// while every verb still reports success — which is exactly how the bug hid.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "UObject/Package.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // The package a level ADynamicMeshActor's edits must land in. On a classic map this
    // is the map package; the helpers dirty the actor's outermost package, which
    // resolves to the same UPackage here.
    UPackage* GeometryDirtyProbeLevelPackage()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        ULevel* Level = World ? World->PersistentLevel : nullptr;
        return Level ? Level->GetOutermost() : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMarksLevelDirtyTest,
    "PinWright.geometry.Persistence.MutatingVerbsMarkLevelDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMarksLevelDirtyTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry dirty-flag test"));
        return true;
    }

    UPackage* LevelPackage = GeometryDirtyProbeLevelPackage();
    if (!TestNotNull(TEXT("Editor level package resolved"), LevelPackage))
    {
        return true;
    }

    // Restore whatever the level's dirty state was on the way in — this test
    // deliberately toggles it, and must not leave a clean map looking modified.
    const bool bWasDirtyOnEntry = LevelPackage->IsDirty();
    ON_SCOPE_EXIT { LevelPackage->SetDirtyFlag(bWasDirtyOnEntry); };

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_DirtyProbe_%s"), *Suffix);
    const FString ToolLabel = FString::Printf(TEXT("PW_DirtyProbeTool_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    ON_SCOPE_EXIT
    {
        GeometryTestHelpers::DestroyActorsWithLabel(Label);
        GeometryTestHelpers::DestroyActorsWithLabel(ToolLabel);
    };

    auto RunVerb = [&Dispatcher, &Sink](const FString& Method, const FString& RequestId,
                                        const TSharedPtr<FJsonObject>& Params) -> bool
    {
        bool bSuccess = false;
        FString Err;
        Dispatch(Dispatcher, Sink, Method, RequestId, Params, bSuccess, Err);
        return bSuccess;
    };

    // ---- 1. SPAWN dirties -------------------------------------------------
    LevelPackage->SetDirtyFlag(false);
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Label);
        if (!TestTrue(TEXT("geometry.create_box succeeded"),
                      RunVerb(TEXT("geometry.create_box"), TEXT("req-dirty-create"), P)))
        {
            return true;
        }
    }
    TestTrue(TEXT("geometry.create_box marks the level package dirty"), LevelPackage->IsDirty());

    // ---- 2. A read-only verb must NOT dirty -------------------------------
    LevelPackage->SetDirtyFlag(false);
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), Label);
        TestTrue(TEXT("geometry.get_mesh_info succeeded"),
                 RunVerb(TEXT("geometry.get_mesh_info"), TEXT("req-dirty-info"), P));
    }
    TestFalse(TEXT("geometry.get_mesh_info leaves the level package clean"), LevelPackage->IsDirty());

    // ---- 3. A representative mutating verb dirties ------------------------
    LevelPackage->SetDirtyFlag(false);
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), Label);
        TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
        Offset->SetNumberField(TEXT("x"), 25.0);
        Offset->SetNumberField(TEXT("y"), 0.0);
        Offset->SetNumberField(TEXT("z"), 0.0);
        P->SetObjectField(TEXT("translation"), Offset);
        if (!TestTrue(TEXT("geometry.translate_mesh succeeded"),
                      RunVerb(TEXT("geometry.translate_mesh"), TEXT("req-dirty-translate"), P)))
        {
            return true;
        }
    }
    TestTrue(TEXT("geometry.translate_mesh marks the level package dirty"), LevelPackage->IsDirty());

    // ---- 4. The boolean family (previously NO commit at all) dirties ------
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), ToolLabel);
        if (!TestTrue(TEXT("tool box spawned for boolean probe"),
                      RunVerb(TEXT("geometry.create_box"), TEXT("req-dirty-tool"), P)))
        {
            return true;
        }
    }
    LevelPackage->SetDirtyFlag(false);
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("targetActor"), Label);
        P->SetStringField(TEXT("toolActor"), ToolLabel);
        P->SetBoolField(TEXT("keepTool"), true);   // isolate the target's own dirty mark
                                                   // from DestroyActor's built-in one
        if (!TestTrue(TEXT("geometry.boolean_subtract succeeded"),
                      RunVerb(TEXT("geometry.boolean_subtract"), TEXT("req-dirty-bool"), P)))
        {
            return true;
        }
    }
    TestTrue(TEXT("geometry.boolean_subtract marks the level package dirty"), LevelPackage->IsDirty());

    return true;
}
