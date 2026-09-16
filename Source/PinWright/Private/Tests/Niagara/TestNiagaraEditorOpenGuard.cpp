// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-edit-with-open-asset-editor-slate-crash.
//
// niagara.add_emitter / niagara.remove_emitter reshape a UNiagaraSystem's emitter-handle array
// through a plain LoadObject. While an asset editor holds that system open, the toolkit's live
// widgets still reference the handles the mutation frees, and the next ordinary Slate redraw
// dereferences them (SNiagaraOverviewGraphTitleBar::IsUsingDeprecatedEmitter ->
// UNiagaraEmitter::GetEmitterData, EXCEPTION_ACCESS_VIOLATION). The fault lands in a redraw, so
// the mutating call had already reported success and nothing in its response could report it.
// The fix refuses the mutation with EDITOR_OPEN while an editor is open.
//
// THIS TEST ASSERTS THE GUARD, NOT THE CRASH, and it is built so it cannot provoke the crash even
// if the guard is reverted: the "open editor" is a stub IAssetEditorInstance registered with
// UAssetEditorSubsystem::NotifyAssetOpened, so no Slate widget exists to read a freed handle. A
// reverted guard therefore fails the EDITOR_OPEN assertions instead of killing the suite host.
//
// Counterfactual: with the RejectStructuralEditWhileAssetEditorOpen calls removed from
// NiagaraHandler.cpp, both verbs return success while the stub editor is registered and the
// handle count moves, so the four "refused" assertions and the two "count unchanged" assertions
// fail. The trailing editor-closed control proves the refusal came from the guard and not from a
// broken fixture: the identical calls must succeed once the editor is unregistered.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Handlers/Niagara/NiagaraEditorOpenGuard.h"

#include "Compat/EngineVersionCompat.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace NiagaraEditorOpenGuardTest
{
    // The smallest thing UAssetEditorSubsystem will accept as an open editor. Registering one is
    // what makes FindEditorForAsset (and therefore the production guard) answer "open" without
    // spawning FNiagaraSystemToolkit, which in a headless suite run would both cost a shader
    // compile and re-create the crash the guard exists to prevent.
    class FStubNiagaraAssetEditorInstance : public IAssetEditorInstance
    {
    public:
        virtual FName GetEditorName() const override { return FName(TEXT("PinWrightStubAssetEditor")); }
        virtual void FocusWindow(UObject* ObjectToFocusOn) override {}
        virtual bool IsPrimaryEditor() const override { return true; }
        virtual void InvokeTab(const struct FTabId& TabId) override {}
        virtual TSharedPtr<class FTabManager> GetAssociatedTabManager() override { return nullptr; }
        virtual double GetLastActivationTime() override { return 0.0; }
        virtual void RemoveEditingAsset(UObject* Asset) override {}
        // Keeps NotifyAssetOpened from routing this fixture into the restore-open-assets config.
        // The UObject*-taking overload arrived in UE 5.4 (and deprecated the no-arg one there), so
        // each engine gets exactly the spelling it declares — overriding the other would either
        // not override anything (5.3) or trip the deprecation warning the Fab gate rejects (5.4+).
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        virtual bool IncludeAssetInRestoreOpenAssetsPrompt() const override { return false; }
#else
        virtual bool IncludeAssetInRestoreOpenAssetsPrompt(UObject* Asset) const override { return false; }
#endif
    };

    // Registers the stub against one asset and guarantees it is unregistered again, so a failed
    // assertion mid-test cannot leave a dangling editor entry behind for the rest of the suite.
    struct FScopedStubAssetEditor
    {
        UAssetEditorSubsystem* Subsystem = nullptr;
        FStubNiagaraAssetEditorInstance Instance;
        bool bRegistered = false;

        explicit FScopedStubAssetEditor(UObject* Asset)
        {
            if (!GEditor || !Asset)
            {
                return;
            }
            Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            if (Subsystem)
            {
                Subsystem->NotifyAssetOpened(Asset, &Instance);
                bRegistered = true;
            }
        }

        void Close()
        {
            if (bRegistered)
            {
                Subsystem->NotifyEditorClosed(&Instance);
                bRegistered = false;
            }
        }

        ~FScopedStubAssetEditor() { Close(); }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEditorOpenGuardTest,
    "PinWright.niagara.editor_open_guard.StructuralEditsRefuseWhileAssetEditorOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEditorOpenGuardTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName ExistingEmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(
        SystemPath, System, SourceEmitter, ExistingEmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    // A second emitter asset, so add_emitter has something to add that is not already a handle.
    FString ProbeEmitterPath;
    UNiagaraEmitter* ProbeEmitter = NiagaraEditTestUtils::NewTransientEmitter(ProbeEmitterPath);
    NiagaraEditTestUtils::FAuthorableSystemRoots ProbeRoots{nullptr, ProbeEmitter};
    if (!TestNotNull(TEXT("probe emitter created"), ProbeEmitter))
    {
        return false;
    }

    const int32 BaselineHandleCount = System->GetEmitterHandles().Num();
    TestEqual(TEXT("fixture starts with one emitter handle"), BaselineHandleCount, 1);

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("systemPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitterPath"), ProbeEmitterPath);
    AddPayload->SetStringField(TEXT("name"), TEXT("GuardProbeEmitter"));
    AddPayload->SetBoolField(TEXT("compile"), false);
    AddPayload->SetBoolField(TEXT("save"), false);

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("systemPath"), SystemPath);
    RemovePayload->SetStringField(TEXT("emitter"), ExistingEmitterName.ToString());
    RemovePayload->SetBoolField(TEXT("compile"), false);
    RemovePayload->SetBoolField(TEXT("save"), false);

    {
        NiagaraEditorOpenGuardTest::FScopedStubAssetEditor OpenEditor(System);

        // The guard keys off the asset, so this precondition is the whole premise of the test:
        // if the stub did not register, every refusal below would be vacuous.
        if (!TestTrue(TEXT("an asset editor is registered against the system"),
                PinWrightNiagara::IsNiagaraAssetEditorOpen(System)))
        {
            return false;
        }

        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.add_emitter"), AddPayload, TEXT("EDITOR_OPEN"));
        TestEqual(TEXT("add_emitter appended no handle while the editor was open"),
            System->GetEmitterHandles().Num(), BaselineHandleCount);

        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.remove_emitter"), RemovePayload, TEXT("EDITOR_OPEN"));
        TestEqual(TEXT("remove_emitter removed no handle while the editor was open"),
            System->GetEmitterHandles().Num(), BaselineHandleCount);
    }

    // Control: with nothing holding the asset open, the identical calls must go through. Without
    // this the test cannot tell a working guard from a fixture that fails both verbs for some
    // unrelated reason.
    if (!TestFalse(TEXT("no asset editor remains registered after the stub is closed"),
            PinWrightNiagara::IsNiagaraAssetEditorOpen(System)))
    {
        return false;
    }

    FTestResponseCapture AddCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.add_emitter"), AddPayload, AddCapture))
    {
        TestEqual(TEXT("add_emitter appended a handle once the editor was closed"),
            System->GetEmitterHandles().Num(), BaselineHandleCount + 1);
    }

    FTestResponseCapture RemoveCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.remove_emitter"), RemovePayload, RemoveCapture))
    {
        TestEqual(TEXT("remove_emitter removed a handle once the editor was closed"),
            System->GetEmitterHandles().Num(), BaselineHandleCount);
    }

    return true;
}

// ---------------------------------------------------------------------------
// B-niagara-editor-open-guard-missing-on-mutators
//
// The guard was deliberately NOT blanket-applied to the remaining niagara.* mutators. Almost all of
// them announce their change on a delegate the open toolkit already listens to
// (UNiagaraEmitter::OnRenderersChanged / OnSimStagesChanged, FNiagaraParameterStore::OnLayoutChange,
// UEdGraph::RemoveNode's GRAPHACTION_RemoveNode, UNiagaraSystem::PostEditChangeProperty ->
// FNiagaraSystemViewModel::RefreshAll), and the stack entries that survive hold TWeakObjectPtr.
// Two families did not, and they are what the two tests below pin:
//
//   * add_event_handler / remove_event_handler -- UNiagaraStackEventHandlerPropertiesItem snapshots
//     EventHandlerScriptProps into a UNiagaraStackEventWrapper once and writes the whole snapshot
//     back from that wrapper's PostEditChangeProperty. Nothing re-takes it, so an out-of-band write
//     is reported as succeeding and then silently reverted; EDITOR_OPEN is the only honest outcome.
//   * reset_module_input / clear_module_overrides -- these MarkAsGarbage() an override UEdGraphPin,
//     and UEdGraphNode::RemovePin's only notification is NotifyGraphNeedsRecompile, which
//     UNiagaraGraph::NotifyGraphChanged early-returns on without ever broadcasting OnGraphChanged.
//     Here a notification DOES fix it (it drops UNiagaraStackFunctionInput::OverridePinCache and
//     invalidates SGraphPanel's pin widgets), so those two send it rather than refusing the edit.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEventHandlerEditorOpenGuardTest,
    "PinWright.niagara.editor_open_guard.EventHandlerMutatorsRefuseWhileAssetEditorOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEventHandlerEditorOpenGuardTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    const bool bSetupOk = NiagaraEditTestUtils::MakeAuthorableSystem(
        SystemPath, System, SourceEmitter, EmitterName);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots{System, SourceEmitter};
    if (!TestTrue(TEXT("authorable system created"), bSetupOk))
    {
        return false;
    }

    // Count off the system's own emitter handle, not the source asset: AddEmitterHandle takes its
    // own instance and the RPC edits that one.
    auto CountEventHandlers = [](UNiagaraSystem* InSystem) -> int32
    {
        for (const FNiagaraEmitterHandle& Handle : InSystem->GetEmitterHandles())
        {
            if (UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter)
            {
                if (FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData())
                {
                    return Data->EventHandlerScriptProps.Num();
                }
            }
        }
        return INDEX_NONE;
    };

    const int32 BaselineHandlers = CountEventHandlers(System);
    if (!TestTrue(TEXT("fixture emitter data is readable"), BaselineHandlers != INDEX_NONE))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), SystemPath);
    AddPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
    AddPayload->SetStringField(TEXT("executionMode"), TEXT("SpawnedParticles"));
    AddPayload->SetNumberField(TEXT("spawnNumber"), 3);
    AddPayload->SetStringField(TEXT("sourceEventName"), TEXT("Collision"));
    AddPayload->SetBoolField(TEXT("compile"), false);
    AddPayload->SetBoolField(TEXT("save"), false);

    {
        NiagaraEditorOpenGuardTest::FScopedStubAssetEditor OpenEditor(System);
        if (!TestTrue(TEXT("an asset editor is registered against the system"),
                PinWrightNiagara::IsNiagaraAssetEditorOpen(System)))
        {
            return false;
        }

        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.add_event_handler"), AddPayload, TEXT("EDITOR_OPEN"));
        TestEqual(TEXT("add_event_handler appended no handler while the editor was open"),
            CountEventHandlers(System), BaselineHandlers);

        TSharedPtr<FJsonObject> RemoveHandlerPayload = MakeShared<FJsonObject>();
        RemoveHandlerPayload->SetStringField(TEXT("assetPath"), SystemPath);
        RemoveHandlerPayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
        RemoveHandlerPayload->SetNumberField(TEXT("eventHandlerIndex"), 0);
        RemoveHandlerPayload->SetBoolField(TEXT("compile"), false);
        RemoveHandlerPayload->SetBoolField(TEXT("save"), false);
        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.remove_event_handler"), RemoveHandlerPayload, TEXT("EDITOR_OPEN"));

        // The half of this ticket a blanket apply would get wrong. A sibling structural verb in the
        // same handler file, on the same open asset, must still go through: its engine mutator
        // broadcasts OnSimStagesChanged and the stack holds the stage by TWeakObjectPtr, so
        // refusing it would cost the caller an edit and prevent nothing. Asserted as "not
        // EDITOR_OPEN" rather than "succeeded" so an unrelated fixture problem in that verb cannot
        // masquerade as the guard having spread.
        TSharedPtr<FJsonObject> StagePayload = MakeShared<FJsonObject>();
        StagePayload->SetStringField(TEXT("assetPath"), SystemPath);
        StagePayload->SetStringField(TEXT("emitter"), EmitterName.ToString());
        StagePayload->SetBoolField(TEXT("compile"), false);
        StagePayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture StageCapture;
        FString StageMethod(TEXT("niagara.add_simulation_stage"));
        TestTrue(TEXT("niagara.add_simulation_stage handler found"),
            InvokeHandlerWithCapture(StageMethod, StagePayload, StageCapture));
        TestNotEqual(TEXT("add_simulation_stage is not refused by the open-editor guard"),
            StageCapture.ErrorCode, FString(TEXT("EDITOR_OPEN")));
    }

    // Control: the refusals above have to come from the guard, not from a broken fixture.
    if (!TestFalse(TEXT("no asset editor remains registered after the stub is closed"),
            PinWrightNiagara::IsNiagaraAssetEditorOpen(System)))
    {
        return false;
    }

    FTestResponseCapture ReopenedAddCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.add_event_handler"), AddPayload, ReopenedAddCapture))
    {
        TestEqual(TEXT("add_event_handler appended a handler once the editor was closed"),
            CountEventHandlers(System), BaselineHandlers + 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraClearModuleOverridesGraphNotifyTest,
    "PinWright.niagara.clear_module_overrides.BroadcastsGraphChangedForOpenEditorCaches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraClearModuleOverridesGraphNotifyTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(
        nullptr, TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"));
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), ModuleScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("NotifyProbe")));
    if (!TestNotNull(TEXT("transient system with one emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::EmitterUpdateScript, ModuleScript);
    if (!TestNotNull(TEXT("module added to the EmitterUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    UEdGraph* Graph = ModuleNode->GetGraph();
    if (!TestNotNull(TEXT("module node has an owning graph"), Graph))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // OnGraphChanged is the delegate UNiagaraStackFunctionInput::OnGraphChanged (which drops the
    // raw OverridePinCache) and SGraphPanel::OnGraphChanged (which invalidates its pin widgets)
    // are bound to. Counting it here counts exactly what an open toolkit would receive.
    int32 GraphChangedCount = 0;
    const FDelegateHandle ChangedHandle = Graph->AddOnGraphChangedHandler(
        FOnGraphChanged::FDelegate::CreateLambda(
            [&GraphChangedCount](const FEdGraphEditAction&) { ++GraphChangedCount; }));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("emitter"), TEXT("NotifyProbe"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this, TEXT("niagara.clear_module_overrides"), Payload, Capture);

    // Counterfactual: before the fix the verb ended at NotifyNiagaraObjectChanged, whose only reach
    // into the graph is UNiagaraNodeFunctionCall::PostEditChangeProperty ->
    // MarkNodeRequiresSynchronization -> NotifyGraphNeedsRecompile, and
    // UNiagaraGraph::NotifyGraphChanged returns on GRAPHACTION_GenericNeedsRecompile before
    // Super::NotifyGraphChanged ever broadcasts. This count was 0, and any override pin the verb
    // had just MarkAsGarbage()d stayed live in an open toolkit's caches.
    if (bSucceeded)
    {
        TestTrue(TEXT("clear_module_overrides broadcast OnGraphChanged at least once"),
            GraphChangedCount > 0);
    }

    Graph->RemoveOnGraphChangedHandler(ChangedHandle);
    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// B-niagara-edits-lost-while-emitter-toolkit-open
//
// The two tests above pin PER-VERB guards. This one pins the ASSET-KIND guard that sits in
// NiagaraEdit::ResolveTarget, the single site every niagara.* write funnels through.
//
// An emitter toolkit never edits the emitter asset it was opened on:
// FNiagaraSystemToolkit::InitializeWithEmitter builds a transient UNiagaraSystem and calls
// FNiagaraSystemViewModel::AddEmitter, which copies the emitter ("Adding the emitter to the system
// has made a copy of it" is the engine's own comment), and
// FNiagaraSystemToolkit::UpdateOriginalEmitter then re-duplicates that copy back over the original
// on Apply -- StaticDuplicateObject into Source->GetOuter() under Source->GetFName(), i.e. an
// overwrite, never a merge. So a write reaching the ORIGINAL emitter asset while its toolkit is
// open is invisible to the toolkit and is destroyed by the next Apply, having already reported
// success.
//
// Counterfactual: with the RefuseEmitterAssetEditWhileToolkitOpen block removed from ResolveTarget,
// both verbs below return success against the open emitter asset, so their EDITOR_OPEN assertions
// and both "unchanged" assertions fail.
//
// Two verbs, deliberately: niagara.set_property resolves through ValidatePropertyPayload in
// NiagaraEditHandler.cpp and niagara.add_simulation_stage through ResolveEmitterTarget in
// NiagaraAdvancedEditHandler.cpp, and add_simulation_stage is a verb the per-verb guard
// deliberately exempts (the test above asserts it must NOT be refused on an open SYSTEM). Both
// being refused here is what shows the condition belongs to the asset kind rather than to any one
// verb.
//
// The system control at the end is the other half: the guard must not spread to Niagara Systems,
// whose toolkit edits the asset itself rather than a duplicate.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraEmitterAssetToolkitOpenGuardTest,
    "PinWright.niagara.editor_open_guard.EmitterAssetWritesRefuseWhileToolkitOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEmitterAssetToolkitOpenGuardTest::RunTest(const FString& Parameters)
{
    FString EmitterPath;
    UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterPath);
    NiagaraEditTestUtils::FAuthorableSystemRoots EmitterRoots{nullptr, Emitter};
    if (!TestNotNull(TEXT("emitter asset created"), Emitter))
    {
        return false;
    }

    FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
    if (!TestNotNull(TEXT("emitter asset exposes versioned emitter data"), EmitterData))
    {
        return false;
    }

    // bLocalSpace is a plain UPROPERTY on FVersionedNiagaraEmitterData, so where the write landed
    // is observable without a compile or a save.
    const bool bBaselineLocalSpace = EmitterData->bLocalSpace;
    const int32 BaselineStages = EmitterData->GetSimulationStages().Num();

    TSharedPtr<FJsonObject> PropertyTarget = MakeShared<FJsonObject>();
    PropertyTarget->SetStringField(TEXT("kind"), TEXT("emitterData"));

    TSharedPtr<FJsonObject> PropertyPayload = MakeShared<FJsonObject>();
    PropertyPayload->SetStringField(TEXT("assetPath"), EmitterPath);
    PropertyPayload->SetObjectField(TEXT("target"), PropertyTarget);
    PropertyPayload->SetStringField(TEXT("propertyPath"), TEXT("bLocalSpace"));
    PropertyPayload->SetBoolField(TEXT("value"), !bBaselineLocalSpace);
    PropertyPayload->SetBoolField(TEXT("compile"), false);
    PropertyPayload->SetBoolField(TEXT("save"), false);

    TSharedPtr<FJsonObject> StagePayload = MakeShared<FJsonObject>();
    StagePayload->SetStringField(TEXT("assetPath"), EmitterPath);
    StagePayload->SetBoolField(TEXT("compile"), false);
    StagePayload->SetBoolField(TEXT("save"), false);

    {
        NiagaraEditorOpenGuardTest::FScopedStubAssetEditor OpenEditor(Emitter);

        // Premise of the whole test: without a registered editor every refusal below is vacuous.
        if (!TestTrue(TEXT("an asset editor is registered against the emitter asset"),
                PinWrightNiagara::IsNiagaraAssetEditorOpen(Emitter)))
        {
            return false;
        }

        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.set_property"), PropertyPayload, TEXT("EDITOR_OPEN"));
        TestEqual(TEXT("set_property left bLocalSpace alone while the emitter toolkit was open"),
            Emitter->GetLatestEmitterData()->bLocalSpace, bBaselineLocalSpace);

        NiagaraEditTestUtils::InvokeExpectError(
            *this, TEXT("niagara.add_simulation_stage"), StagePayload, TEXT("EDITOR_OPEN"));
        TestEqual(TEXT("add_simulation_stage added no stage while the emitter toolkit was open"),
            Emitter->GetLatestEmitterData()->GetSimulationStages().Num(), BaselineStages);
    }

    // Control: the refusals have to come from the guard, not from a fixture that fails both verbs
    // for some unrelated reason.
    if (!TestFalse(TEXT("no asset editor remains registered after the stub is closed"),
            PinWrightNiagara::IsNiagaraAssetEditorOpen(Emitter)))
    {
        return false;
    }

    FTestResponseCapture PropertyCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(
            *this, TEXT("niagara.set_property"), PropertyPayload, PropertyCapture))
    {
        TestEqual(TEXT("set_property flipped bLocalSpace once the emitter toolkit was closed"),
            Emitter->GetLatestEmitterData()->bLocalSpace, !bBaselineLocalSpace);
    }

    // Asserted as "not refused" rather than "succeeded": this verb only has to show the guard
    // stopped answering, and an unrelated fixture problem inside it must not read as the guard
    // still firing.
    FTestResponseCapture ReopenedStageCapture;
    FString StageMethod(TEXT("niagara.add_simulation_stage"));
    TestTrue(TEXT("niagara.add_simulation_stage handler found"),
        InvokeHandlerWithCapture(StageMethod, StagePayload, ReopenedStageCapture));
    TestNotEqual(TEXT("add_simulation_stage is no longer refused once the emitter toolkit is closed"),
        ReopenedStageCapture.ErrorCode, FString(TEXT("EDITOR_OPEN")));

    // The other half of "asset-kind": a Niagara System toolkit edits the system asset itself, so
    // this guard must not reach it. bFixedBounds is the property the existing set_property system
    // test uses, so a failure here is the guard spreading rather than a property-resolution problem.
    FString SystemPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemPath);
    NiagaraEditTestUtils::FAuthorableSystemRoots SystemRoots{System, nullptr};
    if (!TestNotNull(TEXT("system asset created"), System))
    {
        return false;
    }

    TSharedPtr<FJsonObject> SystemTarget = MakeShared<FJsonObject>();
    SystemTarget->SetStringField(TEXT("kind"), TEXT("system"));

    TSharedPtr<FJsonObject> SystemPayload = MakeShared<FJsonObject>();
    SystemPayload->SetStringField(TEXT("assetPath"), SystemPath);
    SystemPayload->SetObjectField(TEXT("target"), SystemTarget);
    SystemPayload->SetStringField(TEXT("propertyPath"), TEXT("bFixedBounds"));
    SystemPayload->SetBoolField(TEXT("value"), !static_cast<bool>(System->bFixedBounds));
    SystemPayload->SetBoolField(TEXT("compile"), false);
    SystemPayload->SetBoolField(TEXT("save"), false);

    {
        NiagaraEditorOpenGuardTest::FScopedStubAssetEditor OpenSystemEditor(System);
        if (!TestTrue(TEXT("an asset editor is registered against the system asset"),
                PinWrightNiagara::IsNiagaraAssetEditorOpen(System)))
        {
            return false;
        }

        FTestResponseCapture SystemCapture;
        FString PropertyMethod(TEXT("niagara.set_property"));
        TestTrue(TEXT("niagara.set_property handler found"),
            InvokeHandlerWithCapture(PropertyMethod, SystemPayload, SystemCapture));
        TestNotEqual(TEXT("the emitter-asset guard does not refuse writes to an open system asset"),
            SystemCapture.ErrorCode, FString(TEXT("EDITOR_OPEN")));
    }

    return true;
}
