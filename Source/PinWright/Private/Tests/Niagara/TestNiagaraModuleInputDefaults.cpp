// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-inspect-default-module-input-has-no-effective-value: an input nobody
// overrode was published as {"valueMode":"default"} with no value at all, so the effective
// configuration of most of a stock module was unreadable from any verb. The declared default
// lives on the module graph's UNiagaraScriptVariable (DefaultMode + the Variable bytes, or
// DefaultBinding); the MapGet default pins that hold the same numbers are anonymous
// and are paired to their outputs only through PinOutputToPinDefaultPersistentId, an id space
// no readback exposes.
//
// Two halves, both against production code:
//  (1) NiagaraEdit::DescribeScriptVariableDefault on constructed script variables — the decode
//      itself, deterministic and independent of engine content, covering the Value case (a
//      canonical pin-default string) and the Binding case the ticket's third encounter singled
//      out as the one an author least expects.
//  (2) NiagaraDumpBuilder::BuildModuleInputsJson on a placed stock AddVelocityInCone, the exact
//      module and input the ticket measured: Cone Axis defaults to (1,0,0), and reading it as
//      the degenerate (0,0,0) is what made a shipped effect's contract undecidable.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "NiagaraCommon.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraModuleInputScriptDefaultTest,
    "PinWright.niagara.dump.ModuleInputScriptDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModuleInputScriptDefaultTest::RunTest(const FString& Parameters)
{
    // (1a) DefaultMode Value: the stored bytes come back as the canonical pin-default string,
    // the same encoding a `local` override's `value` carries.
    {
        UNiagaraScriptVariable* ScriptVariable = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
        if (!TestNotNull(TEXT("Transient UNiagaraScriptVariable constructed"), ScriptVariable))
        {
            return false;
        }
        ScriptVariable->Variable = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Module.Rate")));
        ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
        const float StoredDefault = 2.5f;
        ScriptVariable->SetDefaultValueData(reinterpret_cast<const uint8*>(&StoredDefault));

        NiagaraEdit::FModuleInputDefaultInfo Info;
        TestTrue(TEXT("Value-mode script variable is described"),
            NiagaraEdit::DescribeScriptVariableDefault(*ScriptVariable, Info));
        TestEqual(TEXT("defaultMode names the Value mode"), Info.DefaultMode, FString(TEXT("Value")));
        TestTrue(TEXT("defaultValue is non-empty for a Value-mode declaration"), !Info.DefaultValue.IsEmpty());
        TestEqual(TEXT("defaultValue decodes back to the stored number"),
            FCString::Atof(*Info.DefaultValue), StoredDefault, 0.0001f);
        TestTrue(TEXT("A Value-mode declaration carries no defaultBinding"), Info.DefaultBinding.IsEmpty());
    }

    // (1a') The shape every stock module is in: the parameter panel's edit
    // (FNiagaraScriptVariableDetails::OnValueChanged) writes the new default into Variable and
    // mirrors it to the Map Get default pins, never touching DefaultValueVariant — so a
    // declaration authored that way carries the real number on Variable and a zero-filled
    // variant. Reading the variant reports the type's zero for an authored value.
    {
        UNiagaraScriptVariable* ScriptVariable = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
        if (!TestNotNull(TEXT("Transient panel-authored UNiagaraScriptVariable constructed"), ScriptVariable))
        {
            return false;
        }
        ScriptVariable->Variable = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Module.Rate")));
        ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
        const float VariantDefault = 0.0f;
        ScriptVariable->SetDefaultValueData(reinterpret_cast<const uint8*>(&VariantDefault));
        const float PanelAuthoredDefault = 2.5f;
        ScriptVariable->Variable.SetValue<float>(PanelAuthoredDefault);

        NiagaraEdit::FModuleInputDefaultInfo Info;
        TestTrue(TEXT("Panel-authored script variable is described"),
            NiagaraEdit::DescribeScriptVariableDefault(*ScriptVariable, Info));
        TestEqual(TEXT("defaultValue is the authored number, not the stale variant's zero"),
            FCString::Atof(*Info.DefaultValue), PanelAuthoredDefault, 0.0001f);
    }

    // (1b) DefaultMode Binding: the bound attribute is reported and no value is invented for it.
    // valueMode "default" covers both cases, so without defaultMode a caller cannot tell an
    // input driven by Particles.Age from one holding a constant.
    {
        UNiagaraScriptVariable* ScriptVariable = NewObject<UNiagaraScriptVariable>(GetTransientPackage());
        if (!TestNotNull(TEXT("Transient binding-mode UNiagaraScriptVariable constructed"), ScriptVariable))
        {
            return false;
        }
        ScriptVariable->Variable = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Module.PlayRateTime")));
        ScriptVariable->DefaultMode = ENiagaraDefaultMode::Binding;
        ScriptVariable->DefaultBinding.SetName(FName(TEXT("Particles.Age")));

        NiagaraEdit::FModuleInputDefaultInfo Info;
        TestTrue(TEXT("Binding-mode script variable is described"),
            NiagaraEdit::DescribeScriptVariableDefault(*ScriptVariable, Info));
        TestEqual(TEXT("defaultMode names the Binding mode"), Info.DefaultMode, FString(TEXT("Binding")));
        TestEqual(TEXT("defaultBinding names the bound attribute"),
            Info.DefaultBinding, FString(TEXT("Particles.Age")));
        TestTrue(TEXT("A Binding-mode declaration carries no defaultValue"), Info.DefaultValue.IsEmpty());
    }

    // A null module graph must answer "no default" rather than assert.
    {
        NiagaraEdit::FModuleInputDefaultInfo Info;
        TestFalse(TEXT("Null module graph resolves no default"),
            NiagaraEdit::ResolveModuleInputDefault(
                nullptr,
                FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Module.Rate"))),
                Info));
    }

    // (2) The published readback for the ticket's own measured module/input.
    const TCHAR* ModulePath = TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone");
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not load '%s'"), ModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("ModuleInputDefaults")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("BuildEmptySystemWithEmitter did not produce a transient Niagara system"));
        return true;
    }
    // The fixture factory roots both the system and its emitter; this owns that pin so both are
    // released on the skip returns below as well as on the normal exit.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleSpawnScript,
        ModuleScript);
    if (!ModuleNode)
    {
        NIRTestFixtures::DestroyFixture(System);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("AddVelocityInCone could not be added to the particle spawn stack"));
        return true;
    }

    TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
    Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));

    TSharedPtr<FJsonObject> ConeAxis =
        JsonArrayFindObjectByStringField(Wrapper, TEXT("moduleInputs"), TEXT("name"), TEXT("Cone Axis"));
    if (TestTrue(TEXT("moduleInputs lists the Cone Axis stack input"), ConeAxis.IsValid()))
    {
        FString ValueMode;
        ConeAxis->TryGetStringField(TEXT("valueMode"), ValueMode);
        TestEqual(TEXT("Cone Axis is unwritten on a fresh placement"), ValueMode, FString(TEXT("default")));

        FString DefaultMode;
        TestTrue(TEXT("An unwritten input publishes its declared defaultMode"),
            ConeAxis->TryGetStringField(TEXT("defaultMode"), DefaultMode));
        TestEqual(TEXT("Cone Axis declares a Value default"), DefaultMode, FString(TEXT("Value")));

        FString DefaultValue;
        if (TestTrue(TEXT("An unwritten Value-mode input publishes its declared defaultValue"),
                ConeAxis->TryGetStringField(TEXT("defaultValue"), DefaultValue) && !DefaultValue.IsEmpty()))
        {
            // The measured declaration is (1,0,0); the degenerate (0,0,0) is the reading the
            // anonymous MapGet default pins could not rule out.
            TArray<FString> Components;
            DefaultValue.ParseIntoArray(Components, TEXT(","), /*InCullEmpty=*/true);
            if (TestTrue(TEXT("Cone Axis default is a multi-component vector literal"), Components.Num() >= 3))
            {
                TestEqual(TEXT("Cone Axis default X is 1"), FCString::Atof(*Components[0]), 1.0f, 0.0001f);
                TestEqual(TEXT("Cone Axis default Y is 0"), FCString::Atof(*Components[1]), 0.0f, 0.0001f);
                TestEqual(TEXT("Cone Axis default Z is 0"), FCString::Atof(*Components[2]), 0.0f, 0.0001f);
            }
        }
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// Second half of the same ticket: the graph aspect published a Parameter Map Get's fallback
// pins as name "None" with nothing saying which output parameter each one backs. The pairing is
// stored in PinOutputToPinDefaultPersistentId, keyed on PersistentGuid while the pin `id` the
// dump emits is PinId, so the two id spaces never meet and no reader could join them; pairing by
// position is not a fallback, because AddVelocityInCone's own nodes emit their defaults in
// forward order on one node and reverse order on another.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraMapGetDefaultPinPairingTest,
    "PinWright.niagara.dump.MapGetDefaultPinPairing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMapGetDefaultPinPairingTest::RunTest(const FString& Parameters)
{
    const TCHAR* ModulePath = TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone");
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("could not load '%s'"), ModulePath));
        return true;
    }

    const TSharedPtr<FJsonObject> Graphs = NiagaraDumpBuilder::BuildScriptGraphsJson(ModuleScript);
    if (!TestTrue(TEXT("Script graph aspect built"), Graphs.IsValid()))
    {
        return false;
    }

    // Every anonymous Map Get default pin in the module graph, and the output pin it names.
    int32 AnonymousDefaultPins = 0;
    int32 PairedDefaultPins = 0;
    FString ConeAxisDefaultValue;

    const TArray<TSharedPtr<FJsonValue>>* GraphArray = nullptr;
    Graphs->TryGetArrayField(TEXT("graphs"), GraphArray);
    if (GraphArray)
    {
        for (const TSharedPtr<FJsonValue>& GraphValue : *GraphArray)
        {
            const TSharedPtr<FJsonObject>* Graph = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!GraphValue.IsValid() || !GraphValue->TryGetObject(Graph) || !Graph
                || !(*Graph)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                const TSharedPtr<FJsonObject>* Node = nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
                FString NodeClass;
                if (!NodeValue.IsValid() || !NodeValue->TryGetObject(Node) || !Node
                    || !(*Node)->TryGetStringField(TEXT("class"), NodeClass)
                    || !NodeClass.Contains(TEXT("NiagaraNodeParameterMapGet"))
                    || !(*Node)->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
                {
                    continue;
                }
                for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
                {
                    const TSharedPtr<FJsonObject>* Pin = nullptr;
                    FString Direction;
                    FString PinName;
                    if (!PinValue.IsValid() || !PinValue->TryGetObject(Pin) || !Pin
                        || !(*Pin)->TryGetStringField(TEXT("direction"), Direction)
                        || Direction != TEXT("input")
                        || !(*Pin)->TryGetStringField(TEXT("name"), PinName)
                        || PinName != TEXT("None"))
                    {
                        continue;
                    }
                    ++AnonymousDefaultPins;
                    FString Owner;
                    if ((*Pin)->TryGetStringField(TEXT("defaultForOutputPin"), Owner) && !Owner.IsEmpty())
                    {
                        ++PairedDefaultPins;
                        if (Owner == TEXT("Module.Cone Axis"))
                        {
                            (*Pin)->TryGetStringField(TEXT("defaultValue"), ConeAxisDefaultValue);
                        }
                    }
                }
            }
        }
    }

    if (!TestTrue(TEXT("The module graph emits anonymous Map Get default pins"), AnonymousDefaultPins > 0))
    {
        return false;
    }
    TestEqual(TEXT("Every anonymous default pin names the output pin it backs"),
        PairedDefaultPins, AnonymousDefaultPins);

    // The pairing has to settle the case the ticket could not: two Vector3f defaults on one node,
    // (1,0,0) and (0,0,0), one legal cone axis and one degenerate.
    if (TestTrue(TEXT("The Cone Axis default pin is joinable to its output pin"), !ConeAxisDefaultValue.IsEmpty()))
    {
        TArray<FString> Components;
        ConeAxisDefaultValue.ParseIntoArray(Components, TEXT(","), /*InCullEmpty=*/true);
        if (TestTrue(TEXT("Cone Axis default pin holds a multi-component vector literal"), Components.Num() >= 3))
        {
            TestEqual(TEXT("Cone Axis default pin X is 1"), FCString::Atof(*Components[0]), 1.0f, 0.0001f);
        }
    }

    return true;
}
