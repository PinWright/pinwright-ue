// Copyright (c) 2026 Alexander Penkin. MIT License.

// F-niagara-read-curve-keys.
//
// The over-life ramps in real content are usually not a module's own curve input but the curve of
// a dynamic input driving a float input (`ScaleColor.Scale Alpha` <- FloatFromCurve.FloatCurve).
// niagara.get_curve_keys could read such a curve only if the caller knew the dynamic-input node's
// id, and no readback published it; niagara.inspect {includeStack:true} reported the curve input as
// `valueMode: "data"` with nothing about the curve. So the ramp could be neither read nor found.
//
// Fixed in the stack readback both reporters used: a dynamicInput entry carries
// `dynamicInputEntryId`, and a curve-valued `data` entry carries a `curve` summary. This test walks
// the whole discovery path a caller has: inspect -> entry id -> get_curve_keys.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

namespace TestNiagaraCurveDynamicInputReadbackHelpers
{
    const TCHAR* ScaleColorModulePath = TEXT("/Niagara/Modules/Update/Color/ScaleColor.ScaleColor");
    const TCHAR* FloatFromCurvePath = TEXT("/Niagara/DynamicInputs/ValueFromCurve/FloatFromCurve.FloatFromCurve");

    // The first stack input of Node whose type satisfies Pred, by the short name the verbs take.
    // Preferred is returned when it exists and matches, so the fixture mirrors the ticket's
    // `Scale Alpha` while staying correct if engine content renames it.
    template <typename PredicateType>
    FString FindStackInputName(const UNiagaraNodeFunctionCall& Node, const TCHAR* Preferred, PredicateType Pred)
    {
        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(Node, Inputs);
        FString First;
        for (const FNiagaraVariable& Variable : Inputs)
        {
            if (!Pred(Variable.GetType()))
            {
                continue;
            }
            const FString Name = FNiagaraParameterHandle(Variable.GetName()).GetName().ToString();
            if (Preferred && Name.Equals(Preferred, ESearchCase::IgnoreCase))
            {
                return Name;
            }
            if (First.IsEmpty())
            {
                First = Name;
            }
        }
        return First;
    }

    TSharedPtr<FJsonObject> FindEntry(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& Name)
    {
        for (const TSharedPtr<FJsonValue>& Value : Entries)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            FString EntryName;
            if (Entry.IsValid() && Entry->TryGetStringField(TEXT("name"), EntryName) && EntryName == Name)
            {
                return Entry;
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonValue> MakeKey(double Time, double Value)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("time"), Time);
        Key->SetNumberField(TEXT("value"), Value);
        return MakeShared<FJsonValueObject>(Key);
    }

    TSharedPtr<FJsonObject> MakeCurvePayload(const UNiagaraSystem& System, const FString& EntryId, const FString& InputName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System.GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), EntryId);
        Payload->SetStringField(TEXT("inputName"), InputName);
        return Payload;
    }

    bool ReadRange(const TSharedPtr<FJsonObject>& Channel, const TCHAR* Field, double& OutMin, double& OutMax)
    {
        const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
        if (!Channel.IsValid() || !Channel->TryGetArrayField(Field, Range) || !Range || Range->Num() != 2)
        {
            return false;
        }
        OutMin = (*Range)[0]->AsNumber();
        OutMax = (*Range)[1]->AsNumber();
        return true;
    }
}

// ---------------------------------------------------------------------------
// Counterfactual: drop `dynamicInputEntryId` from NiagaraDumpBuilder::BuildModuleInputsJson and the
// entry-id assertion fails and the get_curve_keys leg has no address to use; drop the `curve`
// summary and every summary assertion fails while the curve is plainly authored (set_curve_keys
// wrote three keys onto it in this test).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraGetCurveKeysDynamicInputDiscoverableTest,
    "PinWright.niagara.get_curve_keys.DynamicInputCurveDiscoverableFromInspect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGetCurveKeysDynamicInputDiscoverableTest::RunTest(const FString& Parameters)
{
    using namespace TestNiagaraCurveDynamicInputReadbackHelpers;

    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ScaleColorModulePath);
    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, FloatFromCurvePath);
    if (!ModuleScript || !DynamicInputScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("stock Niagara content did not load ('%s': %s, '%s': %s)"),
                ScaleColorModulePath, ModuleScript ? TEXT("ok") : TEXT("missing"),
                FloatFromCurvePath, DynamicInputScript ? TEXT("ok") : TEXT("missing")));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }
    // Scoped ownership of the rooted fixture: unroots the system and its emitter on every exit.
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    if (System->GetEmitterHandles().Num() > 0)
    {
        Roots.Emitter = System->GetEmitterHandles()[0].GetInstance().Emitter.Get();
    }
    ON_SCOPE_EXIT { NIRTestFixtures::DestroyFixture(System); };

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System, ENiagaraScriptUsage::ParticleUpdateScript, ModuleScript);
    if (!TestNotNull(TEXT("ScaleColor added to the ParticleUpdate stack"), ModuleNode))
    {
        return false;
    }

    const FString FloatInput = FindStackInputName(*ModuleNode, TEXT("Scale Alpha"),
        [](const FNiagaraTypeDefinition& Type) { return Type == FNiagaraTypeDefinition::GetFloatDef(); });
    if (!TestFalse(TEXT("precondition: ScaleColor declares a float input"), FloatInput.IsEmpty()))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* DynamicInputNode =
        NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(*FloatInput), DynamicInputScript);
    if (!TestNotNull(TEXT("precondition: FloatFromCurve drives the float input"), DynamicInputNode))
    {
        return false;
    }

    const FString CurveInput = FindStackInputName(*DynamicInputNode, nullptr,
        [](const FNiagaraTypeDefinition& Type) { return Type.GetClass() == UNiagaraDataInterfaceCurve::StaticClass(); });
    if (!TestFalse(TEXT("precondition: FloatFromCurve declares a scalar curve input"), CurveInput.IsEmpty()))
    {
        return false;
    }

    // Author the nested curve through the production verb, addressed at the dynamic-input node.
    const FString DynamicInputEntryId = DynamicInputNode->NodeGuid.ToString();
    TSharedPtr<FJsonObject> SetPayload = MakeCurvePayload(*System, DynamicInputEntryId, CurveInput);
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeKey(0.0, 0.2));
    Keys.Add(MakeKey(0.35, 1.0));
    Keys.Add(MakeKey(1.0, 0.05));
    SetPayload->SetArrayField(TEXT("keys"), Keys);
    SetPayload->SetBoolField(TEXT("compile"), false);
    SetPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture SetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_curve_keys"), SetPayload, SetCapture))
    {
        return false;
    }

    // -- discovery: the stack readback a caller actually has -----------------
    const TSharedPtr<FJsonObject> ModuleEntry = FindEntry(NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode), FloatInput);
    if (!TestTrue(TEXT("inspect reports the float input as dynamicInput"),
            ModuleEntry.IsValid() && ModuleEntry->GetStringField(TEXT("valueMode")) == TEXT("dynamicInput")))
    {
        return false;
    }
    FString PublishedEntryId;
    TestTrue(TEXT("the dynamicInput entry publishes dynamicInputEntryId"),
        ModuleEntry->TryGetStringField(TEXT("dynamicInputEntryId"), PublishedEntryId));
    TestEqual(TEXT("dynamicInputEntryId is the placed FloatFromCurve node"), PublishedEntryId, DynamicInputEntryId);

    const TArray<TSharedPtr<FJsonValue>>* NestedInputs = nullptr;
    const TSharedPtr<FJsonObject> CurveEntry =
        ModuleEntry->TryGetArrayField(TEXT("inputs"), NestedInputs) && NestedInputs
            ? FindEntry(*NestedInputs, CurveInput)
            : nullptr;
    if (!TestTrue(TEXT("the nested curve input reads valueMode data"),
            CurveEntry.IsValid() && CurveEntry->GetStringField(TEXT("valueMode")) == TEXT("data")))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Curve = nullptr;
    if (TestTrue(TEXT("the data entry carries a curve summary"), CurveEntry->TryGetObjectField(TEXT("curve"), Curve) && Curve))
    {
        const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
        if (TestTrue(TEXT("summary has one channel for a scalar curve"),
                (*Curve)->TryGetArrayField(TEXT("channels"), Channels) && Channels && Channels->Num() == 1))
        {
            const TSharedPtr<FJsonObject> Channel = (*Channels)[0]->AsObject();
            TestEqual(TEXT("summary counts the three authored keys"),
                Channel.IsValid() ? static_cast<int32>(Channel->GetNumberField(TEXT("keyCount"))) : -1, 3);
            double MinTime = -1.0, MaxTime = -1.0, MinValue = -1.0, MaxValue = -1.0;
            if (TestTrue(TEXT("summary carries timeRange"), ReadRange(Channel, TEXT("timeRange"), MinTime, MaxTime)))
            {
                TestEqual(TEXT("timeRange starts at the first key"), MinTime, 0.0, 0.001);
                TestEqual(TEXT("timeRange ends at the last key"), MaxTime, 1.0, 0.001);
            }
            if (TestTrue(TEXT("summary carries valueRange"), ReadRange(Channel, TEXT("valueRange"), MinValue, MaxValue)))
            {
                TestEqual(TEXT("valueRange minimum is the lowest key"), MinValue, 0.05, 0.001);
                TestEqual(TEXT("valueRange maximum is the highest key"), MaxValue, 1.0, 0.001);
            }
        }
    }

    // -- read: the id the readback published is a working get_curve_keys address ---
    FTestResponseCapture GetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_curve_keys"),
            MakeCurvePayload(*System, PublishedEntryId, CurveInput), GetCapture))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ReadKeys = nullptr;
    const bool bHasKeys = GetCapture.Result->TryGetArrayField(TEXT("curves"), Curves) && Curves && Curves->Num() == 1
        && (*Curves)[0]->AsObject().IsValid()
        && (*Curves)[0]->AsObject()->TryGetArrayField(TEXT("keys"), ReadKeys) && ReadKeys;
    if (TestTrue(TEXT("get_curve_keys returns one channel with keys"), bHasKeys))
    {
        TestEqual(TEXT("get_curve_keys reads back the three keys"), ReadKeys->Num(), 3);
    }
    return true;
}
