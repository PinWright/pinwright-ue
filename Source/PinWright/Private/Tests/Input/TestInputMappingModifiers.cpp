// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Tests/TestUtils.h"

namespace
{
    TSharedPtr<FJsonValue> MakeInputObjectSpec(
        const FString& ClassName,
        const TSharedPtr<FJsonObject>& Properties)
    {
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetStringField(TEXT("class"), ClassName);
        Spec->SetObjectField(TEXT("properties"), Properties);
        return MakeShared<FJsonValueObject>(Spec);
    }

    TSharedPtr<FJsonObject> FindMappingByKey(
        const TSharedPtr<FJsonObject>& Result,
        const FString& Key)
    {
        return JsonArrayFindObjectByStringField(Result, TEXT("mappings"), TEXT("key"), Key);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputAddMappingModifiersAndTriggersRoundTripTest,
    "PinWright.input.add_mapping.PerMappingModifiersAndTriggersRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputAddMappingModifiersAndTriggersRoundTripTest::RunTest(const FString& Parameters)
{
    const FParamSpec* ModifiersParam = GetRegisteredParamSpec(TEXT("input.add_mapping"), TEXT("modifiers"));
    const FParamSpec* TriggersParam = GetRegisteredParamSpec(TEXT("input.add_mapping"), TEXT("triggers"));
    if (TestNotNull(TEXT("modifiers is declared"), ModifiersParam))
    {
        TestEqual(TEXT("modifiers is an array"), ModifiersParam->Type, FString(TEXT("array")));
        TestTrue(TEXT("modifier entries declare class"), ModifiersParam->NestedKeys.Contains(TEXT("class")));
        TestTrue(TEXT("modifier entries declare properties"), ModifiersParam->NestedKeys.Contains(TEXT("properties")));
    }
    if (TestNotNull(TEXT("triggers is declared"), TriggersParam))
    {
        TestEqual(TEXT("triggers is an array"), TriggersParam->Type, FString(TEXT("array")));
        TestTrue(TEXT("trigger entries declare class"), TriggersParam->NestedKeys.Contains(TEXT("class")));
        TestTrue(TEXT("trigger entries declare properties"), TriggersParam->NestedKeys.Contains(TEXT("properties")));
    }

    const FString AssetFolder = TEXT("/Game/PinWrightTests/Input");
    const FString ActionName = TEXT("IA_MappingObjectRoundTrip");
    const FString ContextName = TEXT("IMC_MappingObjectRoundTrip");
    const FString ActionPath = AssetFolder / ActionName;
    const FString ContextPath = AssetFolder / ContextName;

    CleanupTestAsset(ContextPath);
    CleanupTestAsset(ActionPath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ContextPath);
        CleanupTestAsset(ActionPath);
    };

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> CreateAction = MakeShared<FJsonObject>();
    CreateAction->SetStringField(TEXT("name"), ActionName);
    CreateAction->SetStringField(TEXT("path"), AssetFolder);
    TestTrue(TEXT("create action handler found"),
        InvokeHandlerWithCapture(TEXT("input.create_input_action"), CreateAction, Capture));
    if (!TestTrue(TEXT("action created"), Capture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> CreateContext = MakeShared<FJsonObject>();
    CreateContext->SetStringField(TEXT("name"), ContextName);
    CreateContext->SetStringField(TEXT("path"), AssetFolder);
    TestTrue(TEXT("create context handler found"),
        InvokeHandlerWithCapture(TEXT("input.create_input_mapping_context"), CreateContext, Capture));
    if (!TestTrue(TEXT("context created"), Capture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> InvalidMapping = MakeShared<FJsonObject>();
    InvalidMapping->SetStringField(TEXT("contextPath"), ContextPath);
    InvalidMapping->SetStringField(TEXT("actionPath"), ActionPath);
    InvalidMapping->SetStringField(TEXT("key"), TEXT("W"));
    InvalidMapping->SetArrayField(TEXT("modifiers"), {
        MakeInputObjectSpec(TEXT("InputTriggerHold"), MakeShared<FJsonObject>())});
    TestTrue(TEXT("add mapping handler found for invalid modifier"),
        InvokeHandlerWithCapture(TEXT("input.add_mapping"), InvalidMapping, Capture));
    TestFalse(TEXT("a trigger class is refused in the modifiers array"), Capture.bSuccess);
    TestEqual(TEXT("invalid modifier class reports INVALID_CLASS"),
        Capture.ErrorCode, FString(TEXT("INVALID_CLASS")));

    UInputMappingContext* EmptyContext = Cast<UInputMappingContext>(
        UEditorAssetLibrary::LoadAsset(ContextPath));
    if (!TestNotNull(TEXT("mapping context remains loadable after refusal"), EmptyContext))
    {
        return false;
    }
    TestEqual(TEXT("invalid modifier does not add a mapping"),
        EmptyContext->GetMappings().Num(), 0);

    TSharedPtr<FJsonObject> SwizzleProperties = MakeShared<FJsonObject>();
    SwizzleProperties->SetStringField(TEXT("Order"), TEXT("YZX"));
    TSharedPtr<FJsonObject> HoldProperties = MakeShared<FJsonObject>();
    HoldProperties->SetNumberField(TEXT("HoldTimeThreshold"), 0.25);
    HoldProperties->SetBoolField(TEXT("bIsOneShot"), true);

    TSharedPtr<FJsonObject> AddMapping = MakeShared<FJsonObject>();
    AddMapping->SetStringField(TEXT("contextPath"), ContextPath);
    AddMapping->SetStringField(TEXT("actionPath"), ActionPath);
    AddMapping->SetStringField(TEXT("key"), TEXT("W"));
    AddMapping->SetArrayField(TEXT("modifiers"), {
        MakeInputObjectSpec(TEXT("InputModifierSwizzleAxis"), SwizzleProperties)});
    AddMapping->SetArrayField(TEXT("triggers"), {
        MakeInputObjectSpec(TEXT("InputTriggerHold"), HoldProperties)});

    TestTrue(TEXT("add mapping handler found"),
        InvokeHandlerWithCapture(TEXT("input.add_mapping"), AddMapping, Capture));
    if (!TestTrue(TEXT("mapping with modifier and trigger added"), Capture.bSuccess) ||
        !TestTrue(TEXT("add mapping returned a result"), Capture.Result.IsValid()))
    {
        return false;
    }

    TestEqual(TEXT("response key comes from the stored mapping"),
        Capture.Result->GetStringField(TEXT("key")), FString(TEXT("W")));
    TestTrue(TEXT("response reports the stored modifier class"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("modifiers"),
            TEXT("class"), TEXT("InputModifierSwizzleAxis")));
    TestTrue(TEXT("response reports the stored trigger class"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("triggers"),
            TEXT("class"), TEXT("InputTriggerHold")));
    FString MappingStorage;
    const bool bHasMappingStorage = Capture.Result->TryGetStringField(
        TEXT("mappingStorage"), MappingStorage);
#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
    if (TestTrue(TEXT("response names the mapping store"), bHasMappingStorage))
    {
        TestEqual(TEXT("response names the UE 5.7+ mapping store"), MappingStorage,
            FString(TEXT("DefaultKeyMappings.Mappings")));
    }
#else
    if (TestTrue(TEXT("response names the mapping store"), bHasMappingStorage))
    {
        TestEqual(TEXT("response names the legacy mapping store"),
            MappingStorage, FString(TEXT("Mappings")));
    }
#endif

    UInputMappingContext* Context = Cast<UInputMappingContext>(UEditorAssetLibrary::LoadAsset(ContextPath));
    if (!TestNotNull(TEXT("saved mapping context reloads"), Context))
    {
        return false;
    }
    if (!TestEqual(TEXT("one mapping was stored"), Context->GetMappings().Num(), 1))
    {
        return false;
    }

    const FEnhancedActionKeyMapping& Mapping = Context->GetMappings()[0];
    TestEqual(TEXT("stored key"), Mapping.Key.ToString(), FString(TEXT("W")));
    TestEqual(TEXT("one modifier stored"), Mapping.Modifiers.Num(), 1);
    TestEqual(TEXT("one trigger stored"), Mapping.Triggers.Num(), 1);
    if (Mapping.Modifiers.Num() == 1)
    {
        UInputModifierSwizzleAxis* Swizzle = Cast<UInputModifierSwizzleAxis>(Mapping.Modifiers[0]);
        if (TestNotNull(TEXT("stored modifier has requested class"), Swizzle))
        {
            TestTrue(TEXT("stored modifier is outered to the context"), Swizzle->GetOuter() == Context);
            TestTrue(TEXT("stored modifier property was applied"), Swizzle->Order == EInputAxisSwizzle::YZX);
        }
    }
    if (Mapping.Triggers.Num() == 1)
    {
        UInputTriggerHold* Hold = Cast<UInputTriggerHold>(Mapping.Triggers[0]);
        if (TestNotNull(TEXT("stored trigger has requested class"), Hold))
        {
            TestTrue(TEXT("stored trigger is outered to the context"), Hold->GetOuter() == Context);
            TestEqual(TEXT("stored trigger threshold was applied"), Hold->HoldTimeThreshold, 0.25f);
            TestTrue(TEXT("stored trigger boolean was applied"), Hold->bIsOneShot);
        }
    }

    TSharedPtr<FJsonObject> GetInfo = MakeShared<FJsonObject>();
    GetInfo->SetStringField(TEXT("assetPath"), ContextPath);
    TestTrue(TEXT("get input info handler found"),
        InvokeHandlerWithCapture(TEXT("input.get_input_info"), GetInfo, Capture));
    if (!TestTrue(TEXT("mapping readback succeeded"), Capture.bSuccess) ||
        !TestTrue(TEXT("mapping readback returned a result"), Capture.Result.IsValid()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> MappingJson = FindMappingByKey(Capture.Result, TEXT("W"));
    if (!TestTrue(TEXT("readback includes the mapping"), MappingJson.IsValid()))
    {
        return false;
    }
    TSharedPtr<FJsonObject> ModifierJson = JsonArrayFindObjectByStringField(
        MappingJson, TEXT("modifiers"), TEXT("class"), TEXT("InputModifierSwizzleAxis"));
    TSharedPtr<FJsonObject> TriggerJson = JsonArrayFindObjectByStringField(
        MappingJson, TEXT("triggers"), TEXT("class"), TEXT("InputTriggerHold"));
    if (TestTrue(TEXT("readback includes the modifier"), ModifierJson.IsValid()))
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (TestTrue(TEXT("modifier readback includes properties"),
                ModifierJson->TryGetObjectField(TEXT("properties"), Properties) && Properties))
        {
            FString Order;
            if (TestTrue(TEXT("modifier readback includes Order"),
                    (*Properties)->TryGetStringField(TEXT("Order"), Order)))
            {
                TestEqual(TEXT("modifier readback uses the applied enum"),
                    Order, FString(TEXT("YZX")));
            }
        }
    }
    if (TestTrue(TEXT("readback includes the trigger"), TriggerJson.IsValid()))
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (TestTrue(TEXT("trigger readback includes properties"),
                TriggerJson->TryGetObjectField(TEXT("properties"), Properties) && Properties))
        {
            double HoldTimeThreshold = 0.0;
            if (TestTrue(TEXT("trigger readback includes HoldTimeThreshold"),
                    (*Properties)->TryGetNumberField(TEXT("HoldTimeThreshold"), HoldTimeThreshold)))
            {
                TestEqual(TEXT("trigger readback uses the applied threshold"),
                    static_cast<float>(HoldTimeThreshold), 0.25f);
            }
            bool bIsOneShot = false;
            if (TestTrue(TEXT("trigger readback includes bIsOneShot"),
                    (*Properties)->TryGetBoolField(TEXT("bIsOneShot"), bIsOneShot)))
            {
                TestTrue(TEXT("trigger readback uses the applied boolean"), bIsOneShot);
            }
        }
    }

    return true;
}
