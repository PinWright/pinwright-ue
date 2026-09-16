// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-add-emitter-snapshots-emitter-silently.
//
// UNiagaraSystem::AddEmitterHandle always DUPLICATES the source emitter into the system. What it
// does with the link back to the asset is the whole defect: UNiagaraEmitter::CreateWithParentAndOwner
// sets VersionedParent + VersionedParentAtLastMerge on the copy, and AddEmitterHandle strips them
// again when the source asset declares itself non-inheritable (NiagaraSystem.cpp:3019-3032). With
// the link the system tracks the asset; without it the copy is frozen forever. niagara.add_emitter
// published nothing that told the two apart - not in its response, not in niagara.inspect, not in
// niagara.validate - so three sessions wired emitters, kept editing them, and shipped systems
// running the pre-edit content past a clean compile, a clean save and a clean strict validate.
//
// What is asserted here, in the order the defect is closed:
//   1. the default is INHERITANCE, and the response names it (`emitterSource` / `parentEmitterPath`);
//   2. the snapshot is still reachable, but only by asking for it (`inherit: false`);
//   3. an edit to the parent asset does NOT reach the system on its own, validate says so
//      (EMITTER_PARENT_STALE), and niagara.refresh_emitter merges it in;
//   4. refresh refuses on a system with nothing to merge from, rather than reporting a zero-item
//      success;
//   5. a named snapshot refusal carries its identity, leaves the transaction queue unchanged,
//      and preserves the handle/data identity. The transaction-start event is the decisive
//      no-mutation observation; queue/state equality proves no retained transaction or state
//      mutation on the transient fixture.
//
// Counterfactual against the pre-fix handler: (1) fails on the absent `emitterSource` field, (3)
// fails on the absent validate issue and the absent niagara.refresh_emitter verb, (4) fails on the
// absent verb, and (5) fails on the direct TransactionStarted observation and missing named error
// contract. Its queue, state, and identity assertions additionally catch any retained transaction
// or state mutation. (2) passes trivially there, which is the point - the old behaviour is now the
// opt-in.
#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/ScopeExit.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"

namespace
{
    // Names are prefixed rather than bare because PCH + Unity merges translation units, and two
    // anonymous namespaces carrying the same helper name in one merged TU is a redefinition.

    // A transient system with an empty handle list plus a separate transient emitter asset, which
    // is the shape every test below starts from. Both are rooted by the fixture helpers; the
    // caller pairs this with FAuthorableSystemRoots.
    bool NiagaraInheritanceTest_MakeFixture(
        FAutomationTestBase& Test,
        FString& OutSystemPath,
        UNiagaraSystem*& OutSystem,
        FString& OutEmitterPath,
        UNiagaraEmitter*& OutEmitter)
    {
        OutSystem = NiagaraEditTestUtils::NewTransientSystem(OutSystemPath);
        if (!Test.TestNotNull(TEXT("transient UNiagaraSystem fixture"), OutSystem))
        {
            return false;
        }
        OutEmitter = NiagaraEditTestUtils::NewTransientEmitter(OutEmitterPath);
        if (!Test.TestNotNull(TEXT("transient UNiagaraEmitter fixture"), OutEmitter))
        {
            return false;
        }
        return Test.TestEqual(TEXT("the fixture system starts with no emitter handles"),
            OutSystem->GetEmitterHandles().Num(), 0);
    }

    // The engine-side truth the response is checked against: the parent pointer on the copy the
    // system actually holds, not anything the handler said about it.
    const UNiagaraEmitter* NiagaraInheritanceTest_ParentOfFirstHandle(const UNiagaraSystem& System)
    {
        if (System.GetEmitterHandles().Num() == 0)
        {
            return nullptr;
        }
        const FVersionedNiagaraEmitterData* Data = System.GetEmitterHandles()[0].GetEmitterData();
        if (!Data)
        {
            return nullptr;
        }
        return Data->GetParent().Emitter;
    }

    int32 NiagaraInheritanceTest_RendererCountOfFirstHandle(const UNiagaraSystem& System)
    {
        if (System.GetEmitterHandles().Num() == 0)
        {
            return INDEX_NONE;
        }
        const FVersionedNiagaraEmitterData* Data = System.GetEmitterHandles()[0].GetEmitterData();
        return Data ? Data->GetRenderers().Num() : INDEX_NONE;
    }

    bool NiagaraInheritanceTest_HasValidationIssue(const TSharedPtr<FJsonObject>& Result, const TCHAR* Code)
    {
        if (!Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
        if (!Result->TryGetArrayField(TEXT("issues"), Issues) || Issues == nullptr)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Issues)
        {
            const TSharedPtr<FJsonObject>* Issue = nullptr;
            FString IssueCode;
            if (Value.IsValid() && Value->TryGetObject(Issue) && Issue != nullptr
                && (*Issue)->TryGetStringField(TEXT("code"), IssueCode)
                && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> NiagaraInheritanceTest_AddPayload(const FString& SystemPath, const FString& EmitterPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemPath"), SystemPath);
        Payload->SetStringField(TEXT("emitterPath"), EmitterPath);
        Payload->SetStringField(TEXT("name"), TEXT("InheritedEmitter"));
        // compile:false throughout - the relationship under test is written before any compile,
        // and compiling a synthetic fixture buys nothing but time.
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }
}

// 1. The default keeps the link, and the response names the relationship it created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraAddEmitterInheritsFromSourceTest,
    "PinWright.niagara.add_emitter.InheritsFromSourceEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEmitterInheritsFromSourceTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.add_emitter is registered"), IsHandlerRegistered(TEXT("niagara.add_emitter")));

    FString SystemPath;
    FString EmitterPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bFixtureReady = NiagaraInheritanceTest_MakeFixture(*this, SystemPath, System, EmitterPath, Emitter);
    Roots.System = System;
    Roots.Emitter = Emitter;
    if (!bFixtureReady)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.add_emitter"), NiagaraInheritanceTest_AddPayload(SystemPath, EmitterPath), Capture)
        || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("the handle was added"), System->GetEmitterHandles().Num(), 1);

    // The response's own account of what it built. Absent entirely before this fix.
    FString EmitterSource;
    TestTrue(TEXT("the response carries emitterSource"),
        Capture.Result->TryGetStringField(TEXT("emitterSource"), EmitterSource));
    TestEqual(TEXT("the default relationship is inheritance"), EmitterSource, FString(TEXT("inherited")));

    FString ParentPath;
    TestTrue(TEXT("the response names the parent it linked to"),
        Capture.Result->TryGetStringField(TEXT("parentEmitterPath"), ParentPath));
    TestEqual(TEXT("the parent named is the emitter asset that was asked for"),
        ParentPath, Emitter->GetPathName());

    // The measurement the response cannot fake: the copy's own parent pointer.
    TestTrue(TEXT("the system's copy points back at the source emitter asset"),
        NiagaraInheritanceTest_ParentOfFirstHandle(*System) == Emitter);

    return true;
}

// 2. The snapshot survives as an explicit, named opt-in - the editor's "Remove Parent Emitter".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraAddEmitterSnapshotOptionTest,
    "PinWright.niagara.add_emitter.SnapshotOptionRemovesParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEmitterSnapshotOptionTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    FString EmitterPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bFixtureReady = NiagaraInheritanceTest_MakeFixture(*this, SystemPath, System, EmitterPath, Emitter);
    Roots.System = System;
    Roots.Emitter = Emitter;
    if (!bFixtureReady)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = NiagaraInheritanceTest_AddPayload(SystemPath, EmitterPath);
    Payload->SetBoolField(TEXT("inherit"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), Payload, Capture)
        || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("the handle was added"), System->GetEmitterHandles().Num(), 1);

    FString EmitterSource;
    Capture.Result->TryGetStringField(TEXT("emitterSource"), EmitterSource);
    TestEqual(TEXT("inherit:false reports a snapshot"), EmitterSource, FString(TEXT("snapshot")));
    TestFalse(TEXT("a snapshot names no parent"), Capture.Result->HasField(TEXT("parentEmitterPath")));
    TestNull(TEXT("a snapshot carries no parent pointer either"),
        NiagaraInheritanceTest_ParentOfFirstHandle(*System));

    return true;
}

// 3. The staleness itself: an edit to the parent asset does not reach the system, validate says so,
//    and refresh_emitter merges it in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRefreshEmitterMergesParentChangesTest,
    "PinWright.niagara.refresh_emitter.MergesParentChanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRefreshEmitterMergesParentChangesTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.refresh_emitter is registered"),
        IsHandlerRegistered(TEXT("niagara.refresh_emitter")));

    FString SystemPath;
    FString EmitterPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bFixtureReady = NiagaraInheritanceTest_MakeFixture(*this, SystemPath, System, EmitterPath, Emitter);
    Roots.System = System;
    Roots.Emitter = Emitter;
    if (!bFixtureReady)
    {
        return false;
    }

    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.add_emitter"), NiagaraInheritanceTest_AddPayload(SystemPath, EmitterPath), AddCapture))
    {
        return false;
    }
    if (!TestTrue(TEXT("the system's copy inherits from the emitter asset"),
            NiagaraInheritanceTest_ParentOfFirstHandle(*System) == Emitter))
    {
        return false;
    }

    const int32 RenderersBefore = NiagaraInheritanceTest_RendererCountOfFirstHandle(*System);

    // Edit the PARENT asset after it has already been wired in. This is the ordinary authoring
    // order the defect punishes, driven through the same verb an agent would use.
    TSharedPtr<FJsonObject> RendererPayload = MakeShared<FJsonObject>();
    RendererPayload->SetStringField(TEXT("assetPath"), EmitterPath);
    RendererPayload->SetStringField(TEXT("rendererClassPath"), TEXT("NiagaraSpriteRendererProperties"));
    FTestResponseCapture RendererCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.add_renderer"), RendererPayload, RendererCapture))
    {
        return false;
    }

    // The defect, stated as an assertion: the system does NOT follow the asset on its own.
    const FVersionedNiagaraEmitterData* ParentData = Emitter->GetLatestEmitterData();
    TestEqual(TEXT("the parent asset gained a renderer"),
        ParentData ? ParentData->GetRenderers().Num() : INDEX_NONE,
        RenderersBefore + 1);
    TestEqual(TEXT("the system's copy did not follow the edit by itself"),
        NiagaraInheritanceTest_RendererCountOfFirstHandle(*System), RenderersBefore);

    // ...and validate is where an author asks whether the asset is sound, so it has to say so.
    TSharedPtr<FJsonObject> ValidatePayload = MakeShared<FJsonObject>();
    ValidatePayload->SetStringField(TEXT("assetPath"), SystemPath);
    ValidatePayload->SetStringField(TEXT("level"), TEXT("strict"));
    FTestResponseCapture ValidateCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.validate"), ValidatePayload, ValidateCapture))
    {
        TestTrue(TEXT("strict validate reports the stale parent"),
            NiagaraInheritanceTest_HasValidationIssue(ValidateCapture.Result, TEXT("EMITTER_PARENT_STALE")));
    }

    TSharedPtr<FJsonObject> RefreshPayload = MakeShared<FJsonObject>();
    RefreshPayload->SetStringField(TEXT("systemPath"), SystemPath);
    RefreshPayload->SetBoolField(TEXT("compile"), false);
    RefreshPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture RefreshCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.refresh_emitter"), RefreshPayload, RefreshCapture)
        || !RefreshCapture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* RefreshedEntries = nullptr;
    if (!TestTrue(TEXT("the refresh response lists exactly the one handle it touched"),
            RefreshCapture.Result->TryGetArrayField(TEXT("refreshed"), RefreshedEntries)
                && RefreshedEntries != nullptr
                && RefreshedEntries->Num() == 1))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Entry = nullptr;
    if (!TestTrue(TEXT("the refreshed entry is an object"),
            (*RefreshedEntries)[0].IsValid() && (*RefreshedEntries)[0]->TryGetObject(Entry) && Entry != nullptr))
    {
        return false;
    }

    bool bWasStale = false;
    (*Entry)->TryGetBoolField(TEXT("wasStale"), bWasStale);
    TestTrue(TEXT("the refresh reports the handle was behind its parent"), bWasStale);

    FString EntryParentPath;
    (*Entry)->TryGetStringField(TEXT("parentEmitterPath"), EntryParentPath);
    TestEqual(TEXT("the refreshed entry names the parent asset"), EntryParentPath, Emitter->GetPathName());

    int32 MergesFailed = 0;
    RefreshCapture.Result->TryGetNumberField(TEXT("mergesFailed"), MergesFailed);
    if (MergesFailed != 0)
    {
        // The merge itself is the engine's FNiagaraScriptMergeManager working on a synthetic
        // fixture. Everything above this line is asserted unconditionally; only the outcome of the
        // engine merge is stepped over, and the run must not read as clean when it is.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-parent-merge-unavailable"),
            TEXT("MergeChangesFromParent failed on the fixture emitter, so the post-merge renderer ")
            TEXT("count and synchronization assertions did not run."));
        return true;
    }

    int32 MergesApplied = 0;
    RefreshCapture.Result->TryGetNumberField(TEXT("mergesApplied"), MergesApplied);
    TestEqual(TEXT("exactly one handle was merged"), MergesApplied, 1);

    bool bSynchronizedAfter = false;
    (*Entry)->TryGetBoolField(TEXT("synchronizedWithParent"), bSynchronizedAfter);
    TestTrue(TEXT("the handle is up to date with its parent after the merge"), bSynchronizedAfter);

    // The assertion the whole ticket is about: the edit made to the asset AFTER it was wired in
    // now reaches the system, without a remove+add and without discarding the system's own copy.
    TestEqual(TEXT("the parent's new renderer merged into the system's copy"),
        NiagaraInheritanceTest_RendererCountOfFirstHandle(*System), RenderersBefore + 1);

    return true;
}

// 4. A system with nothing to merge from is an error, not a zero-item success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRefreshEmitterRefusesWithoutParentTest,
    "PinWright.niagara.refresh_emitter.RefusesWhenNothingInherits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRefreshEmitterRefusesWithoutParentTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    FString EmitterPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bFixtureReady = NiagaraInheritanceTest_MakeFixture(*this, SystemPath, System, EmitterPath, Emitter);
    Roots.System = System;
    Roots.Emitter = Emitter;
    if (!bFixtureReady)
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = NiagaraInheritanceTest_AddPayload(SystemPath, EmitterPath);
    AddPayload->SetBoolField(TEXT("inherit"), false);
    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), AddPayload, AddCapture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RefreshPayload = MakeShared<FJsonObject>();
    RefreshPayload->SetStringField(TEXT("systemPath"), SystemPath);
    NiagaraEditTestUtils::InvokeExpectError(
        *this, TEXT("niagara.refresh_emitter"), RefreshPayload, TEXT("EMITTER_NOT_INHERITED"));

    return true;
}

// 5. A named snapshot gets an actionable typed refusal before the handler mutates the system.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRefreshEmitterRefusesNamedSnapshotHandleTest,
    "PinWright.niagara.refresh_emitter.RefusesNamedSnapshotHandle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRefreshEmitterRefusesNamedSnapshotHandleTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    FString EmitterPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bFixtureReady = NiagaraInheritanceTest_MakeFixture(*this, SystemPath, System, EmitterPath, Emitter);
    Roots.System = System;
    Roots.Emitter = Emitter;
    if (!bFixtureReady)
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = NiagaraInheritanceTest_AddPayload(SystemPath, EmitterPath);
    AddPayload->SetBoolField(TEXT("inherit"), false);
    FTestResponseCapture AddCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_emitter"), AddPayload, AddCapture))
    {
        return false;
    }

    const FNiagaraEmitterHandle& SnapshotHandle = System->GetEmitterHandles()[0];
    const FGuid SnapshotHandleGuid = SnapshotHandle.GetId();
    const FName SnapshotHandleName = SnapshotHandle.GetName();
    const FString SnapshotName = SnapshotHandle.GetName().ToString();
    const FString SnapshotId = SnapshotHandleGuid.ToString();
    const bool bSnapshotEnabled = SnapshotHandle.GetIsEnabled();
    const FVersionedNiagaraEmitter SnapshotInstanceVersioned = SnapshotHandle.GetInstance();
    const UNiagaraEmitter* SnapshotInstance = SnapshotInstanceVersioned.Emitter;
    FVersionedNiagaraEmitterData* SnapshotData = SnapshotHandle.GetEmitterData();
    const UNiagaraEmitter* SnapshotSource = SnapshotData ? SnapshotData->GetParent().Emitter : nullptr;
    const FGuid SnapshotDataVersionGuid = SnapshotData ? SnapshotData->Version.VersionGuid : FGuid();
    const FGuid SnapshotChangeId = SnapshotInstance ? SnapshotInstance->GetChangeId() : FGuid();
    TestNotNull(TEXT("the snapshot has emitter data"), SnapshotData);
    TestNotNull(TEXT("the snapshot has an instance emitter"), SnapshotInstance);

    TSharedPtr<FJsonObject> RefreshPayload = MakeShared<FJsonObject>();
    RefreshPayload->SetStringField(TEXT("systemPath"), SystemPath);
    RefreshPayload->SetStringField(TEXT("emitter"), SnapshotName);
    RefreshPayload->SetBoolField(TEXT("compile"), false);
    RefreshPayload->SetBoolField(TEXT("save"), false);

    UTransBuffer* TransBuffer = (GEditor && GEditor->Trans)
        ? Cast<UTransBuffer>(GEditor->Trans.Get())
        : nullptr;
    if (!GEditor || !GEditor->Trans || !TransBuffer || GEditor->Trans->IsActive() || !GEditor->CanTransact())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-idle-editor-transactor"),
            TEXT("The transaction-start observation requires an idle, transact-capable UTransBuffer."));
        return true;
    }

    int32 TransactionStartedCount = 0;
    const FDelegateHandle TransactionStateHandle =
        TransBuffer->OnTransactionStateChanged().AddLambda(
            [&TransactionStartedCount](const FTransactionContext&, ETransactionStateEventType EventType)
            {
                if (EventType == ETransactionStateEventType::TransactionStarted)
                {
                    ++TransactionStartedCount;
                }
            });
    ON_SCOPE_EXIT
    {
        TransBuffer->OnTransactionStateChanged().Remove(TransactionStateHandle);
    };

    const int32 TransactionQueueBefore = GEditor->Trans->GetQueueLength();
    FTestResponseCapture RefreshCapture;
    TestTrue(TEXT("niagara.refresh_emitter handler found"),
        InvokeHandlerWithCapture(TEXT("niagara.refresh_emitter"), RefreshPayload, RefreshCapture));
    const int32 TransactionQueueAfter = GEditor->Trans->GetQueueLength();
    TestEqual(TEXT("the rejected refresh starts no transaction"), TransactionStartedCount, 0);
    TestEqual(TEXT("the rejected refresh leaves the transaction queue unchanged"),
        TransactionQueueAfter, TransactionQueueBefore);
    TestTrue(TEXT("niagara.refresh_emitter sent a response"), RefreshCapture.bWasCalled);
    TestFalse(TEXT("the named snapshot refresh is rejected"), RefreshCapture.bSuccess);
    TestEqual(TEXT("the named snapshot reports EMITTER_NOT_INHERITED"),
        RefreshCapture.ErrorCode, FString(TEXT("EMITTER_NOT_INHERITED")));
    TestTrue(TEXT("the refusal names the actual handle"), RefreshCapture.Message.Contains(SnapshotName));
    TestTrue(TEXT("the refusal includes the inherit:true migration path"),
        RefreshCapture.Message.Contains(TEXT("inherit:true")));
    TestTrue(TEXT("the refusal explains the missing source identity"),
        RefreshCapture.Message.Contains(TEXT("source identity")));

    TestNotNull(TEXT("the refusal carries top-level error data"), RefreshCapture.Result.Get());
    if (RefreshCapture.Result.IsValid())
    {
        FString ErrorEmitter;
        FString ErrorHandleId;
        TestTrue(TEXT("error data carries emitter"),
            RefreshCapture.Result->TryGetStringField(TEXT("emitter"), ErrorEmitter));
        TestEqual(TEXT("error data names the selected handle"), ErrorEmitter, SnapshotName);
        TestTrue(TEXT("error data carries handleId"),
            RefreshCapture.Result->TryGetStringField(TEXT("handleId"), ErrorHandleId));
        TestEqual(TEXT("error data names the selected handle Guid"), ErrorHandleId, SnapshotId);
    }

    if (!TestEqual(TEXT("the rejected refresh keeps one emitter handle"), System->GetEmitterHandles().Num(), 1))
    {
        return false;
    }
    const FNiagaraEmitterHandle& AfterHandle = System->GetEmitterHandles()[0];
    TestTrue(TEXT("the rejected refresh preserves the handle id"), AfterHandle.GetId() == SnapshotHandleGuid);
    TestTrue(TEXT("the rejected refresh preserves the handle name"), AfterHandle.GetName() == SnapshotHandleName);
    TestEqual(TEXT("the rejected refresh preserves the enabled flag"),
        AfterHandle.GetIsEnabled(), bSnapshotEnabled);
    const FVersionedNiagaraEmitter AfterInstanceVersioned = AfterHandle.GetInstance();
    FVersionedNiagaraEmitterData* AfterData = AfterHandle.GetEmitterData();
    const UNiagaraEmitter* AfterSource = AfterData ? AfterData->GetParent().Emitter : nullptr;
    const FGuid AfterChangeId = AfterInstanceVersioned.Emitter
        ? AfterInstanceVersioned.Emitter->GetChangeId()
        : FGuid();
    TestTrue(TEXT("the rejected refresh preserves the instance emitter pointer"),
        AfterInstanceVersioned.Emitter == SnapshotInstance);
    TestTrue(TEXT("the rejected refresh preserves the instance emitter version"),
        AfterInstanceVersioned.Version == SnapshotInstanceVersioned.Version);
    TestTrue(TEXT("the rejected refresh preserves the source emitter pointer"), AfterSource == SnapshotSource);
    TestTrue(TEXT("the rejected refresh preserves the emitter data identity"), AfterData == SnapshotData);
    if (AfterData)
    {
        TestTrue(TEXT("the rejected refresh preserves the emitter data version"),
            AfterData->Version.VersionGuid == SnapshotDataVersionGuid);
    }
    TestTrue(TEXT("the rejected refresh preserves the emitter change id"), AfterChangeId == SnapshotChangeId);
    return true;
}
