// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for module-input stack-name validation and declared-type lookup.
//
// Both tests call niagara.set_module_input directly through the production handler. They use the same transient
// saved-system fixture as the neighboring Niagara tests, so a live editor host is required for
// the substantive assertions; missing stock assets are reported with the suite's skip marker.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/ErrorCodes.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

namespace
{
    const TCHAR* ModuleInputScaleSpriteSizeModulePath = TEXT("/Niagara/Modules/Update/Size/ScaleSpriteSize.ScaleSpriteSize");
    const TCHAR* ScaleSpriteSizeFloatInputName = TEXT("Uniform Curve Index");
    const TCHAR* ScaleSpriteSizeReferenceInputName = TEXT("Uniform Scale Factor");

    bool FindNamedStackInputForModuleInputValidation(
        const UNiagaraNodeFunctionCall& ModuleNode,
        const FString& ExpectedInputName,
        FNiagaraTypeDefinition& OutType)
    {
        OutType = FNiagaraTypeDefinition();
        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(ModuleNode, Inputs);
        for (const FNiagaraVariable& Input : Inputs)
        {
            const FString ShortName = FNiagaraParameterHandle(Input.GetName()).GetName().ToString();
            if (ShortName.Equals(ExpectedInputName, ESearchCase::IgnoreCase))
            {
                OutType = Input.GetType();
                return true;
            }
        }
        return false;
    }

    UEdGraphPin* FindOverridePinByGuidForModuleInputValidation(
        UNiagaraNodeFunctionCall* ModuleNode,
        const FString& PinId)
    {
        if (!ModuleNode || PinId.IsEmpty())
        {
            return nullptr;
        }
        UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph();
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinId.ToString() == PinId)
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }

    int32 CountGraphPinsMatchingModuleInputNameForValidation(
        const UNiagaraNodeFunctionCall* ModuleNode,
        const FString& InputName)
    {
        if (!ModuleNode || InputName.IsEmpty())
        {
            return 0;
        }
        const UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph();
        if (!Graph)
        {
            return 0;
        }
        int32 MatchCount = 0;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinName.ToString().Contains(InputName, ESearchCase::IgnoreCase))
                {
                    ++MatchCount;
                }
            }
        }
        return MatchCount;
    }

    bool OverridePinReadsParameterForModuleInputValidationTest(
        const UEdGraphPin* OverridePin,
        const FString& ParameterName)
    {
        if (!OverridePin || OverridePin->LinkedTo.Num() == 0)
        {
            return false;
        }
        for (const UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            const UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (!UpstreamNode)
            {
                continue;
            }
            if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(UpstreamNode))
            {
                if (InputNode->Input.GetName().ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            static UClass* MapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
            if (MapGetClass && UpstreamNode->IsA(MapGetClass))
            {
                for (const UEdGraphPin* Pin : UpstreamNode->Pins)
                {
                    if (Pin && Pin->PinName.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeSetModuleInputPayloadForValidation(
        UNiagaraSystem* System,
        const TCHAR* EmitterName,
        const FString& ModuleNodeId,
        const FString& InputName,
        const TCHAR* ScriptUsage)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), EmitterName);
        Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
        Payload->SetStringField(TEXT("inputName"), InputName);
        Payload->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }
}

// A non-User particle attribute link must use the placed module input's declared type. Before the
// fix, script-input enumeration returned namespaced variables while this request uses the bare
// stack name, so ResolveLinkedParameter returned PARAMETER_NOT_FOUND for Particles.NormalizedAge.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputLinksParticleAttributeTest,
    "PinWright.niagara.set_module_input.LinksParticleAttribute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputLinksParticleAttributeTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModuleInputScaleSpriteSizeModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("ScaleSpriteSize module script did not load from '%s'"), ModuleInputScaleSpriteSizeModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("ParticleAttributeLink")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("BuildEmptySystemWithEmitter did not produce a transient Niagara system"));
        return true;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ModuleScript);
    if (!ModuleNode)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("ScaleSpriteSize could not be added to the ParticleUpdate stack"));
        NIRTestFixtures::DestroyFixture(System);
        return true;
    }

    FNiagaraTypeDefinition DeclaredInputType;
    if (!TestTrue(
            TEXT("ScaleSpriteSize enumerates the expected Uniform Curve Index input"),
            FindNamedStackInputForModuleInputValidation(
                *ModuleNode,
                ScaleSpriteSizeFloatInputName,
                DeclaredInputType)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestTrue(TEXT("Uniform Curve Index is a float module-stack input"),
        DeclaredInputType == FNiagaraTypeDefinition::GetFloatDef());

    TSharedPtr<FJsonObject> Payload = MakeSetModuleInputPayloadForValidation(
        System,
        TEXT("ParticleAttributeLink"),
        ModuleNode->NodeGuid.ToString(),
        FString(ScaleSpriteSizeFloatInputName),
        TEXT("ParticleUpdateScript"));
    TSharedPtr<FJsonObject> LinkValue = MakeShared<FJsonObject>();
    LinkValue->SetStringField(TEXT("link"), TEXT("Particles.NormalizedAge"));
    Payload->SetObjectField(TEXT("value"), LinkValue);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        Capture);
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    bool bLinked = false;
    TestTrue(TEXT("particle attribute response carries linked field"),
        Capture.Result->TryGetBoolField(TEXT("linked"), bLinked));
    TestTrue(TEXT("particle attribute response reports linked: true"), bLinked);
    TestEqual(
        TEXT("response echoes the particle attribute parameter"),
        Capture.Result->GetStringField(TEXT("parameter")),
        FString(TEXT("Particles.NormalizedAge")));

    FString ParameterType;
    TestTrue(TEXT("response reports the inferred linked parameter type"),
        Capture.Result->TryGetStringField(TEXT("parameterType"), ParameterType));
    TestEqual(
        TEXT("particle attribute link uses the declared float input type"),
        ParameterType,
        FNiagaraTypeDefinition::GetFloatDef().GetName());

    const FString PinId = Capture.Result->GetStringField(TEXT("pinId"));
    UEdGraphPin* OverridePin = FindOverridePinByGuidForModuleInputValidation(ModuleNode, PinId);
    TestNotNull(TEXT("particle attribute link returned an override pin"), OverridePin);
    TestTrue(
        TEXT("particle attribute link reads the exact Particles.NormalizedAge parameter"),
        OverridePinReadsParameterForModuleInputValidationTest(OverridePin, TEXT("Particles.NormalizedAge")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// An unknown dotted spelling must not reach the literal/rapid-iteration fallback. The pre-fix
// handler could return success while writing a value that no real module input consumed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRejectsDottedSubInputTest,
    "PinWright.niagara.set_module_input.RejectsDottedSubInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRejectsDottedSubInputTest::RunTest(const FString& Parameters)
{
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, ModuleInputScaleSpriteSizeModulePath);
    if (!ModuleScript)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("ScaleSpriteSize module script did not load from '%s'"), ModuleInputScaleSpriteSizeModulePath));
        return true;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("DottedSubInput")));
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("BuildEmptySystemWithEmitter did not produce a transient Niagara system"));
        return true;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleUpdateScript,
        ModuleScript);
    if (!ModuleNode)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_unavailable"),
            TEXT("ScaleSpriteSize could not be added to the ParticleUpdate stack"));
        NIRTestFixtures::DestroyFixture(System);
        return true;
    }

    FNiagaraTypeDefinition ReferenceInputType;
    if (!TestTrue(
            TEXT("ScaleSpriteSize enumerates the expected Uniform Scale Factor input"),
            FindNamedStackInputForModuleInputValidation(
                *ModuleNode,
                ScaleSpriteSizeReferenceInputName,
                ReferenceInputType)))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }
    TestTrue(TEXT("Uniform Scale Factor is a float module-stack input"),
        ReferenceInputType == FNiagaraTypeDefinition::GetFloatDef());

    const FString InvalidInputName = TEXT("NoSuchInput.SubValue");
    const FString ActualInputName = ScaleSpriteSizeReferenceInputName;

    UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph();
    const int32 GraphNodeCountBefore = Graph ? Graph->Nodes.Num() : INDEX_NONE;
    const int32 InvalidPinCountBefore = CountGraphPinsMatchingModuleInputNameForValidation(
        ModuleNode,
        InvalidInputName);

    TSharedPtr<FJsonObject> Payload = MakeSetModuleInputPayloadForValidation(
        System,
        TEXT("DottedSubInput"),
        ModuleNode->NodeGuid.ToString(),
        InvalidInputName,
        TEXT("ParticleUpdateScript"));
    Payload->SetNumberField(TEXT("value"), 8.0);

    FTestResponseCapture Capture;
    const bool bDispatched = InvokeHandlerWithCapture(
        FString(TEXT("niagara.set_module_input")),
        Payload,
        Capture);
    TestTrue(TEXT("dotted sub-input request was dispatched"), bDispatched && Capture.bWasCalled);
    TestFalse(TEXT("unknown dotted sub-input request was refused"), Capture.bSuccess);
    TestEqual(
        TEXT("unknown dotted sub-input reports MODULE_INPUT_NOT_FOUND"),
        Capture.ErrorCode,
        FString(ErrorCodes::ERR_MODULE_INPUT_NOT_FOUND));
    TestTrue(
        TEXT("refusal publishes the available-input list marker"),
        Capture.Message.Contains(TEXT("Its stack inputs are:")));
    TestTrue(
        TEXT("refusal lists an independently enumerated real stack input"),
        Capture.Message.Contains(ActualInputName));
    TestEqual(
        TEXT("unknown dotted sub-input leaves graph node count unchanged"),
        Graph ? Graph->Nodes.Num() : INDEX_NONE,
        GraphNodeCountBefore);
    TestEqual(
        TEXT("unknown dotted sub-input creates no invalid override pin"),
        CountGraphPinsMatchingModuleInputNameForValidation(ModuleNode, InvalidInputName),
        InvalidPinCountBefore);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
