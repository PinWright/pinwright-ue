// Copyright (c) 2026 Alexander Penkin. MIT License.

// Guards the failure-path rollback against re-introducing the
// MarkBlueprintAsStructurallyModified cascade that fires the
// "ensure(SkeletonCompiledBlueprints.Num()==1)" in BlueprintCompilationManager.cpp.
//
// The original repro was a malformed compile_bpir call that timed out while the
// editor showed the ensure dialog; that dialog blocks the game thread, freezing
// FHttpListener::TickConnections and making subsequent RPC calls pile up in
// CLOSE_WAIT until port 19880 stops accepting connections.
//
// The cascade goes: RollbackLastTransaction → UTransBuffer::Undo →
// BroadcastPostUndoRedo → FBlueprintEditorModule::FixSubObjectReferencesPostUndoRedo →
// FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified → synchronous
// FBlueprintCompilationManager::CompileSynchronously(..., RegenerateSkeletonOnly).
//
// This test instruments an FEditorUndoClient and asserts the failed-compile
// rollback does NOT call PostUndo (the entry point for every step of the
// problematic cascade), then runs a second malformed compile and a follow-up
// handler call to confirm the dispatcher remains responsive — the hung-RPC
// symptom from #5.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorUndoClient.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"


namespace
{
    // Counts PostUndo / PostRedo callbacks fired by the editor undo subsystem.
    // The bug-causing rollback path runs through GEditor->UndoTransaction, which
    // broadcasts PostUndo to every registered client; the fixed path uses
    // FScopedTransaction::Cancel after FTransaction::Apply, which does not.
    struct FUndoCallbackCounter : public FEditorUndoClient
    {
        int32 PostUndoCount = 0;
        int32 PostRedoCount = 0;

        virtual void PostUndo(bool /*bSuccess*/) override { ++PostUndoCount; }
        virtual void PostRedo(bool /*bSuccess*/) override { ++PostRedoCount; }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompileBpirRollbackNoStructuralCompileTest,
    "PinWright.blueprint.compile_bpir.RollbackNoStructuralCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompileBpirRollbackNoStructuralCompileTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("GEditor available"), (UObject*)GEditor))
    {
        return true;
    }

    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirRollbackNoStructuralCompile_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    BP->SetFlags(RF_Transactional);

    FUndoCallbackCounter UndoCounter;
    GEditor->RegisterForUndo(&UndoCounter);

    ON_SCOPE_EXIT
    {
        GEditor->UnregisterForUndo(&UndoCounter);
        if (UPackage* BlueprintPackage = BP->GetOutermost())
        {
            BlueprintPackage->SetDirtyFlag(false);
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };

    // Malformed BPIR: references a function that does not exist anywhere.
    // compile_bpir must report COMPILE_FAILED and roll back without going
    // through GEditor->UndoTransaction (which fires PostUndo on every client).
    const FString BadBpirCode = TEXT(
        "entry custom_event RollbackNoCompile() {\n"
        "    call ThisFunctionDoesNotExistAnywhere()\n"
        "}\n");

    auto RunFailedCompile = [&](const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), BadBpirCode);
        Payload->SetStringField(TEXT("mode"), TEXT("replace"));

        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("[%s] handler found"), Label),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(FString::Printf(TEXT("[%s] response sent"), Label), Capture.bWasCalled);
        TestFalse(FString::Printf(TEXT("[%s] structured error returned"), Label), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("[%s] error code is COMPILE_FAILED"), Label),
            Capture.ErrorCode, FString(TEXT("COMPILE_FAILED")));
    };

    // First failed compile — must roll back via Apply+Cancel, not Undo.
    RunFailedCompile(TEXT("first"));
    // Second failed compile — proves the handler stays callable after the
    // first failure (in the regression scenario the editor would have stalled
    // on an ensure dialog and this call would never return).
    RunFailedCompile(TEXT("second"));

    // Crucial assertion: neither failed-compile rollback may go through
    // GEditor->UndoTransaction. Any PostUndo hit means we're back on the
    // path that fires MarkBlueprintAsStructurallyModified → synchronous
    // skeleton recompile → ensure(SkeletonCompiledBlueprints.Num()==1).
    TestEqual(TEXT("Failed compile rollback did not fire PostUndo"),
        UndoCounter.PostUndoCount, 0);
    TestEqual(TEXT("Failed compile rollback did not fire PostRedo"),
        UndoCounter.PostRedoCount, 0);

    // Sanity follow-up: the dispatcher must still answer after two failed
    // compiles. We invoke a read-only handler and assert only that a response
    // came back — the response code itself is irrelevant; the symptom we are
    // guarding against is the handler hanging forever.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), TEXT("does-not-exist"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.get_node_connections"), Payload, Capture);
        TestTrue(TEXT("Follow-up handler registered"), bFound);
        if (bFound)
        {
            TestTrue(TEXT("Follow-up handler responded after failed compiles"),
                Capture.bWasCalled);
        }
    }

    return true;
}
