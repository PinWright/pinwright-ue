// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-create-payload-defaults: niagara.graph.create_node must refuse a
// SUPPLIED class-specific discriminator it cannot honour instead of keeping the default and
// reporting success. Only an omitted field may keep its default.
//
// Counterfactual: drop the `else` refusals in NiagaraGraphCreate::ApplyCreateNodePayload (usage,
// staticSwitchType), the enumPath requirement, or the target.kind gate in the create_node handler,
// and the matching HasError / error-code assertion below fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraGraphCreateNodePayload.h"

#include "NiagaraCommon.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeStaticSwitch.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraGraphCreateNodeInvalidDiscriminatorsTest,
    "PinWright.niagara.graph.create_node.InvalidPayloadDiscriminatorsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphCreateNodeInvalidDiscriminatorsTest::RunTest(const FString& Parameters)
{
    // --- Input node usage: a supported value is applied and reads back ---
    {
        UNiagaraNodeInput* Node = NewObject<UNiagaraNodeInput>(GetTransientPackage());
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("usage"), TEXT("attribute"));
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestFalse(TEXT("usage 'attribute' is accepted"), Err.HasError());
        TestEqual(TEXT("usage 'attribute' reads back as Attribute"),
            static_cast<int32>(Node->Usage), static_cast<int32>(ENiagaraInputNodeUsage::Attribute));
    }

    // --- Input node usage: a typo is refused, not silently defaulted ---
    {
        UNiagaraNodeInput* Node = NewObject<UNiagaraNodeInput>(GetTransientPackage());
        const ENiagaraInputNodeUsage DefaultUsage = Node->Usage;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("usage"), TEXT("Atribute"));
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestTrue(TEXT("unknown usage is refused"), Err.HasError());
        TestEqual(TEXT("unknown usage code"), Err.Code, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("unknown usage message names the value"), Err.Message.Contains(TEXT("Atribute")));
        TestEqual(TEXT("refused usage left the node's usage unchanged"),
            static_cast<int32>(Node->Usage), static_cast<int32>(DefaultUsage));
    }

    // --- StaticSwitch: unknown staticSwitchType is refused ---
    {
        UNiagaraNodeStaticSwitch* Node = NewObject<UNiagaraNodeStaticSwitch>(GetTransientPackage());
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("staticSwitchType"), TEXT("Boolean"));
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestTrue(TEXT("unknown staticSwitchType is refused"), Err.HasError());
        TestEqual(TEXT("unknown staticSwitchType code"), Err.Code, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("unknown staticSwitchType message names the value"), Err.Message.Contains(TEXT("Boolean")));
    }

    // --- StaticSwitch Enum: enumPath is required ---
    {
        UNiagaraNodeStaticSwitch* Node = NewObject<UNiagaraNodeStaticSwitch>(GetTransientPackage());
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("staticSwitchType"), TEXT("Enum"));
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestTrue(TEXT("Enum switch without enumPath is refused"), Err.HasError());
        TestEqual(TEXT("Enum switch without enumPath code"), Err.Code, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("refused Enum switch carries no enum"), Node->SwitchTypeData.Enum == nullptr);
    }

    // --- StaticSwitch Enum: an unloadable enumPath is refused ---
    {
        UNiagaraNodeStaticSwitch* Node = NewObject<UNiagaraNodeStaticSwitch>(GetTransientPackage());
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("staticSwitchType"), TEXT("Enum"));
        Payload->SetStringField(TEXT("enumPath"), TEXT("/Script/CoreUObject.PinWrightNoSuchEnum_XYZZY"));
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestTrue(TEXT("Enum switch with unloadable enumPath is refused"), Err.HasError());
        TestEqual(TEXT("unloadable enumPath code"), Err.Code, FString(TEXT("ENUM_NOT_FOUND")));
        TestNotEqual(TEXT("refused Enum switch type was not applied"),
            static_cast<int32>(Node->SwitchTypeData.SwitchType), static_cast<int32>(ENiagaraStaticSwitchType::Enum));
    }

    // --- StaticSwitch Enum: a loadable enum is applied and reads back ---
    {
        UEnum* const Enum = StaticEnum<ENiagaraScriptUsage>();
        UNiagaraNodeStaticSwitch* Node = NewObject<UNiagaraNodeStaticSwitch>(GetTransientPackage());
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("staticSwitchType"), TEXT("enum"));
        Payload->SetStringField(TEXT("enumPath"), Enum->GetPathName());
        const FNiagaraEditError Err = NiagaraGraphCreate::ApplyCreateNodePayload(Node, Payload);
        TestFalse(TEXT("Enum switch with a loadable enum is accepted"), Err.HasError());
        TestEqual(TEXT("switch type reads back as Enum"),
            static_cast<int32>(Node->SwitchTypeData.SwitchType), static_cast<int32>(ENiagaraStaticSwitchType::Enum));
        TestTrue(TEXT("switch enum reads back"), Node->SwitchTypeData.Enum == Enum);
    }

    // --- target.kind other than graph is refused before anything is resolved ---
    {
        TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("kind"), TEXT("node"));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), TEXT("/Game/PinWrightTests/NoSuchNiagaraSystem_XYZZY"));
        Params->SetObjectField(TEXT("target"), Target);
        Params->SetStringField(TEXT("nodeClass"), TEXT("NiagaraNodeOp"));
        Params->SetNumberField(TEXT("x"), 0.0);
        Params->SetNumberField(TEXT("y"), 0.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("niagara.graph.create_node handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.graph.create_node"), Params, Capture));
        TestFalse(TEXT("target.kind 'node' is refused"), Capture.bSuccess);
        TestEqual(TEXT("target.kind 'node' code"), Capture.ErrorCode, FString(TEXT("INVALID_TARGET_KIND")));
    }

    return true;
}
