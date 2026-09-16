// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.set_curve_keys / niagara.get_curve_keys / niagara.bind_curve_asset.
//
// Registration counterfactual: if NiagaraCurveHandler.cpp is removed or the REGISTER_RPC_HANDLER
// macros are reverted, IsHandlerRegistered returns false and the first test fails.
//
// The module-input tests below cover B-niagara-set-curve-keys-unreachable-module-input-di: every
// stock over-life curve (ScaleSpriteSize's sprite-scale curve, ScaleColor's alpha curve) lives on a
// stack module's input pin and in NO parameter store, so the store-only `scope` + `parameterName`
// contract could not address one and returned DATA_INTERFACE_NOT_FOUND for every spelling.
//
// The functional coverage the original version of this file deferred is now possible because the
// fixture problem it named was solved elsewhere: NIRTestFixtures::BuildEmptySystemWithEmitter
// duplicates a real saved Niagara system into a transient package, so the editor pipeline
// (PostLoad GraphSource, VersionData, MessageAssetKey, SVM-primed override scaffolding) is live and
// the handlers resolve exactly as production does. The store-form path still has no functional test
// here — it needs a resolved curve DI inside a parameter store, which the module-input form
// deliberately does not produce.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

namespace
{
    // ScaleSpriteSize is the module the ticket names, and the one whose unreachable curve made
    // sixteen shipped emitters render nothing.
    const TCHAR* ScaleSpriteSizeModulePath = TEXT("/Niagara/Modules/Update/Size/ScaleSpriteSize.ScaleSpriteSize");

    // Address the module's SCALAR curve input by the name the module itself declares, rather than
    // hardcoding a display name engine content may rename between versions. Scalar specifically, so
    // the call needs no `channel` and the assertions stay about addressing.
    bool FindScalarCurveInputName(const UNiagaraNodeFunctionCall& ModuleNode, FString& OutInputName)
    {
        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(ModuleNode, Inputs);
        for (const FNiagaraVariable& Variable : Inputs)
        {
            if (Variable.GetType().GetClass() == UNiagaraDataInterfaceCurve::StaticClass())
            {
                OutInputName = FNiagaraParameterHandle(Variable.GetName()).GetName().ToString();
                return true;
            }
        }
        return false;
    }

    TSharedPtr<FJsonValue> MakeCurveKey(double Time, double Value)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("time"), Time);
        Key->SetNumberField(TEXT("value"), Value);
        return MakeShared<FJsonValueObject>(Key);
    }

    // Payload addressing one module input on the fixture system. Callers add the verb-specific
    // fields (`keys`, `channel`) themselves.
    TSharedPtr<FJsonObject> MakeModuleInputPayload(
        const UNiagaraSystem& System,
        const FString& EntryId,
        const FString& InputName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System.GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), EntryId);
        Payload->SetStringField(TEXT("inputName"), InputName);
        return Payload;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetCurveKeysTest,
    "PinWright.niagara.set_curve_keys.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysTest::RunTest(const FString& Parameters)
{
    TestTrue(
        TEXT("niagara.set_curve_keys is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.set_curve_keys")));

    TestTrue(
        TEXT("niagara.get_curve_keys is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.get_curve_keys")));

    TestTrue(
        TEXT("niagara.bind_curve_asset is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.bind_curve_asset")));

    return true;
}

// ---------------------------------------------------------------------------
// A stack module's curve input is addressable, writable and readable.
//
// Counterfactual: revert the module-input branch and `entryId` + `inputName` is either rejected as
// an unknown parameter by the dispatcher or falls through to the store lookup, which returns
// DATA_INTERFACE_NOT_FOUND — the exact refusal three reporters recorded. Either way
// InvokeExpectSuccess fails. Reverting only the create-override half leaves the write landing on
// the module SCRIPT's shared default object, and `createdOverride` reads false.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetCurveKeysModuleInputTest,
    "PinWright.niagara.set_curve_keys.ModuleInputRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysModuleInputTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ScaleSpriteSizeModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("ScaleSpriteSize module script did not load from '%s'"), ScaleSpriteSizeModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ModuleScript);
    if (!TestNotNull(TEXT("ScaleSpriteSize module added to the ParticleUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FString InputName;
    if (!FindScalarCurveInputName(*ModuleNode, InputName))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_module_declares_no_scalar_curve_input"),
            TEXT("ScaleSpriteSize declared no UNiagaraDataInterfaceCurve stack input on this engine"));
        NIRTestFixtures::DestroyFixture(System);
        return true;
    }

    const FString EntryId = ModuleNode->NodeGuid.ToString();

    // -- write ------------------------------------------------------------
    TSharedPtr<FJsonObject> SetPayload = MakeModuleInputPayload(*System, EntryId, InputName);
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeCurveKey(0.0, 0.2));
    Keys.Add(MakeCurveKey(0.35, 1.0));
    Keys.Add(MakeCurveKey(1.0, 0.05));
    SetPayload->SetArrayField(TEXT("keys"), Keys);
    SetPayload->SetBoolField(TEXT("compile"), false);
    SetPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture SetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_curve_keys"), SetPayload, SetCapture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TestEqual(TEXT("the call reports the module-input addressing it used"),
        SetCapture.Result->GetStringField(TEXT("addressing")), FString(TEXT("moduleInput")));
    TestEqual(TEXT("all three keys were written"),
        static_cast<int32>(SetCapture.Result->GetNumberField(TEXT("written"))), 3);
    TestEqual(TEXT("the input now carries a data-interface value"),
        SetCapture.Result->GetStringField(TEXT("valueMode")), FString(TEXT("data")));
    bool bCreatedOverride = false;
    TestTrue(TEXT("createdOverride is reported"),
        SetCapture.Result->TryGetBoolField(TEXT("createdOverride"), bCreatedOverride));
    TestTrue(TEXT("a default-valued input gets its own override rather than the module asset's object"),
        bCreatedOverride);
    TestEqual(TEXT("the response echoes the input name the module declares"),
        SetCapture.Result->GetStringField(TEXT("inputName")), InputName);

    // -- read back --------------------------------------------------------
    TSharedPtr<FJsonObject> GetPayload = MakeModuleInputPayload(*System, EntryId, InputName);
    FTestResponseCapture GetCapture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.get_curve_keys"), GetPayload, GetCapture))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TestEqual(TEXT("the read resolves this asset's override, not the module script default"),
        GetCapture.Result->GetStringField(TEXT("valueMode")), FString(TEXT("data")));

    const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
    if (!GetCapture.Result->TryGetArrayField(TEXT("curves"), Curves) || !Curves)
    {
        AddError(TEXT("get_curve_keys returned no 'curves' array"));
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestEqual(TEXT("a scalar curve DI reports exactly one channel"), Curves->Num(), 1);
    if (Curves->Num() == 1)
    {
        const TSharedPtr<FJsonObject> ChannelObject = (*Curves)[0]->AsObject();
        const TArray<TSharedPtr<FJsonValue>>* ReadKeys = nullptr;
        if (ChannelObject.IsValid() && ChannelObject->TryGetArrayField(TEXT("keys"), ReadKeys) && ReadKeys)
        {
            TestEqual(TEXT("the three written keys read back"), ReadKeys->Num(), 3);
            if (ReadKeys->Num() == 3)
            {
                const TSharedPtr<FJsonObject> MiddleKey = (*ReadKeys)[1]->AsObject();
                if (MiddleKey.IsValid())
                {
                    TestEqual(TEXT("middle key time round-trips"),
                        static_cast<float>(MiddleKey->GetNumberField(TEXT("time"))), 0.35f, 0.001f);
                    TestEqual(TEXT("middle key value round-trips"),
                        static_cast<float>(MiddleKey->GetNumberField(TEXT("value"))), 1.0f, 0.001f);
                }
                else
                {
                    AddError(TEXT("get_curve_keys returned a non-object key entry"));
                }
            }
        }
        else
        {
            AddError(TEXT("get_curve_keys returned a channel with no 'keys' array"));
        }
    }

    // -- second write reuses the override it made the first time -----------
    TSharedPtr<FJsonObject> SecondPayload = MakeModuleInputPayload(*System, EntryId, InputName);
    TArray<TSharedPtr<FJsonValue>> SecondKeys;
    SecondKeys.Add(MakeCurveKey(0.0, 1.0));
    SecondKeys.Add(MakeCurveKey(1.0, 0.0));
    SecondPayload->SetArrayField(TEXT("keys"), SecondKeys);

    FTestResponseCapture SecondCapture;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_curve_keys"), SecondPayload, SecondCapture))
    {
        bool bCreatedAgain = true;
        SecondCapture.Result->TryGetBoolField(TEXT("createdOverride"), bCreatedAgain);
        TestFalse(TEXT("a second write reuses the existing override instead of creating another"),
            bCreatedAgain);
        TestEqual(TEXT("the second write replaced the key set rather than appending"),
            static_cast<int32>(SecondCapture.Result->GetNumberField(TEXT("written"))), 2);
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A wrong input name is refused by name, and the refusal publishes the spellings that work.
//
// The ticket's core complaint was not only that the DI was unreachable but that a caller "cannot
// tell wrong name from wrong scope from not addressable at all". Counterfactual: drop the
// data-interface-input list from the MODULE_INPUT_NOT_FOUND message and the second assertion fails
// while the first still passes — which is exactly the half-fix this guards against.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetCurveKeysUnknownInputTest,
    "PinWright.niagara.set_curve_keys.UnknownModuleInputListsCurveInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysUnknownInputTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ScaleSpriteSizeModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("ScaleSpriteSize module script did not load from '%s'"), ScaleSpriteSizeModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ModuleScript);
    if (!TestNotNull(TEXT("ScaleSpriteSize module added to the ParticleUpdate stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    FString RealInputName;
    const bool bHasCurveInput = FindScalarCurveInputName(*ModuleNode, RealInputName);

    TSharedPtr<FJsonObject> Payload = MakeModuleInputPayload(
        *System, ModuleNode->NodeGuid.ToString(), TEXT("No Such Curve Input"));
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeCurveKey(0.0, 1.0));
    Payload->SetArrayField(TEXT("keys"), Keys);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(FString(TEXT("niagara.set_curve_keys")), Payload, Capture);
    TestTrue(TEXT("the call was refused"), Capture.bWasCalled && !Capture.bSuccess);
    TestEqual(TEXT("an unknown module input is refused by its own code"),
        Capture.ErrorCode, FString(TEXT("MODULE_INPUT_NOT_FOUND")));
    if (bHasCurveInput)
    {
        TestTrue(TEXT("the refusal names the module's data-interface inputs so the spelling is discoverable"),
            Capture.Message.Contains(RealInputName));
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// The store-form refusal points at the form that can reach a module curve.
//
// Counterfactual: restore the bare "Parameter '%s' was not found in scope '%s'." message and the
// assertion fails. That bare text is what left three reporters cycling spellings against a store
// that structurally cannot hold the object they were addressing.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetCurveKeysStoreMissNamesModuleFormTest,
    "PinWright.niagara.set_curve_keys.StoreMissNamesTheModuleForm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysStoreMissNamesModuleFormTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("scope"), TEXT("updateRapidIteration"));
    Payload->SetStringField(TEXT("parameterName"),
        TEXT("Constants.Fountain.ScaleSpriteSize.Uniform Curve Sprite Scale"));
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeCurveKey(0.0, 1.0));
    Payload->SetArrayField(TEXT("keys"), Keys);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(FString(TEXT("niagara.set_curve_keys")), Payload, Capture);
    TestTrue(TEXT("the call was refused"), Capture.bWasCalled && !Capture.bSuccess);
    TestEqual(TEXT("a missing store parameter still reports DATA_INTERFACE_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("DATA_INTERFACE_NOT_FOUND")));
    TestTrue(TEXT("the refusal names the module-input addressing that can reach a stack curve"),
        Capture.Message.Contains(TEXT("entryId")) && Capture.Message.Contains(TEXT("inputName")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// The two addressing forms are mutually exclusive, and neither is optional.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetCurveKeysAddressingExclusivityTest,
    "PinWright.niagara.set_curve_keys.AddressingFormsAreExclusive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetCurveKeysAddressingExclusivityTest::RunTest(const FString& Parameters)
{
    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeCurveKey(0.0, 1.0));

    TSharedPtr<FJsonObject> BothForms = MakeShared<FJsonObject>();
    BothForms->SetStringField(TEXT("assetPath"), System->GetPathName());
    BothForms->SetStringField(TEXT("scope"), TEXT("updateRapidIteration"));
    BothForms->SetStringField(TEXT("parameterName"), TEXT("Constants.Fountain.Whatever"));
    BothForms->SetStringField(TEXT("entryId"), TEXT("00000000000000000000000000000000"));
    BothForms->SetStringField(TEXT("inputName"), TEXT("Uniform Curve Sprite Scale"));
    BothForms->SetArrayField(TEXT("keys"), Keys);
    NiagaraEditTestUtils::InvokeExpectError(
        *this, TEXT("niagara.set_curve_keys"), BothForms, TEXT("INVALID_ARGUMENT"));

    TSharedPtr<FJsonObject> NeitherForm = MakeShared<FJsonObject>();
    NeitherForm->SetStringField(TEXT("assetPath"), System->GetPathName());
    NeitherForm->SetArrayField(TEXT("keys"), Keys);
    NiagaraEditTestUtils::InvokeExpectError(
        *this, TEXT("niagara.set_curve_keys"), NeitherForm, TEXT("INVALID_ARGUMENT"));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
