// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two shutdown-safety decisions of editor.quit.
//
// Nothing here calls the handler: invoking editor.quit on its success path ends
// the process running the suite. The decisions therefore live in
// EditorQuitPolicy.h and the evidence they weigh in State/ClientActivity.h, both
// exercised directly, plus a contract check that the registration still declares
// the escape hatch and still documents what it does.
//
// What they pin, and the incident each comes from:
//   - An editor that had served four RPCs for one agent was shut down by a second
//     agent that believed it was orphaned. Foreign traffic inside the window must
//     refuse; the caller's OWN traffic must never obstruct it.
//   - That shutdown then faulted in ~FStaticMeshEditor during Slate teardown,
//     because a capture had left a Static Mesh editor open. Asset editors must be
//     closed before exit is requested.
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include "Handlers/Editor/EditorQuitPolicy.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "State/ClientActivity.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SoftObjectPath.h"


namespace
{
    const FHandlerRegistration* FindRegistration(const FString& MethodName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    bool HasParam(const FHandlerRegistration* Reg, const TCHAR* ParamName)
    {
        if (!Reg)
        {
            return false;
        }
        for (const FParamSpec& Param : Reg->Params)
        {
            if (Param.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitInUseDecisionTest,
    "PinWright.editor.quit.InUseDecision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitInUseDecisionTest::RunTest(const FString& Parameters)
{
    const double Window = EditorQuitPolicy::InUseWindowSeconds;

    // The failure this exists to stop: the reaped editor's last foreign RPC was
    // 49 s old. Anything inside the window refuses.
    TestTrue(TEXT("foreign traffic inside the window refuses"),
        EditorQuitPolicy::ShouldRefuseAsInUse(
            /*bForce=*/false, /*bHasOtherClient=*/true, /*SecondsAgo=*/49.0, Window));

    // The reaper still works: silence is what an abandoned editor produces, so the
    // window always elapses on one.
    TestFalse(TEXT("foreign traffic older than the window allows the exit"),
        EditorQuitPolicy::ShouldRefuseAsInUse(false, true, Window + 1.0, Window));

    // No other client at all - the ordinary case of an agent closing the editor it
    // has been driving. It must not need force, or the flag becomes reflexive and
    // the guard stops meaning anything.
    TestFalse(TEXT("no other client never refuses"),
        EditorQuitPolicy::ShouldRefuseAsInUse(false, false, 0.0, Window));

    // The override is deliberate, not advisory.
    TestFalse(TEXT("force clears the refusal"),
        EditorQuitPolicy::ShouldRefuseAsInUse(/*bForce=*/true, true, 1.0, Window));

    // A misconfigured window fails OPEN. Failing closed would leave no way to shut
    // the editor down at all.
    TestFalse(TEXT("a non-positive window disables the guard"),
        EditorQuitPolicy::ShouldRefuseAsInUse(false, true, 1.0, 0.0));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitLedgerExcludesOwnTrafficTest,
    "PinWright.editor.quit.LedgerExcludesOwnTraffic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitLedgerExcludesOwnTrafficTest::RunTest(const FString& Parameters)
{
    ClientActivity::ResetForTests();
    ON_SCOPE_EXIT { ClientActivity::ResetForTests(); };

    const double Now = FPlatformTime::Seconds();
    ClientActivity::FActivity Other;

    // One client, its own traffic: quitting the editor you have been driving is
    // not obstructed.
    ClientActivity::NoteDispatchForTests(TEXT("agent-a"), TEXT("actor.list"), Now);
    TestFalse(TEXT("a client's own traffic is not 'another client'"),
        ClientActivity::GetMostRecentOtherClient(TEXT("agent-a"), Other));

    // A second client's traffic is visible to the first, named and dated - the
    // refusal has to be able to say what it saw, not just that it saw something.
    ClientActivity::NoteDispatchForTests(
        TEXT("agent-b"), TEXT("render.capture_asset_preview"), Now);
    TestTrue(TEXT("another client's traffic is reported"),
        ClientActivity::GetMostRecentOtherClient(TEXT("agent-a"), Other));
    TestEqual(TEXT("reports which client"), Other.ClientId, FString(TEXT("agent-b")));
    TestEqual(TEXT("reports the method"), Other.Method,
        FString(TEXT("render.capture_asset_preview")));
    TestTrue(TEXT("reports a fresh age"), Other.SecondsAgo < 60.0);

    // Newest wins: an old foreign client must not mask a live one, and the age
    // reported is the one the window is compared against.
    ClientActivity::NoteDispatchForTests(TEXT("agent-c"), TEXT("actor.describe"),
        Now - 10.0 * EditorQuitPolicy::InUseWindowSeconds);
    TestTrue(TEXT("still reports someone"),
        ClientActivity::GetMostRecentOtherClient(TEXT("agent-a"), Other));
    TestEqual(TEXT("reports the most recent other client"), Other.ClientId,
        FString(TEXT("agent-b")));

    // Backdated foreign traffic reads as old, which is what lets a genuinely
    // abandoned editor be reaped.
    ClientActivity::ResetForTests();
    ClientActivity::NoteDispatchForTests(TEXT("agent-b"), TEXT("actor.list"),
        Now - (EditorQuitPolicy::InUseWindowSeconds + 60.0));
    TestTrue(TEXT("old foreign traffic is still found"),
        ClientActivity::GetMostRecentOtherClient(TEXT("agent-a"), Other));
    TestFalse(TEXT("but it does not refuse the exit"),
        EditorQuitPolicy::ShouldRefuseAsInUse(false, true, Other.SecondsAgo));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitLedgerAnonymousBucketTest,
    "PinWright.editor.quit.LedgerAnonymousBucket",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitLedgerAnonymousBucketTest::RunTest(const FString& Parameters)
{
    ClientActivity::ResetForTests();
    ON_SCOPE_EXIT { ClientActivity::ResetForTests(); };

    const double Now = FPlatformTime::Seconds();
    ClientActivity::FActivity Other;

    // Every caller that sends no id is the same empty id, so the guard is inert
    // between two of them. That is honest rather than convenient: they are
    // genuinely indistinguishable at this layer, and inventing a difference would
    // block a raw client from ever quitting the editor it opened.
    ClientActivity::NoteDispatchForTests(FString(), TEXT("actor.list"), Now);
    TestFalse(TEXT("anonymous traffic is not foreign to an anonymous caller"),
        ClientActivity::GetMostRecentOtherClient(FString(), Other));

    // An identified caller still sees it, and an anonymous caller still sees an
    // identified one - only the anonymous-vs-anonymous pair collapses.
    TestTrue(TEXT("anonymous traffic is foreign to an identified caller"),
        ClientActivity::GetMostRecentOtherClient(TEXT("agent-a"), Other));

    ClientActivity::ResetForTests();
    ClientActivity::NoteDispatchForTests(TEXT("agent-a"), TEXT("actor.list"), Now);
    TestTrue(TEXT("identified traffic is foreign to an anonymous caller"),
        ClientActivity::GetMostRecentOtherClient(FString(), Other));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitClosesAssetEditorsTest,
    "PinWright.editor.quit.ClosesAssetEditors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitClosesAssetEditorsTest::RunTest(const FString& Parameters)
{
    // Deliberately does NOT close windows it did not open: CloseAllAssetEditors is
    // global, and a suite run inside a working editor would take the operator's
    // open asset editors with it. When something is already open the counting
    // contract cannot be checked without that side effect, so the test skips.
    UAssetEditorSubsystem* Subsystem =
        GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!Subsystem)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no UAssetEditorSubsystem (no GEditor in this context)."));
        return true;
    }
    if (Subsystem->GetAllEditedAssets().Num() > 0)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: asset editors this test did not open are ")
                   TEXT("already up; closing them would destroy state the run does not own."));
        return true;
    }

    const EditorQuitPolicy::FAssetEditorCloseResult Result =
        EditorQuitPolicy::CloseOpenAssetEditors();

    // The quit path reports these two numbers to the caller, so an editor that
    // refused to close is visible rather than silently left to fault during Slate
    // teardown.
    TestEqual(TEXT("reports nothing open"), Result.OpenCount, 0);
    TestEqual(TEXT("reports nothing left open"), Result.RemainingCount, 0);
    TestEqual(TEXT("and left nothing open"), Subsystem->GetAllEditedAssets().Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitClosesAnOpenAssetEditorTest,
    "PinWright.editor.quit.ClosesAnOpenAssetEditorBeforeExit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitClosesAnOpenAssetEditorTest::RunTest(const FString& Parameters)
{
    // The ordering test the sibling above cannot be. ClosesAssetEditors only ever runs the
    // nothing-was-open path - it asserts OpenCount == 0 - so it stays green whether or not
    // CloseOpenAssetEditors closes anything, and it stayed green through the entire life of
    // the crash it was filed against. Closing an asset editor that is ACTUALLY open is the
    // only assertion that fails when the pre-exit close is removed, deferred, or quietly
    // stops covering a toolkit type.
    //
    // Why the ordering matters, restated because the fix does not look like a crash fix: an
    // asset editor left open at exit is torn down by Slate from inside FEngineLoop::Exit(),
    // after the editor subsystems its destructor reaches for are gone. FStaticMeshEditor's
    // runs an unguarded RemoveAll() against one of them and faults reading a small member
    // offset. Nothing at the fault site can be guarded from outside the engine; the only
    // thing PinWright controls is whether that state ever reaches shutdown.
    UAssetEditorSubsystem* Subsystem =
        GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!Subsystem)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no UAssetEditorSubsystem (no GEditor in ")
                   TEXT("this context), so no asset editor could be opened to close."));
        return true;
    }
    if (Subsystem->GetAllEditedAssets().Num() > 0)
    {
        // Same restraint as the sibling: CloseAllAssetEditors is global, so a suite run inside
        // a working editor must not take the operator's open tabs with it.
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: asset editors this test did not open are ")
                   TEXT("already up; closing them would destroy state the run does not own."));
        return true;
    }

    // An engine primitive, not project content: this test ships to consumers whose projects
    // share no assets with the one it was written in, and a Static Mesh editor is the exact
    // toolkit the fault was captured in.
    UObject* Subject = FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad();
    if (!Subject)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: /Engine/BasicShapes/Cube did not load, so ")
                   TEXT("no toolkit could be opened."));
        return true;
    }

    ON_SCOPE_EXIT
    {
        // Never leave a window behind for the rest of the suite, including on a failed assert.
        if (Subsystem->GetAllEditedAssets().Num() > 0)
        {
            Subsystem->CloseAllAssetEditors();
        }
    };

    if (!Subsystem->OpenEditorForAsset(Subject) || Subsystem->GetAllEditedAssets().Num() == 0)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: the host could not open an asset editor ")
                   TEXT("(no Slate in this run), so the close could not be measured."));
        return true;
    }

    // The precondition the crash needs, now real rather than assumed.
    TestEqual(TEXT("one asset editor is open before the quit path runs"),
              Subsystem->GetAllEditedAssets().Num(), 1);

    const EditorQuitPolicy::FAssetEditorCloseResult Result =
        EditorQuitPolicy::CloseOpenAssetEditors();

    TestEqual(TEXT("the quit path saw the open editor"), Result.OpenCount, 1);
    TestEqual(TEXT("and none survived it"), Result.RemainingCount, 0);

    // The assertion that stands in for the crash: what reaches FEngineLoop::Exit() is an
    // editor with no toolkits left for Slate to tear down after the subsystems are gone.
    TestEqual(TEXT("nothing is left for Slate teardown to destroy"),
              Subsystem->GetAllEditedAssets().Num(), 0);

    // RemainingCount is only honest if it is derived after the close rather than assumed to
    // be zero - a caller reads it to decide whether the exit it was promised is safe.
    TestEqual(TEXT("the reported remainder matches the subsystem"),
              Result.RemainingCount, Subsystem->GetAllEditedAssets().Num());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorQuitContractTest,
    "PinWright.editor.quit.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorQuitContractTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindRegistration(TEXT("editor.quit"));
    if (!Reg)
    {
        AddError(TEXT("editor.quit is not registered."));
        return false;
    }

    // The override has to exist and has to be discoverable: a refusal that names a
    // flag the schema does not declare is a dead end for the caller.
    TestTrue(TEXT("declares force"), HasParam(Reg, TEXT("force")));

    // The summary is the generated wiki page for this verb. Both guards must be
    // documented there, because a caller that reaps editors reads that page and
    // nothing else before deciding whether a shutdown is safe.
    TestTrue(TEXT("documents the in-use refusal"),
        Reg->Summary.Contains(TEXT("EDITOR_IN_USE")));
    TestTrue(TEXT("documents closing asset editors"),
        Reg->Summary.Contains(TEXT("asset editor")));

    return true;
}

