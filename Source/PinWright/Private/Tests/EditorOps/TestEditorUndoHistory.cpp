// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.undo_history and the `steps` form of editor.undo / editor.redo.
//
// Every test swaps GEditor->Trans for a fresh UTransBuffer and restores the editor's own buffer on
// scope exit, so the editor's real undo history is neither read nor changed and the buffer
// contents are exactly what the test recorded (an empty buffer is otherwise impossible to set up
// without ResetTransaction, which would destroy the user's history).
#include "Misc/AutomationTest.h"

#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Handlers/ErrorCodes.h"
#include "ScopedTransaction.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace EditorUndoHistoryTest
{

struct FScopedIsolatedTransBuffer
{
    UTransactor* EditorBuffer = nullptr;

    FScopedIsolatedTransBuffer()
    {
        EditorBuffer = GEditor->Trans;
        UTransBuffer* Isolated = NewObject<UTransBuffer>();
        Isolated->Initialize(16 * 1024 * 1024);
        GEditor->Trans = Isolated;
    }

    ~FScopedIsolatedTransBuffer()
    {
        GEditor->Trans = EditorBuffer;
    }
};

static UCurveFloat* MakeTransactionalProbe()
{
    return NewObject<UCurveFloat>(GetTransientPackage(), NAME_None, RF_Transactional);
}

static void RecordTransaction(UCurveFloat* Probe, const TCHAR* Title, bool bValue)
{
    FScopedTransaction Transaction(FText::FromString(Title));
    Probe->Modify();
    Probe->bIsEventCurve = bValue;
}

// Titles joined with " | " so a mismatch prints both sides.
static FString Titles(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
{
    TArray<FString> Out;
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (Result.IsValid() && Result->TryGetArrayField(Field, Entries))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Entries)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            FString Title;
            if (Entry->TryGetObject(Obj) && (*Obj)->TryGetStringField(TEXT("title"), Title))
            {
                Out.Add(Title);
            }
        }
    }
    return FString::Join(Out, TEXT(" | "));
}

static TSharedPtr<FJsonObject> StepsPayload(int32 Steps)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("steps"), Steps);
    return Payload;
}

static bool SkipWithoutEditor(FAutomationTestBase& Test)
{
    if (GEditor && GEditor->Trans)
    {
        return false;
    }
    PinWrightTestSkip::SkipAssertions(Test, TEXT("no-editor-transactor"),
        TEXT("editor undo verbs need GEditor and its transaction buffer - skipped."));
    return true;
}

} // namespace EditorUndoHistoryTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorUndoHistoryListsTitledTransactionsTest,
    "PinWright.editor.undo_history.ListsTitledTransactionsNewestFirst",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditorUndoHistoryListsTitledTransactionsTest::RunTest(const FString& Parameters)
{
    using namespace EditorUndoHistoryTest;
    if (SkipWithoutEditor(*this))
    {
        return true;
    }
    FScopedIsolatedTransBuffer Isolated;
    UCurveFloat* Probe = MakeTransactionalProbe();
    RecordTransaction(Probe, TEXT("PW Undo History A"), true);
    RecordTransaction(Probe, TEXT("PW Undo History B"), false);
    RecordTransaction(Probe, TEXT("PW Undo History C"), true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.undo_history registered"),
        InvokeHandlerWithCapture(TEXT("editor.undo_history"), Payload, Capture));
    TestTrue(TEXT("history succeeds"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        return true;
    }
    TestEqual(TEXT("queueLength counts the three transactions"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("queueLength"))), 3);
    TestEqual(TEXT("nothing undone yet"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("undoCount"))), 0);
    TestTrue(TEXT("canUndo"), Capture.Result->GetBoolField(TEXT("canUndo")));
    TestFalse(TEXT("canRedo"), Capture.Result->GetBoolField(TEXT("canRedo")));
    TestTrue(TEXT("canRedoReason present when canRedo is false"),
        Capture.Result->HasField(TEXT("canRedoReason")));
    TestEqual(TEXT("undo side is newest first"), Titles(Capture.Result, TEXT("undo")),
        FString(TEXT("PW Undo History C | PW Undo History B | PW Undo History A")));
    TestEqual(TEXT("redo side empty"), Titles(Capture.Result, TEXT("redo")), FString());
    TestFalse(TEXT("undo side not truncated"), Capture.Result->GetBoolField(TEXT("undoTruncated")));

    // limit caps each side and reports the cut.
    Payload->SetNumberField(TEXT("limit"), 2);
    InvokeHandlerWithCapture(TEXT("editor.undo_history"), Payload, Capture);
    TestTrue(TEXT("limited history succeeds"), Capture.bSuccess);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("limit keeps the two newest"), Titles(Capture.Result, TEXT("undo")),
            FString(TEXT("PW Undo History C | PW Undo History B")));
        TestTrue(TEXT("undoTruncated reports the cut"), Capture.Result->GetBoolField(TEXT("undoTruncated")));
    }

    Payload->SetNumberField(TEXT("limit"), 0);
    InvokeHandlerWithCapture(TEXT("editor.undo_history"), Payload, Capture);
    TestFalse(TEXT("limit 0 is refused"), Capture.bSuccess);
    TestEqual(TEXT("limit 0 is INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorUndoHistoryEmptyBufferTest,
    "PinWright.editor.undo_history.EmptyBufferIsATypedAnswer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditorUndoHistoryEmptyBufferTest::RunTest(const FString& Parameters)
{
    using namespace EditorUndoHistoryTest;
    if (SkipWithoutEditor(*this))
    {
        return true;
    }
    FScopedIsolatedTransBuffer Isolated;

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("editor.undo_history"), MakeShared<FJsonObject>(), Capture);
    TestTrue(TEXT("an empty buffer is answered, not refused"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        return true;
    }
    TestEqual(TEXT("queueLength 0"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("queueLength"))), 0);
    TestFalse(TEXT("canUndo false"), Capture.Result->GetBoolField(TEXT("canUndo")));
    FString Reason;
    TestTrue(TEXT("canUndoReason carries the engine's reason"),
        Capture.Result->TryGetStringField(TEXT("canUndoReason"), Reason) && !Reason.IsEmpty());
    TestEqual(TEXT("undo list empty"), Titles(Capture.Result, TEXT("undo")), FString());
    TestEqual(TEXT("redo list empty"), Titles(Capture.Result, TEXT("redo")), FString());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorUndoStepsReportTitlesTest,
    "PinWright.editor.undo.StepsUndoAndRedoReportTitles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditorUndoStepsReportTitlesTest::RunTest(const FString& Parameters)
{
    using namespace EditorUndoHistoryTest;
    if (SkipWithoutEditor(*this))
    {
        return true;
    }
    FScopedIsolatedTransBuffer Isolated;
    UCurveFloat* Probe = MakeTransactionalProbe();
    Probe->bIsEventCurve = false;
    RecordTransaction(Probe, TEXT("PW Undo Steps A"), true);
    RecordTransaction(Probe, TEXT("PW Undo Steps B"), false);
    RecordTransaction(Probe, TEXT("PW Undo Steps C"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.undo registered"),
        InvokeHandlerWithCapture(TEXT("editor.undo"), StepsPayload(2), Capture));
    TestTrue(TEXT("two-step undo succeeds"), Capture.bSuccess);
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("success is true when every step ran"), Capture.Result->GetBoolField(TEXT("success")));
        TestEqual(TEXT("completedSteps"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("completedSteps"))), 2);
        TestEqual(TEXT("undone newest first"), Titles(Capture.Result, TEXT("undone")),
            FString(TEXT("PW Undo Steps C | PW Undo Steps B")));
        TestEqual(TEXT("undoCount after"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("undoCount"))), 2);
    }
    // The state after undoing C and B is the one A recorded.
    TestTrue(TEXT("undo restored the value A set"), Probe->bIsEventCurve);

    // More steps than remain: the one available runs, the rest is reported, not faked.
    InvokeHandlerWithCapture(TEXT("editor.undo"), StepsPayload(5), Capture);
    TestTrue(TEXT("partial undo still returns the step that ran"), Capture.bSuccess);
    if (Capture.Result.IsValid())
    {
        TestFalse(TEXT("success is false when fewer steps ran"), Capture.Result->GetBoolField(TEXT("success")));
        TestEqual(TEXT("one step completed"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("completedSteps"))), 1);
        TestEqual(TEXT("undone A"), Titles(Capture.Result, TEXT("undone")), FString(TEXT("PW Undo Steps A")));
        TestTrue(TEXT("stoppedReason names why"), Capture.Result->HasField(TEXT("stoppedReason")));
    }
    TestFalse(TEXT("undoing A restored the initial value"), Probe->bIsEventCurve);

    InvokeHandlerWithCapture(TEXT("editor.redo"), StepsPayload(2), Capture);
    TestTrue(TEXT("two-step redo succeeds"), Capture.bSuccess);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("redone in redo order"), Titles(Capture.Result, TEXT("redone")),
            FString(TEXT("PW Undo Steps A | PW Undo Steps B")));
    }
    TestFalse(TEXT("redo re-applied B's value"), Probe->bIsEventCurve);

    InvokeHandlerWithCapture(TEXT("editor.undo_history"), MakeShared<FJsonObject>(), Capture);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("history redo side holds C"), Titles(Capture.Result, TEXT("redo")),
            FString(TEXT("PW Undo Steps C")));
        TestEqual(TEXT("history undo side holds B then A"), Titles(Capture.Result, TEXT("undo")),
            FString(TEXT("PW Undo Steps B | PW Undo Steps A")));
    }

    InvokeHandlerWithCapture(TEXT("editor.undo"), StepsPayload(0), Capture);
    TestFalse(TEXT("steps 0 is refused"), Capture.bSuccess);
    TestEqual(TEXT("steps 0 is INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorUndoEmptyBufferTest,
    "PinWright.editor.undo.EmptyBufferIsNothingToUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditorUndoEmptyBufferTest::RunTest(const FString& Parameters)
{
    using namespace EditorUndoHistoryTest;
    if (SkipWithoutEditor(*this))
    {
        return true;
    }
    FScopedIsolatedTransBuffer Isolated;

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("editor.undo"), StepsPayload(1), Capture);
    TestFalse(TEXT("undo on an empty buffer is an error"), Capture.bSuccess);
    TestEqual(TEXT("undo code"), Capture.ErrorCode, FString(ErrorCodes::ERR_NOTHING_TO_UNDO));
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("payload reports zero completed steps"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("completedSteps"))), 0);
        TestTrue(TEXT("payload carries the engine's reason"), Capture.Result->HasField(TEXT("stoppedReason")));
    }
    else
    {
        AddError(TEXT("NOTHING_TO_UNDO carries no payload"));
    }

    InvokeHandlerWithCapture(TEXT("editor.redo"), StepsPayload(1), Capture);
    TestFalse(TEXT("redo on an empty buffer is an error"), Capture.bSuccess);
    TestEqual(TEXT("redo code"), Capture.ErrorCode, FString(ErrorCodes::ERR_NOTHING_TO_REDO));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
