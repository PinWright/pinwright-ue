// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-link-modes-destroy-override-silently.
//
// niagara.set_module_input has three value modes and all three replace whatever the module
// input's override pin already carried. The literal mode learned to disclose that
// (B-niagara-literal-over-linked-override-pin: refuse by default, replace on
// breakExistingLink:true, report replacedOverride). The { link: ... } and { dynamicInput: ... }
// modes did not: each called ClearModuleInputOverride unconditionally, deleting an artist's
// dynamic-input chain or another agent's parameter binding, and returned a success response with
// nothing in it naming what had been destroyed.
//
// The fix keeps both modes ungated -- unlike a literal over a link, a link or a dynamic input
// written over an existing override is the operation the caller asked for and it takes effect,
// so refusing it would break the ordinary re-bind and the idempotent retry of a verb named "set"
// -- but makes them report `replacedOverride: {valueMode, source, value}`, classified by the same
// NiagaraEdit::ClassifyModuleInputBindings walk that feeds the inspect / asset.dump `valueMode`
// readback, so the write's account of what it displaced and a later inspect cannot disagree.
//
// Preconditions are built by NIRTestFixtures (SetModuleInputDynamicInput / SetModuleInputLiteral),
// not by the verb under test, except where a phase deliberately chains onto the previous phase's
// product and asserts that product structurally first.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace
{
    // Stock Niagara-plugin fixtures, the same pairing the sibling set_module_input tests use: a
    // float module input and a float-output dynamic input, so every assignment below is
    // type-compatible and no write is rejected for a reason unrelated to what is under test.
    const TCHAR* const SpawnRateModulePathForReplacedTest = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    const TCHAR* const AddFloatDynamicInputPathForReplacedTest = TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float");

    // Locate the module input's override pin by its aliased name ("<FunctionName>.<InputName>").
    // Resolving by name rather than by a pin id kept from an earlier call is required here: every
    // replacing write DELETES the old override pin and creates a fresh one, so a cached pointer or
    // id names a pin that no longer exists. Uniquely suffixed so the unity build cannot collide it
    // with the sibling Niagara test TUs' anonymous-namespace helpers.
    UEdGraphPin* FindOverridePinForReplacedTest(UNiagaraNodeFunctionCall* ModuleNode, const TCHAR* InputName)
    {
        if (!ModuleNode)
        {
            return nullptr;
        }
        UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph();
        if (!Graph)
        {
            return nullptr;
        }
        const FName AliasedName(*FString::Printf(TEXT("%s.%s"), *ModuleNode->GetFunctionName(), InputName));
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == AliasedName)
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }

    // True when the override pin is driven upstream by a function-call node running the given
    // dynamic-input script -- the structural signature of a live dynamic-input chain.
    bool PinDrivenByScriptForReplacedTest(UEdGraphPin* OverridePin, UNiagaraScript* DynamicInputScript)
    {
        if (!OverridePin || !DynamicInputScript)
        {
            return false;
        }
        for (UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
            {
                if (FunctionNode->FunctionScript == DynamicInputScript)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // True when the override pin reads from a named parameter: it is wired upstream to a
    // UNiagaraNodeParameterMapGet carrying a pin named for the parameter. ParameterMapGet has no
    // NIAGARAEDITOR_API export, so its class is resolved by reflection.
    bool PinReadsParameterForReplacedTest(UEdGraphPin* OverridePin, const TCHAR* ParameterName)
    {
        if (!OverridePin)
        {
            return false;
        }
        static UClass* MapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
        for (UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (!UpstreamNode || !MapGetClass || !UpstreamNode->IsA(MapGetClass))
            {
                continue;
            }
            for (UEdGraphPin* Pin : UpstreamNode->Pins)
            {
                if (Pin && Pin->PinName.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeSetInputPayloadForReplacedTest(
        UNiagaraSystem* System,
        const FString& ModuleNodeId,
        const TSharedPtr<FJsonObject>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
        Payload->SetObjectField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    // Read the response's replacedOverride object, asserting its presence. Returns an invalid
    // pointer when the field is absent -- which is exactly the pre-fix response shape.
    TSharedPtr<FJsonObject> RequireReplacedOverrideForReplacedTest(
        FAutomationTestBase& Test,
        const FTestResponseCapture& Capture,
        const TCHAR* What)
    {
        const TSharedPtr<FJsonObject>* Replaced = nullptr;
        if (!Test.TestTrue(
                FString::Printf(TEXT("%s reports replacedOverride"), What),
                Capture.Result.IsValid() && Capture.Result->TryGetObjectField(TEXT("replacedOverride"), Replaced)))
        {
            return nullptr;
        }
        return *Replaced;
    }
}

// ---------------------------------------------------------------------------
// A { link: ... } write over an input already driven by a dynamic-input chain takes effect and
// names the chain it destroyed.
//
// Counterfactual -- on the pre-fix handler the link branch called ClearModuleInputOverride
// directly and threaded nothing back to the handler, so the response carried no replacedOverride
// field at all: RequireReplacedOverride fails, and with it both field assertions. The success and
// structural assertions pass before and after, which is the point -- the write was never broken,
// it was undisclosed.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputLinkReportsReplacedOverrideTest,
    "PinWright.niagara.set_module_input.LinkReportsReplacedOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputLinkReportsReplacedOverrideTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRateModulePathForReplacedTest);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, AddFloatDynamicInputPathForReplacedTest);
    if (!TestNotNull(TEXT("Add_Float dynamic-input script loads"), DynamicInputScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::EmitterUpdateScript,
        SpawnRateScript);
    if (!TestNotNull(TEXT("SpawnRate module added to EmitterUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();

    // Precondition, built by the fixture rather than by the verb under test: SpawnRate is driven
    // by a dynamic-input chain, the artist-authored state the link write is about to delete.
    NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);
    if (!TestTrue(
            TEXT("fixture wired SpawnRate's override pin to the Add_Float dynamic input"),
            PinDrivenByScriptForReplacedTest(FindOverridePinForReplacedTest(ModuleNode, TEXT("SpawnRate")), DynamicInputScript)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // The parameter the link will bind to, authored through the production add_parameter RPC.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("assetPath"), System->GetPathName());
        AddPayload->SetStringField(TEXT("scope"), TEXT("user"));
        AddPayload->SetStringField(TEXT("name"), TEXT("User.SpawnRateScale"));
        AddPayload->SetStringField(TEXT("type"), TEXT("float"));
        AddPayload->SetNumberField(TEXT("defaultValue"), 5.0);
        AddPayload->SetBoolField(TEXT("compile"), false);
        AddPayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture AddCapture;
        if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_parameter"), AddPayload, AddCapture))
        {
            NIRTestFixtures::DestroyFixture(System);
            return false;
        }
    }

    TSharedPtr<FJsonObject> LinkValue = MakeShared<FJsonObject>();
    LinkValue->SetStringField(TEXT("link"), TEXT("User.SpawnRateScale"));

    FTestResponseCapture Capture;
    const bool bLinkWritten = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        MakeSetInputPayloadForReplacedTest(System, ModuleNodeId, LinkValue),
        Capture);
    if (!bLinkWritten)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // The link is not gated behind breakExistingLink: it replaced a live driver and still
    // succeeded, which is the deliberate half of this contract.
    bool bLinked = false;
    Capture.Result->TryGetBoolField(TEXT("linked"), bLinked);
    TestTrue(TEXT("link over an existing override succeeds and reports linked: true"), bLinked);

    // The silent half is what the fix closes: the destroyed chain is named.
    if (TSharedPtr<FJsonObject> Replaced = RequireReplacedOverrideForReplacedTest(*this, Capture, TEXT("link write")))
    {
        FString ReplacedValueMode;
        Replaced->TryGetStringField(TEXT("valueMode"), ReplacedValueMode);
        TestEqual(
            TEXT("replacedOverride names the dynamic-input mode the link displaced"),
            ReplacedValueMode,
            FString(TEXT("dynamicInput")));

        FString ReplacedSource;
        Replaced->TryGetStringField(TEXT("source"), ReplacedSource);
        TestEqual(
            TEXT("replacedOverride names the displaced dynamic-input script"),
            ReplacedSource,
            DynamicInputScript->GetPathName());
    }

    // Structural proof that the report describes the graph: the old chain is gone and the pin now
    // reads the parameter.
    UEdGraphPin* LinkedPin = FindOverridePinForReplacedTest(ModuleNode, TEXT("SpawnRate"));
    if (TestNotNull(TEXT("override pin exists after the link write"), LinkedPin))
    {
        TestTrue(
            TEXT("override pin reads User.SpawnRateScale after the link write"),
            PinReadsParameterForReplacedTest(LinkedPin, TEXT("User.SpawnRateScale")));
        TestFalse(
            TEXT("the displaced Add_Float chain no longer drives the input"),
            PinDrivenByScriptForReplacedTest(LinkedPin, DynamicInputScript));
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A { dynamicInput: ... } write reports what it displaced, whether that was a literal (phase 1,
// whose pin-default text is carried in replacedOverride.value because there is no source to name)
// or an earlier dynamic-input chain (phase 2, which also pins down that reassigning a dynamic
// input is allowed rather than refused).
//
// Counterfactual -- the pre-fix dynamic-input branch cleared the pin unconditionally and reported
// nothing, so both RequireReplacedOverride calls fail and every field assertion under them with
// them. Phase 2's success assertion passes before and after by design: the deliberate decision was
// to keep this mode ungated, so the test would also fail if a later change made a re-assignment
// refuse.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputDynamicInputReportsReplacedOverrideTest,
    "PinWright.niagara.set_module_input.DynamicInputReportsReplacedOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputDynamicInputReportsReplacedOverrideTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRateModulePathForReplacedTest);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, AddFloatDynamicInputPathForReplacedTest);
    if (!TestNotNull(TEXT("Add_Float dynamic-input script loads"), DynamicInputScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::EmitterUpdateScript,
        SpawnRateScript);
    if (!TestNotNull(TEXT("SpawnRate module added to EmitterUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();

    TSharedPtr<FJsonObject> DynamicInputValue = MakeShared<FJsonObject>();
    DynamicInputValue->SetStringField(TEXT("dynamicInput"), AddFloatDynamicInputPathForReplacedTest);

    // ---- Phase 1: a literal override is displaced, and its value is reported --------------
    // Fixture-built precondition: SpawnRate carries a literal pin default.
    FNiagaraVariable LiteralVar(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("SpawnRate")));
    LiteralVar.AllocateData();
    const float LiteralValue = 12.0f;
    LiteralVar.SetData(reinterpret_cast<const uint8*>(&LiteralValue));
    NIRTestFixtures::SetModuleInputLiteral(ModuleNode, FName(TEXT("SpawnRate")), LiteralVar);

    // Copy the pin default out before the write: the write deletes the pin that owns it, and the
    // copy is what the response is checked against, so the assertion cannot drift with the
    // per-type serialisation format.
    UEdGraphPin* LiteralPin = FindOverridePinForReplacedTest(ModuleNode, TEXT("SpawnRate"));
    FString LiteralPinDefault;
    if (LiteralPin)
    {
        LiteralPinDefault = LiteralPin->DefaultValue;
    }
    if (!TestFalse(TEXT("fixture landed a literal pin default on SpawnRate"), LiteralPinDefault.IsEmpty()))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FTestResponseCapture OverLiteralCapture;
    const bool bOverLiteral = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        MakeSetInputPayloadForReplacedTest(System, ModuleNodeId, DynamicInputValue),
        OverLiteralCapture);
    if (!bOverLiteral)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    if (TSharedPtr<FJsonObject> Replaced =
            RequireReplacedOverrideForReplacedTest(*this, OverLiteralCapture, TEXT("dynamic input over a literal")))
    {
        FString ReplacedValueMode;
        Replaced->TryGetStringField(TEXT("valueMode"), ReplacedValueMode);
        TestEqual(
            TEXT("replacedOverride names the local literal mode that was displaced"),
            ReplacedValueMode,
            FString(TEXT("local")));

        FString ReplacedLiteral;
        Replaced->TryGetStringField(TEXT("value"), ReplacedLiteral);
        TestEqual(
            TEXT("replacedOverride carries the displaced literal's pin default"),
            ReplacedLiteral,
            LiteralPinDefault);

        FString ReplacedSource;
        TestFalse(
            TEXT("a displaced literal has no driving source to name"),
            Replaced->TryGetStringField(TEXT("source"), ReplacedSource));
    }

    // ---- Phase 2: an earlier dynamic-input chain is displaced ------------------------------
    // Phase 2's precondition is phase 1's product, so it is verified structurally first.
    if (!TestTrue(
            TEXT("phase 1 left the Add_Float chain driving the input"),
            PinDrivenByScriptForReplacedTest(FindOverridePinForReplacedTest(ModuleNode, TEXT("SpawnRate")), DynamicInputScript)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FTestResponseCapture OverChainCapture;
    const bool bOverChain = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        MakeSetInputPayloadForReplacedTest(System, ModuleNodeId, DynamicInputValue),
        OverChainCapture);
    // Reassigning a dynamic input is a working operation and stays ungated -- it must not start
    // refusing the way a literal over a link does.
    TestNotEqual(
        TEXT("reassigning a dynamic input is not refused as MODULE_INPUT_OVERRIDE_LINKED"),
        OverChainCapture.ErrorCode,
        FString(TEXT("MODULE_INPUT_OVERRIDE_LINKED")));
    if (!bOverChain)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    if (TSharedPtr<FJsonObject> Replaced =
            RequireReplacedOverrideForReplacedTest(*this, OverChainCapture, TEXT("dynamic input over a chain")))
    {
        FString ReplacedValueMode;
        Replaced->TryGetStringField(TEXT("valueMode"), ReplacedValueMode);
        TestEqual(
            TEXT("replacedOverride names the dynamic-input mode that was displaced"),
            ReplacedValueMode,
            FString(TEXT("dynamicInput")));

        FString ReplacedSource;
        Replaced->TryGetStringField(TEXT("source"), ReplacedSource);
        TestEqual(
            TEXT("replacedOverride names the displaced dynamic-input script"),
            ReplacedSource,
            DynamicInputScript->GetPathName());
    }

    TestTrue(
        TEXT("the reassigned dynamic-input chain drives the input"),
        PinDrivenByScriptForReplacedTest(FindOverridePinForReplacedTest(ModuleNode, TEXT("SpawnRate")), DynamicInputScript));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
