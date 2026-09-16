// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins the editor-identity handshake (Transport/EditorIdentity.h) and the dispatcher gate that
// enforces it (RpcDispatcher::ProcessRequest).
//
// THE DEFECT THESE PIN. UPinWrightSettings::DerivePortFromPath hashes the PROJECT DIRECTORY, so
// the MCP port is a function of the project and not of the editor instance. Two editors opened
// on one project derive the same port, exactly one wins the bind, and every RPC on that port
// reaches that one - previously with nothing in any response naming the process that ran it. A
// client that had launched its own editor could therefore be driving a different, already-running
// editor with no error at all. That is not hypothetical: a misrouted blueprint.set_default
// reinstanced live drones in the user's interactive editor and crashed it, and four board tickets
// in one week rested on measurements taken against the wrong tree.
//
// The load-bearing assertion is MismatchRefusesBeforeTheHandlerRuns: a mutating call aimed at the
// wrong editor must not execute. A gate that merely annotated the response would leave the
// mutation landed.
//
// These drive FRpcDispatcher::ProcessRequest directly rather than a socket, because the defect is
// in the dispatch decision, not in the wire format; a test needing two same-project editors and a
// contested port would be skipped on every host, which is how the gap survived this long.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Transport/EditorIdentity.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Counts every entry into a handler body. The whole point of the gate is that a refused request
// never reaches one, and only a counter can tell "refused" from "ran, then reported an error".
static int32 GPwIdentityProbeCallCount = 0;

// Declares no parameters on purpose: if the dispatcher ever stopped stripping the reserved
// assertion field, an accepted assertion would reach param validation and be rejected as
// UNKNOWN_PARAMS. MatchingAssertionRunsTheHandler fails in that case.
REGISTER_RPC_HANDLER("_test.identity_probe", "_test",
    "Test-only handler that counts invocations for the editor-identity gate tests",
    RPC_NO_PARAMS)
{
    ++GPwIdentityProbeCallCount;
    Ctx.SendSuccess(TEXT("identity_probe ok"));
    return true;
}

namespace PinWrightEditorIdentityTest
{
    // Unity-build safety: named namespace, and every helper below carries a distinctive name.

    TSharedPtr<FJsonObject> MakeAssertingParams(const TSharedPtr<FJsonObject>& Assertion)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(PinWrightEditorIdentity::AssertionParamName(), Assertion);
        return Params;
    }

    // An assertion naming this process correctly, built from the measured identity rather than
    // from anything a test could invent.
    TSharedPtr<FJsonObject> MakeTruthfulAssertion()
    {
        const PinWrightEditorIdentity::FIdentity& Id = PinWrightEditorIdentity::Measured();
        TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
        Assertion->SetNumberField(TEXT("pid"), static_cast<double>(Id.ProcessId));
        Assertion->SetStringField(TEXT("instance_id"), Id.InstanceId);
        Assertion->SetStringField(TEXT("project_file"), Id.ProjectFilePath);
        return Assertion;
    }

    FString ReadStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Out;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Field, Out);
        }
        return Out;
    }
}

// ============================================================================
// Every published field is read out of the running process, not out of settings
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityMeasuredTest,
    "PinWright.transport.identity.MeasuredFromTheRunningProcess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityMeasuredTest::RunTest(const FString& Parameters)
{
    const PinWrightEditorIdentity::FIdentity& Id = PinWrightEditorIdentity::Measured();

    // Cast to int64: FAutomationTestBase::TestEqual has no uint32 overload, so a raw uint32 pair
    // is ambiguous between its int32/int64/SIZE_T/float/double signatures.
    TestEqual(TEXT("pid is this process's pid"),
        static_cast<int64>(Id.ProcessId),
        static_cast<int64>(FPlatformProcess::GetCurrentProcessId()));

    // Not FPaths::GetProjectFilePath() verbatim: the identity is normalized to a full path so a
    // caller comparing against its own absolute path is not defeated by the engine having stored
    // a relative one.
    TestEqual(TEXT("project file is the absolute .uproject path"),
        Id.ProjectFilePath,
        FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));

    FGuid ParsedInstanceId;
    TestTrue(TEXT("instance id is a real GUID"),
        FGuid::Parse(Id.InstanceId, ParsedInstanceId));
    TestTrue(TEXT("instance id is not the null GUID"), ParsedInstanceId.IsValid());

    // Cached, so two calls in one process agree. A regenerating token would make every
    // assertion fail on the second call and push callers to stop asserting.
    TestEqual(TEXT("instance id is stable within the process"),
        PinWrightEditorIdentity::Measured().InstanceId, Id.InstanceId);

    TestFalse(TEXT("engine version is measured"), Id.EngineVersion.IsEmpty());
    TestFalse(TEXT("plugin build stamp is measured"), Id.PluginBuild.IsEmpty());
    TestFalse(TEXT("executable path is measured"), Id.ExecutablePath.IsEmpty());

    return true;
}

// ============================================================================
// Back-compat: a caller that asserts nothing is served exactly as before
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityNoAssertionServedTest,
    "PinWright.transport.identity.UnassertedCallIsServedUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityNoAssertionServedTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const int32 CallsBefore = GPwIdentityProbeCallCount;

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.identity_probe"),
        TEXT("id-none"), MakeShared<FJsonObject>(), bSuccess, ErrorCode);

    TestTrue(TEXT("an unasserted request is served"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    TestEqual(TEXT("the handler ran"), GPwIdentityProbeCallCount, CallsBefore + 1);

    // system.identity itself must answer without an assertion - it is the handshake a caller
    // makes before it knows anything to assert.
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("system.identity"),
        TEXT("id-handshake"), MakeShared<FJsonObject>(), bSuccess, Result, ErrorCode);

    TestTrue(TEXT("system.identity answers an unasserted call"), bSuccess);
    if (Result.IsValid())
    {
        double ReportedPid = 0.0;
        TestTrue(TEXT("system.identity reports a pid"),
            Result->TryGetNumberField(TEXT("pid"), ReportedPid));
        TestEqual(TEXT("the reported pid is this process"),
            static_cast<int64>(ReportedPid),
            static_cast<int64>(FPlatformProcess::GetCurrentProcessId()));
        TestEqual(TEXT("the reported instance id is the measured one"),
            PinWrightEditorIdentityTest::ReadStringField(Result, TEXT("instance_id")),
            PinWrightEditorIdentity::Measured().InstanceId);
    }
    else
    {
        AddError(TEXT("system.identity returned no result object"));
    }

    return true;
}

// ============================================================================
// A correct assertion is served; a wrong one is refused before the handler runs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityMatchingAssertionTest,
    "PinWright.transport.identity.MatchingAssertionRunsTheHandler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityMatchingAssertionTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const int32 CallsBefore = GPwIdentityProbeCallCount;

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.identity_probe"),
        TEXT("id-match"),
        PinWrightEditorIdentityTest::MakeAssertingParams(
            PinWrightEditorIdentityTest::MakeTruthfulAssertion()),
        bSuccess, ErrorCode);

    // A failure here with UNKNOWN_PARAMS means the reserved field stopped being stripped before
    // handler validation; a failure with EDITOR_IDENTITY_MISMATCH means the comparison is wrong.
    TestEqual(TEXT("a truthful assertion produces no error code"), ErrorCode, FString());
    TestTrue(TEXT("a truthful assertion is served"), bSuccess);
    TestEqual(TEXT("the handler ran"), GPwIdentityProbeCallCount, CallsBefore + 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityMismatchRefusedTest,
    "PinWright.transport.identity.MismatchRefusesBeforeTheHandlerRuns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityMismatchRefusedTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const PinWrightEditorIdentity::FIdentity& Id = PinWrightEditorIdentity::Measured();
    const uint32 WrongPid = Id.ProcessId + 1;

    TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
    Assertion->SetNumberField(TEXT("pid"), static_cast<double>(WrongPid));

    const int32 CallsBefore = GPwIdentityProbeCallCount;

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.identity_probe"),
        TEXT("id-mismatch"),
        PinWrightEditorIdentityTest::MakeAssertingParams(Assertion),
        bSuccess, Result, ErrorCode);

    TestFalse(TEXT("a wrong assertion is not served"), bSuccess);
    TestEqual(TEXT("the refusal is typed"), ErrorCode, FString(TEXT("EDITOR_IDENTITY_MISMATCH")));

    // The reason the whole feature exists: the mutation must not have landed.
    TestEqual(TEXT("the handler did NOT run"), GPwIdentityProbeCallCount, CallsBefore);

    // Both sides named, in the message and in the structured payload. A refusal that names only
    // the expectation leaves the caller unable to say which editor answered - which is the
    // question it asked.
    TestTrue(TEXT("the message names the asserted pid"),
        Sink->Message.Contains(FString::Printf(TEXT("%u"), WrongPid)));
    TestTrue(TEXT("the message names the answering pid"),
        Sink->Message.Contains(FString::Printf(TEXT("%u"), Id.ProcessId)));

    if (Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* ExpectedObj = nullptr;
        const TSharedPtr<FJsonObject>* ActualObj = nullptr;
        if (Result->TryGetObjectField(TEXT("expected"), ExpectedObj) &&
            Result->TryGetObjectField(TEXT("actual"), ActualObj))
        {
            TestEqual(TEXT("payload carries the expected pid"),
                PinWrightEditorIdentityTest::ReadStringField(*ExpectedObj, TEXT("pid")),
                FString::Printf(TEXT("%u"), WrongPid));
            TestEqual(TEXT("payload carries the answering pid"),
                PinWrightEditorIdentityTest::ReadStringField(*ActualObj, TEXT("pid")),
                FString::Printf(TEXT("%u"), Id.ProcessId));
        }
        else
        {
            AddError(TEXT("refusal payload is missing 'expected' and/or 'actual'"));
        }
    }
    else
    {
        AddError(TEXT("refusal carried no payload naming both sides"));
    }

    return true;
}

// ============================================================================
// An assertion that cannot be verified is refused, never quietly ignored
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityUnverifiableAssertionTest,
    "PinWright.transport.identity.UnverifiableAssertionIsRefusedNotIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityUnverifiableAssertionTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // A misspelled field. Silently ignoring it would hand the caller a green answer for a check
    // that compared nothing - strictly worse than not asserting, because the caller then believes
    // it verified the target.
    TSharedPtr<FJsonObject> Misspelled = MakeShared<FJsonObject>();
    Misspelled->SetNumberField(TEXT("pdi"), 1.0);

    int32 CallsBefore = GPwIdentityProbeCallCount;
    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.identity_probe"),
        TEXT("id-typo"), PinWrightEditorIdentityTest::MakeAssertingParams(Misspelled),
        bSuccess, ErrorCode);

    TestFalse(TEXT("an unverifiable assertion is not served"), bSuccess);
    TestEqual(TEXT("refused as bad input"), ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestEqual(TEXT("the handler did NOT run"), GPwIdentityProbeCallCount, CallsBefore);
    TestTrue(TEXT("the refusal names the offending field"), Sink->Message.Contains(TEXT("pdi")));

    // An empty assertion asserts nothing and is refused for the same reason.
    CallsBefore = GPwIdentityProbeCallCount;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("_test.identity_probe"),
        TEXT("id-empty"),
        PinWrightEditorIdentityTest::MakeAssertingParams(MakeShared<FJsonObject>()),
        bSuccess, ErrorCode);

    TestFalse(TEXT("an empty assertion is not served"), bSuccess);
    TestEqual(TEXT("refused as bad input"), ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestEqual(TEXT("the handler did NOT run"), GPwIdentityProbeCallCount, CallsBefore);

    return true;
}

// ============================================================================
// The comparison itself, driven with a synthetic identity so it needs no editor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwEditorIdentityComparisonTest,
    "PinWright.transport.identity.ComparisonAcceptsSpellingsAndFoldsPathCase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwEditorIdentityComparisonTest::RunTest(const FString& Parameters)
{
    PinWrightEditorIdentity::FIdentity Synthetic;
    Synthetic.ProcessId = 4321;
    Synthetic.InstanceId = TEXT("11112222-3333-4444-5555-666677778888");
    Synthetic.ProjectFilePath = TEXT("X:/Src/Unreal/Proj/Proj.uproject");
    Synthetic.ProjectName = TEXT("Proj");
    Synthetic.EngineVersion = TEXT("5.8.0-0+UE5");
    Synthetic.PluginBuild = TEXT("Aug 29 2026 10:00:00");

    // pid as a number, as a numeric string, and as a float all name the same process. A client
    // that routes ids through a string map must not be locked out of the one field it can pin
    // without a prior handshake.
    TArray<TSharedPtr<FJsonValue>> PidSpellings;
    PidSpellings.Add(MakeShared<FJsonValueNumber>(4321.0));
    PidSpellings.Add(MakeShared<FJsonValueString>(FString(TEXT("4321"))));
    for (const TSharedPtr<FJsonValue>& PidSpelling : PidSpellings)
    {
        TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
        Assertion->SetField(TEXT("pid"), PidSpelling);
        const PinWrightEditorIdentity::FAssertionVerdict Verdict =
            PinWrightEditorIdentity::CheckAssertion(
                Synthetic, MakeShared<FJsonValueObject>(Assertion));
        TestTrue(TEXT("every pid spelling of the same process is accepted"), Verdict.bAccepted);
    }

    // Path case and separators are folded, matching how DerivePortFromPath folds the project
    // path when it computes the port these editors collide on.
    {
        TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
        Assertion->SetStringField(TEXT("project_file"), TEXT("x:\\src\\unreal\\proj\\Proj.uproject"));
        const PinWrightEditorIdentity::FAssertionVerdict Verdict =
            PinWrightEditorIdentity::CheckAssertion(
                Synthetic, MakeShared<FJsonValueObject>(Assertion));
        TestTrue(TEXT("the same project path in another spelling is accepted"), Verdict.bAccepted);
    }

    // A different project is a different editor, and must be refused.
    {
        TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
        Assertion->SetStringField(TEXT("project_file"), TEXT("X:/src/unreal/Other/Other.uproject"));
        const PinWrightEditorIdentity::FAssertionVerdict Verdict =
            PinWrightEditorIdentity::CheckAssertion(
                Synthetic, MakeShared<FJsonValueObject>(Assertion));
        TestFalse(TEXT("a different project is refused"), Verdict.bAccepted);
        TestEqual(TEXT("refusal is typed as an identity mismatch"),
            Verdict.ErrorCode, FString(TEXT("EDITOR_IDENTITY_MISMATCH")));
        TestTrue(TEXT("refusal names the asserted project"),
            Verdict.Message.Contains(TEXT("Other.uproject")));
        TestTrue(TEXT("refusal names the answering project"),
            Verdict.Message.Contains(TEXT("Proj.uproject")));
    }

    // Two same-project editors agree on project_file and disagree on instance_id: this pair is
    // the exact case the ticket is about, so it gets its own assertion.
    {
        TSharedPtr<FJsonObject> Assertion = MakeShared<FJsonObject>();
        Assertion->SetStringField(TEXT("project_file"), Synthetic.ProjectFilePath);
        Assertion->SetStringField(TEXT("instance_id"), TEXT("99998888-7777-6666-5555-444433332222"));
        const PinWrightEditorIdentity::FAssertionVerdict Verdict =
            PinWrightEditorIdentity::CheckAssertion(
                Synthetic, MakeShared<FJsonValueObject>(Assertion));
        TestFalse(TEXT("a same-project sibling editor is refused"), Verdict.bAccepted);
        TestTrue(TEXT("refusal names the asserted instance"),
            Verdict.Message.Contains(TEXT("99998888")));
        TestTrue(TEXT("refusal names the answering instance"),
            Verdict.Message.Contains(TEXT("11112222")));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
