// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-entry-id-not-unique-across-emitters.
//
// The stack dump's `entryId` is the raw UEdGraphNode::NodeGuid. Duplicating a Niagara emitter
// copies its graph verbatim, NodeGuids included, so emitters descended from one template carry
// byte-identical entryIds for their corresponding modules. The module resolver is emitter-scoped,
// which makes a bare id addressable only in company with the right `emitter`: paired with the
// wrong one it used to resolve that other emitter's identically-keyed module, mutate it, and echo
// the requested id back unchanged — a wrong-target write indistinguishable from a correct one.
//
// The fix leaves `entryId` a bare NodeGuid (its value is a contract — every mutation verb echoes
// it and TestNiagaraSetModuleInput asserts the round-trip) and adds the owner-qualified
// `entryKey`, "<ownerName>:<nodeGuid>", which the module verbs accept wherever they accept
// `entryId`. A qualified key names the emitter that scopes it, so it selects that emitter when
// none was passed and is refused (MODULE_OWNER_MISMATCH) against any other.
//
// Counterfactual: revert NiagaraDumpBuilder's `entryKey` line and the first test's two colliding
// entries become indistinguishable; revert the ParseTargetSpec split / ResolveTarget owner
// handling and the second test's qualified id resolves against the system graph instead
// (MODULE_NOT_FOUND) and the wrong-emitter pairing succeeds against emitter A.
//
// Fixture: one transient system with two emitter handles duplicated from the same source emitter,
// each carrying a SpawnRate module in its EmitterUpdate stack, with the second module's NodeGuid
// forced to equal the first's. Forcing it makes the collision the ticket measured on a duplicated
// SimpleExplosion deterministic instead of depending on how DuplicateObject happens to treat
// NodeGuid on the engine under test.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraSystemViewModelCache.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
    const TCHAR* SpawnRateModulePathForEntryIdTest = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    const TCHAR* SpawnRateInputNameForEntryIdTest = TEXT("SpawnRate");

    // The graph behind one specific emitter handle. TestNIRFixtures' GetFirstEmitterGraph only
    // ever reaches handle 0, which is exactly the emitter this fixture needs a second of.
    UNiagaraGraph* FindEmitterStackGraph(const FNiagaraEmitterHandle& Handle)
    {
        FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        UNiagaraScriptSource* Source = EmitterData
            ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource)
            : nullptr;
        return Source ? Source->NodeGraph : nullptr;
    }

    // Same AddScriptModuleToStack call the niagara.add_module handler and
    // NIRTestFixtures::AddModuleToStack make, aimed at a named handle's EmitterUpdate output.
    UNiagaraNodeFunctionCall* AddModuleToEmitterHandleStack(
        const FNiagaraEmitterHandle& Handle,
        UNiagaraScript* ModuleScript)
    {
        UNiagaraGraph* Graph = FindEmitterStackGraph(Handle);
        if (!Graph || !ModuleScript)
        {
            return nullptr;
        }
        UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(
            ENiagaraScriptUsage::EmitterUpdateScript,
            FGuid());
        if (!OutputNode)
        {
            return nullptr;
        }
        return FNiagaraStackGraphUtilities::AddScriptModuleToStack(
            ModuleScript,
            *OutputNode,
            INDEX_NONE,
            ModuleScript->GetName());
    }

    // "default" when the input still reads its script-declared default, "local" once an override
    // literal has been written. Reading it off the module node itself is what proves which of the
    // two identically-keyed modules a mutation landed on.
    FString ReadModuleInputValueMode(const UNiagaraNodeFunctionCall* ModuleNode, const TCHAR* InputName)
    {
        for (const TSharedPtr<FJsonValue>& Value : NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode))
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Name;
            if (Entry.IsValid() && Entry->TryGetStringField(TEXT("name"), Name) && Name == InputName)
            {
                FString ValueMode;
                Entry->TryGetStringField(TEXT("valueMode"), ValueMode);
                return ValueMode;
            }
        }
        return FString();
    }

    struct FCollidingEntryIdFixture
    {
        UNiagaraSystem* System = nullptr;
        UNiagaraNodeFunctionCall* ModuleA = nullptr;
        UNiagaraNodeFunctionCall* ModuleB = nullptr;
        FString OwnerA;
        FString OwnerB;
        FString EntryId;
    };

    bool BuildCollidingEntryIdFixture(FCollidingEntryIdFixture& Out)
    {
        FString IgnoredObjectPath;
        Out.System = NiagaraEditTestUtils::NewTransientSystem(IgnoredObjectPath);
        if (!Out.System)
        {
            return false;
        }

        FString EmitterPathA;
        FString EmitterPathB;
        UNiagaraEmitter* EmitterA = NiagaraEditTestUtils::NewTransientEmitter(EmitterPathA);
        UNiagaraEmitter* EmitterB = NiagaraEditTestUtils::NewTransientEmitter(EmitterPathB);
        if (!EmitterA || !EmitterB)
        {
            return false;
        }

        Out.System->AddEmitterHandle(*EmitterA, FName(TEXT("CollidingA")), EmitterA->GetExposedVersion().VersionGuid);
        Out.System->AddEmitterHandle(*EmitterB, FName(TEXT("CollidingB")), EmitterB->GetExposedVersion().VersionGuid);

        // Prime the SVM once both handles exist, so the stack-edit utilities see the full system.
        PinWrightNiagara::AcquireSystemViewModel(*Out.System);

        UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, SpawnRateModulePathForEntryIdTest);
        if (!ModuleScript)
        {
            return false;
        }

        const TArray<FNiagaraEmitterHandle>& Handles = Out.System->GetEmitterHandles();
        if (Handles.Num() != 2)
        {
            return false;
        }
        Out.OwnerA = Handles[0].GetName().ToString();
        Out.OwnerB = Handles[1].GetName().ToString();
        Out.ModuleA = AddModuleToEmitterHandleStack(Handles[0], ModuleScript);
        Out.ModuleB = AddModuleToEmitterHandleStack(Handles[1], ModuleScript);
        if (!Out.ModuleA || !Out.ModuleB)
        {
            return false;
        }

        // The collision under test. Emitter duplication reproduces it for real; the two modules
        // here were added fresh, so reproduce it explicitly rather than relying on guid reuse.
        Out.ModuleB->NodeGuid = Out.ModuleA->NodeGuid;
        Out.EntryId = Out.ModuleA->NodeGuid.ToString();
        return true;
    }
}

// ---------------------------------------------------------------------------
// Read side — one entryId under two emitters, told apart by entryKey.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraEntryIdStackDumpQualifiesOwnerTest,
    "PinWright.niagara.entry_id.StackDumpQualifiesOwner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEntryIdStackDumpQualifiesOwnerTest::RunTest(const FString& Parameters)
{
    FCollidingEntryIdFixture Fixture;
    const bool bBuilt = BuildCollidingEntryIdFixture(Fixture);
    if (!TestTrue(TEXT("two-emitter colliding-entryId fixture built"), bBuilt))
    {
        NIRTestFixtures::DestroyFixture(Fixture.System);
        return false;
    }

    const TSharedPtr<FJsonObject> Stack = NiagaraDumpBuilder::BuildStackJson(Fixture.System);
    if (!TestTrue(TEXT("stack dump produced a modules array"), Stack.IsValid() && Stack->HasField(TEXT("modules"))))
    {
        NIRTestFixtures::DestroyFixture(Fixture.System);
        return false;
    }

    TSet<FString> OwnersCarryingEntryId;
    TSet<FString> KeysCarryingEntryId;
    for (const TSharedPtr<FJsonValue>& Value : Stack->GetArrayField(TEXT("modules")))
    {
        const TSharedPtr<FJsonObject> Module = Value.IsValid() ? Value->AsObject() : nullptr;
        FString EntryId;
        if (!Module.IsValid() || !Module->TryGetStringField(TEXT("entryId"), EntryId) || EntryId != Fixture.EntryId)
        {
            continue;
        }
        FString OwnerName;
        Module->TryGetStringField(TEXT("ownerName"), OwnerName);
        OwnersCarryingEntryId.Add(OwnerName);
        FString EntryKey;
        Module->TryGetStringField(TEXT("entryKey"), EntryKey);
        KeysCarryingEntryId.Add(EntryKey);
    }

    // The defect: one id, two owners. Asserted so the test fails loudly if the fixture ever
    // stops reproducing the collision it is written to guard.
    TestEqual(
        TEXT("the same entryId is listed under both emitters"),
        OwnersCarryingEntryId.Num(),
        2);
    // The fix: the owner-qualified key tells the two entries apart.
    TestEqual(
        TEXT("entryKey distinguishes the two colliding entries"),
        KeysCarryingEntryId.Num(),
        2);
    TestTrue(
        TEXT("entryKey for the first emitter is owner-qualified"),
        KeysCarryingEntryId.Contains(Fixture.OwnerA + TEXT(":") + Fixture.EntryId));
    TestTrue(
        TEXT("entryKey for the second emitter is owner-qualified"),
        KeysCarryingEntryId.Contains(Fixture.OwnerB + TEXT(":") + Fixture.EntryId));

    NIRTestFixtures::DestroyFixture(Fixture.System);
    return true;
}

// ---------------------------------------------------------------------------
// Write side — a qualified key addresses its own emitter, and only its own emitter.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraEntryIdQualifiedKeyAddressesIntendedEmitterTest,
    "PinWright.niagara.entry_id.QualifiedKeyAddressesIntendedEmitter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraEntryIdQualifiedKeyAddressesIntendedEmitterTest::RunTest(const FString& Parameters)
{
    FCollidingEntryIdFixture Fixture;
    const bool bBuilt = BuildCollidingEntryIdFixture(Fixture);
    if (!TestTrue(TEXT("two-emitter colliding-entryId fixture built"), bBuilt))
    {
        NIRTestFixtures::DestroyFixture(Fixture.System);
        return false;
    }

    const FString QualifiedEntryIdB = Fixture.OwnerB + TEXT(":") + Fixture.EntryId;

    // `emitter` deliberately omitted: a qualified key names its own owner, which is the whole
    // point of it being storable and replayable on its own.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.System->GetPathName());
    Payload->SetStringField(TEXT("entryId"), QualifiedEntryIdB);
    Payload->SetStringField(TEXT("inputName"), SpawnRateInputNameForEntryIdTest);
    Payload->SetNumberField(TEXT("value"), 222.0);
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_module_input"), Payload, Capture))
    {
        NIRTestFixtures::DestroyFixture(Fixture.System);
        return false;
    }

    TestEqual(
        TEXT("response names the emitter the write landed on"),
        Capture.Result->GetStringField(TEXT("emitter")),
        Fixture.OwnerB);
    TestEqual(
        TEXT("response echoes the owner-qualified entryKey"),
        Capture.Result->GetStringField(TEXT("entryKey")),
        QualifiedEntryIdB);
    // Unchanged contract: `entryId` stays the bare node guid the caller can keep comparing against.
    TestEqual(
        TEXT("entryId still echoes back as the bare node guid"),
        Capture.Result->GetStringField(TEXT("entryId")),
        Fixture.EntryId);

    TestEqual(
        TEXT("the intended emitter's module took the override"),
        ReadModuleInputValueMode(Fixture.ModuleB, SpawnRateInputNameForEntryIdTest),
        FString(TEXT("local")));
    TestEqual(
        TEXT("the colliding emitter's identically-keyed module was left alone"),
        ReadModuleInputValueMode(Fixture.ModuleA, SpawnRateInputNameForEntryIdTest),
        FString(TEXT("default")));

    // The silent wrong-target write the ticket measured: right id, wrong emitter. It resolved and
    // succeeded before the fix; a qualified key now disagrees with `emitter` out loud.
    TSharedPtr<FJsonObject> MismatchPayload = MakeShared<FJsonObject>();
    MismatchPayload->SetStringField(TEXT("assetPath"), Fixture.System->GetPathName());
    MismatchPayload->SetStringField(TEXT("emitter"), Fixture.OwnerA);
    MismatchPayload->SetStringField(TEXT("entryId"), QualifiedEntryIdB);
    MismatchPayload->SetStringField(TEXT("inputName"), SpawnRateInputNameForEntryIdTest);
    MismatchPayload->SetNumberField(TEXT("value"), 111.0);
    MismatchPayload->SetBoolField(TEXT("compile"), false);
    MismatchPayload->SetBoolField(TEXT("save"), false);

    NiagaraEditTestUtils::InvokeExpectError(
        *this,
        TEXT("niagara.set_module_input"),
        MismatchPayload,
        TEXT("MODULE_OWNER_MISMATCH"));
    TestEqual(
        TEXT("the refused wrong-emitter write left that emitter's module untouched"),
        ReadModuleInputValueMode(Fixture.ModuleA, SpawnRateInputNameForEntryIdTest),
        FString(TEXT("default")));

    NIRTestFixtures::DestroyFixture(Fixture.System);
    return true;
}
