// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.set_module_input / niagara.set_stack_enabled stack inference.
//
// These guard the fix from B-niagara-module-input-stack-infer: when the documented-optional
// `scriptUsage` is omitted, ApplyModuleMutation must infer the module's owning stack output via
// FindOwningStackOutputNode instead of defaulting to ParticleUpdateScript. Without the fix a
// module in any non-ParticleUpdate stage (e.g. a SpawnRate module in EmitterUpdate) is absent
// from the ParticleUpdate stack groups, the SourceGroupIndex predicate fails, and the call is
// wrongly rejected with INVALID_STACK "not in a valid stack group".
//
// This mirrors the proven sibling pattern in
// TestNiagaraSetModuleScript.cpp::InfersOwningEmitterUpdateStack (the F-niagara-set-module-script
// #7-infer-owning-stack fix) — the same FindOwningStackOutputNode inference, extended to the
// SetModuleInput and SetStackEnabled operations that share ApplyModuleMutation.
//
// The fixtures (BuildEmptySystemWithEmitter / AddModuleToStack) duplicate a real saved Niagara
// system into a transient package so the editor pipeline (PostLoad GraphSource, VersionData,
// MessageAssetKey, SVM-primed override scaffolding) is live and the dispatcher resolves the
// module's owning stack exactly as production does.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace
{
    // Find the override pin with the given PinId on the module node's owning graph.
    // Returns nullptr when not found.
    UEdGraphPin* FindOverridePin(UNiagaraNodeFunctionCall* ModuleNode, const FString& PinId)
    {
        if (!ModuleNode || PinId.IsEmpty())
        {
            return nullptr;
        }
        if (UNiagaraGraph* Graph = ModuleNode->GetNiagaraGraph())
        {
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
        }
        return nullptr;
    }

    // Find the override pin with the given PinId on the module node's owning graph and
    // return its Niagara type. Used to prove the fresh-pin path typed the override pin
    // from the value's JSON shape. Returns an invalid type definition when not found.
    FNiagaraTypeDefinition FindOverridePinType(UNiagaraNodeFunctionCall* ModuleNode, const FString& PinId)
    {
        if (UEdGraphPin* Pin = FindOverridePin(ModuleNode, PinId))
        {
            return UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
        }
        return FNiagaraTypeDefinition();
    }

    // True when the override pin reads from a named parameter — i.e. the pin is wired
    // upstream to a parameter-read node (UNiagaraNodeParameterMapGet on UE 5.6+, or a
    // UNiagaraNodeInput on older branches) rather than carrying a literal default. This is
    // the inverse of the NIR decompiler's "$Namespace.Name" linked-parameter classification.
    // If the linked-param write path regressed to a literal, the pin would have no link and
    // this returns false. ParameterMapGet is resolved by reflection (no NIAGARAEDITOR_API
    // export); the linked parameter's name must appear on a pin of that upstream node.
    bool OverridePinReadsParameter(UEdGraphPin* OverridePin, const FString& ParameterName)
    {
        if (!OverridePin || OverridePin->LinkedTo.Num() == 0)
        {
            return false;
        }
        for (UEdGraphPin* UpstreamPin : OverridePin->LinkedTo)
        {
            UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
            if (!UpstreamNode)
            {
                continue;
            }
            // UNiagaraNodeInput linked-param case (older branches / dynamic-input-free links).
            if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(UpstreamNode))
            {
                if (InputNode->Input.GetName().ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            // UNiagaraNodeParameterMapGet case (UE 5.6+ SetLinkedParameterValueForFunctionInput):
            // the parameter name surfaces on the get node's typed output pin.
            static UClass* MapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
            if (MapGetClass && UpstreamNode->IsA(MapGetClass))
            {
                // UpstreamPin is itself a member of UpstreamNode->Pins, so this loop already
                // covers the directly-connected pin — no separate UpstreamPin check needed.
                for (UEdGraphPin* Pin : UpstreamNode->Pins)
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
}

// ---------------------------------------------------------------------------
// Registration tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRegistrationTest,
    "PinWright.niagara.set_module_input.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRegistrationTest::RunTest(const FString& Parameters)
{
    TestTrue(
        TEXT("niagara.set_module_input is registered in the dispatcher"),
        IsHandlerRegistered(TEXT("niagara.set_module_input")));
    return true;
}

// ---------------------------------------------------------------------------
// Dispatcher path — omitted scriptUsage infers the module's owning stack output.
//
// Counterfactual: revert the line-645 inference widening in NiagaraEditHandler.cpp and this
// EmitterUpdate SpawnRate module resolves by entryId but is absent from the ParticleUpdate
// stack groups, so the dispatcher returns INVALID_STACK and this test fails on the
// "did not resolve to INVALID_STACK" assertion (and on success).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputInfersOwningEmitterUpdateStackTest,
    "PinWright.niagara.set_module_input.InfersOwningEmitterUpdateStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputInfersOwningEmitterUpdateStackTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Fountain")));
    if (!TestNotNull(TEXT("Transient system with Fountain emitter created"), System))
    {
        return false;
    }

    // SpawnRate is an EmitterUpdate-stage module, not ParticleUpdate — the exact case the
    // ParticleUpdate-defaulting resolver mis-handles when scriptUsage is omitted.
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

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    Payload->SetNumberField(TEXT("value"), 20.0);
    // scriptUsage deliberately omitted — the documented-optional form that used to fail.
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        Capture);
    TestNotEqual(
        TEXT("omitted scriptUsage did not resolve to INVALID_STACK"),
        Capture.ErrorCode,
        FString(TEXT("INVALID_STACK")));
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TestEqual(
        TEXT("returned entryId matches module node guid"),
        Capture.Result->GetStringField(TEXT("entryId")),
        ModuleNodeId);
    TestEqual(
        TEXT("returned inputName echoes the request"),
        Capture.Result->GetStringField(TEXT("inputName")),
        FString(TEXT("SpawnRate")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// set_stack_enabled shares ApplyModuleMutation and the same line-645 inference gate.
//
// Counterfactual: revert the widening and this EmitterUpdate module resolves to the
// ParticleUpdate stack output, fails the SourceGroupIndex predicate, and the dispatcher
// returns INVALID_STACK with scriptUsage omitted.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetStackEnabledInfersOwningEmitterUpdateStackTest,
    "PinWright.niagara.set_stack_enabled.InfersOwningEmitterUpdateStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetStackEnabledInfersOwningEmitterUpdateStackTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
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

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetBoolField(TEXT("enabled"), false);
    // scriptUsage deliberately omitted — the documented-optional form that used to fail.
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_stack_enabled"),
        Payload,
        Capture);
    TestNotEqual(
        TEXT("omitted scriptUsage did not resolve to INVALID_STACK"),
        Capture.ErrorCode,
        FString(TEXT("INVALID_STACK")));
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    bool bEnabled = true;
    TestTrue(TEXT("enabled field returned"), Capture.Result->TryGetBoolField(TEXT("enabled"), bEnabled));
    TestFalse(TEXT("module reported disabled"), bEnabled);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A 2-component value sets an FVector2D module input (B-niagara-set-module-input-vec2).
//
// Before the fix, InferNiagaraInputType / JsonValueToPinDefaultString had no Vec2 case:
// arrays were only formatted at length >= 3 (Vec3/Vec4) and objects only as {x,y,z}, so a
// 2-element value yielded an empty default string and the fresh-pin path rejected it with
// UNSUPPORTED_INPUT_VALUE. Both natural Vec2 spellings ({x,y} and [x,y]) are exercised here.
//
// Counterfactual: revert the Vec2 branch in the two helpers and InferNiagaraInputType returns
// false for {x:8,y:8} → the dispatcher returns UNSUPPORTED_INPUT_VALUE and InvokeExpectSuccess
// fails (no success, no pinId). The pin-type assertion additionally guards against the value
// being mistyped as anything other than Vec2.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputAcceptsVector2DTest,
    "PinWright.niagara.set_module_input.AcceptsVector2D",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputAcceptsVector2DTest::RunTest(const FString& Parameters)
{
    const TCHAR* InitParticlePath = TEXT("/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle");
    UNiagaraScript* InitParticleScript = LoadObject<UNiagaraScript>(nullptr, InitParticlePath);
    if (!TestNotNull(TEXT("InitializeParticle module script loads"), InitParticleScript))
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
        ENiagaraScriptUsage::ParticleSpawnScript,
        InitParticleScript);
    if (!TestNotNull(TEXT("InitializeParticle module added to ParticleSpawn stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    UNiagaraNodeFunctionCall* ArrayModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleSpawnScript,
        InitParticleScript);
    if (!TestNotNull(TEXT("second InitializeParticle module added for array Vec2 coverage"), ArrayModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();
    const FString ArrayModuleNodeId = ArrayModuleNode->NodeGuid.ToString();

    // Drive one Vec2 spelling end-to-end: set InputName on a module to Value, assert the
    // handler did not reject it as UNSUPPORTED_INPUT_VALUE, and on success assert the
    // freshly created override pin was typed Vec2 from the value's JSON shape. Label feeds
    // the assertion text so each spelling reports distinctly. Each call uses a distinct
    // module instance but the known legitimate Sprite Size input, so both hit the fresh-pin
    // inference branch without inventing a second input name.
    auto CheckVec2 = [&](UNiagaraNodeFunctionCall* TargetModuleNode, const FString& TargetModuleNodeId, const TCHAR* Label, const TCHAR* InputName, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), TargetModuleNodeId);
        Payload->SetStringField(TEXT("inputName"), InputName);
        Payload->SetField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
            *this,
            TEXT("niagara.set_module_input"),
            Payload,
            Capture);
        TestNotEqual(
            *FString::Printf(TEXT("%s value not rejected as UNSUPPORTED_INPUT_VALUE"), Label),
            Capture.ErrorCode,
            FString(TEXT("UNSUPPORTED_INPUT_VALUE")));
        if (bSucceeded)
        {
            const FString PinId = Capture.Result->GetStringField(TEXT("pinId"));
            TestFalse(*FString::Printf(TEXT("pinId returned for the %s override"), Label), PinId.IsEmpty());
            const FNiagaraTypeDefinition PinType = FindOverridePinType(TargetModuleNode, PinId);
            TestTrue(
                *FString::Printf(TEXT("%s override pin is typed Vec2 (not Vec3/empty)"), Label),
                PinType == FNiagaraTypeDefinition::GetVec2Def());
        }
    };

    // The {x,y} object form — the natural Vec2 spelling that used to be rejected.
    TSharedPtr<FJsonObject> Vec2Object = MakeShared<FJsonObject>();
    Vec2Object->SetNumberField(TEXT("x"), 8.0);
    Vec2Object->SetNumberField(TEXT("y"), 8.0);
    CheckVec2(ModuleNode, ModuleNodeId, TEXT("{x,y}"), TEXT("Sprite Size"), MakeShared<FJsonValueObject>(Vec2Object));

    // The [x,y] array form — the other natural Vec2 spelling.
    TArray<TSharedPtr<FJsonValue>> Vec2Array;
    Vec2Array.Add(MakeShared<FJsonValueNumber>(16.0));
    Vec2Array.Add(MakeShared<FJsonValueNumber>(32.0));
    CheckVec2(ArrayModuleNode, ArrayModuleNodeId, TEXT("[x,y]"), TEXT("Sprite Size"), MakeShared<FJsonValueArray>(Vec2Array));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A { link: "User.X" } value binds the module input to read from a parameter
// (F-niagara-link-module-input-to-parameter).
//
// Before the fix, niagara.set_module_input only ever wrote a literal pin default — a
// { link: ... } object fell through to InferNiagaraInputType, which has no link case, so
// the call was rejected as UNSUPPORTED_INPUT_VALUE. The author could only hardcode a
// matching literal, producing a *decorative* user parameter: changing it later did not
// retint the particles because the input was not a live binding.
//
// This test creates a User.WispColor LinearColor parameter, links the InitializeParticle
// Color input to it via { link: "User.WispColor" }, and asserts (a) the handler reports
// linked: true / parameter / parameterType, and (b) the override pin actually reads from a
// parameter-read node naming User.WispColor — i.e. the inverse of the NIR decompiler's
// "$User.WispColor" classification, proving a real link rather than a literal.
//
// Counterfactual: revert the linked-parameter branch in ApplyModuleMutation's SetModuleInput
// case and the { link: ... } object is rejected as UNSUPPORTED_INPUT_VALUE → InvokeExpectSuccess
// fails, linked is false, and OverridePinReadsParameter finds no upstream parameter read.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputLinksParameterTest,
    "PinWright.niagara.set_module_input.LinksParameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputLinksParameterTest::RunTest(const FString& Parameters)
{
    const TCHAR* InitParticlePath = TEXT("/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle");
    UNiagaraScript* InitParticleScript = LoadObject<UNiagaraScript>(nullptr, InitParticlePath);
    if (!TestNotNull(TEXT("InitializeParticle module script loads"), InitParticleScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Wisp")));
    if (!TestNotNull(TEXT("Transient system with Wisp emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleSpawnScript,
        InitParticleScript);
    if (!TestNotNull(TEXT("InitializeParticle module added to ParticleSpawn stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    const FString ModuleNodeId = ModuleNode->NodeGuid.ToString();

    // Create the User.WispColor parameter the link will bind to, through the production
    // niagara.add_parameter RPC (so the user-store entry is authored exactly as a real caller's).
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("assetPath"), System->GetPathName());
        AddPayload->SetStringField(TEXT("scope"), TEXT("user"));
        AddPayload->SetStringField(TEXT("name"), TEXT("User.WispColor"));
        AddPayload->SetStringField(TEXT("type"), TEXT("LinearColor"));
        TSharedPtr<FJsonObject> ColorValue = MakeShared<FJsonObject>();
        ColorValue->SetNumberField(TEXT("r"), 0.05);
        ColorValue->SetNumberField(TEXT("g"), 0.4);
        ColorValue->SetNumberField(TEXT("b"), 1.0);
        ColorValue->SetNumberField(TEXT("a"), 1.0);
        AddPayload->SetObjectField(TEXT("defaultValue"), ColorValue);
        AddPayload->SetBoolField(TEXT("compile"), false);
        AddPayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture AddCapture;
        if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_parameter"), AddPayload, AddCapture))
        {
            NIRTestFixtures::DestroyFixture(System);
            return false;
        }
    }

    // Link the InitializeParticle Color input to User.WispColor via the link value shape.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Wisp"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetStringField(TEXT("inputName"), TEXT("Color"));
    TSharedPtr<FJsonObject> LinkValue = MakeShared<FJsonObject>();
    LinkValue->SetStringField(TEXT("link"), TEXT("User.WispColor"));
    Payload->SetObjectField(TEXT("value"), LinkValue);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        Capture);
    TestNotEqual(
        TEXT("link value not rejected as UNSUPPORTED_INPUT_VALUE"),
        Capture.ErrorCode,
        FString(TEXT("UNSUPPORTED_INPUT_VALUE")));
    if (!bSucceeded)
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    bool bLinked = false;
    TestTrue(TEXT("response carries linked field"), Capture.Result->TryGetBoolField(TEXT("linked"), bLinked));
    TestTrue(TEXT("response reports linked: true"), bLinked);
    TestEqual(
        TEXT("response echoes the fully-qualified parameter name"),
        Capture.Result->GetStringField(TEXT("parameter")),
        FString(TEXT("User.WispColor")));

    // The load-bearing structural assertion: the override pin reads from a parameter node
    // naming User.WispColor. A reverted (literal-only) write would leave the pin unlinked.
    const FString PinId = Capture.Result->GetStringField(TEXT("pinId"));
    TestFalse(TEXT("pinId returned for the linked override"), PinId.IsEmpty());
    UEdGraphPin* OverridePin = FindOverridePin(ModuleNode, PinId);
    TestNotNull(TEXT("override pin resolved by id"), OverridePin);
    TestTrue(
        TEXT("override pin reads from the User.WispColor parameter (live link, not a literal)"),
        OverridePinReadsParameter(OverridePin, TEXT("User.WispColor")));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A { link: "User.Missing" } to a non-existent user parameter is rejected, not silently
// turned into a literal (mirrors the B-set-niagara-param-no-validation no-silent-no-op rule).
//
// Counterfactual: drop the existence check in ResolveLinkedParameter and the call would
// either succeed against a phantom parameter or fall through to a literal — this asserts the
// PARAMETER_NOT_FOUND rejection instead.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputLinkMissingParameterRejectedTest,
    "PinWright.niagara.set_module_input.LinkMissingParameterRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputLinkMissingParameterRejectedTest::RunTest(const FString& Parameters)
{
    const TCHAR* InitParticlePath = TEXT("/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle");
    UNiagaraScript* InitParticleScript = LoadObject<UNiagaraScript>(nullptr, InitParticlePath);
    if (!TestNotNull(TEXT("InitializeParticle module script loads"), InitParticleScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("Wisp")));
    if (!TestNotNull(TEXT("Transient system with Wisp emitter created"), System))
    {
        return false;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleSpawnScript,
        InitParticleScript);
    if (!TestNotNull(TEXT("InitializeParticle module added to ParticleSpawn stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Wisp"));
    Payload->SetStringField(TEXT("entryId"), ModuleNode->NodeGuid.ToString());
    Payload->SetStringField(TEXT("inputName"), TEXT("Color"));
    TSharedPtr<FJsonObject> LinkValue = MakeShared<FJsonObject>();
    LinkValue->SetStringField(TEXT("link"), TEXT("User.DoesNotExist"));
    Payload->SetObjectField(TEXT("value"), LinkValue);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("ParticleSpawnScript"));

    const bool bRejected = NiagaraEditTestUtils::InvokeExpectError(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        TEXT("PARAMETER_NOT_FOUND"));
    TestTrue(TEXT("link to a missing user parameter is rejected with PARAMETER_NOT_FOUND"), bRejected);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A literal set_module_input echoes the value it wrote in the result
// (E-niagara-inspect-params-stale-after-override).
//
// set_module_input writes its value onto a graph override pin, NOT the rapid-iteration
// parameter store that niagara.inspect's `parameters` aspect reads — so a params-only
// readback shows the stale template default and a set-then-verify loop looks like a no-op.
// The remedy is to have the write result echo the canonical pin-default string it wrote, so
// the loop confirms from the result without re-inspecting the lying params aspect.
//
// This exercises the production niagara.set_module_input handler end-to-end through the
// dispatcher and asserts the `value` field is present and carries the written number. A
// non-integer spelling (2.5) additionally proves the *canonical* pin-default string is echoed
// (what actually landed on the pin) rather than the raw request value.
//
// Counterfactual: revert the OutWrittenValue thread-through / the result `value` field in
// NiagaraEditHandler.cpp and the result carries no `value` for the literal path — both the
// presence assertion and the echoed-number assertions fail.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputEchoesWrittenValueTest,
    "PinWright.niagara.set_module_input.EchoesWrittenValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputEchoesWrittenValueTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
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

    // Drive one literal SpawnRate value through the dispatcher and assert the result echoes
    // the written value. The override pin write does not touch the rapid-iteration store, so
    // this echoed `value` is the only confirmation the set landed without re-inspecting graphs.
    auto CheckEcho = [&](const TCHAR* Label, double Value, const TCHAR* ExpectedEcho)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
        Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
        Payload->SetNumberField(TEXT("value"), Value);
        Payload->SetBoolField(TEXT("compile"), false);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
            *this,
            TEXT("niagara.set_module_input"),
            Payload,
            Capture);
        if (!bSucceeded)
        {
            return;
        }

        // The literal path is not a parameter link, so linked must be false (the value echo
        // and the linked report are mutually exclusive in the result).
        bool bLinked = true;
        Capture.Result->TryGetBoolField(TEXT("linked"), bLinked);
        TestFalse(*FString::Printf(TEXT("%s literal write is not linked"), Label), bLinked);

        FString Echoed;
        const bool bHasValue = Capture.Result->TryGetStringField(TEXT("value"), Echoed);
        TestTrue(
            *FString::Printf(TEXT("%s result echoes a value field"), Label),
            bHasValue);
        TestEqual(
            *FString::Printf(TEXT("%s echoed value is the canonical written string"), Label),
            Echoed,
            FString(ExpectedEcho));
    };

    // Integer spelling: the canonical pin-default string for a float input is
    // FString::SanitizeFloat(160.0) == "160.0" (default 1 fractional digit).
    CheckEcho(TEXT("int"), 160.0, TEXT("160.0"));
    // Non-integer spelling: proves the canonical pin-default string (post-normalization) is
    // echoed, not the raw request literal. FString::SanitizeFloat(2.5) == "2.5", the same path
    // the int case asserts exactly. Re-setting the same SpawnRate input routes through the
    // ExistingPin write branch, which also threads OutWrittenValue, so this covers both
    // converging write paths.
    CheckEcho(TEXT("fractional"), 2.5, TEXT("2.5"));

    NIRTestFixtures::DestroyFixture(System);
    return true;
}

// ---------------------------------------------------------------------------
// A refused write leaves the asset's dirty flag exactly as it found it.
//
// Regression guard for B-niagara-refused-edit-dirties-package. Every guard inside
// ApplyModuleMutation fires after the handler's FScopedTransaction has already run
// ModifyResolvedTarget and Graph->Modify(), so all four refusal codes named in the ticket
// (INVALID_STACK, PARAMETER_TYPE_MISMATCH, UNSUPPORTED_INPUT_VALUE and
// MODULE_INPUT_OVERRIDE_LINKED) left the package dirty having changed nothing — and the next
// editor.quit or restart then prompts about, or silently discards, an asset the caller never
// modified. Closing the transaction does not undo it: UTransBuffer::Cancel only pops the record
// off the undo buffer, it never replays it.
//
// MODULE_INPUT_OVERRIDE_LINKED is the refusal driven here because its precondition is built by
// the fixture (SetModuleInputDynamicInput) rather than by the verb under test, so the refusal is
// deterministic. The edit spans two packages — the system asset and the standalone emitter asset
// whose graph carries the module node — and both are asserted.
//
// Counterfactual: revert the FNiagaraCleanPackageBaseline restore in NiagaraEditHandler.cpp and
// both post-refusal assertions fail, because Graph->Modify() dirtied packages that the refused
// call then left dirty.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSetModuleInputRefusalLeavesPackageCleanTest,
    "PinWright.niagara.set_module_input.RefusalLeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetModuleInputRefusalLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    // Add_Float is a stock float DynamicInput-usage script shipped with the Niagara plugin.
    const TCHAR* DynamicInputPath = TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float");
    UNiagaraScript* DynamicInputScript = LoadObject<UNiagaraScript>(nullptr, DynamicInputPath);
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

    // Precondition for the refusal: drive the float SpawnRate input from a dynamic-input chain,
    // wired by the fixture rather than by the verb under test.
    NIRTestFixtures::SetModuleInputDynamicInput(ModuleNode, FName(TEXT("SpawnRate")), DynamicInputScript);

    // The fixture marks its packages RF_Transient so the editor autosaver skips them, and
    // UObjectBaseUtility::MarkPackageDirty walks the outer chain and bails on exactly that flag —
    // so while it is set nothing can dirty these packages and the refusal has nothing to observe.
    // Clear it to reproduce the production shape (a real, saveable asset) and restore it, clean,
    // once the assertions are done. There is no early return between here and the restore.
    UPackage* const SystemPackage = System->GetOutermost();
    UPackage* const GraphPackage = ModuleNode->GetOutermost();
    SystemPackage->ClearFlags(RF_Transient);
    GraphPackage->ClearFlags(RF_Transient);
    SystemPackage->SetDirtyFlag(false);
    GraphPackage->SetDirtyFlag(false);
    TestFalse(TEXT("system package starts clean"), SystemPackage->IsDirty());
    TestFalse(TEXT("emitter graph package starts clean"), GraphPackage->IsDirty());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
    Payload->SetStringField(TEXT("emitter"), TEXT("Fountain"));
    Payload->SetStringField(TEXT("entryId"), ModuleNodeId);
    Payload->SetStringField(TEXT("inputName"), TEXT("SpawnRate"));
    Payload->SetNumberField(TEXT("value"), 42.0);
    Payload->SetStringField(TEXT("scriptUsage"), TEXT("EmitterUpdateScript"));
    // breakExistingLink omitted: the literal is refused rather than replacing the chain.
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    NiagaraEditTestUtils::InvokeExpectError(
        *this,
        TEXT("niagara.set_module_input"),
        Payload,
        TEXT("MODULE_INPUT_OVERRIDE_LINKED"));

    TestFalse(
        TEXT("a refused set_module_input leaves the system package clean"),
        SystemPackage->IsDirty());
    TestFalse(
        TEXT("a refused set_module_input leaves the emitter graph package clean"),
        GraphPackage->IsDirty());

    SystemPackage->SetDirtyFlag(false);
    GraphPackage->SetDirtyFlag(false);
    SystemPackage->SetFlags(RF_Transient);
    GraphPackage->SetFlags(RF_Transient);

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
